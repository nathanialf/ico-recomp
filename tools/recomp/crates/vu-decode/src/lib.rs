//! Decoder for PS2 VU (VU0/VU1) 64-bit instruction bundles.
//!
//! A bundle is 8 bytes. In memory (little-endian) the lower-pipe instruction
//! is the 32-bit word at offset +0 and the upper-pipe (FMAC) instruction is
//! the word at +4; equivalently, bits 0-31 of the 64-bit instruction are the
//! lower op and bits 32-63 the upper op.
//!
//! Field layout facts follow the VU instruction formats in the EE User's
//! Manual. The tables were cross-checked mechanically against the decomp
//! repo's clean-room VU tools (`tools/assemble_vu0.py` and
//! `tools/disasm_vu0.py` in the sibling ico checkout, MIT), and, for the
//! upper flag bit positions and the undefined lower opcode ranges, against
//! PCSX2's microVU dispatch tables as a behavioral reference only (GPL, no
//! code reused).
//!
//! Upper word layout:
//! - bit 31: I flag (the lower word of this bundle is a 32-bit immediate
//!   loaded into the I register, not an instruction; "LOI")
//! - bit 30: E flag (end of microprogram after one more bundle)
//! - bit 29: M flag (VU0 macro-mode sync)
//! - bit 28: D flag (debug break)
//! - bit 27: T flag (trap break)
//! - bits 25-26: reserved (nonzero in a few retail ICO bundles; preserved)
//! - bits 21-24: dest mask (x=bit 24, y=23, z=22, w=21)
//! - bits 16-20: ft, bits 11-15: fs, bits 6-10: fd
//! - bits 0-5: opcode; 0x3C-0x3F select a second-level table keyed by the
//!   fd field
//!
//! Lower word layout:
//! - bits 25-31: opcode; 0x40 selects the second-level table keyed by
//!   bits 0-5, whose entries 0x3C-0x3F select a third-level table keyed by
//!   the fd field
//! - operand fields as above, plus fsf at bits 21-22 and ftf at bits 23-24,
//!   imm11 at bits 0-10, imm15 = dest field (bits 21-24) : imm11,
//!   imm12 = bit 21 : imm11, imm24 at bits 0-23
//!
//! The decoder is total: every 32-bit word decodes to either a defined
//! instruction or an explicit `Invalid`. The ICO VU1 microprograms carry one
//! embedded DMA source-chain tag pair per program inside `.vutext` (upload
//! metadata, not instructions); see [`is_embedded_chain_tag`].

pub mod compat;
pub mod lower;
pub mod upper;

pub use lower::LowerOp;
pub use upper::UpperOp;

/// A VU floating-point register index, vf00 to vf31.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Vf(pub u8);

/// A VU integer register index, vi00 to vi15. Fields that are wider than
/// four bits in the encoding are masked to the architectural register count.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Vi(pub u8);

/// One component selector (broadcast field, fsf, or ftf): 0=x 1=y 2=z 3=w.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Comp {
    X,
    Y,
    Z,
    W,
}

impl Comp {
    pub fn from_bits(b: u32) -> Comp {
        match b & 3 {
            0 => Comp::X,
            1 => Comp::Y,
            2 => Comp::Z,
            _ => Comp::W,
        }
    }

    pub fn letter(self) -> char {
        match self {
            Comp::X => 'x',
            Comp::Y => 'y',
            Comp::Z => 'z',
            Comp::W => 'w',
        }
    }
}

/// Destination component mask from bits 21-24: x=bit 3, y=2, z=1, w=0 of the
/// stored nibble. An empty mask is legal (the op still runs for its flag
/// side effects but writes no lanes).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Dest(pub u8);

impl Dest {
    pub fn from_word(w: u32) -> Dest {
        Dest(((w >> 21) & 0xF) as u8)
    }

    pub fn x(self) -> bool {
        self.0 & 8 != 0
    }
    pub fn y(self) -> bool {
        self.0 & 4 != 0
    }
    pub fn z(self) -> bool {
        self.0 & 2 != 0
    }
    pub fn w(self) -> bool {
        self.0 & 1 != 0
    }
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// Mask letters in canonical xyzw order, e.g. "xyw". Empty for mask 0.
    pub fn letters(self) -> String {
        let mut s = String::new();
        for (c, b) in [('x', 8), ('y', 4), ('z', 2), ('w', 1)] {
            if self.0 & b != 0 {
                s.push(c);
            }
        }
        s
    }
}

/// Upper-word flag bits. Bit positions verified against PCSX2 microVU_Misc.h
/// (_Ibit_ = 1<<31, _Ebit_ = 1<<30, _Mbit_ = 1<<29, _Dbit_ = 1<<28,
/// _Tbit_ = 1<<27).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct UpperFlags {
    /// I: the lower word of this bundle is a 32-bit immediate (LOI).
    pub i: bool,
    /// E: end of microprogram; execution stops after the next bundle.
    pub e: bool,
    /// M: VU0 macro-mode sync bit.
    pub m: bool,
    /// D: debug break.
    pub d: bool,
    /// T: trap break.
    pub t: bool,
    /// Raw value of reserved bits 25-26. Retail ICO sets bit 25 on a few
    /// bundles; kept so nothing is silently dropped.
    pub reserved: u8,
}

impl UpperFlags {
    pub fn from_word(w: u32) -> UpperFlags {
        UpperFlags {
            i: w & (1 << 31) != 0,
            e: w & (1 << 30) != 0,
            m: w & (1 << 29) != 0,
            d: w & (1 << 28) != 0,
            t: w & (1 << 27) != 0,
            reserved: ((w >> 25) & 3) as u8,
        }
    }

    pub fn any(self) -> bool {
        self.i || self.e || self.m || self.d || self.t || self.reserved != 0
    }
}

/// The lower 32-bit slot of a bundle: an instruction, or the raw 32-bit
/// I-register immediate when the upper word has the I flag set.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum LowerSlot {
    Inst(LowerOp),
    /// LOI: raw immediate bits; usually an IEEE 754 single. Use
    /// `f32::from_bits` for the float view.
    Loi(u32),
}

/// One decoded 64-bit bundle.
#[derive(Debug, Clone, PartialEq)]
pub struct Bundle {
    /// Byte offset of the bundle from the start of the microprogram.
    pub offset: u32,
    pub upper_raw: u32,
    pub lower_raw: u32,
    pub flags: UpperFlags,
    pub upper: UpperOp,
    pub lower: LowerSlot,
}

impl Bundle {
    /// Decode from the two 32-bit words.
    pub fn decode(offset: u32, upper_raw: u32, lower_raw: u32) -> Bundle {
        let flags = UpperFlags::from_word(upper_raw);
        let lower = if flags.i {
            LowerSlot::Loi(lower_raw)
        } else {
            LowerSlot::Inst(lower::decode_lower(lower_raw))
        };
        Bundle {
            offset,
            upper_raw,
            lower_raw,
            flags,
            upper: upper::decode_upper(upper_raw),
            lower,
        }
    }

    /// Decode from 8 bytes as laid out in micro memory: lower word first,
    /// both little-endian.
    pub fn from_le_bytes(offset: u32, b: &[u8; 8]) -> Bundle {
        let lower = u32::from_le_bytes([b[0], b[1], b[2], b[3]]);
        let upper = u32::from_le_bytes([b[4], b[5], b[6], b[7]]);
        Bundle::decode(offset, upper, lower)
    }

    /// True when either half failed to decode to a defined instruction.
    pub fn has_invalid(&self) -> bool {
        matches!(self.upper, UpperOp::Invalid { .. })
            || matches!(self.lower, LowerSlot::Inst(LowerOp::Invalid { .. }))
    }
}

/// Decode a whole microprogram image. `bytes` must be a multiple of 8.
pub fn decode_program(bytes: &[u8]) -> Result<Vec<Bundle>, String> {
    if !bytes.len().is_multiple_of(8) {
        return Err(format!(
            "program length {} is not a multiple of 8",
            bytes.len()
        ));
    }
    let mut out = Vec::with_capacity(bytes.len() / 8);
    for (i, chunk) in bytes.chunks_exact(8).enumerate() {
        let mut b = [0u8; 8];
        b.copy_from_slice(chunk);
        out.push(Bundle::from_le_bytes((i * 8) as u32, &b));
    }
    Ok(out)
}

/// Branch target in bytes for an 11-bit signed bundle-granularity offset:
/// target = pc_of_branch + 8 + imm11 * 8 (the +8 is the delay slot bundle).
pub fn branch_target(offset: u32, imm11: i16) -> u32 {
    (offset as i64 + 8 + imm11 as i64 * 8) as u32
}

/// Recognize the DMA source-chain tags that ICO's build embeds in `.vutext`.
///
/// Each of the five VU1 microprograms starts with a bundle whose 64 bits are
/// a chain tag (lower word 0x600000QQ: ID=6, QWC=QQ, where QQ is the payload
/// size in quadwords) and ends with an end tag (lower word 0x70000000: ID=7,
/// QWC=0), both with an all-zero upper word. These are upload metadata read
/// by the DMAC, not VU instructions; their lower opcodes (op7 0x30 and 0x38)
/// are undefined encodings on the VU. Callers walking `.vutext` should treat
/// bundles matching this predicate as data.
pub fn is_embedded_chain_tag(upper_raw: u32, lower_raw: u32) -> bool {
    upper_raw == 0 && matches!(lower_raw >> 28, 0x6 | 0x7) && lower_raw & 0x0FFF_F800 == 0
}

pub(crate) fn bit_range(w: u32, lo: u32, hi: u32) -> u32 {
    (w >> lo) & ((1 << (hi - lo + 1)) - 1)
}

pub(crate) fn sign_extend(value: u32, width: u32) -> i32 {
    let shift = 32 - width;
    ((value << shift) as i32) >> shift
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flag_bits() {
        // pad (upper NOP encoding 0x2FF) with each flag bit set.
        let f = UpperFlags::from_word(0x800002FF);
        assert!(f.i && !f.e && !f.m && !f.d && !f.t && f.reserved == 0);
        let f = UpperFlags::from_word(0x400002FF);
        assert!(f.e && !f.i);
        let f = UpperFlags::from_word(0x200002FF);
        assert!(f.m);
        let f = UpperFlags::from_word(0x100002FF);
        assert!(f.d && !f.m, "bit 28 is D, not M");
        let f = UpperFlags::from_word(0x080002FF);
        assert!(f.t);
        // Reserved bit 25, as seen on a few retail bundles (E+T+bit25).
        let f = UpperFlags::from_word(0x4A000100);
        assert!(f.e && f.t && f.reserved == 1 && !f.i && !f.m && !f.d);
    }

    #[test]
    fn ebit_detection_on_program_end() {
        let b = Bundle::decode(0, 0x400002FF, 0);
        assert!(b.flags.e);
        assert_eq!(b.upper, UpperOp::Nop);
        assert_eq!(b.lower, LowerSlot::Inst(LowerOp::Nop));
    }

    #[test]
    fn ibit_makes_lower_a_loi() {
        // upper: pad with I bit; lower: 1.0f.
        let b = Bundle::decode(0, 0x800002FF, 0x3F800000);
        assert!(b.flags.i);
        assert_eq!(b.lower, LowerSlot::Loi(0x3F800000));
        match b.lower {
            LowerSlot::Loi(bits) => assert_eq!(f32::from_bits(bits), 1.0),
            _ => unreachable!(),
        }
        // Without the I bit the same word is an instruction.
        let b = Bundle::decode(0, 0x000002FF, 0x3F800000);
        assert!(matches!(b.lower, LowerSlot::Inst(_)));
    }

    #[test]
    fn branch_target_math() {
        // 11-bit signed immediate, bundle granularity, one delay bundle.
        assert_eq!(branch_target(0x100, 0), 0x108);
        assert_eq!(branch_target(0x100, -2), 0xF8);
        assert_eq!(branch_target(0x100, 1023), 0x100 + 8 + 1023 * 8);
        assert_eq!(branch_target(0x2000, -1024), 0x2000 + 8 - 1024 * 8);
        // b 0x300 assembled at pc 0x248 encodes imm11 = 0x16 (assemble_vu0.py
        // pin: word 0x40000016).
        assert_eq!(branch_target(0x248, 0x16), 0x300);
    }

    #[test]
    fn chain_tag_recognition() {
        // Shape of the embedded upload tags (ID/QWC fields, zero upper).
        assert!(is_embedded_chain_tag(0, 0x600000DA));
        assert!(is_embedded_chain_tag(0, 0x70000000));
        assert!(!is_embedded_chain_tag(1, 0x70000000));
        // A defined instruction never matches: op7 ranges 0x30-0x3F are the
        // only encodings with a 0x6/0x7 top nibble and those are undefined.
        assert!(!is_embedded_chain_tag(0, 0x40000016)); // b
        assert!(!is_embedded_chain_tag(0, 0x600800DA)); // payload bits set
    }

    #[test]
    fn dest_mask_letters() {
        assert_eq!(Dest(0b1111).letters(), "xyzw");
        assert_eq!(Dest(0b1000).letters(), "x");
        assert_eq!(Dest(0b0001).letters(), "w");
        assert_eq!(Dest(0b0110).letters(), "yz");
        assert!(Dest(0).is_empty());
        // From a word: add.xyzw vf01, vf02, vf03 = 0x01E31068.
        assert_eq!(Dest::from_word(0x01E31068), Dest(0b1111));
        // sub.w vf09, vf10, vf11 = 0x002B526C.
        assert_eq!(Dest::from_word(0x002B526C), Dest(0b0001));
    }
}
