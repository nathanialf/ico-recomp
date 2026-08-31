//! Lower pipe instruction decoding.
//!
//! Opcode map (bits 25-31 of the lower word):
//! - 0x00 LQ, 0x01 SQ, 0x04 ILW, 0x05 ISW
//! - 0x08 IADDIU, 0x09 ISUBIU
//! - 0x10-0x1C flag ops (FC*/FS*/FM*)
//! - 0x20 B, 0x21 BAL, 0x24 JR, 0x25 JALR
//! - 0x28-0x2F conditional branches (IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ)
//! - 0x40 second-level table via bits 0-5: integer ALU ops at 0x30-0x35,
//!   and 0x3C-0x3F select a third-level table keyed by the fd field
//!   (bits 6-10): moves, LSU register forms, DIV unit, random unit, EFU,
//!   XTOP/XITOP/XGKICK, WAITQ/WAITP.
//! - everything else (including 0x30-0x3F and 0x41-0x7F): undefined.
//!
//! Layouts per the EE User's Manual; cross-checked against the decomp
//! repo's assemble_vu0.py/disasm_vu0.py tables and, for the undefined
//! ranges, PCSX2's microVU lower dispatch table (behavioral reference).

use crate::{bit_range, sign_extend, Comp, Dest, Vf, Vi};

/// A decoded lower instruction. When the upper word's I flag is set the
/// lower slot is not an instruction at all; see [`crate::LowerSlot::Loi`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LowerOp {
    /// The all-zero word. (Encoded zero is LQ with every field zero and an
    /// empty dest mask, which writes nothing; assemblers emit it as the
    /// lower no-op alongside the canonical NOP encoding 0x8000033C, which
    /// decodes here as `Move` with an empty mask.)
    Nop,

    // Load/store, immediate offset (11-bit signed, quadword granularity).
    Lq {
        dest: Dest,
        ft: Vf,
        is: Vi,
        imm11: i16,
    },
    Sq {
        dest: Dest,
        fs: Vf,
        it: Vi,
        imm11: i16,
    },
    Ilw {
        dest: Dest,
        it: Vi,
        is: Vi,
        imm11: i16,
    },
    Isw {
        dest: Dest,
        it: Vi,
        is: Vi,
        imm11: i16,
    },
    // Load/store, register forms with pre-decrement (LQD/SQD) or
    // post-increment (LQI/SQI).
    Lqd {
        dest: Dest,
        ft: Vf,
        is: Vi,
    },
    Lqi {
        dest: Dest,
        ft: Vf,
        is: Vi,
    },
    Sqd {
        dest: Dest,
        fs: Vf,
        it: Vi,
    },
    Sqi {
        dest: Dest,
        fs: Vf,
        it: Vi,
    },
    Ilwr {
        dest: Dest,
        it: Vi,
        is: Vi,
    },
    Iswr {
        dest: Dest,
        it: Vi,
        is: Vi,
    },

    // Integer ALU.
    Iadd {
        id: Vi,
        is: Vi,
        it: Vi,
    },
    Isub {
        id: Vi,
        is: Vi,
        it: Vi,
    },
    Iand {
        id: Vi,
        is: Vi,
        it: Vi,
    },
    Ior {
        id: Vi,
        is: Vi,
        it: Vi,
    },
    /// it = is + imm5 (signed 5-bit immediate in the fd field).
    Iaddi {
        it: Vi,
        is: Vi,
        imm5: i8,
    },
    /// it = is + imm15 (unsigned; top 4 bits in the dest field).
    Iaddiu {
        it: Vi,
        is: Vi,
        imm15: u16,
    },
    Isubiu {
        it: Vi,
        is: Vi,
        imm15: u16,
    },

    // Float register moves.
    Move {
        dest: Dest,
        ft: Vf,
        fs: Vf,
    },
    /// ft = fs rotated: x<-y, y<-z, z<-w, w<-x.
    Mr32 {
        dest: Dest,
        ft: Vf,
        fs: Vf,
    },
    Mfir {
        dest: Dest,
        ft: Vf,
        is: Vi,
    },
    Mtir {
        it: Vi,
        fs: Vf,
        fsf: Comp,
    },
    /// ft = P register (EFU result), on dest lanes.
    Mfp {
        dest: Dest,
        ft: Vf,
    },

    // DIV unit (writes Q).
    Div {
        fs: Vf,
        fsf: Comp,
        ft: Vf,
        ftf: Comp,
    },
    Sqrt {
        ft: Vf,
        ftf: Comp,
    },
    Rsqrt {
        fs: Vf,
        fsf: Comp,
        ft: Vf,
        ftf: Comp,
    },
    Waitq,

    // Random unit (R register).
    Rinit {
        fs: Vf,
        fsf: Comp,
    },
    Rget {
        dest: Dest,
        ft: Vf,
    },
    Rnext {
        dest: Dest,
        ft: Vf,
    },
    Rxor {
        fs: Vf,
        fsf: Comp,
    },

    // EFU (VU1 only; decode-only coverage, ICO uses none of them).
    Esadd {
        fs: Vf,
    },
    Ersadd {
        fs: Vf,
    },
    Eleng {
        fs: Vf,
    },
    Erleng {
        fs: Vf,
    },
    Eatanxy {
        fs: Vf,
    },
    Eatanxz {
        fs: Vf,
    },
    Esum {
        fs: Vf,
    },
    Esqrt {
        ft: Vf,
        ftf: Comp,
    },
    Ersqrt {
        ft: Vf,
        ftf: Comp,
    },
    Ercpr {
        ft: Vf,
        ftf: Comp,
    },
    Esin {
        ft: Vf,
        ftf: Comp,
    },
    Eatan {
        ft: Vf,
        ftf: Comp,
    },
    Eexp {
        ft: Vf,
        ftf: Comp,
    },
    Waitp,

    // Flag ops. imm12 = bit 21 : bits 0-10; imm24 = bits 0-23.
    Fsand {
        it: Vi,
        imm12: u16,
    },
    Fseq {
        it: Vi,
        imm12: u16,
    },
    Fsor {
        it: Vi,
        imm12: u16,
    },
    Fsset {
        imm12: u16,
    },
    Fmand {
        it: Vi,
        is: Vi,
    },
    Fmeq {
        it: Vi,
        is: Vi,
    },
    Fmor {
        it: Vi,
        is: Vi,
    },
    Fcand {
        imm24: u32,
    },
    Fceq {
        imm24: u32,
    },
    Fcor {
        imm24: u32,
    },
    Fcset {
        imm24: u32,
    },
    Fcget {
        it: Vi,
    },

    // Branches. imm11 is signed, in bundles; see [`crate::branch_target`].
    Ibeq {
        it: Vi,
        is: Vi,
        imm11: i16,
    },
    Ibne {
        it: Vi,
        is: Vi,
        imm11: i16,
    },
    Ibltz {
        is: Vi,
        imm11: i16,
    },
    Ibgtz {
        is: Vi,
        imm11: i16,
    },
    Iblez {
        is: Vi,
        imm11: i16,
    },
    Ibgez {
        is: Vi,
        imm11: i16,
    },
    B {
        imm11: i16,
    },
    Bal {
        it: Vi,
        imm11: i16,
    },
    Jr {
        is: Vi,
    },
    Jalr {
        it: Vi,
        is: Vi,
    },

    // VU1 XGKICK path and VIF doubling-buffer pointers.
    Xtop {
        it: Vi,
    },
    Xitop {
        it: Vi,
    },
    Xgkick {
        is: Vi,
    },

    /// Encoding the hardware does not define.
    Invalid {
        raw: u32,
    },
}

struct Fields {
    dest: Dest,
    fd: u32,
    fs: u32,
    ft: u32,
    it: Vi,
    is: Vi,
    imm11: i16,
    fsf: Comp,
    ftf: Comp,
}

fn fields(w: u32) -> Fields {
    Fields {
        dest: Dest::from_word(w),
        fd: bit_range(w, 6, 10),
        fs: bit_range(w, 11, 15),
        ft: bit_range(w, 16, 20),
        it: Vi((bit_range(w, 16, 20) & 0xF) as u8),
        is: Vi((bit_range(w, 11, 15) & 0xF) as u8),
        imm11: sign_extend(bit_range(w, 0, 10), 11) as i16,
        fsf: Comp::from_bits(bit_range(w, 21, 22)),
        ftf: Comp::from_bits(bit_range(w, 23, 24)),
    }
}

fn imm15(w: u32) -> u16 {
    ((bit_range(w, 21, 24) << 11) | bit_range(w, 0, 10)) as u16
}

fn imm12(w: u32) -> u16 {
    ((bit_range(w, 21, 21) << 11) | bit_range(w, 0, 10)) as u16
}

/// Decode the 32-bit lower word. LOI handling (upper I flag) is the
/// caller's job; this treats the word as an instruction unconditionally.
pub fn decode_lower(w: u32) -> LowerOp {
    if w == 0 {
        return LowerOp::Nop;
    }
    let f = fields(w);
    let op7 = w >> 25;
    match op7 {
        0x00 => LowerOp::Lq {
            dest: f.dest,
            ft: Vf(f.ft as u8),
            is: f.is,
            imm11: f.imm11,
        },
        0x01 => LowerOp::Sq {
            dest: f.dest,
            fs: Vf(f.fs as u8),
            it: f.it,
            imm11: f.imm11,
        },
        0x04 => LowerOp::Ilw {
            dest: f.dest,
            it: f.it,
            is: f.is,
            imm11: f.imm11,
        },
        0x05 => LowerOp::Isw {
            dest: f.dest,
            it: f.it,
            is: f.is,
            imm11: f.imm11,
        },
        0x08 => LowerOp::Iaddiu {
            it: f.it,
            is: f.is,
            imm15: imm15(w),
        },
        0x09 => LowerOp::Isubiu {
            it: f.it,
            is: f.is,
            imm15: imm15(w),
        },
        0x10 => LowerOp::Fceq {
            imm24: bit_range(w, 0, 23),
        },
        0x11 => LowerOp::Fcset {
            imm24: bit_range(w, 0, 23),
        },
        0x12 => LowerOp::Fcand {
            imm24: bit_range(w, 0, 23),
        },
        0x13 => LowerOp::Fcor {
            imm24: bit_range(w, 0, 23),
        },
        0x14 => LowerOp::Fseq {
            it: f.it,
            imm12: imm12(w),
        },
        0x15 => LowerOp::Fsset { imm12: imm12(w) },
        0x16 => LowerOp::Fsand {
            it: f.it,
            imm12: imm12(w),
        },
        0x17 => LowerOp::Fsor {
            it: f.it,
            imm12: imm12(w),
        },
        0x18 => LowerOp::Fmeq { it: f.it, is: f.is },
        0x1A => LowerOp::Fmand { it: f.it, is: f.is },
        0x1B => LowerOp::Fmor { it: f.it, is: f.is },
        0x1C => LowerOp::Fcget { it: f.it },
        0x20 => LowerOp::B { imm11: f.imm11 },
        0x21 => LowerOp::Bal {
            it: f.it,
            imm11: f.imm11,
        },
        0x24 => LowerOp::Jr { is: f.is },
        0x25 => LowerOp::Jalr { it: f.it, is: f.is },
        0x28 => LowerOp::Ibeq {
            it: f.it,
            is: f.is,
            imm11: f.imm11,
        },
        0x29 => LowerOp::Ibne {
            it: f.it,
            is: f.is,
            imm11: f.imm11,
        },
        0x2C => LowerOp::Ibltz {
            is: f.is,
            imm11: f.imm11,
        },
        0x2D => LowerOp::Ibgtz {
            is: f.is,
            imm11: f.imm11,
        },
        0x2E => LowerOp::Iblez {
            is: f.is,
            imm11: f.imm11,
        },
        0x2F => LowerOp::Ibgez {
            is: f.is,
            imm11: f.imm11,
        },
        0x40 => decode_special(w, &f),
        _ => LowerOp::Invalid { raw: w },
    }
}

fn decode_special(w: u32, f: &Fields) -> LowerOp {
    let id = Vi((f.fd & 0xF) as u8);
    match w & 0x3F {
        0x30 => LowerOp::Iadd {
            id,
            is: f.is,
            it: f.it,
        },
        0x31 => LowerOp::Isub {
            id,
            is: f.is,
            it: f.it,
        },
        0x32 => LowerOp::Iaddi {
            it: f.it,
            is: f.is,
            imm5: sign_extend(f.fd, 5) as i8,
        },
        0x34 => LowerOp::Iand {
            id,
            is: f.is,
            it: f.it,
        },
        0x35 => LowerOp::Ior {
            id,
            is: f.is,
            it: f.it,
        },
        sub6 @ 0x3C..=0x3F => decode_special_fd(w, f, sub6 & 3),
        _ => LowerOp::Invalid { raw: w },
    }
}

fn decode_special_fd(w: u32, f: &Fields, bc: u32) -> LowerOp {
    let ft_vf = Vf(f.ft as u8);
    let fs_vf = Vf(f.fs as u8);
    // Keyed by (third-level entry from the fd field, bc from bits 0-1).
    match (f.fd, bc) {
        (0x0C, 0) => LowerOp::Move {
            dest: f.dest,
            ft: ft_vf,
            fs: fs_vf,
        },
        (0x0C, 1) => LowerOp::Mr32 {
            dest: f.dest,
            ft: ft_vf,
            fs: fs_vf,
        },
        (0x0D, 0) => LowerOp::Lqi {
            dest: f.dest,
            ft: ft_vf,
            is: f.is,
        },
        (0x0D, 1) => LowerOp::Sqi {
            dest: f.dest,
            fs: fs_vf,
            it: f.it,
        },
        (0x0D, 2) => LowerOp::Lqd {
            dest: f.dest,
            ft: ft_vf,
            is: f.is,
        },
        (0x0D, 3) => LowerOp::Sqd {
            dest: f.dest,
            fs: fs_vf,
            it: f.it,
        },
        (0x0E, 0) => LowerOp::Div {
            fs: fs_vf,
            fsf: f.fsf,
            ft: ft_vf,
            ftf: f.ftf,
        },
        (0x0E, 1) => LowerOp::Sqrt {
            ft: ft_vf,
            ftf: f.ftf,
        },
        (0x0E, 2) => LowerOp::Rsqrt {
            fs: fs_vf,
            fsf: f.fsf,
            ft: ft_vf,
            ftf: f.ftf,
        },
        (0x0E, 3) => LowerOp::Waitq,
        (0x0F, 0) => LowerOp::Mtir {
            it: f.it,
            fs: fs_vf,
            fsf: f.fsf,
        },
        (0x0F, 1) => LowerOp::Mfir {
            dest: f.dest,
            ft: ft_vf,
            is: f.is,
        },
        (0x0F, 2) => LowerOp::Ilwr {
            dest: f.dest,
            it: f.it,
            is: f.is,
        },
        (0x0F, 3) => LowerOp::Iswr {
            dest: f.dest,
            it: f.it,
            is: f.is,
        },
        (0x10, 0) => LowerOp::Rnext {
            dest: f.dest,
            ft: ft_vf,
        },
        (0x10, 1) => LowerOp::Rget {
            dest: f.dest,
            ft: ft_vf,
        },
        (0x10, 2) => LowerOp::Rinit {
            fs: fs_vf,
            fsf: f.fsf,
        },
        (0x10, 3) => LowerOp::Rxor {
            fs: fs_vf,
            fsf: f.fsf,
        },
        (0x19, 0) => LowerOp::Mfp {
            dest: f.dest,
            ft: ft_vf,
        },
        (0x1A, 0) => LowerOp::Xtop { it: f.it },
        (0x1A, 1) => LowerOp::Xitop { it: f.it },
        (0x1B, 0) => LowerOp::Xgkick { is: f.is },
        (0x1C, 0) => LowerOp::Esadd { fs: fs_vf },
        (0x1C, 1) => LowerOp::Ersadd { fs: fs_vf },
        (0x1C, 2) => LowerOp::Eleng { fs: fs_vf },
        (0x1C, 3) => LowerOp::Erleng { fs: fs_vf },
        (0x1D, 0) => LowerOp::Eatanxy { fs: fs_vf },
        (0x1D, 1) => LowerOp::Eatanxz { fs: fs_vf },
        (0x1D, 2) => LowerOp::Esum { fs: fs_vf },
        (0x1E, 0) => LowerOp::Esqrt {
            ft: ft_vf,
            ftf: f.ftf,
        },
        (0x1E, 1) => LowerOp::Ersqrt {
            ft: ft_vf,
            ftf: f.ftf,
        },
        (0x1E, 2) => LowerOp::Ercpr {
            ft: ft_vf,
            ftf: f.ftf,
        },
        (0x1E, 3) => LowerOp::Waitp,
        (0x1F, 0) => LowerOp::Esin {
            ft: ft_vf,
            ftf: f.ftf,
        },
        (0x1F, 1) => LowerOp::Eatan {
            ft: ft_vf,
            ftf: f.ftf,
        },
        (0x1F, 2) => LowerOp::Eexp {
            ft: ft_vf,
            ftf: f.ftf,
        },
        _ => LowerOp::Invalid { raw: w },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Expected words below were produced by the decomp repo's clean-room
    // assembler (../ico/tools/assemble_vu0.py, MIT) from hand-picked
    // operands; they are not game bytes.

    #[test]
    fn loads_and_stores() {
        // lq.xyzw vf01, -3(vi02)
        assert_eq!(
            decode_lower(0x01E117FD),
            LowerOp::Lq {
                dest: Dest(0b1111),
                ft: Vf(1),
                is: Vi(2),
                imm11: -3,
            }
        );
        // sq.xyz vf04, 5(vi06)
        assert_eq!(
            decode_lower(0x03C62005),
            LowerOp::Sq {
                dest: Dest(0b1110),
                fs: Vf(4),
                it: Vi(6),
                imm11: 5,
            }
        );
        // ilw.w vi01, 4(vi02)
        assert_eq!(
            decode_lower(0x08211004),
            LowerOp::Ilw {
                dest: Dest(0b0001),
                it: Vi(1),
                is: Vi(2),
                imm11: 4,
            }
        );
        // isw.x vi07, -1(vi08)
        assert_eq!(
            decode_lower(0x0B0747FF),
            LowerOp::Isw {
                dest: Dest(0b1000),
                it: Vi(7),
                is: Vi(8),
                imm11: -1,
            }
        );
        // lqi.xyzw vf03, vi07 / sqi.xyz vf04, vi08
        // lqd.xyzw vf05, vi09 / sqd.w vf06, vi10
        assert_eq!(
            decode_lower(0x81E33B7C),
            LowerOp::Lqi {
                dest: Dest(0b1111),
                ft: Vf(3),
                is: Vi(7),
            }
        );
        assert_eq!(
            decode_lower(0x81C8237D),
            LowerOp::Sqi {
                dest: Dest(0b1110),
                fs: Vf(4),
                it: Vi(8),
            }
        );
        assert_eq!(
            decode_lower(0x81E54B7E),
            LowerOp::Lqd {
                dest: Dest(0b1111),
                ft: Vf(5),
                is: Vi(9),
            }
        );
        assert_eq!(
            decode_lower(0x802A337F),
            LowerOp::Sqd {
                dest: Dest(0b0001),
                fs: Vf(6),
                it: Vi(10),
            }
        );
        // ilwr.y vi01, vi02 / iswr.y vi03, vi04
        assert_eq!(
            decode_lower(0x808113FE),
            LowerOp::Ilwr {
                dest: Dest(0b0100),
                it: Vi(1),
                is: Vi(2),
            }
        );
        assert_eq!(
            decode_lower(0x808323FF),
            LowerOp::Iswr {
                dest: Dest(0b0100),
                it: Vi(3),
                is: Vi(4),
            }
        );
    }

    #[test]
    fn integer_alu() {
        // iadd vi01, vi02, vi03 / isub vi07, vi08, vi09
        // iand vi04, vi05, vi06 / ior vi10, vi11, vi12
        assert_eq!(
            decode_lower(0x80031070),
            LowerOp::Iadd {
                id: Vi(1),
                is: Vi(2),
                it: Vi(3),
            }
        );
        assert_eq!(
            decode_lower(0x800941F1),
            LowerOp::Isub {
                id: Vi(7),
                is: Vi(8),
                it: Vi(9),
            }
        );
        assert_eq!(
            decode_lower(0x80062934),
            LowerOp::Iand {
                id: Vi(4),
                is: Vi(5),
                it: Vi(6),
            }
        );
        assert_eq!(
            decode_lower(0x800C5AB5),
            LowerOp::Ior {
                id: Vi(10),
                is: Vi(11),
                it: Vi(12),
            }
        );
        // iaddi vi01, vi02, -16 (signed 5-bit boundary value)
        assert_eq!(
            decode_lower(0x80011432),
            LowerOp::Iaddi {
                it: Vi(1),
                is: Vi(2),
                imm5: -16,
            }
        );
        // iaddiu vi05, vi03, 32767 (15-bit immediate split across fields)
        assert_eq!(
            decode_lower(0x11E51FFF),
            LowerOp::Iaddiu {
                it: Vi(5),
                is: Vi(3),
                imm15: 32767,
            }
        );
        // isubiu vi05, vi03, 2047 (fits entirely in the low 11 bits)
        assert_eq!(
            decode_lower(0x12051FFF),
            LowerOp::Isubiu {
                it: Vi(5),
                is: Vi(3),
                imm15: 2047,
            }
        );
    }

    #[test]
    fn moves_div_random() {
        // move.xyzw vf11, vf12 / mr32.xyzw vf13, vf14
        assert_eq!(
            decode_lower(0x81EB633C),
            LowerOp::Move {
                dest: Dest(0b1111),
                ft: Vf(11),
                fs: Vf(12),
            }
        );
        assert_eq!(
            decode_lower(0x81ED733D),
            LowerOp::Mr32 {
                dest: Dest(0b1111),
                ft: Vf(13),
                fs: Vf(14),
            }
        );
        // mfir.xyzw vf10, vi03 / mtir vi05, vf09.z
        assert_eq!(
            decode_lower(0x81EA1BFD),
            LowerOp::Mfir {
                dest: Dest(0b1111),
                ft: Vf(10),
                is: Vi(3),
            }
        );
        assert_eq!(
            decode_lower(0x80454BFC),
            LowerOp::Mtir {
                it: Vi(5),
                fs: Vf(9),
                fsf: Comp::Z,
            }
        );
        // div Q, vf01.x, vf02.w / sqrt Q, vf03.y / rsqrt Q, vf04.z, vf05.w
        assert_eq!(
            decode_lower(0x81820BBC),
            LowerOp::Div {
                fs: Vf(1),
                fsf: Comp::X,
                ft: Vf(2),
                ftf: Comp::W,
            }
        );
        assert_eq!(
            decode_lower(0x808303BD),
            LowerOp::Sqrt {
                ft: Vf(3),
                ftf: Comp::Y,
            }
        );
        assert_eq!(
            decode_lower(0x81C523BE),
            LowerOp::Rsqrt {
                fs: Vf(4),
                fsf: Comp::Z,
                ft: Vf(5),
                ftf: Comp::W,
            }
        );
        assert_eq!(decode_lower(0x800003BF), LowerOp::Waitq);
        // rinit R, vf04.y / rget.xyzw vf07 / rnext.xy vf08 / rxor R, vf09.x
        assert_eq!(
            decode_lower(0x8020243E),
            LowerOp::Rinit {
                fs: Vf(4),
                fsf: Comp::Y,
            }
        );
        assert_eq!(
            decode_lower(0x81E7043D),
            LowerOp::Rget {
                dest: Dest(0b1111),
                ft: Vf(7),
            }
        );
        assert_eq!(
            decode_lower(0x8188043C),
            LowerOp::Rnext {
                dest: Dest(0b1100),
                ft: Vf(8),
            }
        );
        assert_eq!(
            decode_lower(0x80004C3F),
            LowerOp::Rxor {
                fs: Vf(9),
                fsf: Comp::X,
            }
        );
        // mfp.xyzw vf15 / waitp
        assert_eq!(
            decode_lower(0x81EF067C),
            LowerOp::Mfp {
                dest: Dest(0b1111),
                ft: Vf(15),
            }
        );
        assert_eq!(decode_lower(0x800007BF), LowerOp::Waitp);
        // xtop vi03 / xitop vi04 / xgkick vi14
        assert_eq!(decode_lower(0x800306BC), LowerOp::Xtop { it: Vi(3) });
        assert_eq!(decode_lower(0x800406BD), LowerOp::Xitop { it: Vi(4) });
        assert_eq!(decode_lower(0x800076FC), LowerOp::Xgkick { is: Vi(14) });
    }

    #[test]
    fn flag_ops() {
        // fsand vi08, 0x2 / fsset 0x800 / fseq vi02, 0xFFF / fsor vi03, 0x1
        assert_eq!(
            decode_lower(0x2C080002),
            LowerOp::Fsand {
                it: Vi(8),
                imm12: 2,
            }
        );
        assert_eq!(decode_lower(0x2A200000), LowerOp::Fsset { imm12: 0x800 });
        assert_eq!(
            decode_lower(0x282207FF),
            LowerOp::Fseq {
                it: Vi(2),
                imm12: 0xFFF,
            }
        );
        assert_eq!(
            decode_lower(0x2E030001),
            LowerOp::Fsor {
                it: Vi(3),
                imm12: 1,
            }
        );
        // fmand vi05, vi06 / fmeq vi07, vi08 / fmor vi09, vi10
        assert_eq!(
            decode_lower(0x34053000),
            LowerOp::Fmand {
                it: Vi(5),
                is: Vi(6),
            }
        );
        assert_eq!(
            decode_lower(0x30074000),
            LowerOp::Fmeq {
                it: Vi(7),
                is: Vi(8),
            }
        );
        assert_eq!(
            decode_lower(0x36095000),
            LowerOp::Fmor {
                it: Vi(9),
                is: Vi(10),
            }
        );
        // fcand vi01, 0xFFFFFF / fcor vi01, 0x123456 / fceq vi01, 0x1
        // fcset 0xABCDEF / fcget vi09
        assert_eq!(decode_lower(0x24FFFFFF), LowerOp::Fcand { imm24: 0xFFFFFF });
        assert_eq!(decode_lower(0x26123456), LowerOp::Fcor { imm24: 0x123456 });
        assert_eq!(decode_lower(0x20000001), LowerOp::Fceq { imm24: 1 });
        assert_eq!(decode_lower(0x22ABCDEF), LowerOp::Fcset { imm24: 0xABCDEF });
        assert_eq!(decode_lower(0x38090000), LowerOp::Fcget { it: Vi(9) });
    }

    #[test]
    fn branches_and_jumps() {
        // Words assembled at known pcs; see also crate::tests::branch_target_math.
        // b 0x300 assembled at pc 0x248
        assert_eq!(decode_lower(0x40000016), LowerOp::B { imm11: 0x16 });
        // bal vi15, 0x100 assembled at pc 0x250 (backward: imm11 = -0x2B)
        assert_eq!(
            decode_lower(0x420F07D5),
            LowerOp::Bal {
                it: Vi(15),
                imm11: -0x2B,
            }
        );
        assert_eq!(crate::branch_target(0x250, -0x2B), 0x100);
        // ibeq vi01, vi02, 0x200 assembled at pc 0x258 (imm11 = -12)
        assert_eq!(
            decode_lower(0x500117F4),
            LowerOp::Ibeq {
                it: Vi(1),
                is: Vi(2),
                imm11: -12,
            }
        );
        assert_eq!(crate::branch_target(0x258, -12), 0x200);
        // ibne vi03, vi04, 0x0 assembled at pc 0x260 (imm11 = -77)
        assert_eq!(
            decode_lower(0x520327B3),
            LowerOp::Ibne {
                it: Vi(3),
                is: Vi(4),
                imm11: -77,
            }
        );
        assert_eq!(crate::branch_target(0x260, -77), 0);
        // ibltz vi05, 0x280 @ 0x268 / ibgtz vi06, 0x280 @ 0x270
        // iblez vi07, 0x280 @ 0x278 / ibgez vi08, 0x280 @ 0x280
        assert_eq!(
            decode_lower(0x58002802),
            LowerOp::Ibltz {
                is: Vi(5),
                imm11: 2,
            }
        );
        assert_eq!(
            decode_lower(0x5A003001),
            LowerOp::Ibgtz {
                is: Vi(6),
                imm11: 1,
            }
        );
        assert_eq!(
            decode_lower(0x5C003800),
            LowerOp::Iblez {
                is: Vi(7),
                imm11: 0,
            }
        );
        assert_eq!(
            decode_lower(0x5E0047FF),
            LowerOp::Ibgez {
                is: Vi(8),
                imm11: -1,
            }
        );
        // jr vi11 / jalr vi12, vi13
        assert_eq!(decode_lower(0x48005800), LowerOp::Jr { is: Vi(11) });
        assert_eq!(
            decode_lower(0x4A0C6800),
            LowerOp::Jalr {
                it: Vi(12),
                is: Vi(13),
            }
        );
    }

    #[test]
    fn efu_decode_only() {
        assert_eq!(decode_lower(0x80001F3C), LowerOp::Esadd { fs: Vf(3) });
        assert_eq!(decode_lower(0x8000273D), LowerOp::Ersadd { fs: Vf(4) });
        assert_eq!(decode_lower(0x80002F3E), LowerOp::Eleng { fs: Vf(5) });
        assert_eq!(decode_lower(0x8000373F), LowerOp::Erleng { fs: Vf(6) });
        assert_eq!(decode_lower(0x80003F7C), LowerOp::Eatanxy { fs: Vf(7) });
        assert_eq!(decode_lower(0x8000477D), LowerOp::Eatanxz { fs: Vf(8) });
        assert_eq!(decode_lower(0x80004F7E), LowerOp::Esum { fs: Vf(9) });
        assert_eq!(
            decode_lower(0x808A07BC),
            LowerOp::Esqrt {
                ft: Vf(10),
                ftf: Comp::Y,
            }
        );
        assert_eq!(
            decode_lower(0x810B07BD),
            LowerOp::Ersqrt {
                ft: Vf(11),
                ftf: Comp::Z,
            }
        );
        assert_eq!(
            decode_lower(0x818C07BE),
            LowerOp::Ercpr {
                ft: Vf(12),
                ftf: Comp::W,
            }
        );
        assert_eq!(
            decode_lower(0x800D07FC),
            LowerOp::Esin {
                ft: Vf(13),
                ftf: Comp::X,
            }
        );
        assert_eq!(
            decode_lower(0x808E07FD),
            LowerOp::Eatan {
                ft: Vf(14),
                ftf: Comp::Y,
            }
        );
        assert_eq!(
            decode_lower(0x810F07FE),
            LowerOp::Eexp {
                ft: Vf(15),
                ftf: Comp::Z,
            }
        );
    }

    #[test]
    fn nop_and_invalid() {
        assert_eq!(decode_lower(0), LowerOp::Nop);
        // Canonical lower NOP encoding: MOVE with all-zero operands and an
        // empty dest mask.
        assert_eq!(
            decode_lower(0x8000033C),
            LowerOp::Move {
                dest: Dest(0),
                ft: Vf(0),
                fs: Vf(0),
            }
        );
        // Undefined primary opcodes.
        for op7 in [
            0x02u32, 0x06, 0x0A, 0x19, 0x1D, 0x22, 0x26, 0x30, 0x38, 0x41, 0x7F,
        ] {
            let w = op7 << 25 | 1;
            assert_eq!(decode_lower(w), LowerOp::Invalid { raw: w });
        }
        // Undefined second-level entries under 0x40.
        let w = (0x40 << 25) | 0x33;
        assert_eq!(decode_lower(w), LowerOp::Invalid { raw: w });
        // Undefined third-level entry: (0x11, bc 0).
        let w = (0x40 << 25) | (0x11 << 6) | 0x3C;
        assert_eq!(decode_lower(w), LowerOp::Invalid { raw: w });
    }
}
