//! `ee` subcommand: drive the ee-emit translator over the whole EE `.text`.

use std::path::{Path, PathBuf};
use std::time::Instant;

use anyhow::{Context, Result};
use ee_emit::EntryHooks;
use ingest::{DiscIngest, RecompConfig};

use crate::target_paths::{check_out_dir, default_out_dir, entry_hooks_path};

pub fn run(
    config_path: &Path,
    out_dir: Option<PathBuf>,
    census_only: bool,
    entry_report_only: bool,
) -> Result<bool> {
    let start = Instant::now();
    let cfg = RecompConfig::load(config_path)?;
    let out_dir = out_dir.unwrap_or_else(|| default_out_dir("ee"));

    let ingest = ingest::load_disc(config_path)?;
    let report = ingest::describe_disc_ingest(&ingest);
    print!("{report}");
    let image = ingest::load_elf_image(config_path)?;

    if entry_report_only {
        // The report and nothing else: the entry proofs and the pointer
        // sweep are what this run is for, and emitting the whole .text to
        // find out what they said would take minutes and write a tree.
        // check_out_dir still applies: the report is ROM-derived.
        check_out_dir(config_path, &out_dir)?;
        std::fs::create_dir_all(&out_dir)
            .with_context(|| format!("creating {}", out_dir.display()))?;
        write_entry_report(&cfg, &ingest, &report, &out_dir)?;
        return Ok(true);
    }

    if census_only {
        let counts = ee_emit::census(&ingest.db, &image)?;
        let total: usize = counts.values().sum();
        let mut sorted: Vec<_> = counts.iter().collect();
        sorted.sort_by(|a, b| b.1.cmp(a.1).then(a.0.cmp(b.0)));
        println!("census: {} mnemonics, {} instructions", counts.len(), total);
        for (m, n) in sorted {
            println!("  {n:8}  {m}");
        }
        return Ok(true);
    }

    // Mechanical gate: translated output is ROM-derived and must never land
    // on a committable path.
    check_out_dir(config_path, &out_dir)?;

    // config/entry_hooks.txt sits next to the config file, so it is
    // resolved from the config path rather than from the process's working
    // directory: a hook address is a fact about one binary.
    let hooks_path = entry_hooks_path(config_path)?;
    let hooks = EntryHooks::load(&hooks_path)?;

    let source = format!("the boot ELF {}", cfg.elf_path.display());
    let emit_report = ee_emit::emit_all(&ingest.db, &image, &hooks, &out_dir, &source)?;
    println!("{emit_report}");
    write_entry_report(&cfg, &ingest, &report, &out_dir)?;
    println!("ee: done in {:.2}s", start.elapsed().as_secs_f64());
    Ok(true)
}

/// What the ingest measured, written to `entry_gaps.txt` in the output
/// directory and named by the runtime's `bad indirect call` fatal.
///
/// Two things go in it. First, the ingest's own counts, the same text the
/// run printed: what the correlation placed, what it could not, which rules
/// admitted or dropped an address. Second, the whole-`.text` pointer sweep:
/// every address a `lui`/`addiu` pair forms that no entry proof turned into
/// a function. Each of those is either data or a function this ingest did
/// not find, and the last column says which the words at the address argue
/// for. Nothing is done with them.
///
/// The output directory is under `generated/`, which is gitignored, so the
/// report never becomes committed ROM-derived data.
fn write_entry_report(
    cfg: &RecompConfig,
    ingest: &DiscIngest,
    ingest_report: &str,
    out_dir: &Path,
) -> Result<()> {
    use std::fmt::Write as _;

    let db = &ingest.db;
    let mut body = String::new();
    let _ = writeln!(
        body,
        "Function boundaries and names for {}, from the ELF's own entry proofs and \
         from {} correlated onto it. Nothing else is consulted.\n",
        cfg.elf_path.display(),
        cfg.disc.objdump_path.display()
    );
    body.push_str(ingest_report);

    let _ = writeln!(
        body,
        "\nunresolved .text pointers ({}): every address a lui/addiu pair forms inside \
         .text that no entry proof turned into a function. Nothing is done with these. \
         Each is either data or an entry this ingest did not find; the last column says \
         which the words at the address argue for. An indirect call to one of them is \
         the runtime's `bad indirect call` fatal.\n",
        db.unresolved_pointers.len()
    );
    for u in &db.unresolved_pointers {
        let inside = match (&u.containing, u.containing_vram) {
            (Some(name), Some(vram)) => format!("inside {name} ({vram:#010X})"),
            _ => "outside every known function".to_string(),
        };
        let _ = writeln!(
            body,
            "  {:#010X}  formed at {:#010X}  {:52}  {}",
            u.target, u.site, inside, u.looks
        );
    }
    println!(
        "unresolved .text pointers: {} (listed in {})",
        db.unresolved_pointers.len(),
        out_dir.join("entry_gaps.txt").display()
    );

    std::fs::write(out_dir.join("entry_gaps.txt"), body)
        .with_context(|| format!("writing {}", out_dir.join("entry_gaps.txt").display()))?;
    Ok(())
}
