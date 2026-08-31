mod verify_decode;

use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Parser, Subcommand};

#[derive(Parser)]
#[command(name = "icorecomp", about = "ICO static recompiler toolchain")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Diff our R5900 disassembly against the decomp repo's asm baselines.
    VerifyDecode {
        /// Path to recomp.toml.
        #[arg(long, default_value = "config/recomp.toml")]
        config: PathBuf,
        /// Maximum number of concrete diffs to print.
        #[arg(long, default_value_t = 50)]
        max_diffs: usize,
    },
}

fn main() -> ExitCode {
    let cli = Cli::parse();
    let result = match cli.cmd {
        Cmd::VerifyDecode { config, max_diffs } => verify_decode::run(&config, max_diffs),
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
