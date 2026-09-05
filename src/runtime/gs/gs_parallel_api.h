/* gs/gs_parallel_api.h: the C ABI of libicorecomp-parallel-gs.
 *
 * This header is the whole boundary between the MIT runtime and the LGPLv3+
 * paraLLEl-GS shared library. Everything Granite/paraLLEl-GS (all C++) stays
 * inside the library, behind the gs/gs_parallel_*.cpp units; the executable side
 * (gs_parallel.cpp, gs_replay_main.cpp) sees only these opaque-handle C
 * functions. The narrow surface is deliberate:
 *   - license: no LGPL class layouts, inline code or vtables compile into
 *     the MIT executable;
 *   - linking: on MSVC a DLL with no exports produces no import library
 *     (LNK1181); explicit RT_GS_API exports make every toolchain emit one.
 *
 * Ours (MIT), like both files on either side of it.
 */
#ifndef ICORECOMP_GS_PARALLEL_API_H
#define ICORECOMP_GS_PARALLEL_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(ICORECOMP_PGS_BUILD_DLL)
#    define RT_GS_API __declspec(dllexport)
#  else
#    define RT_GS_API __declspec(dllimport)
#  endif
#else
#  define RT_GS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

RT_GS_API const char* icorecomp_parallel_gs_shim_version(void);

/* Host services the library may call back into. Messages arrive fully
 * formatted; `fatal` must not return (the host terminates the process). */
typedef struct RtPgsHost {
    void (*log)(const char* component, const char* message);
    void (*fatal)(const char* component, const char* message);
    /* Pumps the host's SDL event loop. NULL: the library pumps SDL itself
     * (the pre-shim-3 behavior; the replay tool and any host without a UI
     * use that). Called from inside Granite's WSI::begin_frame, so a
     * swapchain image may be acquired: the callback may only queue events
     * and call rt_pgs_notify_quit / rt_pgs_notify_resize /
     * rt_pgs_sample_window_state. Any other rt_pgs_* call from inside it is
     * fatal (see the in-frame guard).
     *
     * Only ever called on the creating thread. Once the host has registered
     * a consumer thread (rt_pgs_bind_consumer_thread), the library skips
     * this callback on that thread rather than calling SDL from it, and the
     * host's own per-field pump is what delivers events; see the threads
     * section below. */
    void (*pump_events)(void);
    /* Creates a VkSurfaceKHR for the host's window on `vk_instance`, both
     * carried as uint64_t so that no Vulkan type crosses this ABI; returns 0
     * on failure, having logged. The host owns the window
     * (src/runtime/host/window_service.cpp), so it owns the
     * SDL_Vulkan_CreateSurface call on it too.
     *
     * Required whenever RtPgsCreateOptions::host_window is set; NULL with a
     * host_window is a setup failure through host->fatal, because a window
     * with no way to make a surface from it cannot be presented into.
     * Ignored when host_window is NULL, which is the headless case.
     *
     * Called once, on the creating thread, from inside device creation. */
    uint64_t (*create_vulkan_surface)(uint64_t vk_instance);
} RtPgsHost;

/* Opaque live-backend instance (Vulkan device + optional SDL3 window +
 * ParallelGS::GSInterface, all library-side). */
typedef struct RtPgs RtPgs;

#define RT_PGS_PRESENT_MAILBOX   0u
#define RT_PGS_PRESENT_FIFO      1u
#define RT_PGS_PRESENT_IMMEDIATE 2u
#define RT_PGS_FIT_LETTERBOX 0u
#define RT_PGS_FIT_INTEGER   1u
#define RT_PGS_FIT_STRETCH   2u
#define RT_PGS_FILTER_LINEAR  0u
#define RT_PGS_FILTER_NEAREST 1u
/* Output frame: crt is the renderer's own visible area for the video mode and
 * crops any window that overruns it; window grows the frame until it holds
 * every enabled CRTC window. See the placement rule in gs_parallel_scanout.cpp. */
#define RT_PGS_RASTER_CRT    0u
#define RT_PGS_RASTER_WINDOW 1u
/* How an interlaced scanout becomes one output frame: adaptive weaves the
 * still parts and bobs the moving parts (the renderer's FastMAD filter), bob
 * presents each field on its own at its own raster position, weave always
 * pairs the two newest fields. See RtPgs::vsync in gs_parallel_scanout.cpp. */
#define RT_PGS_DEINTERLACE_ADAPTIVE 0u
#define RT_PGS_DEINTERLACE_BOB      1u
#define RT_PGS_DEINTERLACE_WEAVE    2u

/* Startup options resolved by the host (settings.json with environment
 * variables taking precedence). Extend at the end only. */
typedef struct RtPgsCreateOptions {
    uint32_t present_mode;   /* RT_PGS_PRESENT_* */
    uint32_t fit;            /* RT_PGS_FIT_* */
    uint32_t filter;         /* RT_PGS_FILTER_* */
    /* 1/4/8/16, paraLLEl-GS SuperSampling factor. 4 and up also turn on
     * high-resolution scanout, which has no separate option: 2x is not in
     * the set because it only doubles the vertical sampling rate, which the
     * renderer refuses to scan out at higher resolution. */
    uint32_t render_scale;
    /* Initial window size in logical pixels. 0 (either field): the shim's
     * own 640x480 fallback (see init_windowed's comment on why 4:3). That
     * fallback is not the host's default: display.window_width/height
     * default to 1280x960 (settings.h), and the host always passes them. */
    uint32_t window_width, window_height;
    uint32_t raster;         /* RT_PGS_RASTER_* */
    uint32_t deinterlace;    /* RT_PGS_DEINTERLACE_* */
    /* The host's SDL_Window*, as an opaque pointer. This library no longer
     * creates a window of its own: the executable owns the one window of the
     * run (src/runtime/host/window_service.h), creates it with
     * SDL_WINDOW_VULKAN before the backend exists, and hands it in here. The
     * instance adopts it: it never calls SDL_Init, SDL_CreateWindow or
     * SDL_DestroyWindow, and it asks the host for the surface through
     * RtPgsHost::create_vulkan_surface.
     *
     * NULL means headless, which is what the replay path and a run with no
     * display take. window_width/height above are then unused, since nothing
     * is being sized.
     *
     * The pointer must outlive the instance. The host destroys its window
     * only after rt_pgs_destroy has returned. */
    void* host_window;
} RtPgsCreateOptions;

/* Creates the live backend. Never returns NULL: unrecoverable setup errors
 * (no Vulkan loader/device, GSInterface init failure) go through
 * host->fatal. `host` is copied; the pointed-to struct need not outlive the
 * call. `opts` may be NULL, meaning the pre-settings defaults (present mode
 * from ICORECOMP_GS_PRESENT as before this struct existed, letterbox,
 * linear filtering, render scale 1, and the same raster window and
 * deinterlace bob defaults settings.h carries); otherwise it is
 * copied and the caller resolved it (settings.json with environment
 * variables taking precedence -- see gs_parallel.cpp). */
RT_GS_API RtPgs* rt_pgs_create(const RtPgsHost* host, const RtPgsCreateOptions* opts);
RT_GS_API void rt_pgs_destroy(RtPgs* pgs);

/* Mirrors GsBackend (gs_backend.h): path 0..2, data is qwords*16 bytes. */
RT_GS_API void rt_pgs_submit_gif(RtPgs* pgs, int path, const uint8_t* data, uint32_t qwords);
RT_GS_API void rt_pgs_write_priv(RtPgs* pgs, uint32_t offset, uint64_t v);
RT_GS_API uint64_t rt_pgs_read_priv(RtPgs* pgs, uint32_t offset);

/* End of field: render and scan out. It does not present. The finished
 * field is latched into the instance's latest-scanout slot with a new
 * scanout serial, and rt_pgs_present_pump below is what puts it on the
 * screen; that is what lets the host present at a rate of its own choosing
 * without the guest field rate moving. Returns a bitmask of RT_PGS_VSYNC_*
 * flags; WINDOW_CLOSED asks the host to exit cleanly (the library never
 * terminates the process itself).
 *
 * RT_PGS_VSYNC_PRESENTED is never set by rt_pgs_vsync: this call presents
 * nothing, and whether the present pump is keeping up is what
 * rt_pgs_present_pump's own return value reports. The bit belongs to
 * rt_pgs_present_ui, which latches and presents in one step and is the only
 * caller that reads it (ui/ui_launcher.cpp).
 *
 * RT_PGS_VSYNC_LATCHED: this field carried GIF traffic, so the picture it
 * latched is one the guest drew rather than a repeat of the register state.
 * This is what rt_pgs_vsync's PRESENTED bit used to carry (the flag was
 * named for a frame's worth of traffic reaching the screen, never for a
 * swapchain present), and it is what GsBackend::vsync's bool still reports
 * on every backend. */
#define RT_PGS_VSYNC_PRESENTED     1u
#define RT_PGS_VSYNC_WINDOW_CLOSED 2u
#define RT_PGS_VSYNC_LATCHED       4u
RT_GS_API uint32_t rt_pgs_vsync(RtPgs* pgs, unsigned field);

/* Presents the latest latched scanout, at most once per call. Consumer
 * thread only (see the threads section below): it runs the same
 * present_frame path rt_pgs_vsync used to run inline, so it touches the
 * swapchain and the retained overlay frame.
 *
 * It presents when either is true:
 *   - the scanout serial has advanced since the last present, so there is a
 *     new field to show;
 *   - `max_hz` is above zero and 1/max_hz has elapsed since the last
 *     present, which repeats the picture already on screen.
 *
 * max_hz 0 means one present per new serial and no repeats, which is the
 * behaviour of the build before this entry point existed: exactly one
 * present per field.
 *
 * A repeat is a recomposite, not a new frame. The scanout image is the same
 * one, blitted again, with the retained overlay frame (rt_pgs_overlay_set_
 * frame) drawn over it again, so the menu and the pointer are redrawn at the
 * present rate while their geometry is still produced once per field. It
 * cannot make the guest run faster or slower and it does not ask the
 * renderer for anything.
 *
 * Returns a bitmask of RT_PGS_PUMP_*; 0 means this call presented nothing.
 * RT_PGS_PUMP_PRESENTED is set only when a frame reached the swapchain: a
 * window that is not presentable (minimized, zero-sized) or a swapchain
 * acquire that failed returns 0, and the serial, the pacing clock and the
 * present counters do not advance. A caller counting or logging presents can
 * therefore trust the flag, which is what makes the `present` verbose channel
 * a measurement of display.present_rate rather than of how often this was
 * called.
 * `serial`, when not NULL, is filled with the scanout serial that was
 * presented, or with the latest one when nothing was presented, which is
 * what a host-side diagnostic needs to tell a new field from a repeat. */
#define RT_PGS_PUMP_PRESENTED 1u
#define RT_PGS_PUMP_REPEAT    2u
RT_GS_API uint32_t rt_pgs_present_pump(RtPgs* pgs, double max_hz, uint64_t* serial);

/* ---- threads ------------------------------------------------------------
 *
 * One instance, two host threads at most, never at the same time:
 *
 *   the creating thread   the host's EE and main thread. It called
 *                         rt_pgs_create, owns the SDL window (it created it
 *                         before this instance existed; see
 *                         host/window_service.h) and is the only thread that
 *                         may call SDL, so it is the only thread that may
 *                         call rt_pgs_notify_* and
 *                         rt_pgs_sample_window_state.
 *   the consumer thread   the host's GS command ring worker
 *                         (src/runtime/gs/gs_threaded.cpp). After
 *                         rt_pgs_bind_consumer_thread it is the only thread
 *                         that calls rt_pgs_submit_gif, rt_pgs_write_priv,
 *                         rt_pgs_vsync, rt_pgs_present_pump,
 *                         rt_pgs_set_*, rt_pgs_overlay_*,
 *                         rt_pgs_request_screenshot and
 *                         rt_pgs_present_ui.
 *
 * The rest is readable from either thread at any time: rt_pgs_read_priv,
 * rt_pgs_present_timings, rt_pgs_present_rect, rt_pgs_take_screenshot,
 * and rt_pgs_window_closed. The state behind those is
 * atomic, or published under the library's own mutex, for that reason.
 *
 * rt_pgs_bind_consumer_thread registers the calling thread as the consumer:
 * today that means giving it Granite's thread index 0, which its per-thread
 * command pools are keyed on. The creating thread already holds index 0 from
 * Device::set_context and the two never run library calls at the same time,
 * so they share it. Call once, on the consumer thread, before its first call
 * from the list above. */
RT_GS_API void rt_pgs_bind_consumer_thread(RtPgs* pgs);

/* Non-zero once the window has closed or a quit was requested, sticky, and
 * readable from any thread. rt_pgs_vsync's RT_PGS_VSYNC_WINDOW_CLOSED bit
 * says the same thing, but only to whoever called it; with the consumer on
 * its own thread the host needs the answer even while that thread is inside
 * the library and cannot return one (parked waiting for a minimized window
 * to come back, for instance). */
RT_GS_API uint32_t rt_pgs_window_closed(RtPgs* pgs);

/* Samples the window's pixel size and minimized state into the library's
 * cache. Creating thread only, because it calls SDL; the host calls it from
 * its event pump, so the cache is refreshed once per field and after every
 * resize. That cache is what the consumer thread reads when Granite asks the
 * platform for a surface size or whether the window can be presented to,
 * which is why the consumer never has to touch SDL. Safe from inside
 * pump_events. */
RT_GS_API void rt_pgs_sample_window_state(RtPgs* pgs);

RT_GS_API void rt_pgs_report_stats(RtPgs* pgs);

/* Present-path timings for the host's profiler, since the previous call.
 * Reading clears every counter. The host reports per profile window and
 * cannot see inside the library, so the split has to cross the ABI.
 *
 *   flush_ns    the renderer flush at the top of rt_pgs_vsync
 *   scanout_ns  building the scanout image
 *   present_ns  every rt_pgs_present_pump that presented, repeats included.
 *               Divide by `presents`, not by `fields`: with a present rate
 *               set there is more than one present per field.
 *   fields      the rt_pgs_vsync calls the three above cover
 *   presents    presents that happened, new serials and repeats together
 *   repeats     of those, the ones that showed a serial already on screen
 *
 * Extend at the end only. `out` may be NULL, which clears nothing. */
typedef struct RtPgsPresentTimings {
    uint64_t flush_ns;
    uint64_t scanout_ns;
    uint64_t present_ns;
    uint64_t fields;
    uint64_t presents;
    uint64_t repeats;
} RtPgsPresentTimings;
RT_GS_API void rt_pgs_present_timings(RtPgs* pgs, RtPgsPresentTimings* out);

/* Window control and event-pump inversion (shim 3). The exe owns the only
 * SDL_PollEvent loop (host/window.cpp); these let it drive the window the
 * library created without the library polling SDL itself. */

/* The window handle and the surface size are not answered here. The host
 * owns the window (host/window_service.h): rt_window_handle() and
 * rt_window_surface_size() are the readers, and rt_window_exists() answers
 * "did the windowed path come up at all". Entry points for them existed
 * while the library owned the window and are gone with that ownership. */

/* Reported the same way the inline SDL_EVENT_QUIT handling used to: the
 * next vsync's RT_PGS_VSYNC_WINDOW_CLOSED reflects it. */
RT_GS_API void  rt_pgs_notify_quit(RtPgs* pgs);
/* Reported the same way the inline SDL_EVENT_WINDOW_RESIZED /
 * SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED handling used to: the swapchain is
 * rebuilt at the next presentable frame. */
RT_GS_API void  rt_pgs_notify_resize(RtPgs* pgs);
/* The window-backbuffer rectangle the last presented scanout was blitted
 * into, in backbuffer pixels, plus the backbuffer size it was measured
 * against. All zero before the first present or when headless; the size is
 * zero with the backbuffer size still filled in on a field that presented no
 * scanout image, which is the honest report for "nothing maps window pixels
 * to guest pixels this field". A caller mapping a position into the picture
 * must treat a non-positive width or height as no picture.
 *
 * Published under the library's own mutex by the present that measured it
 * and read back under the same mutex here, so the six values always describe
 * one present and never a mix of two. Safe from pump_events and from either
 * thread at any time (see the threads section above).
 *
 * One consumer: gs_parallel.cpp's adapter, which calls this immediately
 * after each present and republishes the six values through
 * host/window_service.h. Host code that maps a cursor position into the
 * picture reads rt_window_present_rect(), not this, because the window
 * service answers for the native renderer too. */
RT_GS_API void rt_pgs_present_rect(RtPgs* pgs, int32_t* x, int32_t* y, int32_t* w, int32_t* h, int32_t* bb_w, int32_t* bb_h);

/* ---- screenshot of the presented picture ---------------------------------
 *
 * The user-facing capture (host/screenshot.cpp). Separate from the
 * ICORECOMP_GS_SCREENSHOT diagnostic in gs_parallel_scanout.cpp: that one
 * writes the raw scanout image, which is a function of the GS output alone
 * and is not the shape the picture has on screen. This one copies the
 * window backbuffer over exactly the rectangle rt_pgs_present_rect reports,
 * so it is the presented picture at presented size, aspect corrected and
 * with the letterbox bars excluded, and it is taken before the overlay pass
 * so the menu, the pointer and the fps readout are never in it.
 *
 * Two slots per request:
 *
 *   RT_PGS_SHOT_PRE    the picture as the game drew it, copied after the
 *                      scanout blit and before the overlay render pass.
 *                      This is the one the feature writes.
 *   RT_PGS_SHOT_POST   the same field copied again after the overlay render
 *                      pass. Only filled when the host asks for two slots.
 *                      It exists to settle one question by measurement: with
 *                      the menu closed the two files must be byte identical,
 *                      and with the menu open they must differ, which is what
 *                      proves the pre copy is taken where this comment says
 *                      it is. The host gates it on its own verbose channel.
 *
 * rt_pgs_request_screenshot arms one field. `slots` is 1 (pre only) or 2
 * (pre and post); any other value is treated as 1 and logged. Consumer
 * thread only, like every other call that changes what a present does: the
 * host routes it through GsBackend and the GS command ring so it is ordered
 * against the GIF and vsync traffic, and lands on the field the user asked
 * for rather than one either side of it. A field that presents no scanout
 * image (an empty present rectangle) does not consume the arm; it waits for
 * the next field that has a picture.
 *
 * rt_pgs_take_screenshot reads one slot back. Either thread, at any time:
 * the pixels and their size are published under the library's own mutex by
 * the present that copied them, the same way rt_pgs_present_rect is, so it
 * does not ride the command ring. Returns 0 when that slot holds nothing
 * ready, otherwise the number of bytes the image occupies (width * height *
 * 4). `dst` NULL is the size query: *w and *h are filled in and nothing is
 * copied, so the caller can size its buffer. With `dst` non-NULL the slot is
 * consumed: the rows are copied out tightly packed as RGBA8 from the top and
 * the slot goes back to empty, and a `dst_bytes` smaller than the image
 * copies nothing, keeps the slot and returns 0. Any of `w` and `h` may be
 * NULL. */
#define RT_PGS_SHOT_PRE   0u
#define RT_PGS_SHOT_POST  1u
#define RT_PGS_SHOT_SLOTS 2u
RT_GS_API void rt_pgs_request_screenshot(RtPgs* pgs, uint32_t slots);
RT_GS_API size_t rt_pgs_take_screenshot(RtPgs* pgs, uint32_t slot, uint32_t* w, uint32_t* h,
                                        uint8_t* dst, size_t dst_bytes);
/* Between frames only; fatal when called while a frame is in flight
 * (including from pump_events). Same RT_PGS_PRESENT_* values as create. */
RT_GS_API void  rt_pgs_set_present_mode(RtPgs* pgs, uint32_t mode);
/* Presentation of the final scanout only (fit + filter); takes effect at
 * the next present. Safe between frames; fatal from pump_events. */
RT_GS_API void  rt_pgs_set_presentation(RtPgs* pgs, uint32_t fit, uint32_t filter);
/* Output frame the scanout is built at (RT_PGS_RASTER_*); takes effect at
 * the next vsync. Between frames only; fatal from pump_events. */
RT_GS_API void  rt_pgs_set_raster(RtPgs* pgs, uint32_t raster);
/* Deinterlace mode for an interlaced scanout (RT_PGS_DEINTERLACE_*); takes
 * effect at the next vsync. Between frames only; fatal from pump_events. */
RT_GS_API void  rt_pgs_set_deinterlace(RtPgs* pgs, uint32_t deinterlace);
/* display.widescreen's presentation half: the aspect the finished scanout is
 * presented at, overriding the one derived from the CRTC registers. 0 (the
 * value a fresh instance holds) keeps the derivation, which is the retail
 * 4:3. Takes effect at the next vsync. Between frames only; fatal from
 * pump_events. */
RT_GS_API void  rt_pgs_set_widescreen_aspect(RtPgs* pgs, double aspect);
/* Retunes super-sampling in flight (dynamic_super_sampling was set at
 * init). Between frames only; fatal mid-frame. Invalid factor is fatal
 * (host validates). High-resolution scanout follows the factor: requested
 * at 4 and up, off below. */
RT_GS_API void  rt_pgs_set_render_scale(RtPgs* pgs, uint32_t factor);

/* Overlay rendering: a small textured/scissored 2D pass drawn on top of the
 * swapchain backbuffer, used by the settings menu and (later) RmlUi. Plain
 * POD in, no host callback needed; the library owns the Vulkan pipeline,
 * pooled vertex/index buffers and texture images. */

typedef struct RtPgsOverlayVertex {
    float x, y;       /* pixels, origin top-left of the surface */
    float u, v;
    uint32_t rgba;    /* R8G8B8A8_UNORM byte order, straight (non-premultiplied)
                       * alpha unless the command sets
                       * RT_PGS_OVERLAY_PREMULTIPLIED */
} RtPgsOverlayVertex;

#define RT_PGS_OVERLAY_SCISSOR   1u   /* scissor_* fields are valid */
#define RT_PGS_OVERLAY_TRANSFORM 2u   /* transform[] is not identity */
/* This command's vertex colors and texture texels carry premultiplied alpha:
 * blend with ONE / ONE_MINUS_SRC_ALPHA for both color and alpha instead of
 * the default SRC_ALPHA / ONE_MINUS_SRC_ALPHA. Set per command, so one frame
 * may mix both conventions. RmlUi hands the renderer premultiplied vertex
 * colors (Rml::Vertex::colour is ColourbPremultiplied) and premultiplied
 * texture bytes (RenderInterface::GenerateTexture), so the UI path
 * (src/runtime/ui/ui_render.cpp) sets this on every command it emits; the
 * ICORECOMP_UI_TEST frame in gs_parallel.cpp stays straight-alpha. */
#define RT_PGS_OVERLAY_PREMULTIPLIED 4u

typedef struct RtPgsOverlayCmd {
    uint32_t texture;        /* id from rt_pgs_overlay_texture_create; 0 = solid white */
    uint32_t index_offset;
    uint32_t index_count;
    int32_t  vertex_offset;
    float    translate_x, translate_y;
    int32_t  scissor_x, scissor_y, scissor_w, scissor_h;
    uint32_t flags;
    float    transform[16]; /* column-major, column vectors */
} RtPgsOverlayCmd;

typedef struct RtPgsOverlayFrame {
    const RtPgsOverlayVertex* vertices; uint32_t vertex_count;
    const uint32_t*           indices;  uint32_t index_count;
    const RtPgsOverlayCmd*    cmds;     uint32_t cmd_count;
    uint32_t surface_width, surface_height; /* the size the geometry was laid out for */
} RtPgsOverlayFrame;

/* Uploads an immutable RGBA8 texture and returns an id for use as
 * RtPgsOverlayCmd::texture (0 always means "no texture, solid white").
 * 0 on failure, logged. Between frames only: this allocates and submits
 * GPU work, so it reuses the m_in_frame fatal guard (fatal if called from
 * pump_events or otherwise while a frame is in flight). */
RT_GS_API uint32_t rt_pgs_overlay_texture_create(RtPgs* pgs, const uint8_t* rgba8, uint32_t width, uint32_t height);
/* Between frames only; same m_in_frame guard as texture_create. Destroying
 * an id still referenced by the retained overlay frame is the caller's bug;
 * the library does not scan for that. */
RT_GS_API void     rt_pgs_overlay_texture_destroy(RtPgs* pgs, uint32_t texture);
/* Deep-copies frame into retained storage; drawn by every present (both
 * rt_pgs_present_pump's windowed path and rt_pgs_present_ui) until
 * replaced, so a repeat present redraws the retained frame as it stands
 * rather than dropping the overlay. NULL
 * or an empty frame clears it. A malformed frame (out-of-range index/vertex
 * ranges, an unknown texture id) is rejected whole with one loud log; the
 * previous frame keeps drawing rather than risk out-of-bounds geometry.
 * Between frames only; same m_in_frame guard as texture_create. */
RT_GS_API void     rt_pgs_overlay_set_frame(RtPgs* pgs, const RtPgsOverlayFrame* frame);
/* One presented frame with no guest scanout: clear the backbuffer, draw the
 * retained overlay frame, present. For the launcher and any future overlay
 * menu drawn while the guest is not producing scanout. Same RT_PGS_VSYNC_*
 * return as rt_pgs_vsync; headless builds log once and return 0. Respects
 * m_in_frame (fatal if called reentrantly, e.g. from pump_events). */
RT_GS_API uint32_t rt_pgs_present_ui(RtPgs* pgs);

/* Headless replay of a raw dump (gs_dumpwriter.cpp format) through
 * paraLLEl-GS's own GSDumpParser; the consumer-side check of the dump
 * writer. Progress and errors go to stderr. Returns 0 on success, nonzero
 * otherwise (same contract as the icorecomp-gs-replay exit status).
 * screenshot_path may be NULL. */
RT_GS_API int rt_pgs_replay(const char* dump_path, const char* screenshot_path, int verbose);

#ifdef __cplusplus
}
#endif

#endif /* ICORECOMP_GS_PARALLEL_API_H */
