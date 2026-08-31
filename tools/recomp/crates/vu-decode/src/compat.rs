//! Text rendering that mirrors the decomp repo's reference disassembler
//! (`../ico/tools/disasm_vu0.py`) token for token, so the ground-truth test
//! can diff our structured decode against that tool's output directly.
//!
//! Returns `None` wherever the reference tool prints a raw `.word` fallback
//! (or, for LOI lowers, decodes the immediate bits as if they were an
//! instruction); the test skips string comparison there and checks decode
//! validity by other means. This module exists for verification and debug
//! listings, not as the crate's primary output format.

use crate::lower::LowerOp;
use crate::upper::{Rhs, UpperOp};
use crate::{branch_target, Bundle, Comp, Dest, LowerSlot, Vf, Vi};

fn vf(r: Vf) -> String {
    format!("vf{:02}", r.0)
}

fn vi(r: Vi) -> String {
    format!("vi{:02}", r.0)
}

fn dest_suffix(d: Dest) -> String {
    if d.is_empty() {
        String::new()
    } else {
        format!(".{}", d.letters())
    }
}

fn label(offset: u32, imm11: i16) -> String {
    format!("L_{:04X}", branch_target(offset, imm11))
}

/// Render the upper word the way disasm_vu0.py does, or `None` where that
/// tool falls back to `.word`. The reference tool only prints symbolic text
/// when the flag bits 25-30 are all clear (the I bit alone does not stop it).
pub fn upper_compat(b: &Bundle) -> Option<String> {
    let w = b.upper_raw;
    if w & 0x7FFF_FFFF == 0 {
        return Some("nop".to_string());
    }
    if w == 0x000002FF {
        return Some("pad".to_string());
    }
    if w == 0x400002FF {
        return Some("epad".to_string());
    }
    if (w >> 25) & 0x3F != 0 {
        return None;
    }
    let d = |dest: Dest| dest_suffix(dest);
    match b.upper {
        UpperOp::Fmac {
            op,
            dest,
            fd,
            fs,
            rhs,
        } => match rhs {
            Rhs::Ft(ft) => Some(format!(
                "{}{} {}, {}, {}",
                op.name(),
                d(dest),
                vf(fd),
                vf(fs),
                vf(ft)
            )),
            Rhs::Bc(ft, c) => Some(format!(
                "{}{}{} {}, {}, {}",
                op.name(),
                c.letter(),
                d(dest),
                vf(fd),
                vf(fs),
                vf(ft)
            )),
            Rhs::I => Some(format!("{}i{} {}, {}", op.name(), d(dest), vf(fd), vf(fs))),
            Rhs::Q => Some(format!("{}q{} {}, {}", op.name(), d(dest), vf(fd), vf(fs))),
        },
        UpperOp::FmacA { op, dest, fs, rhs } => match rhs {
            Rhs::Ft(ft) => Some(format!("{}a{} {}, {}", op.name(), d(dest), vf(fs), vf(ft))),
            Rhs::Bc(ft, c) => Some(format!(
                "{}a{}{} {}, {}",
                op.name(),
                c.letter(),
                d(dest),
                vf(fs),
                vf(ft)
            )),
            Rhs::I => Some(format!("{}ai{} {}", op.name(), d(dest), vf(fs))),
            Rhs::Q => Some(format!("{}aq{} {}", op.name(), d(dest), vf(fs))),
        },
        UpperOp::Opmula { dest, fs, ft } => {
            Some(format!("opmula{} {}, {}", d(dest), vf(fs), vf(ft)))
        }
        UpperOp::Opmsub { dest, fd, fs, ft } => Some(format!(
            "opmsub{} {}, {}, {}",
            d(dest),
            vf(fd),
            vf(fs),
            vf(ft)
        )),
        UpperOp::Abs { dest, ft, fs } => Some(format!("abs{} {}, {}", d(dest), vf(ft), vf(fs))),
        UpperOp::Ftoi {
            fixed,
            dest,
            ft,
            fs,
        } => Some(format!(
            "ftoi{}{} {}, {}",
            fixed.suffix(),
            d(dest),
            vf(ft),
            vf(fs)
        )),
        UpperOp::Itof {
            fixed,
            dest,
            ft,
            fs,
        } => Some(format!(
            "itof{}{} {}, {}",
            fixed.suffix(),
            d(dest),
            vf(ft),
            vf(fs)
        )),
        // The reference tool has no CLIP decoder and prints .word for it.
        UpperOp::Clip { .. } => None,
        // Non-canonical NOP encodings and undefined ops print as .word.
        UpperOp::Nop | UpperOp::Invalid { .. } => None,
    }
}

fn comp(c: Comp) -> char {
    c.letter()
}

/// Render the lower word the way disasm_vu0.py does, or `None` where that
/// tool prints `.word` or where the slot is a LOI immediate (the reference
/// tool is unaware of the I bit and misdecodes those words).
pub fn lower_compat(b: &Bundle) -> Option<String> {
    let op = match b.lower {
        LowerSlot::Loi(_) => return None,
        LowerSlot::Inst(op) => op,
    };
    let w = b.lower_raw;
    if w == 0 {
        return Some("nop".to_string());
    }
    if w == 0x8000033C {
        return Some("nop_swap".to_string());
    }
    let d = dest_suffix;
    match op {
        LowerOp::Nop => Some("nop".to_string()),
        LowerOp::Lq {
            dest,
            ft,
            is,
            imm11,
        } => Some(format!("lq{} {}, {:+}({})", d(dest), vf(ft), imm11, vi(is))),
        LowerOp::Sq {
            dest,
            fs,
            it,
            imm11,
        } => Some(format!("sq{} {}, {:+}({})", d(dest), vf(fs), imm11, vi(it))),
        LowerOp::Ilw {
            dest,
            it,
            is,
            imm11,
        } => Some(format!(
            "ilw{} {}, {:+}({})",
            d(dest),
            vi(it),
            imm11,
            vi(is)
        )),
        LowerOp::Isw {
            dest,
            it,
            is,
            imm11,
        } => Some(format!(
            "isw{} {}, {:+}({})",
            d(dest),
            vi(it),
            imm11,
            vi(is)
        )),
        LowerOp::Iaddiu { it, is, imm15 } => {
            Some(format!("iaddiu {}, {}, {}", vi(it), vi(is), imm15))
        }
        LowerOp::Isubiu { it, is, imm15 } => {
            Some(format!("isubiu {}, {}, {}", vi(it), vi(is), imm15))
        }
        LowerOp::Fceq { imm24 } => Some(format!("fceq vi01, 0x{:X}", imm24)),
        LowerOp::Fcset { imm24 } => Some(format!("fcset 0x{:X}", imm24)),
        LowerOp::Fcand { imm24 } => Some(format!("fcand vi01, 0x{:X}", imm24)),
        LowerOp::Fcor { imm24 } => Some(format!("fcor vi01, 0x{:X}", imm24)),
        LowerOp::Fseq { it, imm12 } => Some(format!("fseq {}, 0x{:X}", vi(it), imm12)),
        LowerOp::Fsset { imm12 } => Some(format!("fsset 0x{:X}", imm12)),
        LowerOp::Fsand { it, imm12 } => Some(format!("fsand {}, 0x{:X}", vi(it), imm12)),
        LowerOp::Fsor { it, imm12 } => Some(format!("fsor {}, 0x{:X}", vi(it), imm12)),
        LowerOp::Fmeq { it, is } => Some(format!("fmeq {}, {}", vi(it), vi(is))),
        LowerOp::Fmand { it, is } => Some(format!("fmand {}, {}", vi(it), vi(is))),
        LowerOp::Fmor { it, is } => Some(format!("fmor {}, {}", vi(it), vi(is))),
        LowerOp::Fcget { it } => Some(format!("fcget {}", vi(it))),
        LowerOp::B { imm11 } => Some(format!("b {}", label(b.offset, imm11))),
        LowerOp::Bal { it, imm11 } => Some(format!("bal {}, {}", vi(it), label(b.offset, imm11))),
        LowerOp::Jr { is } => Some(format!("jr {}", vi(is))),
        LowerOp::Jalr { it, is } => Some(format!("jalr {}, {}", vi(it), vi(is))),
        LowerOp::Ibeq { it, is, imm11 } => Some(format!(
            "ibeq {}, {}, {}",
            vi(it),
            vi(is),
            label(b.offset, imm11)
        )),
        LowerOp::Ibne { it, is, imm11 } => Some(format!(
            "ibne {}, {}, {}",
            vi(it),
            vi(is),
            label(b.offset, imm11)
        )),
        LowerOp::Ibltz { is, imm11 } => {
            Some(format!("ibltz {}, {}", vi(is), label(b.offset, imm11)))
        }
        LowerOp::Ibgtz { is, imm11 } => {
            Some(format!("ibgtz {}, {}", vi(is), label(b.offset, imm11)))
        }
        LowerOp::Iblez { is, imm11 } => {
            Some(format!("iblez {}, {}", vi(is), label(b.offset, imm11)))
        }
        LowerOp::Ibgez { is, imm11 } => {
            Some(format!("ibgez {}, {}", vi(is), label(b.offset, imm11)))
        }
        LowerOp::Iadd { id, is, it } => Some(format!("iadd {}, {}, {}", vi(id), vi(is), vi(it))),
        LowerOp::Isub { id, is, it } => Some(format!("isub {}, {}, {}", vi(id), vi(is), vi(it))),
        LowerOp::Iand { id, is, it } => Some(format!("iand {}, {}, {}", vi(id), vi(is), vi(it))),
        LowerOp::Ior { id, is, it } => Some(format!("ior {}, {}, {}", vi(id), vi(is), vi(it))),
        LowerOp::Iaddi { it, is, imm5 } => Some(format!("iaddi {}, {}, {}", vi(it), vi(is), imm5)),
        LowerOp::Move { dest, ft, fs } => Some(format!("move{} {}, {}", d(dest), vf(ft), vf(fs))),
        LowerOp::Mr32 { dest, ft, fs } => Some(format!("mr32{} {}, {}", d(dest), vf(ft), vf(fs))),
        LowerOp::Lqi { dest, ft, is } => Some(format!("lqi{} {}, {}", d(dest), vf(ft), vi(is))),
        LowerOp::Lqd { dest, ft, is } => Some(format!("lqd{} {}, {}", d(dest), vf(ft), vi(is))),
        LowerOp::Sqi { dest, fs, it } => Some(format!("sqi{} {}, {}", d(dest), vf(fs), vi(it))),
        LowerOp::Sqd { dest, fs, it } => Some(format!("sqd{} {}, {}", d(dest), vf(fs), vi(it))),
        LowerOp::Ilwr { dest, it, is } => Some(format!("ilwr{} {}, {}", d(dest), vi(it), vi(is))),
        LowerOp::Iswr { dest, it, is } => Some(format!("iswr{} {}, {}", d(dest), vi(it), vi(is))),
        LowerOp::Mfir { dest, ft, is } => Some(format!("mfir{} {}, {}", d(dest), vf(ft), vi(is))),
        LowerOp::Mtir { it, fs, fsf } => Some(format!("mtir {}, {}.{}", vi(it), vf(fs), comp(fsf))),
        LowerOp::Mfp { dest, ft } => Some(format!("mfp{} {}", d(dest), vf(ft))),
        LowerOp::Div { fs, fsf, ft, ftf } => Some(format!(
            "div Q, {}.{}, {}.{}",
            vf(fs),
            comp(fsf),
            vf(ft),
            comp(ftf)
        )),
        LowerOp::Sqrt { ft, ftf } => Some(format!("sqrt Q, {}.{}", vf(ft), comp(ftf))),
        LowerOp::Rsqrt { fs, fsf, ft, ftf } => Some(format!(
            "rsqrt Q, {}.{}, {}.{}",
            vf(fs),
            comp(fsf),
            vf(ft),
            comp(ftf)
        )),
        LowerOp::Waitq => Some("waitq".to_string()),
        LowerOp::Rinit { fs, fsf } => Some(format!("rinit R, {}.{}", vf(fs), comp(fsf))),
        LowerOp::Rxor { fs, fsf } => Some(format!("rxor R, {}.{}", vf(fs), comp(fsf))),
        LowerOp::Rget { dest, ft } => Some(format!("rget{} {}", d(dest), vf(ft))),
        LowerOp::Rnext { dest, ft } => Some(format!("rnext{} {}", d(dest), vf(ft))),
        LowerOp::Xtop { it } => Some(format!("xtop {}", vi(it))),
        LowerOp::Xitop { it } => Some(format!("xitop {}", vi(it))),
        LowerOp::Xgkick { is } => Some(format!("xgkick {}", vi(is))),
        LowerOp::Esadd { fs } => Some(format!("esadd P, {}", vf(fs))),
        LowerOp::Ersadd { fs } => Some(format!("ersadd P, {}", vf(fs))),
        LowerOp::Eleng { fs } => Some(format!("eleng P, {}", vf(fs))),
        LowerOp::Erleng { fs } => Some(format!("erleng P, {}", vf(fs))),
        LowerOp::Eatanxy { fs } => Some(format!("eatanxy P, {}", vf(fs))),
        LowerOp::Eatanxz { fs } => Some(format!("eatanxz P, {}", vf(fs))),
        LowerOp::Esum { fs } => Some(format!("esum P, {}", vf(fs))),
        LowerOp::Esqrt { ft, ftf } => Some(format!("esqrt P, {}.{}", vf(ft), comp(ftf))),
        LowerOp::Ersqrt { ft, ftf } => Some(format!("ersqrt P, {}.{}", vf(ft), comp(ftf))),
        LowerOp::Ercpr { ft, ftf } => Some(format!("ercpr P, {}.{}", vf(ft), comp(ftf))),
        LowerOp::Esin { ft, ftf } => Some(format!("esin P, {}.{}", vf(ft), comp(ftf))),
        LowerOp::Eatan { ft, ftf } => Some(format!("eatan P, {}.{}", vf(ft), comp(ftf))),
        LowerOp::Eexp { ft, ftf } => Some(format!("eexp P, {}.{}", vf(ft), comp(ftf))),
        LowerOp::Waitp => Some("waitp".to_string()),
        LowerOp::Invalid { .. } => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bundle(upper: u32, lower: u32) -> Bundle {
        Bundle::decode(0x100, upper, lower)
    }

    #[test]
    fn upper_rendering() {
        // Words from the assemble_vu0.py pins (see upper.rs tests).
        let cases: [(u32, &str); 6] = [
            (0x01E31040, "addx.xyzw vf01, vf02, vf03"),
            (0x01E31068, "add.xyzw vf01, vf02, vf03"),
            (0x00202922, "addi.w vf04, vf05"),
            (0x01FE08BC, "maddax.xyzw vf01, vf30"),
            (0x01E029FC, "mulaq.xyzw vf05"),
            (0x0025317F, "ftoi15.w vf05, vf06"),
        ];
        for (w, want) in cases {
            assert_eq!(upper_compat(&bundle(w, 0)).as_deref(), Some(want));
        }
        assert_eq!(upper_compat(&bundle(0, 0)).as_deref(), Some("nop"));
        assert_eq!(upper_compat(&bundle(0x000002FF, 0)).as_deref(), Some("pad"));
        assert_eq!(
            upper_compat(&bundle(0x400002FF, 0)).as_deref(),
            Some("epad")
        );
        // Flagged upper: reference tool prints .word, so we return None.
        assert_eq!(upper_compat(&bundle(0x4A000100, 0)), None);
        // I bit alone does not suppress symbolic rendering.
        assert_eq!(
            upper_compat(&bundle(0x81E31068, 0)).as_deref(),
            Some("add.xyzw vf01, vf02, vf03")
        );
    }

    #[test]
    fn lower_rendering() {
        let cases: [(u32, &str); 7] = [
            (0x01E117FD, "lq.xyzw vf01, -3(vi02)"),
            (0x03C62005, "sq.xyz vf04, +5(vi06)"),
            (0x11E51FFF, "iaddiu vi05, vi03, 32767"),
            (0x80011432, "iaddi vi01, vi02, -16"),
            (0x81820BBC, "div Q, vf01.x, vf02.w"),
            (0x2C080002, "fsand vi08, 0x2"),
            (0x800076FC, "xgkick vi14"),
        ];
        for (w, want) in cases {
            assert_eq!(lower_compat(&bundle(0, w)).as_deref(), Some(want));
        }
        // Branch label uses pc + 8 + imm11 * 8 with the bundle's own offset.
        // b with imm11 = 0x16 at offset 0x100 lands at 0x1B8.
        assert_eq!(
            lower_compat(&bundle(0, 0x40000016)).as_deref(),
            Some("b L_01B8")
        );
        assert_eq!(lower_compat(&bundle(0, 0)).as_deref(), Some("nop"));
        assert_eq!(
            lower_compat(&bundle(0, 0x8000033C)).as_deref(),
            Some("nop_swap")
        );
        // LOI slot renders as None (reference tool misdecodes it).
        assert_eq!(lower_compat(&bundle(0x800002FF, 0x3F800000)), None);
    }
}
