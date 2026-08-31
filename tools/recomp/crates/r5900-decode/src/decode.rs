//! The total R5900 decoder.
//!
//! Layout references: EE Core Instruction Set manual (opcode tables for
//! SPECIAL/REGIMM/MMI/COP0/COP1/COP2). Unused fields that the hardware
//! documents as zero are enforced; a nonzero value there decodes to
//! `Kind::Invalid`, which matches how the baseline disassembler treats
//! such words.

use crate::insn::{Insn, Kind, Operand};
use Operand as O;

const COMPS: [char; 4] = ['x', 'y', 'z', 'w'];

#[inline]
fn rs(w: u32) -> u8 {
    ((w >> 21) & 0x1F) as u8
}
#[inline]
fn rt(w: u32) -> u8 {
    ((w >> 16) & 0x1F) as u8
}
#[inline]
fn rd(w: u32) -> u8 {
    ((w >> 11) & 0x1F) as u8
}
#[inline]
fn sa(w: u32) -> u8 {
    ((w >> 6) & 0x1F) as u8
}
#[inline]
fn funct(w: u32) -> u32 {
    w & 0x3F
}
#[inline]
fn imm16(w: u32) -> i32 {
    (w as u16) as i16 as i32
}
#[inline]
fn uimm16(w: u32) -> u32 {
    w & 0xFFFF
}

/// Destination mask suffix from bits 24..21 (x=8, y=4, z=2, w=1).
fn dest_mask(w: u32) -> String {
    let d = (w >> 21) & 0xF;
    let mut s = String::new();
    for (i, c) in COMPS.iter().enumerate() {
        if d & (8 >> i) != 0 {
            s.push(*c);
        }
    }
    s
}

fn branch_target(w: u32, vram: u32) -> u32 {
    vram.wrapping_add(4)
        .wrapping_add((imm16(w) as u32) << 2)
}

fn jump_target(w: u32, vram: u32) -> u32 {
    (vram.wrapping_add(4) & 0xF000_0000) | ((w & 0x03FF_FFFF) << 2)
}

fn op(m: impl Into<String>, operands: Vec<Operand>) -> Kind {
    Kind::Op {
        mnemonic: m.into(),
        operands,
    }
}

fn mem(w: u32) -> Operand {
    O::Mem {
        offset: imm16(w),
        base: rs(w),
    }
}

pub fn decode(word: u32, vram: u32) -> Insn {
    Insn {
        word,
        vram,
        kind: decode_kind(word, vram),
    }
}

fn decode_kind(w: u32, vram: u32) -> Kind {
    let opc = w >> 26;
    match opc {
        0x00 => special(w),
        0x01 => regimm(w, vram),
        0x02 => op("j", vec![O::Target(jump_target(w, vram))]),
        0x03 => op("jal", vec![O::Target(jump_target(w, vram))]),
        0x04 => {
            let t = O::Target(branch_target(w, vram));
            if rs(w) == 0 && rt(w) == 0 {
                op("b", vec![t])
            } else if rt(w) == 0 {
                op("beqz", vec![O::Gpr(rs(w)), t])
            } else {
                op("beq", vec![O::Gpr(rs(w)), O::Gpr(rt(w)), t])
            }
        }
        0x05 => {
            let t = O::Target(branch_target(w, vram));
            if rt(w) == 0 {
                op("bnez", vec![O::Gpr(rs(w)), t])
            } else {
                op("bne", vec![O::Gpr(rs(w)), O::Gpr(rt(w)), t])
            }
        }
        0x06 | 0x07 | 0x16 | 0x17 => {
            if rt(w) != 0 {
                return Kind::Invalid;
            }
            let m = match opc {
                0x06 => "blez",
                0x07 => "bgtz",
                0x16 => "blezl",
                _ => "bgtzl",
            };
            op(m, vec![O::Gpr(rs(w)), O::Target(branch_target(w, vram))])
        }
        0x08 => op("addi", vec![O::Gpr(rt(w)), O::Gpr(rs(w)), O::Imm(imm16(w))]),
        0x09 => op("addiu", vec![O::Gpr(rt(w)), O::Gpr(rs(w)), O::Imm(imm16(w))]),
        0x0A => op("slti", vec![O::Gpr(rt(w)), O::Gpr(rs(w)), O::Imm(imm16(w))]),
        0x0B => op("sltiu", vec![O::Gpr(rt(w)), O::Gpr(rs(w)), O::Imm(imm16(w))]),
        0x0C => op("andi", vec![O::Gpr(rt(w)), O::Gpr(rs(w)), O::UImm(uimm16(w))]),
        0x0D => op("ori", vec![O::Gpr(rt(w)), O::Gpr(rs(w)), O::UImm(uimm16(w))]),
        0x0E => op("xori", vec![O::Gpr(rt(w)), O::Gpr(rs(w)), O::UImm(uimm16(w))]),
        0x0F => {
            if rs(w) != 0 {
                return Kind::Invalid;
            }
            op("lui", vec![O::Gpr(rt(w)), O::UImm(uimm16(w))])
        }
        0x10 => cop0(w, vram),
        0x11 => cop1(w, vram),
        0x12 => cop2(w, vram),
        0x14 => op(
            "beql",
            vec![O::Gpr(rs(w)), O::Gpr(rt(w)), O::Target(branch_target(w, vram))],
        ),
        0x15 => op(
            "bnel",
            vec![O::Gpr(rs(w)), O::Gpr(rt(w)), O::Target(branch_target(w, vram))],
        ),
        0x18 => op("daddi", vec![O::Gpr(rt(w)), O::Gpr(rs(w)), O::Imm(imm16(w))]),
        0x19 => op("daddiu", vec![O::Gpr(rt(w)), O::Gpr(rs(w)), O::Imm(imm16(w))]),
        0x1A => op("ldl", vec![O::Gpr(rt(w)), mem(w)]),
        0x1B => op("ldr", vec![O::Gpr(rt(w)), mem(w)]),
        0x1C => mmi(w),
        0x1E => op("lq", vec![O::Gpr(rt(w)), mem(w)]),
        0x1F => op("sq", vec![O::Gpr(rt(w)), mem(w)]),
        0x20 => op("lb", vec![O::Gpr(rt(w)), mem(w)]),
        0x21 => op("lh", vec![O::Gpr(rt(w)), mem(w)]),
        0x22 => op("lwl", vec![O::Gpr(rt(w)), mem(w)]),
        0x23 => op("lw", vec![O::Gpr(rt(w)), mem(w)]),
        0x24 => op("lbu", vec![O::Gpr(rt(w)), mem(w)]),
        0x25 => op("lhu", vec![O::Gpr(rt(w)), mem(w)]),
        0x26 => op("lwr", vec![O::Gpr(rt(w)), mem(w)]),
        0x27 => op("lwu", vec![O::Gpr(rt(w)), mem(w)]),
        0x28 => op("sb", vec![O::Gpr(rt(w)), mem(w)]),
        0x29 => op("sh", vec![O::Gpr(rt(w)), mem(w)]),
        0x2A => op("swl", vec![O::Gpr(rt(w)), mem(w)]),
        0x2B => op("sw", vec![O::Gpr(rt(w)), mem(w)]),
        0x2C => op("sdl", vec![O::Gpr(rt(w)), mem(w)]),
        0x2D => op("sdr", vec![O::Gpr(rt(w)), mem(w)]),
        0x2E => op("swr", vec![O::Gpr(rt(w)), mem(w)]),
        0x2F => op("cache", vec![O::Hex2(rt(w)), mem(w)]),
        0x31 => op("lwc1", vec![O::Fpr(rt(w)), mem(w)]),
        0x33 => op("pref", vec![O::Hex2(rt(w)), mem(w)]),
        0x36 => op("lqc2", vec![O::Vf(rt(w)), mem(w)]),
        0x37 => op("ld", vec![O::Gpr(rt(w)), mem(w)]),
        0x39 => op("swc1", vec![O::Fpr(rt(w)), mem(w)]),
        0x3E => op("sqc2", vec![O::Vf(rt(w)), mem(w)]),
        0x3F => op("sd", vec![O::Gpr(rt(w)), mem(w)]),
        _ => Kind::Invalid,
    }
}

fn special(w: u32) -> Kind {
    let f = funct(w);
    match f {
        // Shift-immediate group: rs must be zero.
        0x00 | 0x02 | 0x03 | 0x38 | 0x3A | 0x3B | 0x3C | 0x3E | 0x3F => {
            if rs(w) != 0 {
                return Kind::Invalid;
            }
            if w == 0 {
                return op("nop", vec![]);
            }
            let m = match f {
                0x00 => "sll",
                0x02 => "srl",
                0x03 => "sra",
                0x38 => "dsll",
                0x3A => "dsrl",
                0x3B => "dsra",
                0x3C => "dsll32",
                0x3E => "dsrl32",
                _ => "dsra32",
            };
            op(m, vec![O::Gpr(rd(w)), O::Gpr(rt(w)), O::Dec(sa(w) as u32)])
        }
        // Shift-variable group: sa must be zero. Operands: rd, rt, rs.
        0x04 | 0x06 | 0x07 | 0x14 | 0x16 | 0x17 => {
            if sa(w) != 0 {
                return Kind::Invalid;
            }
            let m = match f {
                0x04 => "sllv",
                0x06 => "srlv",
                0x07 => "srav",
                0x14 => "dsllv",
                0x16 => "dsrlv",
                _ => "dsrav",
            };
            op(m, vec![O::Gpr(rd(w)), O::Gpr(rt(w)), O::Gpr(rs(w))])
        }
        0x08 => {
            if rt(w) != 0 || rd(w) != 0 || sa(w) != 0 {
                return Kind::Invalid;
            }
            op("jr", vec![O::Gpr(rs(w))])
        }
        0x09 => {
            if rt(w) != 0 || sa(w) != 0 {
                return Kind::Invalid;
            }
            if rd(w) == 31 {
                op("jalr", vec![O::Gpr(rs(w))])
            } else {
                op("jalr", vec![O::Gpr(rd(w)), O::Gpr(rs(w))])
            }
        }
        0x0A | 0x0B => {
            if sa(w) != 0 {
                return Kind::Invalid;
            }
            let m = if f == 0x0A { "movz" } else { "movn" };
            op(m, vec![O::Gpr(rd(w)), O::Gpr(rs(w)), O::Gpr(rt(w))])
        }
        0x0C => op("syscall", vec![O::Dec((w >> 6) & 0xFFFFF)]),
        0x0D => {
            let c1 = (w >> 16) & 0x3FF;
            let c2 = (w >> 6) & 0x3FF;
            if c2 != 0 {
                op("break", vec![O::Dec(c1), O::Dec(c2)])
            } else {
                op("break", vec![O::Dec(c1)])
            }
        }
        0x0F => {
            if rs(w) != 0 || rt(w) != 0 || rd(w) != 0 {
                return Kind::Invalid;
            }
            if sa(w) == 0x10 {
                op("sync.p", vec![])
            } else {
                op("sync", vec![])
            }
        }
        0x10 | 0x12 => {
            if rs(w) != 0 || rt(w) != 0 || sa(w) != 0 {
                return Kind::Invalid;
            }
            op(if f == 0x10 { "mfhi" } else { "mflo" }, vec![O::Gpr(rd(w))])
        }
        0x11 | 0x13 => {
            if rt(w) != 0 || rd(w) != 0 || sa(w) != 0 {
                return Kind::Invalid;
            }
            op(if f == 0x11 { "mthi" } else { "mtlo" }, vec![O::Gpr(rs(w))])
        }
        0x18 => {
            if sa(w) != 0 {
                return Kind::Invalid;
            }
            op("mult", vec![O::Gpr(rd(w)), O::Gpr(rs(w)), O::Gpr(rt(w))])
        }
        0x19 => {
            // The baseline disassembler prints multu without a destination
            // and rejects encodings with a nonzero rd field.
            if sa(w) != 0 || rd(w) != 0 {
                return Kind::Invalid;
            }
            op("multu", vec![O::Gpr(rs(w)), O::Gpr(rt(w))])
        }
        0x1A | 0x1B => {
            if sa(w) != 0 || rd(w) != 0 {
                return Kind::Invalid;
            }
            let m = if f == 0x1A { "div" } else { "divu" };
            op(m, vec![O::Gpr(0), O::Gpr(rs(w)), O::Gpr(rt(w))])
        }
        0x20..=0x27 | 0x2A..=0x2F => {
            if sa(w) != 0 {
                return Kind::Invalid;
            }
            let m = match f {
                0x20 => "add",
                0x21 => "addu",
                0x22 => "sub",
                0x23 => "subu",
                0x24 => "and",
                0x25 => "or",
                0x26 => "xor",
                0x27 => "nor",
                0x2A => "slt",
                0x2B => "sltu",
                0x2C => "dadd",
                0x2D => "daddu",
                0x2E => "dsub",
                _ => "dsubu",
            };
            // neg/negu pseudos (sub/subu with rs == 0), matching the baseline.
            if rs(w) == 0 && (f == 0x22 || f == 0x23) {
                let m = if f == 0x22 { "neg" } else { "negu" };
                return op(m, vec![O::Gpr(rd(w)), O::Gpr(rt(w))]);
            }
            op(m, vec![O::Gpr(rd(w)), O::Gpr(rs(w)), O::Gpr(rt(w))])
        }
        0x28 => {
            if rs(w) != 0 || rt(w) != 0 || sa(w) != 0 {
                return Kind::Invalid;
            }
            op("mfsa", vec![O::Gpr(rd(w))])
        }
        0x29 => {
            if rt(w) != 0 || rd(w) != 0 || sa(w) != 0 {
                return Kind::Invalid;
            }
            op("mtsa", vec![O::Gpr(rs(w))])
        }
        0x30..=0x34 | 0x36 => {
            let m = match f {
                0x30 => "tge",
                0x31 => "tgeu",
                0x32 => "tlt",
                0x33 => "tltu",
                0x34 => "teq",
                _ => "tne",
            };
            let code = (w >> 6) & 0x3FF;
            op(m, vec![O::Gpr(rs(w)), O::Gpr(rt(w)), O::Dec(code)])
        }
        _ => Kind::Invalid,
    }
}

fn regimm(w: u32, vram: u32) -> Kind {
    let sub = rt(w);
    match sub {
        0x00..=0x03 | 0x10..=0x13 => {
            let t = O::Target(branch_target(w, vram));
            if sub == 0x11 && rs(w) == 0 {
                return op("bal", vec![t]);
            }
            let m = match sub {
                0x00 => "bltz",
                0x01 => "bgez",
                0x02 => "bltzl",
                0x03 => "bgezl",
                0x10 => "bltzal",
                0x11 => "bgezal",
                0x12 => "bltzall",
                _ => "bgezall",
            };
            op(m, vec![O::Gpr(rs(w)), t])
        }
        0x08..=0x0C | 0x0E => {
            let m = match sub {
                0x08 => "tgei",
                0x09 => "tgeiu",
                0x0A => "tlti",
                0x0B => "tltiu",
                0x0C => "teqi",
                _ => "tnei",
            };
            op(m, vec![O::Gpr(rs(w)), O::Imm(imm16(w))])
        }
        0x18 => op("mtsab", vec![O::Gpr(rs(w)), O::UImm(uimm16(w))]),
        0x19 => op("mtsah", vec![O::Gpr(rs(w)), O::UImm(uimm16(w))]),
        _ => Kind::Invalid,
    }
}

fn mmi(w: u32) -> Kind {
    let f = funct(w);
    match f {
        0x00 | 0x01 | 0x20 | 0x21 => {
            if sa(w) != 0 {
                return Kind::Invalid;
            }
            let m = match f {
                0x00 => "madd",
                0x01 => "maddu",
                0x20 => "madd1",
                _ => "maddu1",
            };
            op(m, vec![O::Gpr(rd(w)), O::Gpr(rs(w)), O::Gpr(rt(w))])
        }
        0x04 => {
            if rt(w) != 0 || sa(w) != 0 {
                return Kind::Invalid;
            }
            op("plzcw", vec![O::Gpr(rd(w)), O::Gpr(rs(w))])
        }
        0x08 => mmi0(w),
        0x09 => mmi2(w),
        0x10 | 0x12 => {
            if rs(w) != 0 || rt(w) != 0 || sa(w) != 0 {
                return Kind::Invalid;
            }
            op(if f == 0x10 { "mfhi1" } else { "mflo1" }, vec![O::Gpr(rd(w))])
        }
        0x11 | 0x13 => {
            if rt(w) != 0 || rd(w) != 0 || sa(w) != 0 {
                return Kind::Invalid;
            }
            op(if f == 0x11 { "mthi1" } else { "mtlo1" }, vec![O::Gpr(rs(w))])
        }
        0x18 => {
            if sa(w) != 0 {
                return Kind::Invalid;
            }
            op("mult1", vec![O::Gpr(rd(w)), O::Gpr(rs(w)), O::Gpr(rt(w))])
        }
        0x19 => {
            if sa(w) != 0 || rd(w) != 0 {
                return Kind::Invalid;
            }
            op("multu1", vec![O::Gpr(rs(w)), O::Gpr(rt(w))])
        }
        0x1A | 0x1B => {
            if sa(w) != 0 || rd(w) != 0 {
                return Kind::Invalid;
            }
            let m = if f == 0x1A { "div1" } else { "divu1" };
            op(m, vec![O::Gpr(0), O::Gpr(rs(w)), O::Gpr(rt(w))])
        }
        0x28 => mmi1(w),
        0x29 => mmi3(w),
        0x30 => {
            if rs(w) != 0 || rt(w) != 0 {
                return Kind::Invalid;
            }
            let m = match sa(w) {
                0x00 => "pmfhl.lw",
                0x01 => "pmfhl.uw",
                0x02 => "pmfhl.slw",
                0x03 => "pmfhl.lh",
                0x04 => "pmfhl.sh",
                _ => return Kind::Invalid,
            };
            op(m, vec![O::Gpr(rd(w))])
        }
        0x31 => {
            if rt(w) != 0 || rd(w) != 0 || sa(w) != 0 {
                return Kind::Invalid;
            }
            op("pmthl.lw", vec![O::Gpr(rs(w))])
        }
        0x34 | 0x36 | 0x37 | 0x3C | 0x3E | 0x3F => {
            if rs(w) != 0 {
                return Kind::Invalid;
            }
            let m = match f {
                0x34 => "psllh",
                0x36 => "psrlh",
                0x37 => "psrah",
                0x3C => "psllw",
                0x3E => "psrlw",
                _ => "psraw",
            };
            op(m, vec![O::Gpr(rd(w)), O::Gpr(rt(w)), O::Dec(sa(w) as u32)])
        }
        _ => Kind::Invalid,
    }
}

/// Three-operand parallel op: rd, rs, rt.
fn p3(w: u32, m: &str) -> Kind {
    op(m, vec![O::Gpr(rd(w)), O::Gpr(rs(w)), O::Gpr(rt(w))])
}

/// Two-operand parallel op with unused rs (must be zero): rd, rt.
fn p2(w: u32, m: &str) -> Kind {
    if rs(w) != 0 {
        return Kind::Invalid;
    }
    op(m, vec![O::Gpr(rd(w)), O::Gpr(rt(w))])
}

/// Parallel variable shift: rd, rt, rs.
fn pshiftv(w: u32, m: &str) -> Kind {
    op(m, vec![O::Gpr(rd(w)), O::Gpr(rt(w)), O::Gpr(rs(w))])
}

/// Parallel divide: rs, rt with rd forced zero.
fn pdiv(w: u32, m: &str) -> Kind {
    if rd(w) != 0 {
        return Kind::Invalid;
    }
    op(m, vec![O::Gpr(rs(w)), O::Gpr(rt(w))])
}

fn mmi0(w: u32) -> Kind {
    match sa(w) {
        0x00 => p3(w, "paddw"),
        0x01 => p3(w, "psubw"),
        0x02 => p3(w, "pcgtw"),
        0x03 => p3(w, "pmaxw"),
        0x04 => p3(w, "paddh"),
        0x05 => p3(w, "psubh"),
        0x06 => p3(w, "pcgth"),
        0x07 => p3(w, "pmaxh"),
        0x08 => p3(w, "paddb"),
        0x09 => p3(w, "psubb"),
        0x0A => p3(w, "pcgtb"),
        0x10 => p3(w, "paddsw"),
        0x11 => p3(w, "psubsw"),
        0x12 => p3(w, "pextlw"),
        0x13 => p3(w, "ppacw"),
        0x14 => p3(w, "paddsh"),
        0x15 => p3(w, "psubsh"),
        0x16 => p3(w, "pextlh"),
        0x17 => p3(w, "ppach"),
        0x18 => p3(w, "paddsb"),
        0x19 => p3(w, "psubsb"),
        0x1A => p3(w, "pextlb"),
        0x1B => p3(w, "ppacb"),
        0x1E => p2(w, "pext5"),
        0x1F => p2(w, "ppac5"),
        _ => Kind::Invalid,
    }
}

fn mmi1(w: u32) -> Kind {
    match sa(w) {
        0x01 => p2(w, "pabsw"),
        0x02 => p3(w, "pceqw"),
        0x03 => p3(w, "pminw"),
        0x04 => p3(w, "padsbh"),
        0x05 => p2(w, "pabsh"),
        0x06 => p3(w, "pceqh"),
        0x07 => p3(w, "pminh"),
        0x0A => p3(w, "pceqb"),
        0x10 => p3(w, "padduw"),
        0x11 => p3(w, "psubuw"),
        0x12 => p3(w, "pextuw"),
        0x14 => p3(w, "padduh"),
        0x15 => p3(w, "psubuh"),
        0x16 => p3(w, "pextuh"),
        0x18 => p3(w, "paddub"),
        0x19 => p3(w, "psubub"),
        0x1A => p3(w, "pextub"),
        0x1B => p3(w, "qfsrv"),
        _ => Kind::Invalid,
    }
}

fn mmi2(w: u32) -> Kind {
    match sa(w) {
        0x00 => p3(w, "pmaddw"),
        0x02 => pshiftv(w, "psllvw"),
        0x03 => pshiftv(w, "psrlvw"),
        0x04 => p3(w, "pmsubw"),
        0x08 | 0x09 => {
            if rs(w) != 0 || rt(w) != 0 {
                return Kind::Invalid;
            }
            op(
                if sa(w) == 0x08 { "pmfhi" } else { "pmflo" },
                vec![O::Gpr(rd(w))],
            )
        }
        0x0A => p3(w, "pinth"),
        0x0C => p3(w, "pmultw"),
        0x0D => pdiv(w, "pdivw"),
        0x0E => p3(w, "pcpyld"),
        0x10 => p3(w, "pmaddh"),
        0x11 => p3(w, "phmadh"),
        0x12 => p3(w, "pand"),
        0x13 => p3(w, "pxor"),
        0x14 => p3(w, "pmsubh"),
        0x15 => p3(w, "phmsbh"),
        0x1A => p2(w, "pexeh"),
        0x1B => p2(w, "prevh"),
        0x1C => p3(w, "pmulth"),
        0x1D => pdiv(w, "pdivbw"),
        0x1E => p2(w, "pexew"),
        0x1F => p2(w, "prot3w"),
        _ => Kind::Invalid,
    }
}

fn mmi3(w: u32) -> Kind {
    match sa(w) {
        0x00 => p3(w, "pmadduw"),
        0x03 => pshiftv(w, "psravw"),
        0x08 | 0x09 => {
            if rt(w) != 0 || rd(w) != 0 {
                return Kind::Invalid;
            }
            op(
                if sa(w) == 0x08 { "pmthi" } else { "pmtlo" },
                vec![O::Gpr(rs(w))],
            )
        }
        0x0A => p3(w, "pinteh"),
        0x0C => p3(w, "pmultuw"),
        0x0D => pdiv(w, "pdivuw"),
        0x0E => p3(w, "pcpyud"),
        0x12 => p3(w, "por"),
        0x13 => p3(w, "pnor"),
        0x1A => p2(w, "pexch"),
        0x1B => p2(w, "pcpyh"),
        0x1E => p2(w, "pexcw"),
        _ => Kind::Invalid,
    }
}

fn cop0(w: u32, vram: u32) -> Kind {
    let sub = rs(w);
    match sub {
        0x00 | 0x04 => {
            if w & 0x7FF != 0 {
                return Kind::Invalid;
            }
            let m = if sub == 0x00 { "mfc0" } else { "mtc0" };
            op(m, vec![O::Gpr(rt(w)), O::Cop0(rd(w))])
        }
        0x08 => {
            let m = match rt(w) {
                0x00 => "bc0f",
                0x01 => "bc0t",
                0x02 => "bc0fl",
                0x03 => "bc0tl",
                _ => return Kind::Invalid,
            };
            op(m, vec![O::Target(branch_target(w, vram))])
        }
        0x10..=0x1F => {
            // CO=1 function encodings: all register fields must be zero.
            if w & 0x01FF_FFC0 != 0 {
                return Kind::Invalid;
            }
            let m = match funct(w) {
                0x01 => "tlbr",
                0x02 => "tlbwi",
                0x06 => "tlbwr",
                0x08 => "tlbp",
                0x18 => "eret",
                0x38 => "ei",
                0x39 => "di",
                _ => return Kind::Invalid,
            };
            op(m, vec![])
        }
        _ => Kind::Invalid,
    }
}

fn cop1(w: u32, vram: u32) -> Kind {
    let sub = rs(w);
    match sub {
        0x00 | 0x02 | 0x04 | 0x06 => {
            if w & 0x7FF != 0 {
                return Kind::Invalid;
            }
            match sub {
                0x00 => op("mfc1", vec![O::Gpr(rt(w)), O::Fpr(rd(w))]),
                0x02 => op("cfc1", vec![O::Gpr(rt(w)), O::FpCtl(rd(w))]),
                0x04 => op("mtc1", vec![O::Gpr(rt(w)), O::Fpr(rd(w))]),
                _ => op("ctc1", vec![O::Gpr(rt(w)), O::FpCtl(rd(w))]),
            }
        }
        0x08 => {
            let m = match rt(w) {
                0x00 => "bc1f",
                0x01 => "bc1t",
                0x02 => "bc1fl",
                0x03 => "bc1tl",
                _ => return Kind::Invalid,
            };
            op(m, vec![O::Target(branch_target(w, vram))])
        }
        0x10 => cop1_s(w),
        0x14 => {
            // fmt = W: only cvt.s.w on the EE.
            if funct(w) != 0x20 || rt(w) != 0 {
                return Kind::Invalid;
            }
            op("cvt.s.w", vec![O::Fpr(sa(w)), O::Fpr(rd(w))])
        }
        _ => Kind::Invalid,
    }
}

fn cop1_s(w: u32) -> Kind {
    // fmt = S encodings: ft = rt field, fs = rd field, fd = sa field.
    let ft = rt(w);
    let fs = rd(w);
    let fd = sa(w);
    let f = funct(w);
    match f {
        0x00 | 0x01 | 0x02 | 0x03 | 0x16 | 0x1C | 0x1D | 0x28 | 0x29 => {
            let m = match f {
                0x00 => "add.s",
                0x01 => "sub.s",
                0x02 => "mul.s",
                0x03 => "div.s",
                0x16 => "rsqrt.s",
                0x1C => "madd.s",
                0x1D => "msub.s",
                0x28 => "max.s",
                _ => "min.s",
            };
            op(m, vec![O::Fpr(fd), O::Fpr(fs), O::Fpr(ft)])
        }
        0x04 => {
            if fs != 0 {
                return Kind::Invalid;
            }
            op("sqrt.s", vec![O::Fpr(fd), O::Fpr(ft)])
        }
        0x05 | 0x06 | 0x07 | 0x24 => {
            if ft != 0 {
                return Kind::Invalid;
            }
            let m = match f {
                0x05 => "abs.s",
                0x06 => "mov.s",
                0x07 => "neg.s",
                _ => "cvt.w.s",
            };
            op(m, vec![O::Fpr(fd), O::Fpr(fs)])
        }
        0x18 | 0x19 | 0x1A | 0x1E | 0x1F => {
            if fd != 0 {
                return Kind::Invalid;
            }
            let m = match f {
                0x18 => "adda.s",
                0x19 => "suba.s",
                0x1A => "mula.s",
                0x1E => "madda.s",
                _ => "msuba.s",
            };
            op(m, vec![O::Fpr(fs), O::Fpr(ft)])
        }
        0x30 | 0x32 | 0x34 | 0x36 => {
            if fd != 0 {
                return Kind::Invalid;
            }
            let m = match f {
                0x30 => "c.f.s",
                0x32 => "c.eq.s",
                0x34 => "c.lt.s",
                _ => "c.le.s",
            };
            op(m, vec![O::Fpr(fs), O::Fpr(ft)])
        }
        _ => Kind::Invalid,
    }
}

// ------------------------------------------------------------------ COP2

fn vsuffix(mnem: &str, w: u32) -> String {
    let d = dest_mask(w);
    if d.is_empty() {
        mnem.to_string()
    } else {
        format!("{mnem}.{d}")
    }
}

fn fsf(w: u32) -> char {
    COMPS[((w >> 21) & 3) as usize]
}
fn ftf(w: u32) -> char {
    COMPS[((w >> 23) & 3) as usize]
}

fn cop2(w: u32, vram: u32) -> Kind {
    let sub = rs(w);
    if sub & 0x10 != 0 {
        return cop2_special(w);
    }
    match sub {
        0x01 | 0x02 | 0x05 | 0x06 => {
            // Transfer ops. Bit 0 is the interlock bit; bits 10..1 are zero.
            if w & 0x7FE != 0 {
                return Kind::Invalid;
            }
            let il = if w & 1 != 0 { ".i" } else { ".ni" };
            match sub {
                0x01 => op(format!("qmfc2{il}"), vec![O::Gpr(rt(w)), O::Vf(rd(w))]),
                0x02 => op(format!("cfc2{il}"), vec![O::Gpr(rt(w)), O::Vi(rd(w))]),
                0x05 => op(format!("qmtc2{il}"), vec![O::Gpr(rt(w)), O::Vf(rd(w))]),
                _ => op(format!("ctc2{il}"), vec![O::Gpr(rt(w)), O::Vi(rd(w))]),
            }
        }
        0x08 => {
            let m = match rt(w) {
                0x00 => "bc2f",
                0x01 => "bc2t",
                0x02 => "bc2fl",
                0x03 => "bc2tl",
                _ => return Kind::Invalid,
            };
            op(m, vec![O::Target(branch_target(w, vram))])
        }
        _ => Kind::Invalid,
    }
}

/// COP2 special ops. Field layout (cross-checked against the decomp repo's
/// disasm_vu0.py, whose VU upper-instruction layout is identical):
/// funct = bits 5..0, fd/sub = bits 10..6, fs = bits 15..11,
/// ft = bits 20..16, dest = bits 24..21, fsf = bits 22..21,
/// ftf = bits 24..23.
fn cop2_special(w: u32) -> Kind {
    let f = funct(w);
    let vft = rt(w);
    let vfs = rd(w);
    let vfd = sa(w);
    match f {
        // Broadcast FMAC families: funct = family*4 + bc.
        0x00..=0x1B => {
            let family = match f >> 2 {
                0 => "vadd",
                1 => "vsub",
                2 => "vmadd",
                3 => "vmsub",
                4 => "vmax",
                5 => "vmini",
                _ => "vmul",
            };
            let bc = COMPS[(f & 3) as usize];
            op(
                vsuffix(&format!("{family}{bc}"), w),
                vec![O::Vf(vfd), O::Vf(vfs), O::VfComp(vft, bc)],
            )
        }
        // Q/I broadcast forms.
        0x1C..=0x27 => {
            let (m, src) = match f {
                0x1C => ("vmulq", O::Q),
                0x1D => ("vmaxi", O::I),
                0x1E => ("vmuli", O::I),
                0x1F => ("vminii", O::I),
                0x20 => ("vaddq", O::Q),
                0x21 => ("vmaddq", O::Q),
                0x22 => ("vaddi", O::I),
                0x23 => ("vmaddi", O::I),
                0x24 => ("vsubq", O::Q),
                0x25 => ("vmsubq", O::Q),
                0x26 => ("vsubi", O::I),
                _ => ("vmsubi", O::I),
            };
            if vft != 0 {
                return Kind::Invalid;
            }
            op(vsuffix(m, w), vec![O::Vf(vfd), O::Vf(vfs), src])
        }
        // Plain FMAC forms.
        0x28..=0x2F => {
            let m = match f {
                0x28 => "vadd",
                0x29 => "vmadd",
                0x2A => "vmul",
                0x2B => "vmax",
                0x2C => "vsub",
                0x2D => "vmsub",
                0x2E => "vopmsub",
                _ => "vmini",
            };
            op(vsuffix(m, w), vec![O::Vf(vfd), O::Vf(vfs), O::Vf(vft)])
        }
        // Integer ops. Registers are the low 4 bits of each field.
        0x30 | 0x31 | 0x34 | 0x35 => {
            if (w >> 21) & 0xF != 0 {
                return Kind::Invalid;
            }
            let m = match f {
                0x30 => "viadd",
                0x31 => "visub",
                0x34 => "viand",
                _ => "vior",
            };
            op(
                m,
                vec![O::Vi(vfd & 0xF), O::Vi(vfs & 0xF), O::Vi(vft & 0xF)],
            )
        }
        0x32 => {
            if (w >> 21) & 0xF != 0 {
                return Kind::Invalid;
            }
            let imm5 = ((vfd as i32) << 27) >> 27;
            op(
                "viaddi",
                vec![O::Vi(vft & 0xF), O::Vi(vfs & 0xF), O::Imm(imm5)],
            )
        }
        0x38 => {
            // vcallms: imm15 in bits 20..6 (overlapping the ft/fs/fd
            // fields), byte address = imm15 * 8. Only the dest field
            // must be zero.
            if (w >> 21) & 0xF != 0 {
                return Kind::Invalid;
            }
            let imm15 = (w >> 6) & 0x7FFF;
            op("vcallms", vec![O::UImm(imm15 << 3)])
        }
        0x39 => {
            if (w >> 21) & 0xF != 0 || vft != 0 || vfd != 0 {
                return Kind::Invalid;
            }
            op("vcallmsr", vec![O::Vi(vfs)])
        }
        0x3C..=0x3F => cop2_special2(w),
        _ => Kind::Invalid,
    }
}

fn cop2_special2(w: u32) -> Kind {
    let bc = (funct(w) & 3) as u8;
    let sub = sa(w);
    let vft = rt(w);
    let vfs = rd(w);
    let bcc = COMPS[bc as usize];

    // ACC ops with a broadcast source: vaddax.dest ACC, vfs, vftx
    let acc_bc = |name: &str| -> Kind {
        op(
            vsuffix(&format!("v{name}{bcc}"), w),
            vec![O::Acc, O::Vf(vfs), O::VfComp(vft, bcc)],
        )
    };

    match sub {
        0x00 => acc_bc("adda"),
        0x01 => acc_bc("suba"),
        0x02 => acc_bc("madda"),
        0x03 => acc_bc("msuba"),
        0x04 | 0x05 => {
            let kind = if sub == 0x04 { "vitof" } else { "vftoi" };
            let bits = ["0", "4", "12", "15"][bc as usize];
            op(
                vsuffix(&format!("{kind}{bits}"), w),
                vec![O::Vf(vft), O::Vf(vfs)],
            )
        }
        0x06 => acc_bc("mula"),
        0x07 => match bc {
            0 => {
                if vft != 0 {
                    return Kind::Invalid;
                }
                op(vsuffix("vmulaq", w), vec![O::Acc, O::Vf(vfs), O::Q])
            }
            1 => op(vsuffix("vabs", w), vec![O::Vf(vft), O::Vf(vfs)]),
            2 => {
                if vft != 0 {
                    return Kind::Invalid;
                }
                op(vsuffix("vmulai", w), vec![O::Acc, O::Vf(vfs), O::I])
            }
            _ => op(
                vsuffix("vclipw", w),
                vec![O::Vf(vfs), O::VfComp(vft, 'w')],
            ),
        },
        0x08 | 0x09 => {
            let (m, src) = match (sub, bc) {
                (0x08, 0) => ("vaddaq", O::Q),
                (0x08, 1) => ("vmaddaq", O::Q),
                (0x08, 2) => ("vaddai", O::I),
                (0x08, 3) => ("vmaddai", O::I),
                (0x09, 0) => ("vsubaq", O::Q),
                (0x09, 1) => ("vmsubaq", O::Q),
                (0x09, 2) => ("vsubai", O::I),
                _ => ("vmsubai", O::I),
            };
            if vft != 0 {
                return Kind::Invalid;
            }
            op(vsuffix(m, w), vec![O::Acc, O::Vf(vfs), src])
        }
        0x0A => {
            let m = match bc {
                0 => "vadda",
                1 => "vmadda",
                2 => "vmula",
                _ => return Kind::Invalid,
            };
            op(vsuffix(m, w), vec![O::Acc, O::Vf(vfs), O::Vf(vft)])
        }
        0x0B => match bc {
            0 => op(vsuffix("vsuba", w), vec![O::Acc, O::Vf(vfs), O::Vf(vft)]),
            1 => op(vsuffix("vmsuba", w), vec![O::Acc, O::Vf(vfs), O::Vf(vft)]),
            2 => op(vsuffix("vopmula", w), vec![O::Acc, O::Vf(vfs), O::Vf(vft)]),
            _ => {
                // vnop: canonical encoding has all register fields zero.
                if (w >> 11) & 0x3FFF != 0 {
                    return Kind::Invalid;
                }
                op("vnop", vec![])
            }
        },
        0x0C => match bc {
            0 => op(vsuffix("vmove", w), vec![O::Vf(vft), O::Vf(vfs)]),
            1 => op(vsuffix("vmr32", w), vec![O::Vf(vft), O::Vf(vfs)]),
            _ => Kind::Invalid,
        },
        0x0D => match bc {
            0 => op(
                vsuffix("vlqi", w),
                vec![O::Vf(vft), O::ViInc(vfs & 0xF)],
            ),
            1 => op(
                vsuffix("vsqi", w),
                vec![O::Vf(vfs), O::ViInc(vft & 0xF)],
            ),
            2 => op(
                vsuffix("vlqd", w),
                vec![O::Vf(vft), O::ViDec(vfs & 0xF)],
            ),
            _ => op(
                vsuffix("vsqd", w),
                vec![O::Vf(vfs), O::ViDec(vft & 0xF)],
            ),
        },
        0x0E => match bc {
            0 => op(
                "vdiv",
                vec![O::Q, O::VfComp(vfs, fsf(w)), O::VfComp(vft, ftf(w))],
            ),
            1 => op("vsqrt", vec![O::Q, O::VfComp(vft, ftf(w))]),
            2 => op(
                "vrsqrt",
                vec![O::Q, O::VfComp(vfs, fsf(w)), O::VfComp(vft, ftf(w))],
            ),
            _ => {
                if (w >> 11) & 0x3FFF != 0 {
                    return Kind::Invalid;
                }
                op("vwaitq", vec![])
            }
        },
        0x0F => match bc {
            0 => op("vmtir", vec![O::Vi(vft & 0xF), O::VfComp(vfs, fsf(w))]),
            1 => op(vsuffix("vmfir", w), vec![O::Vf(vft), O::Vi(vfs & 0xF)]),
            2 => op(
                vsuffix("vilwr", w),
                vec![O::Vi(vft & 0xF), O::ViInd(vfs & 0xF)],
            ),
            _ => op(
                vsuffix("viswr", w),
                vec![O::Vi(vft & 0xF), O::ViInd(vfs & 0xF)],
            ),
        },
        0x10 => match bc {
            0 => op(vsuffix("vrnext", w), vec![O::Vf(vft), O::R]),
            1 => op(vsuffix("vrget", w), vec![O::Vf(vft), O::R]),
            2 => op("vrinit", vec![O::R, O::VfComp(vfs, fsf(w))]),
            _ => op("vrxor", vec![O::R, O::VfComp(vfs, fsf(w))]),
        },
        _ => Kind::Invalid,
    }
}
