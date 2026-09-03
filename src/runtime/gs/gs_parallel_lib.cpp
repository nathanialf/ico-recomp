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
        m_opts.raster = RT_PGS_RASTER_WINDOW;
        m_opts.deinterlace = RT_PGS_DEINTERLACE_BOB;
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
    /* The output frame the scanout is built at. Not visible in any other
     * line, and it decides whether a window wider or taller than the mode
     * area (ICO's attract movie) is cropped or presented whole, so a log
     * from a user has to say which one ran. */
    logf("paraLLEl-GS: raster %s", rt_pgs_raster_log_text(m_opts.raster));
    /* Same reason as the raster line: an interlaced scanout can reach the
     * window three different ways and none of the other lines say which. */
    logf("paraLLEl-GS: deinterlace %s (display.deinterlace)",
         rt_pgs_deinterlace_name(m_opts.deinterlace));
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
    /* Same reason, and the same trap: ScanoutResult holds a
     * Vulkan::ImageHandle, and m_held_scanout is declared after m_iface, m_wsi
     * and the headless device, so the member destructor pass reaches it only
     * after this body has already destroyed the device the image belongs to.
     * Reachable whenever the window is closed during the attract movie, which
     * is exactly when a pair is held. */
    m_held_scanout = {};
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

/* DISPFB field layout, public hardware facts (ps2tek, "GS privileged
 * registers"): FBP bits 0-8 in 2048-word units, FBW bits 9-14 in units of 64
 * pixels, PSM bits 15-19, DBX bits 32-42, DBY bits 43-53. Matches
 * ParallelGS::DISPFBBits (gs_registers.hpp:612-621), which is what the
 * renderer reads out of the same word. */
void RtPgs::note_display_register(uint32_t offset, uint64_t old_v, uint64_t new_v) {
    const bool is_dispfb = (offset == 0x0070u || offset == 0x0090u);
    if (is_dispfb) {
        /* The guest pointed the CRTC at a different buffer this field, which
         * is what separates a field the guest skipped from a field whose new
         * picture arrived by a route the display copy snoop cannot see.
         * RtPgs::vsync is the consumer; see the phase derivation there.
         *
         * Two conditions, because a flip that is neither costs a field of
         * high-resolution scanout in gameplay. It has to be the circuit the
         * scanout reads: vsync derives everything from EN2 ? DISPFB2 :
         * DISPFB1, so a DISPFB1 write while circuit 1 is off moves nothing on
         * screen. And it has to move the read window inside VRAM: FBP, DBX or
         * DBY. A write that only reshapes the buffer description (FBW, PSM)
         * is a display environment reprogram, not a new picture, and a
         * gameplay field that dropped its copy and reprogrammed the display
         * would otherwise be handed the counter phase and lose hires. */
        static constexpr uint64_t kReadWindowMask =
            0x1FFull | (0x7FFull << 32) | (0x7FFull << 43);
        const bool is_circuit2 = (offset == 0x0090u);
        const auto& priv = m_iface->get_priv_register_state();
        const bool scanned_circuit = (priv.pmode.EN2 != 0) == is_circuit2;
        if (scanned_circuit && ((old_v ^ new_v) & kReadWindowMask) != 0)
            m_dispfb_flip = true;
        ++m_dispfb_changes;
        /* A change confined to FBP is the per-field buffer select and says
         * nothing new about the geometry. Anything else reshapes the window. */
        const bool geometry_bits = ((old_v ^ new_v) & ~0x1FFull) != 0;
        /* The first handful, then powers of two. The first four cover two
         * full alternations of the pair the attract movie flips between,
         * which is what says whether the two buffers are adjacent field
         * halves of one picture or two whole frames. Geometry changes get a
         * budget of their own on top rather than an exemption from the
         * counter: an exemption is not a bound, and a game that reprograms
         * the display every field would print a line every field. */
        const uint64_t n = m_dispfb_changes;
        bool log_it = false;
        if (m_dispfb_log_left) {
            --m_dispfb_log_left;
            log_it = true;
        } else if ((n & (n - 1)) == 0) {
            log_it = true;
        } else if (geometry_bits && m_dispfb_geom_log_left) {
            --m_dispfb_geom_log_left;
            log_it = true;
        }
        if (log_it) {
            logf("paraLLEl-GS: DISPFB%u change %llu: fbp %u->%u fbw %u->%u psm %u->%u"
                 " dbx %u->%u dby %u->%u (raw 0x%016llx -> 0x%016llx)",
                 offset == 0x0090u ? 2u : 1u, (unsigned long long)n,
                 unsigned(old_v & 0x1FFu), unsigned(new_v & 0x1FFu),
                 unsigned((old_v >> 9) & 0x3Fu), unsigned((new_v >> 9) & 0x3Fu),
                 unsigned((old_v >> 15) & 0x1Fu), unsigned((new_v >> 15) & 0x1Fu),
                 unsigned((old_v >> 32) & 0x7FFu), unsigned((new_v >> 32) & 0x7FFu),
                 unsigned((old_v >> 43) & 0x7FFu), unsigned((new_v >> 43) & 0x7FFu),
                 (unsigned long long)old_v, (unsigned long long)new_v);
        }
        if (geometry_bits) m_display_geom_changed = true;
        return;
    }
    /* PMODE, SMODE1, SMODE2, DISPLAY1, DISPLAY2: every register the scanout
     * rectangle is derived from. */
    if (offset == 0x0000u || offset == 0x0010u || offset == 0x0020u ||
        offset == 0x0080u || offset == 0x00A0u)
        m_display_geom_changed = true;
}

void RtPgs::write_priv(uint32_t offset, uint64_t v) {
    offset &= 0x1FFF;
    auto& priv = m_iface->get_priv_register_state();
    if (offset < 0x1000) {
        uint64_t& slot = priv.qwords_lo[(offset >> 4) * 2];
        if (v != slot) note_display_register(offset, slot, v);
        slot = v;
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
