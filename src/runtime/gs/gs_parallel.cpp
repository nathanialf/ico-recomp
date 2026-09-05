/* gs/gs_parallel.cpp: live GS backend adapter over libicorecomp-parallel-gs.
 *
 * Thin GsBackend implementation that forwards to the shared library's C ABI
 * (gs_parallel_api.h). Everything Granite/paraLLEl-GS lives inside the
 * library (gs_parallel_lib.cpp); this file must stay free of their headers
 * and classes: it is the MIT side of the LGPL boundary.
 *
 * Presentation policy (window vs headless, screenshots, validation layers)
 * is decided library-side at rt_pgs_create; see gs_parallel_lib.cpp. The
 * one policy this side owns is window closure: the library records it and
 * the runtime exits cleanly from hw/gspriv.cpp's field boundary, so process
 * teardown (atexit backend stats, device wait-idle) stays with the host and
 * on the EE thread. It used to exit from inside vsync() here, which stopped
 * being possible when vsync() started running on the command ring's worker
 * thread (gs/gs_threaded.cpp).
 *
 * Thread note: every method below can run on that worker thread. Nothing
 * here holds state of its own beyond m_pgs, and the library's own
 * thread rules are documented per call in gs_parallel_api.h.
 */
#include "gs_backend.h"

#ifdef ICORECOMP_HAVE_PARALLEL_GS

#include "gs_parallel_api.h"
#include "gs_probe_api.h"
#include "runtime.h"

#include "../host/settings.h"
#include "../host/window.h"
#include "../host/window_service.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/* Packs R,G,B,A (0-255 each) into RtPgsOverlayVertex::rgba (R8G8B8A8_UNORM
 * byte order, straight alpha; see gs_parallel_api.h). */
uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

/* Manual GPU test for milestone 4 (overlay render path): builds and submits
 * one static RtPgsOverlayFrame proving every mechanism the ABI added --
 * alpha-blended overlap, a texture, a scissor rect, and a transform -- so a
 * human with a GPU and eyes can confirm the pass actually draws. Gated by
 * ICORECOMP_UI_TEST=1, costs nothing when unset, removed by nobody: this is
 * the only exercise of the overlay ABI until RmlUi replaces it (milestone
 * 5). Laid out for a 640x480 surface, the historic default window size.
 */
void run_overlay_ui_test(RtPgs* pgs) {
    std::vector<RtPgsOverlayVertex> verts;
    std::vector<uint32_t> indices;
    std::vector<RtPgsOverlayCmd> cmds;

    /* Appends one quad (4 verts, 2 triangles) and returns a cmd already
     * pointed at it via vertex_offset/index_offset/index_count; callers set
     * texture/flags/translate/scissor/transform and push_back it. */
    auto add_quad = [&](float x, float y, float w, float h, uint32_t rgba) {
        RtPgsOverlayCmd cmd = {};
        cmd.vertex_offset = int32_t(verts.size());
        cmd.index_offset = uint32_t(indices.size());
        cmd.index_count = 6;
        verts.push_back({ x,     y,     0.0f, 0.0f, rgba });
        verts.push_back({ x + w, y,     1.0f, 0.0f, rgba });
        verts.push_back({ x + w, y + h, 1.0f, 1.0f, rgba });
        verts.push_back({ x,     y + h, 0.0f, 1.0f, rgba });
        for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u }) indices.push_back(i);
        return cmd;
    };

    /* (a) two overlapping 50%-alpha colored quads: proves straight-alpha
     * blending (SRC_ALPHA/ONE_MINUS_SRC_ALPHA) and draw order. */
    cmds.push_back(add_quad(40, 40, 120, 120, pack_rgba(255, 0, 0, 128)));
    cmds.push_back(add_quad(100, 100, 120, 120, pack_rgba(0, 128, 255, 128)));

    /* (b) one quad textured with a generated 8x8 black/white checkerboard:
     * proves rt_pgs_overlay_texture_create and set_texture. */
    uint8_t checker[8 * 8 * 4];
    for (unsigned ty = 0; ty < 8; ++ty) {
        for (unsigned tx = 0; tx < 8; ++tx) {
            const uint8_t v = ((tx ^ ty) & 1u) ? 255 : 0;
            uint8_t* px = &checker[(ty * 8 + tx) * 4];
            px[0] = v; px[1] = v; px[2] = v; px[3] = 255;
        }
    }
    const uint32_t checker_tex = rt_pgs_overlay_texture_create(pgs, checker, 8, 8);
    if (!checker_tex) {
        rt_log_warn("gs", "ICORECOMP_UI_TEST: checkerboard texture create failed");
    }
    {
        RtPgsOverlayCmd c = add_quad(300, 40, 128, 128, pack_rgba(255, 255, 255, 255));
        c.texture = checker_tex;
        cmds.push_back(c);
    }

    /* (c) one quad with RT_PGS_OVERLAY_SCISSOR cutting it in half: proves
     * scissor scaling/clamping (draw_overlay). */
    {
        RtPgsOverlayCmd c = add_quad(40, 300, 160, 100, pack_rgba(0, 255, 0, 255));
        c.flags |= RT_PGS_OVERLAY_SCISSOR;
        c.scissor_x = 40;
        c.scissor_y = 300;
        c.scissor_w = 80; /* left half of the 160-wide quad only */
        c.scissor_h = 100;
        cmds.push_back(c);
    }

    /* (d) one quad with RT_PGS_OVERLAY_TRANSFORM rotating ~15 degrees about
     * its own center, then placed on the surface via translate: proves the
     * transform path and that translate composes with it. */
    {
        RtPgsOverlayCmd c = add_quad(-40, -40, 80, 80, pack_rgba(255, 200, 0, 255));
        c.flags |= RT_PGS_OVERLAY_TRANSFORM;
        const float angle = 15.0f * 3.14159265f / 180.0f;
        const float cs = std::cos(angle), sn = std::sin(angle);
        const float t[16] = {
              cs,  sn, 0.0f, 0.0f,
             -sn,  cs, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
        std::memcpy(c.transform, t, sizeof(t));
        c.translate_x = 460.0f;
        c.translate_y = 400.0f;
        cmds.push_back(c);
    }

    RtPgsOverlayFrame frame = {};
    frame.vertices = verts.data();
    frame.vertex_count = uint32_t(verts.size());
    frame.indices = indices.data();
    frame.index_count = uint32_t(indices.size());
    frame.cmds = cmds.data();
    frame.cmd_count = uint32_t(cmds.size());
    frame.surface_width = 640;
    frame.surface_height = 480;

    rt_pgs_overlay_set_frame(pgs, &frame);
    rt_log_info("gs", "ICORECOMP_UI_TEST=1: static overlay test frame submitted (%u cmds, texture id %u)",
           frame.cmd_count, checker_tex);
}

/* The GS library reaches the log through this one callback, and the ABI
 * gives the callback no level parameter (RtPgsHost in gs_parallel_api.h).
 * The library says the level in the message instead: a line from its warnf
 * or errorf starts with one of the two tags below, which this strips before
 * logging at that level. The tags are literals in gs_parallel_impl.h
 * (RT_PGS_LOG_TAG_WARN / RT_PGS_LOG_TAG_ERROR); they are repeated here
 * because that header is library-private and this file is the MIT side of
 * the LGPL boundary, which may not include it. A mismatch prints the tag
 * inline at whatever level the words below pick, which keeps the line
 * readable rather than losing it.
 *
 * Everything untagged is startup identity, per-change notes and summaries,
 * so info is the right default for those, with one exception: the word list
 * below still promotes a failure line that predates the tags or comes from
 * a message this repo does not own. Substring matching rather than a table
 * of exact strings, for the same reason it always was. */
const char* const kPgsTagWarn = "[warn] ";
const char* const kPgsTagError = "[error] ";

bool pgs_message_is_warning(const char* message) {
    static const char* const kWords[] = {"failed", "refused", "missing", "unsupported", "fallback"};
    for (const char* w : kWords) {
        if (std::strstr(message, w) != nullptr) return true;
    }
    return false;
}

void host_log(const char* component, const char* message) {
    if (std::strncmp(message, kPgsTagError, std::strlen(kPgsTagError)) == 0) {
        rt_log_error(component, "%s", message + std::strlen(kPgsTagError));
    } else if (std::strncmp(message, kPgsTagWarn, std::strlen(kPgsTagWarn)) == 0) {
        rt_log_warn(component, "%s", message + std::strlen(kPgsTagWarn));
    } else if (pgs_message_is_warning(message)) {
        rt_log_warn(component, "%s", message);
    } else {
        rt_log_info(component, "%s", message);
    }
}

void host_fatal(const char* component, const char* message) {
    rt_fatal(component, nullptr, "%s", message);
}

/* Event pump inversion (shim 3): the exe owns the only SDL_PollEvent loop.
 * Called from inside Granite's WSI::begin_frame (see gs_parallel_present.cpp's
 * RtPgs::present_frame); rt_window_pump honors that reentrancy contract
 * (queue/translate events, notify_quit/notify_resize only). */
void host_pump_events() {
    rt_window_pump();
}

/* The window belongs to the executable, so the surface made from it does
 * too: this hands the library's WSI straight to
 * host/window_service.cpp's rt_window_create_vulkan_surface. Both handles
 * travel as uint64_t so that no Vulkan type crosses the C ABI. Called once,
 * on the creating thread, from inside device creation. */
uint64_t host_create_vulkan_surface(uint64_t vk_instance) {
    return rt_window_create_vulkan_surface(vk_instance);
}

/* The window service's sink (host/window_service.h). Every one of these runs
 * on the main thread from inside the event pump, which is exactly the
 * context the three library entry points below are documented as safe from
 * (gs_parallel_api.h: they set a flag or refresh the library's own size
 * cache, and none of them is a GS command). This is the whole of what the
 * paraLLEl-GS backend needs to hear from the window. */
void sink_quit(void* user) { rt_pgs_notify_quit((RtPgs*)user); }
void sink_resize(void* user) { rt_pgs_notify_resize((RtPgs*)user); }
void sink_sample(void* user) { rt_pgs_sample_window_state((RtPgs*)user); }

/* The Display tab's two read-only lines, built once from the device this
 * instance actually created (rt_pgs_live_probe) rather than from a probe
 * device made and destroyed at startup. */
void publish_device_info(RtPgs* pgs) {
    RtPgsProbe p = {};
    if (!rt_pgs_live_probe(pgs, &p) || !p.have_device) {
        rt_window_set_device_info("paraLLEl-GS", "no device reported by the live backend",
                                  "no device reported by the live backend");
        return;
    }
    /* The driver version encoding is vendor specific and Vulkan does not
     * define it, so it is printed raw rather than decoded wrongly for some
     * vendor. Same rule as icorecomp-gs-replay --probe. */
    /* 512, not 256: RtPgsProbe::device_name is itself 256 bytes, so a
     * maximum-length name plus the driver and version suffix does not fit a
     * buffer of the same size, and gcc's -Wformat-truncation says so. The
     * window service's own slot is the same width. */
    char renderer[512];
    std::snprintf(renderer, sizeof(renderer), "%s, driver 0x%08x, Vulkan %u.%u.%u",
        p.device_name, (unsigned)p.driver_version,
        (unsigned)((p.api_version >> 22) & 0x7fu),
        (unsigned)((p.api_version >> 12) & 0x3ffu),
        (unsigned)(p.api_version & 0xfffu));

    struct Feature { const char* name; int32_t ok; };
    const Feature features[] = {
        {"descriptorIndexing", p.descriptor_indexing},
        {"timelineSemaphore", p.timeline_semaphore},
        {"bufferDeviceAddress", p.buffer_device_address},
        {"storageBuffer8BitAccess", p.storage_buffer_8bit},
        {"storageBuffer16BitAccess", p.storage_buffer_16bit},
        {"shaderInt16", p.shader_int16},
        {"scalarBlockLayout", p.scalar_block_layout},
        {"subgroup arithmetic/shuffle/vote/ballot/basic", p.subgroup_ops},
        {"subgroup size control 4 to 64", p.subgroup_size_control},
        {"32 KiB compute shared memory", p.compute_shared_memory},
    };
    std::string missing;
    for (const Feature& f : features) {
        if (f.ok) continue;
        if (!missing.empty()) missing += ", ";
        missing += f.name;
    }
    std::string line = missing.empty() ? std::string("all required features present")
                                       : ("missing: " + missing);
    /* Not a missing feature: the device has it or the run turned it off, and
     * either way the descriptor-buffer path is not the one in use. It is the
     * fact gs_pgs_context.h's fallbacks exist to make visible, so it is
     * reported beside them. */
    if (p.descriptor_buffer_disabled) line += "; descriptor-buffer path disabled";
    rt_window_set_device_info("paraLLEl-GS", renderer, line.c_str());
}

/* Startup options for rt_pgs_create: env resolution plus settings.json.
 * Called from rt_hw_init() (see main.cpp), which runs after
 * rt_settings_init(), so rt_settings() already reflects the loaded file. */
RtPgsCreateOptions resolve_create_options() {
    RtPgsCreateOptions opts = {};
    const RtSettings& s = rt_settings();

    /* display.present, with ICORECOMP_GS_PRESENT winning when it is set.
     * Resolved in gs_select.cpp because both live backends are created with
     * it and the launcher reads it back; see rt_gs_present_mode. */
    opts.present_mode = rt_gs_present_mode();

    /* fit/filter/render_scale/raster/deinterlace have no env twin: straight
     * from settings. */
    switch (s.display.fit) {
    case RtFit::IntegerScale: opts.fit = RT_PGS_FIT_INTEGER; break;
    case RtFit::Stretch: opts.fit = RT_PGS_FIT_STRETCH; break;
    default: opts.fit = RT_PGS_FIT_LETTERBOX; break;
    }
    opts.filter = s.display.filter == RtFilter::Nearest ? RT_PGS_FILTER_NEAREST : RT_PGS_FILTER_LINEAR;
    opts.render_scale = (uint32_t)s.display.render_scale;
    opts.raster = s.display.raster == RtRaster::Window ? RT_PGS_RASTER_WINDOW : RT_PGS_RASTER_CRT;
    switch (s.display.deinterlace) {
    case RtDeinterlace::Bob: opts.deinterlace = RT_PGS_DEINTERLACE_BOB; break;
    case RtDeinterlace::Weave: opts.deinterlace = RT_PGS_DEINTERLACE_WEAVE; break;
    case RtDeinterlace::Adaptive: opts.deinterlace = RT_PGS_DEINTERLACE_ADAPTIVE; break;
    default: opts.deinterlace = RT_PGS_DEINTERLACE_BOB; break;
    }
    /* No env twin either. 0 would mean "the shim's own 640x480 fallback"
     * (see RtPgsCreateOptions in gs_parallel_api.h), which is not what the
     * settings default to: display.window_width/height are 1280x960
     * (settings.h). These are passed through as they stand, so the window
     * opens at the saved size, or at 1280x960 on a fresh install, and the
     * shim's fallback is only reached by a caller that passes 0. */
    opts.window_width = (uint32_t)s.display.window_width;
    opts.window_height = (uint32_t)s.display.window_height;
    /* The window the executable already created (host/window_service.h).
     * Null means this run has none -- no video driver, ICORECOMP_GS_HEADLESS,
     * or a build with no SDL -- and the library goes headless, which is the
     * same decision it used to make for itself from DISPLAY. */
    opts.host_window = rt_window_handle();
    return opts;
}

class ParallelBackend final : public GsBackend {
public:
    ParallelBackend() {
        const RtPgsHost host = { host_log, host_fatal, host_pump_events,
                                 host_create_vulkan_surface };
        RtPgsCreateOptions opts = resolve_create_options();
        m_pgs = rt_pgs_create(&host, &opts); /* fatal (never null) on setup failure */

        /* The window is the exe's, so the quit and resize notifications
         * reach this backend through the window service rather than the
         * other way around. Registered here and cleared in the destructor,
         * so a run with no live backend has no sink. */
        const RtWindowSink sink = { m_pgs, sink_quit, sink_resize, sink_sample };
        rt_window_set_sink(&sink);
        publish_device_info(m_pgs);

        if (const char* v = std::getenv("ICORECOMP_UI_TEST"); v && std::strcmp(v, "1") == 0) {
            run_overlay_ui_test(m_pgs);
        }

        /* display.mode is not applied here any more: gs/gs_select.cpp opens
         * the window and puts it in the configured mode before this
         * constructor runs, because the window is no longer this backend's
         * to make. */
    }

    ~ParallelBackend() override {
        /* Before rt_pgs_destroy: a sink callback fired after the instance is
         * gone would use a freed pointer, and the event pump can run from
         * anywhere on the main thread. */
        rt_window_set_sink(nullptr);
        rt_pgs_destroy(m_pgs);
        m_pgs = nullptr;
    }

    void submit_gif(int path, const uint8_t* data, uint32_t qwords) override {
        rt_pgs_submit_gif(m_pgs, path, data, qwords);
    }

    void write_priv(uint32_t offset, uint64_t v) override {
        rt_pgs_write_priv(m_pgs, offset, v);
    }

    uint64_t read_priv(uint32_t offset) override {
        return rt_pgs_read_priv(m_pgs, offset);
    }

    /* No exit here any more. This can run on the GS command ring's worker
     * thread (gs/gs_threaded.cpp), and process teardown has to happen on the
     * EE thread: std::exit from the worker would run the atexit handler that
     * joins the worker, on the worker. The closure is a sticky flag inside
     * the library instead, and hw/gspriv.cpp polls window_closed() at the
     * field boundary and takes the exit there. */
    bool vsync(unsigned field) override {
        const uint32_t flags = rt_pgs_vsync(m_pgs, field);
        /* LATCHED, not PRESENTED: rt_pgs_vsync stopped presenting when the
         * present rate was decoupled from the field rate, and LATCHED is the
         * bit that still answers GsBackend::vsync's question, "did a frame's
         * worth of traffic land since the previous vsync". PRESENTED now
         * reports on the present pump keeping up, which is not what this
         * bool means on any other backend. See gs_parallel_api.h. */
        return (flags & RT_PGS_VSYNC_LATCHED) != 0;
    }

    /* The present itself (gs_backend.h). Reached either from the GS command
     * ring's worker or, with the ring bypassed, from the EE thread at the
     * field boundary; both are the consumer in their own configuration. */
    void present_pump(double max_hz) override {
        /* The serial says which field reached the swapchain. Nothing here
         * reads it any more; the pointer is still passed because the ABI
         * takes one and a null would be a second code path in the library
         * for no gain. */
        uint64_t serial = 0;
        if (!(rt_pgs_present_pump(m_pgs, max_hz, &serial) & RT_PGS_PUMP_PRESENTED)) return;
        /* One present reached the swapchain. The end-of-run summary counts
         * these and the phase machine uses the first one, which is what
         * separates "the run never got a picture up" from "the picture was
         * up and then something ended the run" (host/run_state.cpp). */
        rt_run_note_present();
        rt_run_phase(RT_PHASE_FIRST_PRESENT);
        publish_present_rect();
        /* The per-present debug line that used to be here, one line per
         * present behind an ICORECOMP_VERBOSE=present token, was removed on
         * 2026-09-05. It was unbounded and it only ever said anything when a
         * reader thought to ask for it. What survives it is the present
         * timing block rt_pgs_present_timings publishes and the
         * presents/repeats counters in the end-of-run stats, both of which
         * are in every log without a token. */
    }

    /* An atomic read inside the library, legal from any thread and true even
     * while the consumer is parked in there with no vsync left to return (a
     * window closed while the swapchain was unusable). */
    bool window_closed() override { return rt_pgs_window_closed(m_pgs) != 0; }

    /* Registers the consumer thread with Granite's thread-index table; see
     * rt_pgs_bind_consumer_thread in gs_parallel_api.h. */
    void bind_consumer_thread() override { rt_pgs_bind_consumer_thread(m_pgs); }

    void report_stats() override {
        rt_pgs_report_stats(m_pgs);
    }

    void present_timings(RtGsPresentTimings* out) override {
        if (!out) return;
        /* The two structs are the same six fields in the same order, but
         * they belong to two different headers on purpose: gs_backend.h
         * stays free of the paraLLEl-GS ABI (see its header comment), so
         * this adapter is where one becomes the other. */
        RtPgsPresentTimings t = {};
        rt_pgs_present_timings(m_pgs, &t);
        out->flush_ns = t.flush_ns;
        out->scanout_ns = t.scanout_ns;
        out->present_ns = t.present_ns;
        out->fields = t.fields;
        out->presents = t.presents;
        out->repeats = t.repeats;
    }

    /* Presentation and overlay control (gs_backend.h). These used to be
     * called on the library handle straight from host/settings_apply.cpp and
     * ui/ui.cpp, which put them outside the GS call stream. They come
     * through the backend now so the command ring sees them in order with
     * the GIF and priv traffic they have to stay ordered against. */
    void set_presentation(uint32_t fit, uint32_t filter) override {
        rt_pgs_set_presentation(m_pgs, fit, filter);
    }

    void set_present_mode(uint32_t mode) override {
        rt_pgs_set_present_mode(m_pgs, mode);
    }

    void set_render_scale(uint32_t factor) override {
        rt_pgs_set_render_scale(m_pgs, factor);
    }
    void set_raster(uint32_t raster) override { rt_pgs_set_raster(m_pgs, raster); }
    void set_deinterlace(uint32_t deinterlace) override { rt_pgs_set_deinterlace(m_pgs, deinterlace); }
    void set_widescreen_aspect(double aspect) override { rt_pgs_set_widescreen_aspect(m_pgs, aspect); }

    uint32_t overlay_texture_create(const uint8_t* rgba8, uint32_t width,
                                    uint32_t height) override {
        return rt_pgs_overlay_texture_create(m_pgs, rgba8, width, height);
    }

    void overlay_texture_destroy(uint32_t texture) override {
        rt_pgs_overlay_texture_destroy(m_pgs, texture);
    }

    void overlay_set_frame(const RtPgsOverlayFrame* frame) override {
        rt_pgs_overlay_set_frame(m_pgs, frame);
    }

    /* Window closure is not handled here, unlike vsync() above: the launcher
     * loop (ui/ui_launcher.cpp) reads the RT_PGS_VSYNC_* bits itself and
     * shuts down its own way. Passing the mask through unchanged is what
     * ui.cpp's wrapper did before. */
    void request_screenshot(uint32_t slots) override {
        rt_pgs_request_screenshot(m_pgs, slots);
    }

    uint32_t present_ui() override {
        const uint32_t r = rt_pgs_present_ui(m_pgs);
        /* The launcher's own presents count too: a run that dies with the
         * launcher on screen has reached "first present", and a run that
         * dies before it has not. */
        if (r & RT_PGS_VSYNC_PRESENTED) {
            rt_run_note_present();
            rt_run_phase(RT_PHASE_FIRST_PRESENT);
        }
        publish_present_rect();
        return r;
    }

    /* The pixels of an armed capture, read back out of the library's
     * published slot. Not a ring record and not a GS command: it is a read
     * of state the present that copied it already published, so it answers
     * on whichever thread asks. See gs_backend.h. */
    size_t take_screenshot(uint32_t slot, uint32_t* w, uint32_t* h, uint8_t* dst,
                           size_t dst_bytes) override {
        return rt_pgs_take_screenshot(m_pgs, slot, w, h, dst, dst_bytes);
    }

private:
    /* Copies the rectangle the library just measured into the window
     * service, which is where every host-side reader of it looks now
     * (host/window_service.h). The library measures it inside its own
     * present, under its own mutex; this runs on the same consumer thread
     * immediately afterwards, so the two never describe different presents.
     *
     * It is a copy rather than a callback because the library must not call
     * back into the exe from inside a present: RtPgsHost's one callback is
     * the event pump, and adding a second reentrant one would put a host
     * mutex inside Granite's frame. */
    void publish_present_rect() {
        int32_t x = 0, y = 0, w = 0, h = 0, bb_w = 0, bb_h = 0;
        rt_pgs_present_rect(m_pgs, &x, &y, &w, &h, &bb_w, &bb_h);
        rt_window_publish_present_rect(x, y, w, h, bb_w, bb_h);
    }

    RtPgs* m_pgs = nullptr;
};

} // namespace

GsBackend* rt_gs_make_parallel_backend() {
    return new ParallelBackend();
}

#endif /* ICORECOMP_HAVE_PARALLEL_GS */
