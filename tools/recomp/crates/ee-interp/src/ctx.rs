//! Owned R5900Context allocation. The struct layout is the C ABI's; Rust
//! never mirrors it, it reads and writes fields through offsets exported by
//! the shim so the two can not drift apart.

use std::alloc::{alloc_zeroed, dealloc, Layout};
use std::sync::OnceLock;

use crate::ffi;

#[derive(Clone, Copy)]
struct Offsets {
    size: usize,
    align: usize,
    r: usize,
    lo: usize,
    hi: usize,
    sa: usize,
    fcr31: usize,
    f: usize,
    vu_vf: usize,
    vu_vi: usize,
    vu_acc: usize,
    vu_q: usize,
    vu_i: usize,
    vu_r: usize,
    vu_status: usize,
    vu_mac: usize,
    vu_clip: usize,
}

fn offs() -> &'static Offsets {
    static OFFS: OnceLock<Offsets> = OnceLock::new();
    OFFS.get_or_init(|| unsafe {
        Offsets {
            size: ffi::x_ctx_size(),
            align: ffi::x_ctx_align(),
            r: ffi::x_off_r(),
            lo: ffi::x_off_lo(),
            hi: ffi::x_off_hi(),
            sa: ffi::x_off_sa(),
            fcr31: ffi::x_off_fcr31(),
            f: ffi::x_off_f(),
            vu_vf: ffi::x_off_vu_vf(),
            vu_vi: ffi::x_off_vu_vi(),
            vu_acc: ffi::x_off_vu_acc(),
            vu_q: ffi::x_off_vu_q(),
            vu_i: ffi::x_off_vu_i(),
            vu_r: ffi::x_off_vu_r(),
            vu_status: ffi::x_off_vu_status(),
            vu_mac: ffi::x_off_vu_mac(),
            vu_clip: ffi::x_off_vu_clip(),
        }
    })
}

/// One guest CPU context, zero initialized except the vf00 = (0,0,0,1)
/// invariant that the runtime also establishes at boot.
pub struct Ctx {
    ptr: *mut u8,
    layout: Layout,
}

// The context is plain data owned by this box.
unsafe impl Send for Ctx {}

impl Ctx {
    pub fn new() -> Ctx {
        let o = offs();
        let layout = Layout::from_size_align(o.size, o.align).expect("ctx layout");
        let ptr = unsafe { alloc_zeroed(layout) };
        assert!(!ptr.is_null(), "context allocation failed");
        let mut c = Ctx { ptr, layout };
        // vf00.w = 1.0f
        c.set_vf_lane(0, 3, 0x3F80_0000);
        c
    }

    pub fn as_mut_ptr(&mut self) -> *mut u8 {
        self.ptr
    }

    pub fn bytes(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.ptr, self.layout.size()) }
    }

    pub fn copy_from(&mut self, other: &Ctx) {
        unsafe {
            std::ptr::copy_nonoverlapping(other.ptr, self.ptr, self.layout.size());
        }
    }

    fn rd<T: Copy>(&self, off: usize) -> T {
        unsafe { (self.ptr.add(off) as *const T).read() }
    }
    fn wr<T: Copy>(&mut self, off: usize, v: T) {
        unsafe { (self.ptr.add(off) as *mut T).write(v) }
    }

    // ---- GPRs ----------------------------------------------------------
    pub fn r64(&self, n: u8) -> u64 {
        if n == 0 {
            0
        } else {
            self.rd(offs().r + n as usize * 16)
        }
    }
    pub fn r32(&self, n: u8) -> u32 {
        self.r64(n) as u32
    }
    /// 64-bit GPR write; $zero writes are suppressed.
    pub fn set_r64(&mut self, n: u8, v: u64) {
        if n != 0 {
            self.wr(offs().r + n as usize * 16, v);
        }
    }
    pub fn r128(&self, n: u8) -> [u8; 16] {
        if n == 0 {
            return [0; 16];
        }
        self.rd(offs().r + n as usize * 16)
    }
    pub fn set_r128(&mut self, n: u8, v: [u8; 16]) {
        if n != 0 {
            self.wr(offs().r + n as usize * 16, v);
        }
    }

    // ---- LO/HI/SA/FCR31 ------------------------------------------------
    pub fn lo_ptr(&mut self, pipe: usize) -> *mut u64 {
        unsafe { (self.ptr.add(offs().lo) as *mut u64).add(pipe) }
    }
    pub fn hi_ptr(&mut self, pipe: usize) -> *mut u64 {
        unsafe { (self.ptr.add(offs().hi) as *mut u64).add(pipe) }
    }
    pub fn lohi(&self, hi: bool, pipe: usize) -> u64 {
        self.rd(if hi { offs().hi } else { offs().lo } + pipe * 8)
    }
    pub fn set_lohi(&mut self, hi: bool, pipe: usize, v: u64) {
        self.wr(if hi { offs().hi } else { offs().lo } + pipe * 8, v);
    }
    pub fn sa(&self) -> u32 {
        self.rd(offs().sa)
    }
    pub fn set_sa(&mut self, v: u32) {
        self.wr(offs().sa, v);
    }
    pub fn fcr31(&self) -> u32 {
        self.rd(offs().fcr31)
    }
    pub fn set_fcr31(&mut self, v: u32) {
        self.wr(offs().fcr31, v);
    }
    pub fn fcr31_ptr(&mut self) -> *mut u32 {
        unsafe { self.ptr.add(offs().fcr31) as *mut u32 }
    }

    // ---- COP1 registers (bit patterns) ---------------------------------
    pub fn f_bits(&self, n: u8) -> u32 {
        self.rd(offs().f + n as usize * 4)
    }
    pub fn set_f_bits(&mut self, n: u8, v: u32) {
        self.wr(offs().f + n as usize * 4, v);
    }

    // ---- VU0 state -----------------------------------------------------
    pub fn set_vf(&mut self, n: u8, lanes: [u32; 4]) {
        for (i, v) in lanes.iter().enumerate() {
            self.set_vf_lane(n, i, *v);
        }
    }
    pub fn set_vf_lane(&mut self, n: u8, lane: usize, v: u32) {
        self.wr(offs().vu_vf + n as usize * 16 + lane * 4, v);
    }
    pub fn vf_lane(&self, n: u8, lane: usize) -> u32 {
        self.rd(offs().vu_vf + n as usize * 16 + lane * 4)
    }
    pub fn set_vi(&mut self, n: u8, v: u16) {
        self.wr(offs().vu_vi + n as usize * 2, v);
    }
    pub fn vi(&self, n: u8) -> u16 {
        self.rd(offs().vu_vi + n as usize * 2)
    }
    pub fn set_acc(&mut self, lanes: [u32; 4]) {
        for (i, v) in lanes.iter().enumerate() {
            self.wr(offs().vu_acc + i * 4, *v);
        }
    }
    pub fn set_vu_q_bits(&mut self, v: u32) {
        self.wr(offs().vu_q, v);
    }
    pub fn set_vu_i_bits(&mut self, v: u32) {
        self.wr(offs().vu_i, v);
    }
    pub fn set_vu_r(&mut self, v: u32) {
        self.wr(offs().vu_r, v);
    }
    pub fn set_vu_status(&mut self, v: u32) {
        self.wr(offs().vu_status, v);
    }
    pub fn set_vu_mac(&mut self, v: u32) {
        self.wr(offs().vu_mac, v);
    }
    pub fn set_vu_clip(&mut self, v: u32) {
        self.wr(offs().vu_clip, v);
    }
}

impl Default for Ctx {
    fn default() -> Self {
        Ctx::new()
    }
}

impl Drop for Ctx {
    fn drop(&mut self) {
        unsafe { dealloc(self.ptr, self.layout) }
    }
}

impl Clone for Ctx {
    fn clone(&self) -> Ctx {
        let layout = self.layout;
        let ptr = unsafe { alloc_zeroed(layout) };
        assert!(!ptr.is_null(), "context allocation failed");
        unsafe { std::ptr::copy_nonoverlapping(self.ptr, ptr, layout.size()) };
        Ctx { ptr, layout }
    }
}

/// Point one 64 KB guest page at a host buffer (or unmap with null).
///
/// # Safety
/// `p` must point at (at least) 64 KB of writable memory that outlives
/// every guest access through this page, or be null to unmap.
pub unsafe fn set_page(idx: u32, p: *mut u8) {
    ffi::x_set_page(idx, p)
}
