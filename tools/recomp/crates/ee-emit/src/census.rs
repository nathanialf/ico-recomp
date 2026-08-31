//! Mnemonic census over the EE `.text` functions. The emitter's coverage
//! policy is derived from this: every mnemonic actually present in the
//! measured binary must be either implemented or explicitly routed (COP1 and
//! COP2 ops to `rt_unimplemented` in P1). Anything else is a hard error.

use std::collections::BTreeMap;

use anyhow::Result;
use ingest::{ElfImage, ProgramDb};
use r5900_decode::decode;

/// Per-mnemonic instruction counts over all functions inside `.text`.
pub fn census(db: &ProgramDb, image: &ElfImage) -> Result<BTreeMap<String, usize>> {
    let text = db
        .sections
        .iter()
        .find(|s| s.name == ".text")
        .ok_or_else(|| anyhow::anyhow!("no .text section in ELF"))?;
    let mut counts: BTreeMap<String, usize> = BTreeMap::new();
    for f in &db.functions {
        if !text.contains_vram(f.vram) {
            continue;
        }
        let mut vram = f.vram;
        // The last .text function's derived size can include alignment pad
        // that belongs to no section byte range; clamp to the section end.
        let end = (f.vram + f.size).min(text.vram_end());
        while vram < end {
            let word = image
                .read_u32(vram)
                .ok_or_else(|| anyhow::anyhow!("unreadable word at {vram:#x} in {}", f.name))?;
            let insn = decode(word, vram);
            match insn.mnemonic() {
                Some(m) => *counts.entry(m.to_string()).or_insert(0) += 1,
                // Embedded data inside a .text function (e.g. the 16-byte
                // constant at 0x254CE0 read back via lq). Tracked so the
                // emitter can trap on it instead of translating it.
                None => *counts.entry("<invalid-word>".to_string()).or_insert(0) += 1,
            }
            vram += 4;
        }
    }
    Ok(counts)
}
