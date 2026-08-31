//! Reference interpreter over the decoded `Insn` model, executing on an
//! R5900Context through the C shim so every semantic step runs the same
//! recomp_ops.h helpers the emitter's generated code uses.
//!
//! Scope: the straight-line mnemonics the emitter covers (integer set,
//! COP1 tier 0, censused COP2 macro set). Control flow, syscalls and the
//! COP0/privileged hooks are the translator's and runtime's business and
//! are rejected here.

use anyhow::{bail, Result};
use ee_emit::cop2::{self, AccFmac, Cop2, Fmac};
use r5900_decode::{Insn, Operand};

use crate::ctx::Ctx;
use crate::ffi;

fn gpr(ops: &[Operand], i: usize) -> u8 {
    match ops.get(i) {
        Some(Operand::Gpr(n)) => *n,
        other => panic!("expected Gpr operand, got {other:?}"),
    }
}

fn fpr(ops: &[Operand], i: usize) -> u8 {
    match ops.get(i) {
        Some(Operand::Fpr(n)) => *n,
        other => panic!("expected Fpr operand, got {other:?}"),
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

fn se32(v: u32) -> u64 {
    v as i32 as i64 as u64
}

fn ea(ctx: &Ctx, base: u8, off: i32) -> u32 {
    ctx.r32(base).wrapping_add(off as u32)
}

fn u16s(b: [u8; 16]) -> [u16; 8] {
    let mut o = [0u16; 8];
    for (i, v) in o.iter_mut().enumerate() {
        *v = u16::from_le_bytes([b[2 * i], b[2 * i + 1]]);
    }
    o
}

fn from_u16s(v: [u16; 8]) -> [u8; 16] {
    let mut b = [0u8; 16];
    for (i, x) in v.iter().enumerate() {
        b[2 * i..2 * i + 2].copy_from_slice(&x.to_le_bytes());
    }
    b
}

fn u32s(b: [u8; 16]) -> [u32; 4] {
    let mut o = [0u32; 4];
    for (i, v) in o.iter_mut().enumerate() {
        *v = u32::from_le_bytes(b[4 * i..4 * i + 4].try_into().unwrap());
    }
    o
}

fn from_u32s(v: [u32; 4]) -> [u8; 16] {
    let mut b = [0u8; 16];
    for (i, x) in v.iter().enumerate() {
        b[4 * i..4 * i + 4].copy_from_slice(&x.to_le_bytes());
    }
    b
}

fn u64s(b: [u8; 16]) -> [u64; 2] {
    [
        u64::from_le_bytes(b[0..8].try_into().unwrap()),
        u64::from_le_bytes(b[8..16].try_into().unwrap()),
    ]
}

fn from_u64s(v: [u64; 2]) -> [u8; 16] {
    let mut b = [0u8; 16];
    b[0..8].copy_from_slice(&v[0].to_le_bytes());
    b[8..16].copy_from_slice(&v[1].to_le_bytes());
    b
}

/// mult/madd family: run the helper, then copy LO into rd if present.
fn mult_like(
    ctx: &mut Ctx,
    pipe: usize,
    rd: u8,
    a: i32,
    b: i32,
    f: unsafe extern "C" fn(*mut u64, *mut u64, i32, i32),
) {
    unsafe { f(ctx.lo_ptr(pipe), ctx.hi_ptr(pipe), a, b) };
    if rd != 0 {
        let lo = ctx.lohi(false, pipe);
        ctx.set_r64(rd, lo);
    }
}

/// Execute one straight-line instruction. Errors on control flow and on
/// anything outside the emitter's measured coverage.
pub fn step(ctx: &mut Ctx, insn: &Insn) -> Result<()> {
    let Some(m) = insn.mnemonic() else {
        bail!("invalid instruction word 0x{:08X}", insn.word);
    };
    let ops = insn.operands();

    if cop2::is_cop2(m) {
        return step_cop2(ctx, insn);
    }

    match m {
        "nop" | "sync" | "sync.p" | "cache" => {}

        // ---- ALU immediate ------------------------------------------------
        "addi" | "addiu" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), imm(ops));
            let r = se32(ctx.r32(rs).wrapping_add(v as u32));
            ctx.set_r64(rt, r);
        }
        "daddiu" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), imm(ops));
            let r = ctx.r64(rs).wrapping_add(v as i64 as u64);
            ctx.set_r64(rt, r);
        }
        "slti" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), imm(ops));
            let r = ((ctx.r64(rs) as i64) < v as i64) as u64;
            ctx.set_r64(rt, r);
        }
        "sltiu" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), imm(ops));
            let r = (ctx.r64(rs) < v as i64 as u64) as u64;
            ctx.set_r64(rt, r);
        }
        "andi" | "ori" | "xori" => {
            let (rt, rs, v) = (gpr(ops, 0), gpr(ops, 1), uimm(ops) as u64);
            let a = ctx.r64(rs);
            let r = match m {
                "andi" => a & v,
                "ori" => a | v,
                _ => a ^ v,
            };
            ctx.set_r64(rt, r);
        }
        "lui" => {
            let (rt, v) = (gpr(ops, 0), uimm(ops));
            ctx.set_r64(rt, ((v << 16) as i32) as i64 as u64);
        }

        // ---- ALU register -------------------------------------------------
        "addu" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let r = se32(ctx.r32(rs).wrapping_add(ctx.r32(rt)));
            ctx.set_r64(rd, r);
        }
        "subu" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let r = se32(ctx.r32(rs).wrapping_sub(ctx.r32(rt)));
            ctx.set_r64(rd, r);
        }
        "negu" => {
            let (rd, rt) = (gpr(ops, 0), gpr(ops, 1));
            let r = se32(0u32.wrapping_sub(ctx.r32(rt)));
            ctx.set_r64(rd, r);
        }
        "daddu" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let r = ctx.r64(rs).wrapping_add(ctx.r64(rt));
            ctx.set_r64(rd, r);
        }
        "dsubu" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let r = ctx.r64(rs).wrapping_sub(ctx.r64(rt));
            ctx.set_r64(rd, r);
        }
        "and" | "or" | "xor" | "nor" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let (a, b) = (ctx.r64(rs), ctx.r64(rt));
            let r = match m {
                "and" => a & b,
                "or" => a | b,
                "xor" => a ^ b,
                _ => !(a | b),
            };
            ctx.set_r64(rd, r);
        }
        "slt" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let r = ((ctx.r64(rs) as i64) < (ctx.r64(rt) as i64)) as u64;
            ctx.set_r64(rd, r);
        }
        "sltu" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let r = (ctx.r64(rs) < ctx.r64(rt)) as u64;
            ctx.set_r64(rd, r);
        }
        "movz" | "movn" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let t = ctx.r64(rt);
            let hit = if m == "movz" { t == 0 } else { t != 0 };
            if hit {
                let v = ctx.r64(rs);
                ctx.set_r64(rd, v);
            }
        }

        // ---- shifts -------------------------------------------------------
        "sll" | "srl" | "sra" => {
            let (rd, rt, sa) = (gpr(ops, 0), gpr(ops, 1), dec(ops));
            let t = ctx.r32(rt);
            let r = match m {
                "sll" => t << sa,
                "srl" => t >> sa,
                _ => ((t as i32) >> sa) as u32,
            };
            ctx.set_r64(rd, se32(r));
        }
        "dsll" | "dsrl" | "dsra" | "dsll32" | "dsrl32" | "dsra32" => {
            let (rd, rt, sa) = (gpr(ops, 0), gpr(ops, 1), dec(ops));
            let sa = if m.ends_with("32") { sa + 32 } else { sa };
            let t = ctx.r64(rt);
            let r = match &m[..4] {
                "dsll" => t << sa,
                "dsrl" => t >> sa,
                _ => ((t as i64) >> sa) as u64,
            };
            ctx.set_r64(rd, r);
        }
        "sllv" | "srlv" | "srav" => {
            let (rd, rt, rs) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let sh = ctx.r32(rs) & 31;
            let t = ctx.r32(rt);
            let r = match m {
                "sllv" => t << sh,
                "srlv" => t >> sh,
                _ => ((t as i32) >> sh) as u32,
            };
            ctx.set_r64(rd, se32(r));
        }
        "dsllv" | "dsrlv" | "dsrav" => {
            let (rd, rt, rs) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let sh = ctx.r32(rs) & 63;
            let t = ctx.r64(rt);
            let r = match m {
                "dsllv" => t << sh,
                "dsrlv" => t >> sh,
                _ => ((t as i64) >> sh) as u64,
            };
            ctx.set_r64(rd, r);
        }

        // ---- loads --------------------------------------------------------
        "lb" | "lbu" | "lh" | "lhu" | "lw" | "lwu" | "ld" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = ea(ctx, base, off);
            let v = unsafe {
                match m {
                    "lb" => ffi::x_read8(a) as i8 as i64 as u64,
                    "lbu" => ffi::x_read8(a) as u64,
                    "lh" => ffi::x_read16(a) as i16 as i64 as u64,
                    "lhu" => ffi::x_read16(a) as u64,
                    "lw" => se32(ffi::x_read32(a)),
                    "lwu" => ffi::x_read32(a) as u64,
                    _ => ffi::x_read64(a),
                }
            };
            ctx.set_r64(rt, v);
        }
        "lq" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = ea(ctx, base, off);
            let mut buf = [0u8; 16];
            unsafe { ffi::x_read128(a, buf.as_mut_ptr()) };
            ctx.set_r128(rt, buf);
        }
        "lwl" | "lwr" | "ldl" | "ldr" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = ea(ctx, base, off);
            let cur = ctx.r64(rt);
            let v = unsafe {
                match m {
                    "lwl" => ffi::x_lwl(a, cur),
                    "lwr" => ffi::x_lwr(a, cur),
                    "ldl" => ffi::x_ldl(a, cur),
                    _ => ffi::x_ldr(a, cur),
                }
            };
            ctx.set_r64(rt, v);
        }

        // ---- stores -------------------------------------------------------
        "sb" | "sh" | "sw" | "sd" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = ea(ctx, base, off);
            unsafe {
                match m {
                    "sb" => ffi::x_write8(a, ctx.r32(rt) as u8),
                    "sh" => ffi::x_write16(a, ctx.r32(rt) as u16),
                    "sw" => ffi::x_write32(a, ctx.r32(rt)),
                    _ => ffi::x_write64(a, ctx.r64(rt)),
                }
            }
        }
        "sq" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = ea(ctx, base, off);
            let v = ctx.r128(rt);
            unsafe { ffi::x_write128(a, v.as_ptr()) };
        }
        "swl" | "swr" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = ea(ctx, base, off);
            let v = ctx.r32(rt);
            unsafe {
                if m == "swl" {
                    ffi::x_swl(a, v)
                } else {
                    ffi::x_swr(a, v)
                }
            }
        }
        "sdl" | "sdr" => {
            let rt = gpr(ops, 0);
            let (off, base) = mem(ops);
            let a = ea(ctx, base, off);
            let v = ctx.r64(rt);
            unsafe {
                if m == "sdl" {
                    ffi::x_sdl(a, v)
                } else {
                    ffi::x_sdr(a, v)
                }
            }
        }

        // ---- multiply / divide --------------------------------------------
        "mult" | "mult1" => {
            let pipe = usize::from(m == "mult1");
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let (a, b) = (ctx.r32(rs) as i32, ctx.r32(rt) as i32);
            mult_like(ctx, pipe, rd, a, b, ffi::x_mult);
        }
        "madd" | "madd1" => {
            let pipe = usize::from(m == "madd1");
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            let (a, b) = (ctx.r32(rs) as i32, ctx.r32(rt) as i32);
            mult_like(ctx, pipe, rd, a, b, ffi::x_madd);
        }
        "multu" => {
            let (a, b) = (ctx.r32(gpr(ops, 0)), ctx.r32(gpr(ops, 1)));
            unsafe { ffi::x_multu(ctx.lo_ptr(0), ctx.hi_ptr(0), a, b) };
        }
        "div" | "div1" => {
            let pipe = usize::from(m == "div1");
            let (a, b) = (ctx.r32(gpr(ops, 1)) as i32, ctx.r32(gpr(ops, 2)) as i32);
            unsafe { ffi::x_div(ctx.lo_ptr(pipe), ctx.hi_ptr(pipe), a, b) };
        }
        "divu" => {
            let (a, b) = (ctx.r32(gpr(ops, 1)), ctx.r32(gpr(ops, 2)));
            unsafe { ffi::x_divu(ctx.lo_ptr(0), ctx.hi_ptr(0), a, b) };
        }
        "mfhi" | "mfhi1" | "mflo" | "mflo1" => {
            let pipe = usize::from(m.ends_with('1'));
            let v = ctx.lohi(m.starts_with("mfhi"), pipe);
            ctx.set_r64(gpr(ops, 0), v);
        }
        "mthi" | "mthi1" | "mtlo" | "mtlo1" => {
            let pipe = usize::from(m.ends_with('1'));
            let v = ctx.r64(gpr(ops, 0));
            ctx.set_lohi(m.starts_with("mthi"), pipe, v);
        }

        // ---- SA register / qfsrv ------------------------------------------
        "mfsa" => {
            let v = ctx.sa() as u64;
            ctx.set_r64(gpr(ops, 0), v);
        }
        "mtsa" => {
            let v = ctx.r32(gpr(ops, 0)) & 0xF;
            ctx.set_sa(v);
        }
        "mtsab" => {
            let (rs, v) = (gpr(ops, 0), uimm(ops));
            let sa = (ctx.r32(rs) & 0xF) ^ (v & 0xF);
            ctx.set_sa(sa);
        }
        "qfsrv" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            if rd != 0 {
                let s = ctx.r128(rs);
                let t = ctx.r128(rt);
                let mut d = [0u8; 16];
                unsafe { ffi::x_qfsrv(s.as_ptr(), t.as_ptr(), ctx.sa(), d.as_mut_ptr()) };
                ctx.set_r128(rd, d);
            }
        }

        // ---- MMI 128-bit integer ------------------------------------------
        "paddh" | "psubb" | "psubw" | "pcgth" | "pmaxh" | "pminh" | "pextlb" | "pextub"
        | "pextlw" | "pextuw" | "pcpyld" | "pcpyud" | "ppacb" | "pand" | "por" | "pxor"
        | "pnor" => {
            let (rd, rs, rt) = (gpr(ops, 0), gpr(ops, 1), gpr(ops, 2));
            if rd != 0 {
                let s = ctx.r128(rs);
                let t = ctx.r128(rt);
                let d = mmi3(m, s, t);
                ctx.set_r128(rd, d);
            }
        }
        "pcpyh" => {
            let (rd, rt) = (gpr(ops, 0), gpr(ops, 1));
            if rd != 0 {
                let t = u16s(ctx.r128(rt));
                let mut d = [0u16; 8];
                for i in 0..4 {
                    d[i] = t[0];
                    d[4 + i] = t[4];
                }
                ctx.set_r128(rd, from_u16s(d));
            }
        }
        "psllh" | "psrlh" | "psrah" => {
            let (rd, rt, sa) = (gpr(ops, 0), gpr(ops, 1), dec(ops) & 15);
            if rd != 0 {
                let t = u16s(ctx.r128(rt));
                let mut d = [0u16; 8];
                for i in 0..8 {
                    d[i] = match m {
                        "psllh" => t[i] << sa,
                        "psrlh" => t[i] >> sa,
                        _ => ((t[i] as i16) >> sa) as u16,
                    };
                }
                ctx.set_r128(rd, from_u16s(d));
            }
        }

        // ---- COP1 (FPU tier 0) --------------------------------------------
        "lwc1" => {
            let f = fpr(ops, 0);
            let (off, base) = mem(ops);
            let a = ea(ctx, base, off);
            let v = unsafe { ffi::x_read32(a) };
            ctx.set_f_bits(f, v);
        }
        "swc1" => {
            let f = fpr(ops, 0);
            let (off, base) = mem(ops);
            let a = ea(ctx, base, off);
            unsafe { ffi::x_write32(a, ctx.f_bits(f)) };
        }
        "mfc1" => {
            let (rt, fs) = (gpr(ops, 0), fpr(ops, 1));
            let v = se32(ctx.f_bits(fs));
            ctx.set_r64(rt, v);
        }
        "mtc1" => {
            let (rt, fs) = (gpr(ops, 0), fpr(ops, 1));
            let v = ctx.r32(rt);
            ctx.set_f_bits(fs, v);
        }
        "mov.s" | "neg.s" | "abs.s" => {
            let (fd, fs) = (fpr(ops, 0), fpr(ops, 1));
            let a = ctx.f_bits(fs);
            let v = unsafe {
                match m {
                    "mov.s" => ffi::x_fmov(a),
                    "neg.s" => ffi::x_fneg(a),
                    _ => ffi::x_fabs(a),
                }
            };
            ctx.set_f_bits(fd, v);
        }
        "add.s" | "sub.s" | "mul.s" | "div.s" => {
            let (fd, fs, ft) = (fpr(ops, 0), fpr(ops, 1), fpr(ops, 2));
            let (a, b) = (ctx.f_bits(fs), ctx.f_bits(ft));
            let v = unsafe {
                match m {
                    "add.s" => ffi::x_fadd(a, b),
                    "sub.s" => ffi::x_fsub(a, b),
                    "mul.s" => ffi::x_fmul(a, b),
                    _ => ffi::x_fdiv(a, b),
                }
            };
            ctx.set_f_bits(fd, v);
        }
        "cvt.s.w" => {
            let (fd, fs) = (fpr(ops, 0), fpr(ops, 1));
            let v = unsafe { ffi::x_cvtsw(ctx.f_bits(fs) as i32) };
            ctx.set_f_bits(fd, v);
        }
        "cvt.w.s" => {
            let (fd, fs) = (fpr(ops, 0), fpr(ops, 1));
            let v = unsafe { ffi::x_cvtws(ctx.f_bits(fs)) } as u32;
            ctx.set_f_bits(fd, v);
        }
        "c.eq.s" | "c.lt.s" | "c.le.s" => {
            let (fs, ft) = (fpr(ops, 0), fpr(ops, 1));
            let (a, b) = (ctx.f_bits(fs), ctx.f_bits(ft));
            unsafe {
                let cond = match m {
                    "c.eq.s" => ffi::x_fc_eq(a, b),
                    "c.lt.s" => ffi::x_fc_lt(a, b),
                    _ => ffi::x_fc_le(a, b),
                };
                ffi::x_fcr31_cond(ctx.fcr31_ptr(), cond);
            }
        }

        other => bail!("interpreter does not cover mnemonic {other}"),
    }
    Ok(())
}

fn mmi3(m: &str, s: [u8; 16], t: [u8; 16]) -> [u8; 16] {
    match m {
        "paddh" => {
            let (a, b) = (u16s(s), u16s(t));
            let mut d = [0u16; 8];
            for i in 0..8 {
                d[i] = a[i].wrapping_add(b[i]);
            }
            from_u16s(d)
        }
        "psubb" => {
            let mut d = [0u8; 16];
            for i in 0..16 {
                d[i] = s[i].wrapping_sub(t[i]);
            }
            d
        }
        "psubw" => {
            let (a, b) = (u32s(s), u32s(t));
            let mut d = [0u32; 4];
            for i in 0..4 {
                d[i] = a[i].wrapping_sub(b[i]);
            }
            from_u32s(d)
        }
        "pcgth" => {
            let (a, b) = (u16s(s), u16s(t));
            let mut d = [0u16; 8];
            for i in 0..8 {
                d[i] = if (a[i] as i16) > (b[i] as i16) { 0xFFFF } else { 0 };
            }
            from_u16s(d)
        }
        "pmaxh" | "pminh" => {
            let (a, b) = (u16s(s), u16s(t));
            let mut d = [0u16; 8];
            for i in 0..8 {
                let (x, y) = (a[i] as i16, b[i] as i16);
                d[i] = if m == "pmaxh" { x.max(y) } else { x.min(y) } as u16;
            }
            from_u16s(d)
        }
        "pextlb" => {
            let mut d = [0u8; 16];
            for i in 0..8 {
                d[2 * i] = t[i];
                d[2 * i + 1] = s[i];
            }
            d
        }
        "pextub" => {
            let mut d = [0u8; 16];
            for i in 0..8 {
                d[2 * i] = t[i + 8];
                d[2 * i + 1] = s[i + 8];
            }
            d
        }
        "pextlw" => {
            let (a, b) = (u32s(s), u32s(t));
            from_u32s([b[0], a[0], b[1], a[1]])
        }
        "pextuw" => {
            let (a, b) = (u32s(s), u32s(t));
            from_u32s([b[2], a[2], b[3], a[3]])
        }
        "pcpyld" => {
            let (a, b) = (u64s(s), u64s(t));
            from_u64s([b[0], a[0]])
        }
        "pcpyud" => {
            let (a, b) = (u64s(s), u64s(t));
            from_u64s([a[1], b[1]])
        }
        "ppacb" => {
            let mut d = [0u8; 16];
            for i in 0..8 {
                d[i] = t[2 * i];
                d[8 + i] = s[2 * i];
            }
            d
        }
        "pand" | "por" | "pxor" | "pnor" => {
            let (a, b) = (u64s(s), u64s(t));
            let f = |x: u64, y: u64| match m {
                "pand" => x & y,
                "por" => x | y,
                "pxor" => x ^ y,
                _ => !(x | y),
            };
            from_u64s([f(a[0], b[0]), f(a[1], b[1])])
        }
        _ => unreachable!("mmi3 dispatch"),
    }
}

fn step_cop2(ctx: &mut Ctx, insn: &Insn) -> Result<()> {
    let op = cop2::parse(insn)?;
    let p = ctx.as_mut_ptr() as *mut std::os::raw::c_void;
    unsafe {
        match op {
            Cop2::Lqc2 { ft, offset, base } => {
                let a = ea(ctx, base, offset);
                ffi::x_vu_lqc2(p, ft as i32, a);
            }
            Cop2::Sqc2 { fs, offset, base } => {
                let a = ea(ctx, base, offset);
                ffi::x_vu_sqc2(p, fs as i32, a);
            }
            Cop2::Qmfc2 { rt, fs } => {
                if rt != 0 {
                    let mut buf = [0u8; 16];
                    ffi::x_vu_qmfc(p, fs as i32, buf.as_mut_ptr());
                    ctx.set_r128(rt, buf);
                }
            }
            Cop2::Qmtc2 { rt, fd } => {
                let v = ctx.r128(rt);
                ffi::x_vu_qmtc(p, fd as i32, v.as_ptr());
            }
            Cop2::Cfc2 { rt, creg } => {
                let v = ffi::x_vu_cfc(p, creg as i32);
                if rt != 0 {
                    ctx.set_r64(rt, se32(v));
                }
            }
            Cop2::Ctc2 { rt, creg } => {
                let v = ctx.r32(rt);
                ffi::x_vu_ctc(p, creg as i32, v);
            }
            Cop2::Fmac { family, fd, fs, ft, sel, mask } => {
                let sel = sel.code() as i32;
                let (fd, fs, ft, mask) = (fd as i32, fs as i32, ft as i32, mask as i32);
                match family {
                    Fmac::Add => ffi::x_vu_fmac(p, 0, 0, fd, fs, ft, sel, mask),
                    Fmac::Sub => ffi::x_vu_fmac(p, 1, 0, fd, fs, ft, sel, mask),
                    Fmac::Mul => ffi::x_vu_fmac(p, 2, 0, fd, fs, ft, sel, mask),
                    Fmac::Madd => ffi::x_vu_fmac(p, 3, 0, fd, fs, ft, sel, mask),
                    Fmac::Msub => ffi::x_vu_fmac(p, 4, 0, fd, fs, ft, sel, mask),
                    Fmac::Max => ffi::x_vu_maxmin(p, 0, fd, fs, ft, sel, mask),
                    Fmac::Mini => ffi::x_vu_maxmin(p, 1, fd, fs, ft, sel, mask),
                }
            }
            Cop2::FmacAcc { family, fs, ft, sel, mask } => {
                let sel = sel.code() as i32;
                let (fs, ft, mask) = (fs as i32, ft as i32, mask as i32);
                match family {
                    AccFmac::Adda => ffi::x_vu_fmac(p, 0, 1, 0, fs, ft, sel, mask),
                    AccFmac::Mula => ffi::x_vu_fmac(p, 2, 1, 0, fs, ft, sel, mask),
                    AccFmac::Madda => ffi::x_vu_fmac(p, 3, 1, 0, fs, ft, sel, mask),
                }
            }
            Cop2::Opmula { fs, ft } => ffi::x_vu_opmula(p, fs as i32, ft as i32),
            Cop2::Opmsub { fd, fs, ft } => {
                ffi::x_vu_opmsub(p, fd as i32, fs as i32, ft as i32)
            }
            Cop2::Ftoi { shift, ft, fs, mask } => {
                ffi::x_vu_ftoi(p, shift as i32, ft as i32, fs as i32, mask as i32)
            }
            Cop2::Itof { shift, ft, fs, mask } => {
                ffi::x_vu_itof(p, shift as i32, ft as i32, fs as i32, mask as i32)
            }
            Cop2::Move { ft, fs, mask } => {
                ffi::x_vu_move(p, ft as i32, fs as i32, mask as i32)
            }
            Cop2::Mr32 { ft, fs, mask } => {
                ffi::x_vu_mr32(p, ft as i32, fs as i32, mask as i32)
            }
            Cop2::Div { fs, fsf, ft, ftf } => {
                ffi::x_vu_div(p, fs as i32, fsf as i32, ft as i32, ftf as i32)
            }
            Cop2::Sqrt { ft, ftf } => ffi::x_vu_sqrt(p, ft as i32, ftf as i32),
            Cop2::Rsqrt { fs, fsf, ft, ftf } => {
                ffi::x_vu_rsqrt(p, fs as i32, fsf as i32, ft as i32, ftf as i32)
            }
            Cop2::Clipw { fs, ft } => ffi::x_vu_clipw(p, fs as i32, ft as i32),
            Cop2::Lqi { ft, is, mask } => {
                ffi::x_vu_lqi(p, ft as i32, is as i32, mask as i32)
            }
            Cop2::Lqd { ft, is, mask } => {
                ffi::x_vu_lqd(p, ft as i32, is as i32, mask as i32)
            }
            Cop2::Sqi { fs, it, mask } => {
                ffi::x_vu_sqi(p, fs as i32, it as i32, mask as i32)
            }
            Cop2::Iaddi { it, is, imm } => ffi::x_vu_iaddi(p, it as i32, is as i32, imm),
            Cop2::Rnext { ft, mask } => ffi::x_vu_rnext(p, ft as i32, mask as i32),
            Cop2::Rinit { fs, fsf } => ffi::x_vu_rinit(p, fs as i32, fsf as i32),
            Cop2::Rxor { fs, fsf } => ffi::x_vu_rxor(p, fs as i32, fsf as i32),
            Cop2::Waitq | Cop2::Nop => {}
        }
    }
    Ok(())
}
