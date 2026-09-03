/* shim.c: FFI surface between the Rust VU1 reference interpreter and the
 * static inline helpers in recomp_ops.h. Every semantic operation the
 * interpreter performs on VU1 state goes through this file, so the
 * interpreter and the emitter's generated C execute the exact same helper
 * implementations, compiled by the same compiler with the same flags.
 *
 * That is the whole point. A disagreement between the two therefore means
 * the emitter and the interpreter disagree about *scheduling* -- which
 * bundle a hazard applies to, when the Q pipeline commits, which branch
 * target is taken -- and never about float semantics, because there is
 * only one implementation of those.
 *
 * Unlike the EE shim there is no guest page table here: Vu1State carries
 * its own 16 KB data memory, so the interpreter needs no memory
 * environment at all. The only external hooks recomp_ops.h can reach from
 * VU1 code are rt_xgkick and rt_unimplemented; both are recorded rather
 * than aborting, because XGKICK output is a result the differential test
 * wants to compare.
 *
 * Floats never cross the FFI as floats: everything is raw bytes or u32 bit
 * patterns, so no NaN payload is altered by an ABI conversion.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_ops.h"

/* ---- recorded side effects ---------------------------------------------- */

/* XGKICK addresses in issue order. The differential test compares these
 * alongside the final register file: a program that computes the right
 * vertices but kicks the wrong packet is still wrong. */
#define X_MAX_KICKS 4096
static uint32_t g_kicks[X_MAX_KICKS];
static uint32_t g_kick_count;
static uint32_t g_unimplemented;

void rt_xgkick(Vu1State* vu, uint32_t qw_addr) {
    (void)vu;
    if (g_kick_count < X_MAX_KICKS) g_kicks[g_kick_count] = qw_addr;
    ++g_kick_count;
}

void rt_unimplemented(const char* what, uint32_t vram) {
    (void)what;
    (void)vram;
    ++g_unimplemented;
}

void x_reset_effects(void) {
    g_kick_count = 0;
    g_unimplemented = 0;
    memset(g_kicks, 0, sizeof g_kicks);
}
uint32_t x_kick_count(void) { return g_kick_count; }
uint32_t x_kick_at(uint32_t i) { return i < X_MAX_KICKS ? g_kicks[i] : 0u; }
uint32_t x_unimplemented_count(void) { return g_unimplemented; }

/* ---- Vu1State layout ---------------------------------------------------- */

size_t x_vu_size(void) { return sizeof(Vu1State); }
size_t x_vu_align(void) { return _Alignof(Vu1State); }
size_t x_off_vf(void) { return offsetof(Vu1State, vf); }
size_t x_off_vi(void) { return offsetof(Vu1State, vi); }
size_t x_off_acc(void) { return offsetof(Vu1State, acc); }
size_t x_off_q(void) { return offsetof(Vu1State, q); }
size_t x_off_i(void) { return offsetof(Vu1State, i); }
size_t x_off_r(void) { return offsetof(Vu1State, r); }
size_t x_off_status(void) { return offsetof(Vu1State, status); }
size_t x_off_mac(void) { return offsetof(Vu1State, mac); }
size_t x_off_clip(void) { return offsetof(Vu1State, clip); }
size_t x_off_pc(void) { return offsetof(Vu1State, pc); }
size_t x_off_xtop(void) { return offsetof(Vu1State, xtop); }
size_t x_off_itop(void) { return offsetof(Vu1State, itop); }
size_t x_off_mem(void) { return offsetof(Vu1State, mem); }
size_t x_off_pending_q(void) { return offsetof(Vu1State, pending_q); }
size_t x_off_q_pending(void) { return offsetof(Vu1State, q_pending); }

/* ---- register access ---------------------------------------------------- */

void x_vf(const Vu1State* vu, int n, uint8_t* out) {
    rc_u128 v = rc_vu1_vf(vu, n);
    memcpy(out, &v, 16);
}
uint32_t x_vi(const Vu1State* vu, int n) { return rc_vu1_vi(vu, n); }
void x_viset(Vu1State* vu, int n, uint32_t v) { rc_vu1_viset(vu, n, v); }
void x_src(const Vu1State* vu, int ft, int sel, uint8_t* out) {
    rc_u128 v = rc_vu1_src(vu, ft, sel);
    memcpy(out, &v, 16);
}
void x_write(Vu1State* vu, int fd, int mask, const uint8_t* p) {
    rc_u128 v;
    memcpy(&v, p, 16);
    rc_vu1_write(vu, fd, mask, v);
}

/* ---- FMAC and friends --------------------------------------------------- */

void x_fmac_calc(const Vu1State* vu, int kind, int fs, int ft, int sel,
                 int mask, uint32_t* mac_out, uint8_t* out) {
    rc_u128 r = rc_vu1_fmac_calc(vu, kind, fs, ft, sel, mask, mac_out);
    memcpy(out, &r, 16);
}
void x_flags_commit(Vu1State* vu, uint32_t mac) { rc_vu1_flags_commit(vu, mac); }
void x_commit_vf(Vu1State* vu, int fd, int mask, const uint8_t* p, uint32_t mac) {
    rc_u128 r;
    memcpy(&r, p, 16);
    rc_vu1_commit_vf(vu, fd, mask, r, mac);
}
void x_commit_acc(Vu1State* vu, int mask, const uint8_t* p, uint32_t mac) {
    rc_u128 r;
    memcpy(&r, p, 16);
    rc_vu1_commit_acc(vu, mask, r, mac);
}
void x_maxmin_calc(const Vu1State* vu, int is_min, int fs, int ft, int sel,
                   uint8_t* out) {
    rc_u128 r = rc_vu1_maxmin_calc(vu, is_min, fs, ft, sel);
    memcpy(out, &r, 16);
}
void x_abs_calc(const Vu1State* vu, int fs, uint8_t* out) {
    rc_u128 r = rc_vu1_abs_calc(vu, fs);
    memcpy(out, &r, 16);
}
void x_ftoi_calc(const Vu1State* vu, int shift, int fs, uint8_t* out) {
    rc_u128 r = rc_vu1_ftoi_calc(vu, shift, fs);
    memcpy(out, &r, 16);
}
void x_itof_calc(const Vu1State* vu, int shift, int fs, uint8_t* out) {
    rc_u128 r = rc_vu1_itof_calc(vu, shift, fs);
    memcpy(out, &r, 16);
}
uint32_t x_clip_calc(const Vu1State* vu, int fs, int ft) {
    return rc_vu1_clip_calc(vu, fs, ft);
}

/* ---- Q pipeline --------------------------------------------------------- */

void x_q_commit(Vu1State* vu) { rc_vu1_q_commit(vu); }
void x_qflags(Vu1State* vu, uint32_t idbits) { rc_vu1_qflags(vu, idbits); }
void x_div(Vu1State* vu, int fs, int fsf, int ft, int ftf) {
    rc_vu1_div(vu, fs, fsf, ft, ftf);
}
void x_rsqrt(Vu1State* vu, int fs, int fsf, int ft, int ftf) {
    rc_vu1_rsqrt(vu, fs, fsf, ft, ftf);
}

/* ---- data memory and register moves ------------------------------------- */

void x_lq(const Vu1State* vu, uint32_t qw, uint8_t* out) {
    rc_u128 v = rc_vu1_lq(vu, qw);
    memcpy(out, &v, 16);
}
void x_sq(Vu1State* vu, int fs, int mask, uint32_t qw) {
    rc_vu1_sq(vu, fs, mask, qw);
}
uint32_t x_ilw(const Vu1State* vu, uint32_t qw, int lane) {
    return rc_vu1_ilw(vu, qw, lane);
}
void x_isw(Vu1State* vu, uint32_t qw, int mask, uint32_t v) {
    rc_vu1_isw(vu, qw, mask, v);
}
void x_mfir(Vu1State* vu, int ft, int mask, uint32_t vi_val) {
    rc_vu1_mfir(vu, ft, mask, vi_val);
}
void x_move(Vu1State* vu, int ft, int fs, int mask) {
    rc_vu1_move(vu, ft, fs, mask);
}
void x_mr32(Vu1State* vu, int ft, int fs, int mask) {
    rc_vu1_mr32(vu, ft, fs, mask);
}
void x_rinit(Vu1State* vu, uint32_t bits) { rc_vu1_rinit(vu, bits); }
void x_rget(Vu1State* vu, int ft, int mask) { rc_vu1_rget(vu, ft, mask); }
uint32_t x_qwaddr(uint32_t qw) { return rc_vu1_qwaddr(qw); }
uint32_t x_hash(const uint8_t* bytes, uint32_t len) {
    return rc_vu1_hash(bytes, len);
}

/* ---- scalar twins, for the helper equivalence test -----------------------
 * recomp_ops.h keeps every VU1 lane helper twice: the branch-free SSE2 form
 * under the public name and the original one-lane-at-a-time body under a
 * _scalar suffix. tests/helpers.rs calls both through these exports and
 * requires them to agree bit for bit, so the vector rewrite is checked
 * against the semantics it claims to preserve rather than against itself.
 * On a target without SSE2 the two names are the same code and the test
 * still passes, trivially; x_sse2_active says which case ran. */

int x_sse2_active(void) {
#ifdef RC_VU_SSE2
    return 1;
#else
    return 0;
#endif
}

void x_write_scalar(Vu1State* vu, int fd, int mask, const uint8_t* p) {
    rc_u128 v;
    memcpy(&v, p, 16);
    rc_vu1_write_scalar(vu, fd, mask, v);
}
void x_fmac_calc_scalar(const Vu1State* vu, int kind, int fs, int ft, int sel,
                        int mask, uint32_t* mac_out, uint8_t* out) {
    rc_u128 r = rc_vu1_fmac_calc_scalar(vu, kind, fs, ft, sel, mask, mac_out);
    memcpy(out, &r, 16);
}
void x_commit_vf_scalar(Vu1State* vu, int fd, int mask, const uint8_t* p,
                        uint32_t mac) {
    rc_u128 r;
    memcpy(&r, p, 16);
    rc_vu1_commit_vf_scalar(vu, fd, mask, r, mac);
}
void x_commit_acc_scalar(Vu1State* vu, int mask, const uint8_t* p, uint32_t mac) {
    rc_u128 r;
    memcpy(&r, p, 16);
    rc_vu1_commit_acc_scalar(vu, mask, r, mac);
}
void x_maxmin_calc_scalar(const Vu1State* vu, int is_min, int fs, int ft,
                          int sel, uint8_t* out) {
    rc_u128 r = rc_vu1_maxmin_calc_scalar(vu, is_min, fs, ft, sel);
    memcpy(out, &r, 16);
}
void x_abs_calc_scalar(const Vu1State* vu, int fs, uint8_t* out) {
    rc_u128 r = rc_vu1_abs_calc_scalar(vu, fs);
    memcpy(out, &r, 16);
}
void x_ftoi_calc_scalar(const Vu1State* vu, int shift, int fs, uint8_t* out) {
    rc_u128 r = rc_vu1_ftoi_calc_scalar(vu, shift, fs);
    memcpy(out, &r, 16);
}
void x_itof_calc_scalar(const Vu1State* vu, int shift, int fs, uint8_t* out) {
    rc_u128 r = rc_vu1_itof_calc_scalar(vu, shift, fs);
    memcpy(out, &r, 16);
}
uint32_t x_clip_calc_scalar(const Vu1State* vu, int fs, int ft) {
    return rc_vu1_clip_calc_scalar(vu, fs, ft);
}
void x_sq_scalar(Vu1State* vu, int fs, int mask, uint32_t qw) {
    rc_vu1_sq_scalar(vu, fs, mask, qw);
}

/* ---- MXCSR control, for the FTZ/DAZ pass of the equivalence test --------
 * The runtime sets FTZ (MXCSR bit 15) and DAZ (bit 6) on every
 * guest-executing thread (src/runtime/main.cpp), so the mode the helpers
 * actually run in is not the host default a `cargo test` process starts
 * with. tests/helpers.rs runs the corpus sweep once in each mode; both the
 * SSE2 and the scalar form are driven under whatever MXCSR is in force, so
 * the comparison stays like for like either way. x_set_ftz_daz returns the
 * previous MXCSR for x_set_mxcsr to restore. On a target without SSE2
 * there is no MXCSR and both are no-ops. */

uint32_t x_set_ftz_daz(void) {
#ifdef RC_VU_SSE2
    unsigned int prev = _mm_getcsr();
    _mm_setcsr(prev | 0x8000u | 0x0040u);
    return (uint32_t)prev;
#else
    return 0u;
#endif
}

void x_set_mxcsr(uint32_t v) {
#ifdef RC_VU_SSE2
    _mm_setcsr((unsigned int)v);
#else
    (void)v;
#endif
}
