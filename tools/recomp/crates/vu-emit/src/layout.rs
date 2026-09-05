//! .vutext upload layout: recover the bytes that land in VU1 micro memory.
//!
//! Layout determination (read from ../ico plus the retail bytes):
//!
//! Each of the five microprograms in `.vutext` is a self-contained DMA
//! source-chain fragment, transferred to VIF1 with TTE (tag transfer
//! enable) on, exactly as the linker placed it. MicroCode.c's
//! `mc_TransMicroCode` is a plain 16-quadword copy helper (`QCOPY16`); the
//! game's display-list builders copy these fragments around and hand them
//! to the DMAC untouched, so the on-disc bytes ARE the upload stream:
//!
//!   quadword 0:   w0 = DMAtag (ID=6 "ret", QWC = payload quadwords)
//!                 w1 = 0
//!                 w2 = VIF NOP (0x00000000)          } sent to VIF1 by TTE
//!                 w3 = VIF MPG num, addr=0 (0x4A..)  }
//!   payload:      QWC quadwords of VIF data:
//!                   MPG instruction data (num 64-bit instructions,
//!                   256 max per MPG), then for programs longer than 256
//!                   instructions an embedded VIF word pair
//!                   [NOP, MPG num', addr=prev_total] and more data,
//!                   repeated; then 0..1 VIF NOP pairs padding to a
//!                   quadword boundary
//!   last quadword: w0 = DMA end tag (ID=7, QWC=0), w1..w3 = 0
//!
//! The MPG addr fields are contiguous from 0, so micro memory receives the
//! concatenation of the MPG payloads at address 0. The embedded VIF word
//! pairs and the framing tags are never written to micro memory; the
//! canonical upload hash (`rc_vu1_hash` in recomp_ops.h, mirrored by
//! [`upload_hash`]) runs over exactly this concatenation, seeded with its
//! byte length. The runtime's VIF1 MPG path sees the same MPG commands and
//! must hash the same bytes.
//!
//! Measured per program (retail PAL ELF):
//!   cluster  435 instructions (MPG 256 @0 + 179 @256), no pad
//!   mesh     312 instructions (256 + 56), one NOP-pair pad
//!   normal_c 726 instructions (256 + 256 + 214), one pad
//!   normal_l 909 instructions (256 + 256 + 256 + 141), one pad
//!   particle 178 instructions (one MPG), no pad

use anyhow::{bail, Context, Result};
use ingest::{ElfImage, ProgramDb, SubsegKind};
use vu_decode::is_embedded_chain_tag;

/// One microprogram's recovered upload.
pub struct Vu1Program {
    /// Short name from the hasm TU ("cluster", "mesh", ...).
    pub name: String,
    /// vram of the .vutext fragment (tag quadword) in the ELF.
    pub vram: u32,
    /// Bytes that land in VU1 micro memory at address 0.
    pub image: Vec<u8>,
    /// (instruction address, instruction count) per MPG transfer.
    pub segments: Vec<(u32, u32)>,
}

impl Vu1Program {
    pub fn instruction_count(&self) -> u32 {
        (self.image.len() / 8) as u32
    }

    pub fn hash(&self) -> u32 {
        upload_hash(&self.image)
    }
}

/// Rust mirror of `rc_vu1_hash` (recomp_ops.h): FNV-1a 32-bit seeded with
/// the byte length. The smoke test cross-checks this against the C
/// implementation compiled from the header.
pub fn upload_hash(bytes: &[u8]) -> u32 {
    let mut h = 0x811C_9DC5u32 ^ bytes.len() as u32;
    for &b in bytes {
        h ^= b as u32;
        h = h.wrapping_mul(0x0100_0193);
    }
    h
}

/// Locate the five hasm TUs in the splat-derived ProgramDb and parse each
/// fragment's embedded DMA/VIF framing into the uploaded image.
pub fn extract_programs(db: &ProgramDb, image: &ElfImage) -> Result<Vec<Vu1Program>> {
    let mut tus: Vec<_> = db
        .translation_units
        .iter()
        .filter(|tu| tu.kind == SubsegKind::HandAsm)
        .collect();
    tus.sort_by_key(|tu| tu.vram_start);
    if tus.len() != 5 {
        bail!(
            "expected exactly 5 hasm translation units in .vutext, found {}",
            tus.len()
        );
    }

    let mut out = Vec::with_capacity(5);
    for tu in tus {
        let name = tu
            .name
            .rsplit('/')
            .next()
            .unwrap_or(tu.name.as_str())
            .to_string();
        let size = tu.vram_size() as usize;
        let bytes = image
            .read_at(tu.vram_start, size)
            .with_context(|| format!("reading .vutext bytes for {name} at {:#x}", tu.vram_start))?;
        let prog = parse_fragment(&name, tu.vram_start, bytes)
            .with_context(|| format!("parsing upload framing of {name}"))?;
        out.push(prog);
    }
    Ok(out)
}

fn word(bytes: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([bytes[off], bytes[off + 1], bytes[off + 2], bytes[off + 3]])
}

fn parse_fragment(name: &str, vram: u32, bytes: &[u8]) -> Result<Vu1Program> {
    if bytes.len() < 32 || !bytes.len().is_multiple_of(16) {
        bail!("fragment size {} is not a whole quadword count >= 2", bytes.len());
    }

    // Leading DMA source-chain tag (ID 6, "transfer QWC then return").
    let w0 = word(bytes, 0);
    let w1 = word(bytes, 4);
    if !is_embedded_chain_tag(w1, w0) || w0 >> 28 != 0x6 {
        bail!("first quadword is not the expected ID=6 chain tag ({w0:#010x} {w1:#010x})");
    }
    let qwc = (w0 & 0xFFFF) as usize;
    if 16 + qwc * 16 + 16 != bytes.len() {
        bail!(
            "chain tag QWC {qwc} does not match fragment size {} (tag + payload + end tag)",
            bytes.len()
        );
    }

    // Trailing DMA end tag (ID 7, QWC 0, rest zero).
    let end = 16 + qwc * 16;
    let e0 = word(bytes, end);
    if !is_embedded_chain_tag(word(bytes, end + 4), e0)
        || e0 >> 28 != 0x7
        || word(bytes, end + 4) != 0
        || word(bytes, end + 8) != 0
        || word(bytes, end + 12) != 0
    {
        bail!("last quadword is not the expected all-zero ID=7 end tag");
    }

    // VIF code stream: the tag's upper 64 bits (TTE) followed by the
    // payload. Only NOP and MPG may appear; MPG addresses must be
    // contiguous from 0.
    let stream = &bytes[8..16 + qwc * 16];
    let mut pos = 0usize;
    let mut total_instr = 0u32;
    let mut segments = Vec::new();
    let mut image_out = Vec::new();
    while pos < stream.len() {
        let code = word(stream, pos);
        if code == 0 {
            pos += 4; // VIF NOP (framing or quadword pad)
            continue;
        }
        let cmd = (code >> 24) & 0x7F;
        if cmd != 0x4A {
            bail!(
                "unexpected VIF code {code:#010x} at stream offset {pos:#x} \
                 (only NOP and MPG belong in a microprogram upload)"
            );
        }
        if code & 0x8000_0000 != 0 {
            bail!("MPG VIF code at stream offset {pos:#x} has the interrupt bit set");
        }
        let num = match (code >> 16) & 0xFF {
            0 => 256u32,
            n => n,
        };
        let addr = code & 0xFFFF;
        if addr != total_instr {
            bail!(
                "MPG at stream offset {pos:#x} targets address {addr}, expected the \
                 contiguous address {total_instr}"
            );
        }
        let data_start = pos + 4;
        let data_end = data_start + num as usize * 8;
        if data_end > stream.len() {
            bail!("MPG at stream offset {pos:#x} overruns the payload");
        }
        image_out.extend_from_slice(&stream[data_start..data_end]);
        segments.push((addr, num));
        total_instr += num;
        pos = data_end;
    }
    if total_instr == 0 || total_instr > 2048 {
        bail!("{name}: implausible uploaded instruction count {total_instr}");
    }

    Ok(Vu1Program {
        name: name.to_string(),
        vram,
        image: image_out,
        segments,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hash_is_fnv1a_with_length_seed() {
        // Empty input: just the seeded basis.
        assert_eq!(upload_hash(&[]), 0x811C_9DC5);
        // One byte, worked by hand:
        // h = basis ^ 1; h = (h ^ 0xAB) * 0x01000193.
        let mut h = 0x811C_9DC5u32 ^ 1;
        h ^= 0xAB;
        h = h.wrapping_mul(0x0100_0193);
        assert_eq!(upload_hash(&[0xAB]), h);
        // Length seeding: all-zero uploads of different lengths still hash
        // differently (plain FNV-1a would collapse them all to the basis
        // times powers of the prime pattern).
        assert_ne!(upload_hash(&[0, 0]), upload_hash(&[0, 0, 0]));
    }

    #[test]
    fn fragment_parser_recovers_multi_mpg_uploads() {
        // Synthetic fragment shaped like the retail layout: tag qw with
        // [NOP, MPG num=2 addr=0], 2 instructions, embedded [NOP, MPG
        // num=1 addr=2], 1 instruction, four NOP words of qw padding, end
        // tag. Payload = 1 + 0.5 + 0.5 + 1 = 3 quadwords.
        let mut b = Vec::new();
        let put = |b: &mut Vec<u8>, w: u32| b.extend_from_slice(&w.to_le_bytes());
        put(&mut b, 0x6000_0003); // DMAtag ID=6 QWC=3
        put(&mut b, 0);
        put(&mut b, 0); // VIF NOP
        put(&mut b, 0x4A02_0000); // MPG num=2 addr=0
        for w in [0x1111_1111u32, 0x2222_2222, 0x3333_3333, 0x4444_4444] {
            put(&mut b, w); // 2 instructions
        }
        put(&mut b, 0); // VIF NOP
        put(&mut b, 0x4A01_0002); // MPG num=1 addr=2
        put(&mut b, 0x5555_5555);
        put(&mut b, 0x6666_6666); // 1 instruction
        put(&mut b, 0);
        put(&mut b, 0);
        put(&mut b, 0);
        put(&mut b, 0); // qw pad
        put(&mut b, 0x7000_0000); // end tag
        put(&mut b, 0);
        put(&mut b, 0);
        put(&mut b, 0);

        let p = parse_fragment("synthetic", 0, &b).expect("parse");
        assert_eq!(p.segments, vec![(0, 2), (2, 1)]);
        assert_eq!(p.instruction_count(), 3);
        assert_eq!(&p.image[0..4], &0x1111_1111u32.to_le_bytes());
        assert_eq!(&p.image[16..20], &0x5555_5555u32.to_le_bytes());
    }

    #[test]
    fn fragment_parser_rejects_bad_framing() {
        let put = |b: &mut Vec<u8>, w: u32| b.extend_from_slice(&w.to_le_bytes());
        // Non-contiguous MPG address.
        let mut b = Vec::new();
        put(&mut b, 0x6000_0001);
        put(&mut b, 0);
        put(&mut b, 0);
        put(&mut b, 0x4A01_0005); // addr 5, expected 0
        put(&mut b, 0xAAAA_AAAA);
        put(&mut b, 0xBBBB_BBBB);
        put(&mut b, 0);
        put(&mut b, 0);
        put(&mut b, 0x7000_0000);
        put(&mut b, 0);
        put(&mut b, 0);
        put(&mut b, 0);
        assert!(parse_fragment("bad", 0, &b).is_err());
        // Missing end tag.
        let mut b = Vec::new();
        put(&mut b, 0x6000_0001);
        put(&mut b, 0);
        put(&mut b, 0);
        put(&mut b, 0x4A02_0000);
        for _ in 0..4 {
            put(&mut b, 0x1234_5678);
        }
        put(&mut b, 0x1234_5678);
        put(&mut b, 0);
        put(&mut b, 0);
        put(&mut b, 0);
        assert!(parse_fragment("bad", 0, &b).is_err());
    }
}
