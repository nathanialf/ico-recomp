//! Static analysis of one uploaded microprogram: entry points, labels,
//! control-flow checks, and the latency audit.
//!
//! Pipeline model facts the audit rests on (EE User's Manual VU chapters,
//! cross-checked against PCSX2's microVU as a behavioral reference, no code
//! reused):
//!
//! * vf data hazards are interlocked. An instruction whose source vf is
//!   still in a preceding FMAC's 4-cycle pipeline STALLS until writeback;
//!   it never observes the old value. Committing FMAC results immediately
//!   is therefore semantically exact; the audit reports these sites as
//!   stall (performance) sites only.
//! * The two halves of one bundle issue in the same cycle and read
//!   pre-bundle state. The emitter computes the upper half into locals,
//!   runs the lower half, then commits (see recomp_ops.h).
//! * Q is NOT interlocked. div/sqrt take 7 cycles, rsqrt 13; a Q read
//!   inside the window sees the old Q. Modeled with pending_q, committed
//!   at waitq, at the next div-unit issue, and at Q-read sites whose
//!   audited distance from the issue is at least the latency.
//! * Integer results feeding a conditional branch or jr in the very next
//!   bundle are NOT interlocked: the branch condition sees the old vi
//!   value (VU manual's integer-branch hazard). Modeled exactly at the
//!   audited sites with a saved pre-write temp.
//! * MAC/status/clip flag reads lag the FMAC that set them by roughly 4
//!   cycles and are NOT interlocked. This pass reports every read inside
//!   the window; the current emitter commits flags immediately (see the
//!   report and the open-questions list before trusting flag-driven
//!   culling paths).

use std::collections::{BTreeMap, BTreeSet};

use anyhow::{bail, Result};
use vu_decode::{
    branch_target, decode_program, lower::LowerOp, upper::Rhs, upper::UpperOp, Bundle, LowerSlot,
};

use crate::layout::Vu1Program;

const FMAC_WINDOW: u32 = 3; // bundles after a write in the 4-cycle pipeline
const FLAG_WINDOW: u32 = 3;
const DIV_LATENCY: u32 = 7; // div and sqrt
const RSQRT_LATENCY: u32 = 13;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DivKind {
    Div,
    Sqrt,
    Rsqrt,
    Waitq,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlagKind {
    Status,
    Mac,
    Clip,
}

/// What the audit found, as printable lines grouped by category.
#[derive(Default)]
pub struct Audit {
    /// vf written by an upper FMAC and read again within 3 bundles.
    /// Hardware interlocks (stalls); immediate commit is exact. Reported
    /// for the record.
    pub fmac_stalls: Vec<String>,
    /// Same-bundle upper/lower exchanges, handled by compute-then-commit.
    pub same_bundle: Vec<String>,
    /// Q pipeline events: every issue with its first consumer.
    pub q_events: Vec<String>,
    /// Conditional branches reading a vi written by the immediately
    /// preceding bundle. Old-value semantics implemented via temps.
    pub int_branch: Vec<String>,
    /// Integer loads whose vi is consumed in the next bundle (report only).
    pub ilw_next: Vec<String>,
    /// Flag reads within the (unmodeled) flag pipeline window.
    pub flag_windows: Vec<String>,
    /// Oddities that are accepted (D bit set on retail bundles).
    pub notes: Vec<String>,
}

pub struct Analysis {
    /// Bundle offset -> the flag-read sites whose visible flags must be
    /// snapshotted at the top of that bundle. The VU flag pipeline is four
    /// deep: a lower-slot fmand/fsand at bundle i sees the flags committed
    /// at bundle i-4, so the snapshot is taken entering bundle i-3.
    pub flag_save_sites: BTreeMap<u32, Vec<(u32, bool)>>,
    /// Bundle offset of a flag read -> true if it reads MAC, false status.
    pub flag_read_sites: BTreeMap<u32, bool>,
    pub bundles: Vec<Bundle>,
    /// MSCAL entry offsets (bytes): offset 0 plus the head branch stubs.
    pub entries: Vec<u32>,
    /// Static branch targets (bytes).
    pub labels: BTreeSet<u32>,
    /// PC-dispatch cases: entries, bal return points, offset 0.
    pub dispatch: BTreeSet<u32>,
    /// Bundle offsets that must commit pending Q before executing.
    pub q_commit_sites: BTreeSet<u32>,
    /// Branch bundle offset -> vi registers that must use the pre-write
    /// value saved in the preceding bundle.
    pub old_vi_sites: BTreeMap<u32, Vec<u8>>,
    pub audit: Audit,
}

#[derive(Default, Clone)]
struct UpperInfo {
    writes_vf: Option<(u8, u8)>, // (reg, mask)
    writes_acc: bool,
    writes_clip: bool,
    sets_macstatus: bool,
    reads_vf: Vec<u8>,
    reads_q: bool,
}

#[derive(Default, Clone)]
struct LowerInfo {
    reads_vf: Vec<u8>,
    writes_vf: Option<u8>,
    reads_vi: Vec<u8>,
    writes_vi: Option<u8>,
    is_iload: bool,
    div: Option<DivKind>,
    reads_flags: Option<FlagKind>,
    /// (condition/address source vis, static target if direct).
    branch: Option<(Vec<u8>, Option<u32>)>,
    is_bal: bool,
}

fn rhs_parts(rhs: Rhs) -> (Option<u8>, bool) {
    match rhs {
        Rhs::Ft(ft) | Rhs::Bc(ft, _) => (Some(ft.0), false),
        Rhs::Q => (None, true),
        Rhs::I => (None, false),
    }
}

fn upper_info(b: &Bundle) -> UpperInfo {
    let mut u = UpperInfo::default();
    match b.upper {
        UpperOp::Nop | UpperOp::Invalid { .. } => {}
        UpperOp::Fmac { op, dest, fd, fs, rhs } => {
            let (ft, q) = rhs_parts(rhs);
            u.reads_vf.push(fs.0);
            if let Some(ft) = ft {
                u.reads_vf.push(ft);
            }
            u.reads_q = q;
            u.writes_vf = Some((fd.0, dest.0));
            use vu_decode::upper::FmacOp::*;
            u.sets_macstatus = !matches!(op, Max | Mini);
        }
        UpperOp::FmacA { op, dest, fs, rhs } => {
            let (ft, q) = rhs_parts(rhs);
            u.reads_vf.push(fs.0);
            if let Some(ft) = ft {
                u.reads_vf.push(ft);
            }
            u.reads_q = q;
            let _ = dest;
            u.writes_acc = true;
            use vu_decode::upper::FmacOp::*;
            u.sets_macstatus = !matches!(op, Max | Mini);
        }
        UpperOp::Opmula { fs, ft, .. } => {
            u.reads_vf.extend([fs.0, ft.0]);
            u.writes_acc = true;
            u.sets_macstatus = true;
        }
        UpperOp::Opmsub { dest, fd, fs, ft } => {
            u.reads_vf.extend([fs.0, ft.0]);
            u.writes_vf = Some((fd.0, dest.0));
            u.sets_macstatus = true;
        }
        UpperOp::Abs { dest, ft, fs } => {
            u.reads_vf.push(fs.0);
            u.writes_vf = Some((ft.0, dest.0));
        }
        UpperOp::Ftoi { dest, ft, fs, .. } | UpperOp::Itof { dest, ft, fs, .. } => {
            u.reads_vf.push(fs.0);
            u.writes_vf = Some((ft.0, dest.0));
        }
        UpperOp::Clip { fs, ft } => {
            u.reads_vf.extend([fs.0, ft.0]);
            u.writes_clip = true;
        }
    }
    u
}

fn lower_info(b: &Bundle) -> LowerInfo {
    let mut l = LowerInfo::default();
    let op = match b.lower {
        LowerSlot::Loi(_) => return l,
        LowerSlot::Inst(op) => op,
    };
    use LowerOp::*;
    match op {
        Nop | Invalid { .. } => {}
        Lq { ft, is, .. } => {
            l.reads_vi.push(is.0);
            l.writes_vf = Some(ft.0);
        }
        Sq { fs, it, .. } => {
            l.reads_vi.push(it.0);
            l.reads_vf.push(fs.0);
        }
        Ilw { it, is, .. } => {
            l.reads_vi.push(is.0);
            l.writes_vi = Some(it.0);
            l.is_iload = true;
        }
        Isw { it, is, .. } => l.reads_vi.extend([it.0, is.0]),
        Lqd { ft, is, .. } | Lqi { ft, is, .. } => {
            l.reads_vi.push(is.0);
            l.writes_vi = Some(is.0);
            l.writes_vf = Some(ft.0);
        }
        Sqd { fs, it, .. } | Sqi { fs, it, .. } => {
            l.reads_vi.push(it.0);
            l.writes_vi = Some(it.0);
            l.reads_vf.push(fs.0);
        }
        Ilwr { it, is, .. } => {
            l.reads_vi.push(is.0);
            l.writes_vi = Some(it.0);
            l.is_iload = true;
        }
        Iswr { it, is, .. } => l.reads_vi.extend([it.0, is.0]),
        Iadd { id, is, it } | Isub { id, is, it } | Iand { id, is, it } | Ior { id, is, it } => {
            l.reads_vi.extend([is.0, it.0]);
            l.writes_vi = Some(id.0);
        }
        Iaddi { it, is, .. } | Iaddiu { it, is, .. } | Isubiu { it, is, .. } => {
            l.reads_vi.push(is.0);
            l.writes_vi = Some(it.0);
        }
        Move { fs, ft, .. } | Mr32 { fs, ft, .. } => {
            l.reads_vf.push(fs.0);
            l.writes_vf = Some(ft.0);
        }
        Mfir { ft, is, .. } => {
            l.reads_vi.push(is.0);
            l.writes_vf = Some(ft.0);
        }
        Mtir { it, fs, .. } => {
            l.reads_vf.push(fs.0);
            l.writes_vi = Some(it.0);
        }
        Mfp { ft, .. } => l.writes_vf = Some(ft.0),
        Div { fs, ft, .. } => {
            l.reads_vf.extend([fs.0, ft.0]);
            l.div = Some(DivKind::Div);
        }
        Sqrt { ft, .. } => {
            l.reads_vf.push(ft.0);
            l.div = Some(DivKind::Sqrt);
        }
        Rsqrt { fs, ft, .. } => {
            l.reads_vf.extend([fs.0, ft.0]);
            l.div = Some(DivKind::Rsqrt);
        }
        Waitq => l.div = Some(DivKind::Waitq),
        Rinit { fs, .. } | Rxor { fs, .. } => l.reads_vf.push(fs.0),
        Rget { ft, .. } | Rnext { ft, .. } => l.writes_vf = Some(ft.0),
        Esadd { fs } | Ersadd { fs } | Eleng { fs } | Erleng { fs } | Eatanxy { fs }
        | Eatanxz { fs } | Esum { fs } => l.reads_vf.push(fs.0),
        Esqrt { ft, .. } | Ersqrt { ft, .. } | Ercpr { ft, .. } | Esin { ft, .. }
        | Eatan { ft, .. } | Eexp { ft, .. } => l.reads_vf.push(ft.0),
        Waitp => {}
        Fsand { it, .. } | Fseq { it, .. } | Fsor { it, .. } => {
            l.writes_vi = Some(it.0);
            l.reads_flags = Some(FlagKind::Status);
        }
        Fsset { .. } => {}
        Fmand { it, is } | Fmeq { it, is } | Fmor { it, is } => {
            l.reads_vi.push(is.0);
            l.writes_vi = Some(it.0);
            l.reads_flags = Some(FlagKind::Mac);
        }
        Fcand { .. } | Fceq { .. } | Fcor { .. } => {
            l.writes_vi = Some(1);
            l.reads_flags = Some(FlagKind::Clip);
        }
        Fcset { .. } => {}
        Fcget { it } => {
            l.writes_vi = Some(it.0);
            l.reads_flags = Some(FlagKind::Clip);
        }
        Ibeq { it, is, imm11 } | Ibne { it, is, imm11 } => {
            l.branch = Some((vec![it.0, is.0], Some(branch_target(b.offset, imm11))));
        }
        Ibltz { is, imm11 } | Ibgtz { is, imm11 } | Iblez { is, imm11 } | Ibgez { is, imm11 } => {
            l.branch = Some((vec![is.0], Some(branch_target(b.offset, imm11))));
        }
        B { imm11 } => l.branch = Some((vec![], Some(branch_target(b.offset, imm11)))),
        Bal { it, imm11 } => {
            l.branch = Some((vec![], Some(branch_target(b.offset, imm11))));
            l.writes_vi = Some(it.0);
            l.is_bal = true;
        }
        Jr { is } => l.branch = Some((vec![is.0], None)),
        Jalr { it, is } => {
            l.branch = Some((vec![is.0], None));
            l.writes_vi = Some(it.0);
        }
        Xtop { it } | Xitop { it } => l.writes_vi = Some(it.0),
        Xgkick { is } => l.reads_vi.push(is.0),
    }
    // vi00 is constant zero; writes to it are suppressed and reads are
    // hazard-free.
    if l.writes_vi == Some(0) {
        l.writes_vi = None;
    }
    l.reads_vi.retain(|&r| r != 0);
    l
}

pub fn analyze(prog: &Vu1Program) -> Result<Analysis> {
    let bundles = decode_program(&prog.image).map_err(anyhow::Error::msg)?;
    let n = bundles.len();
    let uppers: Vec<UpperInfo> = bundles.iter().map(upper_info).collect();
    let lowers: Vec<LowerInfo> = bundles.iter().map(lower_info).collect();
    let mut audit = Audit::default();

    // Reject anything outside the shapes these five programs use.
    for (i, b) in bundles.iter().enumerate() {
        if b.has_invalid() {
            bail!(
                "{}: bundle at {:#x} does not decode ({:08x} {:08x}); \
                 upload framing extraction is broken if this fires",
                prog.name,
                b.offset,
                b.upper_raw,
                b.lower_raw
            );
        }
        if b.flags.m || b.flags.t || b.flags.reserved != 0 {
            bail!(
                "{}: bundle at {:#x} carries M/T/reserved upper flags ({:08x})",
                prog.name,
                b.offset,
                b.upper_raw
            );
        }
        if b.flags.d {
            // Retail normal_c/normal_l each have one D-bit pad bundle. The
            // D break is inert unless the (unemulated) debug registers arm
            // it; ignore with a note.
            audit
                .notes
                .push(format!("{}: D bit set at {:#x} (ignored)", prog.name, b.offset));
        }
        let _ = i;
    }

    // Entry stubs: leading region of pad-upper bundles whose lowers are
    // unconditional branches or nops. Every branch stub is an MSCAL entry.
    let mut entries: BTreeSet<u32> = BTreeSet::new();
    entries.insert(0);
    let mut i = 0usize;
    while i < n {
        let b = &bundles[i];
        if b.upper != UpperOp::Nop || b.flags.any() {
            break;
        }
        match b.lower {
            LowerSlot::Inst(LowerOp::B { .. }) => {
                entries.insert(b.offset);
            }
            LowerSlot::Inst(LowerOp::Nop) => {}
            LowerSlot::Inst(LowerOp::Move { dest, .. }) if dest.is_empty() => {}
            _ => break,
        }
        i += 1;
    }

    // Labels, dispatch, delay slots.
    let mut labels: BTreeSet<u32> = BTreeSet::new();
    let mut dispatch: BTreeSet<u32> = entries.clone();
    let mut slots: BTreeSet<usize> = BTreeSet::new();
    for (i, l) in lowers.iter().enumerate() {
        if let Some((_, target)) = &l.branch {
            if let Some(t) = target {
                if *t as usize >= n * 8 {
                    bail!(
                        "{}: branch at {:#x} targets {:#x}, past the end of the program",
                        prog.name,
                        bundles[i].offset,
                        t
                    );
                }
                labels.insert(*t);
            }
            if i + 1 >= n {
                bail!("{}: branch at {:#x} has no delay slot bundle", prog.name, bundles[i].offset);
            }
            slots.insert(i + 1);
            if l.is_bal {
                // Link value: instruction address of branch + 2 (the bundle
                // after the delay slot); jr through it returns there.
                dispatch.insert((i as u32 + 2) * 8);
            }
        }
    }
    for &s in &slots {
        if lowers[s].branch.is_some() {
            bail!(
                "{}: branch in the delay slot of another branch at {:#x}",
                prog.name,
                bundles[s].offset
            );
        }
        let off = bundles[s].offset;
        if labels.contains(&off) || dispatch.contains(&off) {
            bail!(
                "{}: delay slot at {:#x} is also a jump target; the emitter \
                 would have to duplicate it",
                prog.name,
                off
            );
        }
        if bundles[s].flags.e {
            bail!("{}: E bit inside a delay slot at {:#x}", prog.name, off);
        }
    }
    for (i, b) in bundles.iter().enumerate() {
        if b.flags.e {
            if lowers[i].branch.is_some() {
                bail!("{}: E bit on a branch bundle at {:#x}", prog.name, b.offset);
            }
            if i + 1 >= n {
                bail!("{}: E bit on the final bundle at {:#x}", prog.name, b.offset);
            }
            if lowers[i + 1].branch.is_some() {
                bail!(
                    "{}: branch in the E-bit delay slot at {:#x}",
                    prog.name,
                    bundles[i + 1].offset
                );
            }
            // MSCNT resumes at the bundle after the E-bit delay slot; make
            // every stored resume pc dispatchable. (i + 2 can never be a
            // delay slot: that would need a branch at i or i + 1, both
            // rejected above.)
            if i + 2 < n {
                dispatch.insert((i as u32 + 2) * 8);
            }
        }
    }

    // Same-bundle exchanges and double writes.
    for (i, b) in bundles.iter().enumerate() {
        let (u, l) = (&uppers[i], &lowers[i]);
        if let Some((reg, mask)) = u.writes_vf {
            if reg != 0 && mask != 0 {
                if l.reads_vf.contains(&reg) {
                    audit.same_bundle.push(format!(
                        "{}: {:#x}: lower reads vf{:02} which the upper writes \
                         (pre-state read, handled by deferred commit)",
                        prog.name, b.offset, reg
                    ));
                }
                if l.writes_vf == Some(reg) {
                    bail!(
                        "{}: both halves of bundle {:#x} write vf{:02}; write \
                         priority is not modeled",
                        prog.name,
                        b.offset,
                        reg
                    );
                }
            }
        }
        if let Some(reg) = l.writes_vf {
            if reg != 0 && u.reads_vf.contains(&reg) {
                audit.same_bundle.push(format!(
                    "{}: {:#x}: upper reads vf{:02} which the lower writes \
                     (pre-state read, handled by ordering)",
                    prog.name, b.offset, reg
                ));
            }
        }
    }

    // FMAC result window: reads within 3 bundles of an upper vf write.
    // Hardware interlocks here, so these are stall sites, not old-value
    // sites; recorded to prove the determination was made on real data.
    for i in 0..n {
        let Some((reg, mask)) = uppers[i].writes_vf else { continue };
        if reg == 0 || mask == 0 {
            continue;
        }
        for d in 1..=FMAC_WINDOW as usize {
            let j = i + d;
            if j >= n {
                break;
            }
            if uppers[j].reads_vf.contains(&reg) {
                audit.fmac_stalls.push(format!(
                    "{}: vf{:02} written at {:#x}, upper-pipe read at {:#x} (+{d})",
                    prog.name,
                    reg,
                    bundles[i].offset,
                    bundles[j].offset
                ));
            }
            if lowers[j].reads_vf.contains(&reg) {
                audit.fmac_stalls.push(format!(
                    "{}: vf{:02} written at {:#x}, lower-pipe read at {:#x} (+{d})",
                    prog.name,
                    reg,
                    bundles[i].offset,
                    bundles[j].offset
                ));
            }
        }
    }

    // Q pipeline. Forward events for the report; per-read back-scan for
    // the commit sites. Scans are linear (fall-through order); every
    // consumer in these programs sits in the same straight-line run as its
    // issue, which the forward scan's label check verifies.
    let mut q_commit_sites: BTreeSet<u32> = BTreeSet::new();
    for i in 0..n {
        match lowers[i].div {
            Some(DivKind::Div) | Some(DivKind::Sqrt) | Some(DivKind::Rsqrt) => {}
            _ => continue,
        }
        let lat = if lowers[i].div == Some(DivKind::Rsqrt) { RSQRT_LATENCY } else { DIV_LATENCY };
        let mut event = format!(
            "{}: {:?} issued at {:#x}: ",
            prog.name,
            lowers[i].div.unwrap(),
            bundles[i].offset
        );
        let mut found = false;
        for j in i + 1..n {
            let d = (j - i) as u32;
            if uppers[j].reads_q {
                let class = if d < lat { "inside the window; old Q (modeled)" } else { "past the latency; commit forced at the read site" };
                event.push_str(&format!("first Q read at {:#x} (+{d}), {class}", bundles[j].offset));
                found = true;
                break;
            }
            match lowers[j].div {
                Some(DivKind::Waitq) => {
                    event.push_str(&format!("waitq at {:#x} (+{d})", bundles[j].offset));
                    found = true;
                }
                Some(_) => {
                    event.push_str(&format!(
                        "next issue at {:#x} (+{d}) with no read or waitq between",
                        bundles[j].offset
                    ));
                    found = true;
                }
                None => {}
            }
            if found {
                break;
            }
        }
        if !found {
            event.push_str("no consumer before the program end");
        }
        audit.q_events.push(event);
    }
    for i in 0..n {
        if !uppers[i].reads_q {
            continue;
        }
        // A waitq in this same bundle stalls the whole pair until the
        // divider writes Q, so the upper half reads the new value. The
        // programs pair `mulq | waitq` for exactly that reason.
        if lowers[i].div == Some(DivKind::Waitq) {
            q_commit_sites.insert(bundles[i].offset);
            continue;
        }
        for back in 1..=64usize {
            let Some(j) = i.checked_sub(back) else { break };
            match lowers[j].div {
                Some(DivKind::Waitq) => break, // already committed
                Some(k) => {
                    let lat = if k == DivKind::Rsqrt { RSQRT_LATENCY } else { DIV_LATENCY };
                    if back as u32 >= lat {
                        q_commit_sites.insert(bundles[i].offset);
                    }
                    break;
                }
                None => {}
            }
        }
    }

    // Integer-branch hazard: a conditional branch or jr whose source vi was
    // written by the immediately preceding bundle reads the OLD value.
    // Implemented exactly at these sites with a temp saved before the
    // write.
    let mut old_vi_sites: BTreeMap<u32, Vec<u8>> = BTreeMap::new();
    for i in 1..n {
        let Some((srcs, _)) = &lowers[i].branch else { continue };
        let Some(w) = lowers[i - 1].writes_vi else { continue };
        if !srcs.contains(&w) {
            continue;
        }
        let off = bundles[i].offset;
        if labels.contains(&off) || dispatch.contains(&off) {
            bail!(
                "{}: integer-branch hazard at {:#x} on a bundle that is also a \
                 jump target; the saved-temp fix assumes linear entry",
                prog.name,
                off
            );
        }
        audit.int_branch.push(format!(
            "{}: branch at {:#x} reads vi{:02} written at {:#x}; branch uses the \
             pre-write value (old-value semantics emitted)",
            prog.name,
            off,
            w,
            bundles[i - 1].offset
        ));
        old_vi_sites.entry(off).or_default().push(w);
    }

    // Integer load consumed in the very next bundle (report only; none in
    // the shipped programs, and non-branch consumers interlock anyway).
    for i in 0..n.saturating_sub(1) {
        if !lowers[i].is_iload {
            continue;
        }
        let Some(w) = lowers[i].writes_vi else { continue };
        let next = &lowers[i + 1];
        let branch_reads = next.branch.as_ref().is_some_and(|(s, _)| s.contains(&w));
        if next.reads_vi.contains(&w) || branch_reads {
            audit.ilw_next.push(format!(
                "{}: integer load at {:#x} writes vi{:02}, consumed at {:#x}",
                prog.name,
                bundles[i].offset,
                w,
                bundles[i + 1].offset
            ));
        }
    }

    // Flag pipeline windows (report only; flags commit immediately in the
    // current model). A read at distance 0..=3 from a setter would see the
    // pre-setter flags on hardware.
    for i in 0..n {
        let Some(kind) = lowers[i].reads_flags else { continue };
        for d in 0..=FLAG_WINDOW as usize {
            let Some(j) = i.checked_sub(d) else { break };
            let setter = match kind {
                FlagKind::Mac | FlagKind::Status => uppers[j].sets_macstatus,
                FlagKind::Clip => uppers[j].writes_clip,
            };
            if setter && d == 0 {
                // Same bundle: the emitter commits the upper half after the
                // lower half runs, so the model already shows the old flags
                // here, as hardware does. Not a finding.
            } else if setter {
                // A flag read that only lands in a vf or a stored value is
                // a numeric difference. One whose vi feeds a branch inside
                // the next two bundles changes control flow, which is the
                // same shape as the Q-pipeline bug: a wrong path, not a
                // wrong number. Call those out separately.
                let feeds_branch = lowers[i].writes_vi.and_then(|w| {
                    (i + 1..=(i + 2).min(n - 1)).find(|&k| {
                        lowers[k]
                            .branch
                            .as_ref()
                            .is_some_and(|(srcs, _)| srcs.contains(&w))
                    })
                });
                let tail = match feeds_branch {
                    Some(k) => format!(
                        "; the vi it writes is the branch condition at {:#x}, so this \
                         one selects a path",
                        bundles[k].offset
                    ),
                    None => String::new(),
                };
                audit.flag_windows.push(format!(
                    "{}: {:?} read at {:#x} with a setter at {:#x} (-{d}); hardware \
                     would still show the older flags, immediate model shows the new{tail}",
                    prog.name,
                    kind,
                    bundles[i].offset,
                    bundles[j].offset
                ));
            }
        }
        // Status I/D bits also lag a div-unit issue by its latency.
        if kind == FlagKind::Status {
            for d in 1..DIV_LATENCY as usize {
                let Some(j) = i.checked_sub(d) else { break };
                if matches!(lowers[j].div, Some(DivKind::Div) | Some(DivKind::Sqrt) | Some(DivKind::Rsqrt)) {
                    audit.flag_windows.push(format!(
                        "{}: status read at {:#x} inside the I/D latency of the div \
                         issue at {:#x} (-{d}); model sets I/D at issue time",
                        prog.name,
                        bundles[i].offset,
                        bundles[j].offset
                    ));
                }
            }
        }
    }

    // Flag pipeline. A lower-slot fmand/fsand/fmeq/... at bundle i reads the
    // MAC or status flags as they stood four bundles back, not as this
    // bundle's FMAC just left them. Same fix shape as the integer-branch
    // hazard: snapshot the value into a temp at the earlier bundle and read
    // the temp at the use site.
    let mut flag_save_sites: BTreeMap<u32, Vec<(u32, bool)>> = BTreeMap::new();
    let mut flag_read_sites: BTreeMap<u32, bool> = BTreeMap::new();
    for i in 0..n {
        let Some(kind) = lowers[i].reads_flags else { continue };
        let is_mac = match kind {
            FlagKind::Mac => true,
            FlagKind::Status => false,
            // The clip register is written by vclip and read by fcand/fcor.
            // No read in the shipped programs falls inside its window, so
            // the immediate model is exact there; leave it alone.
            FlagKind::Clip => continue,
        };
        let read_off = bundles[i].offset;
        // Only a setter inside the window makes the immediate model wrong.
        let mut inside = false;
        for d in 1..=FLAG_WINDOW as usize {
            let Some(j) = i.checked_sub(d) else { break };
            // Both MAC and status are written by the FMAC units. Status also
            // carries the divider's D/I bits, which the interpreter's history
            // delays along with everything else, so a div issue inside the
            // window has to count as a setter or the two models disagree.
            let setter = uppers[j].sets_macstatus
                || (!is_mac
                    && matches!(
                        lowers[j].div,
                        Some(DivKind::Div) | Some(DivKind::Sqrt) | Some(DivKind::Rsqrt)
                    ));
            if setter {
                inside = true;
                break;
            }
        }
        if !inside {
            continue;
        }
        flag_read_sites.insert(read_off, is_mac);
        // The snapshot goes at the top of bundle i-3, so it captures the
        // state left by bundle i-4. Fewer than 4 bundles in means the
        // visible flags are the ones the program was entered with, and the
        // temp keeps its entry initialiser.
        let Some(save_idx) = i.checked_sub(FLAG_WINDOW as usize) else {
            audit.flag_windows.push(format!(
                "{}: flag read at {read_off:#x} is within {} bundles of entry; \
                 visible flags are the entry values",
                prog.name, FLAG_WINDOW
            ));
            continue;
        };
        // The temp is written at save_idx and read at i. If anything between
        // them can be entered from elsewhere, the temp may not hold what
        // this path put there.
        let mut linear = true;
        for k in save_idx + 1..=i {
            let off = bundles[k].offset;
            if labels.contains(&off) || dispatch.contains(&off) {
                linear = false;
                break;
            }
        }
        if !linear {
            bail!(
                "{}: flag read at {read_off:#x} needs a snapshot at {:#x}, but a \
                 bundle between them is a jump target; the saved-temp fix assumes \
                 linear entry",
                prog.name,
                bundles[save_idx].offset
            );
        }
        flag_save_sites
            .entry(bundles[save_idx].offset)
            .or_default()
            .push((read_off, is_mac));
        audit.flag_windows.push(format!(
            "{}: flag read at {read_off:#x} now uses the flags snapshotted at {:#x} \
             ({} bundles back), matching the {}-deep flag pipeline",
            prog.name,
            bundles[save_idx].offset,
            FLAG_WINDOW,
            FLAG_WINDOW + 1
        ));
    }

    Ok(Analysis {
        flag_save_sites,
        flag_read_sites,
        bundles,
        entries: entries.into_iter().collect(),
        labels,
        dispatch,
        q_commit_sites,
        old_vi_sites,
        audit,
    })
}
