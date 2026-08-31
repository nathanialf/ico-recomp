//! Parser for `config/symbol_addrs.us.txt`.
//!
//! Each declaration line looks like:
//!
//! ```text
//! Name = 0xADDR; // key:value key:value  // free-text comment, maybe (vendor)
//! ```
//!
//! The first `//`-delimited segment holds machine-readable `key:value`
//! attributes (`type`, `size`, `defined`, `function_owner`); a second,
//! optional `//`-delimited segment holds free text, which is where the
//! `(vendor)` marker and source-file hints live. Plain `#`/blank lines and
//! block comments above declarations are ignored.

use std::collections::HashMap;
use std::path::Path;

use anyhow::{Context, Result};

/// One parsed `symbol_addrs.us.txt` declaration.
pub struct RawSymbol {
    pub name: String,
    pub vram: u32,
    pub kind: String,
    pub size: Option<u32>,
    pub vendor: bool,
    pub function_owner: Option<String>,
    pub defined: Option<bool>,
}

pub fn load_symbols(path: &Path) -> Result<Vec<RawSymbol>> {
    let text = std::fs::read_to_string(path)
        .with_context(|| format!("reading symbol_addrs at {}", path.display()))?;

    let mut out = Vec::new();
    for (lineno, raw_line) in text.lines().enumerate() {
        if let Some(sym) = parse_decl_line(raw_line).with_context(|| {
            format!(
                "{}:{}: malformed symbol declaration",
                path.display(),
                lineno + 1
            )
        })? {
            out.push(sym);
        }
    }
    Ok(out)
}

fn parse_decl_line(raw_line: &str) -> Result<Option<RawSymbol>> {
    let line = raw_line.trim();
    if line.is_empty() || !line.starts_with(|c: char| c.is_ascii_alphabetic() || c == '_') {
        return Ok(None);
    }

    let Some(eq_idx) = line.find('=') else {
        return Ok(None);
    };
    let name = line[..eq_idx].trim();
    if name.is_empty() || !is_ident(name) {
        return Ok(None);
    }

    let rest = line[eq_idx + 1..].trim_start();
    let Some(hex) = rest.strip_prefix("0x").or_else(|| rest.strip_prefix("0X")) else {
        return Ok(None);
    };
    let hex_digits: String = hex.chars().take_while(|c| c.is_ascii_hexdigit()).collect();
    if hex_digits.is_empty() {
        return Ok(None);
    }
    let vram = u32::from_str_radix(&hex_digits, 16)
        .with_context(|| format!("bad hex address in {line:?}"))?;

    let after_addr = hex[hex_digits.len()..].trim_start();
    let Some(after_semi) = after_addr.strip_prefix(';') else {
        return Ok(None);
    };

    // Everything after `;` is the comment region. Split it on the first two
    // `//` markers: attrs, then free text.
    let comment = after_semi.trim_start();
    let attrs_and_free = comment.strip_prefix("//");
    let Some(rest_comment) = attrs_and_free else {
        // No attributes at all: not a symbol_addrs-style declaration we can
        // classify, skip rather than guess.
        return Ok(None);
    };
    // Only the attrs half (before a second `//`, if any) is machine-read;
    // the free-text half is folded into the whole-line vendor check below
    // instead of being carried as its own binding.
    let attrs_str = match rest_comment.find("//") {
        Some(idx) => &rest_comment[..idx],
        None => rest_comment,
    };

    let mut attrs: HashMap<&str, &str> = HashMap::new();
    for tok in attrs_str.split_whitespace() {
        if let Some((k, v)) = tok.split_once(':') {
            attrs.insert(k, v);
        }
    }

    let kind = attrs
        .get("type")
        .with_context(|| format!("{line:?}: missing type: attribute"))?
        .to_string();
    let size = attrs.get("size").map(|v| parse_hex_or_dec(v)).transpose()?;
    let function_owner = attrs.get("function_owner").map(|s| s.to_string());
    let defined = attrs.get("defined").map(|s| s.eq_ignore_ascii_case("true"));
    // Two independent vendor markers show up in this file: the explicit
    // "(vendor)" free-text tag (used for the ~30 symbols with no named
    // source file, e.g. `_start` and the raw crt0/libkernl disassembly),
    // and a `vendor_<ADDR>` source-path reference in the free-text comment
    // (e.g. `// src/cod/vendor_100110.c`) for the ~920 symbols that do have
    // a splat path but one under the vendor-code TUs. A symbol is
    // vendor-owned if either shows up anywhere on the line.
    let vendor = line.contains("(vendor)") || line.contains("vendor_");

    Ok(Some(RawSymbol {
        name: name.to_string(),
        vram,
        kind,
        size,
        vendor,
        function_owner,
        defined,
    }))
}

fn is_ident(s: &str) -> bool {
    let mut chars = s.chars();
    match chars.next() {
        Some(c) if c.is_ascii_alphabetic() || c == '_' => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

fn parse_hex_or_dec(s: &str) -> Result<u32> {
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u32::from_str_radix(hex, 16).with_context(|| format!("bad hex integer {s:?}"))
    } else {
        s.parse::<u32>()
            .with_context(|| format!("bad integer {s:?}"))
    }
}
