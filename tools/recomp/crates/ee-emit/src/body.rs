//! Per-group body emission: basic-block labels, delay slots, calls,
//! returns, and jump-table dispatch.
//!
//! Translation model:
//!  - Every function becomes `void F_<name>(R5900Context* ctx)`. Call sites
//!    go through the per-function `CF_<name>` macro, which indexes
//!    `g_functab` so runtime overrides apply at every call site.
//!  - A "group" is normally one function. When a branch targets another
//!    function's interior, both functions are merged into one emitted body
//!    (`GB_<leader-vram>`) with a per-entry wrapper and an entry switch.
//!  - Delay slots: unconditional branch emits slot then goto; conditional
//!    evaluates the condition into a temp before the slot; likely branches
//!    put the slot inside the taken arm. If the slot address is itself a
//!    branch target, the slot is re-emitted under its label so a direct
//!    jump to the slot executes it without the branch (hardware behavior).
//!  - `jalr` dispatches through `rt_call_indirect` (runtime does the
//!    functab lookup and the bad-target diagnostics). `jr $ra` is
//!    `return`. `jr <other>` becomes a switch keyed on the *loaded target
//!    address* held in the register, over the union of all jump-table
//!    targets owned by the group, with `rt_bad_indirect` as the default.

use std::collections::{BTreeSet, HashMap};
use std::fmt::Write as _;

use anyhow::{bail, Result};
use r5900_decode::Insn;

use crate::flow::{classify, is_control, Flow};
use crate::ops::{self, ru32, OpStats};

/// One function inside a group.
pub struct Member<'a> {
    pub ident: String,
    pub vram: u32,
    /// End of the byte range, clamped to the .text section end.
    pub end: u32,
    pub insns: &'a [Insn],
}

pub struct Group<'a> {
    /// Members sorted by vram. The first is the leader.
    pub members: Vec<Member<'a>>,
    /// Union of all nonzero targets of jump tables owned by members.
    pub jtbl_targets: BTreeSet<u32>,
}

/// Lookups the emitter needs about the rest of the program.
pub struct Resolver<'a> {
    /// vram -> function index, for functions inside .text only.
    pub entry_by_vram: &'a HashMap<u32, usize>,
    /// Sanitized identifier per ProgramDb function index.
    pub fn_ident: &'a [String],
}

#[derive(Debug, Default, Clone)]
pub struct GroupStats {
    pub insns: usize,
    pub delay_slots: usize,
    pub likely_branches: usize,
    pub slot_dups: usize,
    pub direct_calls: usize,
    pub indirect_calls: usize,
    pub returns: usize,
    pub tail_calls: usize,
    pub fallthrough_calls: usize,
    pub jtbl_switches: usize,
    pub jtbl_cases: usize,
    pub ops: OpStats,
}

impl GroupStats {
    pub fn merge(&mut self, o: &GroupStats) {
        self.insns += o.insns;
        self.delay_slots += o.delay_slots;
        self.likely_branches += o.likely_branches;
        self.slot_dups += o.slot_dups;
        self.direct_calls += o.direct_calls;
        self.indirect_calls += o.indirect_calls;
        self.returns += o.returns;
        self.tail_calls += o.tail_calls;
        self.fallthrough_calls += o.fallthrough_calls;
        self.jtbl_switches += o.jtbl_switches;
        self.jtbl_cases += o.jtbl_cases;
        for (k, v) in &o.ops.unimplemented {
            *self.ops.unimplemented.entry(k.clone()).or_insert(0) += v;
        }
        for (k, v) in &o.ops.unknown {
            *self.ops.unknown.entry(k.clone()).or_insert(0) += v;
        }
        self.ops.invalid_words += o.ops.invalid_words;
    }
}

enum Dest {
    Local(u32),
    Tail(usize),
}

pub fn emit_group(g: &Group<'_>, r: &Resolver<'_>) -> Result<(String, GroupStats)> {
    let mut st = GroupStats::default();
    let mut out = String::new();

    let in_group = |t: u32| g.members.iter().any(|m| t >= m.vram && t < m.end);

    // ---- label collection --------------------------------------------------
    let mut labels: BTreeSet<u32> = BTreeSet::new();
    let mut has_jr_reg = false;
    for m in &g.members {
        for insn in m.insns {
            match classify(insn) {
                Flow::Branch { target, .. } => {
                    if in_group(target) {
                        labels.insert(target);
                    }
                }
                Flow::JrReg(_) => has_jr_reg = true,
                _ => {}
            }
        }
    }
    if has_jr_reg {
        for &t in &g.jtbl_targets {
            if !in_group(t) {
                bail!(
                    "jump table target 0x{t:X} falls outside its owning function group \
                     (leader {})",
                    g.members[0].ident
                );
            }
            labels.insert(t);
        }
    }
    let multi = g.members.len() > 1;
    if multi {
        for m in &g.members {
            labels.insert(m.vram);
        }
    }

    // ---- prologue ----------------------------------------------------------
    let leader = &g.members[0];
    if multi {
        let gb = format!("GB_{:08X}", leader.vram);
        let _ = writeln!(out, "static void {gb}(R5900Context* ctx, uint32_t entry);");
        for m in &g.members {
            let _ = writeln!(
                out,
                "void F_{}(R5900Context* ctx) {{ {gb}(ctx, 0x{:X}u); }}",
                m.ident, m.vram
            );
        }
        let _ = writeln!(out, "static void {gb}(R5900Context* ctx, uint32_t entry) {{");
        let _ = writeln!(out, "switch (entry) {{");
        for m in &g.members {
            let _ = writeln!(out, "case 0x{:X}u: goto L_{:08X};", m.vram, m.vram);
        }
        let _ = writeln!(
            out,
            "default: rt_bad_indirect(entry, 0x{:X}u); return;\n}}",
            leader.vram
        );
    } else {
        let _ = writeln!(out, "void F_{}(R5900Context* ctx) {{", leader.ident);
    }

    // ---- bodies ------------------------------------------------------------
    let mut temp = 0usize;
    for (mi, m) in g.members.iter().enumerate() {
        let _ = writeln!(out, "/* -- F_{} 0x{:X}..0x{:X} -- */", m.ident, m.vram, m.end);
        let mut reachable = true;
        let mut i = 0usize;
        let n = m.insns.len();
        while i < n {
            let insn = &m.insns[i];
            let vram = insn.vram;
            if labels.contains(&vram) {
                let _ = writeln!(out, "L_{vram:08X}: ;");
                reachable = true;
            }
            let fl = classify(insn);
            if matches!(fl, Flow::Normal) {
                let s = ops::emit_stmt(insn, &mut st.ops)?;
                if !s.is_empty() {
                    out.push_str(&s);
                    out.push('\n');
                }
                st.insns += 1;
                i += 1;
                continue;
            }

            // Control instruction: consume and validate the delay slot.
            let slot = match m.insns.get(i + 1) {
                Some(s) => s,
                None => bail!(
                    "control instruction at 0x{vram:X} in F_{} has its delay slot outside \
                     the function's byte range",
                    m.ident
                ),
            };
            if is_control(slot) {
                bail!(
                    "branch/jump in delay slot at 0x{:X} (of control op at 0x{vram:X})",
                    slot.vram
                );
            }
            let slot_vram = slot.vram;
            let slot_stmt = ops::emit_stmt(slot, &mut st.ops)?;
            let slot_labeled = labels.contains(&slot_vram);
            st.insns += 2;
            st.delay_slots += 1;

            let resolve = |t: u32| -> Result<Dest> {
                if in_group(t) {
                    Ok(Dest::Local(t))
                } else if let Some(&fi) = r.entry_by_vram.get(&t) {
                    Ok(Dest::Tail(fi))
                } else {
                    bail!(
                        "branch at 0x{vram:X} targets 0x{t:X}, which is neither inside the \
                         function group nor a known function entry"
                    )
                }
            };
            let push_slot = |out: &mut String| {
                if !slot_stmt.is_empty() {
                    out.push_str(&slot_stmt);
                    out.push('\n');
                }
            };
            // Re-emit the slot under its own label so a direct jump to the
            // slot address executes it without the branch. `skip` adds the
            // goto that keeps the normal path from running the slot twice.
            let dup_slot = |out: &mut String, st: &mut GroupStats, skip: bool| {
                if !slot_labeled {
                    return false;
                }
                st.slot_dups += 1;
                if skip {
                    let _ = writeln!(out, "goto LS_{slot_vram:08X};");
                }
                let _ = writeln!(out, "L_{slot_vram:08X}: ;");
                if !slot_stmt.is_empty() {
                    let _ = writeln!(out, "{slot_stmt}");
                }
                if skip {
                    let _ = writeln!(out, "LS_{slot_vram:08X}: ;");
                }
                true
            };

            match fl {
                Flow::Normal => unreachable!(),
                Flow::Branch {
                    cond: None,
                    target,
                    ..
                } => {
                    push_slot(&mut out);
                    match resolve(target)? {
                        Dest::Local(t) => {
                            if t <= vram {
                                let _ = writeln!(out, "rt_backedge();");
                            }
                            let _ = writeln!(out, "goto L_{t:08X};");
                        }
                        Dest::Tail(fi) => {
                            st.tail_calls += 1;
                            let _ = writeln!(out, "CF_{}(ctx);\nreturn;", r.fn_ident[fi]);
                        }
                    }
                    reachable = dup_slot(&mut out, &mut st, false);
                }
                Flow::Branch {
                    cond: Some(cond),
                    likely,
                    target,
                } => {
                    if likely {
                        st.likely_branches += 1;
                    }
                    let c = temp;
                    temp += 1;
                    // Taken backward branches call rt_backedge (liveness for
                    // RAM-only spin loops); forward branches and calls do not.
                    let arm = match resolve(target)? {
                        Dest::Local(t) if t <= vram => format!("rt_backedge(); goto L_{t:08X};"),
                        Dest::Local(t) => format!("goto L_{t:08X};"),
                        Dest::Tail(fi) => {
                            st.tail_calls += 1;
                            format!("CF_{}(ctx); return;", r.fn_ident[fi])
                        }
                    };
                    let _ = writeln!(out, "{{ const int c{c} = ({cond});");
                    if likely {
                        let _ = writeln!(out, "if (c{c}) {{");
                        push_slot(&mut out);
                        let _ = writeln!(out, "{arm}\n}} }}");
                    } else {
                        push_slot(&mut out);
                        let _ = writeln!(out, "if (c{c}) {{ {arm} }} }}");
                    }
                    dup_slot(&mut out, &mut st, true);
                }
                Flow::Jal { target } => {
                    let Some(&fi) = r.entry_by_vram.get(&target) else {
                        bail!(
                            "jal at 0x{vram:X} targets 0x{target:X}, which is not a known \
                             function entry"
                        )
                    };
                    st.direct_calls += 1;
                    // $ra is architecturally written before the delay slot
                    // executes; keep that order in case the slot reads it.
                    let _ = writeln!(out, "ctx->r[31].u64x[0] = 0x{:X}ull;", vram + 8);
                    push_slot(&mut out);
                    let _ = writeln!(out, "CF_{}(ctx);", r.fn_ident[fi]);
                    dup_slot(&mut out, &mut st, true);
                }
                Flow::Jalr { rd, rs } => {
                    st.indirect_calls += 1;
                    let a = temp;
                    temp += 1;
                    let _ = writeln!(out, "{{ const uint32_t a{a} = {};", ru32(rs));
                    if rd != 0 {
                        let _ = writeln!(out, "ctx->r[{rd}].u64x[0] = 0x{:X}ull;", vram + 8);
                    }
                    push_slot(&mut out);
                    let _ = writeln!(out, "rt_call_indirect(ctx, a{a}, 0x{vram:X}u); }}");
                    dup_slot(&mut out, &mut st, true);
                }
                Flow::JrRa => {
                    st.returns += 1;
                    push_slot(&mut out);
                    let _ = writeln!(out, "return;");
                    reachable = dup_slot(&mut out, &mut st, false);
                }
                Flow::JrReg(reg) => {
                    if g.jtbl_targets.is_empty() {
                        bail!(
                            "jr ${reg} at 0x{vram:X} in F_{} has no jump table owned by \
                             this function group",
                            m.ident
                        );
                    }
                    st.jtbl_switches += 1;
                    st.jtbl_cases += g.jtbl_targets.len();
                    let a = temp;
                    temp += 1;
                    let _ = writeln!(out, "{{ const uint32_t a{a} = {};", ru32(reg));
                    push_slot(&mut out);
                    let _ = writeln!(out, "switch (a{a}) {{");
                    for &t in &g.jtbl_targets {
                        let _ = writeln!(out, "case 0x{t:X}u: goto L_{t:08X};");
                    }
                    let _ = writeln!(out, "default: rt_bad_indirect(a{a}, 0x{vram:X}u); return;");
                    let _ = writeln!(out, "}} }}");
                    reachable = dup_slot(&mut out, &mut st, false);
                }
            }
            i += 2;
        }

        // ---- member end: handle fallthrough off the byte range ------------
        if reachable {
            let next = m.end;
            let next_member_adjacent = g
                .members
                .get(mi + 1)
                .is_some_and(|nm| nm.vram == next);
            if next_member_adjacent {
                // Natural fallthrough into the next member's code.
            } else if in_group(next) {
                bail!(
                    "internal error: fallthrough at 0x{next:X} lands inside the group but \
                     not at the next emitted member"
                );
            } else if let Some(&fi) = r.entry_by_vram.get(&next) {
                st.fallthrough_calls += 1;
                let _ = writeln!(
                    out,
                    "CF_{}(ctx);\nreturn; /* fallthrough into adjacent function */",
                    r.fn_ident[fi]
                );
            } else {
                let _ = writeln!(
                    out,
                    "rt_unimplemented(\"fallthrough-off-end\", 0x{next:X}u);\nreturn;"
                );
            }
        }
    }

    out.push_str("}\n");
    Ok((out, st))
}
