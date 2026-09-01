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

#include <cstdlib>
#include <cstring>

namespace {

void host_log(const char* component, const char* message) {
    rt_log(component, "%s", message);
}

void host_fatal(const char* component, const char* message) {
    rt_fatal(component, nullptr, "%s", message);
}

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
    return opts;
}

class ParallelBackend final : public GsBackend {
public:
    ParallelBackend() {
        const RtPgsHost host = { host_log, host_fatal };
        RtPgsCreateOptions opts = resolve_create_options();
        m_pgs = rt_pgs_create(&host, &opts); /* fatal (never null) on setup failure */
    }

    ~ParallelBackend() override {
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

#endif /* ICORECOMP_HAVE_PARALLEL_GS */
