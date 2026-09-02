/* gs/gs_parallel_lib.cpp: the inside of libicorecomp-parallel-gs.
 *
 * Implements the C ABI declared in gs_parallel_api.h on top of paraLLEl-GS
 * (third_party/parallel-gs, LGPLv3+) and Granite. This file is ours (MIT),
 * but it compiles INTO the shared library, so it may use the C++ interfaces
 * freely; the executables must not (see gs_parallel_api.h for why).
 *
 * This file holds construction and teardown, the log/fatal helpers, and the
 * GIF and PRIV submission paths. The rest of the shim is split by unit into
 * gs_parallel_scanout.cpp, gs_parallel_present.cpp, gs_parallel_overlay.cpp
 * and gs_parallel_abi.cpp; the RtPgs type they share is gs_parallel_impl.h.
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
#include "gs_parallel_impl.h"

#include "context.hpp"
#include "gs_interface.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

bool env_is_1(const char* name) {
    const char* v = std::getenv(name);
    return v && std::strcmp(v, "1") == 0;
}

} // namespace

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
    case 4: gs_opts.super_sampling = ParallelGS::SuperSampling::X4; break;
    case 8: gs_opts.super_sampling = ParallelGS::SuperSampling::X8; break;
    case 16: gs_opts.super_sampling = ParallelGS::SuperSampling::X16; break;
    default:
        /* The host (gs_parallel.cpp) validates render_scale against
         * settings.json's allowed set before it ever reaches here, so
         * anything else is a programming error, not user input. */
        fatalf("paraLLEl-GS: render_scale %u is not one of 1/4/8/16", m_opts.render_scale);
    }
    /* Lets rt_pgs_set_render_scale retune the rate in flight without a
     * reinit, which is how a display.render_scale change from the settings
     * menu reaches the backend. */
    gs_opts.dynamic_super_sampling = true;
    /* Off at 1x by definition, and GSInterface forces it off for X1 anyway
     * (gs_interface.cpp:70-78). See set_render_scale for what it buys. */
    gs_opts.super_sampled_textures = m_opts.render_scale >= 4;
    if (!m_iface->init(m_device, gs_opts)) {
        fatalf("paraLLEl-GS: GSInterface::init failed; the Vulkan device does not meet its "
               "requirements (see the log above for the missing features)");
    }

    const auto& gpu_props = m_device->get_gpu_properties();
    logf("paraLLEl-GS: live backend up on \"%s\" (Vulkan %u.%u, %s)",
         gpu_props.deviceName,
         VK_API_VERSION_MAJOR(gpu_props.apiVersion), VK_API_VERSION_MINOR(gpu_props.apiVersion),
         m_wsi_active ? "windowed" : "headless");
    /* The scale is otherwise nearly invisible in the log: super-sampling
     * resolves back to the game's own resolution at scanout unless
     * high-resolution scanout is on, so a 4x run and a 1x run would look
     * alike in every other line. GSInterface clamps rates above the GPU's
     * supported maximum (always at least 4x) without reporting it; the
     * renderer that knows the cap is private to GSInterface, so the
     * requested rate is what is logged here, not the effective one. What
     * the renderer did with the high-resolution request is on the
     * `scanout internal` line's hires= field, per field geometry. */
    logf("paraLLEl-GS: super-sampling %ux (display.render_scale), super-sampled textures %s,"
         " high-resolution scanout %s",
         m_opts.render_scale,
         gs_opts.super_sampled_textures ? "on" : "off",
         m_opts.render_scale >= 4 ? "requested" : "off");
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
    snoop_display_copy_phase(data, qwords);
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

void RtPgs::report_stats() {
    logf("paraLLEl-GS: %llu vsyncs rendered (%s)",
         (unsigned long long)m_vsyncs, m_wsi_active ? "windowed" : "headless");
}
