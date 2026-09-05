//! Boot ELF parsing: section table, entry point, and a byte accessor by
//! vram. The raw bytes never leave this struct (it is not `Serialize`);
//! `ProgramDb` only ever gets the metadata in `ElfImage::sections`.

use std::path::Path;

use anyhow::{bail, Context, Result};
use object::{Object, ObjectSection, SectionFlags, SectionKind as ObjSectionKind};
use sha1::{Digest, Sha1};

use crate::model::ElfSection;

/// The parsed boot ELF: section metadata plus a byte accessor. Intentionally
/// not `Serialize`: this is a runtime helper for callers that need actual
/// bytes (e.g. the decoder), not something that belongs in `programdb.json`.
pub struct ElfImage {
    pub entry_vram: u32,
    pub sections: Vec<ElfSection>,
    data: Vec<u8>,
}

impl ElfImage {
    /// Read, SHA-1-verify, and parse the boot ELF at `path`. `pin_source`
    /// is the config file the pin came from, so a mismatch names the file
    /// the reader has to look at rather than always naming the US one.
    pub fn load(path: &Path, expected_sha1_hex: &str, pin_source: &Path) -> Result<Self> {
        let data =
            std::fs::read(path).with_context(|| format!("reading ELF at {}", path.display()))?;

        let mut hasher = Sha1::new();
        hasher.update(&data);
        let got = hex_encode(&hasher.finalize());
        let want = expected_sha1_hex.trim().to_lowercase();
        if got != want {
            bail!(
                "SHA-1 mismatch for {}: expected {want}, got {got}. \
                 Refusing to parse a boot ELF that doesn't match {}'s [pins].elf_sha1 \
                 pin.",
                path.display(),
                pin_source.display()
            );
        }

        let file = object::File::parse(&*data)
            .with_context(|| format!("parsing ELF at {}", path.display()))?;

        let entry_vram = u32::try_from(file.entry()).with_context(|| {
            format!(
                "ELF entry point {:#x} does not fit in 32 bits",
                file.entry()
            )
        })?;

        let mut sections = Vec::new();
        for sec in file.sections() {
            let name = sec
                .name()
                .with_context(|| "reading ELF section name")?
                .to_string();
            let vram = u32::try_from(sec.address())
                .with_context(|| format!("section {name} address does not fit in 32 bits"))?;
            let size = u32::try_from(sec.size())
                .with_context(|| format!("section {name} size does not fit in 32 bits"))?;
            let file_offset = sec.file_range().map(|(off, _)| off).unwrap_or(0);
            let file_offset = u32::try_from(file_offset)
                .with_context(|| format!("section {name} file offset does not fit in 32 bits"))?;
            let nobits = matches!(sec.kind(), ObjSectionKind::UninitializedData);
            let (executable, writable) = match sec.flags() {
                SectionFlags::Elf { sh_flags } => (
                    sh_flags & u64::from(object::elf::SHF_EXECINSTR) != 0,
                    sh_flags & u64::from(object::elf::SHF_WRITE) != 0,
                ),
                _ => (false, false),
            };

            sections.push(ElfSection {
                name,
                vram,
                file_offset,
                size,
                executable,
                writable,
                nobits,
            });
        }

        Ok(ElfImage {
            entry_vram,
            sections,
            data,
        })
    }

    /// Section containing `vram`, if any.
    pub fn section_at(&self, vram: u32) -> Option<&ElfSection> {
        self.sections.iter().find(|s| s.contains_vram(vram))
    }

    /// Read `len` bytes starting at `vram`. Returns `None` if the range
    /// isn't wholly inside one on-disk (non-NOBITS) section.
    pub fn read_at(&self, vram: u32, len: usize) -> Option<&[u8]> {
        let len_u32 = u32::try_from(len).ok()?;
        let section = self.section_at(vram)?;
        if section.nobits {
            return None;
        }
        let end = vram.checked_add(len_u32)?;
        if end > section.vram_end() {
            return None;
        }
        let start_off = section.file_offset as usize + (vram - section.vram) as usize;
        self.data.get(start_off..start_off + len)
    }

    /// The bytes of one section, addressed by its own file offset rather
    /// than by vram.
    ///
    /// `read_at` resolves a section by vram, and two sections of this ELF
    /// share one: `.DVP.ovlytab` and `.DVP.ovlystrtab` both start at
    /// 0x736198. A vram read of the second therefore lands in the first and
    /// fails its end test, so the section would be skipped, and a section
    /// smaller than its overlapping neighbour would be read from the wrong
    /// file offset entirely. Reading by file offset cannot confuse the two.
    ///
    /// `None` for a NOBITS section (nothing on disk) or a truncated file.
    pub fn section_bytes(&self, section: &ElfSection) -> Option<&[u8]> {
        if section.nobits {
            return None;
        }
        let start = section.file_offset as usize;
        let end = start.checked_add(section.size as usize)?;
        self.data.get(start..end)
    }

    /// Build an image from parts, without an ELF file on disk. Tests only:
    /// it is how the ingest's end-to-end test drives `disc::load` over a
    /// handful of synthetic functions instead of over the retail binary,
    /// which CI does not have.
    #[cfg(test)]
    pub(crate) fn from_parts(entry_vram: u32, sections: Vec<ElfSection>, data: Vec<u8>) -> Self {
        ElfImage {
            entry_vram,
            sections,
            data,
        }
    }

    /// Read a single little-endian u32 at `vram` (MIPS EE is little-endian).
    pub fn read_u32(&self, vram: u32) -> Option<u32> {
        let bytes = self.read_at(vram, 4)?;
        Some(u32::from_le_bytes(bytes.try_into().ok()?))
    }
}

fn hex_encode(bytes: &[u8]) -> String {
    use std::fmt::Write;
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        write!(s, "{b:02x}").unwrap();
    }
    s
}
