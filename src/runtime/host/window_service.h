/* host/window_service.h: the executable owns the window.
 *
 * Ours (MIT). One SDL3 window per run, created here, on the main thread, at
 * rt_hw_init() time, before the GS backend that will present into it exists.
 * Every GS backend is a consumer of this window; none of them creates one.
 * That is the change this file exists for: the window used to be created
 * inside libicorecomp-parallel-gs, which meant the LGPL shared library owned
 * the one object the whole port's input, UI and pointer mapping depend on,
 * and the clean-room renderer (gs/render) could not be handed a surface at
 * all.
 *
 * ---- ownership and threads ------------------------------------------------
 *
 * Stated the way gs/gs_parallel_api.h states its own contract, because the
 * two now describe one arrangement:
 *
 *   the main thread     the host's EE and main thread. It called
 *                       rt_window_create, owns the SDL_Window for the rest of
 *                       the run and is the only thread that may call SDL. So
 *                       it is the only thread that may call
 *                       rt_window_create, rt_window_destroy,
 *                       rt_window_apply_mode (host/window.h),
 *                       rt_window_set_icon, rt_window_native,
 *                       rt_window_create_vulkan_surface, rt_window_notify_*
 *                       and rt_window_sample_state. rt_window_pump
 *                       (host/window.h) runs here too.
 *   the GS consumer     gs/gs_threaded.cpp's worker, or the EE thread when
 *                       the ring is bypassed. It submits and presents. The
 *                       only entry point here it calls is
 *                       rt_window_publish_present_rect, right after the
 *                       present that measured the rectangle.
 *
 * Everything else is readable from either thread at any time:
 * rt_window_exists, rt_window_handle, rt_window_surface_size,
 * rt_window_minimized, rt_window_quit_requested and
 * rt_window_present_rect. The state behind those is atomic, or published
 * under this file's own mutex, for that reason. The window's pixel size and
 * minimized flag are sampled from SDL on the main thread
 * (rt_window_sample_state, once per event pump) and read out of atomics
 * everywhere else, because SDL's event queue and window state belong to the
 * thread that made the window.
 *
 * A backend that has to hear about a quit or a resize registers a sink
 * (rt_window_set_sink); the notifications are forwarded on the main thread,
 * from inside the event pump, so a sink callback may only set flags. It must
 * not submit GS work and must not block.
 *
 * Without SDL (ICORECOMP_HAVE_SDL unset: no third_party/SDL and no Granite
 * copy, or -DICORECOMP_WINDOW=OFF) every function here is a stub that reports
 * no window, which sends every GS backend down its headless path.
 */
#ifndef ICORECOMP_HOST_WINDOW_SERVICE_H
#define ICORECOMP_HOST_WINDOW_SERVICE_H

#include <cstdint>

struct RtSettings;

/* What the window has to be created with for the backend that will present
 * into it. SDL fixes some of these at creation and they cannot be added
 * afterwards, which is why the choice of renderer has to be resolved before
 * the window is made (gs/gs_select.cpp does that). */
enum class RtWindowSurface {
    /* No extra flag. D3D12 and Metal build their swapchain from the native
     * window handle rt_window_native reports. */
    None,
    /* SDL_WINDOW_VULKAN. Both Vulkan consumers want it: the paraLLEl-GS
     * library, whose WSI asks this file for a surface, and the Vulkan RHI
     * backend, which does the same. */
    Vulkan,
};

/* The platform handles the RHI's DeviceDesc takes (rhi/rhi.h). Exactly one
 * platform's pair is filled in and the rest stay null, which is the same
 * contract DeviceDesc documents; this file reads them out of
 * SDL_GetWindowProperties so that nothing in rhi/ links SDL. */
struct RtWindowNative {
    void* win32_hwnd = nullptr;
    void* win32_hinstance = nullptr;
    void* x11_display = nullptr;
    uint64_t x11_window = 0;
    void* wl_display = nullptr;
    void* wl_surface = nullptr;
    void* cocoa_window = nullptr;
};

/* Creates the one window, at display.window_width/height, resizable, with
 * the flags `surface` asks for. `who` names the backend that asked, for the
 * log line. Main thread only, once per run.
 *
 * False, with a log naming the SDL error, when there is no window: no video
 * driver (a headless Linux session, SDL built without X11 or Wayland), or
 * SDL_CreateWindow failed. That is not fatal here, because every backend has
 * a headless path and the caller decides; rt_window_exists() answers false
 * afterwards.
 *
 * display.mode is NOT applied here. The window opens windowed at the
 * configured size, and rt_window_apply_mode (host/window.h) takes it to
 * fullscreen afterwards, which is the order that was already in use: an
 * exclusive-fullscreen failure has to be able to fall back to a window that
 * already exists. */
bool rt_window_create(const RtSettings& s, RtWindowSurface surface, const char* who);

/* Destroys the window. Main thread only, after every backend that presents
 * into it is gone. Idempotent. */
void rt_window_destroy();

/* True when there is a live window. Any thread. */
bool rt_window_exists();

/* SDL_Window* as void*, null when there is none. Any thread for the pointer
 * itself; SDL calls made with it belong to the main thread. Opaque on
 * purpose so that a caller which does not want SDL types does not need
 * them. */
void* rt_window_handle();

/* The window's size in pixels, 0x0 when there is no window. Served from the
 * cache rt_window_sample_state fills, so it is safe from any thread and from
 * inside a present. */
void rt_window_surface_size(uint32_t* width, uint32_t* height);

/* True while the window is minimized, from the same cache. A minimized
 * window has a zero-extent surface, which is what makes a present
 * impossible; a consumer skips the frame rather than blocking on it. */
bool rt_window_minimized();

/* Fills `out` with the native handles for this platform. False when there is
 * no window, or when SDL reports no handle for the platform this build was
 * compiled for. Main thread only (it queries SDL). */
bool rt_window_native(RtWindowNative* out);

/* Creates a VkSurfaceKHR on `vk_instance` for this window, returning it as a
 * uint64_t so that no Vulkan type crosses this header (the paraLLEl-GS C ABI
 * carries it the same way, and the RHI's Vulkan backend has its own headers).
 * 0 on failure, logged. Main thread only, and only for a window created with
 * RtWindowSurface::Vulkan; asking on a window created without the flag logs
 * and returns 0 rather than letting SDL fail obscurely later. */
uint64_t rt_window_create_vulkan_surface(uint64_t vk_instance);

/* The Vulkan instance extensions this window's surface needs, as SDL reports
 * them. Returns the array (owned by SDL, valid for the run) and writes the
 * count; null with a zero count when there is no window. Main thread only. */
const char* const* rt_window_vulkan_instance_extensions(uint32_t* count);

/* ---- notifications -------------------------------------------------------
 *
 * The window service records the fact and forwards it to the backend's sink,
 * in that order, so a backend that has not registered yet still cannot lose a
 * quit: rt_window_quit_requested() keeps it. */

struct RtWindowSink {
    void* user;
    /* The window closed or a quit was requested. Sticky on this side. */
    void (*quit)(void*);
    /* The window's size changed, or it came back from minimized. The size
     * cache has already been refreshed when this is called, so a sink may
     * read rt_window_surface_size from inside it. */
    void (*resize)(void*);
    /* The size and minimized cache was refreshed. For a backend that keeps
     * its own copy of the two (the paraLLEl-GS library does, behind its C
     * ABI, because its WSI reads them from the consumer thread). */
    void (*sample)(void*);
};

/* Registers (or, with null, clears) the live backend's sink. Main thread
 * only, and only between frames: a backend registers in its constructor and
 * clears in its destructor. Every callback runs on the main thread from
 * inside the event pump, which means a swapchain image may already be
 * acquired on the consumer thread: a sink may only set flags. */
void rt_window_set_sink(const RtWindowSink* sink);

/* Records a quit and forwards it. Sticky: once requested it stays requested
 * for the rest of the run. Main thread.
 *
 * `source` names what asked, in words a log reader can act on ("Quit from
 * the menu", "the window's close button", "SDL_EVENT_QUIT with no window
 * close behind it"). It must have static lifetime, or be a literal, because
 * the pointer is what is kept. `user_action` is true only when the player
 * did it on purpose: Quit from the menu or the launcher, the window's own
 * close button, or the restart that applies a cold key. Everything else,
 * including a bare SDL_EVENT_QUIT that no window close explains and a WSI
 * platform that reported the window gone, is false, and the run's exit is
 * then reported at warn rather than info.
 *
 * The first call wins for both: a close has one cause, and the one nearest
 * it is the one worth keeping. */
void rt_window_notify_quit(const char* source, bool user_action);

/* What asked for the quit, and whether the player asked for it. "no quit
 * has been requested" before one has. Any thread. */
const char* rt_window_quit_source();
bool rt_window_quit_was_user();
/* Refreshes the size cache, then forwards the resize. Main thread. */
void rt_window_notify_resize();
/* Refreshes the size and minimized cache from SDL and forwards `sample`.
 * Main thread, once per event pump. */
void rt_window_sample_state();
/* True once a quit has been requested. Any thread. */
bool rt_window_quit_requested();

/* ---- the present rectangle ------------------------------------------------
 *
 * Where the last presented picture landed in the window backbuffer, in
 * backbuffer pixels, plus the backbuffer size it was measured against. This
 * is the one number that maps a window position onto a guest pixel, so the
 * mouse pointer (host/mouse.cpp), the pointer on the game's own menus
 * (guest/menu_nav.cpp), the UI (ui/ui.cpp) and the screenshot cropper
 * (host/screenshot.cpp) all read it.
 *
 * It is published by whichever backend presented, from the consumer thread,
 * right after the present that measured it, and read from the EE thread while
 * that present may still be in flight. The six values are written and read
 * under one mutex so a reader always sees one present's rectangle and never a
 * mix of two.
 *
 * All zero before the first present. A zero width or height with the
 * backbuffer size still filled in is the honest report for "this field
 * presented no picture", and a caller mapping a position must treat a
 * non-positive width or height as no picture. */
void rt_window_publish_present_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                                    int32_t bb_w, int32_t bb_h);
void rt_window_present_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h,
                            int32_t* bb_w, int32_t* bb_h);

/* ---- the device the active backend created --------------------------------
 *
 * The Display tab's two read-only lines. They used to come from a Vulkan
 * probe that created and destroyed a device of its own at every startup,
 * which reported a device that may not be the one the run went on to use.
 * The backend fills these in once, just after it has created its device, and
 * the menu reads them; nothing else creates a device to answer the question.
 *
 * `renderer` is "<device name>, <driver or API version>" and `features` is
 * whatever the backend has to say about the requirements it checked, which
 * for paraLLEl-GS is its feature list and for the native renderer is the
 * backend name and the API level it settled on. Both are empty until a
 * backend has created a device; the menu says so in that case. Set on the
 * thread that created the device, read from the UI on the main thread, so the
 * strings are published under the same mutex the present rectangle uses. */
void rt_window_set_device_info(const char* backend_name, const char* renderer,
                               const char* features);
/* Copies the three into caller-owned buffers, each NUL-terminated. Any of the
 * three may be null. False when no backend has published yet. */
bool rt_window_device_info(char* backend_name, uint32_t backend_bytes,
                           char* renderer, uint32_t renderer_bytes,
                           char* features, uint32_t features_bytes);
/* The widths the service stores each of the three at, so a caller sizes its
 * buffers from one place. A longer string is truncated on the way in. */
enum : uint32_t {
    kRtWindowBackendBytes = 64,
    kRtWindowRendererBytes = 512,
    kRtWindowFeaturesBytes = 512,
};

#endif /* ICORECOMP_HOST_WINDOW_SERVICE_H */
