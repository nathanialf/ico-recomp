/* recomp_ops.h: static inline helpers used by generated code AND the
 * reference interpreter. Both must use these exact implementations so the
 * three-way verification (interpreter vs compiled emit vs calibrated vectors)
 * compares like against like.
 *
 * Conventions:
 *  - 32-bit results are sign-extended to 64 per MIPS64 (RC_SE32).
 *  - Guest loads/stores go through the rc_readN/rc_writeN helpers below;
 *    lq/sq mask the
 *    address to 16-byte alignment (hardware behavior).
 *  - FPU tier 0: host float + clamp. Arithmetic never produces NaN/Inf:
 *    results that would be Inf/NaN clamp to +/-FMAX with the IEEE result's
 *    sign; div-by-zero yields the PS2 +/-0x7FFFFFFF bit pattern. Raw loads
 *    (mtc1/lwc1/qmtc2/lqc2) can still place exponent-255 bit patterns in
 *    registers; arithmetic inputs read them as +/-FMAX (rc_fin/rc_vu_in),
 *    matching the PS2's NaN-free number line and keeping host results
 *    deterministic.
 *  - The runtime sets FTZ+DAZ (MXCSR / FPCR) on every guest-executing thread.
 *
 * This file grows with the emitter; helpers are added as ops are implemented,
 * never speculatively.
 */
#ifndef RECOMP_OPS_H
#define RECOMP_OPS_H

#include <math.h>
#include <string.h>
#include "recomp_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define RC_INLINE static __forceinline
#else
#define RC_INLINE static inline __attribute__((always_inline))
#endif

/* ---- integer helpers --------------------------------------------------- */

RC_INLINE int64_t RC_SE32(int32_t v) { return (int64_t)v; }
RC_INLINE uint32_t RC_ADD32(uint32_t a, uint32_t b) { return a + b; }
RC_INLINE uint32_t RC_SUB32(uint32_t a, uint32_t b) { return a - b; }

/* ---- guest memory ------------------------------------------------------ */

RC_INLINE uint8_t* rc_page(uint32_t addr) { return g_pages[addr >> 16]; }

RC_INLINE uint8_t rc_read8(uint32_t a) {
    uint8_t* p = rc_page(a);
    return p ? p[a & 0xFFFF] : rt_mmio_read8(a);
}
RC_INLINE uint16_t rc_read16(uint32_t a) {
    uint8_t* p = rc_page(a);
    uint16_t v;
    if (!p) return rt_mmio_read16(a);
    memcpy(&v, p + (a & 0xFFFE), 2);
    return v;
}
RC_INLINE uint32_t rc_read32(uint32_t a) {
    uint8_t* p = rc_page(a);
    uint32_t v;
    if (!p) return rt_mmio_read32(a);
    memcpy(&v, p + (a & 0xFFFC), 4);
    return v;
}
RC_INLINE uint64_t rc_read64(uint32_t a) {
    uint8_t* p = rc_page(a);
    uint64_t v;
    if (!p) return rt_mmio_read64(a);
    memcpy(&v, p + (a & 0xFFF8), 8);
    return v;
}
RC_INLINE rc_u128 rc_read128(uint32_t a) {
    uint8_t* p = rc_page(a);
    rc_u128 v;
    if (!p) return rt_mmio_read128(a);
    memcpy(&v, p + (a & 0xFFF0), 16); /* lq masks to 16-byte alignment */
    return v;
}
RC_INLINE void rc_write8(uint32_t a, uint8_t v) {
    uint8_t* p = rc_page(a);
    if (p) p[a & 0xFFFF] = v; else rt_mmio_write8(a, v);
}
RC_INLINE void rc_write16(uint32_t a, uint16_t v) {
    uint8_t* p = rc_page(a);
    if (p) memcpy(p + (a & 0xFFFE), &v, 2); else rt_mmio_write16(a, v);
}
RC_INLINE void rc_write32(uint32_t a, uint32_t v) {
    uint8_t* p = rc_page(a);
    if (p) memcpy(p + (a & 0xFFFC), &v, 4); else rt_mmio_write32(a, v);
}
RC_INLINE void rc_write64(uint32_t a, uint64_t v) {
    uint8_t* p = rc_page(a);
    if (p) memcpy(p + (a & 0xFFF8), &v, 8); else rt_mmio_write64(a, v);
}
RC_INLINE void rc_write128(uint32_t a, rc_u128 v) {
    uint8_t* p = rc_page(a);
    if (p) memcpy(p + (a & 0xFFF0), &v, 16); else rt_mmio_write128(a, v);
}

/* ---- multiply / divide -------------------------------------------------
 * Shared by generated code and the interpreter so LO/HI results match
 * bit-exactly. lo/hi point at the selected pipeline's 64-bit slot:
 * pipeline 0 is lo.u64x[0]/hi.u64x[0], pipeline 1 (mult1/div1/madd1) is
 * lo.u64x[1]/hi.u64x[1]. 32-bit halves are sign-extended per MIPS64. */

RC_INLINE void rc_mult(uint64_t* lo, uint64_t* hi, int32_t a, int32_t b) {
    int64_t p = (int64_t)a * (int64_t)b;
    *lo = (uint64_t)RC_SE32((int32_t)p);
    *hi = (uint64_t)RC_SE32((int32_t)(p >> 32));
}
RC_INLINE void rc_multu(uint64_t* lo, uint64_t* hi, uint32_t a, uint32_t b) {
    uint64_t p = (uint64_t)a * (uint64_t)b;
    *lo = (uint64_t)RC_SE32((int32_t)p);
    *hi = (uint64_t)RC_SE32((int32_t)(p >> 32));
}
/* div by zero: lo = (a >= 0 ? -1 : 1), hi = a. INT_MIN / -1: lo = INT_MIN,
 * hi = 0. Matches the R5900 (no exception is raised). */
RC_INLINE void rc_div(uint64_t* lo, uint64_t* hi, int32_t a, int32_t b) {
    if (b == 0) {
        *lo = (uint64_t)RC_SE32(a >= 0 ? -1 : 1);
        *hi = (uint64_t)RC_SE32(a);
    } else if (a == (int32_t)0x80000000 && b == -1) {
        *lo = (uint64_t)RC_SE32((int32_t)0x80000000);
        *hi = 0;
    } else {
        *lo = (uint64_t)RC_SE32(a / b);
        *hi = (uint64_t)RC_SE32(a % b);
    }
}
RC_INLINE void rc_divu(uint64_t* lo, uint64_t* hi, uint32_t a, uint32_t b) {
    if (b == 0) {
        *lo = (uint64_t)RC_SE32(-1);
        *hi = (uint64_t)RC_SE32((int32_t)a);
    } else {
        *lo = (uint64_t)RC_SE32((int32_t)(a / b));
        *hi = (uint64_t)RC_SE32((int32_t)(a % b));
    }
}
/* madd: {hi[31:0], lo[31:0]} + a*b, results written back sign-extended. */
RC_INLINE void rc_madd(uint64_t* lo, uint64_t* hi, int32_t a, int32_t b) {
    int64_t acc = (int64_t)(((uint64_t)(uint32_t)*hi << 32) | (uint32_t)*lo);
    int64_t p = acc + (int64_t)a * (int64_t)b;
    *lo = (uint64_t)RC_SE32((int32_t)p);
    *hi = (uint64_t)RC_SE32((int32_t)(p >> 32));
}

/* ---- unaligned loads / stores (little-endian R5900) --------------------
 * cur is the destination register's current 64-bit value; the return value
 * is the merged register result. lwl sign-extends the merged 32-bit value
 * (it always writes the sign byte); lwr sign-extends only in the aligned
 * case (equivalent to lw) and otherwise preserves cur's upper 32 bits,
 * relying on the paired lwl for the final sign extension. */

RC_INLINE uint64_t rc_lwl(uint32_t a, uint64_t cur) {
    uint32_t b = a & 3;
    uint32_t w = rc_read32(a);
    uint32_t m = 0x00FFFFFFu >> (b * 8);
    return (uint64_t)RC_SE32((int32_t)(((uint32_t)cur & m) | (w << (24 - b * 8))));
}
RC_INLINE uint64_t rc_lwr(uint32_t a, uint64_t cur) {
    uint32_t b = a & 3;
    uint32_t w = rc_read32(a);
    if (b == 0) return (uint64_t)RC_SE32((int32_t)w);
    uint32_t m = ~(0xFFFFFFFFu >> (b * 8));
    uint32_t r = ((uint32_t)cur & m) | (w >> (b * 8));
    return (cur & 0xFFFFFFFF00000000ull) | r;
}
RC_INLINE uint64_t rc_ldl(uint32_t a, uint64_t cur) {
    uint32_t b = a & 7;
    uint64_t d = rc_read64(a);
    uint64_t m = 0x00FFFFFFFFFFFFFFull >> (b * 8);
    return (cur & m) | (d << (56 - b * 8));
}
RC_INLINE uint64_t rc_ldr(uint32_t a, uint64_t cur) {
    uint32_t b = a & 7;
    uint64_t d = rc_read64(a);
    if (b == 0) return d;
    uint64_t m = ~(0xFFFFFFFFFFFFFFFFull >> (b * 8));
    return (cur & m) | (d >> (b * 8));
}
RC_INLINE void rc_swl(uint32_t a, uint32_t v) {
    uint32_t b = a & 3;
    uint32_t w = rc_read32(a);
    uint32_t m = 0xFFFFFF00u << (b * 8);
    rc_write32(a, (w & m) | (v >> (24 - b * 8)));
}
RC_INLINE void rc_swr(uint32_t a, uint32_t v) {
    uint32_t b = a & 3;
    uint32_t w = rc_read32(a);
    uint32_t m = ~(0xFFFFFFFFu << (b * 8));
    rc_write32(a, (w & m) | (v << (b * 8)));
}
RC_INLINE void rc_sdl(uint32_t a, uint64_t v) {
    uint32_t b = a & 7;
    uint64_t d = rc_read64(a);
    uint64_t m = 0xFFFFFFFFFFFFFF00ull << (b * 8);
    rc_write64(a, (d & m) | (v >> (56 - b * 8)));
}
RC_INLINE void rc_sdr(uint32_t a, uint64_t v) {
    uint32_t b = a & 7;
    uint64_t d = rc_read64(a);
    uint64_t m = ~(0xFFFFFFFFFFFFFFFFull << (b * 8));
    rc_write64(a, (d & m) | (v << (b * 8)));
}

/* ---- qfsrv -------------------------------------------------------------
 * Funnel shift right of the 256-bit {rs || rt} by ctx->sa bytes (sa is
 * byte-granular per recomp_context.h). Byte positions past the funnel
 * read as zero (sa is masked to 0..31). */
RC_INLINE rc_u128 rc_qfsrv(rc_u128 rs, rc_u128 rt, uint32_t sa_bytes) {
    rc_u128 d;
    uint32_t s = sa_bytes & 0x1F;
    for (int i = 0; i < 16; i++) {
        uint32_t j = (uint32_t)i + s;
        d.u8x[i] = j < 16 ? rt.u8x[j] : (j < 32 ? rs.u8x[j - 16] : 0);
    }
    return d;
}

/* ---- FPU tier 0 -------------------------------------------------------- */

#define RC_PS2_FMAX_BITS 0x7F7FFFFFu
#define RC_PS2_DIV0_BITS 0x7FFFFFFFu

RC_INLINE uint32_t rc_f2bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
RC_INLINE float rc_bits2f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

/* Clamp an IEEE result to PS2 FPU range: Inf/NaN -> +/-FMAX keeping sign. */
RC_INLINE float rc_fclamp(float f) {
    uint32_t b = rc_f2bits(f);
    if ((b & 0x7F800000u) == 0x7F800000u)
        b = (b & 0x80000000u) | RC_PS2_FMAX_BITS;
    return rc_bits2f(b);
}

/* Arithmetic input sanitizer: exponent-255 bit patterns (IEEE NaN/Inf) read
 * as +/-FMAX. The PS2 FPU has no NaN or Inf; such patterns only enter guest
 * state through raw loads (mtc1/lwc1) and behave as large finite
 * magnitudes on hardware. Sanitizing also keeps results deterministic: host
 * NaN sign/payload propagation depends on the operand order the compiler
 * happens to emit for commutative ops. */
RC_INLINE float rc_fin(float f) {
    uint32_t b = rc_f2bits(f);
    if ((b & 0x7F800000u) == 0x7F800000u)
        b = (b & 0x80000000u) | RC_PS2_FMAX_BITS;
    return rc_bits2f(b);
}

RC_INLINE float rc_fadd(float a, float b) { return rc_fclamp(rc_fin(a) + rc_fin(b)); }
RC_INLINE float rc_fsub(float a, float b) { return rc_fclamp(rc_fin(a) - rc_fin(b)); }
RC_INLINE float rc_fmul(float a, float b) { return rc_fclamp(rc_fin(a) * rc_fin(b)); }
RC_INLINE float rc_fdiv(float a, float b) {
    if ((rc_f2bits(b) & 0x7FFFFFFFu) == 0) {
        uint32_t sign = (rc_f2bits(a) ^ rc_f2bits(b)) & 0x80000000u;
        return rc_bits2f(sign | RC_PS2_DIV0_BITS);
    }
    return rc_fclamp(rc_fin(a) / rc_fin(b));
}
RC_INLINE float rc_fneg(float a) { return rc_bits2f(rc_f2bits(a) ^ 0x80000000u); }
RC_INLINE float rc_fabs_(float a) { return rc_bits2f(rc_f2bits(a) & 0x7FFFFFFFu); }

/* cvt.w.s: EE truncates toward zero regardless of FCR31, saturating. */
RC_INLINE int32_t rc_cvtws(float f) {
    if (f >= 2147483648.0f) return 0x7FFFFFFF;
    if (f < -2147483648.0f) return (int32_t)0x80000000;
    if (f != f) return 0x7FFFFFFF; /* unreachable after clamping; defensive */
    return (int32_t)f;
}
RC_INLINE float rc_cvtsw(int32_t v) { return (float)v; }

/* mov.s and the raw bit moves (mtc1/lwc1) go through the bits so a NaN
 * pattern loaded from guest memory is never altered by a float copy. */
RC_INLINE float rc_fmov(float a) { return rc_bits2f(rc_f2bits(a)); }

/* c.eq/c.lt/c.le: plain IEEE compares. Post-clamp arithmetic never produces
 * NaN, but NaN bit patterns can still enter f[] through mtc1/lwc1; host
 * compares then answer false. Registered in the uncertainty list in
 * ee-interp/tests/threeway.rs (PS2 hardware compares exponent-255 patterns
 * as huge finite numbers). */
RC_INLINE int rc_fc_eq(float a, float b) { return a == b; }
RC_INLINE int rc_fc_lt(float a, float b) { return a < b; }
RC_INLINE int rc_fc_le(float a, float b) { return a <= b; }

/* FCR31 condition bit (bit 23), read by bc1f/bc1t. */
RC_INLINE void rc_fcr31_cond(uint32_t* fcr31, int cond) {
    if (cond) *fcr31 |= 0x00800000u;
    else *fcr31 &= ~0x00800000u;
}

/* ---- VU0 macro mode (COP2) ---------------------------------------------
 * Semantics shared by the emitter's generated calls and the reference
 * interpreter. Guest data can contain arbitrary bit patterns (NaN/Inf enter
 * vf registers through lqc2/qmtc2), so every helper is total and
 * deterministic on any input bits.
 *
 * IMPORTANT: compile with FP contraction off (-std=c11 does this on gcc,
 * or pass -ffp-contract=off). madd/msub must round the product before the
 * add or the three-way verification breaks.
 *
 * Flag model (VU User's Manual):
 *   mac:    bits 15..12 O, 11..8 U, 7..4 S, 3..0 Z. Within each nibble x is
 *           the MSB (x=bit3, y=bit2, z=bit1, w=bit0), matching the dest
 *           mask bit order.
 *   status: bit0 Z, bit1 S, bit2 U, bit3 O, bit4 I, bit5 D; bits 6..11 are
 *           the sticky copies in the same order.
 * FMAC ops recompute mac and the non-sticky ZSUO bits (unselected lanes
 * clear), OR the new bits into the sticky set, and leave I/D alone. The
 * div/sqrt unit rewrites I/D (+sticky) and leaves mac alone. vmax/vmini,
 * vmove/vmr32, vftoi/vitof and the loads/stores set no flags.
 *
 * Number model matches FPU tier 0 plus explicit flush-to-zero so behavior
 * does not depend on the host MXCSR: results with exponent 255 clamp to
 * +/-FMAX and set O; denormal results flush to signed zero and set U;
 * denormal inputs are read as signed zero.
 *
 * vf00 reads as (0, 0, 0, 1.0f) and writes to it are suppressed. vi0 reads
 * as 0 and writes to it (including pointer increments) are suppressed. */

/* Broadcast selector for the third FMAC operand. */
#define RC_VU_SRC_VEC 4 /* full vector */
#define RC_VU_SRC_Q 5   /* Q register splat */

/* Macro-mode vlqi/vlqd/vsqi address VU0 data memory (4 KB, 256 quadwords,
 * wrapping) through its EE window so the runtime backs it with g_pages. */
#define RC_VU0_MEM_BASE 0x11004000u

/* Input sanitizer: denormals read as signed zero (hardware DAZ) and
 * exponent-255 patterns read as +/-FMAX (see rc_fin: the VU has no NaN or
 * Inf either, and host NaN propagation is operand-order dependent). */
RC_INLINE float rc_vu_in(float f) {
    uint32_t b = rc_f2bits(f);
    if ((b & 0x7F800000u) == 0) b &= 0x80000000u;
    else if ((b & 0x7F800000u) == 0x7F800000u)
        b = (b & 0x80000000u) | RC_PS2_FMAX_BITS;
    return rc_bits2f(b);
}

/* Clamp + flush without flag reporting (Q register results). */
RC_INLINE float rc_vu_fclamp(float f) {
    uint32_t b = rc_f2bits(f);
    uint32_t e = b & 0x7F800000u;
    if (e == 0x7F800000u) b = (b & 0x80000000u) | RC_PS2_FMAX_BITS;
    else if (e == 0) b &= 0x80000000u;
    return rc_bits2f(b);
}

RC_INLINE rc_u128 rc_vu_vf(const Vu0State* vu, int n) {
    if (n != 0) return vu->vf[n];
    rc_u128 v;
    v.u32x[0] = 0;
    v.u32x[1] = 0;
    v.u32x[2] = 0;
    v.u32x[3] = 0x3F800000u; /* 1.0f */
    return v;
}

RC_INLINE uint32_t rc_vu_vi(const Vu0State* vu, int n) {
    return n != 0 ? vu->vi[n & 15] : 0;
}

/* Expand the third FMAC operand: lane 0..3 broadcast, full vector, or Q. */
RC_INLINE rc_u128 rc_vu_src(const Vu0State* vu, int ft, int sel) {
    if (sel == RC_VU_SRC_VEC) return rc_vu_vf(vu, ft);
    float f = sel == RC_VU_SRC_Q ? vu->q : rc_vu_vf(vu, ft).f32x[sel & 3];
    rc_u128 s;
    s.f32x[0] = f;
    s.f32x[1] = f;
    s.f32x[2] = f;
    s.f32x[3] = f;
    return s;
}

/* Clamp/flush one lane result and accumulate its mac nibble bits. */
RC_INLINE float rc_vu_flagres(uint32_t* mac, int lane, float f) {
    uint32_t b = rc_f2bits(f);
    uint32_t e = b & 0x7F800000u;
    int sh = 3 - lane;
    if (e == 0x7F800000u) {
        b = (b & 0x80000000u) | RC_PS2_FMAX_BITS; /* overflow */
        *mac |= 0x1000u << sh;
    } else if (e == 0 && (b & 0x007FFFFFu) != 0) {
        b &= 0x80000000u; /* underflow: flush to signed zero */
        *mac |= 0x0100u << sh;
    }
    if ((b & 0x7FFFFFFFu) == 0) *mac |= 0x0001u << sh;
    if (b & 0x80000000u) *mac |= 0x0010u << sh;
    return rc_bits2f(b);
}

/* Commit mac and derive the status ZSUO bits (+sticky). I/D untouched. */
RC_INLINE void rc_vu_flags_commit(Vu0State* vu, uint32_t mac) {
    uint32_t st = vu->status & 0xFF0u;
    vu->mac = mac & 0xFFFFu;
    if (mac & 0x000Fu) st |= 0x001u;
    if (mac & 0x00F0u) st |= 0x002u;
    if (mac & 0x0F00u) st |= 0x004u;
    if (mac & 0xF000u) st |= 0x008u;
    st |= (st & 0x00Fu) << 6;
    vu->status = st;
}

/* Masked vf write; the vf00 invariant makes writes to register 0 no-ops. */
RC_INLINE void rc_vu_write(Vu0State* vu, int fd, int mask, rc_u128 v) {
    if (fd == 0) return;
    for (int i = 0; i < 4; i++)
        if (mask & (8 >> i)) vu->vf[fd].u32x[i] = v.u32x[i];
}

/* FMAC core. kind: 0 add, 1 sub, 2 mul, 3 madd, 4 msub. acc_dst nonzero
 * writes ACC instead of vf[fd]. */
RC_INLINE void rc_vu_fmac(Vu0State* vu, int kind, int acc_dst, int fd,
                          int fs, int ft, int sel, int mask) {
    rc_u128 a = rc_vu_vf(vu, fs);
    rc_u128 b = rc_vu_src(vu, ft, sel);
    uint32_t mac = 0;
    rc_u128 r;
    r.u32x[0] = 0;
    r.u32x[1] = 0;
    r.u32x[2] = 0;
    r.u32x[3] = 0;
    for (int i = 0; i < 4; i++) {
        if (!(mask & (8 >> i))) continue;
        float x = rc_vu_in(a.f32x[i]);
        float y = rc_vu_in(b.f32x[i]);
        float v;
        switch (kind) {
        case 0: v = x + y; break;
        case 1: v = x - y; break;
        case 2: v = x * y; break;
        case 3: v = rc_vu_in(vu->acc.f32x[i]) + x * y; break;
        default: v = rc_vu_in(vu->acc.f32x[i]) - x * y; break;
        }
        r.f32x[i] = rc_vu_flagres(&mac, i, v);
    }
    rc_vu_flags_commit(vu, mac);
    if (acc_dst) {
        for (int i = 0; i < 4; i++)
            if (mask & (8 >> i)) vu->acc.u32x[i] = r.u32x[i];
    } else {
        rc_vu_write(vu, fd, mask, r);
    }
}

RC_INLINE void rc_vu_add(Vu0State* vu, int fd, int fs, int ft, int sel, int mask) {
    rc_vu_fmac(vu, 0, 0, fd, fs, ft, sel, mask);
}
RC_INLINE void rc_vu_sub(Vu0State* vu, int fd, int fs, int ft, int sel, int mask) {
    rc_vu_fmac(vu, 1, 0, fd, fs, ft, sel, mask);
}
RC_INLINE void rc_vu_mul(Vu0State* vu, int fd, int fs, int ft, int sel, int mask) {
    rc_vu_fmac(vu, 2, 0, fd, fs, ft, sel, mask);
}
RC_INLINE void rc_vu_madd(Vu0State* vu, int fd, int fs, int ft, int sel, int mask) {
    rc_vu_fmac(vu, 3, 0, fd, fs, ft, sel, mask);
}
RC_INLINE void rc_vu_msub(Vu0State* vu, int fd, int fs, int ft, int sel, int mask) {
    rc_vu_fmac(vu, 4, 0, fd, fs, ft, sel, mask);
}
RC_INLINE void rc_vu_adda(Vu0State* vu, int fs, int ft, int sel, int mask) {
    rc_vu_fmac(vu, 0, 1, 0, fs, ft, sel, mask);
}
RC_INLINE void rc_vu_mula(Vu0State* vu, int fs, int ft, int sel, int mask) {
    rc_vu_fmac(vu, 2, 1, 0, fs, ft, sel, mask);
}
RC_INLINE void rc_vu_madda(Vu0State* vu, int fs, int ft, int sel, int mask) {
    rc_vu_fmac(vu, 3, 1, 0, fs, ft, sel, mask);
}

/* vopmula/vopmsub: outer-product terms over xyz, ACC/vfd, flags on xyz. */
RC_INLINE void rc_vu_opmula(Vu0State* vu, int fs, int ft) {
    rc_u128 a = rc_vu_vf(vu, fs);
    rc_u128 b = rc_vu_vf(vu, ft);
    uint32_t mac = 0;
    float r0 = rc_vu_flagres(&mac, 0, rc_vu_in(a.f32x[1]) * rc_vu_in(b.f32x[2]));
    float r1 = rc_vu_flagres(&mac, 1, rc_vu_in(a.f32x[2]) * rc_vu_in(b.f32x[0]));
    float r2 = rc_vu_flagres(&mac, 2, rc_vu_in(a.f32x[0]) * rc_vu_in(b.f32x[1]));
    rc_vu_flags_commit(vu, mac);
    vu->acc.f32x[0] = r0;
    vu->acc.f32x[1] = r1;
    vu->acc.f32x[2] = r2;
}
RC_INLINE void rc_vu_opmsub(Vu0State* vu, int fd, int fs, int ft) {
    rc_u128 a = rc_vu_vf(vu, fs);
    rc_u128 b = rc_vu_vf(vu, ft);
    uint32_t mac = 0;
    rc_u128 r;
    r.u32x[3] = 0;
    r.f32x[0] = rc_vu_flagres(
        &mac, 0, rc_vu_in(vu->acc.f32x[0]) - rc_vu_in(a.f32x[1]) * rc_vu_in(b.f32x[2]));
    r.f32x[1] = rc_vu_flagres(
        &mac, 1, rc_vu_in(vu->acc.f32x[1]) - rc_vu_in(a.f32x[2]) * rc_vu_in(b.f32x[0]));
    r.f32x[2] = rc_vu_flagres(
        &mac, 2, rc_vu_in(vu->acc.f32x[2]) - rc_vu_in(a.f32x[0]) * rc_vu_in(b.f32x[1]));
    rc_vu_flags_commit(vu, mac);
    rc_vu_write(vu, fd, 0xE, r);
}

/* vmax/vmini compare the raw bits as sign-magnitude integers (both negative:
 * smaller signed word wins max). No flags, no DAZ. Handles NaN patterns
 * deterministically; see the uncertainty registry. */
RC_INLINE uint32_t rc_vu_maxbits(uint32_t a, uint32_t b) {
    if ((int32_t)a < 0 && (int32_t)b < 0)
        return (int32_t)a < (int32_t)b ? a : b;
    return (int32_t)a > (int32_t)b ? a : b;
}
RC_INLINE uint32_t rc_vu_minbits(uint32_t a, uint32_t b) {
    if ((int32_t)a < 0 && (int32_t)b < 0)
        return (int32_t)a > (int32_t)b ? a : b;
    return (int32_t)a < (int32_t)b ? a : b;
}
RC_INLINE void rc_vu_maxmin(Vu0State* vu, int is_min, int fd, int fs, int ft,
                            int sel, int mask) {
    rc_u128 a = rc_vu_vf(vu, fs);
    rc_u128 b = rc_vu_src(vu, ft, sel);
    rc_u128 r;
    for (int i = 0; i < 4; i++)
        r.u32x[i] = is_min ? rc_vu_minbits(a.u32x[i], b.u32x[i])
                           : rc_vu_maxbits(a.u32x[i], b.u32x[i]);
    rc_vu_write(vu, fd, mask, r);
}
RC_INLINE void rc_vu_max(Vu0State* vu, int fd, int fs, int ft, int sel, int mask) {
    rc_vu_maxmin(vu, 0, fd, fs, ft, sel, mask);
}
RC_INLINE void rc_vu_mini(Vu0State* vu, int fd, int fs, int ft, int sel, int mask) {
    rc_vu_maxmin(vu, 1, fd, fs, ft, sel, mask);
}

/* vftoi0/vftoi4: truncate toward zero with sign-directed saturation; the
 * shift scales by 2^shift first (fixed point 1:27:4). No flags. */
RC_INLINE int32_t rc_vu_ftoi1(float f, int shift) {
    /* rc_vu_in leaves only finite values, so the scaled saturation below
     * also covers NaN/Inf patterns (they read as +/-FMAX and saturate). */
    float g = rc_vu_in(f) * (float)(1 << shift);
    if (g >= 2147483648.0f) return 0x7FFFFFFF;
    if (g < -2147483648.0f) return (int32_t)0x80000000;
    return (int32_t)g;
}
RC_INLINE void rc_vu_ftoi(Vu0State* vu, int shift, int ft, int fs, int mask) {
    rc_u128 a = rc_vu_vf(vu, fs);
    rc_u128 r;
    for (int i = 0; i < 4; i++) r.s32x[i] = rc_vu_ftoi1(a.f32x[i], shift);
    rc_vu_write(vu, ft, mask, r);
}
RC_INLINE void rc_vu_itof(Vu0State* vu, int shift, int ft, int fs, int mask) {
    rc_u128 a = rc_vu_vf(vu, fs);
    rc_u128 r;
    for (int i = 0; i < 4; i++)
        r.f32x[i] = (float)a.s32x[i] / (float)(1 << shift);
    rc_vu_write(vu, ft, mask, r);
}

RC_INLINE void rc_vu_move(Vu0State* vu, int ft, int fs, int mask) {
    rc_vu_write(vu, ft, mask, rc_vu_vf(vu, fs));
}
RC_INLINE void rc_vu_mr32(Vu0State* vu, int ft, int fs, int mask) {
    rc_u128 a = rc_vu_vf(vu, fs);
    rc_u128 r;
    for (int i = 0; i < 4; i++) r.u32x[i] = a.u32x[(i + 1) & 3];
    rc_vu_write(vu, ft, mask, r);
}

/* Q pipeline (macro mode: results commit immediately, vwaitq is a no-op).
 * idbits: 0x10 sets I (+sticky), 0x20 sets D (+sticky); the non-sticky I/D
 * bits are rewritten on every div-unit op. */
RC_INLINE void rc_vu_qflags(Vu0State* vu, uint32_t idbits) {
    vu->status = (vu->status & ~0x030u) | idbits | (idbits << 6);
}
RC_INLINE void rc_vu_div(Vu0State* vu, int fs, int fsf, int ft, int ftf) {
    uint32_t ab = rc_f2bits(rc_vu_in(rc_vu_vf(vu, fs).f32x[fsf]));
    uint32_t bb = rc_f2bits(rc_vu_in(rc_vu_vf(vu, ft).f32x[ftf]));
    if ((bb & 0x7FFFFFFFu) == 0) {
        /* 0/0 sets I, x/0 sets D; Q becomes +/-FMAX with the xor sign. */
        rc_vu_qflags(vu, (ab & 0x7FFFFFFFu) == 0 ? 0x10u : 0x20u);
        vu->q = rc_bits2f(((ab ^ bb) & 0x80000000u) | RC_PS2_FMAX_BITS);
        return;
    }
    rc_vu_qflags(vu, 0);
    vu->q = rc_vu_fclamp(rc_bits2f(ab) / rc_bits2f(bb));
}
RC_INLINE void rc_vu_sqrt(Vu0State* vu, int ft, int ftf) {
    float b = rc_vu_in(rc_vu_vf(vu, ft).f32x[ftf]);
    rc_vu_qflags(vu, b < 0.0f ? 0x10u : 0u);
    vu->q = rc_vu_fclamp(sqrtf(fabsf(b)));
}
RC_INLINE void rc_vu_rsqrt(Vu0State* vu, int fs, int fsf, int ft, int ftf) {
    uint32_t ab = rc_f2bits(rc_vu_in(rc_vu_vf(vu, fs).f32x[fsf]));
    float b = rc_vu_in(rc_vu_vf(vu, ft).f32x[ftf]);
    uint32_t bb = rc_f2bits(b);
    uint32_t fl = b < 0.0f ? 0x10u : 0u;
    if ((bb & 0x7FFFFFFFu) == 0) {
        fl |= (ab & 0x7FFFFFFFu) == 0 ? 0x10u : 0x20u;
        rc_vu_qflags(vu, fl);
        vu->q = rc_bits2f(((ab ^ bb) & 0x80000000u) | RC_PS2_FMAX_BITS);
        return;
    }
    rc_vu_qflags(vu, fl);
    vu->q = rc_vu_fclamp(rc_bits2f(ab) / sqrtf(fabsf(b)));
}

/* vclipw: shift the 24-bit judgment window left by 6 and or in the new
 * six judgments of fs.xyz against +/-|ft.w|. NaN inputs judge false. */
RC_INLINE void rc_vu_clipw(Vu0State* vu, int fs, int ft) {
    rc_u128 a = rc_vu_vf(vu, fs);
    float w = fabsf(rc_vu_in(rc_vu_vf(vu, ft).f32x[3]));
    uint32_t j = 0;
    for (int i = 0; i < 3; i++) {
        float v = rc_vu_in(a.f32x[i]);
        if (v > w) j |= 1u << (2 * i);
        if (v < -w) j |= 1u << (2 * i + 1);
    }
    vu->clip = ((vu->clip << 6) | j) & 0xFFFFFFu;
}

/* VU0 data memory loads/stores (quadword index, 256 qw wrap). */
RC_INLINE uint32_t rc_vu0_addr(uint32_t qw) {
    return RC_VU0_MEM_BASE + ((qw & 0xFFu) << 4);
}
RC_INLINE void rc_vu_lq(Vu0State* vu, int ft, int mask, uint32_t qw) {
    rc_vu_write(vu, ft, mask, rc_read128(rc_vu0_addr(qw)));
}
RC_INLINE void rc_vu_sq(const Vu0State* vu, int fs, int mask, uint32_t qw) {
    uint32_t a = rc_vu0_addr(qw);
    rc_u128 v = rc_vu_vf(vu, fs);
    for (int i = 0; i < 4; i++)
        if (mask & (8 >> i)) rc_write32(a + 4u * (uint32_t)i, v.u32x[i]);
}
RC_INLINE void rc_vu_lqi(Vu0State* vu, int ft, int is, int mask) {
    rc_vu_lq(vu, ft, mask, rc_vu_vi(vu, is));
    if (is != 0) vu->vi[is] = (uint16_t)(vu->vi[is] + 1);
}
RC_INLINE void rc_vu_lqd(Vu0State* vu, int ft, int is, int mask) {
    if (is != 0) vu->vi[is] = (uint16_t)(vu->vi[is] - 1);
    rc_vu_lq(vu, ft, mask, rc_vu_vi(vu, is));
}
RC_INLINE void rc_vu_sqi(Vu0State* vu, int fs, int it, int mask) {
    rc_vu_sq(vu, fs, mask, rc_vu_vi(vu, it));
    if (it != 0) vu->vi[it] = (uint16_t)(vu->vi[it] + 1);
}

RC_INLINE void rc_vu_iaddi(Vu0State* vu, int it, int is, int imm) {
    if (it != 0) vu->vi[it] = (uint16_t)(rc_vu_vi(vu, is) + (uint32_t)imm);
}

/* R register: 23-bit LFSR (taps 4 and 22) under a fixed 1.0 exponent. */
RC_INLINE void rc_vu_rinit(Vu0State* vu, int fs, int fsf) {
    vu->r = 0x3F800000u | (rc_vu_vf(vu, fs).u32x[fsf] & 0x007FFFFFu);
}
RC_INLINE void rc_vu_rget(Vu0State* vu, int ft, int mask) {
    rc_u128 r;
    for (int i = 0; i < 4; i++) r.u32x[i] = vu->r;
    rc_vu_write(vu, ft, mask, r);
}
RC_INLINE void rc_vu_rnext(Vu0State* vu, int ft, int mask) {
    uint32_t r = vu->r;
    uint32_t x = (r >> 4) & 1u;
    uint32_t y = (r >> 22) & 1u;
    vu->r = 0x3F800000u | (((r << 1) ^ x ^ y) & 0x007FFFFFu);
    rc_vu_rget(vu, ft, mask);
}
RC_INLINE void rc_vu_rxor(Vu0State* vu, int fs, int fsf) {
    vu->r = 0x3F800000u | ((vu->r ^ rc_vu_vf(vu, fs).u32x[fsf]) & 0x007FFFFFu);
}

/* qmfc2/qmtc2 quadword transfers and the EE-address lqc2/sqc2. */
RC_INLINE rc_u128 rc_vu_qmfc(const Vu0State* vu, int fs) { return rc_vu_vf(vu, fs); }
RC_INLINE void rc_vu_qmtc(Vu0State* vu, int fd, rc_u128 v) {
    if (fd != 0) vu->vf[fd] = v;
}
RC_INLINE void rc_vu_lqc2(Vu0State* vu, int ft, uint32_t addr) {
    rc_u128 v = rc_read128(addr); /* read even for vf00 (MMIO side effects) */
    if (ft != 0) vu->vf[ft] = v;
}
RC_INLINE void rc_vu_sqc2(const Vu0State* vu, int fs, uint32_t addr) {
    rc_write128(addr, rc_vu_vf(vu, fs));
}

/* cfc2/ctc2. Known control registers are handled inline; everything else
 * routes to the runtime hooks so unexpected traffic fails loudly. Status
 * writes reach only the sticky bits (write-clear); mac/clip are read-only
 * and their writes go to the hook. */
RC_INLINE uint32_t rc_vu_cfc(R5900Context* ctx, int creg) {
    const Vu0State* vu = &ctx->vu0;
    if (creg == 0) return 0;
    if (creg < 16) return vu->vi[creg];
    switch (creg) {
    case 16: return vu->status & 0xFFFu;
    case 17: return vu->mac & 0xFFFFu;
    case 18: return vu->clip & 0xFFFFFFu;
    case 20: return vu->r;
    case 21: return rc_f2bits(vu->i);
    case 22: return rc_f2bits(vu->q);
    default: return rt_vu0_cfc(ctx, creg);
    }
}
RC_INLINE void rc_vu_ctc(R5900Context* ctx, int creg, uint32_t v) {
    Vu0State* vu = &ctx->vu0;
    if (creg == 0) return;
    if (creg < 16) {
        vu->vi[creg] = (uint16_t)v;
        return;
    }
    switch (creg) {
    case 16: vu->status = (vu->status & 0x03Fu) | (v & 0xFC0u); break;
    case 20: vu->r = 0x3F800000u | (v & 0x007FFFFFu); break;
    case 21: vu->i = rc_bits2f(v); break;
    case 22: vu->q = rc_bits2f(v); break;
    default: rt_vu0_ctc(ctx, creg, v); break;
    }
}

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_OPS_H */
