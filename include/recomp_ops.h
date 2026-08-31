/* recomp_ops.h: static inline helpers used by generated code AND the
 * reference interpreter. Both must use these exact implementations so the
 * three-way verification (interpreter vs compiled emit vs calibrated vectors)
 * compares like against like.
 *
 * Conventions:
 *  - 32-bit results are sign-extended to 64 per MIPS64 (RC_SE32).
 *  - Guest loads/stores go through rc_read*/rc_write* below; lq/sq mask the
 *    address to 16-byte alignment (hardware behavior).
 *  - FPU tier 0: host float + clamp. No NaN/Inf ever exists in guest state:
 *    results that would be Inf/NaN clamp to +/-FMAX with the IEEE result's
 *    sign; div-by-zero yields the PS2 +/-0x7FFFFFFF bit pattern.
 *  - The runtime sets FTZ+DAZ (MXCSR / FPCR) on every guest-executing thread.
 *
 * This file grows with the emitter; helpers are added as ops are implemented,
 * never speculatively.
 */
#ifndef RECOMP_OPS_H
#define RECOMP_OPS_H

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

RC_INLINE float rc_fadd(float a, float b) { return rc_fclamp(a + b); }
RC_INLINE float rc_fsub(float a, float b) { return rc_fclamp(a - b); }
RC_INLINE float rc_fmul(float a, float b) { return rc_fclamp(a * b); }
RC_INLINE float rc_fdiv(float a, float b) {
    if ((rc_f2bits(b) & 0x7FFFFFFFu) == 0) {
        uint32_t sign = (rc_f2bits(a) ^ rc_f2bits(b)) & 0x80000000u;
        return rc_bits2f(sign | RC_PS2_DIV0_BITS);
    }
    return rc_fclamp(a / b);
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

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_OPS_H */
