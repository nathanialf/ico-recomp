mod ee;
mod target_paths;
mod verify_objdump;
mod vu1;

use std::path::{Path, PathBuf};
use std::process::ExitCode;

use anyhow::Result;
use clap::{Parser, Subcommand};

/// Every subcommand takes `--config`, which names the target config.
/// `config/recomp.toml` is the retail target: the boot ELF `SCES_507.60`
/// and the objdump listing `SRCFILE.TXT`, both extracted from the user's own
/// disc by `setup.sh`.
const DEFAULT_CONFIG: &str = "config/recomp.toml";

#[derive(Parser)]
#[command(name = "icorecomp", about = "ICO static recompiler toolchain")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Diff our R5900 disassembly against the GNU objdump listing the disc
    /// carries (SRCFILE.TXT).
    ///
    /// The listing prints the instruction word on every line, so the check
    /// decodes that word and compares our text against binutils' for the
    /// same word. It reads no byte of the retail ELF: see
    /// `verify_objdump.rs` and docs/TARGET.md for what that does and does
    /// not cover.
    VerifyDecode {
        /// Path to a target config.
        #[arg(long, default_value = DEFAULT_CONFIG)]
        config: PathBuf,
        /// Maximum number of concrete diffs to print.
        #[arg(long, default_value_t = 50)]
        max_diffs: usize,
    },
    /// Translate the EE .text to C11 under generated/.
    Ee {
        /// Path to a target config.
        #[arg(long, default_value = DEFAULT_CONFIG)]
        config: PathBuf,
        /// Output directory. Must live under a gitignored generated/ tree.
        /// Defaults to generated/ee.
        #[arg(long)]
        out: Option<PathBuf>,
        /// Print the mnemonic census and exit without emitting anything.
        #[arg(long)]
        census: bool,
        /// Write the entry report (the whole-.text pointer sweep) and exit
        /// without emitting anything.
        #[arg(long)]
        entry_report: bool,
    },
    /// Translate the five VU1 microprograms in .vutext to C11 under
    /// generated/ and print the latency audit.
    Vu1 {
        /// Path to a target config.
        #[arg(long, default_value = DEFAULT_CONFIG)]
        config: PathBuf,
        /// Output directory. Must live under a gitignored generated/ tree.
        /// Defaults to generated/vu1.
        #[arg(long)]
        out: Option<PathBuf>,
    },
}

fn verify(config: &Path, max_diffs: usize) -> Result<bool> {
    verify_objdump::run(config, max_diffs)
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let result = match cli.cmd {
        Cmd::VerifyDecode { config, max_diffs } => verify(&config, max_diffs),
        Cmd::Ee {
            config,
            out,
            census,
            entry_report,
        } => ee::run(&config, out, census, entry_report),
        Cmd::Vu1 { config, out } => vu1::run(&config, out),
    };
    match result {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("error: {e:#}");
            ExitCode::FAILURE
        }
    }
}
