//! Straight-line instruction emission: one decoded instruction to one C
//! statement (possibly a braced block). Control-flow instructions never
//! reach this module; `body.rs` handles them.
//!
//! Coverage policy: the match below implements exactly the integer mnemonic
//! set measured in this one binary (see `census`). COP1 and COP2 mnemonics
//! are routed to `rt_unimplemented`. Anything else is a translation-time
//! hard error.

use std::collections::BTreeMap;

use anyhow::Result;
use r5900_decode::{Insn, Operand};

/// Per-emission-run statistics owned by the caller.
#[derive(Debug, Default, Clone)]
pub struct OpStats {
    /// Mnemonics routed to rt_unimplemented, with counts.
    pub unimplemented: BTreeMap<String, usize>,
    /// Invalid (data) words inside .text function bodies.
    pub invalid_words: usize,
    /// Mnemonics with no implementation and no routing: hard error at end.
    pub unknown: BTreeMap<String, usize>,
}

fn gpr(ops: &[Operand], i: usize) -> u8 {
    match ops.get(i) {
        Some(Operand::Gpr(n)) => *n,
        other => panic!("expected Gpr operand, got {other:?}"),
    }
}

fn mem(ops: &[Operand]) -> (i32, u8) {
    for op in ops {
        if let Operand::Mem { offset, base } = op {
            return (*offset, *base);
        }
    }
    panic!("expected Mem operand");
}

fn imm(ops: &[Operand]) -> i32 {
    for op in ops {
        if let Operand::Imm(v) = op {
            return *v;
        }
    }
    panic!("expected Imm operand");
}

fn uimm(ops: &[Operand]) -> u32 {
    for op in ops {
        if let Operand::UImm(v) = op {
            return *v;
        }
    }
    panic!("expected UImm operand");
}

fn dec(ops: &[Operand]) -> u32 {
    for op in ops {
        if let Operand::Dec(v) = op {
            return *v;
        }
    }
    panic!("expected Dec operand");
}

fn cop0(ops: &[Operand]) -> u8 {
    for op in ops {
        if let Operand::Cop0(v) = op {
            return *v;
        }
    }
    panic!("expected Cop0 operand");
}

// ---- register access expressions ---------------------------------------

pub fn ru64(n: u8) -> String {
    if n == 0 {
        "0ull".into()
    } else {
        format!("ctx->r[{n}].u64x[0]")
    }
}
fn rs64(n: u8) -> String {
    if n == 0 {
        "0ll".into()
    } else {
        format!("ctx->r[{n}].s64x[0]")
    }
}
pub fn ru32(n: u8) -> String {
    if n == 0 {
        "0u".into()
    } else {
        format!("ctx->r[{n}].u32x[0]")
    }
}
fn rs32(n: u8) -> String {
    if n == 0 {
        "0".into()
    } else {
        format!("ctx->r[{n}].s32x[0]")
    }
}

/// 64-bit GPR write; writes to $zero are suppressed (empty statement).
fn w64(n: u8, val: &str) -> String {
    if n == 0 {
        String::new()
    } else {
        format!("ctx->r[{n}].u64x[0] = {val};")
    }
}

fn se32(e: &str) -> String {
    format!("(uint64_t)RC_SE32((int32_t)({e}))")
}

/// Effective address: 32-bit wrapping base + sign-extended offset.
fn addr(base: u8, off: i32) -> String {
    if base == 0 {
        format!("0x{:X}u", off as u32)
    } else {
        format!("(uint32_t)({} + 0x{:X}u)", ru32(base), off as u32)
    }
}

/// rc_u128 source expression; $zero reads as an all-zero quadword.
fn q128(n: u8) -> String {
    if n == 0 {
        "(rc_u128){{0}}".into()
    } else {
        format!("ctx->r[{n}]")
    }
}

/// 128-bit three-operand MMI op. `body` computes `d` from `s` (rs) and `t`
/// (rt), both copied up front so destination aliasing is safe.
fn mmi3(rd: u8, rs: u8, rt: u8, body: &str) -> String {
    if rd == 0 {
        return String::new();
    }
    format!(
        "{{ const rc_u128 s = {}; const rc_u128 t = {}; rc_u128 d; {} ctx->r[{rd}] = d; }}",
        q128(rs),
        q128(rt),
        body
    )
}

/// 128-bit two-operand MMI op (`t` only).
fn mmi2(rd: u8, rt: u8, body: &str) -> String {
    if rd == 0 {
        return String::new();
    }
    format!(
        "{{ const rc_u128 t = {}; rc_u128 d; {} ctx->r[{rd}] = d; }}",
        q128(rt),
        body
    )
}

fn unimpl(m: &str, vram: u32, st: &mut OpStats) -> String {
    *st.unimplemented.entry(m.to_string()).or_insert(0) += 1;
    format!("rt_unimplemented(\"{m}\", 0x{vram:X}u);")
}

/// LO/HI slot lvalues for the given DIV/MULT pipeline (0 or 1).
fn lohi(pipe: usize) -> (String, String) {
    (
        format!("&ctx->lo.u64x[{pipe}]"),
        format!("&ctx->hi.u64x[{pipe}]"),
    )
}

/// mult/madd family with an optional rd destination receiving LO.
fn mult_like(helper: &str, pipe: usize, rd: u8, rs: u8, rt: u8) -> String {
    let (lo, hi) = lohi(pipe);
    let mut s = format!("{helper}({lo}, {hi}, {}, {});", rs32(rs), rs32(rt));
    if rd != 0 {
        s = format!("{{ {s} ctx->r[{rd}].u64x[0] = ctx->lo.u64x[{pipe}]; }}");
    }
    s
}

/// Emit one straight-line instruction as a C statement. Returns an empty
/// string for architectural no-ops.
pub fn emit_stmt(insn: &Insn, st: &mut OpStats) -> Result<String> {
    let vram = insn.vram;
    let m = match insn.mnemonic() {
        Some(m) => m,
        None => {
            // Embedded data words inside .text (e.g. the lq-read constant
            // block at 0x254CE0). Unreachable at runtime; trap loudly if not.
            st.invalid_words += 1;
            return Ok(format!("rt_unimplemented(\"invalid-word\", 0x{vram:X}u);"));
        }
    };
    let ops = insn.operands();

    // COP2 macro ops all start with 'v'; no integer mnemonic does.
    if m.starts_with('v')
        || m.starts_with("qmfc2")
        || m.starts_with("qmtc2")
        || m.starts_with("cfc2")
        || m.starts_with("ctc2")
        || m == "lqc2"
        || m == "sqc2"
    {
        return Ok(unimpl(m, vram, st));
    }

    let s = match m {
        "nop" => String::new(),

        // ---- ALU immediate ------------------------------------------------
        "addi" | "addiu" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), imm(ops));
            if rs == 0 {
                w64(rt, &format!("0x{:X}ull", v as i64 as u64))
            } else {
                w64(rt, &se32(&format!("{} + 0x{:X}u", ru32(rs), v as u32)))
            }
        }
        "daddiu" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), imm(ops));
            w64(rt, &format!("{} + 0x{:X}ull", ru64(rs), v as i64 as u64))
        }
        "slti" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), imm(ops));
            w64(
                rt,
                &format!("(uint64_t)({} < (int64_t)0x{:X}ll)", rs64(rs), v as i64),
            )
        }
        "sltiu" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), imm(ops));
            w64(
                rt,
                &format!("(uint64_t)({} < 0x{:X}ull)", ru64(rs), v as i64 as u64),
            )
        }
        "andi" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), uimm(ops));
            w64(rt, &format!("{} & 0x{v:X}ull", ru64(rs)))
        }
        "ori" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), uimm(ops));
            if rs == 0 {
                w64(rt, &format!("0x{v:X}ull"))
            } else {
                w64(rt, &format!("{} | 0x{v:X}ull", ru64(rs)))
            }
        }
        "xori" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), uimm(ops));
            w64(rt, &format!("{} ^ 0x{v:X}ull", ru64(rs)))
        }
        "lui" => {
            let (rt, v) = (gpr(ops, 0), uimm(ops));
            let val = ((v << 16) as i32) as i64 as u64;
            w64(rt, &format!("0x{val:X}ull"))
        }

        // ---- ALU register -------------------------------------------------
        "addu" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            w64(rd, &se32(&format!("{} + {}", ru32(rs), ru32(rt))))
        }
        "subu" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            w64(rd, &se32(&format!("{} - {}", ru32(rs), ru32(rt))))
        }
        "negu" => {
            let (rd, rt) = (gpr(ops, 0), gpr(ops, 1));
            w64(rd, &se32(&format!("0u - {}", ru32(rt))))
        }
        "daddu" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            w64(rd, &format!("{} + {}", ru64(rs), ru64(rt)))
        }
        "dsubu" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            w64(rd, &format!("{} - {}", ru64(rs), ru64(rt)))
        }
        "and" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            w64(rd, &format!("{} & {}", ru64(rs), ru64(rt)))
        }
        "or" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            w64(rd, &format!("{} | {}", ru64(rs), ru64(rt)))
        }
        "xor" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            w64(rd, &format!("{} ^ {}", ru64(rs), ru64(rt)))
        }
        "nor" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            w64(rd, &format!("~({} | {})", ru64(rs), ru64(rt)))
        }
        "slt" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            w64(rd, &format!("(uint64_t)({} < {})", rs64(rs), rs64(rt)))
        }
        "sltu" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            w64(rd, &format!("(uint64_t)({} < {})", ru64(rs), ru64(rt)))
        }
        "movz" | "movn" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let op = if m == "movz" { "==" } else { "!=" };
            if rd == 0 {
                String::new()
            } else {
                format!(
                    "if ({} {op} 0) {{ ctx->r[{rd}].u64x[0] = {}; }}",
                    ru64(rt),
                    ru64(rs)
                )
            }
        }

        // ---- shifts -------------------------------------------------------
        "sll" | "srl" | "sra" => {
            let (rd, rt, sa) = (gpr(ops, 0), gpr(ops, 1), dec(ops));
            let e = match m {
                "sll" => format!("{} << {sa}", ru32(rt)),
                "srl" => format!("{} >> {sa}", ru32(rt)),
                _ => format!("{} >> {sa}", rs32(rt)),
            };
            w64(rd, &se32(&e))
        }
        "dsll" | "dsrl" | "dsra" | "dsll32" | "dsrl32" | "dsra32" => {
            let (rd, rt, sa) = (gpr(ops, 0), gpr(ops, 1), dec(ops));
            let sa = if m.ends_with("32") { sa + 32 } else { sa };
            let e = match &m[..4] {
                "dsll" => format!("{} << {sa}", ru64(rt)),
                "dsrl" => format!("{} >> {sa}", ru64(rt)),
                _ => format!("(uint64_t)({} >> {sa})", rs64(rt)),
            };
            w64(rd, &e)
        }
        "sllv" | "srlv" | "srav" => {
            let (rd, rt, rs) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let sh = format!("({} & 31)", ru32(rs));
            let e = match m {
                "sllv" => format!("{} << {sh}", ru32(rt)),
                "srlv" => format!("{} >> {sh}", ru32(rt)),
                _ => format!("{} >> {sh}", rs32(rt)),
            };
            w64(rd, &se32(&e))
        }
        "dsllv" | "dsrlv" | "dsrav" => {
            let (rd, rt, rs) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let sh = format!("({} & 63)", ru32(rs));
            let e = match m {
                "dsllv" => format!("{} << {sh}", ru64(rt)),
                "dsrlv" => format!("{} >> {sh}", ru64(rt)),
                _ => format!("(uint64_t)({} >> {sh})", rs64(rt)),
            };
            w64(rd, &e)
        }

        // ---- loads --------------------------------------------------------
        "lb" | "lbu" | "lh" | "lhu" | "lw" | "lwu" | "ld" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = addr(base, off);
            let e = match m {
                "lb" => format!("(uint64_t)(int64_t)(int8_t)rc_read8({a})"),
                "lbu" => format!("(uint64_t)rc_read8({a})"),
                "lh" => format!("(uint64_t)(int64_t)(int16_t)rc_read16({a})"),
                "lhu" => format!("(uint64_t)rc_read16({a})"),
                "lw" => se32(&format!("rc_read32({a})")),
                "lwu" => format!("(uint64_t)rc_read32({a})"),
                _ => format!("rc_read64({a})"),
            };
            if rt == 0 {
                // Loads to $zero still perform the access (MMIO side effects).
                let call = match m {
                    "lb" | "lbu" => format!("rc_read8({a})"),
                    "lh" | "lhu" => format!("rc_read16({a})"),
                    "lw" | "lwu" => format!("rc_read32({a})"),
                    _ => format!("rc_read64({a})"),
                };
                format!("(void){call};")
            } else {
                w64(rt, &e)
            }
        }
        "lq" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = addr(base, off);
            if rt == 0 {
                format!("(void)rc_read128({a});")
            } else {
                format!("ctx->r[{rt}] = rc_read128({a});")
            }
        }
        "lwl" | "lwr" | "ldl" | "ldr" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = addr(base, off);
            let h = format!("rc_{m}({a}, {})", ru64(rt));
            if rt == 0 {
                format!("(void){h};")
            } else {
                w64(rt, &h)
            }
        }

        // ---- stores -------------------------------------------------------
        "sb" | "sh" | "sw" | "sd" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = addr(base, off);
            match m {
                "sb" => format!("rc_write8({a}, (uint8_t){});", ru32(rt)),
                "sh" => format!("rc_write16({a}, (uint16_t){});", ru32(rt)),
                "sw" => format!("rc_write32({a}, {});", ru32(rt)),
                _ => format!("rc_write64({a}, {});", ru64(rt)),
            }
        }
        "sq" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            format!("rc_write128({}, {});", addr(base, off), q128(rt))
        }
        "swl" | "swr" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            format!("rc_{m}({}, {});", addr(base, off), ru32(rt))
        }
        "sdl" | "sdr" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            format!("rc_{m}({}, {});", addr(base, off), ru64(rt))
        }

        // ---- multiply / divide --------------------------------------------
        "mult" | "mult1" => {
            let pipe = usize::from(m == "mult1");
            mult_like("rc_mult", pipe, gpr(ops, 0), gpr(ops, 1), gpr(ops, 2))
        }
        "multu" => {
            let (lo, hi) = lohi(0);
            format!(
                "rc_multu({lo}, {hi}, {}, {});",
                ru32(gpr(ops, 0)),
                ru32(gpr(ops, 1))
            )
        }
        "madd" | "madd1" => {
            let pipe = usize::from(m == "madd1");
            mult_like("rc_madd", pipe, gpr(ops, 0), gpr(ops, 1), gpr(ops, 2))
        }
        "div" | "div1" => {
            // Decoder shape: [Gpr(0), rs, rt].
            let pipe = usize::from(m == "div1");
            let (lo, hi) = lohi(pipe);
            format!(
                "rc_div({lo}, {hi}, {}, {});",
                rs32(gpr(ops, 1)),
                rs32(gpr(ops, 2))
            )
        }
        "divu" => {
            let (lo, hi) = lohi(0);
            format!(
                "rc_divu({lo}, {hi}, {}, {});",
                ru32(gpr(ops, 1)),
                ru32(gpr(ops, 2))
            )
        }
        "mfhi" | "mfhi1" | "mflo" | "mflo1" => {
            let pipe = usize::from(m.ends_with('1'));
            let reg = if m.starts_with("mfhi") { "hi" } else { "lo" };
            w64(gpr(ops, 0), &format!("ctx->{reg}.u64x[{pipe}]"))
        }
        "mthi" | "mthi1" | "mtlo" | "mtlo1" => {
            let pipe = usize::from(m.ends_with('1'));
            let reg = if m.starts_with("mthi") { "hi" } else { "lo" };
            format!("ctx->{reg}.u64x[{pipe}] = {};", ru64(gpr(ops, 0)))
        }

        // ---- SA register / qfsrv ------------------------------------------
        "mfsa" => w64(gpr(ops, 0), "(uint64_t)ctx->sa"),
        "mtsa" => format!("ctx->sa = {} & 0xFu;", ru32(gpr(ops, 0))),
        "mtsab" => {
            let (rs, v) = (gpr(ops, 0), uimm(ops));
            format!("ctx->sa = ({} & 0xFu) ^ 0x{:X}u;", ru32(rs), v & 0xF)
        }
        "qfsrv" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            if rd == 0 {
                String::new()
            } else {
                format!(
                    "ctx->r[{rd}] = rc_qfsrv({}, {}, ctx->sa);",
                    q128(rs),
                    q128(rt)
                )
            }
        }

        // ---- MMI 128-bit integer ------------------------------------------
        "paddh" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "for (int i = 0; i < 8; i++) d.u16x[i] = (uint16_t)(s.u16x[i] + t.u16x[i]);",
        ),
        "psubb" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "for (int i = 0; i < 16; i++) d.u8x[i] = (uint8_t)(s.u8x[i] - t.u8x[i]);",
        ),
        "psubw" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "for (int i = 0; i < 4; i++) d.u32x[i] = s.u32x[i] - t.u32x[i];",
        ),
        "pcgth" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "for (int i = 0; i < 8; i++) d.u16x[i] = s.s16x[i] > t.s16x[i] ? 0xFFFF : 0;",
        ),
        "pmaxh" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "for (int i = 0; i < 8; i++) d.s16x[i] = s.s16x[i] > t.s16x[i] ? s.s16x[i] : t.s16x[i];",
        ),
        "pminh" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "for (int i = 0; i < 8; i++) d.s16x[i] = s.s16x[i] < t.s16x[i] ? s.s16x[i] : t.s16x[i];",
        ),
        "psllh" | "psrlh" | "psrah" => {
            let (rd, rt, sa) = (gpr(ops, 0), gpr(ops, 1), dec(ops) & 15);
            let body = match m {
                "psllh" => format!(
                    "for (int i = 0; i < 8; i++) d.u16x[i] = (uint16_t)(t.u16x[i] << {sa});"
                ),
                "psrlh" => format!("for (int i = 0; i < 8; i++) d.u16x[i] = t.u16x[i] >> {sa};"),
                _ => format!(
                    "for (int i = 0; i < 8; i++) d.u16x[i] = (uint16_t)(t.s16x[i] >> {sa});"
                ),
            };
            mmi2(rd, rt, &body)
        }
        "pextlb" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "for (int i = 0; i < 8; i++) { d.u8x[2 * i] = t.u8x[i]; d.u8x[2 * i + 1] = s.u8x[i]; }",
        ),
        "pextub" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "for (int i = 0; i < 8; i++) { d.u8x[2 * i] = t.u8x[i + 8]; d.u8x[2 * i + 1] = s.u8x[i + 8]; }",
        ),
        "pextlw" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "d.u32x[0] = t.u32x[0]; d.u32x[1] = s.u32x[0]; d.u32x[2] = t.u32x[1]; d.u32x[3] = s.u32x[1];",
        ),
        "pextuw" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "d.u32x[0] = t.u32x[2]; d.u32x[1] = s.u32x[2]; d.u32x[2] = t.u32x[3]; d.u32x[3] = s.u32x[3];",
        ),
        "pcpyld" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "d.u64x[0] = t.u64x[0]; d.u64x[1] = s.u64x[0];",
        ),
        "pcpyud" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "d.u64x[0] = s.u64x[1]; d.u64x[1] = t.u64x[1];",
        ),
        "pcpyh" => mmi2(
            gpr(ops, 0),
            gpr(ops, 1),
            "for (int i = 0; i < 4; i++) { d.u16x[i] = t.u16x[0]; d.u16x[4 + i] = t.u16x[4]; }",
        ),
        "ppacb" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "for (int i = 0; i < 8; i++) { d.u8x[i] = t.u8x[2 * i]; d.u8x[8 + i] = s.u8x[2 * i]; }",
        ),
        "pand" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "d.u64x[0] = s.u64x[0] & t.u64x[0]; d.u64x[1] = s.u64x[1] & t.u64x[1];",
        ),
        "por" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "d.u64x[0] = s.u64x[0] | t.u64x[0]; d.u64x[1] = s.u64x[1] | t.u64x[1];",
        ),
        "pxor" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "d.u64x[0] = s.u64x[0] ^ t.u64x[0]; d.u64x[1] = s.u64x[1] ^ t.u64x[1];",
        ),
        "pnor" => mmi3(
            gpr(ops, 0),
            gpr(ops, 1),
            gpr(ops, 2),
            "d.u64x[0] = ~(s.u64x[0] | t.u64x[0]); d.u64x[1] = ~(s.u64x[1] | t.u64x[1]);",
        ),

        // ---- system -------------------------------------------------------
        "syscall" => "rt_syscall(ctx);".into(),
        "break" => format!("rt_break(ctx, {}u);", dec(ops)),
        "sync" | "sync.p" | "cache" => format!("/* {m} */"),
        "ei" => "rt_ei();".into(),
        "di" => "rt_di();".into(),
        "mfc0" => {
            let (rt, rd) = (gpr(ops, 0), cop0(ops));
            if rt == 0 {
                format!("(void)rt_cop0_read(ctx, {rd});")
            } else {
                w64(rt, &se32(&format!("rt_cop0_read(ctx, {rd})")))
            }
        }
        "mtc0" => {
            let (rt, rd) = (gpr(ops, 0), cop0(ops));
            format!("rt_cop0_write(ctx, {rd}, {});", ru32(rt))
        }
        // Privileged ops with no runtime hook in recomp_api.h. All sit in
        // vendor kernel-context code that is unreachable under the HLE
        // kernel; trap loudly if ever hit.
        "eret" | "tlbr" | "tlbwi" | "tlbp" | "tlbwr" => unimpl(m, vram, st),

        // ---- COP1 (P1: unimplemented except bc1* branches) ----------------
        "lwc1" | "swc1" | "mfc1" | "mtc1" | "add.s" | "sub.s" | "mul.s" | "div.s" | "abs.s"
        | "neg.s" | "mov.s" | "sqrt.s" | "cvt.s.w" | "cvt.w.s" | "c.eq.s" | "c.lt.s"
        | "c.le.s" | "c.f.s" => unimpl(m, vram, st),

        other => {
            // Collected across the whole run; lib.rs hard-errors after the
            // pass listing every unknown mnemonic at once.
            *st.unknown.entry(other.to_string()).or_insert(0) += 1;
            format!("#error \"unknown mnemonic {other}\"")
        }
    };
    Ok(s)
}
