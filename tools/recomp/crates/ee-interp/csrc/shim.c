/* shim.c: FFI surface between the Rust reference interpreter and the
 * static inline helpers in recomp_ops.h. Every semantic operation the
 * interpreter performs on guest state goes through this file, so the
 * interpreter and the emitter's generated code execute the exact same
 * helper implementations (compiled by the same gcc with the same flags).
 *
 * The file also provides the test-process guest memory environment:
 * g_pages plus loud aborting stubs for the runtime hooks that recomp_ops.h
 * inlines can reach (MMIO fallthrough and the unknown-vu0-control hooks).
 *
 * Floats cross the FFI as raw u32 bit patterns so no NaN pattern is ever
 * altered by an ABI conversion.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp_ops.h"

/* ---- test-process guest memory environment ------------------------------ */

uint8_t* g_pages[0x10000];

void x_set_page(uint32_t idx, uint8_t* p) { g_pages[idx & 0xFFFFu] = p; }

static void die(const char* what, uint32_t a) {
    fprintf(stderr, "ee-interp shim: unexpected %s (0x%X)\n", what, a);
    abort();
}

uint8_t rt_mmio_read8(uint32_t a) { die("mmio_read8", a); return 0; }
uint16_t rt_mmio_read16(uint32_t a) { die("mmio_read16", a); return 0; }
uint32_t rt_mmio_read32(uint32_t a) { die("mmio_read32", a); return 0; }
uint64_t rt_mmio_read64(uint32_t a) { die("mmio_read64", a); return 0; }
rc_u128 rt_mmio_read128(uint32_t a) {
    die("mmio_read128", a);
    return (rc_u128){{0}};
}
void rt_mmio_write8(uint32_t a, uint8_t v) { (void)v; die("mmio_write8", a); }
void rt_mmio_write16(uint32_t a, uint16_t v) { (void)v; die("mmio_write16", a); }
void rt_mmio_write32(uint32_t a, uint32_t v) { (void)v; die("mmio_write32", a); }
void rt_mmio_write64(uint32_t a, uint64_t v) { (void)v; die("mmio_write64", a); }
void rt_mmio_write128(uint32_t a, rc_u128 v) { (void)v; die("mmio_write128", a); }
uint32_t rt_vu0_cfc(R5900Context* ctx, int creg) {
    (void)ctx;
    die("vu0_cfc", (uint32_t)creg);
    return 0;
}
void rt_vu0_ctc(R5900Context* ctx, int creg, uint32_t v) {
    (void)ctx;
    (void)v;
    die("vu0_ctc", (uint32_t)creg);
}

/* ---- context layout ----------------------------------------------------- */

size_t x_ctx_size(void) { return sizeof(R5900Context); }
size_t x_ctx_align(void) { return _Alignof(R5900Context); }
size_t x_off_r(void) { return offsetof(R5900Context, r); }
size_t x_off_lo(void) { return offsetof(R5900Context, lo); }
size_t x_off_hi(void) { return offsetof(R5900Context, hi); }
size_t x_off_sa(void) { return offsetof(R5900Context, sa); }
size_t x_off_fcr31(void) { return offsetof(R5900Context, fcr31); }
size_t x_off_f(void) { return offsetof(R5900Context, f); }
size_t x_off_vu_vf(void) { return offsetof(R5900Context, vu0.vf); }
size_t x_off_vu_vi(void) { return offsetof(R5900Context, vu0.vi); }
size_t x_off_vu_acc(void) { return offsetof(R5900Context, vu0.acc); }
size_t x_off_vu_q(void) { return offsetof(R5900Context, vu0.q); }
size_t x_off_vu_r(void) { return offsetof(R5900Context, vu0.r); }
size_t x_off_vu_i(void) { return offsetof(R5900Context, vu0.i); }
size_t x_off_vu_status(void) { return offsetof(R5900Context, vu0.status); }
size_t x_off_vu_mac(void) { return offsetof(R5900Context, vu0.mac); }
size_t x_off_vu_clip(void) { return offsetof(R5900Context, vu0.clip); }

/* ---- guest memory ------------------------------------------------------- */

uint8_t x_read8(uint32_t a) { return rc_read8(a); }
uint16_t x_read16(uint32_t a) { return rc_read16(a); }
uint32_t x_read32(uint32_t a) { return rc_read32(a); }
uint64_t x_read64(uint32_t a) { return rc_read64(a); }
void x_read128(uint32_t a, uint8_t* out) {
    rc_u128 v = rc_read128(a);
    memcpy(out, &v, 16);
}
void x_write8(uint32_t a, uint8_t v) { rc_write8(a, v); }
void x_write16(uint32_t a, uint16_t v) { rc_write16(a, v); }
void x_write32(uint32_t a, uint32_t v) { rc_write32(a, v); }
void x_write64(uint32_t a, uint64_t v) { rc_write64(a, v); }
void x_write128(uint32_t a, const uint8_t* p) {
    rc_u128 v;
    memcpy(&v, p, 16);
    rc_write128(a, v);
}

uint64_t x_lwl(uint32_t a, uint64_t cur) { return rc_lwl(a, cur); }
uint64_t x_lwr(uint32_t a, uint64_t cur) { return rc_lwr(a, cur); }
uint64_t x_ldl(uint32_t a, uint64_t cur) { return rc_ldl(a, cur); }
uint64_t x_ldr(uint32_t a, uint64_t cur) { return rc_ldr(a, cur); }
void x_swl(uint32_t a, uint32_t v) { rc_swl(a, v); }
void x_swr(uint32_t a, uint32_t v) { rc_swr(a, v); }
void x_sdl(uint32_t a, uint64_t v) { rc_sdl(a, v); }
void x_sdr(uint32_t a, uint64_t v) { rc_sdr(a, v); }

void x_qfsrv(const uint8_t* rs, const uint8_t* rt, uint32_t sa, uint8_t* out) {
    rc_u128 s, t, d;
    memcpy(&s, rs, 16);
    memcpy(&t, rt, 16);
    d = rc_qfsrv(s, t, sa);
    memcpy(out, &d, 16);
}

/* ---- multiply / divide -------------------------------------------------- */

void x_mult(uint64_t* lo, uint64_t* hi, int32_t a, int32_t b) { rc_mult(lo, hi, a, b); }
void x_multu(uint64_t* lo, uint64_t* hi, uint32_t a, uint32_t b) { rc_multu(lo, hi, a, b); }
void x_div(uint64_t* lo, uint64_t* hi, int32_t a, int32_t b) { rc_div(lo, hi, a, b); }
void x_divu(uint64_t* lo, uint64_t* hi, uint32_t a, uint32_t b) { rc_divu(lo, hi, a, b); }
void x_madd(uint64_t* lo, uint64_t* hi, int32_t a, int32_t b) { rc_madd(lo, hi, a, b); }

/* ---- COP1 (floats as bit patterns) -------------------------------------- */

uint32_t x_fadd(uint32_t a, uint32_t b) { return rc_f2bits(rc_fadd(rc_bits2f(a), rc_bits2f(b))); }
uint32_t x_fsub(uint32_t a, uint32_t b) { return rc_f2bits(rc_fsub(rc_bits2f(a), rc_bits2f(b))); }
uint32_t x_fmul(uint32_t a, uint32_t b) { return rc_f2bits(rc_fmul(rc_bits2f(a), rc_bits2f(b))); }
uint32_t x_fdiv(uint32_t a, uint32_t b) { return rc_f2bits(rc_fdiv(rc_bits2f(a), rc_bits2f(b))); }
uint32_t x_fneg(uint32_t a) { return rc_f2bits(rc_fneg(rc_bits2f(a))); }
uint32_t x_fabs(uint32_t a) { return rc_f2bits(rc_fabs_(rc_bits2f(a))); }
uint32_t x_fmov(uint32_t a) { return rc_f2bits(rc_fmov(rc_bits2f(a))); }
int32_t x_cvtws(uint32_t a) { return rc_cvtws(rc_bits2f(a)); }
uint32_t x_cvtsw(int32_t v) { return rc_f2bits(rc_cvtsw(v)); }
int x_fc_eq(uint32_t a, uint32_t b) { return rc_fc_eq(rc_bits2f(a), rc_bits2f(b)); }
int x_fc_lt(uint32_t a, uint32_t b) { return rc_fc_lt(rc_bits2f(a), rc_bits2f(b)); }
int x_fc_le(uint32_t a, uint32_t b) { return rc_fc_le(rc_bits2f(a), rc_bits2f(b)); }
void x_fcr31_cond(uint32_t* fcr31, int cond) { rc_fcr31_cond(fcr31, cond); }

/* ---- COP2 (VU0 macro mode) ---------------------------------------------- */

static Vu0State* vu(void* ctx) { return &((R5900Context*)ctx)->vu0; }

void x_vu_fmac(void* ctx, int kind, int acc, int fd, int fs, int ft, int sel, int mask) {
    rc_vu_fmac(vu(ctx), kind, acc, fd, fs, ft, sel, mask);
}
void x_vu_maxmin(void* ctx, int is_min, int fd, int fs, int ft, int sel, int mask) {
    rc_vu_maxmin(vu(ctx), is_min, fd, fs, ft, sel, mask);
}
void x_vu_opmula(void* ctx, int fs, int ft) { rc_vu_opmula(vu(ctx), fs, ft); }
void x_vu_opmsub(void* ctx, int fd, int fs, int ft) { rc_vu_opmsub(vu(ctx), fd, fs, ft); }
void x_vu_ftoi(void* ctx, int shift, int ft, int fs, int mask) {
    rc_vu_ftoi(vu(ctx), shift, ft, fs, mask);
}
void x_vu_itof(void* ctx, int shift, int ft, int fs, int mask) {
    rc_vu_itof(vu(ctx), shift, ft, fs, mask);
}
void x_vu_move(void* ctx, int ft, int fs, int mask) { rc_vu_move(vu(ctx), ft, fs, mask); }
void x_vu_mr32(void* ctx, int ft, int fs, int mask) { rc_vu_mr32(vu(ctx), ft, fs, mask); }
void x_vu_div(void* ctx, int fs, int fsf, int ft, int ftf) {
    rc_vu_div(vu(ctx), fs, fsf, ft, ftf);
}
void x_vu_sqrt(void* ctx, int ft, int ftf) { rc_vu_sqrt(vu(ctx), ft, ftf); }
void x_vu_rsqrt(void* ctx, int fs, int fsf, int ft, int ftf) {
    rc_vu_rsqrt(vu(ctx), fs, fsf, ft, ftf);
}
void x_vu_clipw(void* ctx, int fs, int ft) { rc_vu_clipw(vu(ctx), fs, ft); }
void x_vu_lqi(void* ctx, int ft, int is, int mask) { rc_vu_lqi(vu(ctx), ft, is, mask); }
void x_vu_lqd(void* ctx, int ft, int is, int mask) { rc_vu_lqd(vu(ctx), ft, is, mask); }
void x_vu_sqi(void* ctx, int fs, int it, int mask) { rc_vu_sqi(vu(ctx), fs, it, mask); }
void x_vu_iaddi(void* ctx, int it, int is, int imm) { rc_vu_iaddi(vu(ctx), it, is, imm); }
void x_vu_rnext(void* ctx, int ft, int mask) { rc_vu_rnext(vu(ctx), ft, mask); }
void x_vu_rinit(void* ctx, int fs, int fsf) { rc_vu_rinit(vu(ctx), fs, fsf); }
void x_vu_rxor(void* ctx, int fs, int fsf) { rc_vu_rxor(vu(ctx), fs, fsf); }
void x_vu_qmfc(void* ctx, int fs, uint8_t* out) {
    rc_u128 v = rc_vu_qmfc(vu(ctx), fs);
    memcpy(out, &v, 16);
}
void x_vu_qmtc(void* ctx, int fd, const uint8_t* p) {
    rc_u128 v;
    memcpy(&v, p, 16);
    rc_vu_qmtc(vu(ctx), fd, v);
}
void x_vu_lqc2(void* ctx, int ft, uint32_t addr) { rc_vu_lqc2(vu(ctx), ft, addr); }
void x_vu_sqc2(void* ctx, int fs, uint32_t addr) { rc_vu_sqc2(vu(ctx), fs, addr); }
uint32_t x_vu_cfc(void* ctx, int creg) { return rc_vu_cfc((R5900Context*)ctx, creg); }
void x_vu_ctc(void* ctx, int creg, uint32_t v) { rc_vu_ctc((R5900Context*)ctx, creg, v); }
