//! Ground-truth verification against the retail VU1 microprograms.
//!
//! Inputs come from the read-only decomp checkout configured in
//! `config/recomp.toml` (nothing from it is written into this repo):
//! - the `.vutext` section of the pinned boot ELF (five VU1 microprograms),
//! - `tools/disasm_vu0.py` as an independent reference disassembler,
//! - `tools/assemble_vu0.py` plus the five hand-written microprogram
//!   sources, whose assembled bytes must reproduce the ELF slice exactly
//!   (this pins down the section extraction and program boundaries).
//!
//! These tests skip with a notice when the decomp checkout is absent (CI);
//! with it present they are the mechanical gate for this crate's tables.

use std::path::{Path, PathBuf};
use std::process::Command;

use sha1::{Digest, Sha1};
use vu_decode::{
    compat, decode_program, is_embedded_chain_tag, Bundle, LowerOp, LowerSlot, UpperOp,
};

/// .vutext vram range for SCUS_971.13, from the project plan.
const VUTEXT_VADDR: u32 = 0x0026F5E0;
const VUTEXT_VEND: u32 = 0x002746C0;
const VUTEXT_BUNDLES: usize = 2588;

struct Config {
    decomp_root: PathBuf,
    elf: PathBuf,
    elf_sha1: String,
    vu1_sources: Vec<PathBuf>,
}

fn repo_root() -> PathBuf {
    // crates/vu-decode -> crates -> recomp -> tools -> repo root
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../..")
        .canonicalize()
        .expect("repo root")
}

/// Returns None when the sibling decomp checkout is absent (e.g. CI). The
/// ground-truth tests skip in that case; they are local gates, same
/// convention as the ingest crate's tests.
fn load_config() -> Option<Config> {
    let root = repo_root();
    let path = root.join("config/recomp.toml");
    let text = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", path.display()));
    let v: toml::Value = toml::from_str(&text).expect("recomp.toml parses");
    let decomp = &v["decomp"];
    let decomp_root = root.join(decomp["root"].as_str().expect("decomp.root"));
    let decomp_root = match decomp_root.canonicalize() {
        Ok(p) => p,
        Err(_) => {
            eprintln!(
                "skipping: decomp checkout not found at {}",
                decomp_root.display()
            );
            return None;
        }
    };
    Some(Config {
        elf: decomp_root.join(decomp["elf"].as_str().expect("decomp.elf")),
        elf_sha1: v["pins"]["elf_sha1"]
            .as_str()
            .expect("pins.elf_sha1")
            .to_lowercase(),
        vu1_sources: decomp["vu1_sources"]
            .as_array()
            .expect("decomp.vu1_sources")
            .iter()
            .map(|s| decomp_root.join(s.as_str().expect("source path")))
            .collect(),
        decomp_root,
    })
}

/// Extract the named section from a 32-bit little-endian ELF.
fn elf_section(elf: &[u8], want: &str) -> Option<(u32, Vec<u8>)> {
    assert_eq!(&elf[0..4], b"\x7fELF", "ELF magic");
    assert_eq!(elf[4], 1, "32-bit ELF");
    assert_eq!(elf[5], 1, "little-endian ELF");
    let rd32 = |off: usize| u32::from_le_bytes(elf[off..off + 4].try_into().unwrap());
    let rd16 = |off: usize| u16::from_le_bytes(elf[off..off + 2].try_into().unwrap());
    let shoff = rd32(0x20) as usize;
    let shentsize = rd16(0x2E) as usize;
    let shnum = rd16(0x30) as usize;
    let shstrndx = rd16(0x32) as usize;
    let strtab_off = rd32(shoff + shstrndx * shentsize + 0x10) as usize;
    for i in 0..shnum {
        let sh = shoff + i * shentsize;
        let name_off = strtab_off + rd32(sh) as usize;
        let name_end = elf[name_off..].iter().position(|&b| b == 0).unwrap() + name_off;
        if &elf[name_off..name_end] == want.as_bytes() {
            let addr = rd32(sh + 0x0C);
            let off = rd32(sh + 0x10) as usize;
            let size = rd32(sh + 0x14) as usize;
            return Some((addr, elf[off..off + size].to_vec()));
        }
    }
    None
}

fn read_pinned_elf(cfg: &Config) -> Vec<u8> {
    let elf = std::fs::read(&cfg.elf)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", cfg.elf.display()));
    let mut h = Sha1::new();
    h.update(&elf);
    let hex: String = h.finalize().iter().map(|b| format!("{b:02x}")).collect();
    assert_eq!(hex, cfg.elf_sha1, "boot ELF does not match the SHA-1 pin");
    elf
}

fn extract_vutext(cfg: &Config) -> Vec<u8> {
    let elf = read_pinned_elf(cfg);
    let (addr, bytes) = elf_section(&elf, ".vutext").expect(".vutext section present");
    assert_eq!(addr, VUTEXT_VADDR, ".vutext vram base");
    assert_eq!(
        bytes.len(),
        (VUTEXT_VEND - VUTEXT_VADDR) as usize,
        ".vutext size"
    );
    bytes
}

fn python(cfg: &Config, script: &str, args: &[&str]) -> Vec<u8> {
    let script_path = cfg.decomp_root.join(script);
    let out = Command::new("python3")
        .arg(&script_path)
        .args(args)
        .output()
        .unwrap_or_else(|e| panic!("failed to run python3 {}: {e}", script_path.display()));
    assert!(
        out.status.success(),
        "{script} failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    out.stdout
}

fn tmp_path(name: &str) -> PathBuf {
    std::env::temp_dir().join(format!("vu_decode_{}_{name}", std::process::id()))
}

/// Every bundle in .vutext decodes; the only non-instruction words are the
/// five embedded DMA chain-tag pairs at the program boundaries.
#[test]
fn decode_all_vutext_bundles() {
    let Some(cfg) = load_config() else { return };
    let bytes = extract_vutext(&cfg);
    let bundles = decode_program(&bytes).expect("bundle-aligned section");
    assert_eq!(bundles.len(), VUTEXT_BUNDLES);

    let mut chain_tags = 0usize;
    let mut loi = 0usize;
    let mut ebits = 0usize;
    for b in &bundles {
        if b.has_invalid() {
            assert!(
                is_embedded_chain_tag(b.upper_raw, b.lower_raw),
                "invalid decode at offset 0x{:04X} that is not an embedded \
                 DMA chain tag: upper=0x{:08X} lower=0x{:08X}",
                b.offset,
                b.upper_raw,
                b.lower_raw
            );
            chain_tags += 1;
        }
        if let LowerSlot::Loi(_) = b.lower {
            loi += 1;
        }
        if b.flags.e {
            ebits += 1;
        }
    }
    // One leading tag and one end tag per microprogram.
    assert_eq!(chain_tags, 2 * cfg.vu1_sources.len(), "chain tag count");
    assert!(loi > 0, "expected LOI bundles in the VU1 programs");
    assert!(ebits > 0, "expected E-bit terminators in the VU1 programs");
    println!(
        "decoded {} bundles: {} chain tags, {} LOI, {} E-bit",
        bundles.len(),
        chain_tags,
        loi,
        ebits
    );
}

/// Parse one disasm_vu0.py listing into (offset -> decoded-column text).
/// `column` 0 takes the text before the first ';' (upper), 1 the text after
/// it with the trailing raw-bytes bracket removed (lower).
fn parse_listing(stdout: &[u8], column: usize) -> Vec<(u32, String)> {
    let text = String::from_utf8_lossy(stdout);
    let mut out = Vec::new();
    for line in text.lines() {
        let t = line.trim_start();
        if !t.starts_with("0x") {
            continue; // headers, labels, blank lines
        }
        let (addr, rest) = t.split_once(':').expect("addr colon");
        let pc = u32::from_str_radix(&addr[2..], 16).expect("hex pc");
        let rest = rest.trim_start();
        let field = if column == 0 {
            rest.split(';').next().unwrap().trim().to_string()
        } else {
            let after = rest.split_once(';').expect("column separator").1;
            let after = match after.rfind('[') {
                Some(i) => &after[..i],
                None => after,
            };
            after.trim().to_string()
        };
        out.push((pc, field));
    }
    out
}

/// Cross-check our decode, rendered in disasm_vu0.py's own format, against
/// that tool's output for the identical bytes.
#[test]
fn cross_check_against_reference_disassembler() {
    let Some(cfg) = load_config() else { return };
    let bytes = extract_vutext(&cfg);
    let bundles = decode_program(&bytes).unwrap();

    let bin = tmp_path("vutext.bin");
    std::fs::write(&bin, &bytes).unwrap();
    let bin_s = bin.to_str().unwrap();
    let upper_listing = parse_listing(
        &python(
            &cfg,
            "tools/disasm_vu0.py",
            &["--input", bin_s, "--upper-only"],
        ),
        0,
    );
    let lower_listing = parse_listing(
        &python(
            &cfg,
            "tools/disasm_vu0.py",
            &["--input", bin_s, "--lower-only"],
        ),
        1,
    );
    let _ = std::fs::remove_file(&bin);
    assert_eq!(upper_listing.len(), bundles.len());
    assert_eq!(lower_listing.len(), bundles.len());

    let mut exact_upper = 0usize;
    let mut exact_lower = 0usize;
    let mut ref_word_upper = 0usize;
    let mut ref_word_lower = 0usize;
    let mut loi_skipped = 0usize;

    for (i, b) in bundles.iter().enumerate() {
        let (pc_u, ref up_text) = upper_listing[i];
        let (pc_l, ref lo_text) = lower_listing[i];
        assert_eq!(pc_u, b.offset);
        assert_eq!(pc_l, b.offset);

        // Upper pipe.
        if up_text.starts_with(".word") {
            // Reference fallback (flag bits, CLIP, or I-bit pad). Our decode
            // must still be a defined instruction.
            assert!(
                !matches!(b.upper, UpperOp::Invalid { .. }),
                "upper 0x{:08X} at 0x{:04X}: reference punts but we decode Invalid",
                b.upper_raw,
                b.offset
            );
            ref_word_upper += 1;
        } else {
            let ours = compat::upper_compat(b).unwrap_or_else(|| {
                panic!(
                    "upper 0x{:08X} at 0x{:04X}: reference says {:?} but we \
                     have no rendering",
                    b.upper_raw, b.offset, up_text
                )
            });
            assert_eq!(
                &ours, up_text,
                "upper mismatch at 0x{:04X} (word 0x{:08X})",
                b.offset, b.upper_raw
            );
            exact_upper += 1;
        }

        // Lower pipe.
        match b.lower {
            LowerSlot::Loi(_) => {
                // The reference tool does not track the I bit and decodes
                // the immediate bits as an instruction; nothing to compare.
                loi_skipped += 1;
            }
            LowerSlot::Inst(op) => {
                if lo_text.starts_with(".word") {
                    // Only the embedded DMA chain tags fall out of the
                    // reference tool's lower coverage on this binary.
                    assert!(
                        matches!(op, LowerOp::Invalid { .. })
                            && is_embedded_chain_tag(b.upper_raw, b.lower_raw),
                        "lower 0x{:08X} at 0x{:04X}: unexpected reference fallback",
                        b.lower_raw,
                        b.offset
                    );
                    ref_word_lower += 1;
                } else {
                    let ours = compat::lower_compat(b).unwrap_or_else(|| {
                        panic!(
                            "lower 0x{:08X} at 0x{:04X}: reference says {:?} \
                             but we have no rendering",
                            b.lower_raw, b.offset, lo_text
                        )
                    });
                    assert_eq!(
                        &ours, lo_text,
                        "lower mismatch at 0x{:04X} (word 0x{:08X})",
                        b.offset, b.lower_raw
                    );
                    exact_lower += 1;
                }
            }
        }
    }

    assert_eq!(exact_upper + ref_word_upper, bundles.len());
    assert_eq!(exact_lower + ref_word_lower + loi_skipped, bundles.len());
    assert_eq!(ref_word_lower, 2 * cfg.vu1_sources.len());
    // The reference fallbacks are a small minority; everything else matched
    // token for token.
    assert!(exact_upper > 2400, "exact upper matches: {exact_upper}");
    assert!(exact_lower > 2500, "exact lower matches: {exact_lower}");
    println!(
        "cross-check: {} bundles; upper exact {}, reference .word {}; \
         lower exact {}, reference .word {}, LOI skipped {}",
        bundles.len(),
        exact_upper,
        ref_word_upper,
        exact_lower,
        ref_word_lower,
        loi_skipped
    );
}

/// The five hand-written sources, assembled by the decomp repo's clean-room
/// assembler and concatenated in config order, must equal the ELF slice.
/// This validates the extraction offsets and the program boundaries.
#[test]
fn sources_round_trip_to_elf_slice() {
    let Some(cfg) = load_config() else { return };
    let bytes = extract_vutext(&cfg);

    let mut assembled = Vec::new();
    let mut table = Vec::new();
    for src in &cfg.vu1_sources {
        let raw = tmp_path(&format!(
            "{}.bin",
            src.file_stem().unwrap().to_string_lossy()
        ));
        let src_s = src.to_str().unwrap();
        let raw_s = raw.to_str().unwrap();
        python(&cfg, "tools/assemble_vu0.py", &[src_s, "--raw", raw_s]);
        let prog = std::fs::read(&raw).unwrap();
        let _ = std::fs::remove_file(&raw);
        table.push((src.clone(), assembled.len(), prog.len()));
        assembled.extend_from_slice(&prog);
    }

    assert_eq!(assembled.len(), bytes.len(), "total assembled size");
    for (src, start, len) in &table {
        assert_eq!(
            &assembled[*start..*start + *len],
            &bytes[*start..*start + *len],
            "{} does not match .vutext at offset 0x{:04X}",
            src.display(),
            start
        );
        println!(
            "{}: .vutext+0x{:04X}, {} bytes ({} bundles)",
            src.file_name().unwrap().to_string_lossy(),
            start,
            len,
            len / 8
        );
    }
    assert_eq!(assembled, bytes, "concatenated programs equal .vutext");

    // Each program begins with a leading chain tag; the matching end tag
    // sits one quadword before the end of the program.
    for (src, start, len) in &table {
        let first: Bundle = decode_program(&bytes[*start..*start + 8]).unwrap()[0].clone();
        assert!(
            is_embedded_chain_tag(first.upper_raw, first.lower_raw),
            "{}: expected leading chain tag",
            src.display()
        );
        let tag_off = *start + *len - 16;
        let last: Bundle = decode_program(&bytes[tag_off..tag_off + 8]).unwrap()[0].clone();
        assert!(
            is_embedded_chain_tag(last.upper_raw, last.lower_raw),
            "{}: expected trailing end tag at 0x{:04X}",
            src.display(),
            tag_off
        );
    }
}
