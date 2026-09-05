//! Ground-truth verification of the VU1 decoder against the retail
//! microprograms.
//!
//! The input is the `.vutext` section of the SHA-1-pinned boot ELF named by
//! `[inputs]` in `config/recomp.toml`, which `setup.sh` extracts from the
//! user's own disc. The test skips with a notice when that ELF is absent
//! (CI has no disc); with it present it is the mechanical gate for this
//! crate's tables.
//!
//! Two further tests lived here until 2026-09-05: a cross-check against the
//! decomp's `tools/disasm_vu0.py` and a round trip through its
//! `tools/assemble_vu0.py` and five hand-written microprogram sources. The
//! decomp is not an input to this project any more (CLAUDE.md), so neither
//! could run at all, and neither is kept as a permanently skipped test. What
//! still verifies the emitted microcode against something independent is the
//! differential gate in `vu-interp` (interpreter against compiled emit) and
//! the runtime's own upload-hash check.

use std::path::{Path, PathBuf};

use sha1::{Digest, Sha1};
use vu_decode::{decode_program, is_embedded_chain_tag, LowerSlot};

/// .vutext vram range, read from the pinned boot ELF's section header.
const VUTEXT_VADDR: u32 = 0x0028_9BD0;
const VUTEXT_VEND: u32 = 0x0028_ECB0;
const VUTEXT_BUNDLES: usize = 2588;

struct Config {
    elf: PathBuf,
    elf_sha1: String,
}

/// The five microprograms `.vutext` holds, which is what the leading and
/// trailing DMA chain tags are counted against. Measured on SCES_507.60 and
/// re-derived by `disc::dma_fragments` on every ingest run.
const VUTEXT_PROGRAMS: usize = 5;

fn repo_root() -> PathBuf {
    // crates/vu-decode -> crates -> recomp -> tools -> repo root
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("../../../..")
        .canonicalize()
        .expect("repo root")
}

/// Returns None when the disc inputs are absent (CI), in which case the test
/// skips: it needs the user's own ELF and nothing in this repository can
/// supply one.
fn load_config() -> Option<Config> {
    let root = repo_root();
    let path = root.join("config/recomp.toml");
    let text = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", path.display()));
    let v: toml::Value = toml::from_str(&text).expect("recomp.toml parses");
    let inputs = v.get("inputs").expect("config/recomp.toml has an [inputs] table");
    let elf_name = inputs
        .get("elf")
        .and_then(|e| e.as_str())
        .expect("[inputs].elf");
    // A relative [inputs].root is repo-relative, the same rule
    // ingest::RecompConfig::load applies.
    let inputs_root = inputs
        .get("root")
        .and_then(|r| r.as_str())
        .expect("[inputs].root");
    let elf = root.join(inputs_root).join(elf_name);
    if !elf.is_file() {
        eprintln!(
            "skipping: the boot ELF {} named by [inputs] in config/recomp.toml is not \
             present; run ./setup.sh <your disc image> to extract it",
            elf.display()
        );
        return None;
    }
    let elf_sha1 = v
        .get("pins")
        .and_then(|p| p.get("elf_sha1"))
        .and_then(|h| h.as_str())
        .expect("[pins].elf_sha1")
        .to_lowercase();
    Some(Config { elf, elf_sha1 })
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
    assert_eq!(chain_tags, 2 * VUTEXT_PROGRAMS, "chain tag count");
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
