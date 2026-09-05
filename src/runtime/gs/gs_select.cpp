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
 *   native           gs/render/gs_native.cpp, the clean-room renderer. It has
 *                    not passed its parity gate (docs/GS_RENDERER.md), so
 *                    this value is for development and for the replay tool,
 *                    not for playing. It is never the unset default. Which
 *                    RHI backend it runs on is ICORECOMP_GS_BACKEND's
 *                    answer, exactly as for an unset ICORECOMP_GS.
 *
 * ICORECOMP_GS names the transport, not the renderer. Which live renderer is
 * built, and on which graphics API, is ICORECOMP_GS_BACKEND. There is no
 * settings key: display.backend was withdrawn from settings.json on
 * 2026-09-05 along with the native renderers, and a file that still carries
 * it gets one info line and is otherwise ignored (host/settings.cpp
 * load_retired). The values are:
 *
 *   auto         paraLLEl-GS wherever that backend is built, because the
 *                native renderer has not passed its parity gate; otherwise
 *                metal on macOS and vulkan everywhere else.
 *   parallel-gs  the paraLLEl-GS library.
 *   vulkan       the native renderer on the Vulkan RHI backend.
 *   d3d12        the native renderer on the D3D12 RHI backend (Windows).
 *   metal        the native renderer on the Metal RHI backend (macOS).
 *
 * A value naming a renderer or a backend this build does not have is a warn
 * and a fall through to auto, never fatal (see resolve_live_backend). When
 * ICORECOMP_GS is set it still names the transport, and ICORECOMP_GS_BACKEND
 * still picks the renderer for the live transports, which is what makes
 * ICORECOMP_GS=native plus ICORECOMP_GS_BACKEND=d3d12 mean what it reads as.
 *
 * The window. This file resolves the backend before anything is created,
 * because the answer decides which flags the window needs
 * (SDL_WINDOW_VULKAN for paraLLEl-GS and for the Vulkan backend, none for
 * D3D12 and Metal) and SDL fixes those at creation. So the order is: resolve,
 * create the window (host/window_service.h), then create the backend, which
 * adopts it. ICORECOMP_GS_HEADLESS=1 skips the window and every backend then
 * takes its headless path.
 *
 * The dump writer is authoritative for read_priv in tee mode; both
 * implementations keep last-written-value shadows, so they agree by
 * construction, and keeping one reader avoids ping-ponging.
 *
 * Unknown ICORECOMP_GS values are fatal (loud failure beats silent
 * wrongness). ICORECOMP_GS=parallel|both without the parallel-gs build
 * (see CMakeLists.txt, ICORECOMP_HAVE_PARALLEL_GS) is fatal too.
 *
 * Whichever of the three is built is then wrapped in gs_threaded.cpp's
 * ThreadedBackend, which encodes every call into the GS command ring. The
 * ring is drained inline on the caller's thread until
 * rt_gs_backend_start_worker() (main.cpp, after the launcher) moves the
 * consumer onto its worker thread, which is where it stays for the rest of
 * the run. ICORECOMP_GS_THREAD=0 leaves the wrapper out and hands out the
 * inner backend directly. That is a developer bisect switch for "is the ring
 * responsible for this?", not a user-facing choice, so it is an environment
 * variable only and has no settings.json twin.
 */
#include "gs_backend.h"

/* RT_PGS_PRESENT_* : the presentation vocabulary GsBackend speaks, whichever
 * renderer is behind it. Declarations only; nothing here calls into the
 * paraLLEl-GS library. */
#include "gs_parallel_api.h"
#include "gs_threaded.h"
#ifdef ICORECOMP_NATIVE_GS
#include "render/gs_native.h"
#endif

#include "runtime.h"

#include "../host/settings.h"
#include "../host/window.h"
#include "../host/window_service.h"
#include "../hw/hw.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

/* ICORECOMP_GS_CAPTURE: a capture build. The transport defaults to "both"
 * (the live renderer with every call teed into the dump writer) and the
 * dump path to <base>/capture.gsdump, so a plain double-click run records
 * the GS stream with nothing set. The environment still wins when it is
 * set. Built only for a diagnostic install; never the default. */
static const char* gs_transport_env() {
    const char* m = std::getenv("ICORECOMP_GS");
#ifdef ICORECOMP_GS_CAPTURE
    if (!m || !*m) return "both";
#endif
    return m;
}
static const char* gs_dump_path_env() {
    const char* d = std::getenv("ICORECOMP_GS_DUMP");
#ifdef ICORECOMP_GS_CAPTURE
    if (!d || !*d) {
        static std::string path;
        if (path.empty()) {
            path = std::string(rt_base_dir()) + "/capture.gsdump";
            /* Warn, and the only warn in the tree for a build-time
             * configuration. Deliberate: this build writes ROM-derived data
             * next to the executable on every run, and info is below the
             * shipped default level, so an info line here would be a line
             * nobody sees. The text says it is a notice and not a fault. */
            rt_log_warn("gs", "capture build, notice and not a failure: ICORECOMP_GS defaults to "
                "both and the GS stream is recorded to %s for offline analysis (game data, "
                "never committed)", path.c_str());
        }
        return path.c_str();
    }
#endif
    return d;
}

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

    /* Window closure is a property of the live backend; the dump writer has
     * no window. bind_consumer_thread goes to both because either could need
     * the consumer thread registered with something (only the live one does
     * today, with Granite's thread-index table). */
    bool window_closed() override { return m_live->window_closed(); }

    void bind_consumer_thread() override {
        m_live->bind_consumer_thread();
        m_dump->bind_consumer_thread();
    }

    /* Only the live backend has a present path; the dump writer has none.
     * present_pump is that path's other half and goes the same way: the
     * dump writer has no window to present into and nothing to repeat. */
    void present_timings(RtGsPresentTimings* out) override {
        m_live->present_timings(out);
    }

    void present_pump(double max_hz) override { m_live->present_pump(max_hz); }
    void set_present_rate(double max_hz) override { m_live->set_present_rate(max_hz); }

    /* Presentation and overlay control: the live backend only. The dump
     * writer records the GS command stream, and none of these are part of
     * it; they are window and compositor state. Its GsBackend defaults are
     * no-ops, so calling through would be harmless but misleading. */
    void set_presentation(uint32_t fit, uint32_t filter) override {
        m_live->set_presentation(fit, filter);
    }

    void set_present_mode(uint32_t mode) override { m_live->set_present_mode(mode); }
    void set_render_scale(uint32_t factor) override { m_live->set_render_scale(factor); }
    void set_raster(uint32_t raster) override { m_live->set_raster(raster); }
    void set_deinterlace(uint32_t deinterlace) override { m_live->set_deinterlace(deinterlace); }
    /* Forwarded like the rest: without it a run with ICORECOMP_GS=both takes
     * GsBackend's no-op default and presents at the derived 4:3 aspect while
     * the guest-side widening is in the dump, so the run being inspected is
     * not the run the user sees. */
    void set_widescreen_aspect(double aspect) override { m_live->set_widescreen_aspect(aspect); }

    uint32_t overlay_texture_create(const uint8_t* rgba8, uint32_t width,
                                    uint32_t height) override {
        return m_live->overlay_texture_create(rgba8, width, height);
    }

    void overlay_texture_destroy(uint32_t texture) override {
        m_live->overlay_texture_destroy(texture);
    }

    void overlay_set_frame(const RtPgsOverlayFrame* frame) override {
        m_live->overlay_set_frame(frame);
    }

    uint32_t present_ui() override { return m_live->present_ui(); }
    void request_screenshot(uint32_t slots) override { m_live->request_screenshot(slots); }
    size_t take_screenshot(uint32_t slot, uint32_t* w, uint32_t* h, uint8_t* dst,
                           size_t dst_bytes) override {
        return m_live->take_screenshot(slot, w, h, dst, dst_bytes);
    }

private:
    GsBackend* m_live;
    GsBackend* m_dump;
};

/* ---- which live renderer, and on what (ICORECOMP_GS_BACKEND) --------------
 *
 * Resolved once, before anything is created, because the answer decides the
 * flags the window needs and SDL fixes those at creation. Two live
 * transports consult it: the platform default an unset ICORECOMP_GS resolves
 * to, and an explicit ICORECOMP_GS=parallel|both|native, where it still says
 * which renderer and which graphics API. Only a dump-only run never asks.
 *
 * One predicate decides whether this build has anything live to resolve
 * between, and it is used by every caller here, including
 * rt_gs_backend_selects_live below. The two used to disagree -- make_backend
 * tested (_WIN32 || __APPLE__) and rt_gs_backend_selects_live tested _WIN32
 * -- which meant a macOS build answered "no window will open" and took the
 * boot-first ordering while actually opening one.
 */
#if defined(ICORECOMP_HAVE_PARALLEL_GS) || defined(ICORECOMP_NATIVE_GS)
#define ICORECOMP_HAVE_LIVE_GS 1
#endif

/* The platforms where an unset ICORECOMP_GS means "play in a window". A
 * packaged Windows build is launched from a shortcut and a macOS one from
 * Finder: neither can mean a silent headless dump run. Linux keeps the dump
 * default; dev tooling there sets the variable explicitly. */
#if defined(_WIN32) || defined(__APPLE__)
#define ICORECOMP_GS_LIVE_BY_DEFAULT 1
#endif

#ifdef ICORECOMP_HAVE_LIVE_GS

/* What a run resolved to: the paraLLEl-GS library, or the native renderer on
 * one named RHI backend. */
struct LiveChoice {
    bool native = false;
    RtNativeRhi rhi = RtNativeRhi::Vulkan;
};

/* The live backends this build has, in the spelling ICORECOMP_GS_BACKEND
 * uses. The dump writer is not one: it is a transport ICORECOMP_GS selects,
 * not something ICORECOMP_GS_BACKEND can name. */
const char* built_backends() {
    return
#if defined(ICORECOMP_HAVE_PARALLEL_GS)
        "parallel-gs"
#if defined(ICORECOMP_NATIVE_GS)
        ", "
#endif
#endif
#if defined(ICORECOMP_NATIVE_GS)
        "vulkan"
#if defined(ICORECOMP_RHI_D3D12)
        ", d3d12"
#endif
#if defined(ICORECOMP_RHI_METAL)
        ", metal"
#endif
#endif
        ;
}

const char* backend_choice_name(RtGsBackend b) {
    switch (b) {
    case RtGsBackend::ParallelGs: return "parallel-gs";
    case RtGsBackend::Vulkan: return "vulkan";
    case RtGsBackend::D3D12: return "d3d12";
    case RtGsBackend::Metal: return "metal";
    default: return "auto";
    }
}

const char* live_choice_name(const LiveChoice& c) {
    if (!c.native) return "paraLLEl-GS";
    switch (c.rhi) {
    case RtNativeRhi::D3D12: return "the native renderer on D3D12";
    case RtNativeRhi::Metal: return "the native renderer on Metal";
    default: return "the native renderer on Vulkan";
    }
}

/* Resolved once and remembered: the backend is created once, so a second
 * answer later in the run could only disagree with the object that already
 * exists. The log line is emitted from here for the same reason, so it is
 * one line whichever caller asked first (make_backend, or main.cpp's
 * launcher gate through rt_gs_backend_selects_live). */
LiveChoice resolve_live_backend() {
    static bool resolved = false;
    static LiveChoice value;
    if (resolved) return value;

    /* display.backend was withdrawn from settings.json on 2026-09-05 (the
     * native renderers are not offered to the player until they pass the
     * parity gate in docs/GS_RENDERER.md), so the file names nothing and
     * the choice is auto unless the environment says otherwise. */
    RtGsBackend want = RtGsBackend::Auto;
    const char* source = "the compiled-in default";
    if (const char* env = std::getenv("ICORECOMP_GS_BACKEND"); env && *env) {
        source = "ICORECOMP_GS_BACKEND";
        if (std::strcmp(env, "auto") == 0) {
            want = RtGsBackend::Auto;
        } else if (std::strcmp(env, "parallel-gs") == 0) {
            want = RtGsBackend::ParallelGs;
        } else if (std::strcmp(env, "vulkan") == 0) {
            want = RtGsBackend::Vulkan;
        } else if (std::strcmp(env, "d3d12") == 0) {
            want = RtGsBackend::D3D12;
        } else if (std::strcmp(env, "metal") == 0) {
            want = RtGsBackend::Metal;
        } else {
            /* Not fatal, for the same reason a bad settings.json value is
             * not: this variable was the environment twin of
             * display.backend before that key was retired, and CLAUDE.md
             * ties an environment twin to its key, so a name this build
             * cannot spell falls through to auto with a warn rather than
             * stopping the run. */
            rt_log_warn("gs", "unknown ICORECOMP_GS_BACKEND=%s (expected auto, or one of the "
                              "backends this build has: %s); using auto",
                env, built_backends());
            want = RtGsBackend::Auto;
            source = "ICORECOMP_GS_BACKEND (unknown value, using auto)";
        }
    }

    /* A backend this build does not carry is a warn and a fall through to
     * auto, never a fatal. A script or CI invocation carried over from
     * another machine (macOS spells it "metal", Windows "d3d12") would
     * otherwise stop the run before the launcher window exists. CLAUDE.md:
     * settings handling is never fatal, and this variable is read on the
     * same terms. */
    bool fell_back = false;
    switch (want) {
    case RtGsBackend::ParallelGs:
#ifdef ICORECOMP_HAVE_PARALLEL_GS
        value.native = false;
        break;
#else
        fell_back = true;
        break;
#endif
    case RtGsBackend::Vulkan:
#ifdef ICORECOMP_NATIVE_GS
        value.native = true;
        value.rhi = RtNativeRhi::Vulkan;
        break;
#else
        fell_back = true;
        break;
#endif
    case RtGsBackend::D3D12:
#if defined(ICORECOMP_NATIVE_GS) && defined(ICORECOMP_RHI_D3D12)
        value.native = true;
        value.rhi = RtNativeRhi::D3D12;
        break;
#else
        fell_back = true;
        break;
#endif
    case RtGsBackend::Metal:
#if defined(ICORECOMP_NATIVE_GS) && defined(ICORECOMP_RHI_METAL)
        value.native = true;
        value.rhi = RtNativeRhi::Metal;
        break;
#else
        fell_back = true;
        break;
#endif
    default:
        break;
    }

    if (fell_back) {
        rt_log_warn("gs", "%s = %s but this build has no such backend (built: %s); using auto",
            source, backend_choice_name(want), built_backends());
        want = RtGsBackend::Auto;
    }

    if (want == RtGsBackend::Auto) {
        /* auto. paraLLEl-GS wherever it exists: the native renderer has not
         * passed its parity gate, so it is never picked for someone who did
         * not ask for it by name. Failing that, the platform's own graphics
         * API, which is Metal on macOS and Vulkan everywhere else. */
#if defined(ICORECOMP_HAVE_PARALLEL_GS)
        value.native = false;
#elif defined(ICORECOMP_NATIVE_GS)
        value.native = true;
#if defined(ICORECOMP_RHI_METAL)
        value.rhi = RtNativeRhi::Metal;
#else
        value.rhi = RtNativeRhi::Vulkan;
#endif
#else
        /* Unreachable: this whole block is compiled only when at least one
         * live backend exists (ICORECOMP_HAVE_LIVE_GS above). Kept so the
         * preprocessor arms stay balanced if that ever changes. */
        rt_fatal("gs", nullptr,
                 "this build has no live GS renderer at all (built: %s)", built_backends());
#endif
    }

    resolved = true;
    rt_log_info("gs", "GS backend: %s, from %s = %s (built: %s)%s",
        live_choice_name(value), source, backend_choice_name(want), built_backends(),
        value.native
            ? "; the native renderer has not passed its parity gate, see docs/GS_RENDERER.md"
            : "");
    return value;
}

/* Opens the one window of the run, with the flags the resolved backend
 * needs. SDL fixes SDL_WINDOW_VULKAN at creation, so this cannot be deferred
 * until the backend asks. Failure is not fatal: the window service logs why,
 * and every backend has a headless path. */
void open_window_for(const LiveChoice& choice) {
    if (const char* v = std::getenv("ICORECOMP_GS_HEADLESS"); v && std::strcmp(v, "1") == 0) {
        rt_log_info("gs", "ICORECOMP_GS_HEADLESS=1: no window is created and the GS backend "
                     "renders headless");
        return;
    }
    /* Vulkan for the paraLLEl-GS library and for the Vulkan RHI backend;
     * D3D12 and Metal build their swapchain from the native window handle
     * and want no extra flag. */
    const bool vulkan = !choice.native || choice.rhi == RtNativeRhi::Vulkan;
    rt_window_create(rt_settings(),
                     vulkan ? RtWindowSurface::Vulkan : RtWindowSurface::None,
                     live_choice_name(choice));
    /* display.mode is applied here rather than by the backend: the window is
     * open and windowed at its configured size, and the backend that is about
     * to adopt it does not care which of the three modes it is in. Windowed
     * is already what rt_window_create made. */
    const RtSettings& s = rt_settings();
    if (s.display.mode != RtDisplayMode::Windowed) rt_window_apply_mode(s);
}

/* Creates the resolved live backend, having opened the window for it.
 * Every caller sits under a backend define, so a build with no live backend
 * (the selftest presets) compiles this and never references it. */
[[maybe_unused]] GsBackend* make_live_backend() {
    const LiveChoice choice = resolve_live_backend();
    open_window_for(choice);
#ifdef ICORECOMP_NATIVE_GS
    if (choice.native) return rt_gs_make_native_backend(choice.rhi, rt_gs_present_mode());
#endif
#ifdef ICORECOMP_HAVE_PARALLEL_GS
    return rt_gs_make_parallel_backend();
#else
    /* Unreachable: resolve_live_backend fatals rather than returning a
     * paraLLEl-GS choice in a build without it. */
    rt_fatal("gs", nullptr, "no live GS backend in this build (built: %s)", built_backends());
#endif
}

#endif /* ICORECOMP_HAVE_LIVE_GS */

/* ---- the present mode every live backend is created with ------------------
 *
 * ICORECOMP_GS_PRESENT wins over the compiled-in mailbox default when it is
 * set (display.present was retired on 2026-09-04), mapped the
 * way the paraLLEl-GS library mapped it before this resolution existed
 * (vsync/fifo -> FIFO, tear/immediate -> IMMEDIATE, anything else ->
 * MAILBOX). Resolved once and remembered: both live backends are created
 * once, the launcher reads the value back at hand-off, and a second
 * resolution could only repeat the disagreement warning.
 */
uint32_t g_present_mode = RT_PGS_PRESENT_MAILBOX;
bool g_present_mode_resolved = false;

} // namespace

uint32_t rt_gs_present_mode() {
    if (g_present_mode_resolved) return g_present_mode;
    g_present_mode_resolved = true;

    const RtSettings& s = rt_settings();
    uint32_t from_settings = RT_PGS_PRESENT_MAILBOX;
    switch (s.display.present) {
    case RtPresentMode::Fifo: from_settings = RT_PGS_PRESENT_FIFO; break;
    case RtPresentMode::Immediate: from_settings = RT_PGS_PRESENT_IMMEDIATE; break;
    default: break;
    }

    const char* pm = std::getenv("ICORECOMP_GS_PRESENT");
    if (!pm) {
        g_present_mode = from_settings;
        return g_present_mode;
    }
    if (std::strcmp(pm, "vsync") == 0 || std::strcmp(pm, "fifo") == 0) {
        g_present_mode = RT_PGS_PRESENT_FIFO;
    } else if (std::strcmp(pm, "tear") == 0 || std::strcmp(pm, "immediate") == 0) {
        g_present_mode = RT_PGS_PRESENT_IMMEDIATE;
    } else {
        g_present_mode = RT_PGS_PRESENT_MAILBOX;
    }
    if (from_settings != g_present_mode) {
        rt_log_info("gs", "present mode: ICORECOMP_GS_PRESENT=%s overrides the compiled-in "
                     "default for this run", pm);
    }
    return g_present_mode;
}

namespace {

GsBackend* g_backend = nullptr;
/* The same object as g_backend when the ring is in use, null when
 * ICORECOMP_GS_THREAD=0 bypassed it. Kept as its own pointer rather than
 * recovered with a dynamic_cast: this file is the one place that knows
 * which of the two it built. */
ThreadedBackend* g_ring = nullptr;
bool g_creating = false;

void backend_atexit() {
    if (g_backend) {
        /* First: stop the command ring's worker thread, so the statistics
         * below are final and the teardown after them is the only thread
         * touching the backend. A no-op when the ring is drained inline or
         * bypassed. */
        g_backend->quiesce();
        rt_geom_report();
        g_backend->report_stats();
        /* Destroy the backend so a live Vulkan device tears down cleanly
         * (wait-idle in its destructor) even on std::exit paths. */
        delete g_backend;
        g_backend = nullptr;
        g_ring = nullptr;
    }
    /* The window outlives every backend that presented into it, and is
     * destroyed here, after the last one: a swapchain built on a destroyed
     * window is what the ordering exists to prevent. Idempotent, and a no-op
     * when no window was ever created. */
    rt_window_destroy();
}

GsBackend* make_backend(const char* mode, const char* dump_path) {
    if (!mode || !*mode) {
#if defined(ICORECOMP_GS_LIVE_BY_DEFAULT) && defined(ICORECOMP_HAVE_LIVE_GS)
        rt_log_info("gs", "ICORECOMP_GS unset: this platform defaults to a live backend "
                     "(set ICORECOMP_GS=dump for the headless dump backend)");
        return make_live_backend();
#else
        rt_log_info("gs", "GS backend: the dump backend, this platform's default for an unset "
                     "ICORECOMP_GS; ICORECOMP_GS_BACKEND is not consulted, since no live backend "
                     "is being created (set ICORECOMP_GS=parallel or native for one)");
        return rt_gs_make_dump_backend(dump_path);
#endif
    }
    if (std::strcmp(mode, "dump") == 0) {
        rt_log_info("gs", "GS backend: ICORECOMP_GS=dump, the headless dump backend; "
                     "ICORECOMP_GS_BACKEND is not consulted, since no live backend is being "
                     "created");
        return rt_gs_make_dump_backend(dump_path);
    }

    /* Every remaining transport is live, so ICORECOMP_GS_BACKEND still
     * decides which renderer and which graphics API. ICORECOMP_GS=native
     * additionally pins the renderer, and an ICORECOMP_GS_BACKEND of
     * parallel-gs would then contradict it: that is a fatal rather than a
     * silent preference, the same rule the rest of this resolver follows. */
#ifdef ICORECOMP_HAVE_LIVE_GS
    if (std::strcmp(mode, "native") == 0) {
#ifdef ICORECOMP_NATIVE_GS
        const LiveChoice choice = resolve_live_backend();
        if (!choice.native) {
            rt_fatal("gs", nullptr,
                     "ICORECOMP_GS=native but ICORECOMP_GS_BACKEND resolved to paraLLEl-GS; name "
                     "one of the native backends (vulkan, d3d12, metal) or unset ICORECOMP_GS");
        }
        open_window_for(choice);
        return rt_gs_make_native_backend(choice.rhi, rt_gs_present_mode());
#else
        rt_fatal("gs", nullptr,
                 "ICORECOMP_GS=native but this build has no native GS renderer "
                 "(configure with -DICORECOMP_NATIVE_GS=ON)");
#endif
    }
#ifdef ICORECOMP_HAVE_PARALLEL_GS
    if (std::strcmp(mode, "parallel") == 0) {
        return make_live_backend();
    }
    if (std::strcmp(mode, "both") == 0) {
        /* Sequenced, not written as one expression: C++17 leaves the order
         * of a call's arguments unspecified, so a `both` run could open the
         * dump file before or after the Vulkan device and the window. Nothing
         * depends on that today; two named locals keep it from becoming a
         * dependency nobody notices. */
        GsBackend* live = make_live_backend();
        GsBackend* dump = rt_gs_make_dump_backend(dump_path);
        return new TeeBackend(live, dump);
    }
#endif
#endif /* ICORECOMP_HAVE_LIVE_GS */
#ifndef ICORECOMP_HAVE_PARALLEL_GS
    if (std::strcmp(mode, "parallel") == 0 || std::strcmp(mode, "both") == 0) {
        rt_fatal("gs", nullptr,
                 "ICORECOMP_GS=%s but this build has no paraLLEl-GS backend "
                 "(configure with the third_party/parallel-gs submodule initialized)", mode);
    }
#endif
#ifndef ICORECOMP_HAVE_LIVE_GS
    if (std::strcmp(mode, "native") == 0) {
        rt_fatal("gs", nullptr,
                 "ICORECOMP_GS=native but this build has no native GS renderer "
                 "(configure with -DICORECOMP_NATIVE_GS=ON)");
    }
#endif
    rt_fatal("gs", nullptr,
             "unknown ICORECOMP_GS=%s (expected dump, parallel, both or native)", mode);
}

} // namespace

/* The same resolution make_backend does, with nothing created: no Vulkan
 * device, no window, and no ICORECOMP_GS_DUMP file opened. main.cpp's
 * launcher gate needs the answer before rt_mem_init/rt_hw_init, because a
 * dump run that goes through rt_hw_init first has already truncated the
 * dump file by the time the cheap boot checks can fail.
 *
 * The question is "will a window open". Both live renderers answer yes now:
 * the native one is handed the executable's window like the paraLLEl-GS one
 * (host/window_service.h), so the resolved renderer no longer changes the
 * answer, only whether a live transport was chosen at all. Whether the window
 * then actually opens is a run-time fact (a headless session, or
 * ICORECOMP_GS_HEADLESS=1) that main.cpp checks separately with
 * rt_window_exists(). */
bool rt_gs_backend_selects_live() {
#ifndef ICORECOMP_HAVE_LIVE_GS
    /* No live backend in this build: parallel, both and native are all
     * fatal, which make_backend reports when it actually runs. */
    return false;
#else
    const char* mode = gs_transport_env();
    if (!mode || !*mode) {
#ifdef ICORECOMP_GS_LIVE_BY_DEFAULT
        return true;
#else
        return false;
#endif
    }
    if (std::strcmp(mode, "dump") == 0) return false;
    return std::strcmp(mode, "parallel") == 0 || std::strcmp(mode, "both") == 0
        || std::strcmp(mode, "native") == 0;
#endif
}

/* ---- the device the active backend created --------------------------------
 *
 * These used to run a Vulkan probe that created and destroyed a device of its
 * own on every launch, and reported a device that need not be the one the run
 * went on to use. Each live backend now publishes what it actually created,
 * once, into the window service (host/window_service.h), and these two read
 * it back. A run with no live backend has nothing to report, and says so
 * rather than making a device to answer the question. */
/* Both return a pointer into their own function-local static, so the
 * returned pointer lives only until the next call to the same function, and
 * neither is safe to call from two threads at once past the initialisation
 * guard. Both callers, the startup log below and the Display tab
 * (ui/ui_settings_model.cpp, which copies into a std::string), use the
 * result immediately and run on the main thread, so neither hazard is live.
 * A std::string return would remove both, at the cost of changing hw.h and
 * the UI model. */
const char* rt_gs_probe_renderer_line() {
    static std::string line;
    char renderer[kRtWindowRendererBytes] = {};
    if (!rt_window_device_info(nullptr, 0, renderer, sizeof(renderer), nullptr, 0)) {
        line = "no renderer device created yet";
    } else {
        line = renderer;
    }
    return line.c_str();
}

const char* rt_gs_probe_features_line() {
    static std::string line;
    char features[kRtWindowFeaturesBytes] = {};
    if (!rt_window_device_info(nullptr, 0, nullptr, 0, features, sizeof(features))) {
        line = "no renderer device created yet";
    } else {
        line = features;
    }
    return line.c_str();
}

GsBackend* rt_gs_backend_if_created() {
    return g_backend;
}

GsBackend* rt_gs_backend() {
    if (!g_backend) {
        /* make_backend opens the Vulkan device and the window, which runs a
         * good deal of code (SDL, Granite, the driver) that must not reach
         * back in here and start a second one. Nothing does today; this is
         * the loud version of that fact. */
        if (g_creating) {
            rt_fatal("gs", nullptr,
                     "rt_gs_backend() called while the backend is still being created");
        }
        g_creating = true;
        GsBackend* inner = make_backend(gs_transport_env(),
                                        gs_dump_path_env());
        /* The two lines a machine nobody here owns is asked to report back
         * (docs/MACOS.md), from the device the backend just created rather
         * than from a probe device made and thrown away. The settings menu
         * shows the same two strings. A backend that created no device (the
         * dump writer) publishes nothing and these say so. */
        rt_log_info("gs", "Renderer: %s", rt_gs_probe_renderer_line());
        rt_log_info("gs", "Feature support: %s", rt_gs_probe_features_line());
        const char* thread = std::getenv("ICORECOMP_GS_THREAD");
        if (thread && std::strcmp(thread, "0") == 0) {
            rt_log_info("gs", "ICORECOMP_GS_THREAD=0: GS calls go straight to the backend, "
                         "no command ring");
            g_backend = inner;
        } else {
            rt_log_info("gs", "GS command ring active (gs_threaded.cpp, drained inline until the "
                         "game boots); set ICORECOMP_GS_THREAD=0 to bypass it");
            g_ring = static_cast<ThreadedBackend*>(rt_gs_make_threaded_backend(inner));
            g_backend = g_ring;
        }
        g_creating = false;
        /* A backend object exists from here, whichever flavour it is. The
         * phase is what places a run that dies during device or swapchain
         * setup: "backend created" means the constructor returned, and
         * anything earlier means it did not. */
        rt_run_phase(RT_PHASE_BACKEND_CREATED);
        std::atexit(backend_atexit);
    }
    return g_backend;
}

/* See gs_backend.h. main.cpp calls this once, after the launcher has handed
 * over and before the scheduler boots: the launcher's own present loop
 * (ui/ui_launcher.cpp) then runs single threaded exactly as it did before
 * the worker existed, and every field of the game runs with the ring drained
 * off the EE thread. */
void rt_gs_backend_start_worker() {
    if (!g_backend) {
        /* rt_hw_init() runs before this on both of main.cpp's orderings, so
         * there is always a backend by now. Silence here would mean a run
         * that quietly stayed single threaded, which is exactly the kind of
         * performance change that gets diagnosed as something else. */
        rt_log_info("gs", "GS worker thread not started: no backend exists yet, so the GS command "
                     "ring was never built; this run keeps the GS on the EE thread");
        return;
    }
    if (!g_ring) {
        rt_log_info("gs", "GS worker thread not started: the command ring is bypassed "
                     "(ICORECOMP_GS_THREAD=0), so the GS runs on the EE thread");
        return;
    }
    g_ring->start_worker();
}
