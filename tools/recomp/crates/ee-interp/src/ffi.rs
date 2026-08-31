//! Raw bindings to the C shim (csrc/shim.c). Every semantic helper the
//! interpreter uses lives behind this boundary so the interpreter and the
//! emitter's generated code share one implementation.

use std::os::raw::{c_int, c_void};

extern "C" {
    // Environment.
    pub fn x_set_page(idx: u32, p: *mut u8);

    // Context layout.
    pub fn x_ctx_size() -> usize;
    pub fn x_ctx_align() -> usize;
    pub fn x_off_r() -> usize;
    pub fn x_off_lo() -> usize;
    pub fn x_off_hi() -> usize;
    pub fn x_off_sa() -> usize;
    pub fn x_off_fcr31() -> usize;
    pub fn x_off_f() -> usize;
    pub fn x_off_vu_vf() -> usize;
    pub fn x_off_vu_vi() -> usize;
    pub fn x_off_vu_acc() -> usize;
    pub fn x_off_vu_q() -> usize;
    pub fn x_off_vu_r() -> usize;
    pub fn x_off_vu_i() -> usize;
    pub fn x_off_vu_status() -> usize;
    pub fn x_off_vu_mac() -> usize;
    pub fn x_off_vu_clip() -> usize;

    // Guest memory.
    pub fn x_read8(a: u32) -> u8;
    pub fn x_read16(a: u32) -> u16;
    pub fn x_read32(a: u32) -> u32;
    pub fn x_read64(a: u32) -> u64;
    pub fn x_read128(a: u32, out: *mut u8);
    pub fn x_write8(a: u32, v: u8);
    pub fn x_write16(a: u32, v: u16);
    pub fn x_write32(a: u32, v: u32);
    pub fn x_write64(a: u32, v: u64);
    pub fn x_write128(a: u32, p: *const u8);
    pub fn x_lwl(a: u32, cur: u64) -> u64;
    pub fn x_lwr(a: u32, cur: u64) -> u64;
    pub fn x_ldl(a: u32, cur: u64) -> u64;
    pub fn x_ldr(a: u32, cur: u64) -> u64;
    pub fn x_swl(a: u32, v: u32);
    pub fn x_swr(a: u32, v: u32);
    pub fn x_sdl(a: u32, v: u64);
    pub fn x_sdr(a: u32, v: u64);
    pub fn x_qfsrv(rs: *const u8, rt: *const u8, sa: u32, out: *mut u8);

    // Multiply / divide.
    pub fn x_mult(lo: *mut u64, hi: *mut u64, a: i32, b: i32);
    pub fn x_multu(lo: *mut u64, hi: *mut u64, a: u32, b: u32);
    pub fn x_div(lo: *mut u64, hi: *mut u64, a: i32, b: i32);
    pub fn x_divu(lo: *mut u64, hi: *mut u64, a: u32, b: u32);
    pub fn x_madd(lo: *mut u64, hi: *mut u64, a: i32, b: i32);

    // COP1 (float bit patterns).
    pub fn x_fadd(a: u32, b: u32) -> u32;
    pub fn x_fsub(a: u32, b: u32) -> u32;
    pub fn x_fmul(a: u32, b: u32) -> u32;
    pub fn x_fdiv(a: u32, b: u32) -> u32;
    pub fn x_fneg(a: u32) -> u32;
    pub fn x_fabs(a: u32) -> u32;
    pub fn x_fmov(a: u32) -> u32;
    pub fn x_cvtws(a: u32) -> i32;
    pub fn x_cvtsw(v: i32) -> u32;
    pub fn x_fc_eq(a: u32, b: u32) -> c_int;
    pub fn x_fc_lt(a: u32, b: u32) -> c_int;
    pub fn x_fc_le(a: u32, b: u32) -> c_int;
    pub fn x_fcr31_cond(fcr31: *mut u32, cond: c_int);

    // COP2 (VU0 macro mode).
    pub fn x_vu_fmac(
        ctx: *mut c_void,
        kind: c_int,
        acc: c_int,
        fd: c_int,
        fs: c_int,
        ft: c_int,
        sel: c_int,
        mask: c_int,
    );
    pub fn x_vu_maxmin(
        ctx: *mut c_void,
        is_min: c_int,
        fd: c_int,
        fs: c_int,
        ft: c_int,
        sel: c_int,
        mask: c_int,
    );
    pub fn x_vu_opmula(ctx: *mut c_void, fs: c_int, ft: c_int);
    pub fn x_vu_opmsub(ctx: *mut c_void, fd: c_int, fs: c_int, ft: c_int);
    pub fn x_vu_ftoi(ctx: *mut c_void, shift: c_int, ft: c_int, fs: c_int, mask: c_int);
    pub fn x_vu_itof(ctx: *mut c_void, shift: c_int, ft: c_int, fs: c_int, mask: c_int);
    pub fn x_vu_move(ctx: *mut c_void, ft: c_int, fs: c_int, mask: c_int);
    pub fn x_vu_mr32(ctx: *mut c_void, ft: c_int, fs: c_int, mask: c_int);
    pub fn x_vu_div(ctx: *mut c_void, fs: c_int, fsf: c_int, ft: c_int, ftf: c_int);
    pub fn x_vu_sqrt(ctx: *mut c_void, ft: c_int, ftf: c_int);
    pub fn x_vu_rsqrt(ctx: *mut c_void, fs: c_int, fsf: c_int, ft: c_int, ftf: c_int);
    pub fn x_vu_clipw(ctx: *mut c_void, fs: c_int, ft: c_int);
    pub fn x_vu_lqi(ctx: *mut c_void, ft: c_int, is: c_int, mask: c_int);
    pub fn x_vu_lqd(ctx: *mut c_void, ft: c_int, is: c_int, mask: c_int);
    pub fn x_vu_sqi(ctx: *mut c_void, fs: c_int, it: c_int, mask: c_int);
    pub fn x_vu_iaddi(ctx: *mut c_void, it: c_int, is: c_int, imm: c_int);
    pub fn x_vu_rnext(ctx: *mut c_void, ft: c_int, mask: c_int);
    pub fn x_vu_rinit(ctx: *mut c_void, fs: c_int, fsf: c_int);
    pub fn x_vu_rxor(ctx: *mut c_void, fs: c_int, fsf: c_int);
    pub fn x_vu_qmfc(ctx: *mut c_void, fs: c_int, out: *mut u8);
    pub fn x_vu_qmtc(ctx: *mut c_void, fd: c_int, p: *const u8);
    pub fn x_vu_lqc2(ctx: *mut c_void, ft: c_int, addr: u32);
    pub fn x_vu_sqc2(ctx: *mut c_void, fs: c_int, addr: u32);
    pub fn x_vu_cfc(ctx: *mut c_void, creg: c_int) -> u32;
    pub fn x_vu_ctc(ctx: *mut c_void, creg: c_int, v: u32);
}
