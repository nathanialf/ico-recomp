//! Parser for `config/entry_hooks.txt`: the guest function entry addresses
//! the runtime wants a callback at.
//!
//! Each declaration line looks like:
//!
//! ```text
//! 0xADDRESS  // reason
//! ```
//!
//! `#` lines and blank lines are ignored. The reason is carried so the
//! emitter's hard error can name the line that asked for an address, and so
//! a stale entry reports what it was for rather than just its number.
//!
//! The file holds address facts only: an entry address, and the reason the
//! runtime wants a callback there. Nothing here is ROM-derived output.

use std::collections::BTreeMap;
use std::path::Path;

use anyhow::{bail, Context, Result};

/// The parsed table: entry vram to the reason its line gave.
#[derive(Debug, Default, Clone)]
pub struct EntryHooks {
    pub by_vram: BTreeMap<u32, String>,
    /// The file this table was read from, for the emitter's hard error.
    /// Naming it is what tells the reader which file to edit; one build
    /// serves one target, so that file is `config/entry_hooks.txt`.
    /// `"the entry hook table"` for a table built from a string.
    pub source: String,
}

impl EntryHooks {
    /// An empty table, for callers that translate without a hook config
    /// (the unit tests, and any future consumer of `emit_all`).
    pub fn empty() -> Self {
        EntryHooks {
            by_vram: BTreeMap::new(),
            source: "the entry hook table".to_string(),
        }
    }

    pub fn contains(&self, vram: u32) -> bool {
        self.by_vram.contains_key(&vram)
    }

    pub fn len(&self) -> usize {
        self.by_vram.len()
    }

    pub fn is_empty(&self) -> bool {
        self.by_vram.is_empty()
    }

    /// Load the table. A missing file is an error rather than an empty
    /// table: the emitter is told where the config lives, so not finding it
    /// means the tree is wrong, and quietly emitting no hooks would hand the
    /// runtime a build whose hooks never fire.
    pub fn load(path: &Path) -> Result<Self> {
        let text = std::fs::read_to_string(path)
            .with_context(|| format!("reading entry hook table at {}", path.display()))?;
        let mut hooks = Self::parse(&text)
            .with_context(|| format!("parsing {}", path.display()))?;
        hooks.source = path.display().to_string();
        Ok(hooks)
    }

    pub fn parse(text: &str) -> Result<Self> {
        let mut by_vram: BTreeMap<u32, String> = BTreeMap::new();
        for (lineno, raw_line) in text.lines().enumerate() {
            let line = raw_line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some(rest) = line.strip_prefix("0x").or_else(|| line.strip_prefix("0X")) else {
                bail!("line {}: expected an address starting with 0x, got {line:?}", lineno + 1);
            };
            let digits: String = rest.chars().take_while(|c| c.is_ascii_hexdigit()).collect();
            if digits.is_empty() {
                bail!("line {}: no hex digits after 0x in {line:?}", lineno + 1);
            }
            let vram = u32::from_str_radix(&digits, 16)
                .with_context(|| format!("line {}: bad hex address in {line:?}", lineno + 1))?;

            let after = rest[digits.len()..].trim_start();
            let Some(reason) = after.strip_prefix("//") else {
                bail!(
                    "line {}: address 0x{vram:08X} has no `// reason` after it. Every entry \
                     states why the runtime wants the callback.",
                    lineno + 1
                );
            };
            let reason = reason.trim();
            if reason.is_empty() {
                bail!("line {}: address 0x{vram:08X} has an empty reason", lineno + 1);
            }
            if by_vram.insert(vram, reason.to_string()).is_some() {
                bail!("line {}: address 0x{vram:08X} is listed twice", lineno + 1);
            }
        }
        Ok(EntryHooks {
            by_vram,
            source: "the entry hook table".to_string(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_addresses_and_reasons() {
        let hooks = EntryHooks::parse(
            "# a comment\n\
             \n\
             0x001146F0  // the matrix composer\n\
             0X00114560  // and one more\n",
        )
        .expect("parse should succeed");
        assert_eq!(hooks.len(), 2);
        assert!(hooks.contains(0x0011_46F0));
        assert_eq!(hooks.by_vram[&0x0011_46F0], "the matrix composer");
        assert!(hooks.contains(0x0011_4560));
    }

    #[test]
    fn rejects_a_line_with_no_reason() {
        let err = EntryHooks::parse("0x001146F0\n").unwrap_err().to_string();
        assert!(err.contains("no `// reason`"), "unexpected message: {err}");
    }

    #[test]
    fn rejects_a_duplicate_address() {
        let err = EntryHooks::parse("0x001146F0 // one\n0x001146F0 // two\n")
            .unwrap_err()
            .to_string();
        assert!(err.contains("listed twice"), "unexpected message: {err}");
    }

    #[test]
    fn rejects_a_line_that_is_not_an_address() {
        assert!(EntryHooks::parse("gsb_SetVSMatrixSub // by name\n").is_err());
    }

    /// The committed table is what the translator will actually be handed,
    /// so it is parsed here rather than only in a real translation run.
    #[test]
    fn the_committed_table_parses() {
        // crates/ee-emit -> crates -> recomp -> tools -> repo root
        let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../../..")
            .join("config/entry_hooks.txt");
        let hooks = EntryHooks::load(&path).expect("config/entry_hooks.txt should parse");
        assert!(
            hooks.contains(0x0011_46F0),
            "the matrix composer entry is the reason this file exists"
        );
    }
}
