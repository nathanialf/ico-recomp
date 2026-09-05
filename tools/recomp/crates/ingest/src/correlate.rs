//! Transplanting a donor build's function boundaries onto a target ELF by
//! masked-instruction fingerprint.
//!
//! Why this exists: the PAL disc's `SRCFILE.TXT` is an objdump listing of a
//! development link, not of the shipped `SCES_507.60`. Measured: the
//! listing's `.text` ends at 0x28DB34 and the retail ELF's at 0x289BC4, and
//! comparing the two at equal addresses disagrees on 99% of words. The code
//! itself is the same code, just relinked: masking the fields a relink
//! rewrites makes 3772 of the 5855 listed `.text` functions match a unique
//! position in the retail `.text`, and the delta pass below carries that to
//! 5533 (measured 2026-09-05). So the listing is used as a donor of names,
//! source files and boundaries, and never as an address authority.
//!
//! The mask erases exactly the fields a relink can change:
//!
//! * `j` / `jal`: the whole 26-bit target, which is an absolute address.
//! * `lui`: the 16-bit immediate, which carries the high half of every
//!   `%hi(symbol)`.
//! * `addi` / `addiu` / `ori`: the 16-bit immediate, which carries `%lo`.
//!   This also erases genuine small constants, which costs discrimination
//!   but never causes a false match on its own (the whole body still has
//!   to agree).
//! * every load and store, of any width and to any register file: the
//!   16-bit displacement, which carries `%lo` for absolute addressing. The
//!   set is `scan::opcode_is_memory`, shared with the scan so the two
//!   cannot disagree about what a memory opcode is. Restricting it to
//!   opcodes 0x20..0x2F, as this did until 2026-09-05, left out `lq`, `sq`,
//!   `ld`, `sd`, `ldl`, `ldr`, `lwc1`, `swc1`, `ldc1`, `sdc1`, `lqc2`,
//!   `sqc2` and `pref`, and a relink rewrites those displacements exactly as
//!   it rewrites an `lw`'s: one `lwc1` was the single disagreeing word that
//!   kept the matrix composer `gsb_SetVSMatrixSub` from correlating.
//!
//! Branch offsets are deliberately NOT masked: they are PC-relative and
//! internal to the function, so a relink leaves them alone and they are the
//! strongest discriminator available. Measured, masking the four classes
//! above and keeping branches gives 3772 unique matches; masking only `j`,
//! `jal` and `lui` gives 1234, and masking branches too gives fewer still.
//!
//! Nothing here reads or writes game bytes outside the process.

use std::collections::{BTreeMap, HashMap};

use crate::objdump::DonorFunction;
use crate::scan;

/// Minimum number of contiguous instructions a donor function must offer
/// before its first run is used as an index key. Eight words is enough to
/// make an accidental collision rare while still admitting short leaf
/// functions through the neighbour-delta pass.
const KEY_LEN: usize = 8;

/// How a donor function's target address was established.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MatchKind {
    /// The masked body matched exactly one position in the target section.
    /// This is the strong case and the only one used as an anchor.
    UniqueFingerprint,
    /// The donor functions on both sides are anchored at the same
    /// donor-to-target delta, and applying that delta to this function
    /// reproduces its masked body at the implied address. Weaker than an
    /// anchor only in that the address came from its neighbours; the body
    /// check is the same.
    NeighbourDelta,
}

/// The result of correlating one donor section against one target section.
#[derive(Debug, Clone, Default)]
pub struct Correlation {
    /// donor vram -> target vram.
    pub to_target: BTreeMap<u32, u32>,
    pub how: BTreeMap<u32, MatchKind>,
    /// Donor functions with no target address: either the function is gone
    /// from the retail build or its body genuinely differs.
    pub unresolved: Vec<u32>,
    /// Anchors dropped because they would have made the mapping run
    /// backwards (see `keep_monotonic`).
    pub non_monotonic_dropped: usize,
}

impl Correlation {
    pub fn anchors(&self) -> usize {
        self.how
            .values()
            .filter(|k| **k == MatchKind::UniqueFingerprint)
            .count()
    }

    pub fn by_delta(&self) -> usize {
        self.how
            .values()
            .filter(|k| **k == MatchKind::NeighbourDelta)
            .count()
    }
}

/// Erase the fields a relink rewrites. See the module comment for why each
/// class is masked and why branches are not.
pub fn mask(word: u32) -> u32 {
    let op = word >> 26;
    match op {
        // j, jal: absolute 26-bit target.
        2 | 3 => word & 0xFC00_0000,
        // lui: %hi(symbol).
        0x0F => word & 0xFFFF_0000,
        // addi, addiu, ori: %lo(symbol) (and small constants, accepted).
        8 | 9 | 0x0D => word & 0xFFFF_0000,
        // Loads and stores of every width: %lo(symbol) as a displacement.
        op if scan::opcode_is_memory(op as u8) => word & 0xFFFF_0000,
        _ => word,
    }
}

/// Correlate `donor` functions against `target_words`, the words of the
/// target section starting at `target_base`.
pub fn correlate(
    donor: &[&DonorFunction],
    target_words: &[u32],
    target_base: u32,
) -> Correlation {
    let masked: Vec<u32> = target_words.iter().copied().map(mask).collect();

    let mut index: HashMap<&[u32], Vec<usize>> = HashMap::new();
    if masked.len() >= KEY_LEN {
        for i in 0..=masked.len() - KEY_LEN {
            index.entry(&masked[i..i + KEY_LEN]).or_default().push(i);
        }
    }

    // Pass 1: unique full-body fingerprint matches become anchors.
    let mut anchors: BTreeMap<u32, u32> = BTreeMap::new();
    for f in donor {
        let runs = f.runs();
        let Some(first) = runs.first() else { continue };
        if first.len() < KEY_LEN {
            continue;
        }
        let key: Vec<u32> = first[..KEY_LEN].iter().map(|i| mask(i.word)).collect();
        let Some(positions) = index.get(key.as_slice()) else {
            continue;
        };
        let mut hits = Vec::new();
        for &pos in positions {
            let delta = (target_base as i64 + pos as i64 * 4) - f.vram as i64;
            if body_matches(&runs, delta, &masked, target_base) {
                hits.push(target_base + (pos as u32) * 4);
                if hits.len() > 1 {
                    break;
                }
            }
        }
        if hits.len() == 1 {
            anchors.insert(f.vram, hits[0]);
        }
    }

    let before = anchors.len();
    let anchors = keep_monotonic(anchors);
    let dropped = before - anchors.len();

    // Pass 2: a donor function whose nearest anchor on each side agrees on
    // the donor-to-target delta is placed at that delta, provided its body
    // reproduces there.
    let anchor_keys: Vec<u32> = anchors.keys().copied().collect();
    let mut out = Correlation {
        non_monotonic_dropped: dropped,
        ..Correlation::default()
    };
    for (&dv, &tv) in &anchors {
        out.to_target.insert(dv, tv);
        out.how.insert(dv, MatchKind::UniqueFingerprint);
    }
    for f in donor {
        if out.to_target.contains_key(&f.vram) {
            continue;
        }
        let Some(delta) = delta_hypothesis(&anchor_keys, &anchors, f.vram) else {
            out.unresolved.push(f.vram);
            continue;
        };
        let runs = f.runs();
        if !runs.is_empty() && body_matches(&runs, delta, &masked, target_base) {
            let target = (f.vram as i64 + delta) as u32;
            out.to_target.insert(f.vram, target);
            out.how.insert(f.vram, MatchKind::NeighbourDelta);
        } else {
            out.unresolved.push(f.vram);
        }
    }
    out.unresolved.sort_unstable();
    out
}

/// Every run of the donor function must reproduce, masked, at its address
/// plus `delta`.
fn body_matches(
    runs: &[&[crate::objdump::DonorInsn]],
    delta: i64,
    masked: &[u32],
    target_base: u32,
) -> bool {
    for run in runs {
        let start = run[0].vram as i64 + delta - target_base as i64;
        if start < 0 || start % 4 != 0 {
            return false;
        }
        let start = (start / 4) as usize;
        if start + run.len() > masked.len() {
            return false;
        }
        for (k, insn) in run.iter().enumerate() {
            if masked[start + k] != mask(insn.word) {
                return false;
            }
        }
    }
    true
}

/// Keep the largest subset of anchors whose target addresses increase with
/// their donor addresses. Both builds lay their functions out in the same
/// order, so a pair that runs backwards is a wrong match by construction,
/// and one wrong anchor would poison every neighbour-delta decision around
/// it. Implemented as a longest strictly-increasing subsequence over the
/// donor-ordered target addresses.
fn keep_monotonic(anchors: BTreeMap<u32, u32>) -> BTreeMap<u32, u32> {
    let items: Vec<(u32, u32)> = anchors.into_iter().collect();
    if items.is_empty() {
        return BTreeMap::new();
    }
    // tails[k] = index into items of the smallest tail of an increasing
    // subsequence of length k+1.
    let mut tails: Vec<usize> = Vec::new();
    let mut prev: Vec<Option<usize>> = vec![None; items.len()];
    for i in 0..items.len() {
        let v = items[i].1;
        let pos = tails.partition_point(|&t| items[t].1 < v);
        if pos == tails.len() {
            tails.push(i);
        } else {
            tails[pos] = i;
        }
        prev[i] = if pos > 0 { Some(tails[pos - 1]) } else { None };
    }
    let mut keep = Vec::new();
    let mut cur = tails.last().copied();
    while let Some(i) = cur {
        keep.push(items[i]);
        cur = prev[i];
    }
    keep.reverse();
    keep.into_iter().collect()
}

/// A donor-to-target delta to try for a function no anchor covers.
///
/// This only generates a hypothesis: `body_matches` is the proof, and a
/// wrong hypothesis cannot pass it, so the rule is as permissive as it can
/// be made without weakening anything.
///
/// * anchors on both sides that agree on a delta: that delta. Disagreement
///   means the shift changes somewhere in between and position alone says
///   nothing, so no hypothesis is offered.
/// * an anchor on one side only: that anchor's delta. This is the case that
///   matters for the bottom of `.text`: the EE kernel stub table and crt0
///   sit below the first anchor and are `li $v0,n; syscall; jr $ra; nop`
///   stubs, too short (below `KEY_LEN`) to anchor themselves. Before this
///   arm existed, 141 donor functions there went unnamed and every kernel
///   stub read as `func_XXXXXXXX` in a crash report.
/// * no anchors at all: delta 0, which is the only guess available.
fn delta_hypothesis(
    keys: &[u32],
    anchors: &BTreeMap<u32, u32>,
    vram: u32,
) -> Option<i64> {
    let idx = keys.partition_point(|&k| k <= vram);
    let below = if idx > 0 { Some(keys[idx - 1]) } else { None };
    let above = keys.get(idx).copied();
    let d = |k: u32| anchors[&k] as i64 - k as i64;
    match (below, above) {
        (Some(b), Some(a)) => {
            let (db, da) = (d(b), d(a));
            if db == da {
                Some(db)
            } else {
                None
            }
        }
        (Some(k), None) | (None, Some(k)) => Some(d(k)),
        (None, None) => Some(0),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::objdump::{DonorFunction, DonorInsn};

    fn f(name: &str, vram: u32, words: &[u32]) -> DonorFunction {
        DonorFunction {
            name: name.to_string(),
            vram,
            section: ".text".to_string(),
            source_file: Some("test.c".to_string()),
            insns: words
                .iter()
                .enumerate()
                .map(|(i, &w)| DonorInsn {
                    vram: vram + (i as u32) * 4,
                    word: w,
                    text: None,
                })
                .collect(),
        }
    }

    #[test]
    fn masks_only_the_fields_a_relink_rewrites() {
        // jal 0x00102000 and jal 0x00103000 mask to the same word.
        assert_eq!(mask(0x0C04_0800), mask(0x0C04_0C00));
        // lui $v0, 0x64 and lui $v0, 0x73 mask to the same word.
        assert_eq!(mask(0x3C02_0064), mask(0x3C02_0073));
        // bnez $at, -6 keeps its offset: branches are not masked.
        assert_ne!(mask(0x1420_FFFA), mask(0x1420_FFF9));
        // A register-to-register op is untouched.
        assert_eq!(mask(0x0043_082B), 0x0043_082B);
    }

    #[test]
    fn anchors_and_then_fills_by_neighbour_delta() {
        // Target: 4 words of padding, then A (8 words), B (2 words),
        // C (8 words). Donor holds the same three functions 0x40 lower.
        let a: Vec<u32> = (0..8).map(|i| 0x0000_0021 + i).collect();
        let b: Vec<u32> = vec![0x03E0_0008, 0x0000_0000];
        let c: Vec<u32> = (0..8).map(|i| 0x0000_1021 + i).collect();
        let mut target = vec![0u32; 4];
        target.extend(&a);
        target.extend(&b);
        target.extend(&c);

        let base = 0x0010_0000u32;
        let a_t = base + 4 * 4;
        let b_t = a_t + 8 * 4;
        let c_t = b_t + 2 * 4;

        let fa = f("A", a_t - 0x40, &a);
        let fb = f("B", b_t - 0x40, &b);
        let fc = f("C", c_t - 0x40, &c);
        let donor = vec![&fa, &fb, &fc];

        let corr = correlate(&donor, &target, base);
        assert_eq!(corr.to_target[&fa.vram], a_t);
        assert_eq!(corr.to_target[&fc.vram], c_t);
        // B is only two instructions long: too short to key the index, so
        // it is placed by the delta its neighbours agree on.
        assert_eq!(corr.to_target[&fb.vram], b_t);
        assert_eq!(corr.how[&fb.vram], MatchKind::NeighbourDelta);
        assert_eq!(corr.anchors(), 2);
        assert!(corr.unresolved.is_empty());
    }

    #[test]
    fn a_donor_function_that_is_not_in_the_target_stays_unresolved() {
        let a: Vec<u32> = (0..8).map(|i| 0x0000_0021 + i).collect();
        let gone: Vec<u32> = (0..8).map(|i| 0x0000_5021 + i).collect();
        let base = 0x0010_0000u32;
        let fa = f("A", 0x0020_0000, &a);
        let fg = f("gone", 0x0020_0020, &gone);
        let corr = correlate(&[&fa, &fg], &a, base);
        assert_eq!(corr.to_target[&fa.vram], base);
        assert_eq!(corr.unresolved, vec![fg.vram]);
    }
}
