/* gs/gs_parallel.cpp: live GS backend adapter over libicorecomp-parallel-gs.
 *
 * Thin GsBackend implementation that forwards to the shared library's C ABI
 * (gs_parallel_api.h). Everything Granite/paraLLEl-GS lives inside the
 * library (gs_parallel_lib.cpp); this file must stay free of their headers
 * and classes: it is the MIT side of the LGPL boundary.
 *
 * Presentation policy (window vs headless, screenshots, validation layers)
 * is decided library-side at rt_pgs_create; see gs_parallel_lib.cpp. The
 * one policy this side owns is window closure: the library reports
 * RT_PGS_VSYNC_WINDOW_CLOSED and the runtime exits cleanly here, so process
 * teardown (atexit backend stats, device wait-idle) stays with the host.
 */
#include "gs_backend.h"

#ifdef ICORECOMP_HAVE_PARALLEL_GS

#include "gs_parallel_api.h"
#include "runtime.h"

#include "../host/settings.h"
#include "../host/window.h"

#include <cstdlib>
#include <cstring>

namespace {

void host_log(const char* component, const char* message) {
    rt_log(component, "%s", message);
}

void host_fatal(const char* component, const char* message) {
    rt_fatal(component, nullptr, "%s", message);
}

/* Event pump inversion (shim 3): the exe owns the only SDL_PollEvent loop.
 * Called from inside Granite's WSI::begin_frame (see gs_parallel_lib.cpp's
 * RtPgs::present_frame); rt_window_pump honors that reentrancy contract
 * (queue/translate events, notify_quit/notify_resize only). */
void host_pump_events() {
    rt_window_pump();
}

/* The live backend's RtPgs*, exposed to the exe side (host/window.cpp,
 * host/settings_apply.cpp) via rt_gs_parallel_handle(). Set for the
 * lifetime of the one ParallelBackend instance gs_select.cpp ever creates;
 * RtPgs itself stays opaque here, same as everywhere else on this side of
 * the LGPL boundary. */
RtPgs* g_live_pgs = nullptr;

/* Startup options for rt_pgs_create: env resolution plus settings.json.
 * Called from rt_hw_init() (see main.cpp), which runs after
 * rt_settings_init(), so rt_settings() already reflects the loaded file. */
RtPgsCreateOptions resolve_create_options() {
    RtPgsCreateOptions opts = {};
    const RtSettings& s = rt_settings();

    /* present: ICORECOMP_GS_PRESENT wins when set, mapped exactly as the
     * library mapped it itself before this option existed (vsync/fifo ->
     * FIFO, tear/immediate -> IMMEDIATE, anything else -> MAILBOX);
     * otherwise display.present decides. */
    if (const char* pm = std::getenv("ICORECOMP_GS_PRESENT"); pm && *pm) {
        uint32_t mode = RT_PGS_PRESENT_MAILBOX;
        if (std::strcmp(pm, "vsync") == 0 || std::strcmp(pm, "fifo") == 0) {
            mode = RT_PGS_PRESENT_FIFO;
        } else if (std::strcmp(pm, "tear") == 0 || std::strcmp(pm, "immediate") == 0) {
            mode = RT_PGS_PRESENT_IMMEDIATE;
        }
        opts.present_mode = mode;

        uint32_t settings_mode = RT_PGS_PRESENT_MAILBOX;
        switch (s.display.present) {
        case RtPresentMode::Fifo: settings_mode = RT_PGS_PRESENT_FIFO; break;
        case RtPresentMode::Immediate: settings_mode = RT_PGS_PRESENT_IMMEDIATE; break;
        default: break;
        }
        if (settings_mode != mode) {
            rt_log("gs", "display.present: using ICORECOMP_GS_PRESENT=%s, settings.json value ignored", pm);
        }
    } else {
        switch (s.display.present) {
        case RtPresentMode::Fifo: opts.present_mode = RT_PGS_PRESENT_FIFO; break;
        case RtPresentMode::Immediate: opts.present_mode = RT_PGS_PRESENT_IMMEDIATE; break;
        default: opts.present_mode = RT_PGS_PRESENT_MAILBOX; break;
        }
    }

    /* fit/filter/render_scale/hires_scanout have no env twin: straight from
     * settings. */
    switch (s.display.fit) {
    case RtFit::IntegerScale: opts.fit = RT_PGS_FIT_INTEGER; break;
    case RtFit::Stretch: opts.fit = RT_PGS_FIT_STRETCH; break;
    default: opts.fit = RT_PGS_FIT_LETTERBOX; break;
    }
    opts.filter = s.display.filter == RtFilter::Nearest ? RT_PGS_FILTER_NEAREST : RT_PGS_FILTER_LINEAR;
    opts.render_scale = (uint32_t)s.display.render_scale;
    opts.hires_scanout = s.display.hires_scanout ? 1u : 0u;
    /* No env twin either. 0 would mean "the shim's own 640x480 default"
     * (see RtPgsCreateOptions in gs_parallel_api.h); display.window_width/
     * height already defaults to that same 640x480, so passing them
     * straight through is equivalent to 0 whenever settings are at their
     * compiled-in default and simply honors a user's saved size otherwise. */
    opts.window_width = (uint32_t)s.display.window_width;
    opts.window_height = (uint32_t)s.display.window_height;
    return opts;
}

class ParallelBackend final : public GsBackend {
public:
    ParallelBackend() {
        const RtPgsHost host = { host_log, host_fatal, host_pump_events };
        RtPgsCreateOptions opts = resolve_create_options();
        m_pgs = rt_pgs_create(&host, &opts); /* fatal (never null) on setup failure */
        g_live_pgs = m_pgs;

        /* init_windowed (gs_parallel_lib.cpp) already opened the window at
         * opts.window_width/height, i.e. display.window_width/height, so a
         * Windowed-mode run needs no further action here. Either fullscreen
         * mode still needs rt_window_apply_mode to take the window from
         * "windowed at the configured size" (what rt_pgs_create just did)
         * to what display.mode actually asks for. */
        const RtSettings& s = rt_settings();
        if (s.display.mode != RtDisplayMode::Windowed) {
            rt_window_apply_mode(s);
        }
    }

    ~ParallelBackend() override {
        g_live_pgs = nullptr;
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

    bool vsync(unsigned field) override {
        uint32_t flags = rt_pgs_vsync(m_pgs, field);
        if (flags & RT_PGS_VSYNC_WINDOW_CLOSED) {
            rt_log("gs", "paraLLEl-GS: window closed, exiting");
            std::exit(0);
        }
        return (flags & RT_PGS_VSYNC_PRESENTED) != 0;
    }

    void report_stats() override {
        rt_pgs_report_stats(m_pgs);
    }

private:
    RtPgs* m_pgs = nullptr;
};

} // namespace

GsBackend* rt_gs_make_parallel_backend() {
    return new ParallelBackend();
}

/* See window.h: the exe-side accessor to the live backend's RtPgs*, used by
 * host/window.cpp (the event pump) and host/settings_apply.cpp (warm
 * appliers). window.cpp carries the nullptr stub for builds with no
 * paraLLEl-GS backend at all (ICORECOMP_HAVE_PARALLEL_GS unset); this is
 * the one definition when the backend exists, regardless of whether it is
 * the one currently selected by ICORECOMP_GS. */
RtPgs* rt_gs_parallel_handle() {
    return g_live_pgs;
}

#endif /* ICORECOMP_HAVE_PARALLEL_GS */
