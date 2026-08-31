//! `ingest`: parses every translation input for the ICO retail US
//! recompiler into one `ProgramDb`, serializable to JSON.
//!
//! Inputs (paths resolved from `config/recomp.toml`'s `[decomp]` section):
//!   1. the boot ELF (`object` crate, SHA-1-pinned)
//!   2. the splat yaml (`config/ico.us.yaml`)'s subsegment list -> translation units
//!   3. `config/symbol_addrs.us.txt` -> functions and other symbols
//!   4. `asm/nonmatchings` + `asm/data` (+ `asm/matchings`) -> jump tables
//!   5. derived: a per-function table (size, TU membership, jtbl-owner flag)
//!
//! `ProgramDb` itself holds only addresses, names, and counts: never raw
//! ELF/ROM bytes or raw instruction words. Callers write it to
//! `generated/programdb.json`, which is gitignored; this crate never writes
//! anywhere itself.

mod config;
mod elf;
mod jumptables;
mod model;
mod splat;
mod symtab;

use std::collections::{HashMap, HashSet};
use std::path::Path;

use anyhow::{bail, Context, Result};

pub use config::RecompConfig;
pub use elf::ElfImage;
pub use model::{
    ElfSection, Function, JumpTable, ProgramDb, Stats, SubsegKind, Symbol, TranslationUnit,
};

impl ProgramDb {
    /// Parse every input reachable from `config_path` (normally
    /// `config/recomp.toml`) into one `ProgramDb`.
    pub fn load(config_path: &Path) -> Result<ProgramDb> {
        let cfg = RecompConfig::load(config_path)?;

        if !cfg.decomp_root.is_dir() {
            bail!(
                "decomp root {} does not exist (config/recomp.toml [decomp].root); \
                 the decomp repo is expected as a sibling checkout",
                cfg.decomp_root.display()
            );
        }

        let image = ElfImage::load(&cfg.elf_path, &cfg.elf_sha1)?;
        let translation_units = splat::load_translation_units(&cfg.splat_yaml_path)?;
        let raw_symbols = symtab::load_symbols(&cfg.symbol_addrs_path)?;
        let (raw_jtbls, owners) = jumptables::scan_asm_tree(&cfg.asm_dir)?;

        check_code_coverage(&image, &translation_units)?;

        let mut code_tus: Vec<(usize, &TranslationUnit)> = translation_units
            .iter()
            .enumerate()
            .filter(|(_, tu)| tu.kind.is_code())
            .collect();
        code_tus.sort_by_key(|(_, tu)| tu.vram_start);

        // Split symbol_addrs entries into functions vs. other symbols, and
        // pick out declared jtbl sizes while we're at it.
        let mut raw_funcs = Vec::new();
        let mut symbols = Vec::new();
        let mut declared_jtbl_size: HashMap<String, u32> = HashMap::new();
        for sym in raw_symbols {
            if sym.kind == "func" {
                raw_funcs.push(sym);
            } else {
                if sym.kind == "jtbl" {
                    if let Some(size) = sym.size {
                        declared_jtbl_size.insert(sym.name.clone(), size);
                    }
                }
                symbols.push(model::Symbol {
                    name: sym.name,
                    vram: sym.vram,
                    kind: sym.kind,
                    size: sym.size,
                    vendor: sym.vendor,
                    function_owner: sym.function_owner,
                    defined: sym.defined,
                });
            }
        }
        raw_funcs.sort_by_key(|f| f.vram);

        // Assign each function to its TU and compute its size. Jump table
        // ownership isn't known yet (`is_jtbl_target` is patched in below),
        // since resolving the target-majority fallback for asm/data-only
        // tables needs the function list to already exist.
        let mut functions = Vec::with_capacity(raw_funcs.len());
        for (i, f) in raw_funcs.iter().enumerate() {
            let tu_pos = code_tus
                .binary_search_by(|(_, tu)| {
                    if f.vram < tu.vram_start {
                        std::cmp::Ordering::Greater
                    } else if f.vram >= tu.vram_end {
                        std::cmp::Ordering::Less
                    } else {
                        std::cmp::Ordering::Equal
                    }
                })
                .map_err(|_| {
                    anyhow::anyhow!(
                        "function {} ({:#x}) does not fall inside any code translation unit",
                        f.name,
                        f.vram
                    )
                })?;
            let (tu_index, tu) = code_tus[tu_pos];

            let next_vram = raw_funcs.get(i + 1).map(|n| n.vram).unwrap_or(tu.vram_end);
            let bound = next_vram.min(tu.vram_end);
            let size = bound.saturating_sub(f.vram);

            if let Some(declared) = f.size {
                if declared != size {
                    eprintln!(
                        "warning: {} ({:#x}): derived size {:#x} != declared size:{:#x}",
                        f.name, f.vram, size, declared
                    );
                }
            }

            functions.push(model::Function {
                name: f.name.clone(),
                vram: f.vram,
                size,
                declared_size: f.size,
                tu_index,
                vendor: f.vendor,
                is_jtbl_target: false,
            });
        }

        // Consistency check: `vendor` is parsed from a fairly loose textual
        // marker (see symtab.rs); a threshold catches a future symbol_addrs
        // reformat silently zeroing it out again the way the literal
        // "(vendor)"-only match originally did (30 instead of ~945).
        let vendor_function_count = functions.iter().filter(|f| f.vendor).count();
        if vendor_function_count < 900 {
            bail!(
                "only {vendor_function_count} functions parsed as vendor-owned; expected \
                 roughly 930-950 (30 explicit \"(vendor)\" tags + ~920 \"vendor_\"-path \
                 functions). This almost certainly means the vendor-marker parsing in \
                 symtab.rs regressed."
            );
        }

        // Resolve jump table ownership. Tables discovered inside a
        // `glabel`-bodied .s file (nonmatchings/matchings) get their owner
        // from that textual reference. Tables that only exist in
        // asm/data/**/*.s (the owning function is already matched C, so
        // splat emits no per-function .s stub) fall back to the function
        // whose byte range contains the majority of the table's non-null
        // targets.
        let raw_jtbl_count = raw_jtbls.len();
        let mut jump_tables = Vec::with_capacity(raw_jtbl_count);
        let mut owner_names: HashSet<String> = HashSet::new();
        for jt in raw_jtbls {
            let candidates = owners.get(&jt.name);
            let (owner, via) = match candidates.map(|s| s.len()).unwrap_or(0) {
                0 => match resolve_owner_by_targets(&jt.targets, &functions) {
                    Some(name) => (name, model::OwnerResolution::TargetMajority),
                    None => bail!(
                        "jump table {} ({:#x}) has no resolvable owning function: no glabel \
                         body references it, and none of its {} target(s) fall inside a \
                         known function's byte range",
                        jt.name,
                        jt.vram,
                        jt.targets.iter().filter(|&&t| t != 0).count()
                    ),
                },
                1 => (
                    candidates.unwrap().iter().next().unwrap().clone(),
                    model::OwnerResolution::GlabelReference,
                ),
                _ => bail!(
                    "jump table {} ({:#x}) is referenced from multiple functions: {:?}",
                    jt.name,
                    jt.vram,
                    candidates.unwrap()
                ),
            };
            owner_names.insert(owner.clone());
            jump_tables.push(model::JumpTable {
                declared_size: declared_jtbl_size.get(&jt.name).copied(),
                name: jt.name,
                vram: jt.vram,
                owner,
                owner_resolved_via: via,
                entry_count: jt.targets.len(),
                targets: jt.targets,
            });
        }
        jump_tables.sort_by_key(|j| j.vram);

        // Every `dlabel jtbl_...` block found on disk must have produced a
        // ProgramDb entry (the loop above either pushes one or bails).
        if jump_tables.len() != raw_jtbl_count {
            bail!(
                "internal error: scanned {raw_jtbl_count} jump tables but only {} landed in \
                 ProgramDb",
                jump_tables.len()
            );
        }

        // Every jump table owner must be a real, known function.
        let function_names: HashSet<&str> = functions.iter().map(|f| f.name.as_str()).collect();
        for jt in &jump_tables {
            if !function_names.contains(jt.owner.as_str()) {
                bail!(
                    "jump table {} claims owner {}, which is not a known function \
                     (checked against symbol_addrs.us.txt's type:func entries)",
                    jt.name,
                    jt.owner
                );
            }
        }

        // Patch in `is_jtbl_target` now that every table's owner is known.
        for f in &mut functions {
            if owner_names.contains(&f.name) {
                f.is_jtbl_target = true;
            }
        }

        Ok(ProgramDb {
            elf_sha1: cfg.elf_sha1,
            entry_vram: cfg.entry,
            vram_base: cfg.vram_base,
            gp: cfg.gp,
            sections: image.sections,
            translation_units,
            symbols,
            functions,
            jump_tables,
        })
    }
}

/// Fallback owner resolution for jump tables with no `glabel` textual
/// reference (asm/data-only tables whose owning function is already
/// matched C). Every table's non-null targets land inside a single
/// function's byte range in this binary; pick whichever function contains
/// the most of them.
fn resolve_owner_by_targets(targets: &[u32], functions: &[model::Function]) -> Option<String> {
    let mut counts: HashMap<&str, usize> = HashMap::new();
    for &target in targets {
        if target == 0 {
            continue;
        }
        // `functions` is sorted by vram (built from vram-sorted raw_funcs).
        let idx = functions.partition_point(|f| f.vram <= target);
        if idx == 0 {
            continue;
        }
        let f = &functions[idx - 1];
        if target >= f.vram && target < f.vram + f.size {
            *counts.entry(f.name.as_str()).or_insert(0) += 1;
        }
    }
    counts
        .into_iter()
        .max_by_key(|(_, count)| *count)
        .map(|(name, _)| name.to_string())
}

/// Consistency check: every byte of `.text` and `.vutext` (the two
/// executable sections) must belong to exactly one code-kind translation
/// unit. Splat's subsegment list is built by construction to be contiguous
/// within a segment (each chunk's end is the next chunk's start), so a gap
/// here means the yaml and the ELF have drifted apart.
fn check_code_coverage(image: &ElfImage, tus: &[TranslationUnit]) -> Result<()> {
    let mut code_tus: Vec<&TranslationUnit> = tus.iter().filter(|t| t.kind.is_code()).collect();
    code_tus.sort_by_key(|t| t.vram_start);

    for pair in code_tus.windows(2) {
        if pair[0].vram_end > pair[1].vram_start {
            bail!(
                "code translation units {} ({:#x}..{:#x}) and {} ({:#x}..{:#x}) overlap",
                pair[0].name,
                pair[0].vram_start,
                pair[0].vram_end,
                pair[1].name,
                pair[1].vram_start,
                pair[1].vram_end
            );
        }
    }

    for section in &image.sections {
        if section.name != ".text" && section.name != ".vutext" {
            continue;
        }
        let mut cursor = section.vram;
        let section_end = section.vram_end();
        for tu in &code_tus {
            if tu.vram_end <= cursor || tu.vram_start >= section_end {
                continue;
            }
            if tu.vram_start > cursor {
                bail!(
                    "{}: gap {:#x}..{:#x} is not covered by any code translation unit",
                    section.name,
                    cursor,
                    tu.vram_start
                );
            }
            cursor = cursor.max(tu.vram_end.min(section_end));
        }
        if cursor < section_end {
            bail!(
                "{}: gap {:#x}..{:#x} is not covered by any code translation unit",
                section.name,
                cursor,
                section_end
            );
        }
    }

    Ok(())
}

/// Re-open and byte-verify the boot ELF for callers that need actual bytes
/// (the decoder, the emitter's three-way verification), without paying for
/// that on every `ProgramDb::load`.
pub fn load_elf_image(config_path: &Path) -> Result<ElfImage> {
    let cfg = RecompConfig::load(config_path)?;
    ElfImage::load(&cfg.elf_path, &cfg.elf_sha1)
}

/// Serialize a `ProgramDb` to pretty JSON. Callers are responsible for
/// writing it to `generated/programdb.json` (gitignored) — this crate never
/// touches the filesystem for output.
pub fn to_json(db: &ProgramDb) -> Result<String> {
    serde_json::to_string_pretty(db).context("serializing ProgramDb to JSON")
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn config_path() -> PathBuf {
        // crates/ingest -> crates -> recomp -> tools -> repo root
        PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../..")
            .join("config/recomp.toml")
    }

    fn decomp_present() -> bool {
        RecompConfig::load(&config_path())
            .map(|c| c.decomp_root.is_dir())
            .unwrap_or(false)
    }

    #[test]
    fn loads_real_decomp_repo() {
        if !decomp_present() {
            eprintln!(
                "skipping: ../ico decomp repo not found next to ico-recomp; \
                 this test only runs in an environment with the sibling checkout"
            );
            return;
        }

        let db = ProgramDb::load(&config_path()).expect("ProgramDb::load should succeed");
        let stats = db.stats();

        eprintln!("{stats:#?}");

        assert_eq!(db.functions.len(), 5447, "expected 5447 type:func entries");
        let vendor_count = db.functions.iter().filter(|f| f.vendor).count();
        assert!(
            (900..=1000).contains(&vendor_count),
            "expected roughly 930-950 vendor-tagged functions, got {vendor_count}"
        );
        // Ground truth, independently re-derived: `grep -rhoE '^\s*dlabel
        // jtbl_[0-9A-Fa-f]+' ../ico/asm/{nonmatchings,data,matchings} | sort
        // -u | wc -l` == 101. (A naive `grep -c 'dlabel jtbl_'` over the
        // tree reports 202, but that's double-counting: `enddlabel
        // jtbl_XXXXXXXX` also contains the substring `dlabel
        // jtbl_XXXXXXXX`, since "enddlabel" ends in "dlabel".)
        assert_eq!(stats.jump_tables, 101, "expected 101 unique jump tables");
        assert!(stats.jump_tables_via_glabel > 0);
        // In the current ../ico checkout every jtbl-owning function still
        // has a nonmatchings/matchings .s stub (the tables only present
        // under asm/data have their *.rodata carve* consolidated there,
        // but the *referencing function*'s .text stub is unaffected and
        // still resolves via glabel), so the fallback isn't exercised
        // today. It stays wired up for when decomp progress changes that.
        assert_eq!(stats.jump_tables_via_target_majority, 0);
        assert_eq!(
            stats.jump_tables_via_glabel + stats.jump_tables_via_target_majority,
            stats.jump_tables
        );
        assert!(stats.translation_units > 0);
        assert!(stats.code_translation_units > 0);

        // Every function must resolve to a real TU whose kind is code.
        for f in &db.functions {
            let tu = &db.translation_units[f.tu_index];
            assert!(
                tu.kind.is_code(),
                "{} assigned to non-code TU {}",
                f.name,
                tu.name
            );
            assert!(tu.contains_vram(f.vram));
        }

        // Every jump table owner must be a known function, and every jtbl
        // target should land inside its owner's own byte range (the switch
        // statements in this binary only branch to labels within the same
        // function).
        for jt in &db.jump_tables {
            let owner = db
                .functions
                .iter()
                .find(|f| f.name == jt.owner)
                .unwrap_or_else(|| panic!("jump table {} owner {} not found", jt.name, jt.owner));
            for &target in &jt.targets {
                if target == 0 {
                    continue; // genuine null/pad slot
                }
                assert!(
                    target >= owner.vram && target < owner.vram + owner.size,
                    "{}: target {:#x} outside owner {} ({:#x}..{:#x})",
                    jt.name,
                    target,
                    owner.name,
                    owner.vram,
                    owner.vram + owner.size
                );
            }
        }

        // Round-trips through JSON without loss of the top-level shape.
        let json = to_json(&db).expect("serialize");
        let back: ProgramDb = serde_json::from_str(&json).expect("deserialize");
        assert_eq!(back.functions.len(), db.functions.len());
    }

    #[test]
    fn function_lookup_helpers_agree() {
        if !decomp_present() {
            eprintln!("skipping: ../ico decomp repo not found");
            return;
        }
        let db = ProgramDb::load(&config_path()).unwrap();
        let f = &db.functions[100];
        assert_eq!(db.function_by_vram(f.vram).unwrap().name, f.name);
        assert_eq!(db.function_at(f.vram).unwrap().name, f.name);
        assert_eq!(
            db.function_at(f.vram + 1).map(|f| f.name.as_str()),
            Some(f.name.as_str())
        );
    }

    /// Synthetic coverage for the target-majority fallback: the real ../ico
    /// checkout never currently exercises it (every jtbl-owning function
    /// still has a glabel stub), so this doesn't depend on the repo.
    #[test]
    fn target_majority_fallback_picks_the_containing_function() {
        fn func(name: &str, vram: u32, size: u32) -> model::Function {
            model::Function {
                name: name.to_string(),
                vram,
                size,
                declared_size: None,
                tu_index: 0,
                vendor: false,
                is_jtbl_target: false,
            }
        }

        let functions = vec![
            func("func_a", 0x1000, 0x40), // 0x1000..0x1040
            func("func_b", 0x1040, 0x80), // 0x1040..0x10c0
            func("func_c", 0x10c0, 0x20), // 0x10c0..0x10e0
        ];

        // Three targets land in func_b, one in func_a, one is a null pad
        // slot: func_b should win the plurality vote.
        let targets = [0x1050, 0x1060, 0x1070, 0x1010, 0];
        assert_eq!(
            resolve_owner_by_targets(&targets, &functions),
            Some("func_b".to_string())
        );

        // A table whose only non-null targets fall outside every known
        // function's range is unresolvable.
        let targets = [0x9999, 0];
        assert_eq!(resolve_owner_by_targets(&targets, &functions), None);

        // All-null table is unresolvable too (nothing to vote on).
        let targets = [0, 0, 0];
        assert_eq!(resolve_owner_by_targets(&targets, &functions), None);
    }
}
