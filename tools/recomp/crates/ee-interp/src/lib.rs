//! `ee-interp`: reference interpreter for the covered R5900 mnemonic set,
//! executing on the C-ABI `R5900Context` through a shim (csrc/shim.c) that
//! wraps the static inline helpers in include/recomp_ops.h. The shim is the
//! whole point: the interpreter and the emitter's generated C run the exact
//! same helper implementations, so the three-way tests in
//! tests/threeway.rs compare like against like.

mod ctx;
mod ffi;
mod interp;

pub use ctx::{set_page, Ctx};
pub use interp::step;
