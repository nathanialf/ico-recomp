//! Instruction model: a decoded R5900 instruction with structured operands.

/// One operand of a decoded instruction.
///
/// The variants carry enough semantic information for both formatting
/// (spimdisasm-style text) and value-level comparison in `verify-decode`
/// (e.g. branch targets are absolute vram, immediates keep their
/// sign-extended value).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Operand {
    /// General purpose register, printed numerically (`$29`).
    Gpr(u8),
    /// COP1 floating point register (`$f12`).
    Fpr(u8),
    /// COP1 control register (`$31` for FCR31), printed numerically.
    FpCtl(u8),
    /// COP0 system register, printed numerically (`$12`).
    Cop0(u8),
    /// VU0 float register (`$vf3`).
    Vf(u8),
    /// VU0 float register with a single-component suffix (`$vf3y`),
    /// used for broadcast fields and the fsf/ftf selectors.
    VfComp(u8, char),
    /// VU0 integer register (`$vi15`).
    Vi(u8),
    /// VU0 accumulator.
    Acc,
    /// VU0 Q register.
    Q,
    /// VU0 R register.
    R,
    /// VU0 I register.
    I,
    /// Immediate displayed as signed hex (`-0x20`, `0x54`).
    Imm(i32),
    /// Immediate displayed as unsigned hex (`0xFFFF`).
    UImm(u32),
    /// Immediate displayed as decimal (shift amounts, trap/break codes).
    Dec(u32),
    /// Immediate displayed as two-digit hex (`0x10`), for cache/pref ops.
    Hex2(u8),
    /// Memory operand `offset(base)` with a signed 16-bit offset.
    Mem { offset: i32, base: u8 },
    /// Branch or jump target as absolute vram.
    Target(u32),
    /// VU0 post-increment addressing `($vi13++)` (vlqi/vsqi).
    ViInc(u8),
    /// VU0 pre-decrement addressing `(--$vi13)` (vlqd/vsqd).
    ViDec(u8),
    /// VU0 plain register-indirect `($vi13)` (vilwr/viswr).
    ViInd(u8),
}

/// Decoded instruction kind.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Kind {
    /// Encoding that is not a valid EE instruction.
    Invalid,
    /// A valid instruction: full mnemonic (including any `.dest`, broadcast
    /// or `.i`/`.ni` suffixes) plus operands in display order.
    Op {
        mnemonic: String,
        operands: Vec<Operand>,
    },
}

/// A decoded instruction, tied to the word and address it came from.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Insn {
    pub word: u32,
    pub vram: u32,
    pub kind: Kind,
}

impl Insn {
    pub fn is_valid(&self) -> bool {
        !matches!(self.kind, Kind::Invalid)
    }

    pub fn mnemonic(&self) -> Option<&str> {
        match &self.kind {
            Kind::Invalid => None,
            Kind::Op { mnemonic, .. } => Some(mnemonic),
        }
    }

    pub fn operands(&self) -> &[Operand] {
        match &self.kind {
            Kind::Invalid => &[],
            Kind::Op { operands, .. } => operands,
        }
    }
}
