//! Data model for `ProgramDb`. Every struct here is metadata (names, addresses,
//! counts, ranges) derived from the retail ELF and the disc's own objdump
//! listing (see `disc`). Nothing here holds
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

/// What a translation unit holds. The disc ingest produces two of these:
/// `Code` for a run of functions sharing a donor source file, and `HandAsm`
/// for each of the five VU1 microprograms carved out of `.vutext`. The data
/// kinds are carried because the vocabulary is the one the generated file
/// names and the runtime's registration table already use.
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
    /// Anything not recognized; kept instead of hard-erroring, and surfaced
    /// in `stats()` so it does not go unnoticed.
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

/// One translation unit: a contiguous vram range and the file name the
/// emitted C for it takes.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TranslationUnit {
    /// Source-derived path, e.g. `src/delayFreeManager` from the donor
    /// listing's own source file, or the placeholder `src/cod/1C80` for a
    /// run of functions no donor function named.
    pub name: String,
    pub kind: SubsegKind,
    /// Raw type string (e.g. "c", "hasm"), preserved for callers that need
    /// the exact vocabulary.
    pub raw_kind: String,
    /// True for a carved, attributed chunk. Nothing on the disc path sets
    /// it.
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

/// One function: an entry address the correlation or one of the ELF's own
/// entry proofs established, sized by delta-to-next-entry within its owning
/// TU / section.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Function {
    pub name: String,
    pub vram: u32,
    /// Size derived from the next function's start (capped at the owning
    /// TU's end), cross-checked against `declared_size` when present.
    pub size: u32,
    /// A size the input stated explicitly, when it states one. Nothing on
    /// the disc path does, so it is always `None` today; it stays because
    /// `size` above is derived and the two are worth telling apart.
    pub declared_size: Option<u32>,
    /// Index into `ProgramDb::translation_units`.
    pub tu_index: usize,
    pub vendor: bool,
    pub is_jtbl_target: bool,
}

/// A jump table, recovered from a read-only data section by
/// `scan::scan_jump_tables`: a run of words that all point inside one
/// function's byte range. Deliberately holds only facts (addresses, a count,
/// target addresses): never the raw `.word` encodings.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct JumpTable {
    pub name: String,
    pub vram: u32,
    /// Name of the function whose switch/computed-jump loads this table:
    /// the one whose byte range its targets land in.
    pub owner: String,
    pub entry_count: usize,
    /// Resolved target vrams, in table order. A `0` entry is a genuine
    /// null/unreached slot present in the source (align padding etc.), not
    /// a parse failure.
    pub targets: Vec<u32>,
    /// A size the input stated explicitly. Nothing on the disc path states
    /// one, so it is always `None` today.
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
    pub jump_tables: usize,
    pub jump_table_entries: usize,
    pub function_size_mismatches: usize,
}

/// The full parsed program: everything the translator needs to know about
/// the ICO retail PAL binary, short of the raw bytes themselves.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ProgramDb {
    pub elf_sha1: String,
    pub entry_vram: u32,
    pub vram_base: u32,
    pub gp: u32,
    pub sections: Vec<ElfSection>,
    pub translation_units: Vec<TranslationUnit>,
    pub functions: Vec<Function>,
    pub jump_tables: Vec<JumpTable>,
    /// Every `.text` address a `lui`/`addiu` pair forms that no entry proof
    /// turned into a function. The whole-`.text` sweep: each of these is an
    /// address the guest can put in a function pointer and the runtime would
    /// have no translated function for, so each is either data or an entry
    /// no proof reached.
    #[serde(default)]
    pub unresolved_pointers: Vec<UnresolvedPointer>,
}

/// One `.text` address a `lui`/`addiu` pair forms that no proof turned
/// into a function entry. Reported because an indirect call to one of these
/// is the runtime's `bad indirect call` fatal.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UnresolvedPointer {
    /// The address the pair forms.
    pub target: u32,
    /// The `addiu`/`ori` that forms it.
    pub site: u32,
    /// The function whose range covers `target`, when one does.
    pub containing: Option<String>,
    pub containing_vram: Option<u32>,
    /// What the words at `target` look like, in the terms the entry proofs
    /// are stated in.
    pub looks: String,
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
            jump_tables: self.jump_tables.len(),
            jump_table_entries: self.jump_tables.iter().map(|j| j.entry_count).sum(),
            function_size_mismatches: self
                .functions
                .iter()
                .filter(|f| f.declared_size.map(|d| d != f.size).unwrap_or(false))
                .count(),
        }
    }
}
