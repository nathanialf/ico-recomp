/* gs/gs_parallel_api.h: the C ABI of libicorecomp-parallel-gs.
 *
 * This header is the whole boundary between the MIT runtime and the LGPLv3+
 * paraLLEl-GS shared library. Everything Granite/paraLLEl-GS (all C++) stays
 * inside the library, behind gs_parallel_lib.cpp; the executable side
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
     * and call rt_pgs_notify_quit / rt_pgs_notify_resize. Any other
     * rt_pgs_* call from inside it is fatal (see the in-frame guard). */
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

/* Startup options resolved by the host (settings.json with environment
 * variables taking precedence). Extend at the end only. */
typedef struct RtPgsCreateOptions {
    uint32_t present_mode;   /* RT_PGS_PRESENT_* */
    uint32_t fit;            /* RT_PGS_FIT_* */
    uint32_t filter;         /* RT_PGS_FILTER_* */
    uint32_t render_scale;   /* 1/2/4/8/16, paraLLEl-GS SuperSampling factor */
    uint32_t hires_scanout;  /* nonzero: high resolution scanout; needs render_scale >= 4 */
    /* Initial window size in logical pixels. 0 (either field): the historic
     * 640x480 default (see init_windowed's comment on why 4:3). */
    uint32_t window_width, window_height;
} RtPgsCreateOptions;

/* Creates the live backend. Never returns NULL: unrecoverable setup errors
 * (no Vulkan loader/device, GSInterface init failure) go through
 * host->fatal. `host` is copied; the pointed-to struct need not outlive the
 * call. `opts` may be NULL, meaning the pre-settings defaults (present mode
 * from ICORECOMP_GS_PRESENT as before this struct existed, letterbox,
 * linear filtering, render scale 1, hires scanout off); otherwise it is
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

RT_GS_API void rt_pgs_report_stats(RtPgs* pgs);

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
/* Between frames only; fatal when called while a frame is in flight
 * (including from pump_events). Same RT_PGS_PRESENT_* values as create. */
RT_GS_API void  rt_pgs_set_present_mode(RtPgs* pgs, uint32_t mode);
/* Presentation of the final scanout only (fit + filter); takes effect at
 * the next present. Safe between frames; fatal from pump_events. */
RT_GS_API void  rt_pgs_set_presentation(RtPgs* pgs, uint32_t fit, uint32_t filter);
/* Retunes super-sampling in flight (dynamic_super_sampling was set at
 * init). Between frames only; fatal mid-frame. Invalid factor is fatal
 * (host validates). hires_scanout below 4x logs once and stays off. */
RT_GS_API void  rt_pgs_set_render_scale(RtPgs* pgs, uint32_t factor, uint32_t hires_scanout);

/* Overlay rendering: a small textured/scissored 2D pass drawn on top of the
 * swapchain backbuffer, used by the settings menu and (later) RmlUi. Plain
 * POD in, no host callback needed; the library owns the Vulkan pipeline,
 * pooled vertex/index buffers and texture images. */

typedef struct RtPgsOverlayVertex {
    float x, y;       /* pixels, origin top-left of the surface */
    float u, v;
    uint32_t rgba;    /* R8G8B8A8_UNORM byte order, straight (non-premultiplied) alpha */
} RtPgsOverlayVertex;

#define RT_PGS_OVERLAY_SCISSOR   1u   /* scissor_* fields are valid */
#define RT_PGS_OVERLAY_TRANSFORM 2u   /* transform[] is not identity */

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
