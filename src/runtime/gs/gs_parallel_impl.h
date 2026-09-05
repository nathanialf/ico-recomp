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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
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

/* The host's one log callback carries no level of its own (RtPgsHost in
 * gs_parallel_api.h), so a line that has to reach a reader at the default
 * warn level says so in its own text: warnf and errorf below prefix these
 * tags, and gs_parallel.cpp's host_log strips the tag and picks the level
 * from it. A host that does not know the tags prints it inline, which keeps
 * the line readable instead of losing it. Keep the two files in step. */
#define RT_PGS_LOG_TAG_WARN  "[warn] "
#define RT_PGS_LOG_TAG_ERROR "[error] "

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
    /* Same, at the two levels above it: warnf for something that happened
     * differently from what was asked (a refusal, a fallback, a skipped
     * present), errorf for something that did not happen at all. See the
     * level table in src/runtime/runtime.h. */
    void warnf(const char* fmt, ...);
    void errorf(const char* fmt, ...);
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
    /* See rt_pgs_present_pump in gs_parallel_api.h. Consumer thread only. */
    uint32_t present_pump(double max_hz, uint64_t* serial);
    void report_stats();
    /* See rt_pgs_present_timings in gs_parallel_api.h. Reading clears. */
    void present_timings(RtPgsPresentTimings* out);

    /* Threads; see the threads section of gs_parallel_api.h.
     * on_owner_thread() is what the WSI platform above asks before it does
     * anything SDL: true only on the thread that created this instance and
     * the window. */
    void bind_consumer_thread();
    /* Points Granite's thread-local logging interface at this instance, so
     * its own LOGE/LOGW/LOGI lines (the WSI's VkResult reports among them)
     * reach the host log at their own level instead of an fprintf(stderr)
     * from inside this shared library. Thread local, so every thread that
     * calls into Granite installs it: the creating thread from the
     * constructor, the consumer thread from bind_consumer_thread. */
    void install_granite_log();
    bool window_closed() const { return m_window_closed.load(std::memory_order_acquire); }
    void sample_window_state();
    bool on_owner_thread() const { return std::this_thread::get_id() == m_owner_thread; }

    /* Window control / event pump inversion (shim 3); see gs_parallel_api.h.
     * The window handle and the surface size are host_window_service.h's to
     * answer (rt_window_handle, rt_window_surface_size); the library keeps no
     * entry point for either. */
    void notify_quit();
    void notify_resize();
    void present_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h,
                      int32_t* bb_w, int32_t* bb_h);
    void request_screenshot(uint32_t slots);
    size_t take_screenshot(uint32_t slot, uint32_t* w, uint32_t* h, uint8_t* dst, size_t dst_bytes);
    void set_present_mode(uint32_t mode);
    void set_presentation(uint32_t fit, uint32_t filter);
    void set_raster(uint32_t raster);
    void set_deinterlace(uint32_t deinterlace);
    void set_widescreen_aspect(double aspect);
    void set_render_scale(uint32_t factor);

    /* Overlay rendering (milestone 4); see gs_parallel_api.h. Works headless
     * or windowed (texture upload/retained frame are plain Vulkan::Device
     * operations); only present_ui's actual draw needs a swapchain, and
     * logs once and no-ops when there is none. */
    /* The Vulkan device this instance is running on, windowed or headless.
     * gs_probe_lib.cpp's rt_pgs_live_probe reads its properties so the
     * Display tab reports the device the run actually created rather than
     * one a startup probe made and threw away. Null before construction
     * finishes. */
    Vulkan::Device* live_device() { return m_device; }

    /* Whether this instance's own context dropped the descriptor-buffer bit
     * (gs_pgs_context.h). rt_pgs_live_probe reports it, because it is the
     * one fact about the running device that only the instance that made it
     * knows: the properties every other probe field comes from are readable
     * off the device, and this one is not. */
    bool descriptor_buffer_disabled() const { return m_descbuf_disabled; }

    uint32_t overlay_texture_create(const uint8_t* rgba8, uint32_t width, uint32_t height);
    void overlay_texture_destroy(uint32_t texture);
    void overlay_set_frame(const RtPgsOverlayFrame* frame);
    uint32_t present_ui();

private:
    void init_headless();
    void ensure_overlay_white();
    /* Finishes every screenshot slot whose copy has been submitted: waits its
     * fence, converts the staging bytes to tightly packed RGBA8 and publishes
     * them for rt_pgs_take_screenshot. Called at the top of each present, so
     * the wait is on a submit a whole present old and returns at once, and
     * once more from the destructor before the device goes. Outside the SDL
     * guard in gs_parallel_present.cpp, next to present_rect, because the
     * teardown path reaches it in a headless build too. */
    void drain_screenshots();
    /* Presentation accounting (gs_parallel_present.cpp). Every path that
     * leaves present_frame or present_ui_windowed without a picture on the
     * swapchain calls note_present_skipped with its reason, and every one
     * that finishes a present calls note_present_resumed: one warn when the
     * window stops being fed and one info naming the count when it starts
     * again, never a line a field. Consumer-thread state, like the rest of
     * the present path. */
    void note_present_skipped(const char* reason);
    void note_present_resumed();

#ifdef ICORECOMP_PGS_SDL
    /* Minimal Vulkan::WSIPlatform on SDL3. Only what a fixed-function "blit
     * the scanout" presenter needs: surface creation, size queries, an alive
     * flag and event pumping.
     *
     * Threads: Granite calls every method here from whichever thread is
     * inside the WSI, which after the host starts its GS command ring worker
     * is not the thread that owns the window. SDL's video functions belong to
     * the thread that created the window (on Windows the message queue is
     * per thread), so the only SDL calls left here are the two queries in
     * sample_state(), which runs on the creating thread. What the
     * WSI asks for between frames -- surface size, minimized, alive, resize
     * -- is served from the atomics below, refreshed by sample_state() from
     * the host's own event pump once per field. */
    class SdlWsiPlatform final : public Vulkan::WSIPlatform {
    public:
        explicit SdlWsiPlatform(RtPgs& owner) : m_owner(owner) {}

        /* Adopts the host's window. This library creates no window of its
         * own any more: the executable owns the one window of the run
         * (host/window_service.h), created with SDL_WINDOW_VULKAN before
         * this instance existed, and passes it in through
         * RtPgsCreateOptions::host_window. So there is no SDL_Init here, no
         * SDL_CreateWindow, and no SDL_DestroyWindow in the destructor. */
        bool adopt(SDL_Window* window) {
            if (!window) return false;
            m_window = window;
            /* Primes the cache the WSI reads from before the first
             * begin_frame asks for a surface size. */
            sample_state();
            return true;
        }

        /* Creating thread only: the two SDL queries below are the reason.
         * Called from adopt(), from the host's event pump every field, and
         * from notify_resize. */
        void sample_state() {
            if (!m_window) return;
            int w = 0, h = 0;
            SDL_GetWindowSizeInPixels(m_window, &w, &h);
            m_surface_w.store(uint32_t(w > 0 ? w : 1), std::memory_order_relaxed);
            m_surface_h.store(uint32_t(h > 0 ? h : 1), std::memory_order_relaxed);
            m_minimized.store((SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) != 0,
                              std::memory_order_relaxed);
        }

        /* The window belongs to the host, which destroys it after
         * rt_pgs_destroy has returned. Dropping the pointer is the whole of
         * this platform's teardown. */
        ~SdlWsiPlatform() override { m_window = nullptr; }

        /* The host makes the surface, because the host owns the window: the
         * callback is host/window_service.cpp's
         * rt_window_create_vulkan_surface, reached through the C ABI as a
         * pair of uint64_t so no Vulkan type crosses it. RtPgs's constructor
         * has already made a NULL callback with an adopted window fatal, so
         * the pointer is valid here whenever m_window is. */
        VkSurfaceKHR create_surface(VkInstance instance, VkPhysicalDevice) override {
            uint64_t inst_bits = 0;
            static_assert(sizeof(instance) == sizeof(void*), "VkInstance is a pointer handle");
            void* raw = (void*)instance;
            std::memcpy(&inst_bits, &raw, sizeof(raw));

            const uint64_t surface_bits = m_owner.m_host.create_vulkan_surface(inst_bits);
            if (surface_bits == 0) {
                m_owner.errorf("paraLLEl-GS: the host could not create a Vulkan surface for its"
                               " window; this run falls back to headless and nothing reaches"
                               " the screen");
                return VK_NULL_HANDLE;
            }
            /* memcpy rather than a cast: VkSurfaceKHR is a pointer on 64-bit
             * targets and a uint64_t on 32-bit ones, and only one of the two
             * casts compiles in each case. */
            static_assert(sizeof(VkSurfaceKHR) == sizeof(uint64_t),
                          "VkSurfaceKHR is 64 bits wide");
            VkSurfaceKHR surface = VK_NULL_HANDLE;
            std::memcpy(&surface, &surface_bits, sizeof(surface));
            return surface;
        }

        std::vector<const char*> get_instance_extensions() override {
            Uint32 count = 0;
            const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&count);
            if (!exts) return {};
            return std::vector<const char*>(exts, exts + count);
        }

        /* Cached, not queried: these are called by Granite from whichever
         * thread is inside the WSI. sample_state() keeps them fresh. */
        uint32_t get_surface_width() override { return m_surface_w.load(std::memory_order_relaxed); }

        uint32_t get_surface_height() override { return m_surface_h.load(std::memory_order_relaxed); }

        bool alive(Vulkan::WSI&) override { return m_alive.load(std::memory_order_acquire); }

        /* Shared by the inline poll_input() loop below and the
         * rt_pgs_notify_quit / rt_pgs_notify_resize entry points, so the
         * exe-side pump (host/window.cpp) and the library's own fallback
         * pump apply state changes identically. Public: RtPgs (the
         * enclosing class) calls these directly, and a nested class does
         * not automatically grant the enclosing class access to its own
         * private members. */
        void handle_quit() { m_alive.store(false, std::memory_order_release); }
        /* Granite's own `resize` flag is a plain bool it reads from inside
         * the WSI, so the host's notification lands in an atomic here and is
         * transferred into it by sync_from_host(), on the WSI's own thread,
         * before every frame. */
        void handle_resize() { m_resize_pending.store(true, std::memory_order_release); }
        void sync_from_host() {
            if (m_resize_pending.exchange(false, std::memory_order_acquire)) resize = true;
        }
        SDL_Window* window() const { return m_window; }

        /* True when begin_frame() is safe to call. A minimized window makes
         * the driver report a 0x0 maxImageExtent, which Granite answers with
         * SwapchainError::NoSurface and a call to
         * block_until_wsi_forward_progress. That blocks on the calling
         * thread; the caller is the GS command ring's consumer, so the ring
         * would back up and the EE would stall on it at the next field sync
         * for as long as the window stays minimized. Callers skip the frame
         * instead. */
        bool presentable() { return not_presentable_reason() == nullptr; }

        /* Why begin_frame would park, as a fixed phrase for the log, or NULL
         * when it would not. Split out of presentable() so the skip lines in
         * present_frame and present_ui_windowed can say which of the three
         * it was instead of "not presentable". */
        const char* not_presentable_reason() {
            if (!m_window) return "the WSI platform has no window";
            if (!m_alive.load(std::memory_order_acquire)) return "the window has closed";
            if (m_minimized.load(std::memory_order_relaxed)) return "the window is minimized";
            return nullptr;
        }

        /* Reached only if the window is minimized between presentable() and
         * begin_frame(). Granite's blocking_init_swapchain loops
         * `do { init_swapchain(); } while (err != None)` with no way to give
         * up on NoSurface, so returning while the window is gone would spin
         * that loop forever.
         *
         * Waiting here is correct as long as somebody keeps pumping events:
         * the host does, once per field on its own thread, and every host
         * wait on this consumer pumps too (gs/gs_threaded.cpp), so the
         * restore or the close still arrives while this parks.
         *
         * A close is the one condition this cannot return from, because the
         * swapchain will never come back. It used to call exit(0) from here
         * whichever thread it was on; with the consumer on its own thread
         * that would run the atexit handler that joins this very thread. So
         * off the window's own thread it records the closure, which the host
         * reads through rt_pgs_window_closed, and parks: the host sees the
         * flag at its next field boundary, logs and exits, and its teardown
         * gives up on this thread after a bounded wait rather than
         * destroying a device it is still inside. On the window's own thread
         * there is no other thread to take that exit -- the launcher, a run
         * with ICORECOMP_GS_THREAD=0, icorecomp-gs-replay -- so it keeps the
         * old exit(0) instead of hanging the process. */
        void block_until_wsi_forward_progress(Vulkan::WSI& wsi) override {
            /* Warn, not info: the consumer thread stops here, so the picture
             * stops with it, and a reader at the default level has to see
             * why. One line per park, and the matching line below says the
             * park ended. */
            m_owner.warnf("paraLLEl-GS: window cannot present (minimized), the GS consumer is"
                          " blocked until the window comes back");
            /* Leaves on any of three: a resize, the window coming back from
             * minimized, or the window going away. The minimized test is not
             * redundant with the resize one. Restoring a minimized window on
             * Windows usually keeps the pixel size, so SDL raises no resize
             * event and the host's notify_resize never fires; the host's
             * per-field state sample clears m_minimized either way. Without
             * this test the consumer would park here for the rest of the run
             * and the EE would wait on it every field.
             *
             * The wait comes before the test, not after it. Granite reaches
             * here because the driver reported a 0x0 surface extent, and
             * m_minimized is sampled on the host's thread a pump later, so
             * on entry the two disagree in exactly the case that gets here
             * (the window was minimized between presentable() and
             * begin_frame). Testing first would return without waiting and
             * spin blocking_init_swapchain's do-while on a full core until
             * the two agree. */
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                poll_input();
                sync_from_host();
                if (resize || !alive(wsi) || !m_minimized.load(std::memory_order_relaxed)) break;
            }
            if (!alive(wsi)) {
                m_owner.m_window_closed.store(true, std::memory_order_release);
                if (m_owner.on_owner_thread()) {
                    m_owner.errorf("paraLLEl-GS: window closed while the swapchain was unusable,"
                                   " exiting");
                    std::exit(0);
                }
                m_owner.errorf("paraLLEl-GS: window closed while the swapchain was unusable;"
                               " the GS consumer is parked and the host exits from its own thread");
                for (;;) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            /* The park ended and the consumer is running again. The pair of
             * lines is what tells a reader whether a run that went quiet was
             * blocked here or somewhere else. */
            m_owner.logf("paraLLEl-GS: window can present again (%s), the GS consumer is running",
                         resize ? "resized" : "restored");
        }

        /* Event pump inversion: when the host supplied pump_events (the exe
         * owns the only SDL_PollEvent loop; see gs_parallel_api.h and
         * host/window.cpp), hand control to it instead of polling here. NULL
         * keeps the pre-shim-3 behavior (icorecomp-gs-replay, or any host
         * without a UI).
         *
         * Off the creating thread this does nothing at all. SDL's event
         * queue belongs to the thread that made the window, and the host
         * pumps it there every field, so a call from the GS consumer has
         * nothing to do and must not poll. Logged once so a log says which
         * of the two shapes the run had. */
        void poll_input() override {
            if (!m_owner.on_owner_thread()) {
                if (!m_pump_skipped_logged) {
                    m_pump_skipped_logged = true;
                    m_owner.logf("paraLLEl-GS: WSI event pump called off the window's own thread;"
                                 " skipped (the host pumps SDL on its own thread every field)");
                }
                return;
            }
            if (m_owner.m_host.pump_events) {
                m_owner.m_host.pump_events();
                return;
            }
            /* No fallback poll any more. The window is the host's, so a host
             * that has a window has an event loop for it; a host that
             * supplied no pump_events also supplied no host_window and is
             * headless, where this platform does not exist at all. Polling
             * SDL from here would take events out of the host's own queue. */
            if (!m_pump_missing_logged) {
                m_pump_missing_logged = true;
                m_owner.warnf("paraLLEl-GS: the WSI asked for an event pump and the host supplied"
                              " none; events are the host's to deliver");
            }
        }

        void poll_input_async(Granite::InputTrackerHandler*) override { poll_input(); }

    private:
        RtPgs& m_owner;
        SDL_Window* m_window = nullptr;
        /* Every one of these is written by the creating thread (the host's
         * event pump) and read by whichever thread is inside the WSI. */
        std::atomic<bool> m_alive{true};
        std::atomic<bool> m_resize_pending{false};
        std::atomic<bool> m_minimized{false};
        std::atomic<uint32_t> m_surface_w{1};
        std::atomic<uint32_t> m_surface_h{1};
        bool m_pump_skipped_logged = false;
        bool m_pump_missing_logged = false;
    };

    void init_windowed();
    /* Both return true only when a frame was handed to the swapchain. False
     * means the window was not presentable (minimized, zero-sized) or
     * begin_frame failed, and nothing reached the screen: present_pump uses
     * that answer to decide whether the serial, the clock and the counters
     * advance, so a minimized window cannot report presents it never made. */
    bool present(const ParallelGS::ScanoutResult& scanout, double aspect);
    bool present_frame(const ParallelGS::ScanoutResult& scanout, double aspect);
    /* Screenshot capture (gs_parallel_present.cpp): records a copy of the
     * rectangle out of the backbuffer into slot `slot`'s staging buffer,
     * leaving the image in `layout` again so the code after it is unchanged.
     * The matching drain_screenshots() is not SDL-only and lives below. */
    void capture_backbuffer(Vulkan::CommandBuffer& cmd, const Vulkan::Image& backbuffer,
                            VkImageLayout layout, uint32_t slot,
                            int32_t x, int32_t y, int32_t w, int32_t h);
    void draw_overlay(Vulkan::CommandBuffer& cmd);
    uint32_t present_ui_windowed();
    /* The two swapchain calls that can fail, reported in one place because
     * Granite gives both of them to us as a bare bool. `where` names what
     * the user lost, a guest field or the overlay. Both count every failure
     * and write one warn carrying the state the call was made in; the
     * begin_frame one also opens a present-skip stop. */
    void note_begin_frame_failed(const char* where);
    void note_end_frame_failed(const char* where);
    /* Raises the sticky window-closed flag the host exits on, and says who
     * raised it and what it consulted. One line per run: the flag is read
     * every field once it is up. */
    void note_window_closed(const char* site);
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

    /* The thread that ran the constructor: the host's EE and main thread,
     * which created the window and is the only one that may call SDL. */
    std::thread::id m_owner_thread;

    /* Present-path timings since the host last read them (vsync stamps
     * them, present_timings clears them). Three steady_clock pairs per
     * field, which is nothing next to the work they bracket.
     *
     * Atomics because vsync runs on the host's GS consumer thread while
     * present_timings is read from its EE thread by the profiler. Each is
     * independent, so a read can land between two of the four stores and
     * bill a field's flush to one window and its present to the next. That
     * is a fraction of a millisecond once per profile window and the
     * alternative is a lock on the present path. */
    std::atomic<uint64_t> m_flush_ns{0};
    std::atomic<uint64_t> m_scanout_ns{0};
    std::atomic<uint64_t> m_present_ns{0};
    std::atomic<uint64_t> m_timing_fields{0};
    /* Presents and, of those, repeats of a serial already on screen. Same
     * reason for the atomics as the four above: written by present_pump on
     * the consumer thread, read by the profiler on the EE thread. */
    std::atomic<uint64_t> m_presents{0};
    std::atomic<uint64_t> m_present_repeats{0};
    bool m_transfer_since_vsync = false;
    bool m_wsi_active = false;
    /* Sticky, and read by the host from its own thread through
     * rt_pgs_window_closed: it is how the host learns of a close even while
     * this instance's consumer thread is parked inside the WSI. */
    std::atomic<bool> m_window_closed{false};
    /* True from a successful m_wsi->begin_frame() until the matching
     * end_frame(). Swapchain-touching entry points (set_present_mode,
     * set_presentation, set_render_scale) fatal while this is set: they run
     * from pump_events, which Granite calls from inside begin_frame, and
     * Vulkan::WSI::set_present_mode would otherwise silently no-op mid-frame
     * instead of taking effect.
     *
     * Consumer-thread state, so a plain bool: everything that sets or tests
     * it (vsync, present, present_ui, the set_* entry points) is on the
     * consumer side of the host's command ring, and the ring replays those
     * records in order, which means a settings applier can no longer land
     * inside a frame at all. Before the ring it could, which is what this
     * guard was written for; it stays as the check that keeps it true. */
    bool m_in_frame = false;
    /* The window-backbuffer rectangle the last present blitted the scanout
     * into, and the backbuffer size it was measured against. Written by
     * present_frame once the fit is resolved (so an integer fit that fell
     * back to letterbox reports the letterbox rectangle), read back by
     * rt_pgs_present_rect. Zero until the first present and while headless.
     * On a field that presented no scanout image the size is reported as
     * 0 by 0 with the backbuffer size still filled in: nothing on that field
     * maps window pixels to guest pixels, and reporting the whole backbuffer
     * would put the caller's cursor on a picture that is not there.
     *
     * present_frame runs on the host's GS consumer thread and
     * rt_pgs_present_rect is read on its EE thread, so the six are published
     * and read under m_present_rect_mu. A mutex rather than six atomics
     * because a caller mapping a cursor into the picture needs one
     * consistent rectangle, not six values that could come from two
     * different presents; it is taken once per present and a couple of times
     * per field on the reader. */
    mutable std::mutex m_present_rect_mu;
    int32_t m_present_x = 0, m_present_y = 0;
    int32_t m_present_w = 0, m_present_h = 0;
    int32_t m_present_bb_w = 0, m_present_bb_h = 0;

    /* Screenshot of the presented picture (rt_pgs_request_screenshot; see
     * gs_parallel_api.h for what the two slots are and why the pre one is
     * the picture without the overlay).
     *
     * Three pieces of state with three different owners:
     *
     *   m_shot_slots      the arm. Consumer-thread only: the request rides
     *                     the host's command ring, so it is set on the same
     *                     thread present_frame runs on and needs no atomic.
     *                     0 means nothing is armed.
     *   m_shot[]          the in-flight copy: a CachedHost staging buffer and
     *                     the fence of the submit that filled it, plus the
     *                     size that was copied. Consumer-thread only as well,
     *                     written by capture_backbuffer and cleared by
     *                     drain_screenshots.
     *   m_shot_ready[]    the published pixels, tightly packed RGBA8 rows
     *                     from the top. Written by drain_screenshots on the
     *                     consumer thread and read by rt_pgs_take_screenshot
     *                     from the host's EE thread, so both sides take
     *                     m_shot_mu. The same reasoning as m_present_rect_mu:
     *                     a reader needs one whole image with its own size,
     *                     not a size from one field and rows from another.
     *
     * The fence is deliberately not waited on where the copy is recorded.
     * present_frame runs on the GS worker thread and a fence wait there is a
     * stall on the field the user is looking at; drain_screenshots is called
     * at the top of the next present instead, by which time a whole present
     * has completed and the wait returns immediately. */
    struct ShotPending {
        Vulkan::BufferHandle buffer;
        Vulkan::Fence fence;
        uint32_t width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    struct ShotReady {
        std::vector<uint8_t> rgba;
        uint32_t width = 0, height = 0;
    };
    /* Set at context creation in init_headless / init_windowed; see
     * descriptor_buffer_disabled() above. */
    bool m_descbuf_disabled = false;
    uint32_t m_shot_slots = 0;
    ShotPending m_shot[RT_PGS_SHOT_SLOTS];
    mutable std::mutex m_shot_mu;
    ShotReady m_shot_ready[RT_PGS_SHOT_SLOTS];
    /* Once-only: a swapchain format shot_format_layout does not know, so the
     * capture is refused rather than published as if it were RGBA8, and the
     * line says so. Two latches, not one: the boot trace samples on field 1
     * and the screenshot only when the user presses the key, so one shared
     * latch would let the boot sample fire first and leave every later
     * screenshot refused with nothing in the log. */
    bool m_shot_format_logged = false;   /* rt_pgs_request_screenshot path */
    bool m_sample_format_logged = false; /* boot trace sample path */

    /* Boot trace, display half. Why: see guest/boot_trace.cpp (the two
     * white flashes reported 2026-09-05). Two streams here, both bounded to
     * the first kBootTraceFields fields and to changes only:
     *   boot_trace_registers  in vsync, the CRTC registers the field was
     *                         scanned out from (PMODE enables, the read
     *                         circuit's DISPFB and DISPLAY, BGCOLOR, SMODE1
     *                         CMOD, SMODE2) and whether the renderer gave
     *                         an image;
     *   sample_backbuffer /   in present_frame, a 16x16 copy out of the
     *   drain_boot_sample     backbuffer centre after the scanout blit (or
     *                         the clear, on a field with no image) and
     *                         before the overlay, read back at the top of
     *                         the next present the way screenshots are,
     *                         and logged as its mean colour whenever the
     *                         colour class (black, white, other) changes.
     * The count matches guest/boot_trace.h RT_BOOT_TRACE_FIELDS so the
     * three logs cover one span; it is repeated here because this file
     * builds into the renderer shared library and includes nothing from
     * guest/. */
    static constexpr uint64_t kBootTraceFields = 600;
    struct BootRegSig {
        uint32_t en1 = 0, en2 = 0, fbp = 0, fbw = 0, psm = 0, dbx = 0, dby = 0;
        uint32_t dx = 0, dy = 0, dw = 0, dh = 0, magh = 0, magv = 0;
        uint32_t bgr = 0, bgg = 0, bgb = 0, cmod = 0, inter = 0, ffmd = 0;
        bool image = false;
        bool operator==(const BootRegSig& o) const {
            return en1 == o.en1 && en2 == o.en2 && fbp == o.fbp && fbw == o.fbw &&
                   psm == o.psm && dbx == o.dbx && dby == o.dby && dx == o.dx &&
                   dy == o.dy && dw == o.dw && dh == o.dh && magh == o.magh &&
                   magv == o.magv && bgr == o.bgr && bgg == o.bgg && bgb == o.bgb &&
                   cmod == o.cmod && inter == o.inter && ffmd == o.ffmd && image == o.image;
        }
    };
    bool m_boot_sig_valid = false;
    BootRegSig m_boot_sig;
    void boot_trace_registers(bool have_image);
    struct BootSample {
        Vulkan::BufferHandle buffer;
        Vulkan::Fence fence;
        uint64_t field = 0;
        bool image = false;
        VkFormat format = VK_FORMAT_UNDEFINED;
    };
    BootSample m_boot_sample;
    /* -1 nothing logged yet; 0 black, 1 white, 2 neither. */
    int m_boot_class = -1;
    void sample_backbuffer(Vulkan::CommandBuffer& cmd, const Vulkan::Image& backbuffer,
                           VkImageLayout layout, int bb_w, int bb_h, bool have_image);
    void drain_boot_sample();
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
     * ICO's per-field half pixel lives in XYOFFSET_1: the SDK helper
     * sceGsSetHalfOffset, at 0x00261048 in the retail ELF, writes OFY = t on
     * one field and OFY = t + 8 on the other according to its fourth
     * argument, and nothing else in the packet stream produces a fractional
     * OFY. That last part is inferred, not measured: every other XYOFFSET the
     * game is believed to build is a whole number of pixels shifted left by 4,
     * and no exhaustive search of the ELF's XYOFFSET writers has been done to
     * confirm it. If one did carry a fraction the snoop would latch on the
     * wrong register, which note_xyoffset's disagreement counter would show.
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

    /* The latest-scanout slot: what vsync finished and what present_pump
     * shows. vsync stores the result it would have presented (the held pair
     * above still decides which result that is) and bumps the serial;
     * present_pump presents it and records which serial reached the window.
     *
     * The ScanoutResult holds an image handle, so storing it keeps the image
     * alive for as long as the slot names it, exactly as m_held_scanout
     * does. That is what makes a repeat present legal: the image it blits is
     * still the one the renderer produced for that field.
     *
     * Consumer-thread state, all of it: vsync and present_pump both run
     * there (gs_parallel_api.h, threads). Only the counters above cross to
     * the EE thread, and they are atomics for that.
     *
     * m_last_present_at is only read when m_presented_serial is nonzero, so
     * its default-constructed value is never used as a time. */
    ParallelGS::ScanoutResult m_latest_scanout = {};
    double m_latest_aspect = 0.0;
    uint64_t m_latest_serial = 0;      /* bumped by every windowed vsync */
    uint64_t m_presented_serial = 0;   /* the serial last put on screen */
    std::chrono::steady_clock::time_point m_last_present_at{};
    /* The present-skip latch: the reason currently keeping pictures off the
     * window (NULL when they are reaching it) and how many fields have been
     * skipped since that reason started. Compared by text, so a different
     * reason opens a new stop with its own line. */
    const char* m_present_skip_reason = nullptr;
    uint64_t m_present_skipped = 0;
    uint64_t m_present_skipped_total = 0;
    /* Once per run, then counted: begin_frame and end_frame are per-field
     * calls, so their failures cannot each take a line. report_stats prints
     * the totals. */
    bool m_begin_frame_logged = false;
    uint64_t m_begin_frame_failures = 0;
    bool m_end_frame_logged = false;
    uint64_t m_end_frame_failures = 0;
    /* Fields the renderer produced no image for, in the same shape: one warn
     * when the picture stops, one info with the count when it comes back.
     * A field with no image presents a cleared backbuffer, which is a black
     * window, and nothing else in the log says so. */
    bool m_no_image_logged = false;
    uint64_t m_no_image_fields = 0;
    uint64_t m_no_image_total = 0;
    /* GIF submissions dropped for a path outside 0..2 (rt_pgs_submit_gif is
     * documented for those three). One line, then a count: a caller looping
     * on a bad path would otherwise log every packet. */
    bool m_gif_path_logged = false;
    uint64_t m_gif_dropped = 0;
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
    /* display.widescreen's presentation half (rt_pgs_set_widescreen_aspect):
     * the aspect the scanout is presented at, or 0 to keep the aspect
     * derived from the CRTC registers. Read fresh by vsync. */
    double m_widescreen_aspect = 0.0;
    /* Once per distinct aspect, so a resize in `window` mode does not log a
     * line a field. */
    double m_widescreen_aspect_logged = -1.0;
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
    /* Once-only: a retained frame still naming a texture that was destroyed
     * under it, so that draw is the white fallback and not the picture the
     * caller asked for. */
    bool m_overlay_missing_tex_logged = false;

};

#endif /* ICORECOMP_GS_PARALLEL_IMPL_H */
