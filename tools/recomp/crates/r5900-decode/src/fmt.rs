//! Canonical text formatting, aimed at matching spimdisasm's output style
//! (numeric GPRs, uppercase hex immediates, decimal shift amounts).

use std::fmt::{self, Display, Formatter};

use crate::insn::{Insn, Kind, Operand};

fn write_signed_hex(f: &mut Formatter<'_>, v: i64) -> fmt::Result {
    if v < 0 {
        write!(f, "-0x{:X}", -v)
    } else {
        write!(f, "0x{v:X}")
    }
}

impl Display for Operand {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        match *self {
            Operand::Gpr(n) => write!(f, "${n}"),
            Operand::Fpr(n) => write!(f, "$f{n}"),
            Operand::FpCtl(n) => write!(f, "${n}"),
            Operand::Cop0(n) => write!(f, "${n}"),
            Operand::Vf(n) => write!(f, "$vf{n}"),
            Operand::VfComp(n, c) => write!(f, "$vf{n}{c}"),
            Operand::Vi(n) => write!(f, "$vi{n}"),
            Operand::Acc => write!(f, "ACC"),
            Operand::Q => write!(f, "Q"),
            Operand::R => write!(f, "R"),
            Operand::I => write!(f, "I"),
            Operand::Imm(v) => write_signed_hex(f, v as i64),
            Operand::UImm(v) => write!(f, "0x{v:X}"),
            Operand::Dec(v) => write!(f, "{v}"),
            Operand::Hex2(v) => write!(f, "0x{v:02X}"),
            Operand::Mem { offset, base } => {
                write_signed_hex(f, offset as i64)?;
                write!(f, "(${base})")
            }
            Operand::Target(a) => write!(f, ".L{a:08X}"),
            Operand::ViInc(n) => write!(f, "($vi{n}++)"),
            Operand::ViDec(n) => write!(f, "(--$vi{n})"),
            Operand::ViInd(n) => write!(f, "($vi{n})"),
        }
    }
}

impl Display for Insn {
    fn fmt(&self, f: &mut Formatter<'_>) -> fmt::Result {
        match &self.kind {
            Kind::Invalid => write!(f, ".word 0x{:08X}", self.word),
            Kind::Op {
                mnemonic,
                operands,
            } => {
                write!(f, "{mnemonic}")?;
                for (i, op) in operands.iter().enumerate() {
                    if i == 0 {
                        write!(f, " {op}")?;
                    } else {
                        write!(f, ", {op}")?;
                    }
                }
                Ok(())
            }
        }
    }
}
