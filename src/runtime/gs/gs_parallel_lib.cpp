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
 *   windowed  the host's SDL3 window (RtPgsCreateOptions::host_window) plus
 *             a Granite WSI swapchain on the surface the host makes from it.
 *             Built only where CMake found SDL3 (ICORECOMP_PGS_SDL). Closing
 *             the window is reported to the host via
 *             RT_PGS_VSYNC_WINDOW_CLOSED.
 *   headless  Vulkan device without a surface (the host passed no window).
 *             Renders every field for real; with
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

/* Granite and paraLLEl-GS report their own failures through LOGE/LOGW/LOGI,
 * which without an interface installed end up as fprintf(stderr) from inside
 * this shared library (Granite/util/logging.hpp). The WSI is the reason this
 * matters: end_frame and begin_frame hand the caller a bare bool, and the
 * VkResult behind it is only ever named in Granite's own line
 * (Granite/vulkan/wsi.cpp). Util::set_thread_logging_interface routes those
 * lines into the host log at the level Granite tagged them with, so a driver
 * or swapchain failure sits next to the shim's own line about it whatever
 * the host did with stderr.
 *
 * One instance, never destroyed, holding the owner as an atomic that
 * ~RtPgs clears: the interface pointer is thread local and outlives the
 * instance on any thread that installed it, so a line logged after teardown
 * has to fall back to Granite's own stderr path rather than call into freed
 * memory. Returning false is what asks for that fallback. */
struct PgsGraniteLog final : Util::LoggingInterface {
    std::atomic<RtPgs*> owner{nullptr};

    bool log(const char* tag, const char* fmt, va_list va) override {
        RtPgs* o = owner.load(std::memory_order_acquire);
        if (!o) return false;
        char buf[1024];
        std::vsnprintf(buf, sizeof(buf), fmt, va);
        /* Granite's messages end in a newline and this log adds its own. */
        for (size_t n = std::strlen(buf); n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'); --n) {
            buf[n - 1] = '\0';
        }
        /* The tag is Granite's own "[ERROR]: " / "[WARN]: " / "[INFO]: ",
         * matched loosely so a reworded tag lands on info rather than being
         * dropped. */
        if (std::strstr(tag, "ERROR")) o->errorf("paraLLEl-GS: Granite: %s", buf);
        else if (std::strstr(tag, "WARN")) o->warnf("paraLLEl-GS: Granite: %s", buf);
        else o->logf("paraLLEl-GS: Granite: %s", buf);
        return true;
    }
};

PgsGraniteLog& granite_log() {
    static PgsGraniteLog log;
    return log;
}

} // namespace

void RtPgs::install_granite_log() {
    granite_log().owner.store(this, std::memory_order_release);
    Util::set_thread_logging_interface(&granite_log());
}

void RtPgs::logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_host.log("gs", buf);
}

/* The two levelled twins. The tag is part of the message because the host
 * callback has no level parameter; see RT_PGS_LOG_TAG_WARN in
 * gs_parallel_impl.h for the whole of that arrangement. */
void RtPgs::warnf(const char* fmt, ...) {
    char buf[1024] = RT_PGS_LOG_TAG_WARN;
    const size_t head = sizeof(RT_PGS_LOG_TAG_WARN) - 1;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf + head, sizeof(buf) - head, fmt, ap);
    va_end(ap);
    m_host.log("gs", buf);
}

void RtPgs::errorf(const char* fmt, ...) {
    char buf[1024] = RT_PGS_LOG_TAG_ERROR;
    const size_t head = sizeof(RT_PGS_LOG_TAG_ERROR) - 1;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf + head, sizeof(buf) - head, fmt, ap);
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
    /* The window this constructor is about to create belongs to this thread
     * for the rest of the run: SDL's event queue is per thread on Windows,
     * so every later SDL call has to come from here. on_owner_thread() is
     * the test, and the WSI platform uses it to decide whether it may poll
     * at all. See the threads section of gs_parallel_api.h. */
    m_owner_thread = std::this_thread::get_id();
    /* First, before anything that can fail: device creation and swapchain
     * setup report through Granite's logger, and those are exactly the
     * failures a run has nothing else to say about. */
    install_granite_log();
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
    /* Windowed exactly when the host handed a window in. The library no
     * longer guesses from DISPLAY or WAYLAND_DISPLAY: those were a stand-in
     * for "can SDL_CreateWindow succeed here", and the host now answers that
     * question by having already succeeded or failed
     * (host/window_service.cpp). ICORECOMP_GS_HEADLESS is read on the host
     * side for the same reason. */
    if (m_opts.host_window) {
        if (!m_host.create_vulkan_surface) {
            fatalf("paraLLEl-GS: a host window was passed with no create_vulkan_surface"
                   " callback; a window with no way to make a surface from it cannot be"
                   " presented into (see RtPgsHost in gs_parallel_api.h)");
        }
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
    logf("paraLLEl-GS: deinterlace %s (compiled in; the display.deinterlace key"
         " was retired 2026-09-04)",
         rt_pgs_deinterlace_name(m_opts.deinterlace));
}

RtPgs::~RtPgs() {
    if (m_device) m_device->wait_idle();
    /* Teardown logs through Granite too, and the host callback outlives this
     * instance, so the routing is left in place until the members below are
     * about to go and then dropped by the store at the end of this body. */
    /* Releases any screenshot staging buffer still in flight while the device
     * is up. The fence it waits on has already been signalled by the idle
     * wait above; what matters is that the BufferHandle is dropped here and
     * not by the member destructor pass, which runs after the device below
     * has gone (the same trap m_overlay_textures and m_held_scanout have). */
    drain_screenshots();
    /* Every pipeline this run created is in the device's cache now; store
     * it before the device members below take the VkPipelineCache with
     * them. */
    pipeline_cache_store();
    /* The overlay's images are declared after the device members and would
     * be destroyed after them by the member order alone, but m_wsi.reset()
     * / m_headless_device.reset() below destroy the device first, and an
     * ImageHandle released against a dead device is a leak report from
     * Granite's allocator followed by a fault at exit. Release them here,
     * after the idle wait, while the device is still up. The program is
     * owned by the device's cache; only the pointer is dropped. */
    m_overlay_textures.clear();
    m_overlay_white.reset();
    /* Same reason, and the same trap, for both scanout slots: a
     * ScanoutResult holds a Vulkan::ImageHandle, and m_held_scanout and
     * m_latest_scanout are declared after m_iface, m_wsi and the headless
     * device, so the member destructor pass reaches them only after this
     * body has already destroyed the device the image belongs to.
     * m_held_scanout only holds a pair during the attract movie;
     * m_latest_scanout is written on every windowed field, so it is
     * populated at every normal exit.
     *
     * The full list of members that keep a Vulkan object alive and are
     * declared after the device members in gs_parallel_impl.h, so every one
     * of them has to be released by this body:
     *   m_shot[]            BufferHandle + Fence, dropped by drain_screenshots
     *   m_boot_sample       BufferHandle + Fence, dropped just below
     *   m_held_scanout      ImageHandle inside a ScanoutResult
     *   m_latest_scanout    ImageHandle inside a ScanoutResult
     *   m_overlay_textures  ImageHandle per guest texture
     *   m_overlay_white     ImageHandle
     *   m_overlay_program   raw pointer into the device's cache, only cleared
     * Any future slot that keeps a device object alive belongs in this list
     * too. */
    m_held_scanout = {};
    m_latest_scanout = {};
    /* The boot trace's 16x16 sample is armed on every present for the first
     * kBootTraceFields fields and cleared only by drain_boot_sample at the
     * top of the next present_frame, which a launcher present does not run.
     * So an exit inside the first ten seconds, or one whose last present was
     * a launcher present, leaves a live BufferHandle here. The fence is
     * already signalled by the wait_idle above. */
    m_boot_sample = BootSample{};
    m_latest_aspect = 0.0;
    m_overlay_program = nullptr;
    m_iface.reset();
#ifdef ICORECOMP_PGS_SDL
    m_wsi.reset();
    m_platform.reset();
#endif
    m_headless_device.reset();
    m_headless_context.reset();
    /* Nothing may reach this instance through Granite's thread-local
     * interface from here on; see PgsGraniteLog. */
    granite_log().owner.store(nullptr, std::memory_order_release);
}

void RtPgs::submit_gif(int path, const uint8_t* data, uint32_t qwords) {
    if (path < 0 || path > 2) {
        /* Guest traffic thrown away. rt_pgs_submit_gif is documented for
         * paths 0..2, so this is a caller bug, and a dropped packet that
         * said nothing would show up only as a missing piece of picture.
         * One line, then a count in report_stats: a caller stuck on a bad
         * path submits thousands. */
        ++m_gif_dropped;
        if (!m_gif_path_logged) {
            m_gif_path_logged = true;
            warnf("paraLLEl-GS: rt_pgs_submit_gif path %d is outside 0..2; the transfer of %u"
                  " qword(s) is dropped and further drops are counted, not logged", path, qwords);
        }
        return;
    }
    if (qwords == 0) return;
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
    /* The counters behind the once-only lines. Each of these is a picture
     * the user did not get, so a run that looked wrong has the totals in its
     * log even when the reason came and went in one line hours earlier.
     * Warn, not info, whenever any of them is non-zero: the run did not do
     * what it was asked to. */
    if (m_present_skipped_total || m_begin_frame_failures || m_end_frame_failures ||
        m_no_image_total || m_gif_dropped) {
        warnf("paraLLEl-GS: totals: %llu field(s) not presented, %llu begin_frame failure(s),"
              " %llu end_frame failure(s), %llu field(s) with no scanout image,"
              " %llu GIF transfer(s) dropped",
              (unsigned long long)m_present_skipped_total,
              (unsigned long long)m_begin_frame_failures,
              (unsigned long long)m_end_frame_failures,
              (unsigned long long)m_no_image_total,
              (unsigned long long)m_gif_dropped);
    }
}

void RtPgs::present_timings(RtPgsPresentTimings* out) {
    if (!out) return;
    /* Read and cleared in one step each: the writer is the GS consumer
     * thread and it does not stop for this. */
    out->flush_ns = m_flush_ns.exchange(0, std::memory_order_relaxed);
    out->scanout_ns = m_scanout_ns.exchange(0, std::memory_order_relaxed);
    out->present_ns = m_present_ns.exchange(0, std::memory_order_relaxed);
    out->fields = m_timing_fields.exchange(0, std::memory_order_relaxed);
    out->presents = m_presents.exchange(0, std::memory_order_relaxed);
    out->repeats = m_present_repeats.exchange(0, std::memory_order_relaxed);
}
