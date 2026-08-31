/* main.cpp: P1 boot skeleton entry point.
 *
 * Sequence: set FPU FTZ/DAZ -> allocate guest memory + page table -> read
 * config/recomp.toml -> SHA-1-check and load the boot ELF -> wire up
 * generated code (if linked) -> build an initial R5900Context -> call the
 * translated entry point under a crash handler that dumps registers.
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
R5900Context* g_crash_ctx = nullptr;

void crash_handler(int sig) {
    const char* name = "signal";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV"; break;
        case SIGFPE:  name = "SIGFPE"; break;
        case SIGILL:  name = "SIGILL"; break;
        case SIGBUS:  name = "SIGBUS"; break;
        default: break;
    }
    std::fprintf(stderr, "[icorecomp][crash] FATAL: caught %s while running guest code\n", name);
    if (g_crash_ctx) rt_dump_registers(g_crash_ctx);
    std::fflush(stderr);
    _exit(1);
}

void install_crash_handler() {
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGFPE, crash_handler);
    std::signal(SIGILL, crash_handler);
    std::signal(SIGBUS, crash_handler);
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

    R5900Context ctx{};
    ctx.r[29].u64x[0] = uint64_t(RT_RAM_SIZE - 0x10000); /* $sp: top of RAM minus 64 KB; crt0 relocates it */
    ctx.r[28].u64x[0] = uint64_t(cfg.gp);                /* $gp, from config [target].gp */
    ctx.r[31].u64x[0] = uint64_t(RT_CLEAN_EXIT_VRAM);    /* $ra: sentinel, see rt_call_indirect */

    install_crash_handler();
    g_crash_ctx = &ctx;

    rt_log("main", "calling entry: vram=0x%08x sp=0x%08x gp=0x%08x ra(sentinel)=0x%08x",
        cfg.entry, uint32_t(ctx.r[29].u64x[0]), uint32_t(ctx.r[28].u64x[0]), RT_CLEAN_EXIT_VRAM);

    entry_fn(&ctx);

    /* Only reached if the entry function returned via plain C `return`
     * rather than a jr $ra translated through rt_call_indirect (which exits
     * the process itself on the RT_CLEAN_EXIT_VRAM sentinel -- see mem.cpp). */
    rt_log("main", "entry function returned directly (not via the rt_call_indirect sentinel)");
    rt_dump_registers(&ctx);
    return 0;
}
