/* host/window_service.cpp: see window_service.h for the ownership and the
 * threading contract. This file is the only place in the port that calls
 * SDL_CreateWindow.
 *
 * The whole body is guarded on ICORECOMP_HAVE_SDL. Without it every entry
 * point is a stub that reports no window, which is what sends each GS backend
 * down the headless path it already had; that is why this translation unit is
 * in the unconditional source list rather than the SDL-only one.
 */
#include "window_service.h"

#include "../runtime.h"
#include "settings.h"

#include <cstdio>

#ifdef ICORECOMP_HAVE_SDL

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace {

SDL_Window* g_window = nullptr;
RtWindowSurface g_surface_kind = RtWindowSurface::None;

/* Written by the main thread (rt_window_sample_state, from the event pump)
 * and read by whichever thread is presenting. Same arrangement the
 * paraLLEl-GS library's SdlWsiPlatform had, moved out here: SDL's window
 * state belongs to the thread that made the window, and the consumer needs
 * the answers without touching SDL. */
std::atomic<uint32_t> g_surface_w{0};
std::atomic<uint32_t> g_surface_h{0};
std::atomic<bool> g_minimized{false};
std::atomic<bool> g_quit{false};
/* What asked for the quit, kept so the end-of-run summary and the exit line
 * can name it rather than saying "the window closed" for four different
 * causes. A pointer to a literal, published after g_quit so a reader that
 * sees the flag set sees a source with it. */
std::atomic<const char*> g_quit_source{nullptr};
std::atomic<bool> g_quit_user{false};

/* One mutex for the two pieces of published state a consumer writes and the
 * main thread reads: the present rectangle and the device description. Both
 * are written rarely (once per present, once per run) and read from the UI,
 * so one lock costs nothing and keeps each of them internally consistent. */
std::mutex g_pub_mutex;
struct PresentRect {
    int32_t x = 0, y = 0, w = 0, h = 0, bb_w = 0, bb_h = 0;
};
PresentRect g_rect;

struct DeviceInfo {
    bool valid = false;
    char backend[kRtWindowBackendBytes] = {};
    char renderer[kRtWindowRendererBytes] = {};
    char features[kRtWindowFeaturesBytes] = {};
};
DeviceInfo g_device;

RtWindowSink g_sink = {};
bool g_have_sink = false;

void copy_into(char* dst, size_t dst_bytes, const char* src) {
    if (!dst || dst_bytes == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    std::snprintf(dst, dst_bytes, "%s", src);
}

/* Reads the window's pixel size and minimized flag out of SDL into the two
 * caches. Main thread only; both calls below are SDL video functions. */
void refresh_cache() {
    if (!g_window) return;
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    g_surface_w.store(uint32_t(w > 0 ? w : 0), std::memory_order_relaxed);
    g_surface_h.store(uint32_t(h > 0 ? h : 0), std::memory_order_relaxed);
    const bool minimized = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MINIMIZED) != 0;
    g_minimized.store(minimized, std::memory_order_relaxed);
    /* Pushed rather than pulled: the field watchdog and the end-of-run
     * summary run on threads that must not call SDL, and "was the window
     * minimised" is one of the few facts that separates a hang from a
     * window nobody can see (host/run_state.cpp). */
    rt_run_note_window_minimized(minimized);
}

} // namespace

bool rt_window_create(const RtSettings& s, RtWindowSurface surface, const char* who) {
    if (g_window) {
        /* One window per run. A second call would leave the first window
         * orphaned with the input layer and the pointer mapping still bound
         * to it, which is a silent wrong picture rather than a crash. */
        rt_log_warn("window", "rt_window_create called twice (for %s); keeping the window that "
                    "already exists", who ? who : "?");
        return true;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        rt_log_warn("window", "SDL_Init(VIDEO) failed: %s; this run has no window and every GS "
                    "backend falls back to headless", SDL_GetError());
        return false;
    }

    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE;
    if (surface == RtWindowSurface::Vulkan) flags |= SDL_WINDOW_VULKAN;

    /* The size settings hold, not a fallback of this file's own: settings.h
     * defaults display.window_width/height to 1280x960, and the load
     * validates the range, so a zero here would mean the caller passed a
     * struct the settings layer never produced. */
    const int want_w = s.display.window_width > 0 ? s.display.window_width : 1280;
    const int want_h = s.display.window_height > 0 ? s.display.window_height : 960;

    g_window = SDL_CreateWindow("ico-recomp", want_w, want_h, flags);
    if (!g_window) {
        rt_log_warn("window", "SDL_CreateWindow failed: %s; this run has no window and every GS "
                    "backend falls back to headless", SDL_GetError());
        return false;
    }
    g_surface_kind = surface;
    /* Primes the cache before anything asks for a surface size. */
    refresh_cache();
    rt_log_info("window", "window created for %s: %dx%d%s", who ? who : "?", want_w, want_h,
                surface == RtWindowSurface::Vulkan ? ", SDL_WINDOW_VULKAN" : "");
    rt_run_phase(RT_PHASE_WINDOW_CREATED);
    return true;
}

void rt_window_destroy() {
    if (!g_window) return;
    SDL_DestroyWindow(g_window);
    g_window = nullptr;
    g_surface_w.store(0, std::memory_order_relaxed);
    g_surface_h.store(0, std::memory_order_relaxed);
    /* Everything the created window set goes back with it, so a second
     * rt_window_create starts from the same state the first one did. There is
     * one window per run today, which is why this had no observable effect;
     * a teardown that leaves half its state behind is still the wrong shape
     * for a function named destroy. */
    g_surface_kind = RtWindowSurface::None;
    g_quit.store(false, std::memory_order_release);
}

bool rt_window_exists() { return g_window != nullptr; }

void* rt_window_handle() { return (void*)g_window; }

void rt_window_surface_size(uint32_t* width, uint32_t* height) {
    if (width) *width = g_surface_w.load(std::memory_order_relaxed);
    if (height) *height = g_surface_h.load(std::memory_order_relaxed);
}

bool rt_window_minimized() { return g_minimized.load(std::memory_order_relaxed); }

bool rt_window_native(RtWindowNative* out) {
    if (!out) return false;
    *out = RtWindowNative{};
    if (!g_window) return false;

    SDL_PropertiesID props = SDL_GetWindowProperties(g_window);
    if (props == 0) {
        rt_log_warn("window", "SDL_GetWindowProperties failed: %s", SDL_GetError());
        return false;
    }
#if defined(SDL_PLATFORM_WIN32)
    out->win32_hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    out->win32_hinstance =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, nullptr);
    return out->win32_hwnd != nullptr;
#elif defined(SDL_PLATFORM_APPLE)
    out->cocoa_window = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                               nullptr);
    return out->cocoa_window != nullptr;
#else
    /* One SDL build can carry both video drivers, so which of the two pairs
     * is filled in is a run-time fact, not a compile-time one: whichever
     * driver made this window is the one whose properties are set. */
    out->x11_display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    out->x11_window = (uint64_t)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    out->wl_display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER,
                                             nullptr);
    out->wl_surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER,
                                             nullptr);
    if (out->wl_display && out->wl_surface) return true;
    return out->x11_display != nullptr && out->x11_window != 0;
#endif
}

uint64_t rt_window_create_vulkan_surface(uint64_t vk_instance) {
    if (!g_window) return 0;
    if (g_surface_kind != RtWindowSurface::Vulkan) {
        /* SDL fixes SDL_WINDOW_VULKAN at creation. Saying so here beats
         * letting SDL_Vulkan_CreateSurface fail with a message about the
         * window rather than about the mismatch that caused it. */
        rt_log_warn("window", "a Vulkan surface was asked for on a window created without "
                    "SDL_WINDOW_VULKAN; the backend that asked is not the one the window was "
                    "created for");
        return 0;
    }
    VkInstance instance = nullptr;
    static_assert(sizeof(instance) == sizeof(void*), "VkInstance is a pointer handle");
    void* raw = (void*)(uintptr_t)vk_instance;
    std::memcpy(&instance, &raw, sizeof(instance));

    /* Value-initialised rather than VK_NULL_HANDLE: SDL_vulkan.h declares
     * the two handle types itself and does not define that macro, and
     * VkSurfaceKHR is a pointer on 64-bit targets and a uint64_t on 32-bit
     * ones, so {} is the one spelling that is a null handle in both. */
    VkSurfaceKHR surface{};
    if (!SDL_Vulkan_CreateSurface(g_window, instance, nullptr, &surface)) {
        rt_log_warn("window", "SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return 0;
    }
    /* memcpy rather than a cast: VkSurfaceKHR is a pointer on 64-bit targets
     * and a uint64_t on 32-bit ones, and only one of the two casts compiles
     * in each case. */
    static_assert(sizeof(surface) == sizeof(uint64_t), "VkSurfaceKHR is 64 bits wide");
    uint64_t out = 0;
    std::memcpy(&out, &surface, sizeof(out));
    return out;
}

const char* const* rt_window_vulkan_instance_extensions(uint32_t* count) {
    if (count) *count = 0;
    if (!g_window) return nullptr;
    Uint32 n = 0;
    const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!exts) {
        rt_log_warn("window", "SDL_Vulkan_GetInstanceExtensions failed: %s", SDL_GetError());
        return nullptr;
    }
    if (count) *count = (uint32_t)n;
    return exts;
}

void rt_window_set_sink(const RtWindowSink* sink) {
    if (sink) {
        g_sink = *sink;
        g_have_sink = true;
        /* A quit that arrived before the backend existed is not lost: it is
         * replayed into the sink the moment one registers. */
        if (g_quit.load(std::memory_order_acquire) && g_sink.quit) g_sink.quit(g_sink.user);
    } else {
        g_sink = RtWindowSink{};
        g_have_sink = false;
    }
}

void rt_window_notify_quit(const char* source, bool user_action) {
    /* First cause wins. A close arrives at this function more than once by
     * design: the pump sees the window event and then SDL's own generated
     * quit, and a restricted pump peeks the same queue again. The first is
     * the one that explains the run. */
    const char* expected = nullptr;
    const bool first = g_quit_source.compare_exchange_strong(
        expected, source ? source : "an unnamed source", std::memory_order_acq_rel);
    if (first) {
        g_quit_user.store(user_action, std::memory_order_release);
        /* warn unless the player did it. A run that ends because the WSI
         * platform decided the window was gone, or because a bare
         * SDL_EVENT_QUIT turned up with no window close behind it, is not a
         * quit: it is the run ending for a reason the user did not choose,
         * and at the shipped default level this line is the only thing that
         * would say so. */
        if (user_action) {
            rt_log_info("window", "quit requested by %s", source ? source : "an unnamed source");
        } else {
            rt_log_warn("window", "quit requested by %s. Nothing the player did asked for this,"
                " so the run is ending for a reason outside the program: read the lines above"
                " this one for what the window system or the graphics device reported.",
                source ? source : "an unnamed source");
        }
        rt_run_set_exit_reason(user_action, "quit requested by %s",
            source ? source : "an unnamed source");
    }
    g_quit.store(true, std::memory_order_release);
    if (g_have_sink && g_sink.quit) g_sink.quit(g_sink.user);
}

const char* rt_window_quit_source() {
    const char* s = g_quit_source.load(std::memory_order_acquire);
    return s ? s : "no quit has been requested";
}

bool rt_window_quit_was_user() { return g_quit_user.load(std::memory_order_acquire); }

void rt_window_notify_resize() {
    /* Refresh first, then forward. The consumer acts on the notification, so
     * forwarding first would let it rebuild a swapchain against the size
     * cache this call is about to replace. */
    refresh_cache();
    if (g_have_sink && g_sink.resize) g_sink.resize(g_sink.user);
}

void rt_window_sample_state() {
    refresh_cache();
    if (g_have_sink && g_sink.sample) g_sink.sample(g_sink.user);
}

bool rt_window_quit_requested() { return g_quit.load(std::memory_order_acquire); }

void rt_window_publish_present_rect(int32_t x, int32_t y, int32_t w, int32_t h,
                                    int32_t bb_w, int32_t bb_h) {
    std::lock_guard<std::mutex> lock(g_pub_mutex);
    g_rect = PresentRect{x, y, w, h, bb_w, bb_h};
}

void rt_window_present_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h,
                            int32_t* bb_w, int32_t* bb_h) {
    PresentRect r;
    {
        std::lock_guard<std::mutex> lock(g_pub_mutex);
        r = g_rect;
    }
    if (x) *x = r.x;
    if (y) *y = r.y;
    if (w) *w = r.w;
    if (h) *h = r.h;
    if (bb_w) *bb_w = r.bb_w;
    if (bb_h) *bb_h = r.bb_h;
}

void rt_window_set_device_info(const char* backend_name, const char* renderer,
                               const char* features) {
    std::lock_guard<std::mutex> lock(g_pub_mutex);
    copy_into(g_device.backend, sizeof(g_device.backend), backend_name);
    copy_into(g_device.renderer, sizeof(g_device.renderer), renderer);
    copy_into(g_device.features, sizeof(g_device.features), features);
    g_device.valid = true;
}

bool rt_window_device_info(char* backend_name, uint32_t backend_bytes,
                           char* renderer, uint32_t renderer_bytes,
                           char* features, uint32_t features_bytes) {
    std::lock_guard<std::mutex> lock(g_pub_mutex);
    copy_into(backend_name, backend_bytes, g_device.backend);
    copy_into(renderer, renderer_bytes, g_device.renderer);
    copy_into(features, features_bytes, g_device.features);
    return g_device.valid;
}

#else /* !ICORECOMP_HAVE_SDL */

/* No SDL in this build: there is no window, and saying so is the whole of it.
 * The device description is still recorded, because a headless backend still
 * creates a device and the same two lines are the honest answer for it. */

namespace {
bool g_quit_stub = false;
const char* g_quit_source_stub = nullptr;
bool g_quit_user_stub = false;
bool g_device_valid = false;
char g_backend_stub[kRtWindowBackendBytes] = {};
char g_renderer_stub[kRtWindowRendererBytes] = {};
char g_features_stub[kRtWindowFeaturesBytes] = {};

void copy_into(char* dst, uint32_t dst_bytes, const char* src) {
    if (!dst || dst_bytes == 0) return;
    std::snprintf(dst, dst_bytes, "%s", src ? src : "");
}
} // namespace

bool rt_window_create(const RtSettings&, RtWindowSurface, const char*) { return false; }
void rt_window_destroy() {}
bool rt_window_exists() { return false; }
void* rt_window_handle() { return nullptr; }
void rt_window_surface_size(uint32_t* width, uint32_t* height) {
    if (width) *width = 0;
    if (height) *height = 0;
}
bool rt_window_minimized() { return false; }
bool rt_window_native(RtWindowNative* out) {
    if (out) *out = RtWindowNative{};
    return false;
}
uint64_t rt_window_create_vulkan_surface(uint64_t) { return 0; }
const char* const* rt_window_vulkan_instance_extensions(uint32_t* count) {
    if (count) *count = 0;
    return nullptr;
}
void rt_window_set_sink(const RtWindowSink*) {}
void rt_window_notify_quit(const char* source, bool user_action) {
    if (!g_quit_stub) {
        g_quit_stub = true;
        g_quit_source_stub = source ? source : "an unnamed source";
        g_quit_user_stub = user_action;
        rt_run_set_exit_reason(user_action, "quit requested by %s", g_quit_source_stub);
    }
}
const char* rt_window_quit_source() {
    return g_quit_source_stub ? g_quit_source_stub : "no quit has been requested";
}
bool rt_window_quit_was_user() { return g_quit_user_stub; }
void rt_window_notify_resize() {}
void rt_window_sample_state() {}
bool rt_window_quit_requested() { return g_quit_stub; }
void rt_window_publish_present_rect(int32_t, int32_t, int32_t, int32_t, int32_t, int32_t) {}
void rt_window_present_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h,
                            int32_t* bb_w, int32_t* bb_h) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    if (bb_w) *bb_w = 0;
    if (bb_h) *bb_h = 0;
}
void rt_window_set_device_info(const char* backend_name, const char* renderer,
                               const char* features) {
    copy_into(g_backend_stub, sizeof(g_backend_stub), backend_name);
    copy_into(g_renderer_stub, sizeof(g_renderer_stub), renderer);
    copy_into(g_features_stub, sizeof(g_features_stub), features);
    g_device_valid = true;
}
bool rt_window_device_info(char* backend_name, uint32_t backend_bytes,
                           char* renderer, uint32_t renderer_bytes,
                           char* features, uint32_t features_bytes) {
    copy_into(backend_name, backend_bytes, g_backend_stub);
    copy_into(renderer, renderer_bytes, g_renderer_stub);
    copy_into(features, features_bytes, g_features_stub);
    return g_device_valid;
}

#endif /* ICORECOMP_HAVE_SDL */
