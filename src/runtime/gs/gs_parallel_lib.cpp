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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" const char* icorecomp_parallel_gs_shim_version(void) {
    return "icorecomp-parallel-gs shim 3 (C ABI, see gs_parallel_api.h)";
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

/* Shared by init_windowed (the have_opts branch) and rt_pgs_set_present_mode:
 * RT_PGS_PRESENT_* -> the Vulkan::PresentMode Granite's WSI wants, plus the
 * log wording used at both call sites. */
Vulkan::PresentMode present_mode_from_rt(uint32_t rt_mode, const char** what) {
    switch (rt_mode) {
    case RT_PGS_PRESENT_FIFO:
        *what = "FIFO (blocks on the display refresh)";
        return Vulkan::PresentMode::SyncToVBlank;
    case RT_PGS_PRESENT_IMMEDIATE:
        *what = "immediate (may tear)";
        return Vulkan::PresentMode::UnlockedForceTearing;
    default: /* RT_PGS_PRESENT_MAILBOX */
        *what = "mailbox (non-blocking, no tearing)";
        return Vulkan::PresentMode::UnlockedNoTearing;
    }
}

const char* present_mode_name(Vulkan::PresentMode mode) {
    switch (mode) {
    case Vulkan::PresentMode::SyncToVBlank: return "FIFO";
    case Vulkan::PresentMode::UnlockedMaybeTear: return "mailbox/immediate (driver choice)";
    case Vulkan::PresentMode::UnlockedForceTearing: return "immediate";
    case Vulkan::PresentMode::UnlockedNoTearing: return "mailbox";
    }
    return "?";
}

/* Overlay pass SPIR-V: kOverlayVertSpv / kOverlayFragSpv, generated by
 * tools/gen_overlay_spirv.sh from gs/overlay.vert and gs/overlay.frag (ours,
 * MIT, not ROM-derived) because GRANITE_SHIPPING builds carry no runtime
 * shader compiler. Precedent: paraLLEl-GS's own shaders are precompiled the
 * same way via slangmosh (gs/shaders/slangmosh.hpp). */
#include "overlay_shaders.inc"

/* mvp for one overlay draw command: an orthographic projection of the
 * backbuffer, with geometry authored in RtPgsOverlayFrame::surface_width/
 * height pixel space scaled to the backbuffer's actual size, and the
 * command's translate plus (when RT_PGS_OVERLAY_TRANSFORM is set) its
 * transform applied first, both still in surface pixel space. Column-major,
 * column vectors, matching overlay.vert's `mat4 mvp` push constant and
 * RtPgsOverlayCmd::transform's documented layout.
 *
 * No y-flip: Vulkan's default clip space already puts NDC (-1,-1) at the
 * same top-left corner pixel (0,0) is authored at. */
void build_overlay_mvp(float out[16], const RtPgsOverlayCmd& cmd,
                       double bb_w, double bb_h, double surface_w, double surface_h) {
    float m[16];
    if (cmd.flags & RT_PGS_OVERLAY_TRANSFORM) {
        std::memcpy(m, cmd.transform, sizeof(m));
    } else {
        std::memset(m, 0, sizeof(m));
        m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f; m[15] = 1.0f;
    }
    /* Translate: applied after the transform, still in surface pixel space
     * (column-major storage: the translation lives in column 3). */
    m[12] += cmd.translate_x;
    m[13] += cmd.translate_y;

    /* Fold "scale surface space into backbuffer space" and "project
     * backbuffer pixels to NDC" into one factor per axis: ndc = pixel /
     * backbuffer_size * 2 - 1, and pixel = surface_pixel * (backbuffer /
     * surface). The two scales multiply out algebraically, so this is
     * exactly equivalent to scaling into backbuffer space and then
     * projecting, without needing an intermediate matrix. */
    const double sx = surface_w > 0.0 ? bb_w / surface_w : 1.0;
    const double sy = surface_h > 0.0 ? bb_h / surface_h : 1.0;
    for (int c = 0; c < 4; ++c) {
        out[c * 4 + 0] = float(double(m[c * 4 + 0]) * sx * (2.0 / bb_w));
        out[c * 4 + 1] = float(double(m[c * 4 + 1]) * sy * (2.0 / bb_h));
        out[c * 4 + 2] = m[c * 4 + 2];
        out[c * 4 + 3] = m[c * 4 + 3];
    }
    /* -1 bias applies only to the translation column (column 3, the only
     * one whose row-3 element is 1 rather than 0; see the derivation this
     * loop implements). */
    out[12] -= 1.0f;
    out[13] -= 1.0f;
}

#endif /* ICORECOMP_PGS_SDL */

} // namespace

/* The opaque instance behind RtPgs*. Method bodies moved intact from the
 * pre-C-ABI gs_parallel.cpp ParallelBackend; behavior changes are limited to
 * host-callback logging and reporting window closure instead of exiting. */
struct RtPgs {
    RtPgs(const RtPgsHost& host, const RtPgsCreateOptions* opts);
    ~RtPgs();

    void logf(const char* fmt, ...);
    [[noreturn]] void fatalf(const char* fmt, ...);

    void submit_gif(int path, const uint8_t* data, uint32_t qwords);
    void write_priv(uint32_t offset, uint64_t v);
    uint64_t read_priv(uint32_t offset);
    uint32_t vsync(unsigned field);
    void report_stats();

    /* Window control / event pump inversion (shim 3); see gs_parallel_api.h. */
    void* window_handle();
    void notify_quit();
    void notify_resize();
    void surface_size(uint32_t* width, uint32_t* height);
    void set_present_mode(uint32_t mode);
    void set_presentation(uint32_t fit, uint32_t filter);
    void set_render_scale(uint32_t factor, uint32_t hires_scanout);

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
    std::unique_ptr<ParallelGS::GSInterface> m_iface;
    const char* m_screenshot_path = nullptr;
    uint64_t m_vsyncs = 0;
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
    /* Last (internal w, internal h, mode w, mode h, deinterlaced) whose aspect
     * was logged, so a mode change is visible in the log without spamming
     * every field. */
    uint32_t m_aspect_log_geom[5] = {};

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

RtPgs::RtPgs(const RtPgsHost& host, const RtPgsCreateOptions* opts) : m_host(host) {
    m_have_opts = opts != nullptr;
    if (m_have_opts) {
        m_opts = *opts;
    } else {
        /* Pre-settings defaults, documented on rt_pgs_create: present mode
         * comes from ICORECOMP_GS_PRESENT below, in init_windowed, exactly
         * as it did before this struct existed. */
        m_opts.present_mode = RT_PGS_PRESENT_MAILBOX;
        m_opts.fit = RT_PGS_FIT_LETTERBOX;
        m_opts.filter = RT_PGS_FILTER_LINEAR;
        m_opts.render_scale = 1;
        m_opts.hires_scanout = 0;
    }
    if (m_opts.hires_scanout && m_opts.render_scale < 4) {
        /* Loud, once: upstream documents high_resolution_scanout as
         * requiring at least 4x super-sampling. Silently engaging it below
         * that (or silently ignoring the request) would both be worse than
         * saying so. */
        logf("paraLLEl-GS: high_resolution_scanout needs render_scale >= 4; staying off");
    }

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
    ParallelGS::GSOptions gs_opts = {};
    switch (m_opts.render_scale) {
    case 1: gs_opts.super_sampling = ParallelGS::SuperSampling::X1; break;
    case 2: gs_opts.super_sampling = ParallelGS::SuperSampling::X2; break;
    case 4: gs_opts.super_sampling = ParallelGS::SuperSampling::X4; break;
    case 8: gs_opts.super_sampling = ParallelGS::SuperSampling::X8; break;
    case 16: gs_opts.super_sampling = ParallelGS::SuperSampling::X16; break;
    default:
        /* The host (gs_parallel.cpp) validates render_scale against
         * settings.json's allowed set before it ever reaches here, so
         * anything else is a programming error, not user input. */
        fatalf("paraLLEl-GS: render_scale %u is not one of 1/2/4/8/16", m_opts.render_scale);
    }
    /* Lets rt_pgs_set_render_scale retune this in flight later without a
     * reinit (milestone 6); harmless to set now even before that ABI call
     * exists. */
    gs_opts.dynamic_super_sampling = true;
    if (!m_iface->init(m_device, gs_opts)) {
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
    /* The overlay's images are declared after the device members and would
     * be destroyed after them by the member order alone, but m_wsi.reset()
     * / m_headless_device.reset() below destroy the device first, and an
     * ImageHandle released against a dead device is a leak report from
     * Granite's allocator followed by a fault at exit. Release them here,
     * after the idle wait, while the device is still up. The program is
     * owned by the device's cache; only the pointer is dropped. */
    m_overlay_textures.clear();
    m_overlay_white.reset();
    m_overlay_program = nullptr;
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
    /* Below 4x this is already logged and left off at construction; not
     * repeated per field. */
    info.high_resolution_scanout = m_opts.hires_scanout != 0 && m_opts.render_scale >= 4;
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
         * the "display aspect" log line above.
         *
         * The overlay (draw_overlay) never lands here: it is drawn straight
         * to the swapchain backbuffer in present_frame/present_ui_windowed,
         * not to scanout.image, so a screenshot never contains it. */
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

/* ---- overlay rendering (milestone 4) --------------------------------------
 *
 * texture_create/destroy and set_frame are plain Vulkan::Device operations
 * (headless or windowed, m_device points at whichever is live) and reuse
 * the m_in_frame fatal guard: texture_create allocates and submits GPU work
 * (Device::create_image with initial data stages and uploads through a
 * command buffer internally), so calling any of the three from inside
 * pump_events -- which runs during the live backend's own begin_frame --
 * would race that in-flight frame's command buffer. draw_overlay and
 * present_ui_windowed (the actual draw) are windowed-only and live with
 * present_frame further down.
 */

void RtPgs::ensure_overlay_white() {
    if (m_overlay_white) return;
    const uint8_t white[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    Vulkan::ImageInitialData initial = {};
    initial.data = white;
    auto info = Vulkan::ImageCreateInfo::immutable_2d_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    m_overlay_white = m_device->create_image(info, &initial);
    if (!m_overlay_white) {
        fatalf("paraLLEl-GS: failed to create the 1x1 white overlay fallback image");
    }
}

uint32_t RtPgs::overlay_texture_create(const uint8_t* rgba8, uint32_t width, uint32_t height) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_overlay_texture_create called while a frame is in flight;"
               " overlay textures must be created between frames");
    }
    if (!rgba8 || !width || !height) {
        logf("paraLLEl-GS: rt_pgs_overlay_texture_create: invalid arguments (rgba8=%p, %ux%u)",
             (const void*)rgba8, width, height);
        return 0;
    }
    ensure_overlay_white();

    Vulkan::ImageInitialData initial = {};
    initial.data = rgba8;
    auto info = Vulkan::ImageCreateInfo::immutable_2d_image(width, height, VK_FORMAT_R8G8B8A8_UNORM);
    Vulkan::ImageHandle img = m_device->create_image(info, &initial);
    if (!img) {
        logf("paraLLEl-GS: rt_pgs_overlay_texture_create failed (%ux%u)", width, height);
        return 0;
    }
    const uint32_t id = m_overlay_next_texture++;
    m_overlay_textures.emplace(id, std::move(img));
    return id;
}

void RtPgs::overlay_texture_destroy(uint32_t texture) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_overlay_texture_destroy called while a frame is in flight;"
               " overlay textures must be destroyed between frames");
    }
    if (!texture) return;
    /* Erasing an id still referenced by the retained overlay frame is the
     * caller's bug (the ABI doc says so); the library does not scan
     * m_overlay_cmds for it, draw_overlay would just fail the lookup. */
    if (m_overlay_textures.erase(texture) == 0) {
        logf("paraLLEl-GS: rt_pgs_overlay_texture_destroy: unknown texture id %u", texture);
    }
}

void RtPgs::overlay_set_frame(const RtPgsOverlayFrame* frame) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_overlay_set_frame called while a frame is in flight;"
               " overlay frames must be set between frames");
    }
    if (!frame || frame->cmd_count == 0) {
        m_overlay_vertices.clear();
        m_overlay_indices.clear();
        m_overlay_cmds.clear();
        m_overlay_surface_width = 0;
        m_overlay_surface_height = 0;
        return;
    }
    if ((frame->vertex_count && !frame->vertices) ||
        (frame->index_count && !frame->indices) ||
        !frame->cmds) {
        logf("paraLLEl-GS: rt_pgs_overlay_set_frame rejected: null array pointer with nonzero count");
        return;
    }

    /* Validate whole, before touching retained state: a malformed frame
     * keeps the previous one drawing rather than risk out-of-bounds
     * geometry (loud failure beats silent wrongness). */
    for (uint32_t i = 0; i < frame->cmd_count; ++i) {
        const RtPgsOverlayCmd& c = frame->cmds[i];
        if (c.texture != 0 && m_overlay_textures.find(c.texture) == m_overlay_textures.end()) {
            logf("paraLLEl-GS: rt_pgs_overlay_set_frame rejected: cmd %u references unknown texture %u",
                 i, c.texture);
            return;
        }
        const uint64_t index_end = uint64_t(c.index_offset) + c.index_count;
        if (index_end > frame->index_count) {
            logf("paraLLEl-GS: rt_pgs_overlay_set_frame rejected: cmd %u index range [%u, %llu) exceeds %u indices",
                 i, c.index_offset, (unsigned long long)index_end, frame->index_count);
            return;
        }
        for (uint32_t k = c.index_offset; k < c.index_offset + c.index_count; ++k) {
            const int64_t v = int64_t(frame->indices[k]) + c.vertex_offset;
            if (v < 0 || uint64_t(v) >= frame->vertex_count) {
                logf("paraLLEl-GS: rt_pgs_overlay_set_frame rejected: cmd %u index %u"
                     " (vertex_offset %d) is out of range [0, %u)",
                     i, frame->indices[k], c.vertex_offset, frame->vertex_count);
                return;
            }
        }
    }

    m_overlay_vertices.assign(frame->vertices, frame->vertices + frame->vertex_count);
    m_overlay_indices.assign(frame->indices, frame->indices + frame->index_count);
    m_overlay_cmds.assign(frame->cmds, frame->cmds + frame->cmd_count);
    m_overlay_surface_width = frame->surface_width;
    m_overlay_surface_height = frame->surface_height;
}

uint32_t RtPgs::present_ui() {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_present_ui called while a frame is in flight;"
               " it may not be called reentrantly (e.g. from pump_events)");
    }
#ifdef ICORECOMP_PGS_SDL
    if (m_wsi_active) return present_ui_windowed();
#endif
    if (!m_overlay_ui_headless_logged) {
        m_overlay_ui_headless_logged = true;
        logf("paraLLEl-GS: rt_pgs_present_ui ignored (headless, no window)");
    }
    return 0;
}

/* ---- window control / event pump inversion (shim 3) ----------------------
 *
 * window_handle/notify_quit/notify_resize/surface_size are no-ops (NULL /
 * 0x0 / dropped) when headless, matching the doc comments in
 * gs_parallel_api.h. set_present_mode/set_presentation/set_render_scale are
 * declared reachable "between frames only" by that same header; the
 * m_in_frame check comes first so a caller that violates the contract gets a
 * loud fatal, headless or not.
 */

void* RtPgs::window_handle() {
#ifdef ICORECOMP_PGS_SDL
    if (m_wsi_active && m_platform) return (void*)m_platform->window();
#endif
    return nullptr;
}

void RtPgs::notify_quit() {
#ifdef ICORECOMP_PGS_SDL
    if (m_platform) m_platform->handle_quit();
#endif
}

void RtPgs::notify_resize() {
#ifdef ICORECOMP_PGS_SDL
    if (m_platform) m_platform->handle_resize();
#endif
}

void RtPgs::surface_size(uint32_t* width, uint32_t* height) {
    uint32_t w = 0, h = 0;
#ifdef ICORECOMP_PGS_SDL
    if (m_wsi_active && m_platform) {
        w = m_platform->get_surface_width();
        h = m_platform->get_surface_height();
    }
#endif
    if (width) *width = w;
    if (height) *height = h;
}

void RtPgs::set_present_mode(uint32_t mode) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_set_present_mode called while a frame is in flight;"
               " settings must apply at the field boundary");
    }
#ifdef ICORECOMP_PGS_SDL
    if (!m_wsi_active) {
        logf("paraLLEl-GS: rt_pgs_set_present_mode ignored (headless, no window)");
        return;
    }
    const char* what = "mailbox (non-blocking, no tearing)";
    Vulkan::PresentMode vk_mode = present_mode_from_rt(mode, &what);
    m_opts.present_mode = mode;
    m_wsi->set_present_mode(vk_mode);
    /* set_present_mode only records the request; it takes effect on the
     * swapchain that gets rebuilt at the next begin_frame. Reading
     * get_present_mode() here reports the request just recorded, not
     * necessarily what the driver ends up honoring -- logging requested vs
     * what Granite is about to ask the driver for is still the useful
     * signal for "does this driver even support mailbox". */
    logf("paraLLEl-GS: present mode requested %s (%s, resolved by the host); "
         "driver reports %s once the swapchain rebuilds",
         what, present_mode_name(vk_mode), present_mode_name(m_wsi->get_present_mode()));
#else
    (void)mode;
    logf("paraLLEl-GS: rt_pgs_set_present_mode ignored (built without window support)");
#endif
}

void RtPgs::set_presentation(uint32_t fit, uint32_t filter) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_set_presentation called while a frame is in flight;"
               " settings must apply at the field boundary");
    }
    /* Only stores; present_frame reads m_opts.fit/filter fresh every field,
     * so this takes effect at the next present with no further action. */
    m_opts.fit = fit;
    m_opts.filter = filter;
}

void RtPgs::set_render_scale(uint32_t factor, uint32_t hires_scanout) {
    if (m_in_frame) {
        fatalf("paraLLEl-GS: rt_pgs_set_render_scale called while a frame is in flight;"
               " settings must apply at the field boundary");
    }
    ParallelGS::SuperSampling ss;
    switch (factor) {
    case 1: ss = ParallelGS::SuperSampling::X1; break;
    case 2: ss = ParallelGS::SuperSampling::X2; break;
    case 4: ss = ParallelGS::SuperSampling::X4; break;
    case 8: ss = ParallelGS::SuperSampling::X8; break;
    case 16: ss = ParallelGS::SuperSampling::X16; break;
    default:
        /* The host validates render_scale against settings.json's allowed
         * set before this is ever called, so anything else is a programming
         * error, not user input (same reasoning as the constructor). */
        fatalf("paraLLEl-GS: rt_pgs_set_render_scale factor %u is not one of 1/2/4/8/16", factor);
    }
    /* ordered_super_sampling / super_sampled_textures kept at the GSOptions
     * defaults (gs_interface.hpp): true / false. Nothing here changes them. */
    m_iface->set_super_sampling_rate(ss, true, false);
    m_opts.render_scale = factor;
    if (hires_scanout && factor < 4) {
        /* Same message as the create path (RtPgs::RtPgs); stays off rather
         * than silently engaging below the documented minimum. */
        logf("paraLLEl-GS: high_resolution_scanout needs render_scale >= 4; staying off");
        m_opts.hires_scanout = 0;
    } else {
        m_opts.hires_scanout = hires_scanout;
    }
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
     * TV. display.window_width/height (settings.json) override this when
     * set; 0 in either field keeps the 640x480 default (see
     * RtPgsCreateOptions in gs_parallel_api.h). */
    const unsigned window_w = (m_have_opts && m_opts.window_width) ? m_opts.window_width : 640;
    const unsigned window_h = (m_have_opts && m_opts.window_height) ? m_opts.window_height : 480;
    if (!platform->init(window_w, window_h)) return;

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
        Vulkan::PresentMode mode = Vulkan::PresentMode::UnlockedNoTearing;
        const char* what = "mailbox (non-blocking, no tearing)";
        if (m_have_opts) {
            /* The host already resolved settings.json vs ICORECOMP_GS_PRESENT
             * (gs_parallel.cpp); this reads only the result. */
            mode = present_mode_from_rt(m_opts.present_mode, &what);
            logf("paraLLEl-GS: present mode %s (display.present, resolved by the host)", what);
        } else {
            /* opts == NULL: the pre-settings default path documented on
             * rt_pgs_create. ICORECOMP_GS_PRESENT is read here, and only
             * here, in this one case. */
            const char* pm = std::getenv("ICORECOMP_GS_PRESENT");
            if (pm && (std::strcmp(pm, "vsync") == 0 || std::strcmp(pm, "fifo") == 0)) {
                mode = Vulkan::PresentMode::SyncToVBlank;
                what = "FIFO (blocks on the display refresh)";
            } else if (pm && (std::strcmp(pm, "tear") == 0 || std::strcmp(pm, "immediate") == 0)) {
                mode = Vulkan::PresentMode::UnlockedForceTearing;
                what = "immediate (may tear)";
            }
            logf("paraLLEl-GS: present mode %s (ICORECOMP_GS_PRESENT=vsync|mailbox|tear)", what);
        }
        wsi->set_present_mode(mode);
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

    /* Set before the call, not after it returns: Granite's WSI::begin_frame
     * (wsi.cpp) calls platform->poll_input() -- and so pump_events -- itself,
     * after acquiring the swapchain image but before begin_frame() returns
     * ("Poll after acquire as well for optimal latency"). The guard has to
     * be armed across that reentrant call, which is exactly why
     * pump_events may only queue events and call notify_quit/notify_resize;
     * anything else it calls lands here fatal. */
    m_in_frame = true;
    if (!m_wsi->begin_frame()) {
        m_in_frame = false;
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
        /* Presentation of the already-rendered scanout only: fit and filter
         * decide how it is scaled and sampled into the window backbuffer.
         * Nothing below this point can change what the game rendered. */
        const int bb_w = int(backbuffer.get_width());
        const int bb_h = int(backbuffer.get_height());
        int dst_w = bb_w, dst_h = bb_h;
        uint32_t fit = m_opts.fit;

        if (fit == RT_PGS_FIT_STRETCH) {
            dst_w = bb_w;
            dst_h = bb_h;
        } else if (fit == RT_PGS_FIT_INTEGER) {
            /* Largest integer n whose n * (the scanout image's own pixel
             * height) copy fits the backbuffer in BOTH dimensions; width
             * follows from the SAME display aspect the letterbox path below
             * derives from, so a tall narrow window must shrink n rather
             * than let the derived width spill past the backbuffer edge
             * (a negative blit offset is not a valid Vulkan region). */
            const int scanout_h = int(scanout.image->get_height());
            int n = scanout_h > 0 ? bb_h / scanout_h : 0;
            if (aspect > 0.0) {
                while (n >= 1 && std::lround(double(n * scanout_h) * aspect) > bb_w) --n;
            }
            if (n >= 1 && aspect > 0.0) {
                dst_h = n * scanout_h;
                dst_w = int(std::lround(double(dst_h) * aspect));
            } else {
                static bool warned_no_integer_fit = false;
                if (!warned_no_integer_fit) {
                    warned_no_integer_fit = true;
                    logf("paraLLEl-GS: display.fit=integer has no room for a 1x copy"
                         " (window %dx%d, scanout height %d); falling back to letterbox",
                         bb_w, bb_h, scanout_h);
                }
                fit = RT_PGS_FIT_LETTERBOX;
            }
        }
        if (fit == RT_PGS_FIT_LETTERBOX) {
            /* Letterbox: fit the scanout's display aspect inside the window.
             * scanout_display_aspect() explains why that is not the image's
             * own width:height. aspect <= 0 means the renderer reported no
             * mode, which vsync() has already logged; fill the window rather
             * than invent a ratio, as the pre-aspect code did whenever the
             * mode was zero. */
            dst_w = bb_w;
            dst_h = bb_h;
            if (aspect > 0.0) {
                if (double(bb_w) > double(bb_h) * aspect) {
                    dst_w = int(std::lround(double(bb_h) * aspect));
                } else {
                    dst_h = int(std::lround(double(bb_w) / aspect));
                }
            }
        }

        const int x0 = (bb_w - dst_w) / 2;
        const int y0 = (bb_h - dst_h) / 2;
        const VkFilter filter = m_opts.filter == RT_PGS_FILTER_NEAREST
            ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
        cmd->blit_image(backbuffer, *scanout.image,
                        { x0, y0, 0 }, { dst_w, dst_h, 1 },
                        { 0, 0, 0 },
                        { int(scanout.image->get_width()), int(scanout.image->get_height()), 1 },
                        0, 0, 0, 0, 1, filter);
    }

    if (!m_overlay_cmds.empty()) {
        /* Overlay pass: draw the retained frame on top of what was just
         * blitted, instead of presenting straight from TRANSFER_DST. */
        cmd->image_barrier(backbuffer,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        Vulkan::RenderPassInfo rp = device.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
        /* get_swapchain_render_pass defaults to clear_attachments = ~0u
         * (clear-all); left as-is that would erase the scanout blit just
         * written above. MANDATORY: load what's there, don't clear it. */
        rp.clear_attachments = 0;
        rp.load_attachments = 1u << 0;
        rp.store_attachments = 1u << 0;
        cmd->begin_render_pass(rp);
        draw_overlay(*cmd);
        cmd->end_render_pass();

        cmd->image_barrier(backbuffer,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_NONE, 0);
    } else {
        cmd->image_barrier(backbuffer,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                           VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                           VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_NONE, 0);
    }
    device.submit(cmd);

    if (!m_wsi->end_frame()) {
        logf("paraLLEl-GS: WSI end_frame failed");
    }
    m_in_frame = false;
}

/* Draws m_overlay_cmds into the render pass cmd already has open (either
 * present_frame's on top of the scanout blit, or present_ui_windowed's on
 * top of its own clear). Callers own the barriers and the render pass;
 * this only sets pipeline state, uploads the retained geometry, and issues
 * the draws. Caller must not call this with an empty overlay frame if it
 * matters (drawing zero cmds is harmless, but present_frame skips the whole
 * pass instead so an idle overlay costs nothing). */
void RtPgs::draw_overlay(Vulkan::CommandBuffer& cmd) {
    ensure_overlay_white();
    if (!m_overlay_program) {
        /* This build has GRANITE_VULKAN_SPIRV_CROSS off (see gs_pgs_context.h
         * neighbors and CMakeCache.txt: no runtime reflection library is
         * linked in, the same reason paraLLEl-GS ships slangmosh's
         * precomputed reflection_bank for ITS shaders), so request_program
         * needs the resource layout by hand instead of reflecting
         * overlay_shaders.inc at run time; an unset input_mask would build a
         * pipeline with none of these vertex attributes wired in at all
         * (CommandBuffer::for_each_bit(layout.attribute_mask, ...)), and an
         * unset push_constant_size a pipeline layout with no room for mvp.
         * Hand-computed from overlay.vert/overlay.frag's declared locations,
         * mirroring exactly what Shader::reflect_resource_layout would have
         * produced (see shader.cpp) for these two small, fixed shaders. */
        Vulkan::ResourceLayout vertex_layout = {};
        vertex_layout.input_mask = 0x7;       /* locations 0,1,2: pos, uv, color */
        vertex_layout.output_mask = 0x3;      /* locations 0,1: v_uv, v_color */
        vertex_layout.push_constant_size = sizeof(float) * 16; /* mat4 mvp */

        Vulkan::ResourceLayout fragment_layout = {};
        fragment_layout.input_mask = 0x3;     /* locations 0,1: v_uv, v_color */
        fragment_layout.output_mask = 0x1;    /* location 0: out_color */
        fragment_layout.sets[0].sampled_image_mask = 0x1; /* binding 0: u_tex */
        fragment_layout.sets[0].fp_mask = 0x1;
        fragment_layout.sets[0].meta[0].array_size = 1;

        m_overlay_program = m_device->request_program(
            kOverlayVertSpv, sizeof(kOverlayVertSpv),
            kOverlayFragSpv, sizeof(kOverlayFragSpv),
            &vertex_layout, &fragment_layout);
        if (!m_overlay_program) {
            fatalf("paraLLEl-GS: overlay shader program failed to compile"
                   " (overlay_shaders.inc); see the Vulkan validation log above");
        }
    }
    if (m_overlay_cmds.empty()) return;

    auto& backbuffer = m_device->get_swapchain_view().get_image();
    const double bb_w = double(backbuffer.get_width());
    const double bb_h = double(backbuffer.get_height());
    const double surface_w = m_overlay_surface_width ? double(m_overlay_surface_width) : bb_w;
    const double surface_h = m_overlay_surface_height ? double(m_overlay_surface_height) : bb_h;
    const double scale_x = surface_w > 0.0 ? bb_w / surface_w : 1.0;
    const double scale_y = surface_h > 0.0 ? bb_h / surface_h : 1.0;

    cmd.set_program(m_overlay_program);
    cmd.set_depth_test(false, false);
    /* Granite's request_command_buffer starts every command buffer in
     * set_opaque_state: back-face culling with counter-clockwise front
     * faces. Overlay geometry is authored in a y-down pixel space and
     * projected without a flip, so its triangles are clockwise in
     * framebuffer terms and that default culls every one of them (the
     * symptom is a cleared backbuffer with no UI on it). A 2D overlay has
     * no back faces: draw both windings. */
    cmd.set_cull_mode(VK_CULL_MODE_NONE);
    cmd.set_blend_enable(true);
    cmd.set_blend_op(VK_BLEND_OP_ADD);

    cmd.set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(RtPgsOverlayVertex, x));
    cmd.set_vertex_attrib(1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(RtPgsOverlayVertex, u));
    cmd.set_vertex_attrib(2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(RtPgsOverlayVertex, rgba));

    const VkDeviceSize vtx_size = VkDeviceSize(m_overlay_vertices.size()) * sizeof(RtPgsOverlayVertex);
    void* vtx = cmd.allocate_vertex_data(0, vtx_size, sizeof(RtPgsOverlayVertex));
    std::memcpy(vtx, m_overlay_vertices.data(), size_t(vtx_size));

    const VkDeviceSize idx_size = VkDeviceSize(m_overlay_indices.size()) * sizeof(uint32_t);
    void* idx = cmd.allocate_index_data(idx_size, VK_INDEX_TYPE_UINT32);
    std::memcpy(idx, m_overlay_indices.data(), size_t(idx_size));

    for (const RtPgsOverlayCmd& c : m_overlay_cmds) {
        /* Per command, per gs_parallel_api.h: straight alpha multiplies the
         * source by SRC_ALPHA here, premultiplied alpha (what RmlUi hands us)
         * has that factor already folded into the color and takes ONE. Alpha
         * uses ONE/ONE_MINUS_SRC_ALPHA either way so overlapping translucent
         * draws accumulate coverage correctly. */
        const bool premultiplied = (c.flags & RT_PGS_OVERLAY_PREMULTIPLIED) != 0;
        cmd.set_blend_factors(premultiplied ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA,
                              VK_BLEND_FACTOR_ONE,
                              VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);

        float mvp[16];
        build_overlay_mvp(mvp, c, bb_w, bb_h, surface_w, surface_h);
        cmd.push_constants(mvp, 0, sizeof(mvp));

        const Vulkan::ImageView* view = &m_overlay_white->get_view();
        if (c.texture != 0) {
            auto it = m_overlay_textures.find(c.texture);
            if (it != m_overlay_textures.end()) view = &it->second->get_view();
            /* Else: set_frame already rejected an unknown id whole; a
             * texture destroyed after the frame was set is a caller bug
             * (documented on rt_pgs_overlay_texture_destroy) -- fall back
             * to white rather than reading a dangling ImageView. */
        }
        cmd.set_texture(0, 0, *view, Vulkan::StockSampler::LinearClamp);

        VkRect2D scissor;
        if (c.flags & RT_PGS_OVERLAY_SCISSOR) {
            int32_t x0 = int32_t(std::lround(double(c.scissor_x) * scale_x));
            int32_t y0 = int32_t(std::lround(double(c.scissor_y) * scale_y));
            int32_t x1 = int32_t(std::lround(double(c.scissor_x + c.scissor_w) * scale_x));
            int32_t y1 = int32_t(std::lround(double(c.scissor_y + c.scissor_h) * scale_y));
            x0 = std::max(x0, 0);
            y0 = std::max(y0, 0);
            x1 = std::min(x1, int32_t(bb_w));
            y1 = std::min(y1, int32_t(bb_h));
            scissor.offset = { x0, y0 };
            scissor.extent = { uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0)) };
        } else {
            scissor.offset = { 0, 0 };
            scissor.extent = { uint32_t(bb_w), uint32_t(bb_h) };
        }
        cmd.set_scissor(scissor);

        cmd.draw_indexed(c.index_count, 1, c.index_offset, c.vertex_offset, 0);
    }
}

/* rt_pgs_present_ui's windowed implementation: same begin_frame/end_frame
 * skeleton as present_frame, minus the scanout blit (there is no guest
 * scanout in this path) -- clear to a near-black background, draw the
 * overlay, present. Mirrors present()'s window-closed bookkeeping inline
 * since there is no scanout-producing caller to do it afterwards. */
uint32_t RtPgs::present_ui_windowed() {
    uint32_t flags = 0;
    if (!m_platform->presentable()) {
        /* Same reasoning as present_frame: begin_frame() would park the
         * calling thread while minimized, so poll here instead. */
        m_platform->poll_input();
    } else {
        m_in_frame = true;
        if (!m_wsi->begin_frame()) {
            m_in_frame = false;
            logf("paraLLEl-GS: WSI begin_frame failed (rt_pgs_present_ui)");
        } else {
            auto& device = m_wsi->get_device();
            auto cmd = device.request_command_buffer();
            auto& backbuffer = device.get_swapchain_view().get_image();

            cmd->swapchain_touch_in_stages(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
            cmd->image_barrier(backbuffer,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

            Vulkan::RenderPassInfo rp = device.get_swapchain_render_pass(Vulkan::SwapchainRenderPass::ColorOnly);
            rp.clear_attachments = 1u << 0;
            /* Opaque near-black, not full black: distinguishes "launcher up,
             * no scanout" from a dead/hung window at a glance. */
            rp.clear_color[0].float32[0] = 0.06f;
            rp.clear_color[0].float32[1] = 0.06f;
            rp.clear_color[0].float32[2] = 0.06f;
            rp.clear_color[0].float32[3] = 1.0f;
            rp.load_attachments = 0;
            rp.store_attachments = 1u << 0;
            cmd->begin_render_pass(rp);
            draw_overlay(*cmd);
            cmd->end_render_pass();

            cmd->image_barrier(backbuffer,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_NONE, 0);
            device.submit(cmd);

            if (!m_wsi->end_frame()) {
                logf("paraLLEl-GS: WSI end_frame failed (rt_pgs_present_ui)");
            }
            m_in_frame = false;
            flags |= RT_PGS_VSYNC_PRESENTED;
        }
    }

    /* Same window-closed bookkeeping present() does around present_frame,
     * inlined here since rt_pgs_present_ui has no separate wrapper. */
    if (!m_platform->alive(*m_wsi)) m_window_closed = true;
    if (m_window_closed) flags |= RT_PGS_VSYNC_WINDOW_CLOSED;
    return flags;
}

#endif /* ICORECOMP_PGS_SDL */

/* ---- C ABI ---------------------------------------------------------------- */

extern "C" {

RtPgs* rt_pgs_create(const RtPgsHost* host, const RtPgsCreateOptions* opts) {
    return new RtPgs(*host, opts);
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

void* rt_pgs_window_handle(RtPgs* pgs) {
    return pgs->window_handle();
}

void rt_pgs_notify_quit(RtPgs* pgs) {
    pgs->notify_quit();
}

void rt_pgs_notify_resize(RtPgs* pgs) {
    pgs->notify_resize();
}

void rt_pgs_surface_size(RtPgs* pgs, uint32_t* width, uint32_t* height) {
    pgs->surface_size(width, height);
}

void rt_pgs_set_present_mode(RtPgs* pgs, uint32_t mode) {
    pgs->set_present_mode(mode);
}

void rt_pgs_set_presentation(RtPgs* pgs, uint32_t fit, uint32_t filter) {
    pgs->set_presentation(fit, filter);
}

void rt_pgs_set_render_scale(RtPgs* pgs, uint32_t factor, uint32_t hires_scanout) {
    pgs->set_render_scale(factor, hires_scanout);
}

uint32_t rt_pgs_overlay_texture_create(RtPgs* pgs, const uint8_t* rgba8, uint32_t width, uint32_t height) {
    return pgs->overlay_texture_create(rgba8, width, height);
}

void rt_pgs_overlay_texture_destroy(RtPgs* pgs, uint32_t texture) {
    pgs->overlay_texture_destroy(texture);
}

void rt_pgs_overlay_set_frame(RtPgs* pgs, const RtPgsOverlayFrame* frame) {
    pgs->overlay_set_frame(frame);
}

uint32_t rt_pgs_present_ui(RtPgs* pgs) {
    return pgs->present_ui();
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
