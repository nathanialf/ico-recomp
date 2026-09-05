//! Parser for the disc's `SRCFILE.TXT`: a GNU `objdump -dl` listing of a
//! development link of the same game.
//!
//! The shape, as measured on the PAL disc's file:
//!
//! ```text
//! Disassembly of section .text:
//!
//! 0000000000100008 <_start>:
//! _start():
//! /usr/local/sce/ee/lib/crt0.s:32
//!   100008:    3c020064     lui    $v0,0x64
//! /usr/local/sce/ee/lib/crt0.s:33
//!   10000c:    3c030074     lui    $v1,0x74
//!     ...
//!   10002c:    1420fffa     bnez    $at,100018 <_start+0x10>
//! ```
//!
//! Four line shapes matter:
//!
//! 1. `Disassembly of section NAME:` starts a section.
//! 2. `<16 hex> <NAME>:` starts a labelled block. objdump also prints
//!    anchor-relative labels such as `<_start-0x8>` where a block has no
//!    symbol of its own; those are not functions and are skipped, which is
//!    why the `+0x`/`-0x` suffix is rejected rather than kept as a name.
//! 3. `NAME():` and `PATH:LINE` annotations, emitted by `-l` ahead of the
//!    first instruction they cover. The path is kept as the function's
//!    source file, which is what the ingest uses in place of a splat
//!    translation-unit path.
//! 4. Instruction lines `  ADDR:\tBYTES \tTEXT`. In `.text` the bytes field
//!    is one 8-hex-digit word in *value* order (objdump prints MIPS words
//!    as values, not as memory bytes). In `.vutext`, which objdump does not
//!    know how to disassemble as MIPS, it falls back to a byte dump and the
//!    field is four space-separated bytes in memory order, sometimes with
//!    no text at all (the second half of a 64-bit VU bundle).
//!
//! `\t...` is objdump's elision of a run of identical lines: the addresses
//! on either side of it are not contiguous. That matters because the whole
//! point of parsing this file is to fingerprint contiguous instruction runs
//! against the retail ELF, so [`DonorFunction::runs`] splits a function's
//! instructions wherever the address is not the previous address plus four.
//!
//! Nothing parsed here is ever written into the repository. This is a
//! read-only input that lives outside every repo.

use std::path::Path;

use anyhow::{Context, Result};

/// One listed instruction: the address and word objdump printed, plus the
/// text it printed for them (absent for the continuation half of a VU
/// bundle and for pure data lines).
#[derive(Debug, Clone)]
pub struct DonorInsn {
    pub vram: u32,
    pub word: u32,
    pub text: Option<String>,
}

/// One labelled block from the listing.
#[derive(Debug, Clone)]
pub struct DonorFunction {
    pub name: String,
    pub vram: u32,
    /// Section the block was listed under (".text", ".vutext").
    pub section: String,
    /// Source path from the nearest preceding `-l` annotation, when the
    /// listing carries one. Vendor SDK code annotates absolute
    /// `/usr/local/sce/...` paths; game code annotates bare file names.
    pub source_file: Option<String>,
    pub insns: Vec<DonorInsn>,
}

impl DonorFunction {
    /// Address one past the last listed instruction. Not the same as the
    /// function's end when objdump elided a trailing run, which is why the
    /// ingest derives sizes from the next function's start instead.
    pub fn listed_end(&self) -> u32 {
        self.insns
            .last()
            .map(|i| i.vram + 4)
            .unwrap_or(self.vram)
    }

    /// The listed instructions split into address-contiguous runs. A gap
    /// appears wherever objdump elided identical lines with `...`.
    pub fn runs(&self) -> Vec<&[DonorInsn]> {
        let mut out = Vec::new();
        let mut start = 0usize;
        for i in 1..self.insns.len() {
            if self.insns[i].vram != self.insns[i - 1].vram + 4 {
                out.push(&self.insns[start..i]);
                start = i;
            }
        }
        if start < self.insns.len() {
            out.push(&self.insns[start..]);
        }
        out
    }

    /// True when the source annotation points into the Sony SDK, the GNU
    /// runtime libraries, or crt0: the same "vendor" distinction the US
    /// target draws from symbol_addrs' vendor markers.
    pub fn is_vendor(&self) -> bool {
        match &self.source_file {
            Some(p) => {
                p.starts_with("/usr/local/sce/")
                    || p.starts_with("/usr/local/")
                    || p.contains("/gcc-lib/")
                    || p.ends_with("crt0.s")
            }
            // No annotation at all means the object was linked without
            // debug line info, which in this listing is only ever a
            // library object.
            None => true,
        }
    }
}

/// The parsed listing.
#[derive(Debug, Clone)]
pub struct Objdump {
    pub functions: Vec<DonorFunction>,
}

impl Objdump {
    pub fn load(path: &Path) -> Result<Objdump> {
        let text = std::fs::read_to_string(path)
            .with_context(|| format!("reading objdump listing at {}", path.display()))?;
        Ok(Objdump::parse(&text))
    }

    pub fn parse(text: &str) -> Objdump {
        let mut functions: Vec<DonorFunction> = Vec::new();
        let mut section = String::new();
        let mut pending_source: Option<String> = None;

        for line in text.lines() {
            if let Some(rest) = line.strip_prefix("Disassembly of section ") {
                section = rest.trim_end_matches(':').to_string();
                pending_source = None;
                continue;
            }
            if let Some((vram, name)) = parse_label(line) {
                // A label clears the pending annotation: objdump prints a
                // function's `-l` annotations after its label, so anything
                // still pending here belongs to the function that just
                // ended, not to this one. A function whose object was
                // linked without line info keeps `None`, which
                // `is_vendor` reads as library code.
                pending_source = None;
                functions.push(DonorFunction {
                    name: name.to_string(),
                    vram,
                    section: section.clone(),
                    source_file: None,
                    insns: Vec::new(),
                });
                continue;
            }
            if let Some(insn) = parse_insn(line) {
                if let Some(f) = functions.last_mut() {
                    if f.source_file.is_none() {
                        f.source_file = pending_source.clone();
                    }
                    f.insns.push(insn);
                }
                continue;
            }
            if let Some(src) = parse_source_annotation(line) {
                pending_source = Some(src.to_string());
            }
        }

        Objdump { functions }
    }

    /// Functions in one section, in address order (the listing is already
    /// ordered; this does not re-sort, it filters).
    pub fn section_functions<'a>(
        &'a self,
        section: &'a str,
    ) -> impl Iterator<Item = &'a DonorFunction> + 'a {
        self.functions.iter().filter(move |f| f.section == section)
    }
}

/// `0000000000100008 <_start>:` -> (0x100008, "_start"). Anchor-relative
/// labels (`<_start-0x8>`, `<main+0x20>`) are not symbols and return None.
fn parse_label(line: &str) -> Option<(u32, &str)> {
    let (addr_s, rest) = line.split_once(' ')?;
    if addr_s.len() != 16 || !addr_s.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    let name = rest.strip_prefix('<')?.strip_suffix(">:")?;
    if name.is_empty() || name.contains("+0x") || name.contains("-0x") {
        return None;
    }
    let addr = u64::from_str_radix(addr_s, 16).ok()?;
    Some((u32::try_from(addr).ok()?, name))
}

/// `  100008:\t3c020064 \tlui\t$v0,0x64` and the `.vutext` byte-dump form
/// `  28db44:\t00 00 00 00 `.
fn parse_insn(line: &str) -> Option<DonorInsn> {
    let t = line.strip_prefix("  ")?;
    let (addr_s, rest) = t.split_once(":\t")?;
    if addr_s.is_empty() || !addr_s.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    let vram = u32::from_str_radix(addr_s, 16).ok()?;

    let (bytes_field, text) = match rest.split_once('\t') {
        Some((b, t)) => (b, Some(t.trim().to_string())),
        None => (rest, None),
    };
    let bytes_field = bytes_field.trim();

    let word = if bytes_field.len() == 8 && bytes_field.bytes().all(|b| b.is_ascii_hexdigit()) {
        // MIPS form: objdump prints the instruction word as a value.
        u32::from_str_radix(bytes_field, 16).ok()?
    } else {
        // Byte-dump form: four bytes in memory order, little endian.
        let mut b = [0u8; 4];
        let mut n = 0;
        for tok in bytes_field.split_whitespace() {
            if n == 4 || tok.len() != 2 {
                return None;
            }
            b[n] = u8::from_str_radix(tok, 16).ok()?;
            n += 1;
        }
        if n != 4 {
            return None;
        }
        u32::from_le_bytes(b)
    };

    let text = text.filter(|s| !s.is_empty());
    Some(DonorInsn { vram, word, text })
}

/// A `-l` source annotation: `PATH:LINE`. `NAME():` function annotations and
/// everything else return None.
fn parse_source_annotation(line: &str) -> Option<&str> {
    let t = line.trim_end();
    if t.is_empty() || t.starts_with(' ') || t.starts_with('\t') {
        return None;
    }
    if t.ends_with("():") {
        return None;
    }
    let (path, lineno) = t.rsplit_once(':')?;
    // The line number is a decimal, or "-1" where the listing has no line.
    let lineno = lineno.strip_prefix('-').unwrap_or(lineno);
    if lineno.is_empty() || !lineno.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    if path.is_empty() {
        return None;
    }
    Some(path)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Built from a line list rather than one long literal: a `\\` line
    /// continuation in a Rust string eats the next line's leading spaces,
    /// and the indentation of an objdump instruction line is exactly what
    /// the parser keys on.
    fn sample() -> String {
        [
            "main.elf:     file format elf32-littlemips",
            "",
            "Disassembly of section .text:",
            "",
            "0000000000100000 <_start-0x8>:",
            "\t...",
            "",
            "0000000000100008 <_start>:",
            "_start():",
            "/usr/local/sce/ee/lib/crt0.s:32",
            "  100008:\t3c020064 \tlui\t$v0,0x64",
            "/usr/local/sce/ee/lib/crt0.s:33",
            "  10000c:\t3c030074 \tlui\t$v1,0x74",
            "\t...",
            "  100018:\t7c400000 \tsq\t$zero,0($v0)",
            "",
            "00000000001000b8 <_exit>:",
            "way_util.c:41",
            "  1000b8:\t03e00008 \tjr\t$ra",
            "  1000bc:\t00000000 \tnop",
            "",
            "Disassembly of section .vutext:",
            "",
            "000000000028db40 <ClusterMicroProgram>:",
            "  28db40:\tda 00 00 60 \tdmaret 0xda",
            "  28db44:\t00 00 00 00 ",
        ]
        .join("\n")
    }

    #[test]
    fn parses_labels_instructions_and_sources() {
        let od = Objdump::parse(&sample());
        let names: Vec<&str> = od.functions.iter().map(|f| f.name.as_str()).collect();
        assert_eq!(names, ["_start", "_exit", "ClusterMicroProgram"]);

        let start = &od.functions[0];
        assert_eq!(start.vram, 0x0010_0008);
        assert_eq!(start.section, ".text");
        assert_eq!(start.source_file.as_deref(), Some("/usr/local/sce/ee/lib/crt0.s"));
        assert!(start.is_vendor());
        assert_eq!(start.insns.len(), 3);
        assert_eq!(start.insns[0].word, 0x3C02_0064);
        assert_eq!(start.insns[0].text.as_deref(), Some("lui\t$v0,0x64"));

        // The `...` elision leaves a hole: two runs, not one.
        let runs = start.runs();
        assert_eq!(runs.len(), 2);
        assert_eq!(runs[0].len(), 2);
        assert_eq!(runs[1].len(), 1);

        let exit = &od.functions[1];
        assert_eq!(exit.source_file.as_deref(), Some("way_util.c"));
        assert!(!exit.is_vendor());

        // .vutext byte-dump form, little endian, and a text-less second half.
        let vu = &od.functions[2];
        assert_eq!(vu.section, ".vutext");
        assert_eq!(vu.insns[0].word, 0x6000_00DA);
        assert_eq!(vu.insns[1].word, 0);
        assert!(vu.insns[1].text.is_none());
    }
}
