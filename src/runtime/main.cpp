/* main.cpp: boot entry point.
 *
 * Sequence: open the log sink and install the crash handlers -> set FPU
 * FTZ/DAZ -> allocate guest memory + page table -> read
 * config/recomp.toml -> SHA-1-check and load the boot ELF (mounting the
 * disc image if that is where it comes from) -> wire up generated code (if
 * linked) -> bring up the GS backend and its window -> initialize the P2
 * kernel HLE (scheduler, INTC, timers, SIF) -> create guest thread 1
 * running the translated entry point and enter the scheduler loop, under a
 * crash handler that dumps the current guest context.
 *
 * Two orderings, chosen by the launcher gate below: without the launcher
 * the window comes up late on purpose, and with it the window comes up
 * first. See the gate and the rt_hw_init() call for why both are right.
 *
 * Builds in two modes, selected at CMake configure time by whether
 * generated/ee exists (see CMakeLists.txt, -DICORECOMP_HAVE_GENERATED):
 *   - stub mode: no generated/ee, g_functab is entirely empty. The entry
 *     lookup below fails gracefully with a clear message instead of
 *     crashing, so this mode is a useful smoke test of memory/loader/build
 *     wiring on its own.
 *   - full mode: generated/ee's funcs_table.c is linked in and its
 *     g_functab_init() populates g_functab; we then actually run crt0 until
 *     it traps into rt_syscall / rt_mmio_.. / rt_bad_indirect / etc.
 *
 * Presentation loop (ICORECOMP_GS=parallel|both): the EE scheduler already
 * owns the only loop in the process (ee/sched.cpp sched_loop; rt_sched_boot
 * never returns), and its virtual-clock timeline calls the GS backend's
 * vsync() at every field boundary via rt_gs_vsync_hook. The live backend
 * (gs/gs_parallel.cpp) renders and presents to the window swapchain inside
 * that vsync() call, so the per-field cadence is exactly "run guest to the
 * field boundary, then render + present", just with the loop inverted:
 * presentation runs inside the scheduler rather than the scheduler inside a
 * main-owned frame loop. Everything stays on the main OS thread (guest
 * threads are minicoro coroutines), which keeps SDL/WSI happy. main's part
 * of the contract is creating the backend on the main stack, before any
 * guest code runs, via rt_hw_init() below: Vulkan device and window setup
 * must not first happen lazily under a 2 MB coroutine stack mid-frame.
 * With ICORECOMP_GS=dump or unset the behavior is byte-identical to the
 * dump-only runtime: same calls, same order, no window, no Vulkan.
 * (Wish, if main ever needs to own the loop, e.g. for a GUI event pump
 * that must run between fields: a resumable scheduler entry point like
 * "rt_sched_run_until_field_boundary()" returning control to main once per
 * field. Not needed for the current inversion.)
 */
#include "runtime.h"

#include "ee/kernel.h"
#include "host/settings.h"
#include "host/window.h"
#include "hw/hw.h"
#include "iso/iso9660.h"
#include "prof.h"
#include "ui/ui.h"

#ifdef ICORECOMP_HAVE_PARALLEL_GS
#include "gs/gs_parallel_api.h"
#endif

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define ICORECOMP_X86 1
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

#ifdef ICORECOMP_HAVE_GENERATED
extern "C" void g_functab_init(void);
#endif

namespace {

void set_fpu_ftz_daz() {
#ifdef ICORECOMP_X86
    /* recomp_ops.h requires FTZ+DAZ on every guest-executing thread so the
     * host FPU never produces a denormal the PS2 tier-0 FPU model doesn't
     * expect. FTZ = MXCSR bit 15, DAZ = bit 6. */
    _mm_setcsr(_mm_getcsr() | 0x8000u | 0x0040u);
    rt_log("main", "MXCSR FTZ+DAZ set (0x%08x)", _mm_getcsr());
#else
    rt_log("main", "warning: no known FTZ/DAZ setup for this architecture");
#endif
}

/* Not async-signal-safe (fprintf, unordered_map lookups in rt_dump_registers
 * are reachable transitively via rt_log elsewhere, though not from here).
 * Acceptable for a debug boot skeleton: goal is a readable dump on the way
 * down, not signal-handler purity. */
void crash_handler(int sig) {
    const char* name = "signal";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGFPE:  name = "SIGFPE"; break;
        case SIGILL:  name = "SIGILL"; break;
#ifdef SIGBUS
        case SIGBUS:  name = "SIGBUS"; break;
#endif
        default: break;
    }
    rt_log("crash", "FATAL: caught %s while running guest code (thread %d)",
        name, rt_thread_current_id());
    if (rt_sched_current_ctx()) rt_dump_registers(rt_sched_current_ctx());
    std::fflush(stderr);
    rt_log_hold_console();
    std::_Exit(1);
}

/* Last resort for a failure that never reaches the signal handlers: an
 * uncaught C++ exception (the Vulkan and swapchain paths in the GS library
 * throw) unwinds through std::terminate instead. Without this the process
 * dies through the CRT's own abort message, which on a double-clicked
 * Windows run is exactly the output that disappears with the console. */
[[noreturn]] void terminate_handler() {
    const char* what = "unknown";
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            what = ex.what();
        } catch (...) {
            what = "non-std exception";
        }
    }
    rt_log("crash", "FATAL: std::terminate: %s", what);
    if (rt_sched_current_ctx()) rt_dump_registers(rt_sched_current_ctx());
    std::fflush(stderr);
    rt_log_hold_console();
    std::_Exit(1);
}

#ifdef _WIN32
/* Structured exceptions raised on a minicoro stack, or inside the GS DLL,
 * do not reliably reach the CRT's SIGSEGV mapping. This filter runs for any
 * unhandled one in the process, including a stack overflow. */
LONG WINAPI seh_filter(EXCEPTION_POINTERS* info) {
    DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    const void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    const char* name = "exception";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      name = "ACCESS_VIOLATION"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   name = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_STACK_OVERFLOW:        name = "STACK_OVERFLOW"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    name = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    name = "FLT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_IN_PAGE_ERROR:         name = "IN_PAGE_ERROR"; break;
        case EXCEPTION_PRIV_INSTRUCTION:      name = "PRIV_INSTRUCTION"; break;
        default: break;
    }
    rt_log("crash", "FATAL: unhandled %s (code 0x%08lx) at host address %p (thread %d)",
        name, (unsigned long)code, addr, rt_thread_current_id());
    if (rt_sched_current_ctx()) rt_dump_registers(rt_sched_current_ctx());
    std::fflush(stderr);
    rt_log_hold_console();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

/* SIGINT: dump the thread/semaphore inventory before dying so an
 * interactive interrupt of a parked or spinning run is diagnosable. */
void sigint_handler(int) {
    rt_log("main", "SIGINT");
    if (rt_sched_current_ctx()) rt_dump_registers(rt_sched_current_ctx());
    rt_sched_dump_inventory("SIGINT");
    std::fflush(stderr);
    /* Logging is asynchronous and _Exit runs no atexit, so drain before
     * leaving or the inventory dump above never reaches the file. */
    rt_log_drain();
    std::_Exit(130);
}

void install_crash_handler() {
    std::set_terminate(terminate_handler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(seh_filter);
#endif
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGFPE, crash_handler);
    std::signal(SIGILL, crash_handler);
#ifdef SIGBUS
    /* Not a thing on Windows; faults arrive as SIGSEGV there. */
    std::signal(SIGBUS, crash_handler);
#endif
    std::signal(SIGINT, sigint_handler);
}

/* --no-launcher: skip the launcher window and boot straight into the game
 * (see rt_no_launcher_flag). */
bool g_no_launcher = false;

/* The launcher's last gate condition: the backend rt_hw_init() created is
 * the live one and it actually opened a window. A dump-only run, a headless
 * live run and a build with no paraLLEl-GS at all have nothing to draw a
 * launcher into, and take the boot-first ordering. */
bool live_windowed_backend() {
#ifdef ICORECOMP_HAVE_PARALLEL_GS
    RtPgs* pgs = rt_gs_parallel_handle();
    return pgs && rt_pgs_window_handle(pgs) != nullptr;
#else
    return false;
#endif
}

void print_usage(const char* argv0) {
    std::printf(
        "usage: %s [--disc <path>] [--no-launcher]\n"
        "\n"
        "  --disc <path>   ICO (US) disc image to mount (.iso, or the .bin of a\n"
        "                  bin/cue rip). Without it the runtime looks for a disc\n"
        "                  per the order in src/runtime/iso/iso9660.h (config,\n"
        "                  decomp checkout, then ico.iso next to the exe).\n"
        "  --no-launcher   skip the launcher window and boot the disc directly\n"
        "\n"
        "Behavior toggles are environment variables (ICORECOMP_GS, ICORECOMP_MAX_VBLANKS,\n"
        "ICORECOMP_INPUT_SCRIPT, ICORECOMP_SAVES_DIR, ...); see the runtime sources or\n"
        "the packaged README.txt.\n",
        argv0);
}

} // namespace

bool rt_no_launcher_flag() { return g_no_launcher; }

int main(int argc, char** argv) {
    /* The log file has to exist before anything can log to it, including
     * settings.json loading itself, so this peeks debug.log_file directly
     * off disk instead of waiting for rt_settings_init() below. */
    rt_log_init(rt_base_dir(), rt_settings_peek_log_file());
    rt_prof_init();
    /* Installed before anything that can fault, which now includes the
     * Vulkan device and window setup in rt_hw_init: a crash there used to
     * die with no dump at all. */
    install_crash_handler();

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--disc") == 0 && i + 1 < argc) {
            rt_iso_set_path(argv[++i]);
        } else if (std::strncmp(arg, "--disc=", 7) == 0) {
            rt_iso_set_path(arg + 7);
        } else if (std::strcmp(arg, "--no-launcher") == 0) {
            g_no_launcher = true;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stdout, "unknown argument: %s\n", arg);
            print_usage(argv[0]);
            rt_log_hold_console();
            return 2;
        }
    }

    set_fpu_ftz_daz();

    /* After the log sink, profiler and crash handler are up, and after
     * argument parsing so --disc has already latched, but before anything
     * that reads a setting (mem init has none today, but the GS backend and
     * audio init later in this sequence do). ICORECOMP_VERBOSE always wins
     * over debug.verbose; only apply the settings value when the env var
     * was never set, matching every other env-twin consumer in this
     * codebase (see rt_log_set_verbose's own comment). */
    rt_settings_init();
    if (!std::getenv("ICORECOMP_VERBOSE") && !rt_settings().debug.verbose.empty()) {
        rt_log_set_verbose(rt_settings().debug.verbose.c_str());
    }

    /* ---- launcher gate ---------------------------------------------------
     *
     * Which of the two orderings below this run uses. Every condition is a
     * reason the launcher would be wrong, not merely unnecessary:
     *   - no UI in this build: there is nothing to show.
     *   - ICORECOMP_INPUT_SCRIPT: a scripted run drives the pad from a file
     *     and must stay reproducible; a window waiting for a click is not.
     *   - ICORECOMP_MAX_VBLANKS: a bounded diagnostic run, expected to boot
     *     and exit on its own.
     *   - --no-launcher, and launcher.show_at_startup: the user said so.
     *   - no live windowed backend: a dump-only or headless run has nothing
     *     to draw the launcher into. That one can only be answered after
     *     rt_hw_init(), so the two init calls it needs happen first and the
     *     rest of the sequence below skips them.
     */
    bool launcher = false;
    const char* launcher_reason = "UI built in, live window, no bypass, launcher.show_at_startup";
#ifdef ICORECOMP_UI
    if (std::getenv("ICORECOMP_INPUT_SCRIPT")) {
        launcher_reason = "ICORECOMP_INPUT_SCRIPT is set";
    } else if (std::getenv("ICORECOMP_MAX_VBLANKS")) {
        launcher_reason = "ICORECOMP_MAX_VBLANKS is set";
    } else if (rt_no_launcher_flag()) {
        launcher_reason = "--no-launcher";
    } else if (!rt_settings().launcher.show_at_startup) {
        launcher_reason = "launcher.show_at_startup is false";
    } else {
        launcher = true;
    }
#else
    launcher_reason = "this build has no UI (ICORECOMP_UI=OFF)";
#endif

    bool mem_ready = false, hw_ready = false, ui_ready = false;
    if (launcher) {
        rt_mem_init();
        mem_ready = true;
        rt_hw_init();
        hw_ready = true;
        if (!live_windowed_backend()) {
            launcher = false;
            launcher_reason = "no live windowed backend in this run";
        }
    }
    rt_log("main", "launcher gate: %s (%s)",
        launcher ? "launcher first" : "boot straight into the game", launcher_reason);

    if (launcher) {
        rt_ui_init();
        ui_ready = true;
        /* Returns only when the boot precheck passed and Start was pressed:
         * the disc is mounted and the boot ELF is already read and
         * pin-checked, so the rt_load_elf below reuses it. Quit and window
         * close leave the process here, with nothing loaded. */
        if (!rt_launcher_run()) {
            rt_log("main", "launcher exited without starting the game");
            rt_log_hold_console();
            return 0;
        }
    }

    if (!mem_ready) rt_mem_init();

    LoaderConfig cfg;
    if (!rt_load_config(&cfg)) {
        rt_fatal("main", nullptr, "could not load config/recomp.toml");
    }
    rt_load_elf(cfg);

#ifdef ICORECOMP_HAVE_GENERATED
    g_functab_init();
    rt_log("main", "generated code linked: g_functab_init() called");
#else
    rt_log("main", "stub build: no generated/ee linked; g_functab is empty");
#endif

    if (cfg.entry < RECOMP_TEXT_BASE || cfg.entry >= RECOMP_TEXT_LIMIT) {
        rt_fatal("main", nullptr, "config [target].entry 0x%08x is outside the function table range [0x%08x, 0x%08x)",
            cfg.entry, RECOMP_TEXT_BASE, RECOMP_TEXT_LIMIT);
    }
    uint32_t entry_idx = RECOMP_FUNC_IDX(cfg.entry);
    recomp_fn_t entry_fn = g_functab[entry_idx];
    if (!entry_fn) {
#ifdef ICORECOMP_HAVE_GENERATED
        rt_log("main", "FATAL: entry function at vram 0x%08x not found in g_functab after g_functab_init()"
                        " -- generated code does not cover this address", cfg.entry);
#else
        rt_log("main", "FATAL: no generated code linked (stub build); entry function at vram 0x%08x cannot be"
                        " resolved. Configure with generated/ee present (or -DGENERATED_DIR=...) to run the game.", cfg.entry);
#endif
        rt_log_hold_console();
        return 1;
    }

    (void)entry_fn; /* thread 1's trampoline looks it up again via g_functab */

    /* Creates the GS backend (ICORECOMP_GS selects dump / parallel / both;
     * see gs/gs_select.cpp) on the main thread stack. For the live backend
     * this brings up the Vulkan device and, when a display is available,
     * the window + swapchain, before any guest code exists.
     *
     * Deliberately last on this path: everything above can fail cheaply and
     * often does on a fresh install (no disc image, wrong disc, SHA-1 pin
     * mismatch, no generated code). Opening the window first turned each of
     * those into a black window that vanished, which reads as a crash
     * rather than the specific error it is. That is the failure ordering
     * for a run with no launcher, and it is why the calls above come first
     * here.
     *
     * The launcher path inverts it deliberately: it opens the window before
     * any of them, because rt_boot_precheck (loader.cpp) runs exactly those
     * same steps and reports each failure inside the window, by name,
     * instead of on a console the user may never see. Nothing is lost, the
     * failures just arrive somewhere the user is already looking. Both
     * calls are skipped here when the launcher path already made them. */
    if (!hw_ready) rt_hw_init();

    /* The UI needs the window (and its surface size) to exist, so it comes
     * up right after the backend. A scripted run bypasses the UI entirely
     * (settings plan): ICORECOMP_INPUT_SCRIPT drives the pad from a file and
     * must stay reproducible, which a menu that eats input would break. */
    if (!ui_ready && !std::getenv("ICORECOMP_INPUT_SCRIPT")) rt_ui_init();

    rt_sched_init();

    rt_log("main", "booting scheduler: thread 1 entry vram=0x%08x sp=0x%08x gp=0x%08x",
        cfg.entry, uint32_t(RT_RAM_SIZE - 0x10000), cfg.gp);

    /* Creates guest thread 1 at priority 0 running the translated entry and
     * never returns: the process ends via the Exit syscall, a fatal, or the
     * clean-exit sentinel. Initial $sp matches P1 (top of RAM minus 64 KB;
     * crt0's RFU060 declares the real stack). */
    rt_sched_boot(cfg.entry, cfg.gp, RT_RAM_SIZE - 0x10000);
}
