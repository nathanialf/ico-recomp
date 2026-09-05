//! Where the target's inputs and outputs live, derived from the config path.
//!
//! The translated tree must not be committable. `generated/ee` and
//! `generated/vu1` are the stage output directories, which is the layout the
//! runtime's CMake looks for, so the translator and the build agree without
//! either side being told twice. Both sit directly under `generated/`, which
//! `.gitignore` ignores wholesale (`/generated/`), and which is exactly what
//! `check_out_dir` below requires of any `--out` that resolves inside this
//! repository. A sibling directory such as `generated-ee/` would not satisfy
//! it.

use std::path::{Component, Path, PathBuf};

use anyhow::{bail, Context, Result};

/// Output directory for a translator stage (`"ee"`, `"vu1"`).
pub fn default_out_dir(stage: &str) -> PathBuf {
    PathBuf::from("generated").join(stage)
}

/// The entry hook table, `config/entry_hooks.txt`, resolved beside the config
/// file rather than against the process's working directory. An entry hook is
/// an address in one specific binary, and the emitter hard-errors on an
/// address that is not a function entry in the binary it is translating.
pub fn entry_hooks_path(config_path: &Path) -> Result<PathBuf> {
    let dir = config_path
        .parent()
        .with_context(|| format!("{} has no parent directory", config_path.display()))?;
    Ok(dir.join("entry_hooks.txt"))
}

/// Mechanical gate on `--out`: translated output is ROM-derived and must
/// never land on a committable path.
///
/// The rule the repo actually enforces is rooted, not by component name:
/// `.gitignore` ignores `/generated/` and `/gen/` at the repo root, and
/// `tools/check_no_rom.sh` blocks `generated/*` and `gen/*` there. So this
/// checks the same thing. A path that resolves inside the repository must
/// have `generated` or `gen` as its first component under the repo root;
/// `src/runtime/generated/ee` has a component named `generated` and is
/// committable, which the old component-anywhere test accepted. A path
/// outside the repository is accepted as given: nothing there can be
/// committed by this repo's hook.
///
/// The repo root is the config file's grandparent, the same derivation
/// `ingest::RecompConfig::load` uses for a relative `[inputs].root`. A
/// relative `--out` is resolved against the process's working directory,
/// which is what writing it does.
pub fn check_out_dir(config_path: &Path, out_dir: &Path) -> Result<()> {
    let cwd = std::env::current_dir().context("reading the working directory")?;
    let absolute = |p: &Path| -> PathBuf {
        lexically_normal(&if p.is_absolute() { p.to_path_buf() } else { cwd.join(p) })
    };
    let config_abs = absolute(config_path);
    let repo_root = config_abs
        .parent()
        .and_then(|d| d.parent())
        .with_context(|| {
            format!(
                "{} has no grandparent directory, so the repository root it names \
                 cannot be derived",
                config_path.display()
            )
        })?
        .to_path_buf();
    let out_abs = absolute(out_dir);

    let Ok(rest) = out_abs.strip_prefix(&repo_root) else {
        // Outside the repository entirely.
        return Ok(());
    };
    let first = rest.components().next().and_then(|c| match c {
        Component::Normal(s) => s.to_str(),
        _ => None,
    });
    if matches!(first, Some("generated") | Some("gen")) {
        return Ok(());
    }
    bail!(
        "refusing to write translated output to {}: a path inside this repository \
         ({}) must sit under its rooted generated/ or gen/ directory, which is what \
         .gitignore ignores and what tools/check_no_rom.sh blocks. {} is committable.",
        out_dir.display(),
        repo_root.display(),
        out_abs.display()
    );
}

/// Collapse `.` and `..` without touching the filesystem, so the comparison
/// above does not depend on the output directory existing yet.
fn lexically_normal(path: &Path) -> PathBuf {
    let mut out = PathBuf::new();
    for component in path.components() {
        match component {
            Component::ParentDir => {
                if !out.pop() {
                    out.push("..");
                }
            }
            Component::CurDir => {}
            other => out.push(other),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cfg() -> PathBuf {
        std::env::current_dir().unwrap().join("config/recomp.toml")
    }

    #[test]
    fn the_rooted_generated_directories_are_accepted() {
        let root = std::env::current_dir().unwrap();
        for ok in ["generated/ee", "generated/vu1", "gen/x"] {
            check_out_dir(&cfg(), &root.join(ok)).unwrap_or_else(|e| panic!("{ok}: {e:#}"));
        }
    }

    #[test]
    fn a_committable_path_with_a_generated_component_is_refused() {
        let root = std::env::current_dir().unwrap();
        let err = check_out_dir(&cfg(), &root.join("src/runtime/generated/ee"))
            .unwrap_err()
            .to_string();
        assert!(err.contains("committable"), "{err}");
    }

    #[test]
    fn a_path_outside_the_repository_is_accepted() {
        check_out_dir(&cfg(), Path::new("/tmp/icorecomp-out")).expect("outside the repo");
    }
}
