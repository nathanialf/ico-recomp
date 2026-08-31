/* log.cpp: shared logging and register-dump helpers. */
#include "runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

void rt_vlog(const char* component, const char* fmt, va_list ap) {
    std::fprintf(stderr, "[icorecomp][%s] ", component);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

void rt_log(const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlog(component, fmt, ap);
    va_end(ap);
}

static const char* const kGprNames[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

void rt_dump_registers(const R5900Context* ctx) {
    if (!ctx) {
        std::fprintf(stderr, "[icorecomp][regs] (null context)\n");
        return;
    }
    std::fprintf(stderr, "[icorecomp][regs] pc_hint=0x%08x\n", ctx->pc_hint);
    for (int i = 0; i < 32; i += 2) {
        const rc_u128& a = ctx->r[i];
        const rc_u128& b = ctx->r[i + 1];
        std::fprintf(stderr,
            "  %2d %-4s = %016llx:%016llx   %2d %-4s = %016llx:%016llx\n",
            i, kGprNames[i], (unsigned long long)a.u64x[1], (unsigned long long)a.u64x[0],
            i + 1, kGprNames[i + 1], (unsigned long long)b.u64x[1], (unsigned long long)b.u64x[0]);
    }
    std::fprintf(stderr, "  lo = %016llx:%016llx   hi = %016llx:%016llx\n",
        (unsigned long long)ctx->lo.u64x[1], (unsigned long long)ctx->lo.u64x[0],
        (unsigned long long)ctx->hi.u64x[1], (unsigned long long)ctx->hi.u64x[0]);
    std::fprintf(stderr, "  sa = 0x%08x  fcr31 = 0x%08x\n", ctx->sa, ctx->fcr31);
    /* Guest stack scan: words in [sp, sp+0x800) that look like text
     * addresses. Not a real unwind, but with ra it usually names the
     * caller chain well enough to locate a fault. */
    uint32_t sp = (uint32_t)ctx->r[29].u64x[0];
    if (sp >= 0x1000 && sp < 0x02000000) {
        std::fprintf(stderr, "[icorecomp][regs] stack scan from sp=0x%08x:\n", sp);
        int shown = 0;
        for (uint32_t a = sp & ~3u; a < sp + 0x800 && shown < 24; a += 4) {
            uint8_t* page = g_pages[a >> 16];
            if (!page) break;
            uint32_t v;
            std::memcpy(&v, page + (a & 0xFFFF), 4);
            if (v >= 0x00100000 && v < 0x00300000 && (v & 3) == 0) {
                std::fprintf(stderr, "    [sp+0x%03x] 0x%08x\n", a - sp, v);
                ++shown;
            }
        }
    }
    std::fflush(stderr);
}

[[noreturn]] void rt_fatal(const char* component, const R5900Context* ctx, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[icorecomp][%s] FATAL: ", component);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
    if (ctx) rt_dump_registers(ctx);
    std::fflush(stderr);
    std::exit(1);
}
