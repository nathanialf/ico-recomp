//! C11 generation for one analyzed microprogram.
//!
//! Output shape: `void vu1_<name>(Vu1State* vu)` opens with a PC-dispatch
//! switch over every MSCAL entry stub, bal return point, and offset 0, then
//! carries the whole program as straight-line statements with labels at
//! branch targets. Bundles follow the model documented in recomp_ops.h:
//! upper half computed into locals, lower half executed, upper half
//! committed. Branches evaluate their condition before the delay slot runs
//! and jump after it; `jr` routes through the dispatch switch.
//!
//! Coverage policy: hard error on any instruction shape outside the
//! measured census of the five retail programs. No speculative ops.

use std::fmt::Write as _;

use anyhow::{bail, Result};
use vu_decode::{
    compat,
    lower::LowerOp,
    upper::{FixedPoint, FmacOp, Rhs, UpperOp},
    Bundle, Comp, Dest, LowerSlot,
};

use crate::analyze::Analysis;
use crate::layout::Vu1Program;

pub struct EmitStats {
    /// Bundles emitted at their linear position (must equal the bundle
    /// count; the coverage assertion).
    pub emitted: usize,
    /// Extra inline copies (an E-bit delay slot that is also a jump
    /// target). Zero for the shipped programs.
    pub duplicated: usize,
}

enum Ctl {
    /// Straight-line bundle.
    None,
    /// Conditional branch; lines contain `bc = ...;`.
    Cond { target: u32 },
    /// Unconditional branch (b, or bal with its link write in the lines).
    Jump { target: u32 },
    /// jr; lines contain `pc = ...;`.
    Indirect,
}

fn comp_idx(c: Comp) -> u8 {
    match c {
        Comp::X => 0,
        Comp::Y => 1,
        Comp::Z => 2,
        Comp::W => 3,
    }
}

/// (ft register, selector code) for an FMAC third operand; selector codes
/// match RC_VU_SRC_VEC / RC_VU_SRC_Q / RC_VU1_SRC_I in recomp_ops.h.
fn rhs_sel(rhs: Rhs) -> (u8, u8) {
    match rhs {
        Rhs::Bc(ft, c) => (ft.0, comp_idx(c)),
        Rhs::Ft(ft) => (ft.0, 4),
        Rhs::Q => (0, 5),
        Rhs::I => (0, 6),
    }
}

fn fixed_shift(f: FixedPoint) -> u8 {
    match f {
        FixedPoint::F0 => 0,
        FixedPoint::F4 => 4,
        FixedPoint::F12 => 12,
        FixedPoint::F15 => 15,
    }
}

/// Single-lane index of an ILW/ILWR dest field.
fn single_lane(prog: &str, off: u32, dest: Dest) -> Result<u8> {
    match dest.0 {
        8 => Ok(0),
        4 => Ok(1),
        2 => Ok(2),
        1 => Ok(3),
        m => bail!("{prog}: ilw/ilwr at {off:#x} has a non-single-lane dest mask {m:#x}"),
    }
}

/// Census gate + code for the upper half: (calc lines, commit lines).
fn emit_upper(prog: &str, b: &Bundle) -> Result<(Vec<String>, Vec<String>)> {
    let off = b.offset;
    match b.upper {
        UpperOp::Nop => Ok((vec![], vec![])),
        UpperOp::Fmac { op, dest, fd, fs, rhs } => {
            let (ft, sel) = rhs_sel(rhs);
            let mask = dest.0;
            // Allowlist at (family, source kind) granularity, measured on
            // the five retail programs.
            let kind = match (op, rhs) {
                (FmacOp::Add, _) => 0,
                (FmacOp::Sub, Rhs::Ft(_)) | (FmacOp::Sub, Rhs::Bc(..)) => 1,
                (FmacOp::Mul, Rhs::Ft(_)) | (FmacOp::Mul, Rhs::Bc(..)) | (FmacOp::Mul, Rhs::Q) => 2,
                (FmacOp::Madd, Rhs::Bc(..)) | (FmacOp::Madd, Rhs::I) => 3,
                (FmacOp::Max, Rhs::Bc(..)) => {
                    return Ok((
                        vec![format!(
                            "rc_u128 ur = rc_vu1_maxmin_calc(vu, 0, {}, {ft}, {sel});",
                            fs.0
                        )],
                        vec![format!("rc_vu1_write(vu, {}, 0x{mask:X}, ur);", fd.0)],
                    ));
                }
                (FmacOp::Mini, Rhs::I) => {
                    return Ok((
                        vec![format!(
                            "rc_u128 ur = rc_vu1_maxmin_calc(vu, 1, {}, {ft}, {sel});",
                            fs.0
                        )],
                        vec![format!("rc_vu1_write(vu, {}, 0x{mask:X}, ur);", fd.0)],
                    ));
                }
                _ => bail!("{prog}: upper {op:?}/{rhs:?} at {off:#x} not in the measured census"),
            };
            Ok((
                vec![
                    "uint32_t um = 0;".to_string(),
                    format!(
                        "rc_u128 ur = rc_vu1_fmac_calc(vu, {kind}, {}, {ft}, {sel}, 0x{mask:X}, &um);",
                        fs.0
                    ),
                ],
                vec![format!("rc_vu1_commit_vf(vu, {}, 0x{mask:X}, ur, um);", fd.0)],
            ))
        }
        UpperOp::FmacA { op, dest, fs, rhs } => {
            let (ft, sel) = rhs_sel(rhs);
            let mask = dest.0;
            let kind = match (op, rhs) {
                (FmacOp::Mul, Rhs::Bc(..)) | (FmacOp::Mul, Rhs::I) => 2,
                (FmacOp::Madd, Rhs::Bc(..)) => 3,
                _ => {
                    bail!("{prog}: upper acc {op:?}/{rhs:?} at {off:#x} not in the measured census")
                }
            };
            Ok((
                vec![
                    "uint32_t um = 0;".to_string(),
                    format!(
                        "rc_u128 ur = rc_vu1_fmac_calc(vu, {kind}, {}, {ft}, {sel}, 0x{mask:X}, &um);",
                        fs.0
                    ),
                ],
                vec![format!("rc_vu1_commit_acc(vu, 0x{mask:X}, ur, um);")],
            ))
        }
        UpperOp::Ftoi { fixed, dest, ft, fs } => Ok((
            vec![format!(
                "rc_u128 ur = rc_vu1_ftoi_calc(vu, {}, {});",
                fixed_shift(fixed),
                fs.0
            )],
            vec![format!("rc_vu1_write(vu, {}, 0x{:X}, ur);", ft.0, dest.0)],
        )),
        UpperOp::Itof { fixed, dest, ft, fs } => Ok((
            vec![format!(
                "rc_u128 ur = rc_vu1_itof_calc(vu, {}, {});",
                fixed_shift(fixed),
                fs.0
            )],
            vec![format!("rc_vu1_write(vu, {}, 0x{:X}, ur);", ft.0, dest.0)],
        )),
        UpperOp::Abs { dest, ft, fs } => Ok((
            vec![format!("rc_u128 ur = rc_vu1_abs_calc(vu, {});", fs.0)],
            vec![format!("rc_vu1_write(vu, {}, 0x{:X}, ur);", ft.0, dest.0)],
        )),
        UpperOp::Clip { fs, ft } => Ok((
            vec![format!("uint32_t uc = rc_vu1_clip_calc(vu, {}, {});", fs.0, ft.0)],
            vec!["vu->clip = uc;".to_string()],
        )),
        UpperOp::Opmula { .. } | UpperOp::Opmsub { .. } => {
            bail!("{prog}: outer-product op at {off:#x} not in the measured census")
        }
        UpperOp::Invalid { raw } => bail!("{prog}: invalid upper {raw:#010x} at {off:#x}"),
    }
}

/// Address expression for a vi +/- signed quadword offset.
fn addr_expr(is: u8, imm11: i16) -> String {
    if imm11 == 0 {
        format!("rc_vu1_vi(vu, {is})")
    } else {
        format!("(uint32_t)((int32_t)rc_vu1_vi(vu, {is}) + {imm11})")
    }
}

/// vi read for a branch condition, honoring an old-value site.
fn cond_vi(off: u32, reg: u8, old: &[u8]) -> String {
    if old.contains(&reg) {
        format!("ov_{off:04X}_{reg}")
    } else {
        format!("rc_vu1_vi(vu, {reg})")
    }
}

/// Census gate + code for the lower half.
fn emit_lower(prog: &str, b: &Bundle, old: &[u8]) -> Result<(Vec<String>, Ctl)> {
    let off = b.offset;
    let op = match b.lower {
        LowerSlot::Loi(_) => return Ok((vec![], Ctl::None)), // handled by the caller
        LowerSlot::Inst(op) => op,
    };
    use LowerOp::*;
    let one = |s: String| Ok((vec![s], Ctl::None));
    match op {
        Nop => Ok((vec![], Ctl::None)),
        Move { dest, ft, fs } => {
            if dest.is_empty() {
                Ok((vec![], Ctl::None)) // canonical lower nop encoding
            } else {
                one(format!("rc_vu1_move(vu, {}, {}, 0x{:X});", ft.0, fs.0, dest.0))
            }
        }
        Mr32 { dest, ft, fs } => {
            one(format!("rc_vu1_mr32(vu, {}, {}, 0x{:X});", ft.0, fs.0, dest.0))
        }
        Lq { dest, ft, is, imm11 } => one(format!(
            "rc_vu1_write(vu, {}, 0x{:X}, rc_vu1_lq(vu, {}));",
            ft.0,
            dest.0,
            addr_expr(is.0, imm11)
        )),
        Sq { dest, fs, it, imm11 } => one(format!(
            "rc_vu1_sq(vu, {}, 0x{:X}, {});",
            fs.0,
            dest.0,
            addr_expr(it.0, imm11)
        )),
        Lqi { dest, ft, is } => Ok((
            vec![
                format!(
                    "rc_vu1_write(vu, {}, 0x{:X}, rc_vu1_lq(vu, rc_vu1_vi(vu, {})));",
                    ft.0, dest.0, is.0
                ),
                format!("rc_vu1_viset(vu, {0}, rc_vu1_vi(vu, {0}) + 1u);", is.0),
            ],
            Ctl::None,
        )),
        Sqi { dest, fs, it } => Ok((
            vec![
                format!(
                    "rc_vu1_sq(vu, {}, 0x{:X}, rc_vu1_vi(vu, {}));",
                    fs.0, dest.0, it.0
                ),
                format!("rc_vu1_viset(vu, {0}, rc_vu1_vi(vu, {0}) + 1u);", it.0),
            ],
            Ctl::None,
        )),
        Sqd { dest, fs, it } => Ok((
            vec![
                format!("rc_vu1_viset(vu, {0}, rc_vu1_vi(vu, {0}) - 1u);", it.0),
                format!(
                    "rc_vu1_sq(vu, {}, 0x{:X}, rc_vu1_vi(vu, {}));",
                    fs.0, dest.0, it.0
                ),
            ],
            Ctl::None,
        )),
        Ilwr { dest, it, is } => {
            let lane = single_lane(prog, off, dest)?;
            one(format!(
                "rc_vu1_viset(vu, {}, rc_vu1_ilw(vu, rc_vu1_vi(vu, {}), {lane}));",
                it.0, is.0
            ))
        }
        Iswr { dest, it, is } => one(format!(
            "rc_vu1_isw(vu, rc_vu1_vi(vu, {}), 0x{:X}, rc_vu1_vi(vu, {}));",
            is.0, dest.0, it.0
        )),
        Isw { dest, it, is, imm11 } => one(format!(
            "rc_vu1_isw(vu, {}, 0x{:X}, rc_vu1_vi(vu, {}));",
            addr_expr(is.0, imm11),
            dest.0,
            it.0
        )),
        Iadd { id, is, it } => one(format!(
            "rc_vu1_viset(vu, {}, rc_vu1_vi(vu, {}) + rc_vu1_vi(vu, {}));",
            id.0, is.0, it.0
        )),
        Iand { id, is, it } => one(format!(
            "rc_vu1_viset(vu, {}, rc_vu1_vi(vu, {}) & rc_vu1_vi(vu, {}));",
            id.0, is.0, it.0
        )),
        Iaddi { it, is, imm5 } => one(format!(
            "rc_vu1_viset(vu, {}, (uint32_t)((int32_t)rc_vu1_vi(vu, {}) + {imm5}));",
            it.0, is.0
        )),
        Iaddiu { it, is, imm15 } => one(format!(
            "rc_vu1_viset(vu, {}, rc_vu1_vi(vu, {}) + 0x{imm15:X}u);",
            it.0, is.0
        )),
        Isubiu { it, is, imm15 } => one(format!(
            "rc_vu1_viset(vu, {}, rc_vu1_vi(vu, {}) - 0x{imm15:X}u);",
            it.0, is.0
        )),
        Mfir { dest, ft, is } => one(format!(
            "rc_vu1_mfir(vu, {}, 0x{:X}, rc_vu1_vi(vu, {}));",
            ft.0, dest.0, is.0
        )),
        Div { fs, fsf, ft, ftf } => one(format!(
            "rc_vu1_div(vu, {}, {}, {}, {});",
            fs.0,
            comp_idx(fsf),
            ft.0,
            comp_idx(ftf)
        )),
        Rsqrt { fs, fsf, ft, ftf } => one(format!(
            "rc_vu1_rsqrt(vu, {}, {}, {}, {});",
            fs.0,
            comp_idx(fsf),
            ft.0,
            comp_idx(ftf)
        )),
        Waitq => one("rc_vu1_q_commit(vu);".to_string()),
        Rinit { fs, fsf } => one(format!(
            "rc_vu1_rinit(vu, rc_vu1_vf(vu, {}).u32x[{}]);",
            fs.0,
            comp_idx(fsf)
        )),
        Rget { dest, ft } => one(format!("rc_vu1_rget(vu, {}, 0x{:X});", ft.0, dest.0)),
        Fsand { it, imm12 } => one(format!(
            "rc_vu1_viset(vu, {}, vu->status & 0x{imm12:X}u);",
            it.0
        )),
        Fmand { it, is } => one(format!(
            "rc_vu1_viset(vu, {}, vu->mac & rc_vu1_vi(vu, {}));",
            it.0, is.0
        )),
        Fcand { imm24 } => one(format!(
            "rc_vu1_viset(vu, 1, (vu->clip & 0x{imm24:X}u) != 0u ? 1u : 0u);"
        )),
        Fcor { imm24 } => one(format!(
            "rc_vu1_viset(vu, 1, ((vu->clip | 0x{imm24:X}u) & 0xFFFFFFu) == 0xFFFFFFu ? 1u : 0u);"
        )),
        Fcget { it } => one(format!("rc_vu1_viset(vu, {}, vu->clip & 0xFFFu);", it.0)),
        Xtop { it } => one(format!("rc_vu1_viset(vu, {}, vu->xtop & 0x3FFu);", it.0)),
        Xgkick { is } => one(format!(
            "rt_xgkick(vu, rc_vu1_vi(vu, {}) & 0x3FFu);",
            is.0
        )),
        B { imm11 } => Ok((vec![], Ctl::Jump { target: vu_decode::branch_target(off, imm11) })),
        Bal { it, imm11 } => Ok((
            // Link register: instruction address (units of 8 bytes) of the
            // bundle after the delay slot.
            vec![format!("rc_vu1_viset(vu, {}, 0x{:X}u);", it.0, off / 8 + 2)],
            Ctl::Jump { target: vu_decode::branch_target(off, imm11) },
        )),
        Jr { is } => Ok((
            vec![format!("pc = {} << 3;", cond_vi(off, is.0, old))],
            Ctl::Indirect,
        )),
        Ibeq { it, is, imm11 } => Ok((
            vec![format!(
                "bc = {} == {};",
                cond_vi(off, it.0, old),
                cond_vi(off, is.0, old)
            )],
            Ctl::Cond { target: vu_decode::branch_target(off, imm11) },
        )),
        Ibne { it, is, imm11 } => Ok((
            vec![format!(
                "bc = {} != {};",
                cond_vi(off, it.0, old),
                cond_vi(off, is.0, old)
            )],
            Ctl::Cond { target: vu_decode::branch_target(off, imm11) },
        )),
        Ibgtz { is, imm11 } => Ok((
            vec![format!("bc = (int16_t){} > 0;", cond_vi(off, is.0, old))],
            Ctl::Cond { target: vu_decode::branch_target(off, imm11) },
        )),
        other => bail!("{prog}: lower {other:?} at {off:#x} not in the measured census"),
    }
}

/// Comment text for a bundle: the compat disassembly when available, raw
/// words otherwise.
fn bundle_comment(b: &Bundle) -> String {
    let upper = compat::upper_compat(b).unwrap_or_else(|| format!(".word {:#010x}", b.upper_raw));
    let lower = match b.lower {
        LowerSlot::Loi(bits) => format!("loi {:#010x} ({})", bits, f32::from_bits(bits)),
        LowerSlot::Inst(_) => {
            compat::lower_compat(b).unwrap_or_else(|| format!(".word {:#010x}", b.lower_raw))
        }
    };
    format!("{:#06x}:  {upper}  |  {lower}", b.offset)
}

/// All statements of one bundle in execution order (no wrapping braces).
fn bundle_lines(fname: &str, a: &Analysis, i: usize) -> Result<(Vec<String>, Ctl)> {
    let b = &a.bundles[i];
    let mut lines = vec![format!("/* {} */", bundle_comment(b))];
    if a.q_commit_sites.contains(&b.offset) {
        lines.push("rc_vu1_q_commit(vu); /* audited: div latency has elapsed here */".to_string());
    }
    if let LowerSlot::Loi(bits) = b.lower {
        lines.push(format!("vu->i = rc_bits2f(0x{bits:08X}u);"));
    }
    // Save pre-write vi values for an old-value branch in the NEXT bundle.
    if let Some(regs) = a.old_vi_sites.get(&(b.offset + 8)) {
        for &r in regs {
            lines.push(format!(
                "ov_{:04X}_{r} = rc_vu1_vi(vu, {r}); /* integer-branch hazard */",
                b.offset + 8
            ));
        }
    }
    let (calc, commit) = emit_upper(fname, b)?;
    let old: &[u8] = a.old_vi_sites.get(&b.offset).map(Vec::as_slice).unwrap_or(&[]);
    let (lower, ctl) = emit_lower(fname, b, old)?;
    lines.extend(calc);
    lines.extend(lower);
    lines.extend(commit);
    Ok((lines, ctl))
}

fn push_block(out: &mut String, indent: &str, lines: &[String]) {
    let _ = writeln!(out, "{indent}{{");
    for l in lines {
        let _ = writeln!(out, "{indent}    {l}");
    }
    let _ = writeln!(out, "{indent}}}");
}

pub fn emit_program(prog: &Vu1Program, a: &Analysis) -> Result<(String, EmitStats)> {
    let name = &prog.name;
    let fname = format!("vu1_{name}");
    let n = a.bundles.len();
    let has_jr = a
        .bundles
        .iter()
        .any(|b| matches!(b.lower, LowerSlot::Inst(LowerOp::Jr { .. })));

    for &d in &a.dispatch {
        if d as usize >= n * 8 {
            bail!("{name}: dispatch offset {d:#x} is outside the program");
        }
    }

    let mut out = String::new();
    let _ = writeln!(
        out,
        "/* {fname}.c: generated by `icorecomp vu1`. DO NOT EDIT, DO NOT COMMIT.\n\
         * Source: .vutext fragment at vram {:#010x} ({} uploaded instructions,\n\
         * upload hash {:#010x} = rc_vu1_hash over the MPG payload bytes).\n\
         * Semantics model: see the VU1 section of include/recomp_ops.h. */\n\
         #include \"recomp_ops.h\"\n",
        prog.vram,
        prog.instruction_count(),
        prog.hash()
    );
    let _ = writeln!(out, "void {fname}(Vu1State* vu);\n");
    let _ = writeln!(out, "void {fname}(Vu1State* vu) {{");
    let _ = writeln!(out, "    uint32_t pc = vu->pc;");
    for (&off, regs) in &a.old_vi_sites {
        for &r in regs {
            let _ = writeln!(out, "    uint32_t ov_{off:04X}_{r} = 0;");
        }
    }
    if has_jr {
        let _ = writeln!(out, "dispatch:");
    }
    let _ = writeln!(out, "    switch (pc) {{");
    for &d in &a.dispatch {
        let _ = writeln!(out, "    case 0x{d:X}u: goto L_{d:04X};");
    }
    let _ = writeln!(out, "    default:");
    let _ = writeln!(out, "        vu->pc = pc;");
    let _ = writeln!(out, "        rt_unimplemented(\"{fname}: pc is not a known entry\", pc);");
    let _ = writeln!(out, "        return;");
    let _ = writeln!(out, "    }}");

    let mut consumed = vec![false; n];
    let mut stats = EmitStats { emitted: 0, duplicated: 0 };
    let needs_label = |off: u32| a.labels.contains(&off) || a.dispatch.contains(&off);

    for i in 0..n {
        if consumed[i] {
            continue;
        }
        let off = a.bundles[i].offset;
        if needs_label(off) {
            let _ = writeln!(out, "L_{off:04X}:");
        }
        let (lines, ctl) = bundle_lines(&fname, a, i)?;
        match ctl {
            Ctl::None => {
                push_block(&mut out, "    ", &lines);
                consumed[i] = true;
                stats.emitted += 1;
                if a.bundles[i].flags.e {
                    // E bit: run the delay bundle, then stop; MSCNT resumes
                    // at the bundle after it.
                    let s = i + 1;
                    let (slines, sctl) = bundle_lines(&fname, a, s)?;
                    if !matches!(sctl, Ctl::None) {
                        bail!("{fname}: control flow in the E-bit delay slot at {:#x}", off + 8);
                    }
                    let soff = a.bundles[s].offset;
                    if needs_label(soff) {
                        // The slot doubles as a jump target: keep a labeled
                        // linear copy and inline a duplicate for the E path.
                        push_block(&mut out, "    ", &slines);
                        stats.duplicated += 1;
                    } else {
                        push_block(&mut out, "    ", &slines);
                        consumed[s] = true;
                        stats.emitted += 1;
                    }
                    let _ = writeln!(out, "    vu->pc = 0x{:X}u;", soff + 8);
                    let _ = writeln!(out, "    return;");
                }
            }
            branch_ctl => {
                let s = i + 1; // analyze() guarantees a plain, unlabeled slot
                let (slines, sctl) = bundle_lines(&fname, a, s)?;
                if !matches!(sctl, Ctl::None) {
                    bail!("{fname}: branch in a delay slot at {:#x}", off + 8);
                }
                let _ = writeln!(out, "    {{");
                if matches!(branch_ctl, Ctl::Cond { .. }) {
                    let _ = writeln!(out, "        int bc;");
                }
                push_block(&mut out, "        ", &lines);
                push_block(&mut out, "        ", &slines);
                match branch_ctl {
                    Ctl::Cond { target } => {
                        let _ = writeln!(out, "        if (bc) goto L_{target:04X};");
                    }
                    Ctl::Jump { target } => {
                        let _ = writeln!(out, "        goto L_{target:04X};");
                    }
                    Ctl::Indirect => {
                        let _ = writeln!(out, "        goto dispatch;");
                    }
                    Ctl::None => unreachable!(),
                }
                let _ = writeln!(out, "    }}");
                consumed[i] = true;
                consumed[s] = true;
                stats.emitted += 2;
            }
        }
    }

    // Coverage assertion: every uploaded bundle was emitted exactly once at
    // its linear position.
    if stats.emitted != n {
        bail!(
            "{fname}: coverage assertion failed: {} of {n} bundles emitted",
            stats.emitted
        );
    }

    let _ = writeln!(out, "    vu->pc = 0x{:X}u;", n * 8);
    let _ = writeln!(
        out,
        "    rt_unimplemented(\"{fname}: execution ran off the end of micro memory\", 0x{:X}u);",
        n * 8
    );
    let _ = writeln!(out, "}}");
    Ok((out, stats))
}
