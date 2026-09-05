//! `ProgramDb` construction from the disc's own inputs: the retail ELF and
//! the development-build objdump listing the disc carries.
//!
//! The pipeline, in the order the steps depend on each other:
//!
//! 1. Parse the objdump listing (`objdump`) into donor functions: name,
//!    source file, and the instruction words the listing printed.
//! 2. Correlate those functions onto the retail `.text` by masked
//!    fingerprint (`correlate`). This is what turns the listing's addresses,
//!    which belong to a different link, into addresses in this ELF.
//! 3. Add the entries the ELF itself proves: all five proofs in `scan`. A
//!    correlated function keeps its donor name; anything only the scan
//!    found becomes provisional, `func_XXXXXXXX`.
//! 4. Size every function as the distance to the next entry, capped at the
//!    section end.
//! 5. Group consecutive functions by donor source file into translation
//!    units, made contiguous by construction so the coverage check in
//!    `lib.rs` holds.
//! 6. Carve `.vutext` into its five microprogram translation units by
//!    walking the DMA framing the section describes about itself.
//! 7. Recover jump tables from `.rodata` (`scan::scan_jump_tables`).
//!
//! What no input carries is a data symbol table for this ELF, so nothing
//! here names a data address. A question about one is answered by finding
//! the instruction that materializes it.

use std::collections::{BTreeMap, HashSet};

use anyhow::{bail, Context, Result};

use crate::config::RecompConfig;
use crate::correlate::{correlate, Correlation};
use crate::elf::ElfImage;
use crate::model::{
    ElfSection, Function, JumpTable, ProgramDb, SubsegKind, TranslationUnit,
};
use crate::objdump::{DonorFunction, Objdump};
use crate::scan;

/// Minimum entries a `.rodata` word run must have before it counts as a
/// switch table. Three is the smallest a compiler emits (below that it
/// builds a compare chain), and it is high enough that the single-owner
/// requirement makes a false positive very unlikely.
const MIN_JTBL_ENTRIES: usize = 3;

/// Everything the correlation established, kept for the CLI to report.
pub struct DiscIngest {
    pub db: ProgramDb,
    pub objdump: Objdump,
    pub correlation: Correlation,
    /// Functions that exist in the retail ELF but that no donor function
    /// could be matched to, named `func_XXXXXXXX`.
    pub unnamed_functions: usize,
    /// Donor functions with no counterpart in the retail ELF.
    pub donor_functions_lost: usize,
    /// Donor labels that were not function entries (assembly-local labels
    /// the dev build exported) and were folded back into the function that
    /// contains them.
    pub interior_labels: usize,
    /// `j` targets that passed the positional test but had neither a
    /// prologue nor a donor name, and were therefore not made entries.
    pub jump_targets_dropped: usize,
    /// `j` targets that became entries on evidence beyond their position.
    pub jump_targets_kept: usize,
    /// Entries moved one word later because the word before them is a
    /// control instruction and they are its delay slot.
    pub delay_slot_moves: Vec<u32>,
    /// Sections the scan could not read, named so a lost entry proof is
    /// visible rather than silent.
    pub unreadable_sections: Vec<String>,
    /// Backward branches into a function entry: the hazard that turns a
    /// loop into unbounded recursion once the boundary is a call. Fatal
    /// when non-empty (`load` bails), so this is always zero here; the
    /// count is reported so a regression is visible.
    pub back_branch_hazards: usize,
}

pub fn load(cfg: &RecompConfig, image: ElfImage) -> Result<DiscIngest> {
    let disc = &cfg.disc;
    let objdump = Objdump::load(&disc.objdump_path)?;

    let text = section(&image, ".text")?.clone();
    let vutext = section(&image, ".vutext")?.clone();

    // ---- correlate the donor listing onto this ELF -------------------------
    let text_words = words_of(&image, &text)?;
    let donor_text: Vec<&DonorFunction> = objdump
        .section_functions(".text")
        .filter(|f| !f.insns.is_empty())
        .collect();
    if donor_text.is_empty() {
        bail!(
            "{}: no .text functions parsed; the objdump listing is not in the expected \
             `ADDR <name>:` / `  addr:\\tword \\ttext` form",
            disc.objdump_path.display()
        );
    }
    let correlation = correlate(&donor_text, &text_words, text.vram);

    let mut name_at: BTreeMap<u32, &DonorFunction> = BTreeMap::new();
    for f in &donor_text {
        if let Some(&target) = correlation.to_target.get(&f.vram) {
            // Two donor functions resolving to one address would make the
            // name ambiguous; the first in donor order wins and the second
            // is treated as lost, which the report counts.
            name_at.entry(target).or_insert(f);
        }
    }

    // ---- entries the ELF itself proves -------------------------------------
    let scanned = scan::scan_text(&text_words, text.vram);
    // The fourth proof (scan::data_pointers_to_prologues): an aligned word of
    // any on-disk non-.text section holding the address of a prologue. The
    // table-held functions (thread entries, state and switch tables) that
    // no jal and no instruction-formed pointer names.
    let mut data_prologue_targets: Vec<u32> = Vec::new();
    let mut unreadable_sections: Vec<String> = Vec::new();
    for sec in image.sections.iter().filter(|s| s.name != ".text" && !s.nobits && s.vram != 0 && s.size >= 4) {
        // By file offset, not by vram: two sections of this ELF share a
        // vram (.DVP.ovlytab and .DVP.ovlystrtab, both at 0x736198) and a
        // vram read of the second resolves to the first. See
        // ElfImage::section_bytes.
        let Some(bytes) = image.section_bytes(sec) else {
            unreadable_sections.push(sec.name.clone());
            continue;
        };
        let data_words: Vec<u32> = bytes
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect();
        for (target, _holder) in scan::data_pointers_to_prologues(&data_words, sec.vram, &text_words, text.vram) {
            data_prologue_targets.push(target);
        }
    }
    data_prologue_targets.sort_unstable();
    data_prologue_targets.dedup();
    let shape_targets = scan::prologues_after_transfers(&text_words, text.vram);
    // A `j` target is the one candidate class with no test on the words at
    // the target, and `prologues_after_transfers` explicitly refuses an
    // address any branch or `j` reaches, so admitting one on position alone
    // re-admits exactly what that proof rejected, on weaker evidence. It
    // becomes an entry only with something the words or the listing say:
    // a prologue at the target, or a donor function correlated onto it.
    // Measured on SCES_507.60, 2026-09-05: this keeps the matrix composer
    // at 0x001146F0, whose only caller is the backward `j` at 0x00114E4C
    // and which `prologue_at` accepts.
    let mut donor_named: HashSet<u32> = name_at.keys().copied().collect();
    let jump_ok = |a: u32, donor_named: &HashSet<u32>| {
        scan::prologue_at(&text_words, text.vram, a) || donor_named.contains(&a)
    };
    let mut jump_targets_dropped = 0usize;
    let mut jump_targets_kept = 0usize;
    let mut entries: Vec<u32> = Vec::new();
    entries.push(cfg.entry);
    entries.extend(name_at.keys().copied());
    entries.extend(scanned.jal_targets.iter().copied());
    entries.extend(scanned.pointer_targets.iter().copied());
    entries.extend(scanned.prologue_targets.iter().copied());
    entries.extend(data_prologue_targets.iter().copied());
    entries.extend(shape_targets.iter().copied());
    for &a in &scanned.jump_targets {
        if jump_ok(a, &donor_named) {
            entries.push(a);
        }
    }
    entries.retain(|&a| a >= text.vram && a < text.vram_end() && a.is_multiple_of(4));
    entries.sort_unstable();
    entries.dedup();

    // The donor listing labels more than functions. Hand-written assembly
    // in the SDK exports its own local labels (`sceSifWriteBackDCache` is
    // followed by `loop1`, `eight`, `loop8`, `last`), and the dev build's
    // symbol table carries them, so taking every label as an entry chops a
    // routine into four-byte fragments and leaves a branch with its delay
    // slot outside the fragment it belongs to. A label is kept as an entry
    // only when something proves it is one: the ELF entry point, a `jal`
    // target, a pointer taken after a function end, or a position that
    // follows an unconditional transfer and its delay slot, which is how
    // every function in this binary begins.
    let proven: HashSet<u32> = std::iter::once(cfg.entry)
        .chain(scanned.jal_targets.iter().copied())
        .chain(scanned.pointer_targets.iter().copied())
        .chain(scanned.prologue_targets.iter().copied())
        .chain(data_prologue_targets.iter().copied())
        .chain(shape_targets.iter().copied())
        .collect();
    // An entry that is the delay slot of the control instruction before it
    // cannot be a function of its own: the emitter would be left with a
    // control instruction whose delay slot lies outside its function. The
    // the emitter would refuse to translate the function that ends there.
    // Measured on SCES_507.60 at 0x00265AE8, the SetAlarm callback the
    // listing names CB_DelayTh, whose preceding word is a lone jr $ra; the
    // runtime's alarm HLE enters it a word later (ee/alarms.cpp).
    let word_at = |vram: u32| -> Option<u32> {
        let i = vram.checked_sub(text.vram)? / 4;
        text_words.get(i as usize).copied()
    };
    // The word after such a delay slot becomes the entry instead: the
    // delay slot executes as the previous function's last instruction and
    // as the callback's first, and the runtime's alarm HLE enters the
    // callback one word later after stepping the shared instruction itself
    // (ee/alarms.cpp, "1 skipped stack adjustment"). Without this entry the
    // HLE has nothing to enter and the run is a fatal; measured 2026-09-05.
    let mut moved: Vec<u32> = Vec::new();
    let mut move_failures: Vec<String> = Vec::new();
    entries.retain(|&a| {
        if a == cfg.entry || !word_at(a.wrapping_sub(4)).is_some_and(crate::is_control_word) {
            return true;
        }
        // Two cases the move cannot describe, and both are loud rather than
        // quietly wrong. A `jal` target is entered by a call, so it is the
        // first instruction of its function and cannot also be another
        // instruction's delay slot; moving it would drop that instruction
        // from every call. And the last word of `.text` has no word after
        // it to move to.
        if scanned.jal_targets.contains(&a) {
            move_failures.push(format!(
                "{a:#010X} is the target of a jal and is also the delay slot of the \
                 control instruction at {:#010X}. One of the two readings is wrong and \
                 this ingest cannot tell which, so nothing was moved.",
                a - 4
            ));
            return true;
        }
        if a + 4 >= text.vram_end() {
            move_failures.push(format!(
                "{a:#010X} is the delay slot of the control instruction at {:#010X} and \
                 is the last word of .text, so there is no word after it to move the \
                 entry to.",
                a - 4
            ));
            return true;
        }
        moved.push(a + 4);
        false
    });
    if !move_failures.is_empty() {
        bail!("delay-slot entry moves that cannot be made:\n  {}", move_failures.join("\n  "));
    }
    // The donor name follows the entry: the listing calls the function at
    // 0x00265AE8 CB_DelayTh, and the entry that stands for it is at
    // 0x00265AEC.
    for &m in &moved {
        if let Some(donor) = name_at.remove(&(m - 4)) {
            donor_named.remove(&(m - 4));
            donor_named.insert(m);
            name_at.entry(m).or_insert(donor);
        }
    }
    entries.extend(moved.iter().copied());
    entries.sort_unstable();
    entries.dedup();
    let first = entries.first().copied().unwrap_or(text.vram);
    let mut interior_labels = 0usize;
    entries.retain(|&a| {
        if a == first || proven.contains(&a) || moved.contains(&a)
            || scan::follows_a_return(&text_words, text.vram, a) {
            true
        } else {
            interior_labels += 1;
            false
        }
    });

    // What the tightened `j` rule cost and kept, over the addresses no other
    // proof reached. Reported every run.
    for &a in &scanned.jump_targets {
        if proven.contains(&a) || a == cfg.entry {
            continue;
        }
        if !scan::follows_a_return(&text_words, text.vram, a) {
            continue;
        }
        if jump_ok(a, &donor_named) {
            jump_targets_kept += 1;
        } else {
            jump_targets_dropped += 1;
        }
    }

    // The hazard a boundary can create: a branch that crosses it backwards
    // into an entry. The emitter renders that as a call and a return, so a
    // guest loop through it adds a native stack frame per iteration and
    // never unwinds it. Stated once in
    // `crate::back_branches_into_entries`, which says exactly which
    // directions are hazards and why, and run here over the final entry
    // list. The count is reported even when it is zero so that a change to
    // the proof set cannot reintroduce the hazard silently.
    let back_branches = crate::back_branches_into_entries(&text_words, text.vram, &entries);
    if !back_branches.is_empty() {
        let mut lines = String::new();
        for (entry, site) in back_branches.iter().take(20) {
            let _ = std::fmt::Write::write_fmt(
                &mut lines,
                format_args!("\n  entry {entry:#010X} is the target of the branch at {site:#010X}"),
            );
        }
        bail!(
            "{} function entr(ies) are the target of a branch that crosses a function \
             boundary backwards. The emitter renders such a branch as a call and a \
             return, so a loop through it would add a stack frame per iteration.{lines}",
            back_branches.len()
        );
    }

    // ---- translation units -------------------------------------------------
    // Consecutive functions sharing a donor source file form one TU. A
    // function with no donor name inherits the TU it falls inside, so the
    // TU list stays contiguous and the coverage check holds.
    let mut tu_of_entry: Vec<usize> = Vec::with_capacity(entries.len());
    let mut translation_units: Vec<TranslationUnit> = Vec::new();
    let mut current_source: Option<String> = None;
    for &addr in &entries {
        let source = name_at
            .get(&addr)
            .and_then(|f| f.source_file.clone())
            .or_else(|| current_source.clone());
        let starts_new = translation_units.is_empty() || source != current_source;
        if starts_new {
            if let Some(last) = translation_units.last_mut() {
                last.vram_end = addr;
                last.rom_end = addr;
            }
            let start = if translation_units.is_empty() {
                text.vram
            } else {
                addr
            };
            translation_units.push(TranslationUnit {
                name: tu_name(source.as_deref(), addr),
                kind: SubsegKind::Code,
                raw_kind: "c".to_string(),
                carved: false,
                rom_start: start,
                rom_end: text.vram_end(),
                vram_start: start,
                vram_end: text.vram_end(),
            });
            current_source = source;
        }
        tu_of_entry.push(translation_units.len() - 1);
    }

    // ---- .vutext: five microprogram TUs, from the section's own framing ----
    let vutext_bytes = image
        .read_at(vutext.vram, vutext.size as usize)
        .with_context(|| format!("reading .vutext at {:#x}", vutext.vram))?;
    let vu_names = vutext_program_names(&objdump);
    for (i, (start, size)) in dma_fragments(vutext_bytes, vutext.vram)?.iter().enumerate() {
        let name = vu_names
            .get(i)
            .cloned()
            .unwrap_or_else(|| format!("vu1_{i}"));
        translation_units.push(TranslationUnit {
            name: format!("src/{name}"),
            kind: SubsegKind::HandAsm,
            raw_kind: "hasm".to_string(),
            carved: false,
            rom_start: *start,
            rom_end: start + size,
            vram_start: *start,
            vram_end: start + size,
        });
    }

    // ---- functions ---------------------------------------------------------
    let mut functions: Vec<Function> = Vec::with_capacity(entries.len());
    let mut unnamed = 0usize;
    for (i, &addr) in entries.iter().enumerate() {
        let next = entries.get(i + 1).copied().unwrap_or(text.vram_end());
        let size = next.min(text.vram_end()).saturating_sub(addr);
        if size == 0 {
            bail!("two function entries share the address {addr:#010x}");
        }
        let donor = name_at.get(&addr);
        let name = match donor {
            Some(f) => f.name.clone(),
            None => {
                unnamed += 1;
                format!("func_{addr:08X}")
            }
        };
        let tu_index = tu_of_entry[i];
        let vendor = match donor {
            Some(f) => f.is_vendor(),
            // No donor name: inherit the vendor status of the TU it fell
            // inside, which is the donor source file of its neighbours.
            None => is_vendor_tu(&translation_units[tu_index].name),
        };
        functions.push(Function {
            name,
            vram: addr,
            size,
            declared_size: None,
            tu_index,
            vendor,
            is_jtbl_target: false,
        });
    }

    // ---- jump tables from .rodata -----------------------------------------
    let ranges: Vec<(u32, u32)> = functions.iter().map(|f| (f.vram, f.vram + f.size)).collect();
    let mut jump_tables = Vec::new();
    let mut owners: HashSet<String> = HashSet::new();
    if let Ok(rodata) = section(&image, ".rodata") {
        let bytes = image
            .read_at(rodata.vram, rodata.size as usize)
            .with_context(|| format!("reading .rodata at {:#x}", rodata.vram))?;
        for table in scan::scan_jump_tables(bytes, rodata.vram, &ranges, MIN_JTBL_ENTRIES) {
            let owner_idx = ranges
                .partition_point(|&(start, _)| start <= table.targets[0])
                .saturating_sub(1);
            let owner = functions[owner_idx].name.clone();
            owners.insert(owner.clone());
            jump_tables.push(JumpTable {
                name: format!("jtbl_{:08X}", table.vram),
                vram: table.vram,
                // Nothing textual claims this table: ownership is the
                // function whose byte range its targets land in, which is
                // what makes a run of words a switch table in the first
                // place.
                owner,
                entry_count: table.targets.len(),
                targets: table.targets,
                declared_size: None,
            });
        }
    }
    for f in &mut functions {
        if owners.contains(&f.name) {
            f.is_jtbl_target = true;
        }
    }

    // The whole-.text sweep, against the entries this path settled on: an
    // address a lui/addiu pair forms that is not one of them is an indirect
    // call the runtime could not resolve.
    let sweep_funcs: Vec<(u32, String)> = functions.iter().map(|f| (f.vram, f.name.clone())).collect();
    let sweep_jtbl: std::collections::BTreeSet<u32> = jump_tables
        .iter()
        .flat_map(|t| t.targets.iter().copied())
        .filter(|&t| t != 0)
        .collect();
    let unresolved_pointers =
        crate::unresolved_pointers(&scanned.pointers, &sweep_funcs, &sweep_jtbl);

    // Every donor function is either placed or unresolved; a placed one
    // can still lose its name if two donor functions collapsed onto the
    // same retail address, which the count below makes visible.
    let donor_lost = correlation.unresolved.len() + (correlation.to_target.len() - name_at.len());

    Ok(DiscIngest {
        db: ProgramDb {
            elf_sha1: cfg.elf_sha1.clone(),
            entry_vram: cfg.entry,
            vram_base: cfg.vram_base,
            gp: cfg.gp,
            sections: image.sections.clone(),
            translation_units,
            functions,
            jump_tables,
            unresolved_pointers,
        },
        objdump,
        correlation,
        unnamed_functions: unnamed,
        donor_functions_lost: donor_lost,
        interior_labels,
        jump_targets_dropped,
        jump_targets_kept,
        delay_slot_moves: moved,
        unreadable_sections,
        back_branch_hazards: back_branches.len(),
    })
}

fn section<'a>(image: &'a ElfImage, name: &str) -> Result<&'a ElfSection> {
    image
        .sections
        .iter()
        .find(|s| s.name == name)
        .with_context(|| format!("no {name} section in the target ELF"))
}

fn words_of(image: &ElfImage, section: &ElfSection) -> Result<Vec<u32>> {
    let bytes = image
        .read_at(section.vram, section.size as usize)
        .with_context(|| format!("reading {} at {:#x}", section.name, section.vram))?;
    Ok(bytes
        .chunks_exact(4)
        .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
        .collect())
}

/// A translation unit name from a donor source path. The path is a file
/// name for game code (`way_util.c`) and an absolute path for vendor code
/// (`/usr/local/sce/ee/lib/crt0.s`); both are reduced to a splat-like
/// `src/<stem>`, which is what the emitted C file is named after.
fn tu_name(source: Option<&str>, first_addr: u32) -> String {
    match source {
        Some(path) => {
            let file = path.rsplit('/').next().unwrap_or(path);
            let stem = file.rsplit_once('.').map(|(s, _)| s).unwrap_or(file);
            if path.starts_with("/usr/local/") || path.contains("/gcc-lib/") {
                format!("src/vendor/{stem}")
            } else {
                format!("src/{stem}")
            }
        }
        None => format!("src/cod/{first_addr:06X}"),
    }
}

/// Vendor means Sony SDK code, which the donor listing marks by its source
/// path (`/usr/local/sce/...`, `/gcc-lib/...`). `src/cod/<addr>` is the
/// opposite: a run of functions no donor function named at all, so its
/// provenance is unknown and calling it vendor was wrong.
fn is_vendor_tu(name: &str) -> bool {
    name.starts_with("src/vendor/")
}

/// Walk the `.vutext` DMA framing and return each fragment's
/// `(vram, size)`. Each microprogram is a self-contained source-chain
/// fragment: one tag quadword whose QWC counts the payload, the payload,
/// then one end-tag quadword, so a fragment is `(QWC + 2) * 16` bytes and
/// the section is exactly the fragments back to back. Reading the framing
/// rather than trusting a symbol table is what makes this work on an ELF
/// with no symbols.
fn dma_fragments(bytes: &[u8], vram: u32) -> Result<Vec<(u32, u32)>> {
    let mut out = Vec::new();
    let mut off = 0usize;
    while off < bytes.len() {
        if off + 16 > bytes.len() {
            bail!(".vutext: trailing {} bytes are not a whole quadword", bytes.len() - off);
        }
        let tag = u32::from_le_bytes([bytes[off], bytes[off + 1], bytes[off + 2], bytes[off + 3]]);
        let id = (tag >> 28) & 0x7;
        let qwc = tag & 0xFFFF;
        if id != 6 {
            bail!(
                ".vutext: expected a DMA `ret` tag (id 6) at {:#010x}, got id {id} (tag {tag:#010x})",
                vram + off as u32
            );
        }
        let size = (qwc + 2) * 16;
        if off + size as usize > bytes.len() {
            bail!(
                ".vutext: fragment at {:#010x} claims {size:#x} bytes, past the section end",
                vram + off as u32
            );
        }
        out.push((vram + off as u32, size));
        off += size as usize;
    }
    Ok(out)
}

/// The five microprogram names, taken from the donor listing's `.vutext`
/// labels (`ClusterMicroProgram`, `NormalCMicroProgram`, ...) and converted
/// to the lower-case, underscore-separated form the emitted files and the
/// runtime's registration table use (`cluster`, `normal_c`).
fn vutext_program_names(objdump: &Objdump) -> Vec<String> {
    let mut names: Vec<String> = Vec::new();
    for f in objdump.section_functions(".vutext") {
        let Some(stem) = f.name.strip_suffix("MicroProgram") else {
            continue;
        };
        let mut out = String::new();
        for (i, ch) in stem.chars().enumerate() {
            if ch.is_ascii_uppercase() && i > 0 {
                out.push('_');
            }
            out.push(ch.to_ascii_lowercase());
        }
        names.push(out);
    }
    names
}

/// Everything the ingest measured, printed by the `ee` run.
///
/// The numbers quoted in the module comments and in `correlate` come from
/// this text, so a change to a rule shows up here first.
pub fn describe(ingest: &DiscIngest) -> String {
    use std::fmt::Write as _;
    let mut s = String::new();
    let corr = &ingest.correlation;
    let _ = writeln!(
        s,
        "correlation: {} donor functions placed ({} unique fingerprint anchors, {} by \
         neighbour delta), {} unresolved, {} anchors dropped as non-monotonic",
        corr.to_target.len(),
        corr.anchors(),
        corr.by_delta(),
        corr.unresolved.len(),
        corr.non_monotonic_dropped
    );
    let _ = writeln!(
        s,
        "functions: {} total, {} named from the donor listing, {} provisional func_XXXXXXXX",
        ingest.db.functions.len(),
        ingest.db.functions.len() - ingest.unnamed_functions,
        ingest.unnamed_functions
    );
    let vendor = ingest.db.functions.iter().filter(|f| f.vendor).count();
    let _ = writeln!(
        s,
        "vendor split: {} vendor (a Sony SDK source path in the listing), {} game or \
         unattributed",
        vendor,
        ingest.db.functions.len() - vendor
    );
    let _ = writeln!(
        s,
        "donor labels folded back as interior labels: {}",
        ingest.interior_labels
    );
    let _ = writeln!(
        s,
        "donor functions with no counterpart in the retail ELF: {}",
        ingest.donor_functions_lost
    );
    let _ = writeln!(
        s,
        "j targets: {} kept as entries (a prologue at the target or a donor name, on top \
         of the position), {} dropped for having neither",
        ingest.jump_targets_kept, ingest.jump_targets_dropped
    );
    let _ = write!(
        s,
        "entries moved one word later (the word before them is a control instruction and \
         they are its delay slot): {}",
        ingest.delay_slot_moves.len()
    );
    for m in &ingest.delay_slot_moves {
        let _ = write!(s, " {:#010X}->{:#010X}", m - 4, m);
    }
    let _ = writeln!(s);
    let _ = writeln!(
        s,
        "branches crossing a function boundary backwards into an entry (a loop that \
         would become unbounded recursion; the run fails when this is not zero): {}",
        ingest.back_branch_hazards
    );
    let smallest = ingest
        .db
        .jump_tables
        .iter()
        .map(|t| t.entry_count)
        .min()
        .unwrap_or(0);
    let at_minimum = ingest
        .db
        .jump_tables
        .iter()
        .filter(|t| t.entry_count == MIN_JTBL_ENTRIES)
        .count();
    let _ = writeln!(
        s,
        "translation units: {}, jump tables: {} ({} of them at the {}-entry minimum, \
         smallest {})",
        ingest.db.translation_units.len(),
        ingest.db.jump_tables.len(),
        at_minimum,
        MIN_JTBL_ENTRIES,
        smallest
    );
    if !ingest.unreadable_sections.is_empty() {
        let _ = writeln!(
            s,
            "warning: {} section(s) could not be read and were not scanned for data \
             pointers, so an entry proof may be missing: {}",
            ingest.unreadable_sections.len(),
            ingest.unreadable_sections.join(", ")
        );
    }
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dma_framing_splits_a_section_into_fragments() {
        // Two fragments: QWC 1 and QWC 2, so 3 and 4 quadwords.
        let mut bytes = Vec::new();
        let mut tag = |qwc: u32| {
            let w = (6u32 << 28) | qwc;
            bytes.extend_from_slice(&w.to_le_bytes());
            bytes.extend_from_slice(&[0u8; 12]);
            for _ in 0..qwc + 1 {
                bytes.extend_from_slice(&[0u8; 16]);
            }
        };
        tag(1);
        tag(2);
        let frags = dma_fragments(&bytes, 0x0028_9BD0).expect("framing parses");
        assert_eq!(frags, vec![(0x0028_9BD0, 0x30), (0x0028_9C00, 0x40)]);
    }

    #[test]
    fn a_non_ret_tag_is_a_loud_error() {
        let bytes = vec![0u8; 32];
        let err = dma_fragments(&bytes, 0).unwrap_err().to_string();
        assert!(err.contains("expected a DMA `ret` tag"), "{err}");
    }

    #[test]
    fn translation_unit_names_follow_the_source_path() {
        assert_eq!(tu_name(Some("way_util.c"), 0), "src/way_util");
        assert_eq!(
            tu_name(Some("/usr/local/sce/ee/lib/crt0.s"), 0),
            "src/vendor/crt0"
        );
        assert_eq!(tu_name(None, 0x0010_0008), "src/cod/100008");
    }

    /// The whole live path over a synthetic binary: five donor functions,
    /// a data word holding a function pointer, a `.rodata` jump table and a
    /// `.vutext` of two DMA fragments. This is the only test that runs
    /// `disc::load` end to end, and it needs no ELF on disk, so it runs in
    /// CI where the retail binary is absent.
    fn synthetic_ingest() -> DiscIngest {
        const TEXT: u32 = 0x0010_0000;
        // 0x100000 funcA: frame, save, jal funcB, delay slot, jr $ra, pad*3
        // 0x100020 funcB: frame, save, jr $ra, pad
        // 0x100030 funcC: frame, save $s0, jr $ra, pad
        let jal_b = 0x0C00_0000 | ((0x0010_0020u32 >> 2) & 0x03FF_FFFF);
        let words: Vec<u32> = vec![
            0x27BD_FFF0, 0xFFBF_0000, jal_b, 0x0000_0000,
            0x03E0_0008, 0x0000_0000, 0x0000_0000, 0x0000_0000,
            0x27BD_FFF0, 0xFFBF_0000, 0x03E0_0008, 0x0000_0000,
            0x27BD_FFE0, 0xFFB0_0010, 0x03E0_0008, 0x0000_0000,
        ];
        let mut text_bytes = Vec::new();
        for w in &words {
            text_bytes.extend_from_slice(&w.to_le_bytes());
        }
        // .rodata: a three-entry switch table, every target inside funcA.
        let mut rodata = Vec::new();
        for t in [0x0010_0004u32, 0x0010_0008, 0x0010_000C] {
            rodata.extend_from_slice(&t.to_le_bytes());
        }
        // .data: one word holding funcC's entry, which no jal and no
        // instruction-formed pointer names.
        let data_sec = 0x0010_0030u32.to_le_bytes().to_vec();
        // .vutext: two source-chain fragments, QWC 1 and QWC 2.
        let mut vutext = Vec::new();
        for qwc in [1u32, 2] {
            vutext.extend_from_slice(&((6u32 << 28) | qwc).to_le_bytes());
            vutext.extend_from_slice(&[0u8; 12]);
            vutext.extend_from_slice(&vec![0u8; ((qwc + 1) * 16) as usize]);
        }

        let mut image_bytes = Vec::new();
        let mut section = |name: &str, vram: u32, bytes: &[u8], exec: bool| {
            let file_offset = image_bytes.len() as u32;
            image_bytes.extend_from_slice(bytes);
            ElfSection {
                name: name.to_string(),
                vram,
                file_offset,
                size: bytes.len() as u32,
                executable: exec,
                writable: false,
                nobits: false,
            }
        };
        let sections = vec![
            section(".text", TEXT, &text_bytes, true),
            section(".rodata", 0x0020_0000, &rodata, false),
            section(".data", 0x0030_0000, &data_sec, false),
            section(".vutext", 0x0040_0000, &vutext, true),
        ];
        let image = ElfImage::from_parts(TEXT, sections, image_bytes);

        // The donor listing. Every body is identical to the target's at the
        // same address, so nothing anchors (each run is shorter than
        // KEY_LEN) and everything is placed by the delta-0 hypothesis.
        // `loop1` is an assembly-local label inside funcA, and `CB` sits on
        // the delay slot of funcA's jal.
        let insn = |vram: u32, w: u32| format!("  {vram:x}:\t{w:08x} \tsomething\n");
        let mut listing = String::from("Disassembly of section .text:\n\n");
        let block = |addr: u32, name: &str, src: Option<&str>, count: usize| {
            let mut out = format!("{addr:016x} <{name}>:\n");
            if let Some(src) = src {
                out.push_str(&format!("{name}():\n{src}:1\n"));
            }
            for k in 0..count {
                let a = addr + (k as u32) * 4;
                out.push_str(&insn(a, words[((a - TEXT) / 4) as usize]));
            }
            out
        };
        listing.push_str(&block(0x0010_0000, "funcA", Some("a.c"), 1));
        listing.push_str(&block(0x0010_0004, "loop1", Some("a.c"), 2));
        listing.push_str(&block(0x0010_000C, "CB", Some("a.c"), 1));
        listing.push_str(&block(0x0010_0020, "funcB", Some("a.c"), 3));
        listing.push_str(&block(0x0010_0030, "funcC", Some("b.c"), 4));
        listing.push_str("\nDisassembly of section .vutext:\n\n");
        listing.push_str("0000000000400000 <ClusterMicroProgram>:\n");
        listing.push_str("0000000000400030 <NormalCMicroProgram>:\n");

        let dir = std::env::temp_dir().join(format!("icorecomp-disc-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let objdump_path = dir.join("SRCFILE.TXT");
        std::fs::write(&objdump_path, listing).unwrap();

        let cfg = crate::config::RecompConfig {
            config_path: dir.join("recomp.toml"),
            elf_path: dir.join("ELF"),
            elf_sha1: String::new(),
            entry: TEXT,
            vram_base: TEXT,
            gp: 0,
            disc: crate::config::DiscPaths {
                root: dir.clone(),
                objdump_path,
            },
        };
        let ingest = load(&cfg, image).expect("the synthetic ingest loads");
        let _ = std::fs::remove_dir_all(&dir);
        ingest
    }

    #[test]
    fn the_disc_path_builds_the_entry_set_it_can_prove() {
        let ingest = synthetic_ingest();
        let addrs: Vec<u32> = ingest.db.functions.iter().map(|f| f.vram).collect();
        let names: Vec<&str> = ingest.db.functions.iter().map(|f| f.name.as_str()).collect();
        // funcA (the ELF entry), CB (moved off the delay slot), funcB (a jal
        // target), funcC (held by a data word). `loop1` is not here.
        assert_eq!(
            addrs,
            vec![0x0010_0000, 0x0010_0010, 0x0010_0020, 0x0010_0030]
        );
        assert_eq!(names, vec!["funcA", "CB", "funcB", "funcC"]);
        assert_eq!(ingest.unnamed_functions, 0);
        // Every donor function was placed, all of them by the delta-0
        // hypothesis: none is long enough to anchor.
        assert_eq!(ingest.correlation.to_target.len(), 5);
        assert_eq!(ingest.correlation.anchors(), 0);
        assert!(ingest.correlation.unresolved.is_empty());
    }

    #[test]
    fn an_interior_label_is_folded_back_and_a_delay_slot_entry_moves() {
        let ingest = synthetic_ingest();
        assert_eq!(ingest.interior_labels, 1, "loop1 is not a function entry");
        // CB was listed on the jal's delay slot at 0x0010000C; the entry is
        // the word after it and the name went with it.
        assert_eq!(ingest.delay_slot_moves, vec![0x0010_0010]);
        assert!(ingest.db.function_by_vram(0x0010_000C).is_none());
        assert_eq!(
            ingest.db.function_by_vram(0x0010_0010).map(|f| f.name.as_str()),
            Some("CB")
        );
        assert_eq!(ingest.back_branch_hazards, 0);
        assert!(ingest.unreadable_sections.is_empty());
    }

    #[test]
    fn translation_units_are_contiguous_and_the_tables_are_found() {
        let ingest = synthetic_ingest();
        let names: Vec<&str> = ingest
            .db
            .translation_units
            .iter()
            .map(|t| t.name.as_str())
            .collect();
        assert_eq!(
            names,
            vec!["src/a", "src/b", "src/cluster", "src/normal_c"]
        );
        // The two code TUs cover .text end to end, which is what the
        // emitter's coverage assumption rests on.
        let code: Vec<&TranslationUnit> = ingest
            .db
            .translation_units
            .iter()
            .filter(|t| t.kind == SubsegKind::Code)
            .collect();
        assert_eq!(code[0].vram_start, 0x0010_0000);
        assert_eq!(code[0].vram_end, code[1].vram_start);
        assert_eq!(code[1].vram_end, 0x0010_0040);

        assert_eq!(ingest.db.jump_tables.len(), 1);
        let jt = &ingest.db.jump_tables[0];
        assert_eq!(jt.vram, 0x0020_0000);
        assert_eq!(jt.owner, "funcA");
        assert_eq!(jt.targets, vec![0x0010_0004, 0x0010_0008, 0x0010_000C]);
        assert!(ingest.db.functions[0].is_jtbl_target);
    }

    #[test]
    fn microprogram_names_become_the_us_style_short_names() {
        use crate::objdump::DonorFunction;
        let f = |name: &str| DonorFunction {
            name: name.to_string(),
            vram: 0,
            section: ".vutext".to_string(),
            source_file: None,
            insns: Vec::new(),
        };
        let od = Objdump {
            functions: vec![
                f("ClusterMicroProgram"),
                f(".dma.19"),
                f("NormalCMicroProgram"),
            ],
        };
        assert_eq!(vutext_program_names(&od), vec!["cluster", "normal_c"]);
    }
}
