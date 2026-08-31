//! Loader for `config/recomp.toml`: the `[decomp]` input paths and `[pins]`
//! SHA-1 hashes that gate every other parsing step.

use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use serde::Deserialize;

#[derive(Debug, Clone, Deserialize)]
struct RawConfig {
    decomp: RawDecomp,
    pins: RawPins,
    target: RawTarget,
}

#[derive(Debug, Clone, Deserialize)]
struct RawDecomp {
    root: PathBuf,
    elf: PathBuf,
    #[allow(dead_code)]
    rom: PathBuf,
    splat_yaml: PathBuf,
    symbol_addrs: PathBuf,
    asm_dir: PathBuf,
    #[allow(dead_code)]
    vu1_sources: Vec<PathBuf>,
}

#[derive(Debug, Clone, Deserialize)]
struct RawPins {
    elf_sha1: String,
}

#[derive(Debug, Clone, Deserialize)]
struct RawTarget {
    entry: u32,
    vram_base: u32,
    gp: u32,
}

/// Resolved paths and pins, ready for the individual parsers to consume.
#[derive(Debug, Clone)]
pub struct RecompConfig {
    /// Root of the decomp checkout (`../ico`, resolved relative to this
    /// repo's root, i.e. `recomp.toml`'s grandparent directory).
    pub decomp_root: PathBuf,
    pub elf_path: PathBuf,
    pub splat_yaml_path: PathBuf,
    pub symbol_addrs_path: PathBuf,
    pub asm_dir: PathBuf,
    pub elf_sha1: String,
    pub entry: u32,
    pub vram_base: u32,
    pub gp: u32,
}

impl RecompConfig {
    /// Load and resolve `config/recomp.toml`. `config_path` is the path to
    /// the toml file itself (e.g. `config/recomp.toml` under the repo
    /// root); `[decomp].root` is resolved relative to that repo root, i.e.
    /// `config_path`'s parent's parent.
    pub fn load(config_path: &Path) -> Result<Self> {
        let text = std::fs::read_to_string(config_path)
            .with_context(|| format!("reading recomp config at {}", config_path.display()))?;
        let raw: RawConfig = toml::from_str(&text)
            .with_context(|| format!("parsing recomp config at {}", config_path.display()))?;

        let config_dir = config_path
            .parent()
            .with_context(|| format!("{} has no parent directory", config_path.display()))?;
        // config/recomp.toml lives directly under the repo root, so the repo
        // root is this file's grandparent; [decomp].root is documented as
        // relative to the repo root.
        let repo_root = config_dir.parent().unwrap_or(config_dir);
        let decomp_root = normalize(&repo_root.join(&raw.decomp.root));

        Ok(RecompConfig {
            elf_path: decomp_root.join(&raw.decomp.elf),
            splat_yaml_path: decomp_root.join(&raw.decomp.splat_yaml),
            symbol_addrs_path: decomp_root.join(&raw.decomp.symbol_addrs),
            asm_dir: decomp_root.join(&raw.decomp.asm_dir),
            decomp_root,
            elf_sha1: raw.pins.elf_sha1.to_lowercase(),
            entry: raw.target.entry,
            vram_base: raw.target.vram_base,
            gp: raw.target.gp,
        })
    }
}

/// Lexically collapse `..` / `.` components without requiring the path to
/// exist (canonicalize would, and the decomp root may legitimately be
/// missing at ingest time — that's reported as a clear error later, not a
/// panic here).
fn normalize(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for component in path.components() {
        use std::path::Component::*;
        match component {
            ParentDir => {
                if !out.pop() {
                    out.push("..");
                }
            }
            CurDir => {}
            other => out.push(other),
        }
    }
    out
}
