/* gs/render/gs_native.cpp: the clean-room GS backend.
 *
 * Ours (MIT). Implements GsBackend (gs/gs_backend.h) on top of the RHI
 * (rhi/rhi.h) with no paraLLEl-GS involvement of any kind. The pieces it is
 * assembled from:
 *
 *   gif_decode.h    GIF packets to GS register writes and the HWREG stream
 *   gs_regs.h       the register file, both contexts and the privileged block
 *   gs_vram.h       4 MiB of local memory and the three transfer directions
 *   gs_swizzle.h    addressing, shared verbatim with the compute shader
 *   gs_crtc.h       the CRTC: mode, circuits, frame, merge, deinterlace
 *   shaders/        scanout.comp, overlay.vert, overlay.frag
 *
 * What each milestone added is in docs/GS_RENDERER.md. Render scale lives
 * here: the sample count picks the fine pass's tile, the super-sampled shadow
 * of local memory (gs_shadow.h) holds the samples between batches, a resolve
 * pass puts the native picture back after every batch, and the scanout reads
 * the shadow directly when the buffer the CRTC displays is one the game drew
 * into at this scale.
 *
 * Where the picture lives. Local memory exists twice: a host copy and a
 * device buffer, and both are written. The transfer engine and the CLUT loads
 * are CPU work over the host copy; the rasteriser writes the device buffer.
 * The two are reconciled in both directions at the batch boundary, which the
 * "---- drawing ----" block below sets out: the words the host dirtied are
 * uploaded before a batch that reads them, and the words the GPU wrote are
 * read back before the host reads them. The scanout compute shader reads the
 * device buffer and writes the frame image. The frame image is persistent,
 * because weave needs the other field's rows to survive.
 *
 * Windowing. The executable owns the window (host/window_service.h): it
 * creates it before this backend exists, with the flags the RHI backend this
 * run resolved to needs, and this constructor reads the native handles out of
 * it into rhi::DeviceDesc. With no window the device is created headless,
 * which is what the replay tool and a run with no display take. The present
 * path runs whenever the device has a swapchain, and the fit and filter
 * semantics are the ones docs/SETTINGS.md already defines.
 */
#include "gs_native.h"

#include "../gs_backend.h"
#include "../gs_parallel_api.h"

#include "gs_clut.h"
#include "gs_crtc.h"
#include "gs_draw.h"
#include "gs_dump_parse.h"
#include "gs_regs.h"
#include "gs_shadow.h"
#include "gs_vram.h"
#include "gif_decode.h"

#include "../../host/window_service.h"
#include "../../rhi/rhi.h"
#include "../../rhi/rhi_shaders.h"
#include "../../runtime.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace gsr;

/* Presentation fit and filter, same values as RT_PGS_FIT_* / RT_PGS_FILTER_*
 * so a setting reaches either backend unchanged. */
struct Presentation {
    uint32_t fit = RT_PGS_FIT_LETTERBOX;
    uint32_t filter = RT_PGS_FILTER_LINEAR;
    uint32_t raster = GSR_RASTER_WINDOW;
    uint32_t deinterlace = GSR_DEINTERLACE_BOB;
    uint32_t render_scale = 1;
};

/* One retained overlay frame, deep copied out of the caller's POD. */
struct OverlayFrame {
    std::vector<RtPgsOverlayVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<RtPgsOverlayCmd> cmds;
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
    bool valid = false;
};

/* The push constants overlay.vert and overlay.frag share. Laid out to match
 * the std430 rules a push constant block uses: the mat4 first at offset 0,
 * then the two vec2s, then the two flags. 88 bytes. */
struct OverlayPush {
    float transform[16];
    float surface_w, surface_h;
    float translate_x, translate_y;
    uint32_t use_transform;
    uint32_t use_texture;
};

/* Every push block this file pushes, against the shader's own field count and
 * against the RHI's budget. The budget half can only be written here, where
 * rhi.h and the three gsr headers are both in scope, and it is the half that
 * matters on hardware: push_constants() fatals on an oversized block at the
 * first field, with no line before it naming which struct grew. */
static_assert(sizeof(OverlayPush) == 88,
              "OverlayPush must stay 88 bytes, in step with shaders/overlay.vert");
static_assert(sizeof(OverlayPush) <= rhi::kPushConstantBytes,
              "the overlay push block is over the RHI's push constant budget");
static_assert(sizeof(gsr::RasterPush) <= rhi::kPushConstantBytes,
              "the fine pass's push block is over the RHI's push constant budget");
static_assert(sizeof(gsr::ScanoutPush) <= rhi::kPushConstantBytes,
              "the scanout push block is over the RHI's push constant budget");
static_assert(sizeof(gsr::ShadowPush) <= rhi::kPushConstantBytes,
              "the shadow push block is over the RHI's push constant budget");

/* The dispatch grid a compute pass may ask for is checked against what the
 * device this run created reports, not against a number written here:
 * rhi::Limits::max_workgroup_count, read at device creation from
 * maxComputeWorkGroupCount on Vulkan and from the fixed D3D12 value. Every
 * group count below is derived from a guest register: the scissor a batch was
 * clipped to, or the CRTC window the game programmed. None of them is bounded
 * by anything on the host side. A dispatch above the limit is not an error a
 * driver has to report: it is undefined, and what it looks like on a Windows
 * machine is the process ending with no line in the log. See
 * dispatch_checked. */

/* The largest scanout image this renderer will build, on each axis. The frame
 * is the CRTC window (gs_crtc.cpp), which is DISPLAY DX + DW+1 over the
 * magnification: DW is twelve bits and DH eleven, so the geometry alone
 * allows a frame several times the largest 2D image either backend can
 * create, and at hires the renderer doubles it again. Refusing the field with
 * the numbers is the loud half; creating the texture is a fatal inside the
 * backend that names a size and not the registers it came from. */
constexpr uint32_t kMaxScanoutAxis = 4096u;

/* Writes an RGBA8 image as the P6 PPM the existing screenshot path writes:
 * raw scanout, alpha dropped, no aspect correction. See gs/gs_readback.h for
 * why the aspect is deliberately absent. */
bool write_ppm(const char* path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    if (!path || !w || !h || rgba.size() < (size_t)w * h * 4) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t)w * h; ++i) std::fwrite(rgba.data() + i * 4, 1, 3, f);
    std::fclose(f);
    return true;
}

using PumpClock = std::chrono::steady_clock;

uint64_t elapsed_ns(PumpClock::time_point since) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        PumpClock::now() - since).count();
}

const char* rhi_backend_name(rhi::Backend b) {
    switch (b) {
    case rhi::Backend::D3D12: return "d3d12";
    case rhi::Backend::Metal: return "metal";
    case rhi::Backend::Vulkan: return "vulkan";
    default: return "auto";
    }
}

/* RT_PGS_PRESENT_* to the RHI's own enum. The three names line up one for
 * one, and the reasoning behind each is in docs/SETTINGS.md section 6;
 * nothing is reinterpreted here. The value arrives from gs/gs_select.cpp,
 * which resolved display.present against ICORECOMP_GS_PRESENT, because this
 * file also compiles into icorecomp-gs-replay, which links no settings
 * layer. */
rhi::PresentMode present_mode_from_rt(uint32_t mode) {
    switch (mode) {
    case RT_PGS_PRESENT_FIFO: return rhi::PresentMode::Fifo;
    case RT_PGS_PRESENT_IMMEDIATE: return rhi::PresentMode::Immediate;
    default: return rhi::PresentMode::Mailbox;
    }
}

/* The window service's sink (host/window_service.h). The native renderer
 * needs one thing from the window and it is the resize: the RHI rebuilds its
 * swapchain from the size it is told. A quit is already recorded by the
 * service itself and read back through window_closed(), and the size cache
 * the service keeps is the same one this backend reads, so neither the quit
 * nor the sample callback has anything left to do here.
 *
 * The callback runs on the main thread from inside the event pump, while the
 * consumer thread may be inside a present, so it may not touch the device: it
 * sets an atomic that the consumer reads at the top of its next present, the
 * same shape the paraLLEl-GS shim's handle_resize/sync_from_host pair has. */
std::atomic<bool> g_resize_pending{false};

void sink_native_resize(void*) { g_resize_pending.store(true, std::memory_order_release); }

class NativeBackend final : public GsBackend, public DrawFlusher {
public:
    NativeBackend(RtNativeRhi which, uint32_t present_mode, bool headless_only) {
        rhi::DeviceDesc dd;
        /* The graphics API's own validation, which is a device creation
         * option and not a log line: on D3D12 the debug layer, on Vulkan
         * VK_LAYER_KHRONOS_validation and the debug messenger. It stays a
         * developer switch because turning it on costs the whole run's
         * performance and because a machine without the layer installed
         * cannot have it at all. */
        /* Compile-time, not a run-time switch: layer validation costs on
         * every call and is a developer build option. docs/GS_RENDERER.md
         * says how to turn it on. */
#ifdef ICORECOMP_RHI_VALIDATION
        dd.validation = true;
#else
        dd.validation = false;
#endif
        /* The backend is a desc field so that a D3D12 or Metal device that
         * fails to start is a fatal naming that backend rather than a quiet
         * fall back onto Vulkan. ICORECOMP_GS_BACKEND resolved which one this
         * run asked for (gs/gs_select.cpp), and the window was created with
         * that backend's flags before this constructor ran. */
        switch (which) {
        case RtNativeRhi::D3D12: dd.backend = rhi::Backend::D3D12; break;
        case RtNativeRhi::Metal: dd.backend = rhi::Backend::Metal; break;
        default: dd.backend = rhi::Backend::Vulkan; break;
        }

        /* ICORECOMP_RHI_SOFTWARE=1 puts the device on the software rasteriser:
         * WARP on D3D12, a CPU implementation such as lavapipe on Vulkan. It
         * is a CI and debugging knob and has no settings.json twin, because a
         * player who reaches it has a picture at a frame a second and no way
         * of knowing why. rhi.h says it is never a fallback, so a machine with
         * no software device is a fatal on both backends rather than a quiet
         * run on hardware. */
        if (const char* sw = std::getenv("ICORECOMP_RHI_SOFTWARE"); sw && *sw && *sw != '0') {
            dd.prefer_software = true;
            rt_log_info("gsr", "ICORECOMP_RHI_SOFTWARE=%s: the device is created on the "
                               "software rasteriser", sw);
        }

        /* The window is the executable's (host/window_service.h). Headless
         * when the caller asked for it (the replay tool) or when this run has
         * no window at all: no video driver, or SDL_CreateWindow failed, both
         * of which the window service has already logged. */
        RtWindowNative native = {};
        const bool windowed = !headless_only && rt_window_exists()
                              && rt_window_native(&native);
        dd.headless = !windowed;
        if (windowed) {
            dd.win32_hwnd = native.win32_hwnd;
            dd.win32_hinstance = native.win32_hinstance;
            dd.x11_display = native.x11_display;
            dd.x11_window = native.x11_window;
            dd.wl_display = native.wl_display;
            dd.wl_surface = native.wl_surface;
            dd.cocoa_window = native.cocoa_window;
            rt_window_surface_size(&dd.surface_width, &dd.surface_height);
            dd.present_mode = present_mode_from_rt(present_mode);
        } else if (!headless_only && rt_window_exists()) {
            /* A window exists but SDL reported no native handle for it, so
             * there is nothing to build a swapchain from. Say which, rather
             * than letting the run look headless for no stated reason. */
            rt_log_warn("gsr", "the window reported no native handle for this platform; the "
                               "native renderer runs headless this run");
        }
#if defined(ICORECOMP_RHI_D3D12)
        if (dd.backend == rhi::Backend::D3D12) {
            m_device = rhi::create_d3d12_device(dd);
        } else
#endif
#if defined(ICORECOMP_RHI_METAL)
        if (dd.backend == rhi::Backend::Metal) {
            m_device = rhi::create_metal_device(dd);
        } else
#endif
        {
            m_device = rhi::create_vulkan_device(dd);
        }
        m_backend_name = rhi_backend_name(dd.backend);
        /* Read once, here, and used wherever this file would otherwise have
         * written a number of its own: dispatch_checked measures every guest
         * derived group count against max_workgroup_count, and the overlay
         * geometry is double buffered against frames_in_flight so a slot is
         * free by the time it comes round again. See rhi::Limits. */
        m_limits = m_device->limits();
        m_overlay_buf.resize(m_limits.frames_in_flight < 2u ? 2u
                                                            : m_limits.frames_in_flight);

        rhi::BufferDesc bd;
        bd.size = GS_VRAM_BYTES;
        bd.kind = rhi::BufferKind::DeviceLocal;
        /* CopySrc as well as CopyDst: from milestone (b) the rasteriser
         * writes this buffer on the device, and a host access to local
         * memory has to read the changed words back. */
        bd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::CopyDst
                 | rhi::BufferUsage::CopySrc;
        bd.debug_name = "gs local memory";
        m_vram = m_device->create_buffer(bd);

        bd.kind = rhi::BufferKind::Upload;
        bd.usage = rhi::BufferUsage::CopySrc;
        bd.debug_name = "gs local memory upload";
        m_vram_staging = m_device->create_buffer(bd);

        const rhi::ShaderBlob scanout = rhi::shader_scanout_comp();
        m_scanout = m_device->create_compute_pipeline(scanout.words, scanout.word_count,
                                                      "gs scanout");

        const rhi::ShaderBlob raster = rhi::shader_raster_comp();
        m_raster = m_device->create_compute_pipeline(raster.words, raster.word_count,
                                                     "gs fine rasteriser");

        const rhi::ShaderBlob shadow = rhi::shader_shadow_comp();
        m_shadow_pipe = m_device->create_compute_pipeline(shadow.words, shadow.word_count,
                                                          "gs shadow seed and resolve");
        m_draw.set_flusher(this);
        /* The CLUT is loaded from the host copy of local memory, on the CPU,
         * at the TEX0 write that asks for it; gs_clut.h says why. */
        m_clut.set_memory(&m_mem);
        m_draw.set_clut(&m_clut);

        /* The whole store is uploaded once so the device buffer starts as a
         * true mirror of a zeroed local memory rather than as whatever the
         * allocation happened to contain. */
        m_mem.mark_all_dirty();
        rt_log_info("gsr", "native GS renderer active on %s (%s), %s backend%s",
                    m_device->device_name(), m_device->api_version(), m_backend_name,
                    m_device->has_swapchain() ? "" : ", headless");
        /* A window was created and the device came back without one. The
         * window is on screen either way, so this is the difference between a
         * black window with a reason and a black window without one. */
        if (windowed && !m_device->has_swapchain()) {
            rt_log_warn("gsr", "the run has a window but the %s device was created headless, "
                               "so the window will stay black. The rhi lines above say which "
                               "handle or surface was missing.", m_backend_name);
        }

        /* The Display tab's two read-only lines, from the device this run
         * actually created. The second one names the backend and the API
         * level rather than a feature list: the RHI states its requirements
         * as a floor a device either meets or is rejected for
         * (docs/GS_RENDERER.md), so there is no partial answer to report the
         * way paraLLEl-GS's feature check has. */
        {
            char renderer[256];
            std::snprintf(renderer, sizeof(renderer), "%s, %s", m_device->device_name(),
                          m_device->api_version());
            char features[256];
            std::snprintf(features, sizeof(features),
                          "native renderer on the %s backend; it has not passed its parity gate "
                          "(docs/GS_RENDERER.md)", m_backend_name);
            rt_window_set_device_info(m_backend_name, renderer, features);
        }

        /* A resize has to reach the swapchain, and it arrives on the main
         * thread while this backend's consumer may be presenting, so the sink
         * only sets a flag (see sink_native_resize above). Registered after
         * the device exists and cleared in the destructor. */
        if (m_device->has_swapchain()) {
            const RtWindowSink sink = { this, nullptr, sink_native_resize, nullptr };
            rt_window_set_sink(&sink);
        }
    }

    ~NativeBackend() override {
        if (!m_device) return;
        rt_window_set_sink(nullptr);
        m_device->wait_idle();
        for (auto& kv : m_overlay_textures) m_device->destroy_texture(kv.second);
        if (m_shot_buffer) m_device->destroy_buffer(m_shot_buffer);
        for (OverlayBuffers& b : m_overlay_buf) {
            if (b.vertices) m_device->destroy_buffer(b.vertices);
            if (b.indices) m_device->destroy_buffer(b.indices);
        }
        if (m_overlay_pipeline) m_device->destroy_graphics_pipeline(m_overlay_pipeline);
        if (m_overlay_pipeline_straight) {
            m_device->destroy_graphics_pipeline(m_overlay_pipeline_straight);
        }
        if (m_frame) m_device->destroy_texture(m_frame);
        if (m_shadow_buffer) m_device->destroy_buffer(m_shadow_buffer);
        if (m_seed_buffer) m_device->destroy_buffer(m_seed_buffer);
        if (m_history_buffer) m_device->destroy_buffer(m_history_buffer);
        if (m_shadow_pipe) m_device->destroy_compute_pipeline(m_shadow_pipe);
        if (m_clut_buffer) m_device->destroy_buffer(m_clut_buffer);
        if (m_prim_buffer) m_device->destroy_buffer(m_prim_buffer);
        if (m_binidx_buffer) m_device->destroy_buffer(m_binidx_buffer);
        if (m_binrng_buffer) m_device->destroy_buffer(m_binrng_buffer);
        if (m_vram_readback) m_device->destroy_buffer(m_vram_readback);
        if (m_raster) m_device->destroy_compute_pipeline(m_raster);
        if (m_scanout) m_device->destroy_compute_pipeline(m_scanout);
        if (m_vram_staging) m_device->destroy_buffer(m_vram_staging);
        if (m_vram) m_device->destroy_buffer(m_vram);
        delete m_device;
    }

    /* ---- GIF ------------------------------------------------------------- */

    void submit_gif(int path, const uint8_t* data, uint32_t qwords) override {
        /* Both arms drop guest traffic, so neither may be silent. A path
         * outside 0..2 is a caller bug and not guest data: the hardware has
         * three GIF paths and nothing else can reach here. Said once per
         * distinct bad value, so a mis-wired caller is named without a line
         * per packet; the run totals are in the stats block. */
        if (path < 0 || path > 2) {
            ++m_gif_bad_path;
            if (m_logged_bad_path != path) {
                m_logged_bad_path = path;
                rt_log_warn("gsr", "submit_gif on GIF path %d: only paths 0, 1 and 2 exist, "
                                   "so %u qwords of guest traffic were dropped", path,
                            qwords);
            }
            return;
        }
        if (qwords == 0) {
            ++m_gif_empty;
            if (!m_logged_empty_gif) {
                m_logged_empty_gif = true;
                rt_log_warn("gsr", "submit_gif on GIF path %d with 0 qwords: there is "
                                   "nothing to decode. Said once", path);
            }
            return;
        }
        m_transfer_since_vsync = true;
        Sink sink(*this);
        gif_decode(m_gif[path], data, qwords, sink);
    }

    void write_priv(uint32_t offset, uint64_t v) override { m_regs.write_priv(offset, v); }
    uint64_t read_priv(uint32_t offset) override { return m_regs.read_priv(offset); }

    /* ---- field ----------------------------------------------------------- */

    /* End of field: build the scanout image and latch it. Nothing reaches a
     * window here. present_pump() shows the latest latched field and repeats
     * it, which is the contract gs_backend.h states, and the bool this
     * returns is "a field's worth of GIF traffic landed", not "something was
     * presented". */
    bool vsync(unsigned field) override {
        const bool latched = m_transfer_since_vsync;
        m_transfer_since_vsync = false;

        /* Anything still being assembled belongs to the field that is
         * ending, so it is drawn before the CRTC reads local memory. */
        gsr_flush_draws();

        const auto t_field = PumpClock::now();
        ScanoutPlan plan = crtc_plan(m_regs, m_present.raster, m_present.deinterlace,
                                     field);
        if (plan.have_picture) {
            check_display_psm(plan);
            plan.push.samples = m_shadow.samples();
            plan.push.hires = choose_hires(plan) ? 1u : 0u;
            if (!scanout_size_ok(plan)) {
                /* The frame the CRTC asked for is not one this renderer can
                 * build. Refusing the field keeps the last picture on screen
                 * with a line saying which registers produced the size;
                 * carrying on hands create_texture a width of zero or a
                 * gigabyte of image, and both are a fatal inside the backend
                 * that names a size and not where it came from. */
                plan.have_picture = false;
            }
        }
        if (plan.have_picture) {
            ensure_frame(scanout_width(plan), scanout_height(plan));
            ensure_history(scanout_width(plan), scanout_height(plan));
            plan.push.hist_valid = m_history_fresh ? 0u : 1u;
            m_history_fresh = false;
        }
        /* display.widescreen: presented at the target aspect instead of the
         * derived one, the same override the paraLLEl-GS shim applies
         * (gs_parallel_scanout.cpp). The value comes from the ring record
         * set_widescreen_aspect carried, not from a direct read of
         * guest/widescreen.cpp: the settings layer writes that atomic on the
         * EE thread, and reading it here would take whatever it held at this
         * instant rather than the value ordered with the fields this vsync
         * applies to (gs_backend.h). 0 is off, which is what the header says
         * it means. */
        m_last_aspect = m_widescreen_aspect > 0.0 ? m_widescreen_aspect : plan.display_aspect;

        host_current_for_upload();
        rhi::CommandList* cmd = m_device->begin_command_list();
        if (plan.have_picture) {
            upload_dirty_vram(cmd);
            record_scanout(cmd, plan);
        }
        const uint64_t value = m_device->submit(cmd);
        /* The scanout image is the product on a headless device and the
         * source of the next present on a windowed one, so the field is not
         * latched until its work has finished. This wait is one of the two
         * places a stalled device stops the run, so the run state carries it
         * for the watchdog and the end-of-run summary to name. */
        rt_run_note_rhi("the scanout submitted, waiting");
        m_device->wait(value);
        rt_run_note_rhi("idle");

        m_have_frame = plan.have_picture;
        if (plan.have_picture) ++m_frame_serial;

        ++m_fields;
        ++m_fields_since_read;
        m_scanout_ns += elapsed_ns(t_field);
        return latched;
    }

    /* Said once when a present is asked for and the device has no window to
     * present into. Two different situations, and they are not equally
     * expected: the replay tool and a run with no display are headless by
     * design and say so at info, while a run that has a window and a device
     * with no swapchain has a window the user is looking at with nothing in
     * it, which is a warn with the reason in it. Neither may be silent: a
     * present path that returns without a word is a black window with no
     * explanation. */
    void no_window_note(const char* who) const {
        if (rt_window_exists()) {
            rt_log_warn("gsr", "%s: this run has a window but the %s device has no "
                               "swapchain, so nothing is drawn into it. The reason is in "
                               "the rhi lines above (no native window handle, a minimised "
                               "window, or a surface the device cannot present to).",
                        who, m_backend_name);
        } else {
            rt_log_info("gsr", "%s with no window: this renderer's device has no swapchain, "
                               "so nothing is presented", who);
        }
    }

    /* acquire_backbuffer returning false is not an error at the RHI (rhi.h:
     * a minimised window, or a swapchain being rebuilt, both take it), but a
     * run that stops acquiring is a black window, and the log has to say
     * which of the two it is looking at. Once at warn on the way in, once at
     * info on the way out with the number of presents that were skipped in
     * between, so an outage has a start, an end and a size. */
    void note_acquire_failed(const char* who) {
        ++m_acquire_skipped;
        ++m_acquire_skipped_run;
        if (m_logged_acquire) return;
        m_logged_acquire = true;
        rt_log_warn("gsr", "%s: the %s device had no backbuffer to acquire, so nothing "
                           "reaches the window (a minimised window, or a swapchain being "
                           "rebuilt). Said once; the skipped presents are counted and "
                           "named when it comes back", who, m_backend_name);
    }

    void note_acquire_ok(const char* who) {
        if (!m_logged_acquire) return;
        m_logged_acquire = false;
        rt_log_info("gsr", "%s: the backbuffer can be acquired again; %" PRIu64 " presents "
                           "were skipped while it could not", who, m_acquire_skipped);
        m_acquire_skipped = 0;
    }

    /* Presents the newest latched field, and repeats it every 1/max_hz
     * seconds while no newer one arrives. max_hz 0 is one present per field
     * and no repeats.
     *
     * With no window (the replay tool, a run with no display) this returns
     * immediately and says so once; every counter it would fill stays zero,
     * which is the honest report rather than a present that did not
     * happen. */
    void present_pump(double max_hz) override {
        if (!m_device->has_swapchain()) {
            if (!m_logged_no_pump_window) {
                m_logged_no_pump_window = true;
                no_window_note("present_pump");
            }
            return;
        }
        sync_window();
        drain_screenshots();
        if (!m_have_frame) {
            /* No field has ever been latched, so there is nothing to show and
             * the window keeps whatever it had. Said once: a run that never
             * gets past this is a black window, and without the line the log
             * says nothing at all about why. */
            if (!m_logged_no_frame) {
                m_logged_no_frame = true;
                rt_log_warn("gsr", "present_pump: no field has produced a picture yet "
                                   "(fields=%" PRIu64 "), so nothing is presented",
                            m_fields);
            }
            rt_run_note_rhi("idle, no latched field");
            return;
        }

        /* The rate gate below is the one quiet return in this function. A
         * present that is only early happened exactly as asked, so it is not
         * news at any level. */
        const bool newer = m_frame_serial != m_presented_serial;
        const auto now = PumpClock::now();
        if (!newer) {
            if (max_hz <= 0.0) return;
            const double since = std::chrono::duration<double>(now - m_last_present).count();
            if (since < 1.0 / max_hz) return;
        }

        rt_run_note_rhi("acquiring the backbuffer");
        rhi::Texture* backbuffer = nullptr;
        if (!m_device->acquire_backbuffer(&backbuffer) || !backbuffer) {
            note_acquire_failed("present_pump");
            return;
        }
        note_acquire_ok("present_pump");

        const auto t_present = PumpClock::now();
        rt_run_note_rhi("recording a command list");
        rhi::CommandList* cmd = m_device->begin_command_list();
        record_present(cmd, backbuffer, m_frame, m_last_aspect);
        const uint64_t submitted = m_device->submit(cmd);
        note_overlay_submit(submitted);
        note_shot_submit(submitted);
        rt_run_note_rhi("presenting");
        m_device->present();
        /* Past this point the picture belongs to the swapchain. That is what
         * the end-of-run summary counts as a present, and the phase is what
         * places a run that dies after it. */
        rt_run_note_present();
        rt_run_phase(RT_PHASE_FIRST_PRESENT);
        rt_run_note_rhi("idle");
        m_present_ns += elapsed_ns(t_present);

        ++m_presents;
        ++m_presents_run;
        if (!newer) {
            ++m_repeats;
            ++m_repeats_run;
        }
        m_presented_serial = m_frame_serial;
        m_last_present = now;
        publish_present_rect();
    }

    /* Takes the window changes the main thread recorded since the last
     * present and applies them here, on the consumer thread, which is the
     * only one that may touch the swapchain. The size comes from the window
     * service's cache, which the event pump refreshed before it set the
     * flag. */
    void sync_window() {
        if (!g_resize_pending.exchange(false, std::memory_order_acquire)) return;
        uint32_t w = 0, h = 0;
        rt_window_surface_size(&w, &h);
        if (w == 0 || h == 0) {
            /* Minimised, or a window with no surface yet: there is no size to
             * rebuild the swapchain against. The flag is put back rather than
             * consumed, because a window is restored to the size it was
             * minimised at and SDL sends no second resize for that: dropping
             * the pending resize here left the swapchain built for the size
             * before the one that was minimised, for the rest of the run.
             * Re-arming costs one atomic store per present while the window
             * is down and rebuilds on the first present after it comes back.
             * Said once, because a window that stays this way is the whole
             * explanation for a run that presents nothing. */
            g_resize_pending.store(true, std::memory_order_release);
            if (!m_logged_zero_size) {
                m_logged_zero_size = true;
                rt_log_warn("gsr", "a resize arrived with the window at %ux%u (minimised, "
                                   "or no surface yet); the swapchain is left as it is and "
                                   "the resize is held until the window has a size again",
                            w, h);
            }
            return;
        }
        if (m_logged_zero_size) {
            m_logged_zero_size = false;
            rt_log_info("gsr", "the window has a size again (%ux%u); the swapchain is "
                               "rebuilt against it", w, h);
        }
        m_device->notify_resize(w, h);
    }

    /* Hands the rectangle the present just measured to the window service,
     * which is where the UI, the mouse pointer and the screenshot cropper
     * read it (host/window_service.h). record_present stored the four
     * numbers; the backbuffer size they were measured against is the
     * device's own surface size. */
    void publish_present_rect() {
        rt_window_publish_present_rect(m_present_x, m_present_y, m_present_w, m_present_h,
                                       (int32_t)m_device->surface_width(),
                                       (int32_t)m_device->surface_height());
    }

    /* The exit condition hw/gspriv.cpp polls at the field boundary. The
     * window service holds the answer, because the window is the
     * executable's and a close reaches it first; this backend has no closure
     * state of its own to add.
     *
     * Said once, the first time it is true. A run that ends here ends
     * without a fatal and without an error, so this line is the only thing
     * that can name the cause, and it has to be there at the shipped default
     * level when the player did not ask for it: warn for anything that is
     * not a deliberate quit, info for one that is. The window service
     * records who asked (host/window_service.h); nothing is decided here. */
    bool window_closed() override {
        const bool closed = rt_window_quit_requested();
        if (closed && !m_logged_closed) {
            m_logged_closed = true;
            if (rt_window_quit_was_user()) {
                rt_log_info("gsr", "the window reports a quit asked for by the player (%s); "
                                   "the run ends at this field boundary",
                            rt_window_quit_source());
            } else {
                rt_log_warn("gsr", "the window reports a quit nobody asked for (%s); the "
                                   "run ends at this field boundary",
                            rt_window_quit_source());
            }
        }
        return closed;
    }

    /* Stored rather than acted on: the rate that governs a present comes in
     * as present_pump's own argument, from whichever thread is the consumer.
     * Keeping it here means the statistics line can say what the run was
     * asked for even when nothing pumped. */
    void set_present_rate(double max_hz) override { m_present_rate = max_hz; }

    void present_timings(RtGsPresentTimings* out) override {
        if (!out) {
            /* The counters are only cleared by a read, so a null read loses
             * a window of timings and the next one covers two. */
            rt_log_warn("gsr", "present_timings with no destination; this window's timings "
                               "are not read and fold into the next one");
            return;
        }
        out->flush_ns = 0; /* nothing in this renderer defers work to a flush */
        out->scanout_ns = m_scanout_ns;
        out->present_ns = m_present_ns;
        out->fields = m_fields_since_read;
        out->presents = m_presents;
        out->repeats = m_repeats;
        m_scanout_ns = 0;
        m_present_ns = 0;
        m_fields_since_read = 0;
        m_presents = 0;
        m_repeats = 0;
    }

    void report_stats() override {
        rt_log_info("gsr", "---- native GS renderer stats ----");
        rt_log_info("gsr", "  fields=%" PRIu64 " register writes=%" PRIu64,
                    m_fields, m_reg_writes);
        const DrawEngine::Stats& d = m_draw.stats();
        rt_log_info("gsr", "  drawing: %" PRIu64 " kicks, %" PRIu64 " primitives in %"
                           PRIu64 " batches over %" PRIu64 " dispatches",
                    d.kicks, d.prims, d.batches, m_dispatches);
        rt_log_info("gsr", "  dropped: %" PRIu64 " zero area, %" PRIu64 " outside the "
                           "scissor, %" PRIu64 " reserved PRIM 7; %" PRIu64 " sub-pixel "
                           "lines drawn as one pixel",
                    d.degenerate, d.offscreen, d.reserved_prim, d.short_lines);
        const ClutCache::Stats& c = m_clut.stats();
        rt_log_info("gsr", "  texture: %" PRIu64 " textured primitives, %" PRIu64
                           " mipmapped, %" PRIu64 " batches broken by the feedback rule",
                    d.textured, d.mipmapped, d.feedback_flushes);
        rt_log_info("gsr", "  CLUT: %" PRIu64 " TEX0 writes, %" PRIu64 " loads, %" PRIu64
                           " skipped by CLD, %" PRIu64 " CSM2, %" PRIu64 " TEXFLUSH",
                    c.writes, c.loads, c.skipped, c.csm2_loads, m_texflushes);
        rt_log_info("gsr", "  AA1: %" PRIu64 " lines and triangles with edge coverage, %"
                           PRIu64 " points and sprites where the bit does nothing",
                    d.aa1, d.aa1_ignored);
        rt_log_info("gsr", "  SCANMSK: %" PRIu64 " primitives assembled under a row mask",
                    d.scanmsk);
        const ShadowPages::Stats& sh = m_shadow.stats();
        rt_log_info("gsr", "  render scale %u: %" PRIu64 " pages seeded, %" PRIu64
                           " dropped by a native write, %" PRIu64 " whole-shadow drops, %"
                           PRIu64 " resolves",
                    m_present.render_scale, sh.seeds, sh.drops, sh.drop_alls, sh.resolves);
        rt_log_info("gsr", "  local memory read back from the device %" PRIu64 " times, %"
                           PRIu64 " runs, %" PRIu64 " blocks, %" PRIu64 " words (%.1f MiB)",
                    m_vram_syncs, m_vram_sync_runs, m_vram_sync_blocks, m_vram_sync_words,
                    double(m_vram_sync_words) * 4.0 / (1024.0 * 1024.0));
        rt_log_info("gsr", "  local memory uploaded %" PRIu64 " times, %" PRIu64 " runs, %"
                           PRIu64 " blocks (%.1f MiB)",
                    m_uploads, m_upload_runs, m_upload_blocks,
                    double(m_upload_blocks) * 256.0 / (1024.0 * 1024.0));
        if (m_mem.both_owners() != 0) {
            /* A block both copies wrote since the last reconciliation. One of
             * the two sets of words is lost, and which one depends on the
             * order the marks arrived in, so this is never quiet. */
            rt_log_warn("gsr", "  %" PRIu64 " blocks were marked written by both the host "
                               "and the device without a reconciliation in between; each "
                               "one is 256 bytes where one side's words were lost. The "
                               "ordering rule is that every batch uploads before it draws, "
                               "so this counting above zero is a fault in this renderer",
                        m_mem.both_owners());
        }
        {
            std::string d;
            char buf[128];
            for (const BlockTarget& t : m_draw_targets) {
                std::snprintf(buf, sizeof(buf), "%s%u(bw %u psm 0x%02x fbmsk 0x%08x, %"
                                                PRIu64 ")",
                              d.empty() ? "" : ", ", t.base_block, t.bw, t.psm, t.mask,
                              t.count);
                d += buf;
            }
            rt_log_info("gsr", "  drawn into base blocks: %s",
                        d.empty() ? "none" : d.c_str());
            std::string x;
            for (const BlockTarget& t : m_xfer_targets) {
                std::snprintf(buf, sizeof(buf), "%s%u(bw %u psm 0x%02x, %" PRIu64 ")",
                              x.empty() ? "" : ", ", t.base_block, t.bw, t.psm, t.count);
                x += buf;
            }
            rt_log_info("gsr", "  transfer destinations, base blocks: %s",
                        x.empty() ? "none" : x.c_str());
        }
        rt_log_info("gsr", "  presents=%" PRIu64 " of which repeats=%" PRIu64
                           " (display.present_rate %.1f Hz)",
                    m_presents_run, m_repeats_run, m_present_rate);
        /* The run totals behind the warn lines that are said once each. A
         * nonzero count here is work the guest asked for that never
         * happened, or a picture the window never got. */
        rt_log_info("gsr", "  not drawn or not presented: %" PRIu64 " GIF packets on a bad "
                           "path, %" PRIu64 " empty GIF packets, %" PRIu64 " draw batches "
                           "covering no tile, %" PRIu64 " presents with no backbuffer",
                    m_gif_bad_path, m_gif_empty, m_empty_batches, m_acquire_skipped_run);
        rt_log_info("gsr", "  transfers: host->local %" PRIu64 " px, local->local %" PRIu64
                           " px, local->host %" PRIu64 " px, stray image qwords %" PRIu64,
                    m_xfer.host_to_local_pixels, m_xfer.local_to_local_pixels,
                    m_xfer.local_to_host_pixels, m_xfer.overrun_qwords);
    }

    /* ---- presentation and overlay ---------------------------------------- */

    void set_presentation(uint32_t fit, uint32_t filter) override {
        m_present.fit = fit;
        m_present.filter = filter;
    }

    void set_present_mode(uint32_t mode) override {
        switch (mode) {
            case RT_PGS_PRESENT_MAILBOX:
                m_device->set_present_mode(rhi::PresentMode::Mailbox);
                break;
            case RT_PGS_PRESENT_IMMEDIATE:
                m_device->set_present_mode(rhi::PresentMode::Immediate);
                break;
            default:
                m_device->set_present_mode(rhi::PresentMode::Fifo);
                break;
        }
    }

    /* display.render_scale, as the number of samples per pixel. The settings
     * layer already restricts the key to 1, 4, 8 and 16; a value that reaches
     * here anyway keeps the compiled-in default of 1 and says so, rather than
     * rounding to the nearest allowed one. */
    void set_render_scale(uint32_t factor) override {
        if (!gs_scale_allowed(factor)) {
            rt_log_warn("gsr", "display.render_scale %u is not one of 1, 4, 8 or 16; this "
                               "run renders at scale 1", factor);
            factor = 1;
        }
        if (factor == m_present.render_scale) return;
        /* Every primitive assembled so far belongs to the old scale, and the
         * shadow it drew into is about to be dropped, so it is drawn and
         * resolved first. */
        gsr_flush_draws();
        m_present.render_scale = factor;
        m_shadow.set_samples(factor);
        if (m_shadow_buffer) {
            m_device->wait_idle();
            m_device->destroy_buffer(m_shadow_buffer);
            m_shadow_buffer = nullptr;
        }
        rt_log_info("gsr", "display.render_scale %u: %u samples per pixel, %llu MiB of "
                           "super-sampled shadow", factor, factor,
                    (unsigned long long)(gs_shadow_bytes(factor) >> 20));
    }

    void set_raster(uint32_t raster) override { m_present.raster = raster; }
    void set_deinterlace(uint32_t deinterlace) override { m_present.deinterlace = deinterlace; }
    /* Stored only, read by vsync at the next present, the same contract
     * set_raster and set_deinterlace keep and the same one
     * RtPgs::set_widescreen_aspect keeps on the other backend. Not validated:
     * the host derived this number and correcting it here would be a
     * divergence from what it asked for. */
    void set_widescreen_aspect(double aspect) override { m_widescreen_aspect = aspect; }

    uint32_t overlay_texture_create(const uint8_t* rgba8, uint32_t width,
                                    uint32_t height) override {
        if (!rgba8 || !width || !height) {
            /* Returning 0 leaves the caller with a texture id that names
             * nothing, and every overlay command using it is then rejected by
             * overlay_set_frame for a reason that reads as unrelated. */
            rt_log_warn("gsr", "overlay texture not created: size %ux%u, pixels %s",
                        width, height, rgba8 ? "given" : "null");
            return 0;
        }
        rhi::TextureDesc td;
        td.width = width;
        td.height = height;
        td.format = rhi::Format::RGBA8Unorm;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopyDst;
        td.debug_name = "overlay texture";
        rhi::Texture* t = m_device->create_texture(td);

        rhi::BufferDesc bd;
        bd.size = (uint64_t)width * height * 4;
        bd.kind = rhi::BufferKind::Upload;
        bd.usage = rhi::BufferUsage::CopySrc;
        bd.debug_name = "overlay texture upload";
        rhi::Buffer* staging = m_device->create_buffer(bd);
        std::memcpy(m_device->map(staging), rgba8, (size_t)bd.size);

        rhi::CommandList* cmd = m_device->begin_command_list();
        cmd->copy_buffer_to_texture(t, staging, 0);
        cmd->texture_barrier(t, rhi::Stage::Copy, rhi::Access::Write,
                             rhi::Stage::Graphics, rhi::Access::Read);
        m_device->wait(m_device->submit(cmd));
        m_device->destroy_buffer(staging);

        const uint32_t id = ++m_next_texture_id;
        m_overlay_textures[id] = t;
        return id;
    }

    void overlay_texture_destroy(uint32_t texture) override {
        auto it = m_overlay_textures.find(texture);
        if (it == m_overlay_textures.end()) {
            /* Every live id came from overlay_texture_create, so an unknown
             * one is a double destroy or a stale id in the UI layer. */
            rt_log_warn("gsr", "overlay_texture_destroy for texture %u, which this renderer "
                               "does not hold", texture);
            return;
        }
        m_device->wait_idle();
        m_device->destroy_texture(it->second);
        m_overlay_textures.erase(it);
    }

    void overlay_set_frame(const RtPgsOverlayFrame* frame) override {
        if (!frame || frame->cmd_count == 0 || frame->index_count == 0
            || frame->vertex_count == 0) {
            /* Not a failure: this is how the UI layer says the overlay is
             * empty, once per field with the menu closed. Traced at debug so
             * a per-field log still shows the overlay being cleared. */
            rt_log_debug("gsr", "overlay cleared (%s)",
                         frame ? "the frame carries no geometry" : "no frame");
            m_overlay = OverlayFrame{};
            return;
        }
        /* Rejected whole with one log rather than clipped, the same rule the
         * paraLLEl-GS overlay path states: a frame with an out-of-range index
         * would read past the geometry. */
        for (uint32_t i = 0; i < frame->index_count; ++i) {
            if (frame->indices[i] >= frame->vertex_count) {
                rt_log_warn("gsr", "overlay frame rejected: index %u of %u is %u, and there "
                                   "are %u vertices", i, frame->index_count,
                            frame->indices[i], frame->vertex_count);
                return;
            }
        }
        for (uint32_t i = 0; i < frame->cmd_count; ++i) {
            const RtPgsOverlayCmd& c = frame->cmds[i];
            if ((uint64_t)c.index_offset + c.index_count > frame->index_count) {
                rt_log_warn("gsr", "overlay frame rejected: command %u covers indices "
                                   "%u..%u of %u", i, c.index_offset,
                            c.index_offset + c.index_count, frame->index_count);
                return;
            }
            if (c.texture != 0 && m_overlay_textures.find(c.texture) == m_overlay_textures.end()) {
                rt_log_warn("gsr", "overlay frame rejected: command %u names texture %u, "
                                   "which does not exist", i, c.texture);
                return;
            }
        }

        OverlayFrame f;
        f.vertices.assign(frame->vertices, frame->vertices + frame->vertex_count);
        f.indices.assign(frame->indices, frame->indices + frame->index_count);
        f.cmds.assign(frame->cmds, frame->cmds + frame->cmd_count);
        f.surface_width = frame->surface_width;
        f.surface_height = frame->surface_height;
        f.valid = true;
        m_overlay = std::move(f);
        m_overlay_uploaded = false;
    }

    uint32_t present_ui() override {
        if (!m_device->has_swapchain()) {
            if (!m_logged_no_ui_window) {
                m_logged_no_ui_window = true;
                no_window_note("present_ui");
            }
            return 0;
        }
        sync_window();
        drain_screenshots();
        rt_run_note_rhi("acquiring the backbuffer");
        rhi::Texture* backbuffer = nullptr;
        if (!m_device->acquire_backbuffer(&backbuffer) || !backbuffer) {
            /* The launcher and the menu present through here, so a silent
             * return is a launcher that never appears with nothing in the log
             * to say why. Same pair of lines as the field path. */
            note_acquire_failed("present_ui");
            return 0;
        }
        note_acquire_ok("present_ui");
        rt_run_note_rhi("recording a command list");
        rhi::CommandList* cmd = m_device->begin_command_list();
        record_present(cmd, backbuffer, nullptr, 0.0);
        const uint64_t submitted = m_device->submit(cmd);
        note_overlay_submit(submitted);
        note_shot_submit(submitted);
        rt_run_note_rhi("presenting");
        m_device->present();
        rt_run_note_present();
        rt_run_phase(RT_PHASE_FIRST_PRESENT);
        rt_run_note_rhi("idle");
        publish_present_rect();
        return RT_PGS_VSYNC_PRESENTED
             | (rt_window_quit_requested() ? RT_PGS_VSYNC_WINDOW_CLOSED : 0u);
    }

    /* Arms one capture of the presented picture. slots is 1 for the picture
     * alone or RT_PGS_SHOT_SLOTS for the pre/post pair; anything else is
     * treated as 1 and logged, which is the contract gs_parallel_api.h
     * states for the other backend. */
    void request_screenshot(uint32_t slots) override {
        if (slots != 1 && slots != RT_PGS_SHOT_SLOTS) {
            rt_log_warn("gsr", "screenshot request for %u slots; 1 and %u are the only "
                               "values, taking 1", slots, (unsigned)RT_PGS_SHOT_SLOTS);
            slots = 1;
        }
        if (!m_device->has_swapchain()) {
            /* No window means no presented picture to copy. The raw scanout
             * readback (write_scanout_ppm) is what exists in this milestone,
             * and the replay tool uses it. */
            /* Warn, not info: the user asked for a capture and did not get
             * one, which is a refusal by the level table in runtime.h. */
            if (!m_logged_screenshot) {
                m_logged_screenshot = true;
                rt_log_warn("gsr", "screenshot request ignored: this renderer milestone has "
                                   "no presented picture to copy, only the raw scanout");
            }
            return;
        }
        /* A new arm supersedes anything still sitting in the slots. Such an
         * image belongs to an earlier arm the host gave up on:
         * host/screenshot.cpp disarms after two seconds while this backend's
         * arm stays live, so a later field can publish into a slot nobody is
         * waiting for. Left there it would be handed to this arm's first take
         * and written under a fresh timestamp, showing a scene from minutes
         * ago. Dropped here, and named, so the discard is visible. The same
         * shape the paraLLEl-GS backend has (gs_parallel_present.cpp). */
        {
            std::lock_guard<std::mutex> lk(m_shot_mu);
            for (uint32_t i = 0; i < RT_PGS_SHOT_SLOTS; ++i) {
                if (m_shot_ready[i].rgba.empty()) continue;
                rt_log_warn("gsr", "screenshot slot %u still held a %ux%u image from an arm "
                                   "the host abandoned; dropped rather than served as this "
                                   "capture", i, m_shot_ready[i].width, m_shot_ready[i].height);
                m_shot_ready[i] = ShotReady{};
            }
        }
        m_shot_slots = slots;
    }

    /* Reads one published slot back. Called on the host's EE thread
     * (host/screenshot.cpp) while the consumer thread may be inside a
     * present, so it touches no RHI object at all: drain_screenshots did the
     * wait, the map and the crop on the consumer thread and left tightly
     * packed RGBA8 rows in m_shot_ready under m_shot_mu, and this is a copy
     * out of that. rhi.h states the single-thread contract the earlier form
     * broke by calling wait() and map() from here.
     *
     * Returns the byte count, or 0 when the slot holds nothing. dst null is
     * the size query. The rows are the presented picture at presented size,
     * cropped to the rectangle it occupied, with the letterbox bars excluded.
     *
     * A GsBackend virtual, so host/screenshot.cpp reads this renderer's
     * slots by the same route it reads the other backend's; the ring passes
     * it straight through rather than recording it, because it is a read of
     * state the present already published. See gs_backend.h. */
    size_t take_screenshot(uint32_t slot, uint32_t* w, uint32_t* h,
                           uint8_t* dst, size_t dst_bytes) override {
        if (slot >= RT_PGS_SHOT_SLOTS) {
            rt_log_warn("gsr", "take_screenshot for slot %u: this renderer has %u slots",
                        slot, (unsigned)RT_PGS_SHOT_SLOTS);
            return 0;
        }
        std::lock_guard<std::mutex> lk(m_shot_mu);
        ShotReady& r = m_shot_ready[slot];
        /* The one quiet 0: host/screenshot.cpp polls every field until a
         * capture lands, so "the slot holds nothing yet" is the expected
         * answer and not a failure. */
        if (r.rgba.empty()) return 0;
        if (w) *w = r.width;
        if (h) *h = r.height;
        if (!dst) return r.rgba.size();          /* size query, slot kept */
        if (dst_bytes < r.rgba.size()) {
            /* The caller reads 0 as "nothing here", so a short buffer would
             * lose a capture without a word. The image is kept, which is what
             * the other backend does; the next arm drops it with a line
             * rather than leaving it to be served as a later capture. */
            rt_log_warn("gsr", "take_screenshot slot %u holds %zu bytes and the caller "
                               "offered %zu; the image is kept", slot, r.rgba.size(),
                        dst_bytes);
            return 0;
        }
        const size_t bytes = r.rgba.size();
        std::memcpy(dst, r.rgba.data(), bytes);
        r = ShotReady{};
        return bytes;
    }

    /* Turns every completed backbuffer copy into host-side pixels. Consumer
     * thread only, called at the top of each present: by then the submit that
     * carried the copy is a whole present old, so the wait returns at once
     * and the map and the crop cost one image's memcpy per capture rather
     * than a stall on the field the player is looking at. The same
     * arrangement as the paraLLEl-GS backend's drain_screenshots. */
    void drain_screenshots() {
        for (uint32_t slot = 0; slot < RT_PGS_SHOT_SLOTS; ++slot) {
            Shot& s = m_shot[slot];
            if (!s.pending) continue;
            /* Captured but not yet submitted: the copy is in the command list
             * being recorded right now, and note_shot_submit will give it a
             * timeline value. */
            if (s.timeline == 0) continue;
            m_device->wait(s.timeline);
            std::vector<uint8_t> rgba((size_t)s.width * s.height * 4);
            const uint8_t* src = (const uint8_t*)m_device->map(m_shot_buffer)
                               + s.buffer_offset;
            for (uint32_t y = 0; y < s.height; ++y) {
                std::memcpy(rgba.data() + (size_t)y * s.width * 4,
                            src + ((size_t)(s.origin_y + y) * s.surface_width + s.origin_x) * 4,
                            (size_t)s.width * 4);
            }
            {
                std::lock_guard<std::mutex> lk(m_shot_mu);
                m_shot_ready[slot].rgba = std::move(rgba);
                m_shot_ready[slot].width = s.width;
                m_shot_ready[slot].height = s.height;
            }
            s = Shot{};
        }
    }

    /* Raw scanout readback, for the replay tool. */
    bool write_scanout_ppm(const char* path) {
        /* The replay tool reports only that the file was not written, so the
         * reason has to come from here. Error level throughout: each of the
         * three is an operation that did not happen at all. */
        const char* where = path ? path : "(null)";
        if (!m_frame) {
            rt_log_error("gsr", "no scanout image to write to %s: no field with a picture "
                                "was reached", where);
            return false;
        }
        std::vector<uint8_t> pixels;
        uint32_t w = 0, h = 0;
        if (!m_device->read_texture(m_frame, pixels, &w, &h)) {
            rt_log_error("gsr", "the %s device could not read the scanout image back for %s",
                         m_backend_name, where);
            return false;
        }
        if (!write_ppm(path, pixels, w, h)) {
            rt_log_error("gsr", "could not write the %ux%u scanout to %s (%zu bytes read "
                                "back)", w, h, where, pixels.size());
            return false;
        }
        return true;
    }

    double display_aspect() const { return m_last_aspect; }

private:
    /* ---- the GIF sink ---------------------------------------------------- */

    struct Sink {
        NativeBackend& be;
        explicit Sink(NativeBackend& b) : be(b) {}

        void reg(uint32_t addr, uint64_t value) {
            ++be.m_reg_writes;
            be.m_regs.write(addr, value);
            switch (addr) {
                case GS_REG_TRXDIR:
                    /* A transfer reads and writes the host copy of local
                     * memory, so every primitive already assembled has to
                     * have been drawn and the words the device wrote have to
                     * be back on the host before it runs. */
                    be.gsr_flush_draws();
                    be.sync_for_transfer();
                    be.m_xfer.trxdir(value, be.m_regs, be.m_mem);
                    break;
                case gsr::GS_REG_PRIM:
                    /* A PRIM write starts a new primitive; the vertex queue
                     * is emptied, which is how a strip is restarted. */
                    be.m_draw.prim_written();
                    break;
                case gsr::GS_REG_TEX0_1:
                case gsr::GS_REG_TEX0_2:
                    /* A TEX0 write is where the CLUT load rules apply. */
                    be.clut_written(value);
                    break;
                case gsr::GS_REG_TEX2_1:
                case gsr::GS_REG_TEX2_2: {
                    /* TEX2 updates only PSM, CBP, CPSM, CSM, CSA and CLD of
                     * the TEX0 of the same context, so it is applied by
                     * merging those fields in; everything downstream reads
                     * TEX0 alone. The raw TEX2 value stays in the register
                     * file as written, because a dump has to reproduce the
                     * register stream exactly. */
                    const uint32_t t0 = (addr == gsr::GS_REG_TEX2_1) ? gsr::GS_REG_TEX0_1
                                                                     : gsr::GS_REG_TEX0_2;
                    const uint64_t merged = apply_tex2(be.m_regs.read(t0), value);
                    be.m_regs.write(t0, merged);
                    be.clut_written(merged);
                    break;
                }
                case gsr::GS_REG_TEXFLUSH:
                    /* TEXFLUSH invalidates the texture cache before the next
                     * drawing kick. This renderer reads texels out of local
                     * memory in place and keeps no texture cache at all, so
                     * there is nothing to invalidate; the write is counted
                     * and does nothing. The ordering it exists for is the
                     * feedback rule, which gs_draw.cpp's page tracker
                     * enforces from the addresses rather than from this
                     * register. */
                    ++be.m_texflushes;
                    break;
                case gsr::GS_REG_XYZ2:
                case gsr::GS_REG_XYZF2:
                case gsr::GS_REG_XYZ3:
                case gsr::GS_REG_XYZF3:
                    /* A vertex. The first two kick, the second two queue it
                     * without drawing (the PACKED ADC bit arrives here as
                     * one of those addresses; see gif_decode.h). */
                    be.m_draw.vertex(addr, value);
                    break;
                default:
                    break;
            }
        }

        void image(const uint8_t* qw, uint32_t qwords) {
            /* The armed transfer's range was synced at the TRXDIR write;
             * this catches a drawing kick that landed between the two. */
            be.gsr_flush_draws();
            be.sync_if_overlaps(be.m_xfer_first, be.m_xfer_last);
            be.m_xfer.host_to_local_data(qw, qwords, be.m_mem);
        }

        void note(const char* what) {
            /* One line per distinct message, whatever the packet rate. */
            if (be.m_notes.emplace(what, true).second) rt_log_warn("gsr", "%s", what);
        }
    };

    /* ---- device work ----------------------------------------------------- */

    /* Every dispatch this renderer issues goes through here.
     *
     * The group counts are all derived from guest registers, and none of the
     * four sites had anything bounding them: a scissor of 2047 by 2047 at
     * render scale 16 is a 512 by 512 tile grid, which is fine, but a
     * malformed FRAME or DISPLAY reaches the same arithmetic and the result
     * is bounded only by 32 bits. Vulkan guarantees 65535 groups per axis and
     * D3D12 states the same, and going past it is undefined rather than an
     * error the driver reports, so it is refused here with the numbers
     * instead. Refusing one dispatch loses one batch or one field's scanout,
     * which is a wrong picture with a line saying why; issuing it is a device
     * loss with nothing in the log at all.
     *
     * A zero count is not refused: it is a legal dispatch of no work, and it
     * happens for real when a batch's rectangle is empty. */
    bool dispatch_checked(rhi::CommandList* cmd, uint32_t gx, uint32_t gy, uint32_t gz,
                          const char* what) {
        const uint32_t* max = m_limits.max_workgroup_count;
        if (gx <= max[0] && gy <= max[1] && gz <= max[2]) {
            cmd->dispatch(gx, gy, gz);
            return true;
        }
        if (!m_logged_group_count) {
            m_logged_group_count = true;
            rt_log_error("gsr", "the %s pass asked for a %ux%ux%u workgroup grid and the %s "
                                "device reports %ux%ux%u; the dispatch is dropped and this "
                                "field's picture is wrong. The counts come from the registers "
                                "the game programmed, so the FRAME, SCISSOR and DISPLAY values "
                                "of this field are what to look at.",
                         what, gx, gy, gz, m_backend_name, max[0], max[1], max[2]);
        }
        return false;
    }

    void ensure_frame(uint32_t w, uint32_t h) {
        if (m_frame && m_device->texture_width(m_frame) == w
            && m_device->texture_height(m_frame) == h) {
            return;
        }
        if (m_frame) {
            m_device->wait_idle();
            m_device->destroy_texture(m_frame);
            m_frame = nullptr;
        }
        rhi::TextureDesc td;
        td.width = w;
        td.height = h;
        td.format = rhi::Format::RGBA8Unorm;
        td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled
                 | rhi::TextureUsage::CopySrc;
        /* CopyDst on every run, not only on a probe run. The RHI fixes a
         * texture's usage when it is created and has no way to add one later
         * (create_texture builds the D3D12 resource and the VkImage from
         * TextureDesc and neither API can widen it afterwards), and a probe
         * that only fires on a fault cannot ask for a differently created
         * image before the fault happens. The cost is nothing on D3D12 or
         * Metal, where a copy destination is a resource state and not a
         * creation flag, and one VK_IMAGE_USAGE_TRANSFER_DST_BIT on Vulkan on
         * an image that already carries STORAGE, SAMPLED and TRANSFER_SRC, so
         * there is no compression mode left for it to give up. */
        td.usage = td.usage | rhi::TextureUsage::CopyDst;
        td.debug_name = "gs scanout frame";
        m_frame = m_device->create_texture(td);
        /* A newly created frame image holds nothing, so the first weave field
         * has no partner and is woven with itself. docs/SETTINGS.md already
         * describes that as the behaviour after a mode change. */
        rhi::CommandList* cmd = m_device->begin_command_list();
        cmd->texture_barrier(m_frame, rhi::Stage::Host, rhi::Access::Write,
                             rhi::Stage::Compute, rhi::Access::ReadWrite);
        m_device->wait(m_device->submit(cmd));
        rt_log_info("gsr", "scanout frame is now %ux%u", w, h);
    }

    /* Rule 3: upload exactly the host-dirty blocks, as contiguous runs, and
     * clear them. After it the two copies agree over every block uploaded, so
     * nothing has to be marked device-written and nothing has to be read back
     * to make the upload safe. The old version uploaded the whole min-to-max
     * span, which wrote the host's copy of words it had never touched over
     * the device's newer ones, and dropped the shadow samples of every page
     * in between. */
    void upload_dirty_vram(rhi::CommandList* cmd) {
        if (!m_mem.any_host()) return;
        uint8_t* staging = (uint8_t*)m_device->map(m_vram_staging);
        uint32_t b = m_mem.host_lo();
        const uint32_t end = m_mem.host_hi();
        uint32_t b0 = 0, b1 = 0;
        uint64_t runs = 0, blocks = 0;
        while (gsr::LocalMemory::next_run(m_mem.host_bits(), b, end, &b0, &b1)) {
            const uint32_t first = gsr::LocalMemory::block_word(b0);
            const uint32_t last = gsr::LocalMemory::block_word(b1);
            /* Native local memory is about to change under the shadow, so the
             * pages this run covers lose their samples and will be seeded
             * again from the new contents before anything draws into them.
             * That is the transfer arm of gs_shadow.h's state machine, and it
             * is now told the run rather than the whole span (rule 5). */
            m_shadow.invalidate_words(first, last);
            const uint64_t offset = (uint64_t)first * 4;
            const uint64_t bytes = (uint64_t)(last - first) * 4;
            std::memcpy(staging + offset, m_mem.words() + first, (size_t)bytes);
            cmd->copy_buffer(m_vram, offset, m_vram_staging, offset, bytes);
            ++runs;
            blocks += b1 - b0;
            b = b1;
        }
        if (runs == 0) return;
        /* ReadWrite rather than Read: from milestone (b) the next compute
         * pass on this buffer may be the rasteriser, which writes it. */
        cmd->buffer_barrier(m_vram, rhi::Stage::Copy, rhi::Access::Write,
                            rhi::Stage::Compute, rhi::Access::ReadWrite);
        m_mem.clear_host_blocks(m_mem.host_lo(), m_mem.host_hi());
        m_upload_runs += runs;
        m_upload_blocks += blocks;
        ++m_uploads;
    }

    /* A TEX0 (or merged TEX2) value has landed. The CLUT load rules decide
     * whether anything is read at all; when something is, the host copy of
     * local memory has to be current over the source first, which means
     * drawing the open batch when it is writing those pages and reading the
     * device buffer back when it holds them. */
    void clut_written(uint64_t tex0) {
        const uint64_t texclut = m_regs.read(GS_REG_TEXCLUT);
        if (m_clut.would_load(tex0)) {
            uint32_t first = 0, last = 0;
            m_clut.source_word_range(tex0, texclut, &first, &last);
            PageSet src;
            gs_mark_page_words(first, last, &src);
            if (src.intersects(m_draw.written_pages())) gsr_flush_draws();
            sync_if_overlaps(first, last);
        }
        m_clut.tex0_written(tex0, texclut);
    }

    /* ---- drawing ---------------------------------------------------------
     *
     * Where the authoritative copy of local memory is, now that both sides
     * write it. Ownership is per GS block, 256 bytes, in the two bitmaps
     * LocalMemory carries, and the five rules are:
     *
     *   1  a host write reconciles the blocks it will touch from the device
     *      first, and only the blocks the device owns, then writes and marks
     *      them host-dirty. sync_for_transfer is where that happens, over the
     *      union of the transfer's source and destination.
     *   2  a readback for any other reason copies only device-owned blocks
     *      that are not host-dirty. A host-dirty block is never overwritten
     *      by device contents; that is what threw away the font uploads.
     *   3  upload_dirty_vram uploads exactly the host-dirty blocks as
     *      contiguous runs and clears them. After it both copies agree over
     *      every block it wrote, so nothing is marked either way.
     *   4  a batch's FRAME and ZBUF write range marks its blocks
     *      device-written and clears host-dirty for them.
     *   5  the shadow's drop and seed decision follows the same runs, not a
     *      conservative span.
     *
     * The one place block granularity can still merge two owners is a block
     * both sides wrote since the last reconciliation. The ordering prevents
     * it: gsr_flush_draws uploads every host-dirty block before it dispatches,
     * so rule 4 never runs over a host-owned block, and rule 1 reconciles
     * before a host write lands on a device-owned one. Where the two marks do
     * meet, LocalMemory::both_owners() counts it and the end-of-run summary
     * says so, because one side's 256 bytes are then lost and which side
     * depends on the order the marks arrived in.
     */

    static bool ranges_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
        return a0 < a1 && b0 < b1 && a0 < b1 && b0 < a1;
    }

    /* Reads the words the rasteriser wrote back into the host store. They go
     * in without marking the store dirty: the device already holds exactly
     * these values, so uploading them again would be work for nothing. */
    /* Rule 2: bring the device's words into the host store over [first, last),
     * and only for the blocks the device owns.
     *
     * A host-dirty block is never overwritten: the host holds words the device
     * has not got, and copying the device over them is what threw away the
     * PSMT4 font uploads. Only device-written blocks that are not host-dirty
     * are read, as contiguous runs packed consecutively into the readback
     * buffer, so a range with nothing to reconcile costs no submit at all and
     * a range with a few blocks costs a few kilobytes rather than the whole
     * min-to-max span.
     *
     * The runs are recorded on one command list and waited on once. */
    void reconcile_from_device(uint32_t first, uint32_t last) {
        if (first >= last || !m_mem.any_device()) return;
        uint32_t b = gsr::LocalMemory::block_of(first);
        const uint32_t end = gsr::LocalMemory::block_end_of(last);
        if (b < m_mem.device_lo()) b = m_mem.device_lo();
        const uint32_t stop = end < m_mem.device_hi() ? end : m_mem.device_hi();
        if (b >= stop) return;

        struct Run { uint32_t b0, b1; uint64_t offset; };
        std::vector<Run> runs;
        uint64_t packed = 0;
        uint32_t r0 = 0, r1 = 0;
        while (gsr::LocalMemory::next_run(m_mem.device_bits(), b, stop, &r0, &r1)) {
            /* Split the device run on the host-dirty blocks inside it: those
             * belong to the host and must not be read over. */
            uint32_t k = r0;
            while (k < r1) {
                while (k < r1 && m_mem.host_block(k)) ++k;
                uint32_t s0 = k;
                while (k < r1 && !m_mem.host_block(k)) ++k;
                if (k > s0) {
                    Run run{ s0, k, packed };
                    packed += (uint64_t)(k - s0) * gsr::LocalMemory::kBlockWords * 4ull;
                    runs.push_back(run);
                }
            }
            b = r1;
        }
        if (runs.empty()) return;

        rhi::Buffer* rb = ensure_vram_readback();
        if (packed > m_device->buffer_size(rb)) return;
        rhi::CommandList* cmd = m_device->begin_command_list();
        cmd->buffer_barrier(m_vram, rhi::Stage::Compute, rhi::Access::Write,
                            rhi::Stage::Copy, rhi::Access::Read);
        for (const Run& run : runs) {
            const uint64_t src = (uint64_t)gsr::LocalMemory::block_word(run.b0) * 4ull;
            const uint64_t bytes = (uint64_t)(run.b1 - run.b0)
                                 * gsr::LocalMemory::kBlockWords * 4ull;
            cmd->copy_buffer(rb, run.offset, m_vram, src, bytes);
        }
        m_device->wait(m_device->submit(cmd));
        const uint8_t* base = (const uint8_t*)m_device->map(rb);
        uint64_t blocks = 0;
        for (const Run& run : runs) {
            const uint32_t w0 = gsr::LocalMemory::block_word(run.b0);
            const uint64_t bytes = (uint64_t)(run.b1 - run.b0)
                                 * gsr::LocalMemory::kBlockWords * 4ull;
            std::memcpy(m_mem.words() + w0, base + run.offset, (size_t)bytes);
            m_mem.clear_device_blocks(run.b0, run.b1);
            blocks += run.b1 - run.b0;
        }
        ++m_vram_syncs;
        m_vram_sync_runs += runs.size();
        m_vram_sync_blocks += blocks;
        m_vram_sync_words += packed / 4u;
    }

    void sync_if_overlaps(uint32_t first, uint32_t last) {
        reconcile_from_device(first, last);
    }

    /* The host store is about to be uploaded over the range it marked dirty.
     * If any of those words are ones the device wrote, the host copy of them
     * is stale and uploading it would undo the drawing, so they come back
     * first. */
    void host_current_for_upload() {
        /* Nothing to do since the ownership tracker landed. The upload writes
         * exactly the blocks the host owns, and a block the host owns is by
         * construction one the device has no newer words in, so there is
         * nothing to bring back first. It used to reconcile the whole dirty
         * span, which is what made a fresh upload wait on a 1.7 MB readback
         * and, worse, let that readback overwrite the upload. Kept as a named
         * no-op so the two call sites still say where the rule applies. */
    }

    /* A conservative word range for the transfer BITBLTBUF/TRXPOS/TRXREG
     * describe, both endpoints, kept so image() can reuse it. */
    /* Which base blocks this run writes, and by what.
     *
     * The font probe found a full-screen sprite sampling PSMT8H at block
     * 10240, a region beyond both frame buffers and the Z buffer, empty on
     * both the host and the device. The question "who was supposed to write
     * block 10240" has no answer anywhere in the log, and it should not need a
     * hard-coded address to ask: these two lists are every distinct
     * destination the rasteriser and the transfer engine used, so a block
     * nothing writes is visible by its absence. Capped, and reported once at
     * the end of a run. */
    struct BlockTarget {
        uint32_t base_block = 0;
        uint32_t bw = 0;
        uint32_t psm = 0;
        uint32_t mask = 0;      /* FBMSK for a draw, 0 for a transfer */
        uint64_t count = 0;
    };
    static void note_target(std::vector<BlockTarget>& v, uint32_t block, uint32_t bw,
                            uint32_t psm, uint32_t mask) {
        for (BlockTarget& t : v) {
            if (t.base_block == block && t.bw == bw && t.psm == psm && t.mask == mask) {
                ++t.count;
                return;
            }
        }
        if (v.size() >= 24) return;
        BlockTarget t;
        t.base_block = block;
        t.bw = bw;
        t.psm = psm;
        t.mask = mask;
        t.count = 1;
        v.push_back(t);
    }

    /* The "transfer check" that used to live here was removed on 2026-09-05.
     * It counted the nonzero words of the destination's min-to-max word span
     * after each of the first twelve transfers, which is range blind: a
     * transfer into a narrow rectangle of a wide buffer spans most of that
     * buffer, so the count said nothing about whether the texels landed where
     * BITBLTBUF pointed. It reported on the wrong thing at warn level. If the
     * question comes back, the measurement is per texel against the rectangle
     * the transfer actually names, not against a span. */

    void sync_for_transfer() {
        const Bitbltbuf b = decode_bitbltbuf(m_regs.read(GS_REG_BITBLTBUF));
        note_target(m_xfer_targets, b.dbp, b.dbw, b.dpsm, 0);
        const Trxpos p = decode_trxpos(m_regs.read(GS_REG_TRXPOS));
        const Trxreg r = decode_trxreg(m_regs.read(GS_REG_TRXREG));
        uint32_t sf, sl, df, dl;
        gs_buffer_word_range(b.spsm, b.sbp, b.sbw, p.ssay + r.rrh, &sf, &sl);
        gs_buffer_word_range(b.dpsm, b.dbp, b.dbw, p.dsay + r.rrh, &df, &dl);
        m_xfer_first = sf < df ? sf : df;
        m_xfer_last = sl > dl ? sl : dl;
        sync_if_overlaps(m_xfer_first, m_xfer_last);
    }

    static rhi::Buffer* grow_storage(rhi::Device* dev, rhi::Buffer* b, uint64_t bytes,
                                     const char* name) {
        if (b && dev->buffer_size(b) >= bytes) return b;
        if (b) {
            dev->wait_idle();
            dev->destroy_buffer(b);
        }
        rhi::BufferDesc bd;
        /* Doubled, so a batch that grows a little does not reallocate every
         * flush. */
        bd.size = bytes * 2u;
        bd.kind = rhi::BufferKind::Upload;
        bd.usage = rhi::BufferUsage::Storage;
        bd.debug_name = name;
        return dev->create_buffer(bd);
    }

    /* One batch: the records and the bin lists to the device, one dispatch of
     * one workgroup per 16x16 tile over the batch's own tile rectangle, then
     * a wait. The wait is what makes the host store safe to read again and is
     * the honest cost of a milestone that reconciles two copies of local
     * memory; a later milestone can pipeline it behind a second set of upload
     * buffers. */
    void gsr_flush_draws() override {
        if (m_draw.empty()) return;
        m_draw.build_bins();
        const uint32_t samples = m_shadow.samples();
        uint32_t tile_w = 0, tile_h = 0;
        gs_scale_tile(samples, &tile_w, &tile_h);
        uint32_t tx = 0, ty = 0, tw = 0, th = 0;
        m_draw.tile_grid(tile_w, tile_h, &tx, &ty, &tw, &th);
        if (tw == 0 || th == 0) {
            /* The batch covers no tile, so nothing would be dispatched and
             * the primitives are thrown away. That is guest drawing
             * disappearing, which is never quiet: counted for the stats block
             * and said once. */
            ++m_empty_batches;
            if (!m_logged_empty_batch) {
                m_logged_empty_batch = true;
                rt_log_warn("gsr", "a draw batch covers no %ux%u tile and was dropped "
                                   "without being drawn. Said once; the run total is in "
                                   "the stats block", tile_w, tile_h);
            }
            m_draw.clear();
            return;
        }

        check_frame_z_alias();

        const uint64_t prim_bytes = (uint64_t)m_draw.prims().size() * 4u;
        const uint64_t idx_bytes = (uint64_t)m_draw.bin_index().size() * 4u;
        const uint64_t rng_bytes = (uint64_t)m_draw.bin_range().size() * 4u;
        /* A batch with no textured primitive has no CLUT snapshots, and a
         * descriptor still has to point at something, so the binding gets
         * one word rather than a null buffer. */
        const uint64_t clut_words = (uint64_t)m_draw.cluts().size();
        const uint64_t clut_bytes = clut_words ? clut_words * 4u : 4u;
        m_prim_buffer = grow_storage(m_device, m_prim_buffer, prim_bytes, "gs primitives");
        m_binidx_buffer = grow_storage(m_device, m_binidx_buffer, idx_bytes, "gs bin lists");
        m_binrng_buffer = grow_storage(m_device, m_binrng_buffer, rng_bytes, "gs bin ranges");
        m_clut_buffer = grow_storage(m_device, m_clut_buffer, clut_bytes, "gs CLUT table");
        if (clut_words) {
            std::memcpy(m_device->map(m_clut_buffer), m_draw.cluts().data(),
                        (size_t)clut_words * 4u);
        }
        std::memcpy(m_device->map(m_prim_buffer), m_draw.prims().data(), (size_t)prim_bytes);
        std::memcpy(m_device->map(m_binidx_buffer), m_draw.bin_index().data(),
                    (size_t)idx_bytes);
        std::memcpy(m_device->map(m_binrng_buffer), m_draw.bin_range().data(),
                    (size_t)rng_bytes);

        host_current_for_upload();
        rhi::CommandList* cmd = m_device->begin_command_list();
        upload_dirty_vram(cmd);

        /* The shadow, in the order the dependencies run: the pages this batch
         * draws into that have no samples yet are broadcast out of the native
         * copy the upload above just made current, then the fine pass writes
         * samples, then the resolve puts the native picture back. */
        const bool use_shadow = samples > 1 && ensure_shadow(samples);
        if (use_shadow) {
            PageSet touched;
            batch_shadow_pages(m_draw.push(), &touched);
            m_shadow.take_seed_list(touched, m_seed_pages);
            if (!m_seed_pages.empty()) record_shadow_seed(cmd, samples);
        }

        cmd->bind_compute_pipeline(m_raster);
        cmd->bind_storage_buffer(0, m_vram, 0, 0);
        cmd->bind_storage_buffer(1, m_prim_buffer, 0, prim_bytes);
        cmd->bind_storage_buffer(2, m_binidx_buffer, 0, idx_bytes);
        cmd->bind_storage_buffer(3, m_binrng_buffer, 0, rng_bytes);
        cmd->bind_storage_buffer(4, m_clut_buffer, 0, clut_bytes);
        if (use_shadow) cmd->bind_storage_buffer(5, m_shadow_buffer, 0, 0);
        RasterPush push = m_draw.push();
        push.tile_x0 = tx;
        push.tile_y0 = ty;
        push.samples = use_shadow ? samples : 1u;
        push.tile_w = use_shadow ? tile_w : GSP_TILE_PIXELS;
        push.tile_h = use_shadow ? tile_h : GSP_TILE_PIXELS;
        push.shadow = use_shadow ? 1u : 0u;
        cmd->push_constants(&push, sizeof(push));
        dispatch_checked(cmd, tw, th, 1, "fine rasteriser");
        cmd->buffer_barrier(m_vram, rhi::Stage::Compute, rhi::Access::Write,
                            rhi::Stage::Compute, rhi::Access::ReadWrite);
        if (use_shadow) {
            cmd->buffer_barrier(m_shadow_buffer, rhi::Stage::Compute, rhi::Access::Write,
                                rhi::Stage::Compute, rhi::Access::Read);
            record_shadow_resolve(cmd, samples, push);
            cmd->buffer_barrier(m_vram, rhi::Stage::Compute, rhi::Access::Write,
                                rhi::Stage::Compute, rhi::Access::ReadWrite);
        }
        /* The other place a stalled device stops the run; see the same pair
         * in vsync. */
        rt_run_note_rhi("a draw batch submitted, waiting");
        m_device->wait(m_device->submit(cmd));
        rt_run_note_rhi("idle");

        uint32_t wf = 0, wl = 0;
        m_draw.write_range(&wf, &wl);
        /* Rule 4: the device now holds words the host store does not, over
         * exactly this batch's FRAME and ZBUF range. gsr_flush_draws uploads
         * every host-dirty block before it dispatches, so no block here can
         * still be host-owned; LocalMemory counts it if one is. */
        m_mem.note_device_words(wf, wl);
        note_target(m_draw_targets, push.frame_base_block, push.frame_bw, push.frame_psm,
                    push.frame_mask);

        ++m_dispatches;
        m_draw.clear();
    }

    /* A frame buffer and a Z buffer in the same memory. The GS writes both
     * through local memory, so where the two name the same bits they are one
     * storage cell that the pixel pipeline writes twice, colour then depth;
     * the fine pass reproduces that per pixel (gs_prim.h's gs_alias_pixel and
     * the block at the end of shade()). Reported once, because a game that
     * does it on purpose is doing something worth seeing in the log.
     *
     * The case the fine pass does not model per pixel is a partial overlap:
     * two buffers of different widths over the same word, where a colour and
     * a depth share bits without sharing all of them. The masked atomics keep
     * the word itself consistent, but the per-pixel ordering inside a tile is
     * not reproduced, so that one is a warning. */
    void check_frame_z_alias() {
        if (m_logged_fz_overlap) return;
        const RasterPush& p = m_draw.push();
        if (!p.z_write) return;
        const uint32_t rows = m_draw.max_row();
        uint32_t ff, fl, zf, zl;
        gs_buffer_word_range(p.frame_psm, p.frame_base_block, p.frame_bw, rows, &ff, &fl);
        gs_buffer_word_range(p.z_psm, p.z_base_block, p.frame_bw, rows, &zf, &zl);
        if (!ranges_overlap(ff, fl, zf, zl)) return;
        m_logged_fz_overlap = true;
        const bool same_width = gs_addr_bits(p.frame_psm) == gs_addr_bits(p.z_psm);
        if (same_width) {
            rt_log_info("gsr", "FRAME at block %u and ZBUF at block %u share local memory; "
                               "a pixel whose colour and depth land on the same bits is "
                               "written colour first and depth second, so the depth is what "
                               "the memory holds", p.frame_base_block, p.z_base_block);
        } else {
            rt_log_warn("gsr", "FRAME at block %u (PSM 0x%02x) and ZBUF at block %u "
                               "(PSM 0x%02x) overlap at different widths; the per-pixel "
                               "write order is not modelled for a partial overlap",
                        p.frame_base_block, p.frame_psm, p.z_base_block, p.z_psm);
        }
    }

    /* ---- the super-sampled shadow ----------------------------------------
     *
     * gs_shadow.h states the model: one plane of the whole of local memory
     * per sample, valid per page, seeded from native local memory and
     * resolved back into it after every batch. What is here is the three
     * dispatches that make it so.
     */

    /* Every page the fine pass will read or write at this scale, which is
     * both buffers over the whole rectangle the batch covers and not just the
     * pages its primitives land on. Three reasons it has to be the whole
     * rectangle:
     *
     *   - the fine pass loads and stores every pixel of every tile it
     *     dispatches, including the pixels no primitive covers, and the
     *     resolve writes native local memory back over the same rectangle. A
     *     page it read unseeded would resolve garbage into a picture nothing
     *     drew on.
     *   - the depth test reads the Z buffer even when ZMSK stops it being
     *     written, so the Z pages are seeded whether or not this batch
     *     writes them.
     *
     * A pixel inside the rectangle that no primitive touches is seeded from
     * native and resolves back to it, because every sample of it holds the
     * same value, so the round trip is an identity. */
    void batch_shadow_pages(const RasterPush& p, PageSet* out) {
        uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        /* No pixel grid means no page is seeded. The same condition skips the
         * resolve, which is where it is reported; see record_shadow_resolve. */
        if (!m_draw.pixel_grid(&x0, &y0, &x1, &y1)) return;
        gs_mark_pages(p.frame_psm, p.frame_base_block, p.frame_bw, (int32_t)x0,
                      (int32_t)y0, (int32_t)x1, (int32_t)y1, out);
        gs_mark_pages(p.z_psm, p.z_base_block, p.frame_bw, (int32_t)x0, (int32_t)y0,
                      (int32_t)x1, (int32_t)y1, out);
    }

    bool ensure_shadow(uint32_t samples) {
        const uint64_t bytes = gs_shadow_bytes(samples);
        if (m_shadow_buffer && m_device->buffer_size(m_shadow_buffer) >= bytes) return true;
        if (m_shadow_buffer) {
            m_device->wait_idle();
            m_device->destroy_buffer(m_shadow_buffer);
            m_shadow_buffer = nullptr;
        }
        rhi::BufferDesc bd;
        bd.size = bytes;
        bd.kind = rhi::BufferKind::DeviceLocal;
        bd.usage = rhi::BufferUsage::Storage;
        bd.debug_name = "gs super-sampled shadow";
        m_shadow_buffer = m_device->create_buffer(bd);
        /* Nothing in it means anything until a page is seeded. */
        m_shadow.invalidate_all();
        if (!m_shadow_buffer && !m_logged_shadow_alloc) {
            /* The caller falls back to scale 1 for this batch, which is the
             * picture the user asked not to have. Once, because the next
             * batch will try the same allocation again. */
            m_logged_shadow_alloc = true;
            rt_log_warn("gsr", "the %llu MiB super-sampled shadow for %u samples per pixel "
                               "could not be allocated; drawing falls back to scale 1",
                        (unsigned long long)(bytes >> 20), samples);
        }
        return m_shadow_buffer != nullptr;
    }

    void record_shadow_seed(rhi::CommandList* cmd, uint32_t samples) {
        const uint64_t bytes = (uint64_t)m_seed_pages.size() * 4u;
        m_seed_buffer = grow_storage(m_device, m_seed_buffer, bytes, "gs shadow page list");
        std::memcpy(m_device->map(m_seed_buffer), m_seed_pages.data(), (size_t)bytes);

        ShadowPush sp{};
        sp.mode = GSR_SHADOW_SEED;
        sp.samples = samples;
        cmd->bind_compute_pipeline(m_shadow_pipe);
        cmd->bind_storage_buffer(0, m_vram, 0, 0);
        cmd->bind_storage_buffer(1, m_seed_buffer, 0, bytes);
        cmd->bind_storage_buffer(5, m_shadow_buffer, 0, 0);
        cmd->push_constants(&sp, sizeof(sp));
        dispatch_checked(cmd, (uint32_t)m_seed_pages.size(), 1, 1, "shadow seed");
        cmd->buffer_barrier(m_shadow_buffer, rhi::Stage::Compute, rhi::Access::Write,
                            rhi::Stage::Compute, rhi::Access::ReadWrite);
    }

    void record_shadow_resolve(rhi::CommandList* cmd, uint32_t samples,
                               const RasterPush& push) {
        uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!m_draw.pixel_grid(&x0, &y0, &x1, &y1)) {
            /* The fine pass has already written samples into the shadow and
             * nothing will put them back into native local memory, so the
             * picture keeps the pixels from before the batch. gsr_flush_draws
             * only reaches here with a non-empty tile grid, so an empty pixel
             * grid is the two disagreeing and not an ordinary empty batch. */
            if (!m_logged_no_resolve) {
                m_logged_no_resolve = true;
                rt_log_warn("gsr", "a batch with a tile grid reported no pixel grid, so its "
                                   "super-sampled result is not resolved back into local "
                                   "memory and the drawing is lost");
            }
            return;
        }
        ShadowPush sp{};
        sp.mode = GSR_SHADOW_RESOLVE;
        sp.samples = samples;
        sp.frame_base_block = push.frame_base_block;
        sp.frame_bw = push.frame_bw;
        sp.frame_psm = push.frame_psm;
        sp.frame_mask = push.frame_mask;
        sp.z_base_block = push.z_base_block;
        sp.z_psm = push.z_psm;
        sp.z_write = push.z_write;
        sp.x0 = x0;
        sp.y0 = y0;
        sp.x1 = x1;
        sp.y1 = y1;
        cmd->bind_compute_pipeline(m_shadow_pipe);
        cmd->bind_storage_buffer(0, m_vram, 0, 0);
        cmd->bind_storage_buffer(5, m_shadow_buffer, 0, 0);
        cmd->push_constants(&sp, sizeof(sp));
        dispatch_checked(cmd, (x1 - x0) / 8u + 1u, (y1 - y0) / 8u + 1u, 1, "shadow resolve");
        m_shadow.note_resolve();
    }

    /* ---- the high-resolution scanout decision ------------------------------
     *
     * docs/SETTINGS.md section 6: at render scale 4 and up the picture is
     * built from the sub-samples at double resolution, but only on a buffer
     * the game drew into, because that is the only buffer whose sub-samples
     * exist. Here that is exactly "every page the enabled circuits read has a
     * valid shadow", and nothing else. FFMD does not disqualify a buffer:
     * scanout.comp has an arm for each setting of it, weaving the two
     * interleaved fields of an FFMD 1 buffer and doubling the field line of
     * an FFMD 0 one, so a buffer that carries sub-samples is usable either
     * way. FFMD is still computed here, because the pages an FFMD 1 circuit
     * reads are twice as tall and the shadow has to be valid over all of
     * them. docs/GS_RENDERER.md says the same beside the hires row.
     *
     * The decision is reported the first time it is made and again whenever
     * it changes, which is what the `hires=` field on the paraLLEl-GS path's
     * scanout line does. */
    bool choose_hires(const ScanoutPlan& plan) {
        bool ok = m_shadow.active() && m_shadow_buffer != nullptr;
        const char* why = "render scale is 1";
        if (ok) {
            /* FFMD's row rule is FFMD's alone and does not depend on INT, so
             * the pages an FFMD circuit reads are twice as tall whatever the
             * raster is. scanout.comp's merge_at states the rule and its
             * source. */
            const bool ffmd = plan.push.ffmd != 0;
            PageSet reads;
            mark_circuit_pages(plan.push.c1_enable, plan.push.c1_psm,
                               plan.push.c1_base_block, plan.push.c1_fbw,
                               plan.push.c1_dbx, plan.push.c1_dby,
                               plan.push.c1_w, plan.push.c1_h, ffmd, &reads);
            mark_circuit_pages(plan.push.c2_enable, plan.push.c2_psm,
                               plan.push.c2_base_block, plan.push.c2_fbw,
                               plan.push.c2_dbx, plan.push.c2_dby,
                               plan.push.c2_w, plan.push.c2_h, ffmd, &reads);
            /* An empty read set is not a buffer whose pages are all valid.
             *
             * all_valid() is a subset test, so it answers yes for a set with
             * nothing in it, and mark_circuit_pages marks nothing whenever a
             * circuit's window is empty or its PSM has no addressing
             * (gs_mark_pages returns on GS_FAM_BAD). The scanout would then
             * be read out of a shadow no draw ever seeded for that buffer:
             * whatever the last batch at this scale left in those planes,
             * shown as the picture. Requiring at least one page makes the
             * test say what it means. */
            bool any = false;
            for (uint32_t i = 0; i < GSP_PAGE_WORDS; ++i) any = any || reads.bits[i] != 0;
            if (!any) {
                ok = false;
                why = "the enabled circuits name no page of local memory, so there is no "
                      "buffer to check for sub-samples";
            } else if (!m_shadow.all_valid(reads)) {
                ok = false;
                why = "the displayed buffer holds no sub-samples: nothing drew it at this "
                      "scale";
            }
        }
        const int state = ok ? 1 : 0;
        if (state != m_logged_hires) {
            m_logged_hires = state;
            if (ok) {
                rt_log_info("gsr", "scanout hires=yes: %ux%u from %u samples per pixel, not "
                                   "deinterlaced", plan.push.frame_w * 2,
                            plan.push.frame_h * 2, m_shadow.samples());
            } else if (m_present.render_scale > 1) {
                /* The run asked for a scale and this field is not getting
                 * one, which is a refusal by the level table in runtime.h.
                 * At scale 1 the same line is only a statement of the
                 * setting, so it stays at info. */
                rt_log_warn("gsr", "scanout hires=no at display.render_scale %u (%s)",
                            m_present.render_scale, why);
            } else {
                rt_log_info("gsr", "scanout hires=no (%s)", why);
            }
        }
        return ok;
    }

    /* The pages one circuit reads. With FFMD the rows are the field's own
     * lines doubled, so the buffer rows the two fields between them cover run
     * from DBY * 2 to (DBY + h - 1) * 2 + 1. */
    static void mark_circuit_pages(uint32_t enable, uint32_t psm, uint32_t base_block,
                                   uint32_t fbw, uint32_t dbx, uint32_t dby, uint32_t w,
                                   uint32_t h, bool ffmd, PageSet* out) {
        if (!enable || w == 0 || h == 0) return;
        const uint32_t y0 = ffmd ? dby * 2u : dby;
        const uint32_t y1 = ffmd ? (dby + h - 1u) * 2u + 1u : dby + h - 1u;
        gs_mark_pages(psm, base_block, fbw, (int32_t)dbx, (int32_t)y0,
                      (int32_t)(dbx + w - 1), (int32_t)y1, out);
    }

    /* The output image's size. A high-resolution scanout is twice the frame
     * on each axis: two sub-samples across and two down are what every
     * allowed sample count has (gs_prim.h's grid), so double is the
     * resolution the samples actually carry. */
    static uint32_t scanout_width(const ScanoutPlan& plan) {
        return plan.push.hires ? plan.push.frame_w * 2 : plan.push.frame_w;
    }
    static uint32_t scanout_height(const ScanoutPlan& plan) {
        return plan.push.hires ? plan.push.frame_h * 2 : plan.push.frame_h;
    }

    /* Whether the frame the CRTC asked for is one this renderer can build.
     *
     * Nothing bounded it before. gs_crtc.cpp derives frame_w and frame_h from
     * DISPLAY DX + DW+1 over the magnification and DY + DH+1, doubled for an
     * interlaced raster; DW is twelve bits and DH eleven, so a DISPLAY value
     * the game never programs in practice still reaches here and asks for an
     * image several times the largest either backend can create, and hires
     * doubles it again. Zero is the other end of the same range: an image of
     * width zero is a fatal in create_texture and a history buffer of zero
     * bytes is a fatal in create_buffer, neither of which says which
     * registers produced it.
     *
     * Reported every time the answer changes rather than once, because the
     * numbers are what identify the field. */
    bool scanout_size_ok(const ScanoutPlan& plan) {
        const uint32_t w = scanout_width(plan);
        const uint32_t h = scanout_height(plan);
        const bool ok = w != 0 && h != 0 && w <= kMaxScanoutAxis && h <= kMaxScanoutAxis;
        if (!ok && !m_logged_frame_size) {
            m_logged_frame_size = true;
            rt_log_error("gsr", "the CRTC asked for a %ux%u frame (hires %u) and this renderer "
                                "builds up to %ux%u; the field is not scanned out. DISPLAY1 "
                                "and DISPLAY2 are what set the window, and DISPFB names the "
                                "buffer it reads.",
                         w, h, plan.push.hires, kMaxScanoutAxis, kMaxScanoutAxis);
        }
        return ok;
    }

    /* The previous field, for display.deinterlace adaptive. The filter
     * compares what is standing at a row against what this field would bob
     * there, and a storage buffer is what carries it: the scanout image is a
     * storage image the RHI only guarantees stores into, and reading one back
     * would ask a typed UAV load of the D3D12 backend that feature level 12_0
     * does not guarantee. */
    void ensure_history(uint32_t w, uint32_t h) {
        const uint64_t bytes = (uint64_t)w * h * 4u;
        if (m_history_buffer && m_device->buffer_size(m_history_buffer) >= bytes) return;
        if (m_history_buffer) {
            m_device->wait_idle();
            m_device->destroy_buffer(m_history_buffer);
        }
        /* A fresh allocation holds nothing, and there is no fill in the RHI,
         * so the first field after one is told there is no history and bobs
         * every row rather than comparing against whatever was in the
         * memory. */
        m_history_fresh = true;
        rhi::BufferDesc bd;
        bd.size = bytes;
        bd.kind = rhi::BufferKind::DeviceLocal;
        bd.usage = rhi::BufferUsage::Storage;
        bd.debug_name = "gs deinterlace history";
        m_history_buffer = m_device->create_buffer(bd);
    }

    void record_scanout(rhi::CommandList* cmd, const ScanoutPlan& plan) {
        /* Every slot scanout.comp declares has to carry a real buffer.
         *
         * Both backends fill an unbound slot with the device's 256-byte dummy
         * so the descriptor is legal (rhi_vulkan_cmd.cpp build_descriptor_set,
         * rhi_d3d12_cmd.cpp build_descriptor_table). That keeps the driver
         * quiet and does not keep the shader inside the allocation: the
         * history slot is written once per output pixel, so a null there is a
         * stream of out-of-bounds writes into 256 bytes, which on this
         * hardware is a device removal with nothing in the log. The shadow
         * slot is read at plane * 4 MiB for a hires field, the same fault
         * further out. Both are caller invariants today (vsync calls
         * ensure_history, choose_hires requires the shadow); this says so
         * rather than trusting the two to stay that way. */
        rhi::Buffer* samples = plan.push.hires ? m_shadow_buffer : m_vram;
        if (!m_frame || !m_history_buffer || !samples) {
            if (!m_logged_scanout_bind) {
                m_logged_scanout_bind = true;
                rt_log_error("gsr", "the scanout pass has no %s, so this field is not scanned "
                                    "out and the last picture stays on screen",
                             !m_frame ? "frame image"
                                      : (!m_history_buffer ? "deinterlace history buffer"
                                                           : "super-sampled shadow"));
            }
            return;
        }
        cmd->bind_compute_pipeline(m_scanout);
        cmd->bind_storage_buffer(0, m_vram, 0, 0);
        cmd->bind_storage_buffer(1, samples, 0, 0);
        cmd->bind_storage_buffer(2, m_history_buffer, 0, 0);
        cmd->bind_storage_image(0, m_frame);
        cmd->push_constants(&plan.push, sizeof(plan.push));
        const uint32_t gx = (scanout_width(plan) + 7) / 8;
        const uint32_t gy = (scanout_height(plan) + 7) / 8;
        dispatch_checked(cmd, gx, gy, 1, "scanout");
        cmd->texture_barrier(m_frame, rhi::Stage::Compute, rhi::Access::Write,
                             rhi::Stage::Copy, rhi::Access::Read);
    }

    /* Where the picture lands in the backbuffer, in backbuffer pixels. */
    void present_rect(uint32_t surface_w, uint32_t surface_h, uint32_t frame_w,
                      uint32_t frame_h, double aspect,
                      int32_t* x, int32_t* y, uint32_t* w, uint32_t* h) const {
        if (aspect <= 0.0) aspect = double(frame_w) / double(frame_h ? frame_h : 1);
        double target_w = double(surface_w);
        double target_h = double(surface_h);
        if (m_present.fit == RT_PGS_FIT_STRETCH) {
            /* Fill the surface and ignore the aspect, which is what the
             * setting says. */
        } else if (m_present.fit == RT_PGS_FIT_INTEGER) {
            /* The largest whole multiple of the frame that fits, with the
             * aspect applied to the width. A frame larger than the surface
             * still gets one multiple, because zero would present nothing. */
            const double pixel_w = aspect * double(frame_h) / double(frame_w);
            uint32_t scale = 1;
            for (uint32_t s = 1; s < 64; ++s) {
                if (double(frame_w) * pixel_w * s <= double(surface_w)
                    && double(frame_h) * s <= double(surface_h)) {
                    scale = s;
                } else {
                    break;
                }
            }
            target_w = double(frame_w) * pixel_w * scale;
            target_h = double(frame_h) * scale;
        } else {
            /* Letterbox: the largest rectangle of the display aspect that
             * fits inside the surface. */
            if (double(surface_w) / double(surface_h) > aspect) {
                target_h = double(surface_h);
                target_w = target_h * aspect;
            } else {
                target_w = double(surface_w);
                target_h = target_w / aspect;
            }
        }
        if (target_w < 1.0) target_w = 1.0;
        if (target_h < 1.0) target_h = 1.0;
        *w = (uint32_t)(target_w + 0.5);
        *h = (uint32_t)(target_h + 0.5);
        *x = (int32_t)((double(surface_w) - target_w) * 0.5 + 0.5);
        *y = (int32_t)((double(surface_h) - target_h) * 0.5 + 0.5);
    }

    void record_present(rhi::CommandList* cmd, rhi::Texture* backbuffer,
                        rhi::Texture* picture, double aspect) {
        const uint32_t sw = m_device->texture_width(backbuffer);
        const uint32_t sh = m_device->texture_height(backbuffer);

        if (picture) {
            int32_t x = 0, y = 0;
            uint32_t w = sw, h = sh;
            present_rect(sw, sh, m_device->texture_width(picture),
                         m_device->texture_height(picture), aspect, &x, &y, &w, &h);
            /* The blit's destination has to be inside the backbuffer.
             *
             * display.fit = integer takes one whole multiple of the frame
             * even when the frame is larger than the window, which is stated
             * in present_rect and is the behaviour the setting asks for; it
             * is also a destination rectangle that runs off the backbuffer.
             * vkCmdBlitImage and the D3D12 blit are both undefined for a
             * region outside the image, so the rectangle is cut to the
             * surface here. The picture is cropped rather than rescaled,
             * which is what "one whole multiple" already means when the
             * window is too small, and the cut is said once with the
             * numbers. */
            if (x < 0 || y < 0 || (uint32_t)x + w > sw || (uint32_t)y + h > sh) {
                if (!m_logged_present_clip) {
                    m_logged_present_clip = true;
                    rt_log_warn("gsr", "the present rectangle %d,%d %ux%u runs outside the "
                                       "%ux%u backbuffer and is cut to it; a blit outside the "
                                       "image is undefined. display.fit = integer on a window "
                                       "smaller than the frame is what reaches this.",
                                x, y, w, h, sw, sh);
                }
                if (x < 0) {
                    const uint32_t cut = (uint32_t)(-(int64_t)x);
                    w = cut < w ? w - cut : 0u;
                    x = 0;
                }
                if (y < 0) {
                    const uint32_t cut = (uint32_t)(-(int64_t)y);
                    h = cut < h ? h - cut : 0u;
                    y = 0;
                }
                if ((uint32_t)x >= sw || (uint32_t)y >= sh) {
                    w = 0;
                    h = 0;
                } else {
                    if ((uint32_t)x + w > sw) w = sw - (uint32_t)x;
                    if ((uint32_t)y + h > sh) h = sh - (uint32_t)y;
                }
            }
            /* The bars around a letterboxed picture have to be cleared, or
             * the previous frame's edges stay on screen. */
            cmd->begin_render_pass(backbuffer, true, 0.0f, 0.0f, 0.0f, 1.0f);
            cmd->end_render_pass();
            /* A zero-extent blit is as undefined as one outside the image, so
             * the cleared backbuffer is what a picture with nothing left of
             * it presents. */
            if (w != 0 && h != 0) {
                cmd->blit_texture(backbuffer, x, y, w, h, picture,
                                  m_present.filter == RT_PGS_FILTER_LINEAR);
            }
            m_present_x = x;
            m_present_y = y;
            m_present_w = (int32_t)w;
            m_present_h = (int32_t)h;
        } else {
            cmd->begin_render_pass(backbuffer, true, 0.0f, 0.0f, 0.0f, 1.0f);
            cmd->end_render_pass();
            m_present_w = 0;
            m_present_h = 0;
        }

        /* The user-facing capture, taken here and not after the overlay: this
         * is the picture as the game drew it, with the menu, the pointer and
         * the fps readout not yet on it. The whole backbuffer is copied and
         * the present rectangle is cropped out on the way back
         * (take_screenshot), because a region copy would need the RHI to
         * carry an offset it has no other user for. A field with no picture
         * does not consume the arm. */
        if (m_shot_slots != 0 && picture) capture_shot(cmd, backbuffer, RT_PGS_SHOT_PRE);

        /* Stage (c): the backbuffer as the blit left it, then again as the
         * overlay left it. Two copies rather than one, because a picture the
         * overlay covers and a picture the blit never wrote are the same
         * black window and different faults. */

        draw_overlay(cmd, backbuffer, sw, sh);


        /* The post slot exists to settle one question by measurement: with
         * the menu closed the two files must be byte identical, and with it
         * open they must differ, which is what proves the pre copy is taken
         * where this comment says it is. */
        if (m_shot_slots == RT_PGS_SHOT_SLOTS && picture) {
            capture_shot(cmd, backbuffer, RT_PGS_SHOT_POST);
        }
        if (m_shot_slots != 0 && picture) m_shot_slots = 0;

        cmd->texture_barrier(backbuffer, rhi::Stage::Graphics, rhi::Access::Write,
                             rhi::Stage::Present, rhi::Access::Read);
    }

    /* One backbuffer copy into a slot of the readback buffer. */
    void capture_shot(rhi::CommandList* cmd, rhi::Texture* backbuffer, uint32_t slot) {
        const uint32_t sw = m_device->texture_width(backbuffer);
        const uint32_t sh = m_device->texture_height(backbuffer);
        const uint64_t slot_bytes = (uint64_t)sw * sh * 4;
        const uint64_t need = slot_bytes * RT_PGS_SHOT_SLOTS;
        if (!m_shot_buffer || m_device->buffer_size(m_shot_buffer) < need) {
            if (m_shot_buffer) {
                m_device->wait_idle();
                m_device->destroy_buffer(m_shot_buffer);
            }
            rhi::BufferDesc bd;
            bd.size = need;
            bd.kind = rhi::BufferKind::Readback;
            bd.usage = rhi::BufferUsage::CopyDst;
            bd.debug_name = "screenshot readback";
            m_shot_buffer = m_device->create_buffer(bd);
        }
        cmd->copy_texture_to_buffer(m_shot_buffer, slot_bytes * slot, backbuffer);
        Shot& s = m_shot[slot];
        s.buffer_offset = slot_bytes * slot;
        s.surface_width = sw;
        s.origin_x = (uint32_t)(m_present_x < 0 ? 0 : m_present_x);
        s.origin_y = (uint32_t)(m_present_y < 0 ? 0 : m_present_y);
        s.width = (uint32_t)(m_present_w < 0 ? 0 : m_present_w);
        s.height = (uint32_t)(m_present_h < 0 ? 0 : m_present_h);
        /* Cleared, not carried over: note_shot_submit fills it in with the
         * value of the submit that carries this copy, and drain_screenshots
         * waits on that one rather than on whatever an earlier present left
         * behind. */
        s.timeline = 0;
        s.pending = s.width != 0 && s.height != 0;
        if (!s.pending && !m_logged_empty_shot) {
            /* record_present consumes the arm either way, so the user pressed
             * the key and no file will ever appear. */
            m_logged_empty_shot = true;
            rt_log_warn("gsr", "a screenshot was captured on a field whose present "
                               "rectangle is %dx%d; the capture is dropped",
                        m_present_w, m_present_h);
        }
    }

    void ensure_overlay_pipelines(rhi::Texture* backbuffer) {
        if (m_overlay_pipeline) return;
        const rhi::ShaderBlob vs = rhi::shader_overlay_vert();
        const rhi::ShaderBlob fs = rhi::shader_overlay_frag();
        rhi::GraphicsPipelineDesc gd;
        gd.vertex_spirv = vs.words;
        gd.vertex_spirv_words = vs.word_count;
        gd.fragment_spirv = fs.words;
        gd.fragment_spirv_words = fs.word_count;
        /* The pipeline's colour format has to match the attachment, and the
         * swapchain picked that format. */
        gd.color_format = m_device->texture_format(backbuffer);
        gd.blend = true;
        gd.premultiplied = true;
        gd.debug_name = "overlay (premultiplied)";
        m_overlay_pipeline = m_device->create_graphics_pipeline(gd);
        gd.premultiplied = false;
        gd.debug_name = "overlay (straight alpha)";
        m_overlay_pipeline_straight = m_device->create_graphics_pipeline(gd);
    }

    void draw_overlay(rhi::CommandList* cmd, rhi::Texture* backbuffer,
                      uint32_t sw, uint32_t sh) {
        if (!m_overlay.valid) return;
        ensure_overlay_pipelines(backbuffer);
        upload_overlay();

        const float surface_w = m_overlay.surface_width ? float(m_overlay.surface_width)
                                                        : float(sw);
        const float surface_h = m_overlay.surface_height ? float(m_overlay.surface_height)
                                                         : float(sh);

        /* This command list reads the slot upload_overlay just settled on;
         * note_overlay_submit ties it to the submit that carries it. */
        m_overlay_drawn = true;
        cmd->begin_render_pass(backbuffer, false, 0.0f, 0.0f, 0.0f, 0.0f);
        cmd->bind_vertex_buffer(m_overlay_buf[m_overlay_slot].vertices, 0);
        cmd->bind_index_buffer(m_overlay_buf[m_overlay_slot].indices, 0);
        for (const RtPgsOverlayCmd& c : m_overlay.cmds) {
            if (c.index_count == 0) continue;
            const bool premultiplied = (c.flags & RT_PGS_OVERLAY_PREMULTIPLIED) != 0;
            cmd->bind_graphics_pipeline(premultiplied ? m_overlay_pipeline
                                                      : m_overlay_pipeline_straight);
            cmd->set_viewport(0.0f, 0.0f, float(sw), float(sh));
            if (c.flags & RT_PGS_OVERLAY_SCISSOR) {
                const int32_t sx = c.scissor_x < 0 ? 0 : c.scissor_x;
                const int32_t sy = c.scissor_y < 0 ? 0 : c.scissor_y;
                const int32_t sw_i = c.scissor_w < 0 ? 0 : c.scissor_w;
                const int32_t sh_i = c.scissor_h < 0 ? 0 : c.scissor_h;
                cmd->set_scissor(sx, sy, (uint32_t)sw_i, (uint32_t)sh_i);
            } else {
                cmd->set_scissor(0, 0, sw, sh);
            }

            OverlayPush push{};
            if (c.flags & RT_PGS_OVERLAY_TRANSFORM) {
                std::memcpy(push.transform, c.transform, sizeof(push.transform));
                push.use_transform = 1;
            } else {
                for (int i = 0; i < 16; ++i) push.transform[i] = (i % 5) == 0 ? 1.0f : 0.0f;
                push.use_transform = 0;
            }
            push.surface_w = surface_w;
            push.surface_h = surface_h;
            push.translate_x = c.translate_x;
            push.translate_y = c.translate_y;
            push.use_texture = c.texture != 0 ? 1 : 0;
            cmd->push_constants(&push, sizeof(push));

            auto it = m_overlay_textures.find(c.texture);
            cmd->bind_texture(0, it != m_overlay_textures.end() ? it->second
                                                                : nullptr);
            cmd->draw_indexed(c.index_count, c.index_offset, c.vertex_offset);
        }
        cmd->end_render_pass();
    }

    /* Uploads the retained overlay frame into the slot the current draw is
     * not reading.
     *
     * The geometry buffers are host visible and the draw that reads them is
     * still in flight when the next UI frame arrives. This used to wait for
     * the whole device to go idle before every upload, which stalled the GS
     * worker on each field the menu was open. One set of buffers per frame in
     * flight instead: a new frame is written into the next set round, which
     * is one no draw still in flight can be reading, and each slot records
     * the timeline value of the submit that last read it, so the wait is on
     * that one submit and not on the device. By the time a slot comes round
     * again that submit is a whole round of presents old and the wait returns
     * without blocking. The one wait that can still block is on
     * growth, where the buffer being freed is the one in flight.
     *
     * Retained-frame semantics are unchanged: m_overlay_uploaded is cleared
     * only by overlay_set_frame, so a frame is uploaded once and then drawn
     * from its slot on every field until it is replaced. */
    void upload_overlay() {
        if (m_overlay_uploaded) return;
        const uint32_t slot = (m_overlay_slot + 1u) % (uint32_t)m_overlay_buf.size();
        OverlayBuffers& b = m_overlay_buf[slot];
        const uint64_t vbytes = m_overlay.vertices.size() * sizeof(RtPgsOverlayVertex);
        const uint64_t ibytes = m_overlay.indices.size() * sizeof(uint32_t);
        ensure_overlay_buffer(&b.vertices, vbytes, rhi::BufferUsage::Vertex,
                              "overlay vertices", b.last_submit);
        ensure_overlay_buffer(&b.indices, ibytes, rhi::BufferUsage::Index,
                              "overlay indices", b.last_submit);
        m_device->wait(b.last_submit);
        if (vbytes) {
            std::memcpy(m_device->map(b.vertices), m_overlay.vertices.data(), (size_t)vbytes);
        }
        if (ibytes) {
            std::memcpy(m_device->map(b.indices), m_overlay.indices.data(), (size_t)ibytes);
        }
        m_overlay_slot = slot;
        m_overlay_uploaded = true;
    }

    /* Grows one slot's buffer to hold `bytes`, at twice the largest frame
     * that slot has seen so a frame that grows a little does not reallocate
     * on every upload. Freeing the old buffer is the one place this path
     * waits, and it waits on the submit that last read this slot rather
     * than on the device. */
    void ensure_overlay_buffer(rhi::Buffer** buf, uint64_t bytes, rhi::BufferUsage usage,
                               const char* name, uint64_t last_submit) {
        if (*buf && m_device->buffer_size(*buf) >= bytes) return;
        if (*buf) {
            m_device->wait(last_submit);
            m_device->destroy_buffer(*buf);
            *buf = nullptr;
        }
        rhi::BufferDesc bd;
        /* Never zero: a valid frame that carries no geometry would otherwise
         * ask for a zero-byte buffer, which no backend accepts. */
        const uint64_t want = bytes * 2;
        bd.size = want > 4096 ? want : 4096;
        bd.kind = rhi::BufferKind::Upload;
        bd.usage = usage;
        bd.debug_name = name;
        *buf = m_device->create_buffer(bd);
    }

    /* Records which submit read the overlay slot, so the next upload waits
     * on that one value. Called right after every submit whose command list
     * may have carried a draw_overlay. */
    void note_overlay_submit(uint64_t value) {
        if (!m_overlay_drawn) return;
        m_overlay_drawn = false;
        m_overlay_buf[m_overlay_slot].last_submit = value;
    }

    /* Records which submit carries each backbuffer copy this command list
     * holds, so drain_screenshots waits on that one value. Called right after
     * every submit of a list record_present may have written a capture into;
     * a slot that already has a value belongs to an earlier present that has
     * not been drained yet. */
    void note_shot_submit(uint64_t value) {
        for (Shot& s : m_shot) {
            if (s.pending && s.timeline == 0) s.timeline = value;
        }
    }

    /* The CRTC reads only the four colour formats; anything else is named
     * once rather than silently read as PSMCT32 by the shader. */
    void check_display_psm(const ScanoutPlan& plan) {
        const uint32_t psms[2] = { plan.push.c1_psm, plan.push.c2_psm };
        const uint32_t enabled[2] = { plan.push.c1_enable, plan.push.c2_enable };
        for (int i = 0; i < 2; ++i) {
            if (!enabled[i]) continue;
            const uint32_t psm = psms[i];
            if (psm == GS_PSMCT32 || psm == GS_PSMCT24 || psm == GS_PSMCT16
                || psm == GS_PSMCT16S) {
                continue;
            }
            if (m_logged_psm == psm) continue;
            m_logged_psm = psm;
            rt_log_warn("gsr", "DISPFB%d PSM 0x%02x is not one of the four the CRTC reads; "
                               "it is read as PSMCT32 and the picture will be wrong",
                        i + 1, psm);
        }
    }


    /* ---- present-path diagnostics, removed 2026-09-05 ---------------------
     *
     * What used to be here: three content measures per sampled field (the
     * words of device local memory the CRTC was about to read, the pixels of
     * the scanout image after its dispatch, and the pixels of the backbuffer
     * before and after the overlay), plus a scanout probe that overwrote the
     * frame with a gradient, plus one-shot probes over the first palettised
     * batch and the first primitives. They were written on 2026-09-04 to
     * localise a renderer that presented an entirely black picture, and they
     * did that.
     *
     * Why they are gone. The sampling ran on the first three picture fields
     * and then one field in 64 for the rest of the run, and each sampled
     * field cost a full device stall and about 5 MB of readback. That is a
     * permanent 1-in-64 stall in a renderer whose next milestone is a parity
     * gate that includes timing, so the diagnostic was noise in the very
     * measurement it would be judged by. Dev-only code does not get to stall
     * the GPU unconditionally.
     *
     * What replaces them. Nothing standing. If the black-picture question
     * returns, the measurement is written again against that run, fired from
     * the condition being investigated and removed with it. The cross check
     * went with the sampling because it had no cheap trigger of its own: it
     * compared a black scanout image against the device's local memory, and
     * both halves of that comparison are readbacks.
     *
     * What stays, because it costs an integer increment when idle: the VRAM
     * reconcile counters (m_vram_syncs and friends), the block ownership
     * tracker in gsr::LocalMemory, and the per-field timing report_stats
     * prints. */

    rhi::Buffer* ensure_vram_readback() {
        if (!m_vram_readback) {
            rhi::BufferDesc bd;
            bd.size = GS_VRAM_BYTES;
            bd.kind = rhi::BufferKind::Readback;
            bd.usage = rhi::BufferUsage::CopyDst;
            bd.debug_name = "gs local memory readback";
            m_vram_readback = m_device->create_buffer(bd);
        }
        return m_vram_readback;
    }

    rhi::Device* m_device = nullptr;
    /* A copy of what the device reported when it was created; see
     * rhi::Limits and the read in the constructor. */
    rhi::Limits m_limits;
    rhi::Buffer* m_vram = nullptr;
    rhi::Buffer* m_vram_staging = nullptr;
    rhi::Texture* m_frame = nullptr;
    rhi::ComputePipeline* m_scanout = nullptr;
    rhi::GraphicsPipeline* m_overlay_pipeline = nullptr;
    rhi::GraphicsPipeline* m_overlay_pipeline_straight = nullptr;
    /* One set of overlay geometry buffers per frame the backend keeps in
     * flight (rhi::Limits::frames_in_flight, never fewer than two).
     * m_overlay_slot is the set the retained frame lives in and every draw
     * binds; upload_overlay writes the next one round. last_submit is the
     * timeline value of the submit that last read that set, which is what
     * the next upload waits on instead of waiting for the device to go idle.
     * Sized from the device rather than fixed at two so that a backend which
     * ever keeps three frames in flight does not turn every upload into a
     * wait on a draw that is still reading. */
    struct OverlayBuffers {
        rhi::Buffer* vertices = nullptr;
        rhi::Buffer* indices = nullptr;
        uint64_t last_submit = 0;
    };
    std::vector<OverlayBuffers> m_overlay_buf;
    uint32_t m_overlay_slot = 0;
    bool m_overlay_drawn = false;

    rhi::ComputePipeline* m_raster = nullptr;
    rhi::Buffer* m_prim_buffer = nullptr;
    rhi::Buffer* m_binidx_buffer = nullptr;
    rhi::Buffer* m_binrng_buffer = nullptr;
    rhi::Buffer* m_clut_buffer = nullptr;
    rhi::Buffer* m_vram_readback = nullptr;

    std::vector<BlockTarget> m_draw_targets;
    std::vector<BlockTarget> m_xfer_targets;

    /* Render scale: the shadow, the page list a seed pass walks, and the
     * previous field the adaptive deinterlacer compares against. */
    rhi::ComputePipeline* m_shadow_pipe = nullptr;
    rhi::Buffer* m_shadow_buffer = nullptr;
    rhi::Buffer* m_seed_buffer = nullptr;
    rhi::Buffer* m_history_buffer = nullptr;
    ShadowPages m_shadow;
    std::vector<uint32_t> m_seed_pages;
    int m_logged_hires = -1;
    bool m_history_fresh = true;

    LocalMemory m_mem;
    TransferEngine m_xfer;
    RegisterFile m_regs;
    GifDecodeState m_gif[3];
    ClutCache m_clut;
    DrawEngine m_draw{m_regs};

    /* The words the rasteriser wrote that the host store has not read back
     * yet, as [first, last). Empty when first >= last. */
    uint64_t m_vram_sync_runs = 0;
    uint64_t m_vram_sync_blocks = 0;
    uint64_t m_uploads = 0;
    uint64_t m_upload_runs = 0;
    uint64_t m_upload_blocks = 0;
    /* The range of the armed transfer, kept between the TRXDIR write and the
     * image data that follows it. */
    uint32_t m_xfer_first = 0;
    uint32_t m_xfer_last = 0;
    uint64_t m_dispatches = 0;
    uint64_t m_vram_syncs = 0;
    uint64_t m_vram_sync_words = 0;
    uint64_t m_texflushes = 0;
    bool m_logged_fz_overlap = false;
    /* Work that was dropped rather than done. Each has a warn line said once
     * at the site; these are the run totals report_stats prints, so a log
     * whose one warn line is easy to miss still ends with the size of it.
     * m_logged_bad_path holds the path value already reported, and 0 is never
     * a bad one, so it doubles as "nothing said yet". */
    uint64_t m_gif_bad_path = 0;
    uint64_t m_gif_empty = 0;
    uint64_t m_empty_batches = 0;
    int m_logged_bad_path = 0;
    bool m_logged_empty_gif = false;
    bool m_logged_empty_batch = false;
    bool m_logged_shadow_alloc = false;
    bool m_logged_no_resolve = false;
    bool m_logged_empty_shot = false;
    /* Said once each: a dispatch grid over the device's limit, a scanout slot
     * with no buffer behind it, a CRTC frame this renderer cannot build, and
     * a present rectangle cut to the backbuffer. Each of the four stands for
     * a picture that is wrong from this field on, so one line is enough and
     * one per field is not. */
    bool m_logged_group_count = false;
    bool m_logged_scanout_bind = false;
    bool m_logged_frame_size = false;
    bool m_logged_present_clip = false;

    Presentation m_present;
    OverlayFrame m_overlay;
    bool m_overlay_uploaded = false;
    std::unordered_map<uint32_t, rhi::Texture*> m_overlay_textures;
    uint32_t m_next_texture_id = 0;

    /* Decoder notes already said, so each is said once. */
    std::unordered_map<std::string, bool> m_notes;

    /* One armed capture of the presented picture, cropped out of a whole
     * backbuffer copy. Three pieces of state with two owners, the same split
     * the paraLLEl-GS backend has (gs_parallel_impl.h):
     *
     *   m_shot_slots   the arm. Consumer-thread only: request_screenshot
     *                  rides the host's GS command ring, so it is set on the
     *                  thread record_present runs on. 0 means nothing armed.
     *   m_shot[]       the in-flight copy: where in m_shot_buffer the
     *                  backbuffer went, the rectangle to crop out of it, and
     *                  the timeline value of the submit that carries the
     *                  copy. Consumer-thread only, written by capture_shot
     *                  and cleared by drain_screenshots.
     *   m_shot_ready[] the published pixels, tightly packed RGBA8 rows from
     *                  the top. Written by drain_screenshots on the consumer
     *                  thread and read by take_screenshot on the host's EE
     *                  thread, so both sides take m_shot_mu: a reader needs
     *                  one whole image with its own size, not a size from
     *                  one capture and rows from another. */
    struct Shot {
        uint64_t buffer_offset = 0;
        uint64_t timeline = 0;       /* the submit carrying the copy, 0 until it is made */
        uint32_t surface_width = 0;  /* the copy's row stride, in pixels */
        uint32_t origin_x = 0, origin_y = 0;
        uint32_t width = 0, height = 0;
        bool pending = false;
    };
    struct ShotReady {
        std::vector<uint8_t> rgba;
        uint32_t width = 0, height = 0;
    };
    Shot m_shot[RT_PGS_SHOT_SLOTS];
    mutable std::mutex m_shot_mu;
    ShotReady m_shot_ready[RT_PGS_SHOT_SLOTS];
    rhi::Buffer* m_shot_buffer = nullptr;
    uint32_t m_shot_slots = 0;

    int32_t m_present_x = 0, m_present_y = 0, m_present_w = 0, m_present_h = 0;
    double m_last_aspect = 0.0;
    /* display.widescreen's presentation half, pushed through the ring by
     * set_widescreen_aspect. 0 means off. */
    double m_widescreen_aspect = 0.0;

    /* Latched-field state. m_frame_serial counts fields that produced a
     * picture; present_pump presents whenever it is ahead of what is on
     * screen, and repeats the same one on the interval when it is not. */
    bool m_have_frame = false;
    uint64_t m_frame_serial = 0;
    uint64_t m_presented_serial = 0;
    PumpClock::time_point m_last_present = PumpClock::now();
    double m_present_rate = 0.0;

    /* Present-path timings, cleared by present_timings(). */
    uint64_t m_scanout_ns = 0;
    uint64_t m_present_ns = 0;
    uint64_t m_fields_since_read = 0;
    uint64_t m_presents = 0;
    uint64_t m_repeats = 0;
    /* Run totals, kept apart from the window counters above because
     * present_timings() clears those and report_stats runs after it. */
    uint64_t m_presents_run = 0;
    uint64_t m_repeats_run = 0;

    uint64_t m_fields = 0;
    uint64_t m_reg_writes = 0;
    bool m_transfer_since_vsync = false;
    bool m_logged_no_ui_window = false;
    bool m_logged_no_pump_window = false;
    bool m_logged_no_frame = false;
    bool m_logged_zero_size = false;
    /* window_closed is polled once per field, so the line naming the cause
     * of the end of the run is latched to the first true. */
    bool m_logged_closed = false;
    /* The backbuffer outage: m_logged_acquire is "an outage is open", the
     * first counter is this outage's skipped presents and the second the
     * run's. See note_acquire_failed. */
    bool m_logged_acquire = false;
    uint64_t m_acquire_skipped = 0;
    uint64_t m_acquire_skipped_run = 0;
    /* The RHI backend this device was created on, as the name ICORECOMP_GS_BACKEND
     * spells it. A string literal from rhi_backend_name, so it outlives
     * everything that reads it. */
    const char* m_backend_name = "vulkan";
    bool m_logged_screenshot = false;
    uint32_t m_logged_psm = 0xFFFFFFFFu;
};

} // namespace

GsBackend* rt_gs_make_native_backend(RtNativeRhi which, uint32_t present_mode) {
    /* Windowed when the executable's window service has a window; see the
     * file comment on windowing. */
    return new NativeBackend(which, present_mode, false);
}

int rt_gs_native_replay(const char* dump_path, const char* screenshot_path, int verbose) {
    /* Headless: the replay tool has no window and its whole point is the
     * picture the GS produced, which is read back out of the scanout image
     * rather than presented.
     *
     * Vulkan by default, because it is the one backend built on every
     * platform this tool runs on. ICORECOMP_GS_BACKEND names another, in the
     * same spelling ICORECOMP_GS_BACKEND uses, so the cross-backend CI job can run
     * the same dump through D3D12 and compare the two pictures byte for byte.
     * A backend this build does not have is a fatal naming it, not a quiet
     * run on Vulkan: the comparison would otherwise pass by comparing Vulkan
     * against Vulkan. */
    RtNativeRhi which = RtNativeRhi::Vulkan;
    if (const char* env = std::getenv("ICORECOMP_GS_BACKEND"); env && *env) {
        if (std::strcmp(env, "vulkan") == 0) {
            which = RtNativeRhi::Vulkan;
        } else if (std::strcmp(env, "d3d12") == 0) {
#if defined(ICORECOMP_RHI_D3D12)
            which = RtNativeRhi::D3D12;
#else
            rt_fatal("gsr", nullptr, "ICORECOMP_GS_BACKEND=d3d12 and this build has no "
                                     "D3D12 backend");
#endif
        } else if (std::strcmp(env, "metal") == 0) {
#if defined(ICORECOMP_RHI_METAL)
            which = RtNativeRhi::Metal;
#else
            rt_fatal("gsr", nullptr, "ICORECOMP_GS_BACKEND=metal and this build has no "
                                     "Metal backend");
#endif
        } else {
            rt_fatal("gsr", nullptr, "unknown ICORECOMP_GS_BACKEND=%s for the replay tool "
                                     "(expected vulkan, d3d12 or metal)", env);
        }
        rt_log_info("gsr", "replay on the %s backend (ICORECOMP_GS_BACKEND)", env);
    }
    NativeBackend* be = new NativeBackend(which, RT_PGS_PRESENT_MAILBOX, true);

    struct Replay final : gsr::DumpSink {
        NativeBackend* be;
        int verbose;
        uint64_t fields = 0;
        uint64_t transfers = 0;
        explicit Replay(NativeBackend* b, int v) : be(b), verbose(v) {}

        void gif(int path, const uint8_t* data, uint32_t qwords) override {
            ++transfers;
            be->submit_gif(path, data, qwords);
        }
        void vsync(unsigned field) override {
            ++fields;
            be->vsync(field);
            if (verbose) {
                std::fprintf(stderr, "field %llu (parity %u)\n",
                             (unsigned long long)fields, field);
            }
        }
        void priv_snapshot(const uint64_t* lo, const uint64_t* hi) override {
            /* The snapshot's own layout: index (off >> 4) * 2 inside a bank. */
            for (uint32_t off = 0; off < 0x1000; off += 16) {
                be->write_priv(off, lo[(off >> 4) * 2]);
                be->write_priv(0x1000 + off, hi[(off >> 4) * 2]);
            }
        }
    } sink(be, verbose);

    char error[256] = {};
    uint64_t packets = 0;
    const bool ok = gsr::dump_parse(dump_path, sink, error, sizeof(error), &packets);
    std::fprintf(stderr, "gs-replay (native): %llu packets, %llu GIF transfers, %llu fields\n",
                 (unsigned long long)packets, (unsigned long long)sink.transfers,
                 (unsigned long long)sink.fields);
    if (!ok) std::fprintf(stderr, "gs-replay (native): %s\n", error);

    int status = ok ? 0 : 1;
    if (screenshot_path) {
        if (!be->write_scanout_ppm(screenshot_path)) {
            std::fprintf(stderr, "gs-replay (native): could not write %s\n", screenshot_path);
            status = 1;
        } else {
            std::fprintf(stderr, "gs-replay (native): wrote %s\n", screenshot_path);
        }
    }
    be->report_stats();
    delete be;
    return status;
}
