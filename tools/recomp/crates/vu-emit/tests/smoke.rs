//! Smoke gate for the VU1 static recompiler. Requires the boot ELF named by
//! `[inputs]` in `config/recomp.toml`, which `setup.sh` extracts from the
//! user's own disc; skips cleanly without it, since nothing in this
//! repository can supply one.
//!
//! What it proves:
//!  * the upload framing parser and emitter run over all five retail
//!    programs with full bundle coverage and no duplication,
//!  * the generated C compiles with gcc -Wall -Werror,
//!  * the C `rc_vu1_hash` (recomp_ops.h) and the Rust mirror
//!    (`vu_emit::upload_hash`) agree on the real uploaded bytes: the driver
//!    hashes the image files with the C helper and must find the matching
//!    registration constant emitted from the Rust side,
//!  * each program's offset-0 entry runs to its E-bit stop on a zeroed
//!    Vu1State for a few synthetic xtop values, without tripping
//!    rt_unimplemented, and every XGKICK the run produces addresses a
//!    quadword inside the 16 KB data memory.
//!
//! Full record/replay verification against PCSX2 traces comes later; this
//! is the crash/coverage/hash gate.

use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use ingest::{ProgramDb, RecompConfig};

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../..")
}

fn config_path() -> PathBuf {
    repo_root().join("config/recomp.toml")
}

/// The boot ELF the translator reads. Absent on CI, which has no disc.
fn boot_elf_present() -> bool {
    RecompConfig::load(&config_path())
        .map(|c| c.elf_path.is_file())
        .unwrap_or(false)
}

const DRIVER: &str = r#"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "recomp_ops.h"

typedef struct {
    uint32_t hash;
    uint32_t size;
    void (*fn)(Vu1State*);
} Reg;
static Reg g_regs[16];
static int g_nregs = 0;

void rt_vu1_register(uint32_t hash, uint32_t size_bytes, void (*entry)(Vu1State* vu)) {
    if (g_nregs < 16) {
        g_regs[g_nregs].hash = hash;
        g_regs[g_nregs].size = size_bytes;
        g_regs[g_nregs].fn = entry;
        g_nregs++;
    }
}
void rt_vu1_register_all(void);

static int g_kicks = 0;
static int g_kick_bad = 0;
void rt_xgkick(Vu1State* vu, uint32_t qw_addr) {
    (void)vu;
    g_kicks++;
    if (qw_addr >= 1024u) g_kick_bad++;
}

void rt_unimplemented(const char* what, uint32_t vram) {
    printf("UNIMPL %s %08x\n", what, vram);
    fflush(stdout);
    exit(3);
}

static Vu1State g_vu;
static uint8_t g_buf[1 << 16];

int main(int argc, char** argv) {
    alarm(30); /* watchdog: a hung microprogram fails the test loudly */
    rt_vu1_register_all();
    printf("registered %d\n", g_nregs);
    int i;
    for (i = 1; i < argc; i++) {
        char* eq = strchr(argv[i], '=');
        if (!eq) return 2;
        *eq = 0;
        const char* name = argv[i];
        FILE* f = fopen(eq + 1, "rb");
        if (!f) return 2;
        size_t len = fread(g_buf, 1, sizeof g_buf, f);
        fclose(f);
        uint32_t h = rc_vu1_hash(g_buf, (uint32_t)len);
        Reg* r = 0;
        int j;
        for (j = 0; j < g_nregs; j++)
            if (g_regs[j].hash == h && g_regs[j].size == (uint32_t)len) r = &g_regs[j];
        if (!r) {
            printf("NOMATCH %s %08x %u\n", name, h, (unsigned)len);
            return 4;
        }
        printf("match %s %08x %u\n", name, h, (unsigned)len);
        static const uint32_t xtops[3] = {0u, 16u, 512u};
        int k;
        for (k = 0; k < 3; k++) {
            memset(&g_vu, 0, sizeof g_vu);
            g_vu.xtop = xtops[k];
            g_vu.pc = 0;
            g_kicks = 0;
            g_kick_bad = 0;
            r->fn(&g_vu);
            printf("ran %s xtop %u kicks %d bad %d pc %u\n", name, xtops[k], g_kicks,
                   g_kick_bad, g_vu.pc);
            if (g_kick_bad) return 5;
        }
    }
    return 0;
}
"#;

#[test]
fn emit_compile_and_replay_all_programs() {
    if !boot_elf_present() {
        eprintln!(
            "skipping: the boot ELF named by [inputs] in config/recomp.toml is not \
             present; run ./setup.sh <your disc image> to extract it"
        );
        return;
    }

    let root = repo_root();
    let db = ProgramDb::load(&config_path()).expect("ProgramDb::load");
    let image = ingest::load_elf_image(&config_path()).expect("load_elf_image");
    let programs = vu_emit::extract_programs(&db, &image).expect("extract_programs");
    assert_eq!(programs.len(), 5);

    let dir = std::env::temp_dir().join(format!("icorecomp-vu1-smoke-{}", std::process::id()));
    let out = dir.join("out");
    std::fs::create_dir_all(&out).unwrap();

    let report = vu_emit::emit_all(&db, &image, &out).expect("emit_all");

    // Coverage assertion: every uploaded bundle emitted exactly once.
    for p in &report.programs {
        assert_eq!(
            p.bundles_emitted as u32, p.instructions,
            "{}: bundle coverage mismatch",
            p.name
        );
        assert_eq!(p.bundles_duplicated, 0, "{}: unexpected duplicated bundles", p.name);
        assert!(p.entries.contains(&0), "{}: offset 0 must be an entry", p.name);
    }

    // The uploaded images (ROM-derived; temp files only, never committed)
    // and the driver.
    let mut args = Vec::new();
    for p in &programs {
        let path = dir.join(format!("{}.img", p.name));
        std::fs::write(&path, &p.image).unwrap();
        args.push(format!("{}={}", p.name, path.display()));
    }
    let driver_c = dir.join("driver.c");
    std::fs::write(&driver_c, DRIVER).unwrap();

    let exe = dir.join("driver");
    let mut gcc = Command::new("gcc");
    gcc.args([
        "-std=c11",
        "-O1",
        "-fno-strict-aliasing",
        "-ffp-contract=off",
        "-Wall",
        "-Werror",
    ])
    .arg("-I")
    .arg(root.join("include"));
    for p in &programs {
        gcc.arg(out.join(format!("vu1_{}.c", p.name)));
    }
    gcc.arg(out.join("vu1_table.c"))
        .arg(&driver_c)
        .arg("-o")
        .arg(&exe)
        .arg("-lm");
    let status = gcc.status().expect("running gcc");
    assert!(status.success(), "generated VU1 C failed -Wall -Werror compile");

    let mut child = Command::new(&exe)
        .args(&args)
        .stdout(Stdio::piped())
        .spawn()
        .expect("spawning driver");
    let deadline = Instant::now() + Duration::from_secs(60);
    let status = loop {
        match child.try_wait().expect("try_wait") {
            Some(s) => break s,
            None if Instant::now() > deadline => {
                let _ = child.kill();
                panic!("driver timed out: a microprogram is likely stuck in a loop");
            }
            None => std::thread::sleep(Duration::from_millis(100)),
        }
    };
    let mut stdout = String::new();
    use std::io::Read as _;
    child.stdout.take().unwrap().read_to_string(&mut stdout).unwrap();
    eprintln!("driver output:\n{stdout}");
    assert!(status.success(), "driver exited with {status}: \n{stdout}");

    // Hash parity: the driver hashed the image files with the C
    // rc_vu1_hash and matched the registration constants that came from
    // the Rust mirror. Assert the printed values line by line too.
    assert!(stdout.contains("registered 5"));
    for p in &programs {
        let want = format!(
            "match {} {:08x} {}",
            p.name,
            vu_emit::upload_hash(&p.image),
            p.image.len()
        );
        assert!(
            stdout.contains(&want),
            "missing or mismatched hash line: wanted {want:?}"
        );
        // Three xtop runs each, none hitting rt_unimplemented or a bad
        // XGKICK address.
        let runs = stdout
            .lines()
            .filter(|l| l.starts_with(&format!("ran {} ", p.name)))
            .count();
        assert_eq!(runs, 3, "{}: expected 3 zero-state runs", p.name);
    }
    assert!(!stdout.contains("UNIMPL"), "a run tripped rt_unimplemented");
    assert!(!stdout.contains("NOMATCH"));

    let _ = std::fs::remove_dir_all(&dir);
}
