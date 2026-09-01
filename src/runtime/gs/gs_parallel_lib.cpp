/* gs/gs_parallel_lib.cpp: the inside of libicorecomp-parallel-gs.
 *
 * Implements the C ABI declared in gs_parallel_api.h on top of paraLLEl-GS
 * (third_party/parallel-gs, LGPLv3+) and Granite. This file is ours (MIT),
 * but it compiles INTO the shared library, so it may use the C++ interfaces
 * freely; the executables must not (see gs_parallel_api.h for why).
 *
 * Wiring (mirrors the patterns in the submodule's own consumers,
 * tools/gs_dump_replayer.cpp for headless and tools/gs_stream_replayer.cpp
 * for presentation, without copying code):
 *   rt_pgs_submit_gif  -> GSInterface::gif_transfer(path, data, bytes)
 *   rt_pgs_write_priv  -> PrivRegisterState qword slots (same 16-byte-slot
 *                         layout the dump writer uses; see gs_dumpwriter.cpp)
 *   rt_pgs_vsync       -> GSInterface::flush() + vsync(VSyncInfo{...}), then
 *                         the ScanoutResult is blitted to a window swapchain
 *                         when one exists.
 *
 * Presentation modes, decided once at rt_pgs_create:
 *   windowed  SDL3 window + Granite WSI swapchain (built only when CMake
 *             found the vendored SDL3: ICORECOMP_PGS_SDL). Requires a
 *             display (always assumed present on Windows); closing the
 *             window is reported to the host via RT_PGS_VSYNC_WINDOW_CLOSED.
 *   headless  Vulkan device without a surface (no display, or
 *             ICORECOMP_GS_HEADLESS=1). Renders every field for real; with
 *             ICORECOMP_GS_SCREENSHOT=/path/out.ppm the latest scanout is
 *             written after each field so the file holds the final frame at
 *             exit (path must be outside the repo; scanout pixels are
 *             ROM-derived data).
 *
 * Validation layers: Granite auto-enables VK_LAYER_KHRONOS_validation when
 * present. We keep that off unless ICORECOMP_VVL=1 so normal runs match CI
 * machines without layers installed. The suppression must happen in THIS
 * module: on Windows each CRT keeps its own environment copy, and Granite
 * reads the variable from the library's CRT.
 *
 * Vulkan itself is loaded dynamically by Granite's context
 * (LoadLibraryA("vulkan-1.dll") / dlopen("libvulkan.so.1")); nothing here
 * links a Vulkan import library.
 *
 * Failure policy: no usable Vulkan loader/device, or a device missing
 * paraLLEl-GS's required features, is fatal through host->fatal with the
 * reason logged (loud failure beats silent wrongness).
 */
#include "gs_parallel_api.h"

#include "gs_pgs_context.h"
#include "gs_readback.h"

#include "context.hpp"
#include "device.hpp"
#include "gs_dump_parser.hpp"
#include "gs_interface.hpp"

#ifdef ICORECOMP_PGS_SDL
#include "wsi.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

extern "C" const char* icorecomp_parallel_gs_shim_version(void) {
    return "icorecomp-parallel-gs shim 2 (C ABI, see gs_parallel_api.h)";
}

namespace {

bool env_is_1(const char* name) {
    const char* v = std::getenv(name);
    return v && std::strcmp(v, "1") == 0;
}

/* Scanout aspect ratio.
 *
 * paraLLEl-GS never reports a display aspect directly. It reports the mode
 * area (mode_width x mode_height) as "reference for the target output aspect
 * ratio" and the part of it the CRTC actually scanned out (internal_width x
 * internal_height); see ScanoutResult in gs_renderer.hpp. Every NTSC and PAL
 * mode area the renderer models is the active area of an analog TV, which is
 * displayed 4:3. PS2 pixels are not square, so the scanout image's own
 * width:height is not the ratio to present at:
 *
 *   display aspect = 4/3 * (internal_w / mode_w) / (internal_h / mode_h)
 *
 * Measured for ICO (US), from the GS_DISPLAY2/GS_SMODE2 values the game
 * programs: NTSC, INT=1, FFMD=1, DISPLAY2 DW+1=2560 with MAGH+1=5 (512
 * pixels) and DH+1=448. FFMD with INT forces a deinterlaced scanout, so
 * gs_renderer.cpp does not double mode_height and the mode lands at 512x224
 * after adapt_to_internal_horizontal_resolution rescales mode_width by
 * clock_divider/(MAGH+1). internal then equals mode, the fraction terms
 * cancel, and the target is a plain 4:3. Presenting at 512:224 (2.29:1),
 * which is what this file used to do, stretched the picture 1.71x
 * horizontally and letterboxed the result inside the window.
 *
 * The image's own size is no better a source: force_deinterlace runs
 * fastmad_deinterlace, so result.image comes back 512x448 here while
 * internal/mode stay 512x224. Vertical sampling doubled, screen area did not.
 *
 * double_strike (240p) needs no correction here. The note in gs_renderer.cpp
 * about doubling the height for aspect purposes applies when the aspect is
 * derived from raw pixel counts; in the mode-fraction form a 240p picture
 * already covers the whole mode height, so internal_h / mode_h is 1 either
 * way.
 *
 * The fraction terms are written out but cannot currently move: the merged
 * scanout path sets internal equal to mode (gs_renderer.cpp, "result.internal_
 * width = mode_width"), and the one path where they differ is the
 * raw_circuit_scanout early return, which RtPgs::vsync never asks for. So for
 * every option set this shim passes today the answer is exactly 4:3. The terms
 * stay because they are what makes that a derivation rather than a constant
 * someone has to re-derive if raw_circuit_scanout or high_resolution_scanout
 * is ever enabled.
 *
 * Returns 0 when the renderer reported no usable mode. */
constexpr bool kScanoutOverscan = false;
constexpr double kModeDisplayAspect = 4.0 / 3.0;

double scanout_display_aspect(const ParallelGS::ScanoutResult& s) {
    /* Two mode families this constant does NOT describe, neither reachable
     * from here:
     *
     *  - Overscan. Those mode areas are 712x240 (NTSC) and 712x288 (PAL)
     *    against 640x224 / 640x256 active areas. kScanoutOverscan is the
     *    single place info.overscan is set, and this assert is the gate.
     *  - LC_HDTV (SMODE1 CMOD progressive + HDTV clock): gs_renderer.cpp
     *    reports 1920x540 and 1280x720 mode areas, which are 16:9, not 4:3.
     *    ScanoutResult carries no CMOD/LC field, so this function cannot tell
     *    them apart and would silently squeeze such a picture by 25%. It never
     *    sees one: rt_gs_program_crt (hw/gspriv.cpp) is the only writer of
     *    SMODE1 and calls rt_fatal on any SetGsCrt mode that is not NTSC or
     *    PAL. Anyone lifting that fatal has to derive the aspect here first. */
    static_assert(!kScanoutOverscan, "kModeDisplayAspect assumes the non-overscan mode area");
    if (!s.mode_width || !s.mode_height || !s.internal_width || !s.internal_height) return 0.0;
    return kModeDisplayAspect
         * (double(s.internal_width) / double(s.mode_width))
         * (double(s.mode_height) / double(s.internal_height));
}

#ifdef ICORECOMP_PGS_SDL

constexpr Vulkan::ContextCreationFlags kContextFlags =
    Vulkan::CONTEXT_CREATION_ENABLE_PUSH_DESCRIPTOR_BIT |
    Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_HEAP_BIT |
    Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;

#endif /* ICORECOMP_PGS_SDL */

} // namespace

/* The opaque instance behind RtPgs*. Method bodies moved intact from the
 * pre-C-ABI gs_parallel.cpp ParallelBackend; behavior changes are limited to
 * host-callback logging and reporting window closure instead of exiting. */
struct RtPgs {
    explicit RtPgs(const RtPgsHost& host);
    ~RtPgs();

    void logf(const char* fmt, ...);
    [[noreturn]] void fatalf(const char* fmt, ...);

    void submit_gif(int path, const uint8_t* data, uint32_t qwords);
    void write_priv(uint32_t offset, uint64_t v);
    uint64_t read_priv(uint32_t offset);
    uint32_t vsync(unsigned field);
    void report_stats();

private:
    void init_headless();

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

        void poll_input() override {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                    case SDL_EVENT_QUIT:
                        m_alive = false;
                        break;
                    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                    case SDL_EVENT_WINDOW_RESIZED:
                        resize = true;
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
#endif /* ICORECOMP_PGS_SDL */

    RtPgsHost m_host;
    std::unique_ptr<Vulkan::Context> m_headless_context;
    std::unique_ptr<Vulkan::Device> m_headless_device;
#ifdef ICORECOMP_PGS_SDL
    std::unique_ptr<SdlWsiPlatform> m_platform;
    std::unique_ptr<Vulkan::WSI> m_wsi;
#endif
    Vulkan::Device* m_device = nullptr; /* whichever of the above is live */
    std::unique_ptr<ParallelGS::GSInterface> m_iface;
    const char* m_screenshot_path = nullptr;
    uint64_t m_vsyncs = 0;
    bool m_transfer_since_vsync = false;
    bool m_wsi_active = false;
    bool m_window_closed = false;
    /* Last (internal w, internal h, mode w, mode h, deinterlaced) whose aspect
     * was logged, so a mode change is visible in the log without spamming
     * every field. */
    uint32_t m_aspect_log_geom[5] = {};
};

void RtPgs::logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_host.log("gs", buf);
}

void RtPgs::fatalf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_host.fatal("gs", buf);
    /* host->fatal must not return; guarantee noreturn regardless. */
    std::abort();
}

RtPgs::RtPgs(const RtPgsHost& host) : m_host(host) {
    if (!env_is_1("ICORECOMP_VVL")) {
        /* Granite auto-enables the validation layer when installed; keep
         * runs reproducible unless explicitly requested. */
#ifdef _WIN32
        if (!std::getenv("GRANITE_VULKAN_NO_VALIDATION")) {
            _putenv_s("GRANITE_VULKAN_NO_VALIDATION", "1");
        }
#else
        setenv("GRANITE_VULKAN_NO_VALIDATION", "1", 0);
#endif
    } else {
        logf("paraLLEl-GS: ICORECOMP_VVL=1, Vulkan validation enabled if the layer is installed");
    }

    if (!Vulkan::Context::init_loader(nullptr)) {
        fatalf("paraLLEl-GS: Vulkan loader initialization failed (no vulkan-1.dll/libvulkan or no ICD)");
    }

    m_screenshot_path = std::getenv("ICORECOMP_GS_SCREENSHOT");

#ifdef ICORECOMP_PGS_SDL
#ifdef _WIN32
    const bool display_available = true;
#else
    const bool display_available =
        (std::getenv("DISPLAY") && *std::getenv("DISPLAY")) ||
        (std::getenv("WAYLAND_DISPLAY") && *std::getenv("WAYLAND_DISPLAY"));
#endif
    if (display_available && !env_is_1("ICORECOMP_GS_HEADLESS")) {
        init_windowed();
    }
#endif
    if (!m_device) init_headless();

    m_iface = std::make_unique<ParallelGS::GSInterface>();
    ParallelGS::GSOptions opts = {};
    if (!m_iface->init(m_device, opts)) {
        fatalf("paraLLEl-GS: GSInterface::init failed; the Vulkan device does not meet its "
               "requirements (see the log above for the missing features)");
    }

    const auto& gpu_props = m_device->get_gpu_properties();
    logf("paraLLEl-GS: live backend up on \"%s\" (Vulkan %u.%u, %s)",
         gpu_props.deviceName,
         VK_API_VERSION_MAJOR(gpu_props.apiVersion), VK_API_VERSION_MINOR(gpu_props.apiVersion),
         m_wsi_active ? "windowed" : "headless");
}

RtPgs::~RtPgs() {
    if (m_device) m_device->wait_idle();
    m_iface.reset();
#ifdef ICORECOMP_PGS_SDL
    m_wsi.reset();
    m_platform.reset();
#endif
    m_headless_device.reset();
    m_headless_context.reset();
}

void RtPgs::submit_gif(int path, const uint8_t* data, uint32_t qwords) {
    if (path < 0 || path > 2 || qwords == 0) return;
    m_iface->gif_transfer(uint32_t(path), data, size_t(qwords) * 16);
    m_transfer_since_vsync = true;
}

void RtPgs::write_priv(uint32_t offset, uint64_t v) {
    offset &= 0x1FFF;
    auto& priv = m_iface->get_priv_register_state();
    if (offset < 0x1000) {
        priv.qwords_lo[(offset >> 4) * 2] = v;
    } else {
        priv.qwords_hi[((offset - 0x1000) >> 4) * 2] = v;
    }
}

uint64_t RtPgs::read_priv(uint32_t offset) {
    offset &= 0x1FFF;
    const auto& priv = m_iface->get_priv_register_state();
    if (offset < 0x1000) return priv.qwords_lo[(offset >> 4) * 2];
    return priv.qwords_hi[((offset - 0x1000) >> 4) * 2];
}

uint32_t RtPgs::vsync(unsigned field) {
    ++m_vsyncs;
    const bool presented = m_transfer_since_vsync;
    m_transfer_since_vsync = false;

    m_iface->flush();

    ParallelGS::VSyncInfo info = {};
    info.phase = field & 1;
    info.force_progressive = true;
    info.anti_blur = true;
    info.adapt_to_internal_horizontal_resolution = true;
    /* Paired with kModeDisplayAspect: that constant is the aspect of the
     * non-overscan mode area only. Flipping this to true without deriving a
     * new constant distorts geometry. */
    info.overscan = kScanoutOverscan;
    /* Both consumers of the scanout image here (swapchain blit, screenshot
     * readback) want a transfer source. */
    info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dst_stage = VK_PIPELINE_STAGE_2_BLIT_BIT;
    info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;

    ParallelGS::ScanoutResult scanout = m_iface->vsync(info);

    /* Logged here rather than in present() so headless runs report it too,
     * and once per geometry change so a mode switch is visible without
     * spamming every field. */
    const double aspect = scanout_display_aspect(scanout);
    /* A non-null image always carries non-zero mode and internal dimensions,
     * so an all-zero m_aspect_log_geom already means "nothing logged yet". */
    const uint32_t geom[5] = {
        scanout.internal_width, scanout.internal_height,
        scanout.mode_width, scanout.mode_height,
        scanout.interlaced ? 1u : 0u,
    };
    if (scanout.image && std::memcmp(geom, m_aspect_log_geom, sizeof(geom)) != 0) {
        std::memcpy(m_aspect_log_geom, geom, sizeof(geom));
        if (aspect > 0.0) {
            /* "deinterlaced", not "interlaced": ScanoutResult::interlaced is
             * assigned should_deinterlace, so it describes what happened to
             * this result, not what the source mode was. */
            logf("paraLLEl-GS: scanout internal %ux%u, mode %ux%u, deinterlaced=%s"
                 " -> display aspect %.4f",
                 scanout.internal_width, scanout.internal_height,
                 scanout.mode_width, scanout.mode_height,
                 scanout.interlaced ? "yes" : "no", aspect);
        } else {
            logf("paraLLEl-GS: scanout reported no mode (internal %ux%u, mode %ux%u);"
                 " presenting stretched to the window, which is not the game's aspect",
                 scanout.internal_width, scanout.internal_height,
                 scanout.mode_width, scanout.mode_height);
        }
    }

#ifdef ICORECOMP_PGS_SDL
    if (m_wsi_active) present(scanout, aspect);
#endif
    if (m_screenshot_path && scanout.image) {
        /* Raw scanout pixels, deliberately NOT aspect-corrected: this file is
         * the regression baseline for rendering, so it has to stay a function
         * of the GS output alone and byte-comparable against a gs-replay dump.
         * It is therefore not the shape the game has on screen (512x448 here
         * against a 4:3 display); the display aspect for the same frame is in
         * the "display aspect" log line above. */
        if (!rt_gs_write_scanout_ppm(*m_device, *scanout.image, m_screenshot_path)) {
            logf("paraLLEl-GS: screenshot write to %s failed", m_screenshot_path);
            m_screenshot_path = nullptr; /* do not spam every field */
        }
    }

    uint32_t flags = presented ? RT_PGS_VSYNC_PRESENTED : 0u;
    if (m_window_closed) flags |= RT_PGS_VSYNC_WINDOW_CLOSED;
    return flags;
}

void RtPgs::report_stats() {
    logf("paraLLEl-GS: %llu vsyncs rendered (%s)",
         (unsigned long long)m_vsyncs, m_wsi_active ? "windowed" : "headless");
}

void RtPgs::init_headless() {
    RtGsContextResult ctx = rt_gs_make_pgs_context();
    if (!ctx.context) {
        fatalf("paraLLEl-GS: no usable Vulkan device (instance/device creation failed)");
    }
    if (ctx.descriptor_buffer_disabled) {
        logf("paraLLEl-GS: descriptor-buffer path disabled (CPU device or "
             "ICORECOMP_GS_NO_DESCBUF=1; see gs_pgs_context.h)");
    }
    m_headless_context = std::move(ctx.context);
    m_headless_device = std::make_unique<Vulkan::Device>();
    m_headless_device->set_context(*m_headless_context);
    m_headless_device->init_frame_contexts(4);
    m_device = m_headless_device.get();
    logf("paraLLEl-GS: headless Vulkan device (no display or ICORECOMP_GS_HEADLESS=1); "
         "set ICORECOMP_GS_SCREENSHOT=/path/out.ppm to capture the scanout");
}

#ifdef ICORECOMP_PGS_SDL

void RtPgs::init_windowed() {
    auto platform = std::make_unique<SdlWsiPlatform>(*this);
    /* 640x480: the 4:3 this backend presents at (scanout_display_aspect), so
     * the window opens with no letterbox. Not the scanout's pixel dimensions:
     * ICO scans out 512x224 in the mode domain, which is not its shape on a
     * TV. */
    if (!platform->init(640, 480)) return;

    auto wsi = std::make_unique<Vulkan::WSI>();
    wsi->set_platform(platform.get());
    wsi->set_backbuffer_format(Vulkan::BackbufferFormat::UNORM);

    /* Present pacing. Granite defaults to FIFO, which blocks each present
     * until the display has scanned out the previous one. The guest clock
     * here is advanced by guest execution, not by the presenter, so a
     * blocking present does not pace the game, it only stalls the thread
     * that is trying to run it: miss a 60 Hz deadline once and the
     * swapchain settles at 30, and the game runs at half speed while still
     * looking smooth. MAILBOX presents without blocking and without
     * tearing. ICORECOMP_GS_PRESENT=vsync restores the old behavior,
     * =tear forces IMMEDIATE. */
    {
        const char* pm = std::getenv("ICORECOMP_GS_PRESENT");
        Vulkan::PresentMode mode = Vulkan::PresentMode::UnlockedNoTearing;
        const char* what = "mailbox (non-blocking, no tearing)";
        if (pm && (std::strcmp(pm, "vsync") == 0 || std::strcmp(pm, "fifo") == 0)) {
            mode = Vulkan::PresentMode::SyncToVBlank;
            what = "FIFO (blocks on the display refresh)";
        } else if (pm && (std::strcmp(pm, "tear") == 0 || std::strcmp(pm, "immediate") == 0)) {
            mode = Vulkan::PresentMode::UnlockedForceTearing;
            what = "immediate (may tear)";
        }
        wsi->set_present_mode(mode);
        logf("paraLLEl-GS: present mode %s (ICORECOMP_GS_PRESENT=vsync|mailbox|tear)", what);
    }
    /* WSI owns context creation here, so the CPU-device auto-fallback in
     * gs_pgs_context.h does not apply; a windowed run on a software device
     * needs ICORECOMP_GS_NO_DESCBUF=1 by hand. */
    Vulkan::ContextCreationFlags flags = kContextFlags;
    if (std::getenv("ICORECOMP_GS_NO_DESCBUF")) {
        flags &= ~Vulkan::CONTEXT_CREATION_ENABLE_DESCRIPTOR_BUFFER_BIT;
    }
    if (!wsi->init_context_from_platform(1, {}, flags)) {
        logf("paraLLEl-GS: WSI context init failed, falling back to headless");
        return;
    }
    if (!wsi->init_device() || !wsi->init_surface_swapchain()) {
        logf("paraLLEl-GS: WSI device/swapchain init failed, falling back to headless");
        return;
    }
    m_platform = std::move(platform);
    m_wsi = std::move(wsi);
    m_device = &m_wsi->get_device();
    m_wsi_active = true;
}

void RtPgs::present(const ParallelGS::ScanoutResult& scanout, double aspect) {
    present_frame(scanout, aspect);
    /* Runs on every path out of present_frame, including its early returns.
     * A window closed while the swapchain was unusable still has to reach the
     * host: RT_PGS_VSYNC_WINDOW_CLOSED is the only signal gs_parallel.cpp
     * exits on, so missing it leaves the process running with no window and
     * no way to quit. */
    if (!m_platform->alive(*m_wsi)) m_window_closed = true;
}

void RtPgs::present_frame(const ParallelGS::ScanoutResult& scanout, double aspect) {
    if (!m_platform->presentable()) {
        /* begin_frame() would park the EE thread here; see presentable().
         * Nothing else pumps SDL when the frame is skipped, so do it here or
         * the restore and close events are never seen. */
        m_platform->poll_input();
        return;
    }

    if (!m_wsi->begin_frame()) {
        logf("paraLLEl-GS: WSI begin_frame failed");
        return;
    }

    auto& device = m_wsi->get_device();
    auto cmd = device.request_command_buffer();
    auto& backbuffer = device.get_swapchain_view().get_image();

    cmd->swapchain_touch_in_stages(VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT);
    cmd->image_barrier(backbuffer,
                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, 0,
                       VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkClearValue clear = {};
    cmd->clear_image(backbuffer, clear);

    if (scanout.image) {
        /* Letterbox: fit the scanout's display aspect inside the window.
         * scanout_display_aspect() explains why that is not the image's own
         * width:height. */
        const int bb_w = int(backbuffer.get_width());
        const int bb_h = int(backbuffer.get_height());
        int dst_w = bb_w, dst_h = bb_h;
        /* aspect <= 0 means the renderer reported no mode, which vsync() has
         * already logged. Fill the window rather than invent a ratio; this is
         * what the pre-aspect code did whenever the mode was zero. */
        if (aspect > 0.0) {
            if (double(bb_w) > double(bb_h) * aspect) {
                dst_w = int(std::lround(double(bb_h) * aspect));
            } else {
                dst_h = int(std::lround(double(bb_w) / aspect));
            }
        }
        const int x0 = (bb_w - dst_w) / 2;
        const int y0 = (bb_h - dst_h) / 2;
        cmd->blit_image(backbuffer, *scanout.image,
                        { x0, y0, 0 }, { dst_w, dst_h, 1 },
                        { 0, 0, 0 },
                        { int(scanout.image->get_width()), int(scanout.image->get_height()), 1 },
                        0, 0, 0, 0, 1, VK_FILTER_LINEAR);
    }

    cmd->image_barrier(backbuffer,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_NONE, 0);
    device.submit(cmd);

    if (!m_wsi->end_frame()) {
        logf("paraLLEl-GS: WSI end_frame failed");
    }
}

#endif /* ICORECOMP_PGS_SDL */

/* ---- C ABI ---------------------------------------------------------------- */

extern "C" {

RtPgs* rt_pgs_create(const RtPgsHost* host) {
    return new RtPgs(*host);
}

void rt_pgs_destroy(RtPgs* pgs) {
    delete pgs;
}

void rt_pgs_submit_gif(RtPgs* pgs, int path, const uint8_t* data, uint32_t qwords) {
    pgs->submit_gif(path, data, qwords);
}

void rt_pgs_write_priv(RtPgs* pgs, uint32_t offset, uint64_t v) {
    pgs->write_priv(offset, v);
}

uint64_t rt_pgs_read_priv(RtPgs* pgs, uint32_t offset) {
    return pgs->read_priv(offset);
}

uint32_t rt_pgs_vsync(RtPgs* pgs, unsigned field) {
    return pgs->vsync(field);
}

void rt_pgs_report_stats(RtPgs* pgs) {
    pgs->report_stats();
}

/* Body moved intact from the pre-C-ABI gs_replay_main.cpp so the replay
 * executable needs no Granite/paraLLEl-GS classes. stderr output keeps the
 * "gs-replay:" prefix that tooling greps for. */
int rt_pgs_replay(const char* dump_path, const char* screenshot_path, int verbose) {
    if (!Vulkan::Context::init_loader(nullptr)) {
        std::fprintf(stderr, "gs-replay: Vulkan loader initialization failed\n");
        return 1;
    }

    RtGsContextResult ctx = rt_gs_make_pgs_context();
    if (!ctx.context) {
        std::fprintf(stderr, "gs-replay: no usable Vulkan device\n");
        return 1;
    }
    if (ctx.descriptor_buffer_disabled) {
        std::fprintf(stderr, "gs-replay: descriptor-buffer path disabled (see gs_pgs_context.h)\n");
    }

    Vulkan::Device device;
    device.set_context(*ctx.context);
    device.init_frame_contexts(4);
    std::fprintf(stderr, "gs-replay: device \"%s\"\n", device.get_gpu_properties().deviceName);

    ParallelGS::GSInterface iface;
    ParallelGS::GSOptions opts = {};
    if (!iface.init(&device, opts)) {
        std::fprintf(stderr, "gs-replay: GSInterface::init failed; device lacks required features (see log)\n");
        return 1;
    }

    ParallelGS::GSDumpParser parser;
    if (!parser.open_raw(dump_path, 4 * 1024 * 1024, &iface)) {
        std::fprintf(stderr, "gs-replay: could not open %s\n", dump_path);
        return 1;
    }

    /* iterate_until_vsync returns at each vsync that had GIF traffic since
     * the previous return, and false at end of stream. The parser consumes
     * trailing traffic-free vsyncs before reporting eof, so "vsync groups"
     * below counts frames with actual transfers, not raw vsync packets. */
    unsigned groups = 0;
    ParallelGS::ScanoutResult last = {};
    while (parser.iterate_until_vsync(false, false)) {
        ++groups;
        ParallelGS::ScanoutResult res = parser.consume_vsync_result();
        if (verbose) {
            std::fprintf(stderr,
                         "gs-replay: vsync group %u: image=%s internal=%ux%u mode=%ux%u\n",
                         groups, res.image ? "yes" : "no",
                         res.internal_width, res.internal_height,
                         res.mode_width, res.mode_height);
        }
        if (res.image) last = res;
    }

    iface.flush();
    device.wait_idle();

    if (groups == 0) {
        std::fprintf(stderr, "gs-replay: FAILED: no vsync with GIF traffic found in %s\n", dump_path);
        return 1;
    }

    std::fprintf(stderr, "gs-replay: OK: %u vsync group(s) rendered from %s\n", groups, dump_path);
    if (last.image) {
        std::fprintf(stderr, "gs-replay: final scanout %ux%u (mode %ux%u)\n",
                     last.image->get_width(), last.image->get_height(),
                     last.mode_width, last.mode_height);
    }

    if (screenshot_path) {
        if (!last.image) {
            std::fprintf(stderr, "gs-replay: no scanout image to screenshot\n");
            return 1;
        }
        /* The parser requests READ_ONLY_OPTIMAL as the scanout layout. */
        if (!rt_gs_write_scanout_ppm(device, *last.image, screenshot_path,
                                     VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL)) {
            std::fprintf(stderr, "gs-replay: screenshot write failed\n");
            return 1;
        }
        std::fprintf(stderr, "gs-replay: wrote %s\n", screenshot_path);
    }
    return 0;
}

} /* extern "C" */
