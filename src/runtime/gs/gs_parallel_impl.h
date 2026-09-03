/* gs/gs_parallel_impl.h: the RtPgs instance shared by the shim's translation
 * units.
 *
 * Private to libicorecomp-parallel-gs. gs_parallel_api.h keeps RtPgs opaque
 * for the executables; this header is the complete type, so it may use the
 * Granite and paraLLEl-GS C++ interfaces freely. The method bodies live in:
 *
 *   gs_parallel_lib.cpp      construction, teardown, GIF/PRIV submission
 *   gs_parallel_scanout.cpp  vsync, scanout geometry, display copy phase
 *   gs_parallel_present.cpp  device/window/swapchain setup, present, window
 *                            control
 *   gs_parallel_overlay.cpp  overlay textures, retained frame, overlay pass
 *   gs_parallel_abi.cpp      the rt_pgs_* C ABI and the replay entry point
 */
#ifndef ICORECOMP_GS_PARALLEL_IMPL_H
#define ICORECOMP_GS_PARALLEL_IMPL_H

#include "gs_parallel_api.h"

#include "context.hpp"
#include "device.hpp"
#include "gs_interface.hpp"

#ifdef ICORECOMP_PGS_SDL
#include "wsi.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/* One phrase per mode, shared by the startup lines in gs_parallel_lib.cpp and
 * the live changes in gs_parallel_present.cpp, so a log reads the same
 * whether the value came from settings.json or from the menu. */
inline const char* rt_pgs_raster_log_text(uint32_t raster) {
    return raster == RT_PGS_RASTER_WINDOW
        ? "window (display.raster): frame grows to the display window,"
          " DBX/DBY read offset ignored"
        : "crt (display.raster): the renderer's mode area, a window past it is cropped";
}

inline const char* rt_pgs_deinterlace_name(uint32_t mode) {
    switch (mode) {
    case RT_PGS_DEINTERLACE_BOB: return "bob";
    case RT_PGS_DEINTERLACE_WEAVE: return "weave";
    default: return "adaptive";
    }
}

/* The opaque instance behind RtPgs*. Method bodies moved intact from the
 * pre-C-ABI gs_parallel.cpp ParallelBackend; behavior changes are limited to
 * host-callback logging and reporting window closure instead of exiting. */
struct RtPgs {
    RtPgs(const RtPgsHost& host, const RtPgsCreateOptions* opts);
    ~RtPgs();

    void logf(const char* fmt, ...);
    [[noreturn]] void fatalf(const char* fmt, ...);

    void submit_gif(int path, const uint8_t* data, uint32_t qwords);
    /* Reads the display copy's per-field XYOFFSET_1 out of the GIF stream so
     * vsync can hand the renderer the phase the copy in VRAM was actually
     * drawn for. See the comment on info.phase in vsync(). */
    void snoop_display_copy_phase(const uint8_t* data, uint32_t qwords);
    void note_xyoffset(uint32_t reg, uint32_t ofy);
    void note_display_register(uint32_t offset, uint64_t old_v, uint64_t new_v);
    void write_priv(uint32_t offset, uint64_t v);
    uint64_t read_priv(uint32_t offset);
    uint32_t vsync(unsigned field);
    void report_stats();
    /* See rt_pgs_present_timings in gs_parallel_api.h. Reading clears. */
    void present_timings(uint64_t* flush_ns, uint64_t* scanout_ns,
                         uint64_t* present_ns, uint64_t* fields);

    /* Window control / event pump inversion (shim 3); see gs_parallel_api.h. */
    void* window_handle();
    void notify_quit();
    void notify_resize();
    void surface_size(uint32_t* width, uint32_t* height);
    void set_present_mode(uint32_t mode);
    void set_presentation(uint32_t fit, uint32_t filter);
    void set_raster(uint32_t raster);
    void set_deinterlace(uint32_t deinterlace);
    void set_render_scale(uint32_t factor);

    /* Overlay rendering (milestone 4); see gs_parallel_api.h. Works headless
     * or windowed (texture upload/retained frame are plain Vulkan::Device
     * operations); only present_ui's actual draw needs a swapchain, and
     * logs once and no-ops when there is none. */
    uint32_t overlay_texture_create(const uint8_t* rgba8, uint32_t width, uint32_t height);
    void overlay_texture_destroy(uint32_t texture);
    void overlay_set_frame(const RtPgsOverlayFrame* frame);
    uint32_t present_ui();

private:
    void init_headless();
    void ensure_overlay_white();

#ifdef ICORECOMP_PGS_SDL
    /* Minimal Vulkan::WSIPlatform on SDL3. Only what a fixed-function "blit
     * the scanout" presenter needs: surface creation, size queries, an alive
     * flag and event pumping. */
    class SdlWsiPlatform final : public Vulkan::WSIPlatform {
    public:
        explicit SdlWsiPlatform(RtPgs& owner) : m_owner(owner) {}

        bool init(unsigned width, unsigned height) {
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                m_owner.logf("paraLLEl-GS: SDL_Init failed: %s", SDL_GetError());
                return false;
            }
            m_window = SDL_CreateWindow("ico-recomp", int(width), int(height),
                                        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
            if (!m_window) {
                m_owner.logf("paraLLEl-GS: SDL_CreateWindow failed: %s", SDL_GetError());
                return false;
            }
            return true;
        }

        ~SdlWsiPlatform() override {
            if (m_window) SDL_DestroyWindow(m_window);
            m_window = nullptr;
        }

        VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice) override {
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            if (!SDL_Vulkan_CreateSurface(m_window, instance, nullptr, &surface)) {
                m_owner.logf("paraLLEl-GS: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
                return VK_NULL_HANDLE;
            }
            return surface;
        }

        std::vector<const char*> get_instance_extensions() override {
            Uint32 count = 0;
            const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&count);
            if (!exts) return {};
            return std::vector<const char*>(exts, exts + count);
        }

        uint32_t get_surface_width() override {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(m_window, &w, &h);
            return uint32_t(w > 0 ? w : 1);
        }

        uint32_t get_surface_height() override {
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(m_window, &w, &h);
            return uint32_t(h > 0 ? h : 1);
        }

        bool alive(Vulkan::WSI&) override { return m_alive; }

        /* Shared by the inline poll_input() loop below and the
         * rt_pgs_notify_quit / rt_pgs_notify_resize entry points, so the
         * exe-side pump (host/window.cpp) and the library's own fallback
         * pump apply state changes identically. Public: RtPgs (the
         * enclosing class) calls these directly, and a nested class does
         * not automatically grant the enclosing class access to its own
         * private members. */
        void handle_quit() { m_alive = false; }
        void handle_resize() { resize = true; }
        SDL_Window* window() const { return m_window; }

        /* True when begin_frame() is safe to call. A minimized window makes
         * the driver report a 0x0 maxImageExtent, which Granite answers with
         * SwapchainError::NoSurface and a call to
         * block_until_wsi_forward_progress. That blocks on the calling
         * thread, and the caller here is the EE thread, so guest execution
         * would stop for as long as the window stays minimized. Callers skip
         * the frame instead. */
        bool presentable() {
            if (!m_alive || !m_window) return false;
            return (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) == 0;
        }

        /* Reached only if the window is minimized between presentable() and
         * begin_frame(). Granite's blocking_init_swapchain loops
         * `do { init_swapchain(); } while (err != None)` with no way to give
         * up on NoSurface, so returning while the window is gone would spin
         * that loop forever with no way for the host to see the close. The
         * window being gone is exactly the condition gs_parallel.cpp answers
         * with exit(0); apply the same policy from the one place that cannot
         * return to it. */
        void block_until_wsi_forward_progress(Vulkan::WSI& wsi) override {
            m_owner.logf("paraLLEl-GS: window cannot present (minimized), guest execution is blocked");
            while (!resize && alive(wsi)) {
                poll_input();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!alive(wsi)) {
                m_owner.logf("paraLLEl-GS: window closed while the swapchain was unusable, exiting");
                std::exit(0);
            }
        }

        /* Event pump inversion: when the host supplied pump_events (the exe
         * owns the only SDL_PollEvent loop; see gs_parallel_api.h and
         * host/window.cpp), hand control to it instead of polling here. NULL
         * keeps the pre-shim-3 behavior (icorecomp-gs-replay, or any host
         * without a UI). */
        void poll_input() override {
            if (m_owner.m_host.pump_events) {
                m_owner.m_host.pump_events();
                return;
            }
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                    case SDL_EVENT_QUIT:
                        handle_quit();
                        break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    case SDL_EVENT_WINDOW_RESIZED:
                        handle_resize();
                        break;
                    default:
                        break;
                }
            }
        }

        void poll_input_async(Granite::InputTrackerHandler*) override { poll_input(); }

    private:
        RtPgs& m_owner;
        SDL_Window* m_window = nullptr;
        bool m_alive = true;
    };

    void init_windowed();
    void present(const ParallelGS::ScanoutResult& scanout, double aspect);
    void present_frame(const ParallelGS::ScanoutResult& scanout, double aspect);
    void draw_overlay(Vulkan::CommandBuffer& cmd);
    uint32_t present_ui_windowed();
#endif /* ICORECOMP_PGS_SDL */

    RtPgsHost m_host;
    RtPgsCreateOptions m_opts{};
    /* True when rt_pgs_create was given a non-NULL opts: the caller (today,
     * always gs_parallel.cpp) has already resolved settings.json vs
     * environment, so this instance must not re-read ICORECOMP_GS_PRESENT
     * itself. False is the NULL path documented on rt_pgs_create. */
    bool m_have_opts = false;
    std::unique_ptr<Vulkan::Context> m_headless_context;
    std::unique_ptr<Vulkan::Device> m_headless_device;
#ifdef ICORECOMP_PGS_SDL
    std::unique_ptr<SdlWsiPlatform> m_platform;
    std::unique_ptr<Vulkan::WSI> m_wsi;
#endif
    Vulkan::Device* m_device = nullptr; /* whichever of the above is live */
    /* Vulkan pipeline cache persistence (gs_parallel_present.cpp). The
     * standalone paraLLEl-GS build turns Granite's own cache file handling
     * off (GRANITE_VULKAN_SYSTEM_HANDLES), so without these two calls every
     * run compiles every pipeline from scratch: about three seconds of the
     * first field window on the EE thread, and a synchronous compile for
     * any variant first met mid-run. The file lives in cache/ next to the
     * executable (SDL_GetBasePath), which is writable for the packaged
     * layout this port ships and is not for a system-wide install: the
     * store there logs and gives up rather than failing the run. A stale or
     * foreign file is rejected by Granite's own checks, never the driver's,
     * and the load starts from an empty cache when that happens. */
    void pipeline_cache_load();
    void pipeline_cache_store();
    std::string m_pipeline_cache_path;
    std::unique_ptr<ParallelGS::GSInterface> m_iface;
    const char* m_screenshot_path = nullptr;
    uint64_t m_vsyncs = 0;

    /* Present-path timings since the host last read them (vsync stamps
     * them, present_timings clears them). Three steady_clock pairs per
     * field, which is nothing next to the work they bracket.
     *
     * Plain integers, not atomics, because vsync and present_timings are
     * both called from the host's EE thread today (gs/gs_threaded.cpp
     * drains its ring inline). They have to become atomics on the day that
     * ring gets a worker thread, since vsync would then write them while
     * the profiler reads and clears them. */
    uint64_t m_flush_ns = 0;
    uint64_t m_scanout_ns = 0;
    uint64_t m_present_ns = 0;
    uint64_t m_timing_fields = 0;
    bool m_transfer_since_vsync = false;
    bool m_wsi_active = false;
    bool m_window_closed = false;
    /* True from a successful m_wsi->begin_frame() until the matching
     * end_frame(). Swapchain-touching entry points (set_present_mode,
     * set_presentation, set_render_scale) fatal while this is set: they run
     * from pump_events, which Granite calls from inside begin_frame, and
     * Vulkan::WSI::set_present_mode would otherwise silently no-op mid-frame
     * instead of taking effect. */
    bool m_in_frame = false;
    /* Last (internal w, internal h, mode w, mode h, deinterlaced,
     * render scale, high-resolution scanout, deinterlace mode) whose aspect
     * was logged, so a geometry, scale or mode change is visible in the log
     * without spamming every field. Kept in the same order the geom[]
     * literal in vsync() builds. */
    uint32_t m_aspect_log_geom[8] = {};
    /* Fields still owed a "crtc field" line. Armed at construction, re-armed
     * whenever the scanout geometry line above fires, and re-armed once on the
     * first field that takes the DISPFB branch of the phase derivation, so a
     * log always carries a few consecutive fields of CRTC state for both
     * phases without the line running for the whole session. Two phases plus a
     * spare pair. */
    unsigned m_crtc_log_left = 6;

    /* Display copy phase tracking (snoop_display_copy_phase / note_xyoffset).
     *
     * ICO's per-field half pixel lives in XYOFFSET_1: the SDK helper at
     * 0x00243640 (../ico src/cod/vendor_2418A0.c:203-212) writes OFY = t on
     * one field and OFY = t + 8 on the other, and nothing else in the packet
     * stream produces a fractional OFY, because every other XYOFFSET the game
     * builds is a whole number of pixels shifted left by 4 (gsb_setNormalReg
     * builds its own as (0x800 - w/2) << 4 | (0x800 - h/4) << 36).
     *
     * So the first XYOFFSET whose OFY fraction is 8 identifies both the
     * register and the base value; from then on OFY == base + 8 means the
     * "+8" field and OFY == base means the other one. m_copy_parity is that
     * observation for the traffic since the last vsync, which is exactly the
     * traffic that produced the buffer the next vsync scans out. */
    uint32_t m_copy_ofy_reg = 0;      /* 0x18 or 0x19 once latched, else 0 */
    int32_t m_copy_ofy_base = -1;     /* OFY with a zero fraction, once seen */
    int m_copy_parity = -1;           /* this field: -1 none, 0 base, 1 base+8 */
    /* This field: a draw programmed the copy's XYOFFSET at all, whichever
     * half of the pair it was. Separate from m_copy_parity because the parity
     * needs the half pixel form to have been seen once and this does not, so
     * m_hires_from_copy is right from the first copy field rather than from
     * the first half pixel field. Set by note_xyoffset, cleared by vsync. */
    bool m_copy_seen = false;
    int m_last_phase = -1;            /* phase handed over on the previous field */
    uint64_t m_phase_held = 0;        /* fields with no copy, phase repeated */
    uint64_t m_phase_disagreed = 0;   /* fields where the copy and the field counter differ */
    /* Set by write_priv when DISPFB1 or DISPFB2 changes value, cleared by
     * vsync. The attract movie never draws the display copy: it decodes to
     * two field buffers in VRAM and alternates DISPFB2's FBP between them
     * once per field from its own vblank handler, so a field with no copy in
     * it is a new picture there rather than a repeat of the last one. */
    bool m_dispfb_flip = false;
    uint64_t m_phase_from_flip = 0;   /* fields with no copy but a DISPFB change */
    /* False once a field's picture is known to have arrived by a plain
     * DISPFB flip rather than by the super-sampled display copy. It gates
     * info.high_resolution_scanout; see the derivation in RtPgs::vsync.
     * Sticky across held fields, because a held field scans out the same
     * buffer the last decided field did. */
    bool m_hires_from_copy = true;
    /* Set by write_priv for any display register change that is not just the
     * movie's per-field FBP select, cleared by vsync. Forces the scanout
     * geometry line and the CRTC lines to log again, so a log shows the
     * registers each distinct display setup was built from. Budgeted, since
     * the point is a handful of setups and not every field. */
    bool m_display_geom_changed = false;
    unsigned m_display_relog_left = 12;
    /* DISPFB change lines still owed. The first few carry the alternating
     * pair the movie flips between; after that the count decides. */
    unsigned m_dispfb_log_left = 8;
    /* DISPFB changes that touch something other than FBP get this many lines
     * beyond the opening budget and the power-of-two counter. A bound, not an
     * exemption: a game that reprograms the display every field must not turn
     * the change log into a per-field log. */
    unsigned m_dispfb_geom_log_left = 8;
    uint64_t m_dispfb_changes = 0;
    /* The attract movie's field pair, held between vsyncs, in
     * display.deinterlace weave only. Its two buffers are the even and odd
     * rows of one decoded 29.97 fps picture (two moments 1/60 s apart, the
     * source being interlaced video) and the pair is only complete on the
     * field that uploaded the second half, so the other field presents this
     * copy instead of a composition that pairs rows of two different
     * pictures. Adaptive and bob hold nothing. See RtPgs::vsync. */
    ParallelGS::ScanoutResult m_held_scanout = {};
    double m_held_aspect = 0.0;
    uint64_t m_pair_repeats = 0;
    /* The crop lines: the game asked for a display window wider or taller
     * than the mode area the renderer models, so the right or the bottom of
     * it is not scanned out. Loud rather than silent, per the accuracy rule.
     *
     * One DH and one DW, so the state is "the last window height that was
     * reported" and not a set: a run that alternates between two heights
     * re-reports both. m_crop_log_left is what bounds that, shared by the two
     * lines, since a change detector on its own is not a bound. */
    uint32_t m_crop_logged_dh = 0;
    uint32_t m_crop_logged_dw = 0;
    /* Last non-zero (DBX << 16 | DBY) that window mode reported as ignored,
     * so the line fires once per distinct pair rather than per field. */
    uint32_t m_dbxy_ignored_logged = 0;
    /* Once-only: window mode on a CMOD whose window aspect is not derived. */
    bool m_window_aspect_cmod_logged = false;
    unsigned m_crop_log_left = 8;
    bool m_copy_ofy_logged = false;   /* the once-only calibration line */
    unsigned m_copy_ofy_search_left = 600; /* fields spent looking before saying so */

    /* Overlay render state (milestone 4). Retained frame is deep-copied by
     * overlay_set_frame and redrawn by every present (rt_pgs_vsync's
     * windowed path and rt_pgs_present_ui) until replaced; empty means
     * nothing to draw. Texture ids start at 1; 0 always means "no texture,
     * draw the white fallback" (RtPgsOverlayCmd::texture). */
    std::vector<RtPgsOverlayVertex> m_overlay_vertices;
    std::vector<uint32_t> m_overlay_indices;
    std::vector<RtPgsOverlayCmd> m_overlay_cmds;
    uint32_t m_overlay_surface_width = 0, m_overlay_surface_height = 0;
    std::unordered_map<uint32_t, Vulkan::ImageHandle> m_overlay_textures;
    uint32_t m_overlay_next_texture = 1;
    Vulkan::ImageHandle m_overlay_white; /* 1x1 fallback for untextured draws */
    Vulkan::Program* m_overlay_program = nullptr; /* lazily requested, see draw_overlay */
    bool m_overlay_ui_headless_logged = false; /* present_ui's once-only headless log */
    bool m_overlay_diag_logged = false;        /* draw_overlay's once-only first-draw log */

};

#endif /* ICORECOMP_GS_PARALLEL_IMPL_H */
