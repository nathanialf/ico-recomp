//! `ingest`: parses every translation input for the ICO retail build into
//! one `ProgramDb`, serializable to JSON.
//!
//! There is one input path, the disc target (`disc`), and its inputs are the
//! two files `setup.sh` extracts from the user's own disc image:
//!
//!   1. the retail boot ELF `SCES_507.60` (`object` crate, SHA-1-pinned)
//!   2. `SRCFILE.TXT`, the objdump listing of a late development link, whose
//!      functions are correlated onto the retail `.text` by masked
//!      fingerprint (`correlate`) and supply names, source files and
//!      boundaries
//!   3. the entry proofs the ELF itself carries (`scan`), which supply the
//!      boundaries the correlation cannot
//!   4. derived: a per-function table (size, TU membership, jtbl-owner flag)
//!
//! The decomp is a separate project and not an input (the user's ruling,
//! 2026-09-05; see CLAUDE.md).
//!
//! `ProgramDb` itself holds only addresses, names, and counts: never raw
//! ELF/ROM bytes or raw instruction words. Callers write it to
//! `generated/programdb.json`, which is gitignored; this crate never writes
//! anywhere itself.

mod config;
mod correlate;
mod disc;
mod elf;
mod model;
mod objdump;
mod scan;

use std::collections::HashSet;
use std::path::Path;

use anyhow::{bail, Context, Result};

pub use config::{DiscPaths, RecompConfig, PAL_ROOT_ENV};
pub use correlate::{correlate, Correlation, MatchKind};
pub use disc::{describe as describe_disc_ingest, DiscIngest};
pub use elf::ElfImage;
pub use model::{
    ElfSection, Function, JumpTable, ProgramDb, Stats, SubsegKind, TranslationUnit,
};
pub use objdump::{DonorFunction, DonorInsn, Objdump};
pub use scan::opcode_writes_gpr_rt;

impl ProgramDb {
    /// Parse every input reachable from `config_path` (normally
    /// `config/recomp.toml`) into one `ProgramDb`.
    pub fn load(config_path: &Path) -> Result<ProgramDb> {
        Ok(load_disc(config_path)?.db)
    }
}

/// True when `word` has a delay slot: a branch, a `j`, a `jal`, a `jr` or a
/// `jalr`. The word after one of these is executed before the transfer, so
/// it cannot be given to another function.
pub(crate) fn is_control_word(word: u32) -> bool {
    if scan::branch_target(word, 0).is_some() {
        return true;
    }
    // jal
    if word >> 26 == 3 {
        return true;
    }
    // SPECIAL jr, jalr
    word >> 26 == 0 && matches!(word & 0x3F, 0x08 | 0x09)
}

/// Branches that a function boundary turns into unbounded recursion, over a
/// final entry list.
///
/// Why this has to be checked. `ee-emit` merges two functions into one group
/// only when a branch target is not an exact entry (`lib.rs`, the union-find
/// over branch targets); a branch to another function's exact entry is
/// emitted as `Dest::Tail`, which is `CF_x(ctx); return;`. Whether that is
/// harmless depends on the direction:
///
/// * forward, from inside function A to the entry of a later function B: the
///   guest runs A's prefix, then B, then returns to A's caller, and the
///   emitted form calls B and returns, which is the same thing one frame
///   deeper. The branch site cannot be reached again, so nothing grows.
///   Measured on SCES_507.60, 2026-09-05: 16 of these, all in the vendor
///   libc block at 0x0025D7C0 and 0x0027E704 onwards.
/// * backward, from inside a later function to the entry of an earlier one:
///   the guest is looping, and the emitted form adds a native frame per
///   iteration and never unwinds it. That is the hazard, and it is fatal.
///
/// A branch to the entry of the function that contains it is safe either
/// way: the target is inside the group, so the emitter renders it as a
/// `goto`. A `j` is excluded entirely: a `j` to an exact entry is a tail
/// call, which is correct as emitted, and this binary has 497 of them.
///
/// Returns `(entry, branch site)` for each backward violation.
pub(crate) fn back_branches_into_entries(
    words: &[u32],
    base: u32,
    entries: &[u32],
) -> Vec<(u32, u32)> {
    let entry_set: HashSet<u32> = entries.iter().copied().collect();
    let mut out = Vec::new();
    for (i, &word) in words.iter().enumerate() {
        // A `j` to an exact entry is a tail call, which the emitter renders
        // as a call and a return. Only a branch is a hazard.
        if word >> 26 == 2 {
            continue;
        }
        let vram = base + (i as u32) * 4;
        let Some(target) = scan::branch_target(word, vram) else {
            continue;
        };
        if target >= vram || !entry_set.contains(&target) {
            continue;
        }
        // The entry of the function this branch sits in is a `goto`, not a
        // call: only a branch that crosses a boundary backwards is a hazard.
        let own = entries.partition_point(|&e| e <= vram);
        if own > 0 && entries[own - 1] == target {
            continue;
        }
        out.push((target, vram));
    }
    out.sort_unstable();
    out
}

/// The whole-`.text` sweep: every address a `lui`/`addiu` pair forms that is
/// still not a function entry after every proof.
///
/// Nothing is done with these. They are the answer to "what else could the
/// guest call that would not resolve": each is either data (a table base, a
/// string, a jump table) or a function entry no proof reached, and the
/// `looks` column says which of the two the words at the address argue for.
/// The runtime's `bad indirect call` fatal names this report.
pub fn unresolved_pointers(
    sweep: &[scan::PointerSite],
    funcs: &[(u32, String)],
    jtbl_targets: &std::collections::BTreeSet<u32>,
) -> Vec<model::UnresolvedPointer> {
    let entries: HashSet<u32> = funcs.iter().map(|f| f.0).collect();
    let starts: Vec<u32> = funcs.iter().map(|f| f.0).collect();
    let mut seen: HashSet<u32> = HashSet::new();
    let mut out = Vec::new();
    for p in sweep {
        if entries.contains(&p.target) || !seen.insert(p.target) {
            continue;
        }
        let idx = starts.partition_point(|&s| s <= p.target);
        let container = (idx > 0).then(|| &funcs[idx - 1]);
        let mut looks = String::new();
        if p.prologue {
            looks.push_str("a function prologue");
        } else if p.after_return {
            looks.push_str("the word after a return");
        } else {
            looks.push_str("no prologue");
        }
        if p.mem_base {
            looks.push_str(", dereferenced as a load or store base");
        }
        if jtbl_targets.contains(&p.target) {
            looks.push_str(", a jump-table target");
        }
        out.push(model::UnresolvedPointer {
            target: p.target,
            site: p.site,
            containing: container.map(|c| c.1.clone()),
            containing_vram: container.map(|c| c.0),
            looks,
        });
    }
    out.sort_by_key(|u| u.target);
    out
}

/// Re-open and byte-verify the boot ELF for callers that need actual bytes
/// (the decoder, the emitter's three-way verification), without paying for
/// that on every `ProgramDb::load`.
pub fn load_elf_image(config_path: &Path) -> Result<ElfImage> {
    let cfg = RecompConfig::load(config_path)?;
    ElfImage::load(&cfg.elf_path, &cfg.elf_sha1, &cfg.config_path)
}

/// Load the disc inputs and keep everything the correlation learned, not
/// just the `ProgramDb`. The CLI reports the correlation counts.
pub fn load_disc(config_path: &Path) -> Result<DiscIngest> {
    let cfg = RecompConfig::load(config_path)?;
    if !cfg.disc.root.is_dir() {
        bail!(
            "the disc inputs directory {} does not exist ([inputs].root in {}, \
             overridable with {}). Run ./setup.sh <your disc image> to extract \
             SCES_507.60 and SRCFILE.TXT into it.",
            cfg.disc.root.display(),
            cfg.config_path.display(),
            config::PAL_ROOT_ENV
        );
    }
    let image = ElfImage::load(&cfg.elf_path, &cfg.elf_sha1, &cfg.config_path)?;
    disc::load(&cfg, image)
}

/// Serialize a `ProgramDb` to pretty JSON. Callers are responsible for
/// writing it to `generated/programdb.json` (gitignored): this crate never
/// touches the filesystem for output.
pub fn to_json(db: &ProgramDb) -> Result<String> {
    serde_json::to_string_pretty(db).context("serializing ProgramDb to JSON")
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A backward branch across a boundary is the hazard; the same branch
    /// as a `j`, a forward branch, and a branch to the containing
    /// function's own entry are not.
    #[test]
    fn back_branches_into_entries_finds_only_the_backward_crossing() {
        let base = 0x0010_0000u32;
        let entries = vec![base, 0x0010_0008, 0x0010_0010];
        // A branch at 0x100010 back to 0x100000, two entries earlier.
        // offset = (target - (vram + 4)) / 4 = -5.
        let bne_back = 0x1400_0000u32 | 0xFFFBu32;
        let words = vec![0, 0, 0, 0, bne_back, 0];
        assert_eq!(
            back_branches_into_entries(&words, base, &entries),
            vec![(base, 0x0010_0010)]
        );

        // The same word as a `j` is a tail call.
        let j_back = 0x0800_0000u32 | ((base >> 2) & 0x03FF_FFFF);
        let words = vec![0, 0, 0, 0, j_back, 0];
        assert!(back_branches_into_entries(&words, base, &entries).is_empty());

        // A branch to the entry of the function it sits in is a goto.
        // At 0x100014, back to 0x100010: offset -2.
        let bne_own = 0x1400_0000u32 | 0xFFFEu32;
        let words = vec![0, 0, 0, 0, 0, bne_own];
        assert!(back_branches_into_entries(&words, base, &entries).is_empty());

        // Forward, from 0x100000 into the entry at 0x100008: offset 1.
        let bne_fwd = 0x1400_0000u32 | 0x0001u32;
        let words = vec![bne_fwd, 0, 0, 0, 0, 0];
        assert!(back_branches_into_entries(&words, base, &entries).is_empty());
    }
}
