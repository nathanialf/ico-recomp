//! `ee-emit`: translates the EE `.text` of the ICO retail US boot ELF into
//! C11, one file per code translation unit. See `emit` for the translation
//! model and `census` for the coverage policy.

mod census;

pub use census::census;
