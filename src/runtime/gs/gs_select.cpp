/* gs/gs_select.cpp: GS backend selection.
 *
 * ICORECOMP_GS picks the implementation behind rt_gs_backend():
 *   dump (or unset)  gs_dumpwriter.cpp; writes a raw stream when
 *                    ICORECOMP_GS_DUMP=path is set, otherwise shadow+stats.
 *                    On Windows builds with the live backend, unset means
 *                    parallel instead: a packaged exe must open a window.
 *   parallel         gs_parallel.cpp, live paraLLEl-GS rendering.
 *   both             live rendering, with every call teed into the dump
 *                    writer as well, so a run can be replayed offline while
 *                    also being displayed.
 *
 * The dump writer is authoritative for read_priv in tee mode; both
 * implementations keep last-written-value shadows, so they agree by
 * construction, and keeping one reader avoids ping-ponging.
 *
 * Unknown ICORECOMP_GS values are fatal (loud failure beats silent
 * wrongness). ICORECOMP_GS=parallel|both without the parallel-gs build
 * (see CMakeLists.txt, ICORECOMP_HAVE_PARALLEL_GS) is fatal too.
 */
#include "gs_backend.h"

#include "runtime.h"

#include "../hw/hw.h"

#include <cstdlib>
#include <cstring>

namespace {

class TeeBackend final : public GsBackend {
public:
    TeeBackend(GsBackend* live, GsBackend* dump) : m_live(live), m_dump(dump) {}
    ~TeeBackend() override {
        delete m_live;
        delete m_dump;
    }

    void submit_gif(int path, const uint8_t* data, uint32_t qwords) override {
        m_live->submit_gif(path, data, qwords);
        m_dump->submit_gif(path, data, qwords);
    }

    void write_priv(uint32_t offset, uint64_t v) override {
        m_live->write_priv(offset, v);
        m_dump->write_priv(offset, v);
    }

    uint64_t read_priv(uint32_t offset) override {
        return m_dump->read_priv(offset);
    }

    bool vsync(unsigned field) override {
        bool live = m_live->vsync(field);
        bool dump = m_dump->vsync(field);
        return live || dump;
    }

    void report_stats() override {
        m_live->report_stats();
        m_dump->report_stats();
    }

private:
    GsBackend* m_live;
    GsBackend* m_dump;
};

GsBackend* g_backend = nullptr;

void backend_atexit() {
    if (g_backend) {
        rt_geom_report();
        g_backend->report_stats();
        /* Destroy the backend so a live Vulkan device tears down cleanly
         * (wait-idle in its destructor) even on std::exit paths. */
        delete g_backend;
        g_backend = nullptr;
    }
}

GsBackend* make_backend(const char* mode, const char* dump_path) {
    if (!mode || !*mode) {
#if defined(_WIN32) && defined(ICORECOMP_HAVE_PARALLEL_GS)
        /* Packaged Windows builds are double-clicked: an unset ICORECOMP_GS
         * must mean "play in a window", not a silent headless dump run.
         * Linux keeps the dump default; dev tooling there sets the variable
         * explicitly. */
        rt_log("gs", "ICORECOMP_GS unset: defaulting to the live paraLLEl-GS backend "
                     "(set ICORECOMP_GS=dump for the headless dump backend)");
        return rt_gs_make_parallel_backend();
#else
        return rt_gs_make_dump_backend(dump_path);
#endif
    }
    if (std::strcmp(mode, "dump") == 0) {
        return rt_gs_make_dump_backend(dump_path);
    }
#ifdef ICORECOMP_HAVE_PARALLEL_GS
    if (std::strcmp(mode, "parallel") == 0) {
        return rt_gs_make_parallel_backend();
    }
    if (std::strcmp(mode, "both") == 0) {
        return new TeeBackend(rt_gs_make_parallel_backend(),
                              rt_gs_make_dump_backend(dump_path));
    }
#else
    if (std::strcmp(mode, "parallel") == 0 || std::strcmp(mode, "both") == 0) {
        rt_fatal("gs", nullptr,
                 "ICORECOMP_GS=%s but this build has no paraLLEl-GS backend "
                 "(configure with the third_party/parallel-gs submodule initialized)", mode);
    }
#endif
    rt_fatal("gs", nullptr, "unknown ICORECOMP_GS=%s (expected dump, parallel or both)", mode);
}

} // namespace

/* The same resolution make_backend does, with nothing created: no Vulkan
 * device, no window, and no ICORECOMP_GS_DUMP file opened. main.cpp's
 * launcher gate needs the answer before rt_mem_init/rt_hw_init, because a
 * dump run that goes through rt_hw_init first has already truncated the
 * dump file by the time the cheap boot checks can fail.
 *
 * A build with no paraLLEl-GS answers false for every value: parallel and
 * both are fatal there, which make_backend reports when it actually runs. */
bool rt_gs_backend_selects_live() {
    const char* mode = std::getenv("ICORECOMP_GS");
    if (!mode || !*mode) {
#if defined(_WIN32) && defined(ICORECOMP_HAVE_PARALLEL_GS)
        return true;
#else
        return false;
#endif
    }
#ifdef ICORECOMP_HAVE_PARALLEL_GS
    return std::strcmp(mode, "parallel") == 0 || std::strcmp(mode, "both") == 0;
#else
    return false;
#endif
}

GsBackend* rt_gs_backend() {
    if (!g_backend) {
        g_backend = make_backend(std::getenv("ICORECOMP_GS"),
                                 std::getenv("ICORECOMP_GS_DUMP"));
        std::atexit(backend_atexit);
    }
    return g_backend;
}
