//! Jump table discovery: walks `asm/nonmatchings/**/*.s`, `asm/data/**/*.s`,
//! and `asm/matchings/**/*.s` (the latter two if present) looking for
//!
//! ```text
//! dlabel jtbl_XXXXXXXX
//!     .word .LYYYYYYYY
//!     ...
//! enddlabel jtbl_XXXXXXXX
//! ```
//!
//! blocks, and resolves each table's owning function by scanning every
//! `glabel NAME ... endlabel NAME` body in the same tree for a textual
//! reference to the table's symbol (the `%hi(jtbl_...)/%lo(jtbl_...)` load
//! pair, or a direct switch-table load). Deliberately keeps only addresses
//! and counts: raw `.word` instruction encodings never make it into the
//! returned structs.
//!
//! `asm/data/**/*.s` carries the .rodata carve for every jtbl in the
//! binary; a handful of tables there aren't independently re-declared
//! under `nonmatchings`/`matchings` (their `.rodata` carve is
//! consolidated into a shared per-TU data file), so scanning `asm/data`
//! too is what brings the discovered table count up to the true total.
//! Ownership resolution (`lib.rs`) prefers a direct `glabel` textual
//! reference; if a table's owning function is ever fully matched from C
//! with no `.s` stub left to scan (not currently the case for any table in
//! this binary, but a real possibility as decomp progress continues),
//! ownership falls back to whichever function's byte range contains the
//! majority of the table's target vrams.
//!
//! Caution: naive `grep 'dlabel jtbl_'` over this tree overcounts by 2x,
//! because `enddlabel jtbl_XXXXXXXX` also contains the substring
//! `dlabel jtbl_XXXXXXXX` (`"enddlabel"` ends in `"dlabel"`). This scanner
//! anchors on a trimmed line *starting with* `"dlabel "` (with the
//! trailing space), which `"enddlabel "` does not match, so it isn't
//! affected — but it's why a quick shell sanity check of this file's output
//! needs `^\s*dlabel jtbl_`, not a bare substring grep.

use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};

pub struct RawJumpTable {
    pub name: String,
    pub vram: u32,
    pub targets: Vec<u32>,
}

/// jtbl name -> set of function names whose body references it.
pub type OwnerMap = HashMap<String, HashSet<String>>;

/// Walk the asm tree and return (jump tables, owner map). The owner map is
/// exposed separately so callers can produce a clear error listing any
/// table with zero or more than one candidate owner.
pub fn scan_asm_tree(asm_dir: &Path) -> Result<(Vec<RawJumpTable>, OwnerMap)> {
    let mut files = Vec::new();
    let nonmatchings = asm_dir.join("nonmatchings");
    if !nonmatchings.is_dir() {
        bail!(
            "{}: expected asm/nonmatchings directory, not found",
            nonmatchings.display()
        );
    }
    collect_s_files(&nonmatchings, &mut files)?;

    let data = asm_dir.join("data");
    if data.is_dir() {
        collect_s_files(&data, &mut files)?;
    }

    let matchings = asm_dir.join("matchings");
    if matchings.is_dir() {
        collect_s_files(&matchings, &mut files)?;
    }
    files.sort();

    let mut tables: Vec<RawJumpTable> = Vec::new();
    let mut seen: HashMap<String, usize> = HashMap::new();
    let mut owners: HashMap<String, HashSet<String>> = HashMap::new();

    for file in &files {
        let text = std::fs::read_to_string(file)
            .with_context(|| format!("reading asm file {}", file.display()))?;

        for jt in extract_jump_tables(&text) {
            if let Some(&idx) = seen.get(&jt.name) {
                let existing = &tables[idx];
                if existing.vram != jt.vram || existing.targets != jt.targets {
                    bail!(
                        "{}: {} redefined with different contents (also defined elsewhere)",
                        file.display(),
                        jt.name
                    );
                }
                // Identical redefinition (e.g. also carved in asm/data) is fine.
                continue;
            }
            seen.insert(jt.name.clone(), tables.len());
            tables.push(jt);
        }

        collect_owners(&text, &mut owners);
    }

    tables.sort_by_key(|t| t.vram);
    Ok((tables, owners))
}

fn collect_s_files(dir: &Path, out: &mut Vec<PathBuf>) -> Result<()> {
    let mut entries: Vec<_> = std::fs::read_dir(dir)
        .with_context(|| format!("reading directory {}", dir.display()))?
        .collect::<std::result::Result<_, _>>()
        .with_context(|| format!("reading directory {}", dir.display()))?;
    entries.sort_by_key(|e| e.path());

    for entry in entries {
        let path = entry.path();
        let file_type = entry.file_type()?;
        if file_type.is_dir() {
            collect_s_files(&path, out)?;
        } else if file_type.is_file() && path.extension().is_some_and(|e| e == "s") {
            out.push(path);
        }
    }
    Ok(())
}

fn extract_jump_tables(text: &str) -> Vec<RawJumpTable> {
    let mut out = Vec::new();
    let mut lines = text.lines().peekable();
    while let Some(line) = lines.next() {
        let trimmed = line.trim();
        let Some(name) = trimmed.strip_prefix("dlabel ") else {
            continue;
        };
        let name = name.trim();
        if !name.starts_with("jtbl_") {
            continue;
        }
        let Some(vram) = parse_addr_suffix(name, "jtbl_") else {
            continue;
        };

        let end_marker = format!("enddlabel {name}");
        let mut targets = Vec::new();
        for body_line in lines.by_ref() {
            if body_line.trim() == end_marker {
                break;
            }
            let Some(word_idx) = body_line.find(".word") else {
                continue;
            };
            let operand = body_line[word_idx + ".word".len()..].trim();
            let operand = operand.split_whitespace().next().unwrap_or("");
            if let Some(label) = operand.strip_prefix(".L") {
                if let Ok(v) = u32::from_str_radix(label, 16) {
                    targets.push(v);
                }
            } else if let Some(hex) = operand
                .strip_prefix("0x")
                .or_else(|| operand.strip_prefix("0X"))
            {
                if let Ok(v) = u32::from_str_radix(hex, 16) {
                    targets.push(v);
                }
            }
        }

        out.push(RawJumpTable {
            name: name.to_string(),
            vram,
            targets,
        });
    }
    out
}

fn collect_owners(text: &str, owners: &mut HashMap<String, HashSet<String>>) {
    let mut current: Option<&str> = None;
    for line in text.lines() {
        let trimmed = line.trim();
        if let Some(name) = trimmed.strip_prefix("glabel ") {
            current = Some(name.trim());
            continue;
        }
        if trimmed.starts_with("endlabel ") {
            current = None;
            continue;
        }
        let Some(func) = current else { continue };
        for name in find_jtbl_refs(line) {
            owners.entry(name).or_default().insert(func.to_string());
        }
    }
}

/// Find every `jtbl_XXXXXXXX` substring in a line (as used inside
/// `%hi(jtbl_...)`/`%lo(jtbl_...)` operands).
fn find_jtbl_refs(line: &str) -> Vec<String> {
    let mut out = Vec::new();
    let bytes = line.as_bytes();
    let needle = b"jtbl_";
    let mut i = 0;
    while i + needle.len() <= bytes.len() {
        if &bytes[i..i + needle.len()] == needle {
            let start = i + needle.len();
            let hex_len = bytes[start..]
                .iter()
                .take_while(|b| b.is_ascii_hexdigit())
                .count();
            if hex_len == 8 {
                out.push(line[i..start + hex_len].to_string());
            }
            i = start + hex_len.max(1);
        } else {
            i += 1;
        }
    }
    out
}

fn parse_addr_suffix(name: &str, prefix: &str) -> Option<u32> {
    let hex = name.strip_prefix(prefix)?;
    u32::from_str_radix(hex, 16).ok()
}
