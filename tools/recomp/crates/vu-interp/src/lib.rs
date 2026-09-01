//! `vu-interp`: reference interpreter for the covered VU1 instruction set,
//! executing on the C-ABI `Vu1State` through a shim (csrc/shim.c) that
//! wraps the static inline helpers in include/recomp_ops.h. The shim is the
//! whole point: the interpreter and the emitter's generated C run the exact
//! same helper implementations, so the differential test in
//! tests/differential.rs compares like against like and any mismatch is a
//! scheduling disagreement, not a float-semantics one.
//!
//! See interp.rs for the hazard model, which is deliberately dynamic where
//! vu-emit's is static.

mod ffi;
mod interp;
mod state;

pub use interp::{run, Outcome, Stop};
pub use state::{reset_effects, take_effects, Effects, VuState};

/// XGKICK from the interpreter, recorded by the shim so the differential
/// test can compare packet issue order as well as the register file.
pub(crate) unsafe fn xgkick(vu: *mut u8, qw_addr: u32) {
    extern "C" {
        fn rt_xgkick(vu: *mut u8, qw_addr: u32);
    }
    rt_xgkick(vu, qw_addr)
}

/// The upload hash as the runtime computes it, via the same helper.
pub fn upload_hash(bytes: &[u8]) -> u32 {
    unsafe { ffi::x_hash(bytes.as_ptr(), bytes.len() as u32) }
}
