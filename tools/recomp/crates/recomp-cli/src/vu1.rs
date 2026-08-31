//! `vu1` subcommand: statically recompile the five .vutext microprograms.

use std::path::Path;
use std::time::Instant;

use anyhow::{bail, Result};
use ingest::ProgramDb;

pub fn run(config_path: &Path, out_dir: &Path) -> Result<bool> {
    let start = Instant::now();
    let db = ProgramDb::load(config_path)?;
    let image = ingest::load_elf_image(config_path)?;

    // Mechanical gate: translated output is ROM-derived and must never land
    // on a committable path (same rule as the ee subcommand).
    let gated = out_dir
        .components()
        .any(|c| matches!(c.as_os_str().to_str(), Some("generated") | Some("gen")));
    if !gated {
        bail!(
            "refusing to write translated output to {}: output must live under a \
             generated/ or gen/ directory (gitignored; see tools/check_no_rom.sh)",
            out_dir.display()
        );
    }

    let report = vu_emit::emit_all(&db, &image, out_dir)?;
    print!("{report}");
    println!("vu1: done in {:.2}s", start.elapsed().as_secs_f64());
    Ok(true)
}
