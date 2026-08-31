//! Data model for `ProgramDb`. Every struct here is metadata (names, addresses,
//! counts, ranges) derived from the decomp repo's inputs. Nothing here holds
//! raw ELF/ROM bytes or raw instruction words: that keeps `generated/programdb.json`
//! small and keeps us honest about the "no game bytes committed" rule even
//! though the file itself is gitignored.

use serde::{Deserialize, Serialize};

/// One ELF section, as read from the boot ELF's section headers.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ElfSection {
    pub name: String,
    pub vram: u32,
    pub file_offset: u32,
    pub size: u32,
    pub executable: bool,
    pub writable: bool,
    /// True for NOBITS sections (.bss, .sbss, .vubss): no on-disk bytes.
    pub nobits: bool,
}

impl ElfSection {
    pub fn vram_end(&self) -> u32 {
        self.vram + self.size
    }

    pub fn contains_vram(&self, vram: u32) -> bool {
        self.size > 0 && vram >= self.vram && vram < self.vram_end()
    }
}

/// Splat subsegment "type" field, generalized. Dot-prefixed splat types
/// (`.rodata`, `.data`, `.lit4`, `.sdata`) mark a carved chunk with real
/// symbol ownership; the bare form (`rodata`, `data`, ...) marks an
/// unattributed resume blob owned by a `src/cod/<offset>` placeholder path.
/// `carved` on `TranslationUnit` records that distinction.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum SubsegKind {
    /// `c`: a real, C-source translation unit's code.
    Code,
    /// `asm`: raw disassembly not yet attributed to a TU (vendor code).
    Asm,
    /// `hasm`: hand-written assembly TU (VU1 microprograms).
    HandAsm,
    Data,
    RoData,
    Lit4,
    SData,
    SBss,
    Bss,
    TextBin,
    /// Anything not recognized; kept instead of hard-erroring on new splat
    /// subsegment types, but surfaced in `stats()` so it doesn't go unnoticed.
    Other,
}

impl SubsegKind {
    /// Code-bearing kinds: functions can only live inside these.
    pub fn is_code(self) -> bool {
        matches!(
            self,
            SubsegKind::Code | SubsegKind::Asm | SubsegKind::HandAsm
        )
    }
}

/// One splat subsegment: `[rom_offset, type, path]` or the bss/sbss dict form.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TranslationUnit {
    /// Splat path, e.g. `src/delayFreeManager` or the placeholder
    /// `src/cod/1C80` used for unattributed blobs.
    pub name: String,
    pub kind: SubsegKind,
    /// Raw splat type string (e.g. ".rodata", "c", "hasm"), preserved for
    /// callers that need the exact splat vocabulary.
    pub raw_kind: String,
    /// True for dot-prefixed subsegment types (a carved, attributed chunk).
    pub carved: bool,
    pub rom_start: u32,
    pub rom_end: u32,
    pub vram_start: u32,
    pub vram_end: u32,
}

impl TranslationUnit {
    pub fn vram_size(&self) -> u32 {
        self.vram_end.saturating_sub(self.vram_start)
    }

    pub fn contains_vram(&self, vram: u32) -> bool {
        vram >= self.vram_start && vram < self.vram_end
    }
}

/// A non-function symbol from `symbol_addrs.us.txt` (data, jtbl markers,
/// literal pool slots, etc). Function symbols become `Function` records
/// instead; jtbl symbols are cross-checked against jump tables discovered by
/// walking the asm, not represented here.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Symbol {
    pub name: String,
    pub vram: u32,
    /// splat `type:` attribute, e.g. "u8", "s32", "asciz", "jtbl".
    pub kind: String,
    pub size: Option<u32>,
    pub vendor: bool,
    /// splat `function_owner:` attribute, when present.
    pub function_owner: Option<String>,
    /// splat `defined:` attribute, when present.
    pub defined: Option<bool>,
}

/// One function, derived primarily from `symbol_addrs.us.txt`'s `type:func`
/// entries, sized by delta-to-next-function within its owning TU / section.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Function {
    pub name: String,
    pub vram: u32,
    /// Size derived from the next function's start (capped at the owning
    /// TU's end), cross-checked against `declared_size` when present.
    pub size: u32,
    /// The `size:` attribute from symbol_addrs, when the file states one
    /// explicitly.
    pub declared_size: Option<u32>,
    /// Index into `ProgramDb::translation_units`.
    pub tu_index: usize,
    pub vendor: bool,
    pub is_jtbl_target: bool,
}

/// How a `JumpTable`'s `owner` was determined.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum OwnerResolution {
    /// A `glabel NAME ... endlabel NAME` body in `asm/nonmatchings` or
    /// `asm/matchings` textually references this table (the
    /// `%hi(jtbl_...)/%lo(jtbl_...)` load pair). This is the strong case:
    /// the compiler-emitted reference names the owner directly.
    GlabelReference,
    /// No `glabel` body references the table — its owning function is
    /// already matched from C, so splat no longer emits a per-function
    /// `.s` stub with the load instructions, only the `.rodata` carve
    /// under `asm/data`. Ownership was inferred instead as the function
    /// whose `[vram, vram+size)` range contains the majority of the
    /// table's non-null target vrams (every table's targets land in one
    /// function in this binary, so a plurality is already decisive).
    TargetMajority,
}

/// A jump table, discovered by walking `dlabel jtbl_XXXXXXXX` blocks in the
/// disassembly. Deliberately holds only facts (addresses, a count, target
/// addresses): never the raw `.word` instruction encodings.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JumpTable {
    pub name: String,
    pub vram: u32,
    /// Name of the function whose switch/computed-jump loads this table.
    pub owner: String,
    pub owner_resolved_via: OwnerResolution,
    pub entry_count: usize,
    /// Resolved target vrams, in table order. A `0` entry is a genuine
    /// null/unreached slot present in the source (align padding etc.), not
    /// a parse failure.
    pub targets: Vec<u32>,
    /// `size:` attribute from symbol_addrs.us.txt, when the table is also
    /// manually annotated there.
    pub declared_size: Option<u32>,
}

/// Per-category counts, useful for smoke-testing an ingest run and for
/// progress reporting.
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct Stats {
    pub sections: usize,
    pub translation_units: usize,
    pub code_translation_units: usize,
    pub functions: usize,
    pub vendor_functions: usize,
    pub jtbl_target_functions: usize,
    pub symbols: usize,
    pub jump_tables: usize,
    /// Tables owned via a direct `glabel` textual reference.
    pub jump_tables_via_glabel: usize,
    /// Tables owned via the target-vram-majority fallback (matched-C
    /// functions with no `.s` stub to scan).
    pub jump_tables_via_target_majority: usize,
    pub jump_table_entries: usize,
    pub function_size_mismatches: usize,
}

/// The full parsed program: everything the translator needs to know about
/// the ICO retail US binary, short of the raw bytes themselves.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProgramDb {
    pub elf_sha1: String,
    pub entry_vram: u32,
    pub vram_base: u32,
    pub gp: u32,
    pub sections: Vec<ElfSection>,
    pub translation_units: Vec<TranslationUnit>,
    pub symbols: Vec<Symbol>,
    pub functions: Vec<Function>,
    pub jump_tables: Vec<JumpTable>,
}

impl ProgramDb {
    /// Look up the function whose range contains `vram`, if any.
    pub fn function_at(&self, vram: u32) -> Option<&Function> {
        // Functions are stored sorted by vram (see loader).
        let idx = self.functions.partition_point(|f| f.vram <= vram);
        if idx == 0 {
            return None;
        }
        let f = &self.functions[idx - 1];
        if vram >= f.vram && vram < f.vram + f.size {
            Some(f)
        } else {
            None
        }
    }

    /// Exact-address function lookup.
    pub fn function_by_vram(&self, vram: u32) -> Option<&Function> {
        let idx = self.functions.partition_point(|f| f.vram < vram);
        self.functions.get(idx).filter(|f| f.vram == vram)
    }

    /// Look up the translation unit whose range contains `vram`, if any.
    pub fn tu_at(&self, vram: u32) -> Option<&TranslationUnit> {
        self.translation_units
            .iter()
            .find(|tu| tu.contains_vram(vram))
    }

    /// Iterate the functions belonging to one translation unit, in vram
    /// order.
    pub fn functions_in_tu<'a>(
        &'a self,
        tu_index: usize,
    ) -> impl Iterator<Item = &'a Function> + 'a {
        self.functions
            .iter()
            .filter(move |f| f.tu_index == tu_index)
    }

    pub fn jump_table_by_name(&self, name: &str) -> Option<&JumpTable> {
        self.jump_tables.iter().find(|j| j.name == name)
    }

    pub fn stats(&self) -> Stats {
        Stats {
            sections: self.sections.len(),
            translation_units: self.translation_units.len(),
            code_translation_units: self
                .translation_units
                .iter()
                .filter(|tu| tu.kind.is_code())
                .count(),
            functions: self.functions.len(),
            vendor_functions: self.functions.iter().filter(|f| f.vendor).count(),
            jtbl_target_functions: self.functions.iter().filter(|f| f.is_jtbl_target).count(),
            symbols: self.symbols.len(),
            jump_tables: self.jump_tables.len(),
            jump_tables_via_glabel: self
                .jump_tables
                .iter()
                .filter(|j| j.owner_resolved_via == OwnerResolution::GlabelReference)
                .count(),
            jump_tables_via_target_majority: self
                .jump_tables
                .iter()
                .filter(|j| j.owner_resolved_via == OwnerResolution::TargetMajority)
                .count(),
            jump_table_entries: self.jump_tables.iter().map(|j| j.entry_count).sum(),
            function_size_mismatches: self
                .functions
                .iter()
                .filter(|f| f.declared_size.map(|d| d != f.size).unwrap_or(false))
                .count(),
        }
    }
}
