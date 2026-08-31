//! COP2 (VU0 macro mode) classification: one structured form per censused
//! macro instruction, derived from the decoded `Insn`. Shared by the C
//! emitter (`ops.rs`) and the reference interpreter (`ee-interp`) so both
//! sides drive the same `rc_vu_*` helpers with the same arguments.
//!
//! Coverage policy: `parse` errors on any COP2 shape outside the measured
//! census of this one binary (family plus source kind granularity; dest
//! masks and broadcast lanes are generic). The emitter turns that error
//! into the whole-run hard error, same as an unknown integer mnemonic.

use anyhow::{bail, Result};
use r5900_decode::{Insn, Operand};

/// Third-operand source of an FMAC-family op.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Sel {
    /// Broadcast of one lane of vft (0 = x .. 3 = w).
    Lane(u8),
    /// Full vector vft.
    Vec,
    /// Q register broadcast.
    Q,
}

impl Sel {
    /// Encoding used by the `rc_vu_*` helpers (RC_VU_SRC_*).
    pub fn code(self) -> u8 {
        match self {
            Sel::Lane(l) => l,
            Sel::Vec => 4,
            Sel::Q => 5,
        }
    }
}

/// FMAC family with a vf destination.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Fmac {
    Add,
    Sub,
    Mul,
    Madd,
    Msub,
    Max,
    Mini,
}

/// FMAC family with the ACC destination.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AccFmac {
    Adda,
    Mula,
    Madda,
}

/// One classified COP2 macro instruction.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Cop2 {
    Lqc2 { ft: u8, offset: i32, base: u8 },
    Sqc2 { fs: u8, offset: i32, base: u8 },
    Qmfc2 { rt: u8, fs: u8 },
    Qmtc2 { rt: u8, fd: u8 },
    Cfc2 { rt: u8, creg: u8 },
    Ctc2 { rt: u8, creg: u8 },
    Fmac { family: Fmac, fd: u8, fs: u8, ft: u8, sel: Sel, mask: u8 },
    FmacAcc { family: AccFmac, fs: u8, ft: u8, sel: Sel, mask: u8 },
    Opmula { fs: u8, ft: u8 },
    Opmsub { fd: u8, fs: u8, ft: u8 },
    Ftoi { shift: u8, ft: u8, fs: u8, mask: u8 },
    Itof { shift: u8, ft: u8, fs: u8, mask: u8 },
    Move { ft: u8, fs: u8, mask: u8 },
    Mr32 { ft: u8, fs: u8, mask: u8 },
    Div { fs: u8, fsf: u8, ft: u8, ftf: u8 },
    Sqrt { ft: u8, ftf: u8 },
    Rsqrt { fs: u8, fsf: u8, ft: u8, ftf: u8 },
    Clipw { fs: u8, ft: u8 },
    Lqi { ft: u8, is: u8, mask: u8 },
    Lqd { ft: u8, is: u8, mask: u8 },
    Sqi { fs: u8, it: u8, mask: u8 },
    Iaddi { it: u8, is: u8, imm: i32 },
    Rnext { ft: u8, mask: u8 },
    Rinit { fs: u8, fsf: u8 },
    Rxor { fs: u8, fsf: u8 },
    Waitq,
    Nop,
}

/// True for every mnemonic the COP2 path owns (valid or not).
pub fn is_cop2(m: &str) -> bool {
    m.starts_with('v')
        || m.starts_with("qmfc2")
        || m.starts_with("qmtc2")
        || m.starts_with("cfc2")
        || m.starts_with("ctc2")
        || m == "lqc2"
        || m == "sqc2"
}

fn lane(c: char) -> Result<u8> {
    Ok(match c {
        'x' => 0,
        'y' => 1,
        'z' => 2,
        'w' => 3,
        _ => bail!("bad component char {c:?}"),
    })
}

fn mask_of(s: &str) -> Result<u8> {
    let mut m = 0u8;
    for c in s.chars() {
        m |= 8 >> lane(c)?;
    }
    Ok(m)
}

fn vf(ops: &[Operand], i: usize) -> Result<u8> {
    match ops.get(i) {
        Some(Operand::Vf(n)) | Some(Operand::VfComp(n, _)) => Ok(*n),
        other => bail!("expected vf operand, got {other:?}"),
    }
}

fn gpr(ops: &[Operand], i: usize) -> Result<u8> {
    match ops.get(i) {
        Some(Operand::Gpr(n)) => Ok(*n),
        other => bail!("expected gpr operand, got {other:?}"),
    }
}

fn comp(ops: &[Operand], i: usize) -> Result<u8> {
    match ops.get(i) {
        Some(Operand::VfComp(_, c)) => lane(*c),
        other => bail!("expected vf component operand, got {other:?}"),
    }
}

fn mem(ops: &[Operand]) -> Result<(i32, u8)> {
    for op in ops {
        if let Operand::Mem { offset, base } = op {
            return Ok((*offset, *base));
        }
    }
    bail!("expected mem operand")
}

/// Split `base.mask` (mask may be absent, e.g. `vdiv`, `vnop`).
fn split_mask(m: &str) -> Result<(&str, u8)> {
    match m.split_once('.') {
        Some((b, s)) => Ok((b, mask_of(s)?)),
        None => Ok((m, 0)),
    }
}

/// Classify one COP2 macro instruction. Errors on anything outside the
/// measured census (the caller routes that into the hard-error path).
pub fn parse(insn: &Insn) -> Result<Cop2> {
    let m = insn
        .mnemonic()
        .ok_or_else(|| anyhow::anyhow!("invalid word reached cop2::parse"))?;
    let ops = insn.operands();

    // Transfer ops. Only the non-interlocked forms are censused.
    if let Some(base) = m.strip_suffix(".ni") {
        return Ok(match base {
            "qmfc2" => Cop2::Qmfc2 { rt: gpr(ops, 0)?, fs: vf(ops, 1)? },
            "qmtc2" => Cop2::Qmtc2 { rt: gpr(ops, 0)?, fd: vf(ops, 1)? },
            "cfc2" | "ctc2" => {
                let rt = gpr(ops, 0)?;
                let creg = match ops.get(1) {
                    Some(Operand::Vi(n)) => *n,
                    other => bail!("expected vi operand, got {other:?}"),
                };
                if base == "cfc2" {
                    Cop2::Cfc2 { rt, creg }
                } else {
                    Cop2::Ctc2 { rt, creg }
                }
            }
            _ => bail!("mnemonic {m} not in the measured COP2 census"),
        });
    }
    match m {
        "lqc2" => {
            let (offset, base) = mem(ops)?;
            return Ok(Cop2::Lqc2 { ft: vf(ops, 0)?, offset, base });
        }
        "sqc2" => {
            let (offset, base) = mem(ops)?;
            return Ok(Cop2::Sqc2 { fs: vf(ops, 0)?, offset, base });
        }
        "vnop" => return Ok(Cop2::Nop),
        "vwaitq" => return Ok(Cop2::Waitq),
        "vdiv" => {
            return Ok(Cop2::Div {
                fs: vf(ops, 1)?,
                fsf: comp(ops, 1)?,
                ft: vf(ops, 2)?,
                ftf: comp(ops, 2)?,
            })
        }
        "vsqrt" => {
            return Ok(Cop2::Sqrt { ft: vf(ops, 1)?, ftf: comp(ops, 1)? })
        }
        "vrsqrt" => {
            return Ok(Cop2::Rsqrt {
                fs: vf(ops, 1)?,
                fsf: comp(ops, 1)?,
                ft: vf(ops, 2)?,
                ftf: comp(ops, 2)?,
            })
        }
        "viaddi" => {
            let (it, is) = match (ops.first(), ops.get(1)) {
                (Some(Operand::Vi(t)), Some(Operand::Vi(s))) => (*t, *s),
                other => bail!("bad viaddi operands {other:?}"),
            };
            let imm = match ops.get(2) {
                Some(Operand::Imm(v)) => *v,
                other => bail!("bad viaddi imm {other:?}"),
            };
            return Ok(Cop2::Iaddi { it, is, imm });
        }
        "vrinit" => {
            return Ok(Cop2::Rinit { fs: vf(ops, 1)?, fsf: comp(ops, 1)? })
        }
        "vrxor" => {
            return Ok(Cop2::Rxor { fs: vf(ops, 1)?, fsf: comp(ops, 1)? })
        }
        _ => {}
    }

    let (base, mask) = split_mask(m)?;
    match base {
        "vmove" => return Ok(Cop2::Move { ft: vf(ops, 0)?, fs: vf(ops, 1)?, mask }),
        "vmr32" => return Ok(Cop2::Mr32 { ft: vf(ops, 0)?, fs: vf(ops, 1)?, mask }),
        "vftoi0" | "vftoi4" | "vitof0" | "vitof4" => {
            let shift = if base.ends_with('4') { 4 } else { 0 };
            let (ft, fs) = (vf(ops, 0)?, vf(ops, 1)?);
            return Ok(if base.starts_with("vftoi") {
                Cop2::Ftoi { shift, ft, fs, mask }
            } else {
                Cop2::Itof { shift, ft, fs, mask }
            });
        }
        "vclipw" => return Ok(Cop2::Clipw { fs: vf(ops, 0)?, ft: vf(ops, 1)? }),
        "vrnext" => return Ok(Cop2::Rnext { ft: vf(ops, 0)?, mask }),
        "vlqi" | "vlqd" => {
            let ft = vf(ops, 0)?;
            let is = match ops.get(1) {
                Some(Operand::ViInc(n)) | Some(Operand::ViDec(n)) => *n,
                other => bail!("bad {base} pointer operand {other:?}"),
            };
            return Ok(if base == "vlqi" {
                Cop2::Lqi { ft, is, mask }
            } else {
                Cop2::Lqd { ft, is, mask }
            });
        }
        "vsqi" => {
            let fs = vf(ops, 0)?;
            let it = match ops.get(1) {
                Some(Operand::ViInc(n)) => *n,
                other => bail!("bad vsqi pointer operand {other:?}"),
            };
            return Ok(Cop2::Sqi { fs, it, mask });
        }
        "vopmula" => return Ok(Cop2::Opmula { fs: vf(ops, 1)?, ft: vf(ops, 2)? }),
        "vopmsub" => {
            return Ok(Cop2::Opmsub { fd: vf(ops, 0)?, fs: vf(ops, 1)?, ft: vf(ops, 2)? })
        }
        _ => {}
    }

    // FMAC families. Strip the broadcast lane or 'q' suffix off the base
    // name, guided by the third operand's shape.
    let (family_name, sel) = match ops.last() {
        Some(Operand::VfComp(_, c)) => {
            let stripped = base
                .strip_suffix(*c)
                .ok_or_else(|| anyhow::anyhow!("broadcast suffix mismatch in {m}"))?;
            (stripped, Sel::Lane(lane(*c)?))
        }
        Some(Operand::Q) => {
            let stripped = base
                .strip_suffix('q')
                .ok_or_else(|| anyhow::anyhow!("q suffix mismatch in {m}"))?;
            (stripped, Sel::Q)
        }
        Some(Operand::Vf(_)) => (base, Sel::Vec),
        other => bail!("mnemonic {m} not in the measured COP2 census ({other:?})"),
    };

    // Q broadcasts carry no vft register; the helpers ignore ft then.
    let ft = if sel == Sel::Q { 0 } else { vf(ops, ops.len() - 1)? };

    if matches!(ops.first(), Some(Operand::Acc)) {
        let family = match (family_name, sel) {
            ("vadda", Sel::Lane(_)) => AccFmac::Adda,
            ("vmula", Sel::Lane(_)) => AccFmac::Mula,
            ("vmadda", Sel::Lane(_)) => AccFmac::Madda,
            _ => bail!("mnemonic {m} not in the measured COP2 census"),
        };
        return Ok(Cop2::FmacAcc { family, fs: vf(ops, 1)?, ft, sel, mask });
    }

    // Allowlist at (family, source kind) granularity, from the census.
    let family = match (family_name, sel) {
        ("vadd", Sel::Vec) | ("vadd", Sel::Lane(_)) | ("vadd", Sel::Q) => Fmac::Add,
        ("vsub", Sel::Vec) | ("vsub", Sel::Lane(_)) => Fmac::Sub,
        ("vmul", Sel::Vec) | ("vmul", Sel::Lane(_)) | ("vmul", Sel::Q) => Fmac::Mul,
        ("vmadd", Sel::Vec) | ("vmadd", Sel::Lane(_)) => Fmac::Madd,
        ("vmsub", Sel::Lane(_)) => Fmac::Msub,
        ("vmax", Sel::Lane(_)) => Fmac::Max,
        ("vmini", Sel::Lane(_)) => Fmac::Mini,
        _ => bail!("mnemonic {m} not in the measured COP2 census"),
    };
    Ok(Cop2::Fmac { family, fd: vf(ops, 0)?, fs: vf(ops, 1)?, ft, sel, mask })
}
