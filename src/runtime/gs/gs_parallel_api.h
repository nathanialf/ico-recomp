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

/* End of field: render, and present when a window is up. Returns a bitmask
 * of RT_PGS_VSYNC_* flags; WINDOW_CLOSED asks the host to exit cleanly (the
 * library never terminates the process itself). */
#define RT_PGS_VSYNC_PRESENTED     1u
#define RT_PGS_VSYNC_WINDOW_CLOSED 2u
RT_GS_API uint32_t rt_pgs_vsync(RtPgs* pgs, unsigned field);

/* ---- threads ------------------------------------------------------------
 *
 * One instance, two host threads at most, never at the same time:
 *
 *   the creating thread   the host's EE and main thread. It called
 *                         rt_pgs_create, owns the SDL window and is the only
 *                         thread that may call SDL, so it is the only thread
 *                         that may call rt_pgs_notify_* and
 *                         rt_pgs_sample_window_state.
 *   the consumer thread   the host's GS command ring worker
 *                         (src/runtime/gs/gs_threaded.cpp). After
 *                         rt_pgs_bind_consumer_thread it is the only thread
 *                         that calls rt_pgs_submit_gif, rt_pgs_write_priv,
 *                         rt_pgs_vsync, rt_pgs_set_*, rt_pgs_overlay_* and
 *                         rt_pgs_present_ui.
 *
 * The rest is readable from either thread at any time: rt_pgs_read_priv,
 * rt_pgs_present_timings, rt_pgs_present_rect, rt_pgs_surface_size,
 * rt_pgs_window_handle and rt_pgs_window_closed. The state behind those is
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

/* Present-path timings for the host's profiler: nanoseconds accumulated in
 * the renderer flush, the scanout and the swapchain present since the
 * previous call, and the number of vsyncs they cover. Reading clears them.
 * The host reports per profile window and cannot see inside the library, so
 * the split has to cross the ABI. Any pointer may be NULL. */
RT_GS_API void rt_pgs_present_timings(RtPgs* pgs, uint64_t* flush_ns,
                                      uint64_t* scanout_ns, uint64_t* present_ns,
                                      uint64_t* fields);

/* Window control and event-pump inversion (shim 3). The exe owns the only
 * SDL_PollEvent loop (host/window.cpp); these let it drive the window the
 * library created without the library polling SDL itself. */

/* SDL_Window* as void*, NULL when headless. Opaque on purpose: the exe must
 * not link SDL types through this boundary, only pass the pointer to SDL
 * calls it makes itself (see host/window.cpp). */
RT_GS_API void* rt_pgs_window_handle(RtPgs* pgs);
/* Reported the same way the inline SDL_EVENT_QUIT handling used to: the
 * next vsync's RT_PGS_VSYNC_WINDOW_CLOSED reflects it. */
RT_GS_API void  rt_pgs_notify_quit(RtPgs* pgs);
/* Reported the same way the inline SDL_EVENT_WINDOW_RESIZED /
 * SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED handling used to: the swapchain is
 * rebuilt at the next presentable frame. */
RT_GS_API void  rt_pgs_notify_resize(RtPgs* pgs);
/* Current surface size in pixels, 0x0 when headless. */
RT_GS_API void  rt_pgs_surface_size(RtPgs* pgs, uint32_t* width, uint32_t* height);
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
 * thread at any time (see the threads section above): the host reads it on
 * the EE thread to map a mouse position into the picture while the consumer
 * thread is presenting. */
RT_GS_API void rt_pgs_present_rect(RtPgs* pgs, int32_t* x, int32_t* y, int32_t* w, int32_t* h, int32_t* bb_w, int32_t* bb_h);
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
 * rt_pgs_vsync's windowed path and rt_pgs_present_ui) until replaced. NULL
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
