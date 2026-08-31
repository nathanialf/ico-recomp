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

#include <cstdlib>

namespace {

void host_log(const char* component, const char* message) {
    rt_log(component, "%s", message);
}

void host_fatal(const char* component, const char* message) {
    rt_fatal(component, nullptr, "%s", message);
}

class ParallelBackend final : public GsBackend {
public:
    ParallelBackend() {
        const RtPgsHost host = { host_log, host_fatal };
        m_pgs = rt_pgs_create(&host); /* fatal (never null) on setup failure */
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
