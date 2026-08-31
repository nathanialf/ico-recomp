//! `vu-emit`: static recompiler for the five VU1 microprograms in ICO's
//! `.vutext` (cluster, mesh, normal_c, normal_l, particle).
//!
//! Pipeline: `layout` recovers the bytes that the VIF1 MPG upload places in
//! micro memory (stripping the embedded DMA chain tags, VIF codes, and pad
//! NOPs), `analyze` finds entry points and runs the latency audit, `emit`
//! writes one C11 function per program plus a registration table keyed by
//! the canonical upload hash (`rc_vu1_hash` in recomp_ops.h, mirrored here
//! by [`upload_hash`]).
//!
//! All output is ROM-derived and must only ever be written under a
//! gitignored `generated/` tree; the CLI enforces that.

mod analyze;
mod emit;
mod layout;

use std::fmt;
use std::path::Path;

use anyhow::{Context, Result};
use ingest::{ElfImage, ProgramDb};

pub use analyze::{analyze, Analysis, Audit};
pub use layout::{extract_programs, upload_hash, Vu1Program};

/// Per-program summary for the CLI report.
pub struct ProgramReport {
    pub name: String,
    pub vram: u32,
    pub instructions: u32,
    pub hash: u32,
    pub segments: Vec<(u32, u32)>,
    pub entries: Vec<u32>,
    pub dispatch_cases: usize,
    pub labels: usize,
    pub bundles_emitted: usize,
    pub bundles_duplicated: usize,
    pub audit: Audit,
}

pub struct Report {
    pub programs: Vec<ProgramReport>,
}

impl fmt::Display for Report {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        for p in &self.programs {
            writeln!(
                f,
                "{}: vram {:#010x}, {} instructions uploaded ({} bytes), hash {:#010x}",
                p.name,
                p.vram,
                p.instructions,
                p.instructions * 8,
                p.hash
            )?;
            let segs: Vec<String> = p
                .segments
                .iter()
                .map(|(a, n)| format!("{n}@{a}"))
                .collect();
            writeln!(f, "  upload MPG segments: {}", segs.join(" + "))?;
            let entries: Vec<String> = p.entries.iter().map(|e| format!("{e:#x}")).collect();
            writeln!(
                f,
                "  entry points ({}): {}",
                p.entries.len(),
                entries.join(" ")
            )?;
            writeln!(
                f,
                "  dispatch cases {}, labels {}, bundles emitted {} (+{} duplicated)",
                p.dispatch_cases, p.labels, p.bundles_emitted, p.bundles_duplicated
            )?;
        }
        writeln!(f, "latency audit:")?;
        let mut any = false;
        for p in &self.programs {
            let a = &p.audit;
            for (title, lines) in [
                ("FMAC result window (hardware interlocks; stall sites, immediate commit exact)", &a.fmac_stalls),
                ("same-bundle exchanges (handled by compute-then-commit ordering)", &a.same_bundle),
                ("Q pipeline events", &a.q_events),
                ("integer-branch old-value sites (old-value semantics emitted)", &a.int_branch),
                ("integer load consumed next bundle", &a.ilw_next),
                ("flag pipeline windows (NOT modeled; flags commit immediately)", &a.flag_windows),
                ("notes", &a.notes),
            ] {
                if !lines.is_empty() {
                    writeln!(f, "  [{}] {title}:", p.name)?;
                    for l in lines {
                        writeln!(f, "    {l}")?;
                    }
                    any = true;
                }
            }
        }
        if !any {
            writeln!(f, "  no findings")?;
        }
        Ok(())
    }
}

/// Translate all five microprograms into `out_dir` (one `vu1_<name>.c`
/// per program plus `vu1_table.c`) and return the report. The caller is
/// responsible for the generated-path gate.
pub fn emit_all(db: &ProgramDb, image: &ElfImage, out_dir: &Path) -> Result<Report> {
    let programs = extract_programs(db, image)?;
    std::fs::create_dir_all(out_dir)
        .with_context(|| format!("creating {}", out_dir.display()))?;

    let mut report = Report { programs: Vec::new() };
    for prog in &programs {
        let analysis = analyze(prog)?;
        let (code, stats) = emit::emit_program(prog, &analysis)?;
        let path = out_dir.join(format!("vu1_{}.c", prog.name));
        std::fs::write(&path, code).with_context(|| format!("writing {}", path.display()))?;
        report.programs.push(ProgramReport {
            name: prog.name.clone(),
            vram: prog.vram,
            instructions: prog.instruction_count(),
            hash: prog.hash(),
            segments: prog.segments.clone(),
            entries: analysis.entries.clone(),
            dispatch_cases: analysis.dispatch.len(),
            labels: analysis.labels.len(),
            bundles_emitted: stats.emitted,
            bundles_duplicated: stats.duplicated,
            audit: analysis.audit,
        });
    }

    let table = emit_table(&programs);
    let table_path = out_dir.join("vu1_table.c");
    std::fs::write(&table_path, table)
        .with_context(|| format!("writing {}", table_path.display()))?;
    Ok(report)
}

/// Registration table. The hash constants are precomputed with the Rust
/// mirror of rc_vu1_hash; the size argument doubles as the hash's length
/// seed, so the runtime can verify a candidate upload with
/// `rc_vu1_hash(bytes, size)` alone.
fn emit_table(programs: &[Vu1Program]) -> String {
    use std::fmt::Write as _;
    let mut out = String::new();
    let _ = writeln!(
        out,
        "/* vu1_table.c: generated by `icorecomp vu1`. DO NOT EDIT, DO NOT COMMIT.\n\
         * Registers every recompiled VU1 microprogram by the canonical upload\n\
         * hash (rc_vu1_hash in recomp_ops.h: FNV-1a 32-bit over the uploaded\n\
         * instruction bytes, seeded with the byte length). The runtime's VIF1\n\
         * MPG path must compute the hash with that same helper over the bytes\n\
         * it writes to micro memory. */\n\
         #include \"recomp_api.h\"\n"
    );
    for p in programs {
        let _ = writeln!(out, "void vu1_{}(Vu1State* vu);", p.name);
    }
    let _ = writeln!(out, "\nvoid rt_vu1_register_all(void);\n");
    let _ = writeln!(out, "void rt_vu1_register_all(void) {{");
    for p in programs {
        let _ = writeln!(
            out,
            "    rt_vu1_register(0x{:08X}u, {}u, vu1_{}); /* {} instructions */",
            p.hash(),
            p.image.len(),
            p.name,
            p.instruction_count()
        );
    }
    let _ = writeln!(out, "}}");
    out
}
