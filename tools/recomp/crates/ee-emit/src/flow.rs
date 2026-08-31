//! Control-flow classification of decoded instructions, shared by the
//! grouping pass and the body emitter.

use r5900_decode::{Insn, Operand};

/// How one instruction affects control flow.
#[derive(Debug, Clone)]
pub enum Flow {
    /// Straight-line instruction; emitted by `ops::emit_stmt`.
    Normal,
    /// Direct branch or jump. `cond` is a C boolean expression evaluated
    /// before the delay slot; `None` means unconditional (b, j).
    Branch {
        cond: Option<String>,
        likely: bool,
        target: u32,
    },
    /// jal: direct call through the function table.
    Jal { target: u32 },
    /// jr $ra: function return.
    JrRa,
    /// jr with a non-$ra register: jump table dispatch.
    JrReg(u8),
    /// jalr: indirect call. `rd` receives the return address.
    Jalr { rd: u8, rs: u8 },
}

fn gpr(ops: &[Operand], i: usize) -> u8 {
    match ops.get(i) {
        Some(Operand::Gpr(n)) => *n,
        _ => unreachable!("decoder operand shape"),
    }
}

fn target(ops: &[Operand]) -> u32 {
    for op in ops {
        if let Operand::Target(t) = op {
            return *t;
        }
    }
    unreachable!("branch without target operand")
}

fn ru64(n: u8) -> String {
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

/// Classify an instruction. Returns `Flow::Normal` for everything that is
/// not a branch/jump (including invalid words, which the op emitter traps).
pub fn classify(insn: &Insn) -> Flow {
    let m = match insn.mnemonic() {
        Some(m) => m,
        None => return Flow::Normal,
    };
    let ops = insn.operands();
    match m {
        "b" | "j" => Flow::Branch {
            cond: None,
            likely: false,
            target: target(ops),
        },
        "jal" => Flow::Jal {
            target: target(ops),
        },
        "jr" => {
            let rs = gpr(ops, 0);
            if rs == 31 {
                Flow::JrRa
            } else {
                Flow::JrReg(rs)
            }
        }
        "jalr" => {
            if ops.len() == 1 {
                Flow::Jalr {
                    rd: 31,
                    rs: gpr(ops, 0),
                }
            } else {
                Flow::Jalr {
                    rd: gpr(ops, 0),
                    rs: gpr(ops, 1),
                }
            }
        }
        "beq" | "beql" => Flow::Branch {
            cond: Some(format!("{} == {}", ru64(gpr(ops, 0)), ru64(gpr(ops, 1)))),
            likely: m == "beql",
            target: target(ops),
        },
        "bne" | "bnel" => Flow::Branch {
            cond: Some(format!("{} != {}", ru64(gpr(ops, 0)), ru64(gpr(ops, 1)))),
            likely: m == "bnel",
            target: target(ops),
        },
        "beqz" => Flow::Branch {
            cond: Some(format!("{} == 0", ru64(gpr(ops, 0)))),
            likely: false,
            target: target(ops),
        },
        "bnez" => Flow::Branch {
            cond: Some(format!("{} != 0", ru64(gpr(ops, 0)))),
            likely: false,
            target: target(ops),
        },
        "blez" | "blezl" => Flow::Branch {
            cond: Some(format!("{} <= 0", rs64(gpr(ops, 0)))),
            likely: m == "blezl",
            target: target(ops),
        },
        "bgtz" | "bgtzl" => Flow::Branch {
            cond: Some(format!("{} > 0", rs64(gpr(ops, 0)))),
            likely: m == "bgtzl",
            target: target(ops),
        },
        "bltz" | "bltzl" => Flow::Branch {
            cond: Some(format!("{} < 0", rs64(gpr(ops, 0)))),
            likely: m == "bltzl",
            target: target(ops),
        },
        "bgez" | "bgezl" => Flow::Branch {
            cond: Some(format!("{} >= 0", rs64(gpr(ops, 0)))),
            likely: m == "bgezl",
            target: target(ops),
        },
        // COP1 condition branches read FCR31 bit 23, which is integer state
        // in R5900Context; the branch itself is fully implemented even
        // though the compare ops that set the bit are P1-unimplemented.
        "bc1f" | "bc1fl" => Flow::Branch {
            cond: Some("(ctx->fcr31 & 0x00800000u) == 0".into()),
            likely: m == "bc1fl",
            target: target(ops),
        },
        "bc1t" | "bc1tl" => Flow::Branch {
            cond: Some("(ctx->fcr31 & 0x00800000u) != 0".into()),
            likely: m == "bc1tl",
            target: target(ops),
        },
        _ => Flow::Normal,
    }
}

/// True if the instruction may transfer control (illegal in a delay slot).
pub fn is_control(insn: &Insn) -> bool {
    !matches!(classify(insn), Flow::Normal)
}
