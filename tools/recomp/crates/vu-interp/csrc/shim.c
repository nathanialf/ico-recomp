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
