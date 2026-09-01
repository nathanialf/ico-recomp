//! Owned Vu1State allocation. The struct layout is the C ABI's; Rust never
//! mirrors it, it reads and writes through offsets exported by the shim so
//! the two can not drift apart. Same trick as ee-interp/src/ctx.rs.

use std::alloc::{alloc_zeroed, dealloc, Layout};
use std::sync::OnceLock;

use crate::ffi;

#[derive(Clone, Copy)]
struct Offsets {
    size: usize,
    align: usize,
    vf: usize,
    vi: usize,
    q: usize,
    clip: usize,
    status: usize,
    mac: usize,
    pc: usize,
    xtop: usize,
    itop: usize,
    mem: usize,
}

fn offs() -> &'static Offsets {
    static OFFS: OnceLock<Offsets> = OnceLock::new();
    OFFS.get_or_init(|| unsafe {
        Offsets {
            size: ffi::x_vu_size(),
            align: ffi::x_vu_align(),
            vf: ffi::x_off_vf(),
            vi: ffi::x_off_vi(),
            q: ffi::x_off_q(),
            clip: ffi::x_off_clip(),
            status: ffi::x_off_status(),
            mac: ffi::x_off_mac(),
            pc: ffi::x_off_pc(),
            xtop: ffi::x_off_xtop(),
            itop: ffi::x_off_itop(),
            mem: ffi::x_off_mem(),
        }
    })
}

/// One VU1 state, zero initialized except the vf00 = (0,0,0,1) invariant
/// that vu1rt.cpp also establishes when it allocates the window.
pub struct VuState {
    ptr: *mut u8,
    layout: Layout,
}

// Plain data owned by this box.
unsafe impl Send for VuState {}

impl VuState {
    pub fn new() -> VuState {
        let o = offs();
        let layout = Layout::from_size_align(o.size, o.align).expect("vu layout");
        let ptr = unsafe { alloc_zeroed(layout) };
        assert!(!ptr.is_null(), "Vu1State allocation failed");
        let mut s = VuState { ptr, layout };
        s.set_vf_lane(0, 3, 0x3F80_0000); // vf00.w = 1.0f
        s
    }

    pub fn as_mut_ptr(&mut self) -> *mut u8 {
        self.ptr
    }
    pub fn as_ptr(&self) -> *const u8 {
        self.ptr
    }

    /// The whole state as bytes, for byte-exact comparison.
    pub fn bytes(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.ptr, offs().size) }
    }

    pub fn size(&self) -> usize {
        offs().size
    }

    fn u32_at(&self, off: usize) -> u32 {
        unsafe { std::ptr::read_unaligned(self.ptr.add(off) as *const u32) }
    }
    fn set_u32_at(&mut self, off: usize, v: u32) {
        unsafe { std::ptr::write_unaligned(self.ptr.add(off) as *mut u32, v) }
    }

    pub fn set_vf_lane(&mut self, reg: usize, lane: usize, bits: u32) {
        let o = offs();
        self.set_u32_at(o.vf + reg * 16 + lane * 4, bits);
    }
    pub fn vf_lane(&self, reg: usize, lane: usize) -> u32 {
        self.u32_at(offs().vf + reg * 16 + lane * 4)
    }

    pub fn set_vi(&mut self, n: usize, v: u16) {
        let o = offs();
        unsafe { std::ptr::write_unaligned(self.ptr.add(o.vi + n * 2) as *mut u16, v) }
    }
    pub fn vi(&self, n: usize) -> u16 {
        unsafe { std::ptr::read_unaligned(self.ptr.add(offs().vi + n * 2) as *const u16) }
    }

    pub fn pc(&self) -> u32 {
        self.u32_at(offs().pc)
    }
    pub fn set_pc(&mut self, v: u32) {
        self.set_u32_at(offs().pc, v)
    }
    pub fn set_xtop(&mut self, v: u32) {
        self.set_u32_at(offs().xtop, v)
    }
    pub fn set_itop(&mut self, v: u32) {
        self.set_u32_at(offs().itop, v)
    }
    pub fn clip(&self) -> u32 {
        self.u32_at(offs().clip)
    }
    pub fn set_clip(&mut self, v: u32) {
        self.set_u32_at(offs().clip, v)
    }
    pub fn status(&self) -> u32 {
        self.u32_at(offs().status)
    }
    pub fn mac(&self) -> u32 {
        self.u32_at(offs().mac)
    }
    pub fn q_bits(&self) -> u32 {
        self.u32_at(offs().q)
    }

    /// Overwrite the whole state from a captured blob.
    pub fn load(&mut self, blob: &[u8]) {
        assert_eq!(blob.len(), offs().size, "captured state size mismatch");
        unsafe { std::ptr::copy_nonoverlapping(blob.as_ptr(), self.ptr, blob.len()) };
    }

    /// Byte offset of `pc` within the state, for reading a captured blob.
    pub fn pc_offset(&self) -> usize {
        offs().pc
    }

    /// FNV-1a over everything except the 16KB data memory. Must match
    /// d_reghash in the differential harness byte for byte.
    pub fn reg_hash(&self) -> u32 {
        let n = offs().mem;
        let bytes = unsafe { std::slice::from_raw_parts(self.ptr, n) };
        let mut h: u32 = 2166136261;
        for &b in bytes {
            h ^= b as u32;
            h = h.wrapping_mul(16777619);
        }
        h
    }

    /// True when a byte offset lands in the u16 vi array, where a 4-byte
    /// lane would straddle two registers.
    pub fn is_vi_offset(&self, off: usize) -> bool {
        let o = offs();
        off >= o.vi && off < o.vi + 16 * 2
    }

    /// Name the field a byte offset falls in, so a differential failure can
    /// say "vf12.y" instead of "[0x1c4]".
    pub fn field_name(&self, off: usize) -> String {
        let o = offs();
        const LANE: [&str; 4] = ["x", "y", "z", "w"];
        if off >= o.vf && off < o.vf + 32 * 16 {
            let d = off - o.vf;
            return format!("vf{:02}.{}", d / 16, LANE[(d % 16) / 4]);
        }
        if off >= o.vi && off < o.vi + 16 * 2 {
            return format!("vi{:02}", (off - o.vi) / 2);
        }
        // vi is a u16 array: a 4-byte lane straddles two registers, so the
        // caller must not widen offsets inside it.

        if off >= o.mem && off < o.mem + 16384 {
            let d = off - o.mem;
            return format!("mem[{:#06x}].{}", d / 16, LANE[(d % 16) / 4]);
        }
        for (name, base, len) in [
            ("q", o.q, 4),
            ("clip", o.clip, 4),
            ("status", o.status, 4),
            ("mac", o.mac, 4),
            ("pc", o.pc, 4),
            ("xtop", o.xtop, 4),
            ("itop", o.itop, 4),
        ] {
            if off >= base && off < base + len {
                return name.to_string();
            }
        }
        format!("+{off:#x}")
    }

    /// VU1 data memory as a mutable slice, for seeding vertex data.
    pub fn mem_mut(&mut self) -> &mut [u8] {
        let o = offs();
        unsafe { std::slice::from_raw_parts_mut(self.ptr.add(o.mem), 16384) }
    }
    pub fn mem(&self) -> &[u8] {
        let o = offs();
        unsafe { std::slice::from_raw_parts(self.ptr.add(o.mem), 16384) }
    }
}

impl Clone for VuState {
    fn clone(&self) -> VuState {
        let o = offs();
        let layout = Layout::from_size_align(o.size, o.align).expect("vu layout");
        let ptr = unsafe { alloc_zeroed(layout) };
        assert!(!ptr.is_null(), "Vu1State allocation failed");
        unsafe { std::ptr::copy_nonoverlapping(self.ptr, ptr, o.size) };
        VuState { ptr, layout }
    }
}

impl Drop for VuState {
    fn drop(&mut self) {
        unsafe { dealloc(self.ptr, self.layout) }
    }
}

impl Default for VuState {
    fn default() -> Self {
        Self::new()
    }
}

/// Side effects recorded by the shim during the most recent run.
#[derive(Debug, Default, PartialEq, Eq, Clone)]
pub struct Effects {
    pub kicks: Vec<u32>,
    pub unimplemented: u32,
}

pub fn reset_effects() {
    unsafe { ffi::x_reset_effects() }
}

pub fn take_effects() -> Effects {
    unsafe {
        let n = ffi::x_kick_count();
        let kicks = (0..n.min(4096)).map(|i| ffi::x_kick_at(i)).collect();
        Effects {
            kicks,
            unimplemented: ffi::x_unimplemented_count(),
        }
    }
}
