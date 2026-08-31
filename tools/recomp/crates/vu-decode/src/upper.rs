//! Upper (FMAC) pipe instruction decoding.
//!
//! Opcode map (bits 0-5 of the upper word):
//! - 0x00-0x1B: broadcast FMAC. Family in bits 2-5 (0=ADD 1=SUB 2=MADD
//!   3=MSUB 4=MAX 5=MINI 6=MUL), broadcast component in bits 0-1.
//! - 0x1C-0x27: I/Q-operand FMAC (ft slot replaced by the I or Q register).
//! - 0x28-0x2F: plain three-register FMAC, plus OPMSUB.
//! - 0x3C-0x3F: second-level table selected by bits 0-1, entry selected by
//!   the fd field (bits 6-10): accumulator forms, ITOF/FTOI, ABS, CLIP, NOP.
//! - everything else: undefined.
//!
//! Layouts per the EE User's Manual; cross-checked against the decomp
//! repo's assemble_vu0.py/disasm_vu0.py tables.

use crate::{bit_range, Comp, Dest, Vf};

/// FMAC operation family.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FmacOp {
    Add,
    Sub,
    Madd,
    Msub,
    Max,
    Mini,
    Mul,
}

impl FmacOp {
    pub fn name(self) -> &'static str {
        match self {
            FmacOp::Add => "add",
            FmacOp::Sub => "sub",
            FmacOp::Madd => "madd",
            FmacOp::Msub => "msub",
            FmacOp::Max => "max",
            FmacOp::Mini => "mini",
            FmacOp::Mul => "mul",
        }
    }
}

/// The second source operand of an FMAC op.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Rhs {
    /// Full ft register.
    Ft(Vf),
    /// One broadcast component of ft.
    Bc(Vf, Comp),
    /// The I register. Max and Mini support this form; Q does not exist for
    /// them.
    I,
    /// The Q register.
    Q,
}

/// Fixed-point position for FTOI/ITOF: number of fraction bits.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FixedPoint {
    F0,
    F4,
    F12,
    F15,
}

impl FixedPoint {
    fn from_bc(bc: u32) -> FixedPoint {
        match bc & 3 {
            0 => FixedPoint::F0,
            1 => FixedPoint::F4,
            2 => FixedPoint::F12,
            _ => FixedPoint::F15,
        }
    }

    pub fn suffix(self) -> &'static str {
        match self {
            FixedPoint::F0 => "0",
            FixedPoint::F4 => "4",
            FixedPoint::F12 => "12",
            FixedPoint::F15 => "15",
        }
    }
}

/// A decoded upper instruction. Flag bits live in
/// [`crate::UpperFlags`], not here.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum UpperOp {
    /// Upper NOP: second-level entry (w, 0x0B). The canonical encodings are
    /// 0x000002FF ("pad") and, with the E bit, 0x400002FF.
    Nop,
    /// fd = fs op rhs, on dest lanes.
    Fmac {
        op: FmacOp,
        dest: Dest,
        fd: Vf,
        fs: Vf,
        rhs: Rhs,
    },
    /// ACC = fs op rhs, on dest lanes (ADDA/SUBA/MADDA/MSUBA/MULA families).
    /// MADDA/MSUBA read ACC as well. Max/Mini have no accumulator form.
    FmacA {
        op: FmacOp,
        dest: Dest,
        fs: Vf,
        rhs: Rhs,
    },
    /// Outer product start: ACC = fs x ft (fixed xyz semantics).
    Opmula { dest: Dest, fs: Vf, ft: Vf },
    /// Outer product finish: fd = ACC - fs x ft.
    Opmsub { dest: Dest, fd: Vf, fs: Vf, ft: Vf },
    /// ft = |fs| on dest lanes.
    Abs { dest: Dest, ft: Vf, fs: Vf },
    /// ft = float-to-fixed(fs) on dest lanes.
    Ftoi {
        fixed: FixedPoint,
        dest: Dest,
        ft: Vf,
        fs: Vf,
    },
    /// ft = fixed-to-float(fs) on dest lanes.
    Itof {
        fixed: FixedPoint,
        dest: Dest,
        ft: Vf,
        fs: Vf,
    },
    /// CLIPw.xyz: judge fs.xyz against |ft.w|, shifting the clip flag. The
    /// dest field is fixed by the hardware encoding (xyz); only fs/ft vary.
    Clip { fs: Vf, ft: Vf },
    /// Encoding the hardware does not define.
    Invalid { raw: u32 },
}

const FAMILY: [FmacOp; 7] = [
    FmacOp::Add,
    FmacOp::Sub,
    FmacOp::Madd,
    FmacOp::Msub,
    FmacOp::Max,
    FmacOp::Mini,
    FmacOp::Mul,
];

/// Decode the 32-bit upper word (flag bits are handled by the caller).
pub fn decode_upper(w: u32) -> UpperOp {
    let op6 = w & 0x3F;
    let dest = Dest::from_word(w);
    let fd = Vf(bit_range(w, 6, 10) as u8);
    let fs = Vf(bit_range(w, 11, 15) as u8);
    let ft = Vf(bit_range(w, 16, 20) as u8);

    match op6 {
        // Broadcast FMAC.
        0x00..=0x1B => {
            let op = FAMILY[(op6 >> 2) as usize];
            let bc = Comp::from_bits(op6);
            UpperOp::Fmac {
                op,
                dest,
                fd,
                fs,
                rhs: Rhs::Bc(ft, bc),
            }
        }
        // I/Q forms. The ft field is unused by the hardware here; a few
        // retail bundles carry nonzero ft bits, which stay in the raw word.
        0x1C => fmac(FmacOp::Mul, dest, fd, fs, Rhs::Q),
        0x1D => fmac(FmacOp::Max, dest, fd, fs, Rhs::I),
        0x1E => fmac(FmacOp::Mul, dest, fd, fs, Rhs::I),
        0x1F => fmac(FmacOp::Mini, dest, fd, fs, Rhs::I),
        0x20 => fmac(FmacOp::Add, dest, fd, fs, Rhs::Q),
        0x21 => fmac(FmacOp::Madd, dest, fd, fs, Rhs::Q),
        0x22 => fmac(FmacOp::Add, dest, fd, fs, Rhs::I),
        0x23 => fmac(FmacOp::Madd, dest, fd, fs, Rhs::I),
        0x24 => fmac(FmacOp::Sub, dest, fd, fs, Rhs::Q),
        0x25 => fmac(FmacOp::Msub, dest, fd, fs, Rhs::Q),
        0x26 => fmac(FmacOp::Sub, dest, fd, fs, Rhs::I),
        0x27 => fmac(FmacOp::Msub, dest, fd, fs, Rhs::I),
        // Plain three-register forms.
        0x28 => fmac(FmacOp::Add, dest, fd, fs, Rhs::Ft(ft)),
        0x29 => fmac(FmacOp::Madd, dest, fd, fs, Rhs::Ft(ft)),
        0x2A => fmac(FmacOp::Mul, dest, fd, fs, Rhs::Ft(ft)),
        0x2B => fmac(FmacOp::Max, dest, fd, fs, Rhs::Ft(ft)),
        0x2C => fmac(FmacOp::Sub, dest, fd, fs, Rhs::Ft(ft)),
        0x2D => fmac(FmacOp::Msub, dest, fd, fs, Rhs::Ft(ft)),
        0x2E => UpperOp::Opmsub { dest, fd, fs, ft },
        0x2F => fmac(FmacOp::Mini, dest, fd, fs, Rhs::Ft(ft)),
        // Second-level table: bc from bits 0-1, entry from the fd field.
        0x3C..=0x3F => decode_fd_table(w, op6 & 3, fd.0 as u32, dest, fs, ft),
        _ => UpperOp::Invalid { raw: w },
    }
}

fn fmac(op: FmacOp, dest: Dest, fd: Vf, fs: Vf, rhs: Rhs) -> UpperOp {
    UpperOp::Fmac {
        op,
        dest,
        fd,
        fs,
        rhs,
    }
}

fn fmac_a(op: FmacOp, dest: Dest, fs: Vf, rhs: Rhs) -> UpperOp {
    UpperOp::FmacA { op, dest, fs, rhs }
}

fn decode_fd_table(w: u32, bc: u32, sub: u32, dest: Dest, fs: Vf, ft: Vf) -> UpperOp {
    let bcast = Rhs::Bc(ft, Comp::from_bits(bc));
    match sub {
        0x00 => fmac_a(FmacOp::Add, dest, fs, bcast),
        0x01 => fmac_a(FmacOp::Sub, dest, fs, bcast),
        0x02 => fmac_a(FmacOp::Madd, dest, fs, bcast),
        0x03 => fmac_a(FmacOp::Msub, dest, fs, bcast),
        0x04 => UpperOp::Itof {
            fixed: FixedPoint::from_bc(bc),
            dest,
            ft,
            fs,
        },
        0x05 => UpperOp::Ftoi {
            fixed: FixedPoint::from_bc(bc),
            dest,
            ft,
            fs,
        },
        0x06 => fmac_a(FmacOp::Mul, dest, fs, bcast),
        0x07 => match bc {
            0 => fmac_a(FmacOp::Mul, dest, fs, Rhs::Q),
            1 => UpperOp::Abs { dest, ft, fs },
            2 => fmac_a(FmacOp::Mul, dest, fs, Rhs::I),
            _ => UpperOp::Clip { fs, ft },
        },
        0x08 => match bc {
            0 => fmac_a(FmacOp::Add, dest, fs, Rhs::Q),
            1 => fmac_a(FmacOp::Madd, dest, fs, Rhs::Q),
            2 => fmac_a(FmacOp::Add, dest, fs, Rhs::I),
            _ => fmac_a(FmacOp::Madd, dest, fs, Rhs::I),
        },
        0x09 => match bc {
            0 => fmac_a(FmacOp::Sub, dest, fs, Rhs::Q),
            1 => fmac_a(FmacOp::Msub, dest, fs, Rhs::Q),
            2 => fmac_a(FmacOp::Sub, dest, fs, Rhs::I),
            _ => fmac_a(FmacOp::Msub, dest, fs, Rhs::I),
        },
        0x0A => match bc {
            0 => fmac_a(FmacOp::Add, dest, fs, Rhs::Ft(ft)),
            1 => fmac_a(FmacOp::Madd, dest, fs, Rhs::Ft(ft)),
            2 => fmac_a(FmacOp::Mul, dest, fs, Rhs::Ft(ft)),
            _ => UpperOp::Invalid { raw: w },
        },
        0x0B => match bc {
            0 => fmac_a(FmacOp::Sub, dest, fs, Rhs::Ft(ft)),
            1 => fmac_a(FmacOp::Msub, dest, fs, Rhs::Ft(ft)),
            2 => UpperOp::Opmula { dest, fs, ft },
            _ => UpperOp::Nop,
        },
        _ => UpperOp::Invalid { raw: w },
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Expected words below were produced by the decomp repo's clean-room
    // assembler (../ico/tools/assemble_vu0.py, MIT) from hand-picked
    // operands; they are not game bytes.

    #[test]
    fn broadcast_fmac() {
        // addx.xyzw vf01, vf02, vf03
        assert_eq!(
            decode_upper(0x01E31040),
            UpperOp::Fmac {
                op: FmacOp::Add,
                dest: Dest(0b1111),
                fd: Vf(1),
                fs: Vf(2),
                rhs: Rhs::Bc(Vf(3), Comp::X),
            }
        );
        // mulw.xw vf31, vf15, vf07
        assert_eq!(
            decode_upper(0x01277FDB),
            UpperOp::Fmac {
                op: FmacOp::Mul,
                dest: Dest(0b1001),
                fd: Vf(31),
                fs: Vf(15),
                rhs: Rhs::Bc(Vf(7), Comp::W),
            }
        );
        // maddz.y vf12, vf13, vf14
        assert_eq!(
            decode_upper(0x008E6B0A),
            UpperOp::Fmac {
                op: FmacOp::Madd,
                dest: Dest(0b0100),
                fd: Vf(12),
                fs: Vf(13),
                rhs: Rhs::Bc(Vf(14), Comp::Z),
            }
        );
    }

    #[test]
    fn plain_fmac() {
        // add.xyzw vf01, vf02, vf03
        assert_eq!(
            decode_upper(0x01E31068),
            UpperOp::Fmac {
                op: FmacOp::Add,
                dest: Dest(0b1111),
                fd: Vf(1),
                fs: Vf(2),
                rhs: Rhs::Ft(Vf(3)),
            }
        );
        // sub.w vf09, vf10, vf11
        assert_eq!(
            decode_upper(0x002B526C),
            UpperOp::Fmac {
                op: FmacOp::Sub,
                dest: Dest(0b0001),
                fd: Vf(9),
                fs: Vf(10),
                rhs: Rhs::Ft(Vf(11)),
            }
        );
        // opmsub.xyz vf01, vf02, vf03
        assert_eq!(
            decode_upper(0x01C3106E),
            UpperOp::Opmsub {
                dest: Dest(0b1110),
                fd: Vf(1),
                fs: Vf(2),
                ft: Vf(3),
            }
        );
    }

    #[test]
    fn iq_fmac() {
        // addi.w vf04, vf05
        assert_eq!(
            decode_upper(0x00202922),
            UpperOp::Fmac {
                op: FmacOp::Add,
                dest: Dest(0b0001),
                fd: Vf(4),
                fs: Vf(5),
                rhs: Rhs::I,
            }
        );
        // mulq.xyz vf06, vf07
        assert_eq!(
            decode_upper(0x01C0399C),
            UpperOp::Fmac {
                op: FmacOp::Mul,
                dest: Dest(0b1110),
                fd: Vf(6),
                fs: Vf(7),
                rhs: Rhs::Q,
            }
        );
        // maxi.xyzw vf08, vf09
        assert_eq!(
            decode_upper(0x01E04A1D),
            UpperOp::Fmac {
                op: FmacOp::Max,
                dest: Dest(0b1111),
                fd: Vf(8),
                fs: Vf(9),
                rhs: Rhs::I,
            }
        );
        // minii.z vf10, vf11
        assert_eq!(
            decode_upper(0x00405A9F),
            UpperOp::Fmac {
                op: FmacOp::Mini,
                dest: Dest(0b0010),
                fd: Vf(10),
                fs: Vf(11),
                rhs: Rhs::I,
            }
        );
        // The ft field is dead in I/Q forms; nonzero ft bits must not change
        // the decode (a few retail bundles carry them).
        let with_ft = 0x00202922 | (0x1F << 16);
        assert_eq!(decode_upper(with_ft), decode_upper(0x00202922));
    }

    #[test]
    fn accumulator_forms() {
        // maddax.xyzw vf01, vf30
        assert_eq!(
            decode_upper(0x01FE08BC),
            UpperOp::FmacA {
                op: FmacOp::Madd,
                dest: Dest(0b1111),
                fs: Vf(1),
                rhs: Rhs::Bc(Vf(30), Comp::X),
            }
        );
        // msubaw.zw vf02, vf29
        assert_eq!(
            decode_upper(0x007D10FF),
            UpperOp::FmacA {
                op: FmacOp::Msub,
                dest: Dest(0b0011),
                fs: Vf(2),
                rhs: Rhs::Bc(Vf(29), Comp::W),
            }
        );
        // adda.xyzw vf03, vf04
        assert_eq!(
            decode_upper(0x01E41ABC),
            UpperOp::FmacA {
                op: FmacOp::Add,
                dest: Dest(0b1111),
                fs: Vf(3),
                rhs: Rhs::Ft(Vf(4)),
            }
        );
        // mulaq.xyzw vf05
        assert_eq!(
            decode_upper(0x01E029FC),
            UpperOp::FmacA {
                op: FmacOp::Mul,
                dest: Dest(0b1111),
                fs: Vf(5),
                rhs: Rhs::Q,
            }
        );
        // mulai.xy vf26
        assert_eq!(
            decode_upper(0x0180D1FE),
            UpperOp::FmacA {
                op: FmacOp::Mul,
                dest: Dest(0b1100),
                fs: Vf(26),
                rhs: Rhs::I,
            }
        );
        // maddai.xyzw vf07
        assert_eq!(
            decode_upper(0x01E03A3F),
            UpperOp::FmacA {
                op: FmacOp::Madd,
                dest: Dest(0b1111),
                fs: Vf(7),
                rhs: Rhs::I,
            }
        );
        // subaq.x vf08
        assert_eq!(
            decode_upper(0x0100427C),
            UpperOp::FmacA {
                op: FmacOp::Sub,
                dest: Dest(0b1000),
                fs: Vf(8),
                rhs: Rhs::Q,
            }
        );
        // opmula.xyz vf08, vf09
        assert_eq!(
            decode_upper(0x01C942FE),
            UpperOp::Opmula {
                dest: Dest(0b1110),
                fs: Vf(8),
                ft: Vf(9),
            }
        );
    }

    #[test]
    fn conversions_abs_clip() {
        // ftoi0.xyzw vf01, vf02 / ftoi4.xyzw vf10, vf11
        // ftoi12.x vf03, vf04 / ftoi15.w vf05, vf06
        for (w, fixed, dest, ft, fs) in [
            (0x01E1117Cu32, FixedPoint::F0, 0b1111u8, 1u8, 2u8),
            (0x01EA597D, FixedPoint::F4, 0b1111, 10, 11),
            (0x0103217E, FixedPoint::F12, 0b1000, 3, 4),
            (0x0025317F, FixedPoint::F15, 0b0001, 5, 6),
        ] {
            assert_eq!(
                decode_upper(w),
                UpperOp::Ftoi {
                    fixed,
                    dest: Dest(dest),
                    ft: Vf(ft),
                    fs: Vf(fs),
                }
            );
        }
        // itof0.xyzw vf07, vf08
        assert_eq!(
            decode_upper(0x01E7413C),
            UpperOp::Itof {
                fixed: FixedPoint::F0,
                dest: Dest(0b1111),
                ft: Vf(7),
                fs: Vf(8),
            }
        );
        // itof15.xy vf01, vf02
        assert_eq!(
            decode_upper(0x0181113F),
            UpperOp::Itof {
                fixed: FixedPoint::F15,
                dest: Dest(0b1100),
                ft: Vf(1),
                fs: Vf(2),
            }
        );
        // abs.xy vf12, vf13
        assert_eq!(
            decode_upper(0x018C69FD),
            UpperOp::Abs {
                dest: Dest(0b1100),
                ft: Vf(12),
                fs: Vf(13),
            }
        );
        // CLIPw.xyz vf05, vf06: op6 0x3F, fd-field entry 7, dest fixed xyz.
        let clip = (0b1110 << 21) | (6 << 16) | (5 << 11) | (7 << 6) | 0x3F;
        assert_eq!(
            decode_upper(clip),
            UpperOp::Clip {
                fs: Vf(5),
                ft: Vf(6),
            }
        );
    }

    #[test]
    fn nop_and_invalid() {
        assert_eq!(decode_upper(0x000002FF), UpperOp::Nop);
        // Undefined opcodes decode as Invalid, never panic.
        for op6 in 0x30..=0x3B {
            assert_eq!(decode_upper(op6), UpperOp::Invalid { raw: op6 });
        }
        // (bc=3, entry 0x0A) is a hole in the second-level table.
        let hole = (0x0A << 6) | 0x3F;
        assert_eq!(decode_upper(hole), UpperOp::Invalid { raw: hole });
    }
}
