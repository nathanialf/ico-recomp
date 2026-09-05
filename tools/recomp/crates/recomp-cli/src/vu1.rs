//! `vu1` subcommand: statically recompile the five .vutext microprograms.

use std::path::{Path, PathBuf};
use std::time::Instant;

use anyhow::Result;
use ingest::ProgramDb;

use crate::target_paths::{check_out_dir, default_out_dir};

pub fn run(config_path: &Path, out_dir: Option<PathBuf>) -> Result<bool> {
    let start = Instant::now();
    let out_dir = out_dir.unwrap_or_else(|| default_out_dir("vu1"));
    let db = ProgramDb::load(config_path)?;
    let image = ingest::load_elf_image(config_path)?;

    // Mechanical gate: translated output is ROM-derived and must never land
    // on a committable path (same rule as the ee subcommand).
    check_out_dir(config_path, &out_dir)?;

    let report = vu_emit::emit_all(&db, &image, &out_dir)?;
    print!("{report}");
    println!("vu1: done in {:.2}s", start.elapsed().as_secs_f64());
    Ok(true)
}
