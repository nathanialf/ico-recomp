//! Total decoder for the EE core (MIPS R5900) instruction set.
//!
//! Every 32-bit word decodes to either a structured instruction or
//! `Kind::Invalid`; decoding never panics. The canonical formatter
//! (`Insn::to_string` / `Display`) targets the output conventions of
//! splat/spimdisasm as measured against the ICO decomp repo baselines,
//! since `verify-decode` diffs our disassembly against those files.
//!
//! Field layouts follow the EE Core Instruction Set manual. COP2 macro
//! field layouts were cross-checked against the decomp repo's MIT
//! disasm_vu0.py reference tables.

mod decode;
mod fmt;
mod insn;

pub use decode::decode;
pub use insn::{Insn, Kind, Operand};

#[cfg(test)]
mod tests;
