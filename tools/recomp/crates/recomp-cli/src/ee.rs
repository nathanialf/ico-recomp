//! `ee` subcommand: drive the ee-emit translator over the whole EE `.text`.

use std::path::Path;
use std::time::Instant;

use anyhow::{bail, Result};
use ingest::ProgramDb;

pub fn run(config_path: &Path, out_dir: &Path, census_only: bool) -> Result<bool> {
    let start = Instant::now();
    let db = ProgramDb::load(config_path)?;
    let image = ingest::load_elf_image(config_path)?;

    if census_only {
        let counts = ee_emit::census(&db, &image)?;
        let total: usize = counts.values().sum();
        let mut sorted: Vec<_> = counts.iter().collect();
        sorted.sort_by(|a, b| b.1.cmp(a.1).then(a.0.cmp(b.0)));
        println!("census: {} mnemonics, {} instructions", counts.len(), total);
        for (m, n) in sorted {
            println!("  {n:8}  {m}");
        }
        return Ok(true);
    }

    // Mechanical gate: translated output is ROM-derived and must never land
    // on a committable path. Only paths under a generated/ or gen/ directory
    // (both gitignored) are accepted.
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

    let report = ee_emit::emit_all(&db, &image, out_dir)?;
    println!("{report}");
    println!("ee: done in {:.2}s", start.elapsed().as_secs_f64());
    Ok(true)
}
