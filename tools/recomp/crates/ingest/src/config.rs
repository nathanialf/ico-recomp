//! Loader for the target config (`config/recomp.toml`): the input paths and
//! the `[pins]` SHA-1 hash that gates every other parsing step.
//!
//! One table names the inputs.
//!
//! * `[inputs]`: the retail boot ELF and the development-build objdump
//!   listing the disc carries, both extracted from the user's own disc image
//!   by `setup.sh`. The listing's functions are transplanted onto the retail
//!   ELF by fingerprint correlation (`correlate`), and the ELF's own entry
//!   proofs (`scan`) supply the boundaries the correlation cannot carry.
//!
//! There is no second table and no second authority. The decomp is a
//! separate project and not an input to this one (the user's ruling,
//! 2026-09-05; see CLAUDE.md), so a `[decomp]` table is not a supported
//! configuration and this loader does not read one.

use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

use anyhow::{Context, Result};
use serde::Deserialize;

/// Environment override for `[inputs].root`. The environment wins over the
/// config file and says so on stderr when it does, so an existing invocation
/// keeps working when the files move.
pub const PAL_ROOT_ENV: &str = "ICORECOMP_PAL_ROOT";

#[derive(Debug, Clone, Deserialize)]
struct RawConfig {
    inputs: RawInputs,
    pins: RawPins,
    target: RawTarget,
}

#[derive(Debug, Clone, Deserialize)]
struct RawInputs {
    root: PathBuf,
    elf: PathBuf,
    objdump: PathBuf,
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

/// Print one line to stderr the first time this process is asked to.
///
/// A command loads its config two or three times (directly, then again
/// inside `ProgramDb::load` and `load_elf_image`), so an override notice
/// printed at the point of use appeared three times per run. The rule for
/// the runtime's twin of this is that the environment wins and says so at
/// startup, once. Keyed by the whole message.
fn note_once(message: String) {
    static SEEN: OnceLock<Mutex<HashSet<String>>> = OnceLock::new();
    let seen = SEEN.get_or_init(|| Mutex::new(HashSet::new()));
    let mut seen = match seen.lock() {
        Ok(g) => g,
        // A poisoned lock means another thread panicked while holding it.
        // Printing the notice twice is better than losing it.
        Err(e) => e.into_inner(),
    };
    if seen.insert(message.clone()) {
        eprintln!("{message}");
    }
}

/// Paths the `[inputs]` table resolves to.
#[derive(Debug, Clone)]
pub struct DiscPaths {
    /// Where the files extracted from the disc live. A relative `root` is
    /// resolved against this repository's root, which is what the shipped
    /// config uses (`baserom/pal`, gitignored); an absolute one is used as
    /// given.
    pub root: PathBuf,
    /// The objdump -dl listing left on the disc: the donor for names,
    /// source files and function boundaries. It is the only disc file the
    /// ingest reads apart from the ELF. `MAIN.MAP` and `TRFILE.TXT` are also
    /// on the disc and nothing here opens them; see `config/recomp.toml`.
    pub objdump_path: PathBuf,
}

/// Resolved paths and pins, ready for the individual parsers to consume.
#[derive(Debug, Clone)]
pub struct RecompConfig {
    /// The config file this was loaded from. Carried so a failure about a
    /// pinned or configured value can name the file the reader has to edit.
    pub config_path: PathBuf,
    pub elf_path: PathBuf,
    pub elf_sha1: String,
    pub entry: u32,
    pub vram_base: u32,
    pub gp: u32,
    pub disc: DiscPaths,
}

impl RecompConfig {
    /// Load and resolve a target config. `config_path` is the path to the
    /// toml file itself (e.g. `config/recomp.toml` under the repo root); a
    /// relative `[inputs].root` is resolved against that repo root, i.e.
    /// `config_path`'s parent's parent, after the `ICORECOMP_PAL_ROOT`
    /// override is applied.
    pub fn load(config_path: &Path) -> Result<Self> {
        let text = std::fs::read_to_string(config_path)
            .with_context(|| format!("reading recomp config at {}", config_path.display()))?;
        let raw: RawConfig = toml::from_str(&text)
            .with_context(|| format!("parsing recomp config at {}", config_path.display()))?;

        let config_dir = config_path
            .parent()
            .with_context(|| format!("{} has no parent directory", config_path.display()))?;
        // config/recomp*.toml lives directly under the repo root, so the repo
        // root is this file's grandparent.
        let repo_root = config_dir.parent().unwrap_or(config_dir);

        let root = match std::env::var_os(PAL_ROOT_ENV) {
            Some(v) => {
                let p = PathBuf::from(v);
                note_once(format!(
                    "{PAL_ROOT_ENV}={} overrides [inputs].root ({}) from {}",
                    p.display(),
                    raw.inputs.root.display(),
                    config_path.display()
                ));
                p
            }
            None => repo_root.join(&raw.inputs.root),
        };
        let root = normalize(&root);
        let elf_path = root.join(&raw.inputs.elf);
        let disc = DiscPaths {
            objdump_path: root.join(&raw.inputs.objdump),
            root,
        };

        Ok(RecompConfig {
            config_path: config_path.to_path_buf(),
            elf_path,
            elf_sha1: raw.pins.elf_sha1.to_lowercase(),
            entry: raw.target.entry,
            vram_base: raw.target.vram_base,
            gp: raw.target.gp,
            disc,
        })
    }
}

/// Lexically collapse `..` / `.` components without requiring the path to
/// exist (canonicalize would, and the inputs may legitimately be missing at
/// load time: that is reported as a clear error when a file is opened, not a
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn repo_config(name: &str) -> PathBuf {
        // crates/ingest -> crates -> recomp -> tools -> repo root
        PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../..")
            .join("config")
            .join(name)
    }

    #[test]
    fn the_shipped_config_names_the_disc_inputs() {
        let cfg = RecompConfig::load(&repo_config("recomp.toml")).expect("the config parses");
        // .reginfo ri_gp_value of SCES_507.60, cross-checked against crt0.
        assert_eq!(cfg.gp, 0x0064_0AF0);
        assert_eq!(cfg.elf_sha1, "da3644c54c26fe760f3b6a591a5fc2eab396ed2b");
        assert!(cfg.disc.objdump_path.ends_with("SRCFILE.TXT"));
        assert!(cfg.elf_path.starts_with(&cfg.disc.root));
        // A relative [inputs].root is repo-relative, not relative to the
        // process's working directory, so `cargo test` from tools/recomp and
        // `icorecomp ee` from the repo root resolve to the same files.
        assert!(cfg.disc.root.ends_with("baserom/pal"));
        let repo_root = normalize(&PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../.."));
        assert_eq!(cfg.disc.root, repo_root.join("baserom/pal"));
    }

    #[test]
    fn a_config_with_no_inputs_table_is_rejected() {
        let dir = std::env::temp_dir().join("icorecomp-config-test");
        std::fs::create_dir_all(dir.join("config")).unwrap();
        let p = dir.join("config/neither.toml");
        std::fs::write(
            &p,
            "[target]\nentry = 0\nvram_base = 0\ngp = 0\n[pins]\nelf_sha1 = \"00\"\n",
        )
        .unwrap();
        let err = RecompConfig::load(&p).unwrap_err().to_string();
        assert!(err.contains("parsing recomp config"), "unexpected message: {err}");
    }

    #[test]
    fn an_absolute_inputs_root_is_taken_as_given() {
        let dir = std::env::temp_dir().join("icorecomp-config-test");
        std::fs::create_dir_all(dir.join("config")).unwrap();
        let p = dir.join("config/abs.toml");
        std::fs::write(
            &p,
            "[target]\nentry = 0\nvram_base = 0\ngp = 0\n\
             [pins]\nelf_sha1 = \"00\"\n\
             [inputs]\nroot = \"/somewhere/extracted\"\nelf = \"SCES_507.60\"\n\
             objdump = \"SRCFILE.TXT\"\n",
        )
        .unwrap();
        let cfg = RecompConfig::load(&p).expect("absolute root parses");
        assert_eq!(cfg.disc.root, PathBuf::from("/somewhere/extracted"));
        assert_eq!(cfg.elf_path, PathBuf::from("/somewhere/extracted/SCES_507.60"));
        assert_eq!(
            cfg.disc.objdump_path,
            PathBuf::from("/somewhere/extracted/SRCFILE.TXT")
        );
    }
}
