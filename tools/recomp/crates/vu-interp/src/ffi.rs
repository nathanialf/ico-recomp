#![allow(dead_code)]
//! Raw bindings to the C shim (csrc/shim.c). Every semantic helper the
//! interpreter uses lives behind this boundary so the interpreter and the
//! emitter's generated code share one implementation.

use std::os::raw::c_int;

extern "C" {
    // Recorded side effects.
    pub fn x_reset_effects();
    pub fn x_kick_count() -> u32;
    pub fn x_kick_at(i: u32) -> u32;
    pub fn x_unimplemented_count() -> u32;

    // Vu1State layout.
    pub fn x_vu_size() -> usize;
    pub fn x_vu_align() -> usize;
    pub fn x_off_vf() -> usize;
    pub fn x_off_vi() -> usize;
    pub fn x_off_acc() -> usize;
    pub fn x_off_q() -> usize;
    pub fn x_off_i() -> usize;
    pub fn x_off_r() -> usize;
    pub fn x_off_status() -> usize;
    pub fn x_off_mac() -> usize;
    pub fn x_off_clip() -> usize;
    pub fn x_off_pc() -> usize;
    pub fn x_off_xtop() -> usize;
    pub fn x_off_itop() -> usize;
    pub fn x_off_mem() -> usize;
    pub fn x_off_pending_q() -> usize;
    pub fn x_off_q_pending() -> usize;

    // Register access.
    pub fn x_vf(vu: *const u8, n: c_int, out: *mut u8);
    pub fn x_vi(vu: *const u8, n: c_int) -> u32;
    pub fn x_viset(vu: *mut u8, n: c_int, v: u32);
    pub fn x_src(vu: *const u8, ft: c_int, sel: c_int, out: *mut u8);
    pub fn x_write(vu: *mut u8, fd: c_int, mask: c_int, p: *const u8);

    // FMAC and friends.
    pub fn x_fmac_calc(
        vu: *const u8,
        kind: c_int,
        fs: c_int,
        ft: c_int,
        sel: c_int,
        mask: c_int,
        mac_out: *mut u32,
        out: *mut u8,
    );
    pub fn x_flags_commit(vu: *mut u8, mac: u32);
    pub fn x_commit_vf(vu: *mut u8, fd: c_int, mask: c_int, p: *const u8, mac: u32);
    pub fn x_commit_acc(vu: *mut u8, mask: c_int, p: *const u8, mac: u32);
    pub fn x_maxmin_calc(
        vu: *const u8,
        is_min: c_int,
        fs: c_int,
        ft: c_int,
        sel: c_int,
        out: *mut u8,
    );
    pub fn x_abs_calc(vu: *const u8, fs: c_int, out: *mut u8);
    pub fn x_ftoi_calc(vu: *const u8, shift: c_int, fs: c_int, out: *mut u8);
    pub fn x_itof_calc(vu: *const u8, shift: c_int, fs: c_int, out: *mut u8);
    pub fn x_clip_calc(vu: *const u8, fs: c_int, ft: c_int) -> u32;

    // Q pipeline.
    pub fn x_q_commit(vu: *mut u8);
    pub fn x_qflags(vu: *mut u8, idbits: u32);
    pub fn x_div(vu: *mut u8, fs: c_int, fsf: c_int, ft: c_int, ftf: c_int);
    pub fn x_rsqrt(vu: *mut u8, fs: c_int, fsf: c_int, ft: c_int, ftf: c_int);

    // Data memory and register moves.
    pub fn x_lq(vu: *const u8, qw: u32, out: *mut u8);
    pub fn x_sq(vu: *mut u8, fs: c_int, mask: c_int, qw: u32);
    pub fn x_ilw(vu: *const u8, qw: u32, lane: c_int) -> u32;
    pub fn x_isw(vu: *mut u8, qw: u32, mask: c_int, v: u32);
    pub fn x_mfir(vu: *mut u8, ft: c_int, mask: c_int, vi_val: u32);
    pub fn x_move(vu: *mut u8, ft: c_int, fs: c_int, mask: c_int);
    pub fn x_mr32(vu: *mut u8, ft: c_int, fs: c_int, mask: c_int);
    pub fn x_rinit(vu: *mut u8, bits: u32);
    pub fn x_rget(vu: *mut u8, ft: c_int, mask: c_int);
    pub fn x_hash(bytes: *const u8, len: u32) -> u32;
}
