/* main.cpp: boot entry point.
 *
 * Sequence: set FPU FTZ/DAZ -> allocate guest memory + page table -> read
 * config/recomp.toml -> SHA-1-check and load the boot ELF -> wire up
 * generated code (if linked) -> initialize the P2 kernel HLE (scheduler,
 * INTC, timers, SIF) -> create guest thread 1 running the translated entry
 * point and enter the scheduler loop, under a crash handler that dumps the
 * current guest context.
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
 */
#include "runtime.h"

#include "ee/kernel.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
        case SIGBUS:  name = "SIGBUS"; break;
        default: break;
    }
    std::fprintf(stderr, "[icorecomp][crash] FATAL: caught %s while running guest code (thread %d)\n",
        name, rt_thread_current_id());
    if (rt_sched_current_ctx()) rt_dump_registers(rt_sched_current_ctx());
    std::fflush(stderr);
    _exit(1);
}

/* SIGINT: dump the thread/semaphore inventory before dying so an
 * interactive interrupt of a parked or spinning run is diagnosable. */
void sigint_handler(int) {
    std::fprintf(stderr, "\n[icorecomp][main] SIGINT\n");
    if (rt_sched_current_ctx()) rt_dump_registers(rt_sched_current_ctx());
    rt_sched_dump_inventory("SIGINT");
    _exit(130);
}

void install_crash_handler() {
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGFPE, crash_handler);
    std::signal(SIGILL, crash_handler);
    std::signal(SIGBUS, crash_handler);
    std::signal(SIGINT, sigint_handler);
}

} // namespace

int main() {
    set_fpu_ftz_daz();
    rt_mem_init();

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
        return 1;
    }

    (void)entry_fn; /* thread 1's trampoline looks it up again via g_functab */

    install_crash_handler();
    rt_sched_init();

    rt_log("main", "booting scheduler: thread 1 entry vram=0x%08x sp=0x%08x gp=0x%08x",
        cfg.entry, uint32_t(RT_RAM_SIZE - 0x10000), cfg.gp);

    /* Creates guest thread 1 at priority 0 running the translated entry and
     * never returns: the process ends via the Exit syscall, a fatal, or the
     * clean-exit sentinel. Initial $sp matches P1 (top of RAM minus 64 KB;
     * crt0's RFU060 declares the real stack). */
    rt_sched_boot(cfg.entry, cfg.gp, RT_RAM_SIZE - 0x10000);
}
