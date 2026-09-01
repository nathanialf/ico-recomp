//! Differential gate for the VU1 static recompiler.
//!
//! Runs each of the five retail microprograms twice from the same seeded
//! `Vu1State` -- once through the Rust reference interpreter, once through
//! the emitter's generated C compiled into a shared object -- and requires
//! the two to agree byte for byte on the whole state, plus on the XGKICK
//! addresses issued and their order.
//!
//! Both sides call the same `rc_vu1_*` helpers out of recomp_ops.h, so a
//! mismatch is never a float-semantics difference. It is a scheduling
//! difference, and the two schedulers are deliberately independent:
//! vu-emit decides hazards statically in analyze.rs (`old_vi_sites`,
//! `q_commit_sites`), while vu-interp models the pipelines dynamically.
//! That is what gives the comparison its teeth.
//!
//! Requires the sibling ../ico checkout; skips cleanly without it, like
//! the other gated tests. Unix only: the generated C is loaded with dlopen,
//! so the whole file is gated rather than the one test.

#![cfg(unix)]

use std::ffi::c_void;
use std::path::{Path, PathBuf};
use std::process::Command;

use ingest::{ProgramDb, RecompConfig};
use vu_interp::{reset_effects, run, take_effects, VuState};

fn repo_root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../..")
}

fn config_path() -> PathBuf {
    repo_root().join("config/recomp.toml")
}

fn decomp_present() -> bool {
    RecompConfig::load(&config_path())
        .map(|c| c.decomp_root.is_dir())
        .unwrap_or(false)
}

/// A shared object exposing every generated program by upload hash.
const DRIVER: &str = r#"
#include <string.h>
#include <stddef.h>
#include "recomp_ops.h"

typedef struct { uint32_t hash; uint32_t size; void (*fn)(Vu1State*); } Reg;
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

/* The .so records its own side effects rather than relying on symbol
 * interposition from the test binary, which does not export its symbols.
 * The shapes match the vu-interp shim so the two are directly comparable. */
#define D_MAX_KICKS 4096
static uint32_t g_kicks[D_MAX_KICKS];
static uint32_t g_kick_count;
static uint32_t g_unimplemented;

void rt_xgkick(Vu1State* vu, uint32_t qw_addr) {
    (void)vu;
    if (g_kick_count < D_MAX_KICKS) g_kicks[g_kick_count] = qw_addr;
    ++g_kick_count;
}
void rt_unimplemented(const char* what, uint32_t vram) {
    (void)what; (void)vram;
    ++g_unimplemented;
}

#define D_MAX_TRACE 200000
static uint32_t g_trace[D_MAX_TRACE];
static uint32_t g_hash[D_MAX_TRACE];
static uint32_t g_trace_count;

/* FNV-1a over everything except the 16KB data memory: registers, the Q
 * pipeline fields, and the flags. A wrong value reaches one of these
 * before it reaches memory. */
static uint32_t d_reghash(const Vu1State* vu) {
    const unsigned char* p = (const unsigned char*)vu;
    size_t n = offsetof(Vu1State, mem), i;
    uint32_t h = 2166136261u;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

void rc_vu_trace(uint32_t off, const Vu1State* vu) {
    if (g_trace_count < D_MAX_TRACE) {
        g_trace[g_trace_count] = off;
        g_hash[g_trace_count] = d_reghash(vu);
    }
    ++g_trace_count;
}
uint32_t d_hash_at(uint32_t i) { return i < D_MAX_TRACE ? g_hash[i] : 0u; }
uint32_t d_trace_count(void) { return g_trace_count; }
uint32_t d_trace_at(uint32_t i) { return i < D_MAX_TRACE ? g_trace[i] : 0xFFFFFFFFu; }

void d_reset(void) {
    g_kick_count = 0;
    g_trace_count = 0;
    g_unimplemented = 0;
    memset(g_kicks, 0, sizeof g_kicks);
}
uint32_t d_kick_count(void) { return g_kick_count; }
uint32_t d_kick_at(uint32_t i) { return i < D_MAX_KICKS ? g_kicks[i] : 0u; }
uint32_t d_unimplemented(void) { return g_unimplemented; }

static int g_inited = 0;
/* Runs the program whose upload hash is `hash` on the caller's state.
 * Returns 0 on success, 1 if no program has that hash. */
int d_run(uint32_t hash, void* state) {
    if (!g_inited) { rt_vu1_register_all(); g_inited = 1; }
    int j;
    for (j = 0; j < g_nregs; j++) {
        if (g_regs[j].hash == hash) {
            g_regs[j].fn((Vu1State*)state);
            return 0;
        }
    }
    return 1;
}
"#;

// ---- minimal dlopen, same approach as ee-interp/tests/threeway.rs -------

#[cfg(unix)]
mod dso {
    use std::ffi::CString;
    use std::os::raw::{c_char, c_int, c_void};
    extern "C" {
        fn dlopen(filename: *const c_char, flag: c_int) -> *mut c_void;
        fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    }
    pub fn open(path: &str) -> *mut c_void {
        let c = CString::new(path).unwrap();
        let h = unsafe { dlopen(c.as_ptr(), 2 /* RTLD_NOW */) };
        assert!(!h.is_null(), "dlopen failed for {path}");
        h
    }
    pub fn sym(h: *mut c_void, name: &str) -> *mut c_void {
        let c = CString::new(name).unwrap();
        let p = unsafe { dlsym(h, c.as_ptr()) };
        assert!(!p.is_null(), "dlsym failed for {name}");
        p
    }
}

type DRun = unsafe extern "C" fn(u32, *mut u8) -> i32;
type DVoid = unsafe extern "C" fn();
type DU32 = unsafe extern "C" fn() -> u32;
type DAt = unsafe extern "C" fn(u32) -> u32;

/// The .so's recorded effects, in the same shape as vu_interp::Effects.
struct Native {
    reset: DVoid,
    count: DU32,
    at: DAt,
    unimpl: DU32,
    trace_count: DU32,
    trace_at: DAt,
    hash_at: DAt,
}

impl Native {
    /// The generated code's executed bundle offsets, in order.
    fn trace(&self) -> Vec<u32> {
        let n = unsafe { (self.trace_count)() };
        (0..n.min(200_000)).map(|i| unsafe { (self.trace_at)(i) }).collect()
    }
    /// Register-file hash observed on entry to each traced bundle.
    fn hashes(&self) -> Vec<u32> {
        let n = unsafe { (self.trace_count)() };
        (0..n.min(200_000)).map(|i| unsafe { (self.hash_at)(i) }).collect()
    }
}

impl Native {
    fn take(&self) -> vu_interp::Effects {
        unsafe {
            let n = (self.count)();
            vu_interp::Effects {
                kicks: (0..n.min(4096)).map(|i| (self.at)(i)).collect(),
                unimplemented: (self.unimpl)(),
            }
        }
    }
}

fn build_so(out_dir: &Path) -> PathBuf {
    let driver = out_dir.join("driver.c");
    std::fs::write(&driver, DRIVER).unwrap();

    let mut sources: Vec<PathBuf> = std::fs::read_dir(out_dir)
        .unwrap()
        .filter_map(|e| {
            let p = e.unwrap().path();
            (p.extension().and_then(|s| s.to_str()) == Some("c")).then_some(p)
        })
        .collect();
    sources.sort();

    let so = out_dir.join("libvu1diff.so");
    let mut cmd = Command::new("gcc");
    cmd.args([
        "-std=c11",
        "-O1",
        "-shared",
        "-fPIC",
        "-fno-strict-aliasing",
        "-ffp-contract=off",
        "-Wall",
        "-Werror",
        "-DRC_VU_TRACE_ENABLE",
    ])
    .arg("-I")
    .arg(repo_root().join("include"))
    .args(&sources)
    .arg("-o")
    .arg(&so)
    .arg("-lm");
    let out = cmd.output().expect("gcc");
    assert!(
        out.status.success(),
        "gcc failed:\n{}",
        String::from_utf8_lossy(&out.stderr)
    );
    so
}

/// Deterministic seed material. The smoke test runs from an all-zero
/// state, which exercises almost no data-dependent control flow; these
/// patterns give the clip cascades and the LOD tiers something to decide
/// about, without needing a captured frame.
fn seed(st: &mut VuState, salt: u32) {
    let mut x = 0x9E37_79B9u32 ^ salt.wrapping_mul(0x85EB_CA6B);
    let mut next = move || {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        x
    };
    // Data memory: quadwords that look like vertex/matrix rows, with
    // exponents kept in a sane range so the FMACs do not all saturate.
    let mem = st.mem_mut();
    for q in 0..1024usize {
        for lane in 0..4usize {
            let mant = next() & 0x007F_FFFF;
            let exp = 0x3F80_0000u32 + ((next() % 12).wrapping_sub(6) << 23);
            let sign = (next() & 1) << 31;
            let bits = sign | (exp & 0x7F80_0000) | mant;
            let off = q * 16 + lane * 4;
            mem[off..off + 4].copy_from_slice(&bits.to_le_bytes());
        }
    }
    // vf registers, leaving vf00 at its (0,0,0,1) invariant.
    for r in 1..32usize {
        for lane in 0..4usize {
            let mant = next() & 0x007F_FFFF;
            let exp = 0x3F80_0000u32 + ((next() % 8).wrapping_sub(4) << 23);
            st.set_vf_lane(r, lane, (exp & 0x7F80_0000) | mant);
        }
    }
    // vi registers, kept small so loads stay inside data memory.
    for r in 1..16usize {
        st.set_vi(r, (next() % 256) as u16);
    }
}

/// The prologue reads a quadword count out of `mem[xtop].x` and masked to
/// 0x7FFF; random bits there make every program spin ~30,000 copy
/// iterations and stop before doing anything interesting. Plant a small
/// count at every plausible base so the copy path is short and the
/// geometry paths get reached.
fn plant_counts(st: &mut VuState, count: u32) {
    let mem = st.mem_mut();
    for base_qw in [0usize, 4, 8, 16, 512, 516, 520] {
        let off = base_qw * 16;
        if off + 4 <= mem.len() {
            mem[off..off + 4].copy_from_slice(&count.to_le_bytes());
        }
    }
}

/// Captured `(hash, Vu1State)` records from a real run, written by the
/// runtime under ICORECOMP_VU1_CAPTURE. These are the seeds synthetic
/// generation cannot produce: VU1 programs are stateful across MSCAL, so
/// the deep geometry paths are only reachable from a state that a previous
/// call and a VIF1 unpack built up.
fn load_captures(path: &Path, state_size: usize) -> Vec<(u32, Vec<u8>)> {
    let data = std::fs::read(path).expect("reading VU1 capture");
    assert!(data.len() >= 16, "capture too short");
    let magic = u32::from_le_bytes(data[0..4].try_into().unwrap());
    assert_eq!(magic, 0x4331_5556, "not a VU1 capture file");
    let size = u32::from_le_bytes(data[8..12].try_into().unwrap()) as usize;
    assert_eq!(
        size, state_size,
        "capture was written by a build with a different Vu1State layout"
    );
    let rec = 4 + state_size;
    let mut out = Vec::new();
    let mut off = 16;
    while off + rec <= data.len() {
        let hash = u32::from_le_bytes(data[off..off + 4].try_into().unwrap());
        out.push((hash, data[off + 4..off + rec].to_vec()));
        off += rec;
    }
    out
}

#[test]
fn interpreter_and_generated_code_agree_on_all_programs() {
    if !decomp_present() {
        eprintln!("differential: ../ico not present, skipping");
        return;
    }
    let db = ProgramDb::load(&config_path()).unwrap();
    let image = ingest::load_elf_image(&config_path()).unwrap();
    let programs = vu_emit::extract_programs(&db, &image).unwrap();

    // Generated C is ROM-derived: keep it outside the repository so
    // tools/check_no_rom.sh can stay a mechanical gate.
    let out_dir = std::env::temp_dir().join(format!("vu1-diff-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&out_dir);
    vu_emit::emit_all(&db, &image, &out_dir).unwrap();
    let so_path = build_so(&out_dir);

    let handle = dso::open(so_path.to_str().unwrap());
    let d_run = unsafe { std::mem::transmute::<*mut c_void, DRun>(dso::sym(handle, "d_run")) };
    let native = unsafe {
        Native {
            reset: std::mem::transmute::<*mut c_void, DVoid>(dso::sym(handle, "d_reset")),
            count: std::mem::transmute::<*mut c_void, DU32>(dso::sym(handle, "d_kick_count")),
            at: std::mem::transmute::<*mut c_void, DAt>(dso::sym(handle, "d_kick_at")),
            unimpl: std::mem::transmute::<*mut c_void, DU32>(
                dso::sym(handle, "d_unimplemented"),
            ),
            trace_count: std::mem::transmute::<*mut c_void, DU32>(
                dso::sym(handle, "d_trace_count"),
            ),
            trace_at: std::mem::transmute::<*mut c_void, DAt>(dso::sym(handle, "d_trace_at")),
            hash_at: std::mem::transmute::<*mut c_void, DAt>(dso::sym(handle, "d_hash_at")),
        }
    };

    let budget: u64 = 2_000_000;
    // Widen the seed sweep for exploration: VU_SALTS=200 cargo test ...
    let salts: u32 = std::env::var("VU_SALTS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(3);
    let mut failures: Vec<String> = Vec::new();
    let mut compared = 0usize;
    let mut total_bundles = 0u64;
    let mut total_kicks = 0u64;
    let mut reached: std::collections::BTreeMap<String, std::collections::BTreeSet<usize>> =
        Default::default();

    let by_hash: std::collections::HashMap<u32, (String, Vec<vu_decode::Bundle>)> = programs
        .iter()
        .map(|p| {
            (
                p.hash(),
                (p.name.clone(), vu_decode::decode_program(&p.image).unwrap()),
            )
        })
        .collect();

    for prog in &programs {
        let bundles = vu_decode::decode_program(&prog.image).unwrap();
        let hash = prog.hash();
        // Every MSCAL entry the analyzer found, not just offset 0: entry 0
        // is a branch stub into the DMA copy path, and the geometry work
        // hangs off the other entries. The runtime log confirms the game
        // calls 0x0, 0x10, 0x80, 0x90, 0xc0, 0x100 and 0x130.
        let entries = vu_emit::analyze(prog).unwrap().entries;
        if std::env::var("VU_DUMP").is_ok() {
            println!("  {} entries: {:x?}", prog.name, entries);
        }

        for &entry in &entries {
        for (si, &(xtop, itop)) in [(0u32, 0u32), (16, 4), (512, 12)].iter().enumerate() {
            for salt in 0..salts {
                let mut base = VuState::new();
                seed(&mut base, salt.wrapping_mul(31).wrapping_add(si as u32));
                plant_counts(&mut base, 4 + salt);
                base.set_xtop(xtop);
                base.set_itop(itop);
                base.set_pc(entry);

                let mut a = base.clone();
                let mut b = base.clone();

                reset_effects();
                let outcome = match run(&bundles, &mut a, budget) {
                    Ok(o) => o,
                    Err(e) => {
                        failures.push(format!(
                            "{} entry={entry:#x} xtop={xtop} salt={salt}: \
                             interpreter error: {e}",
                            prog.name
                        ));
                        continue;
                    }
                };
                let eff_interp = take_effects();

                unsafe { (native.reset)() };
                let rc = unsafe { d_run(hash, b.as_mut_ptr()) };
                assert_eq!(rc, 0, "{}: no generated program with hash {hash:#010x}", prog.name);
                let eff_native = native.take();

                compared += 1;
                total_bundles += outcome.bundles_executed;
                total_kicks += eff_interp.kicks.len() as u64;
                for (i, &c) in outcome.coverage.iter().enumerate() {
                    if c > 0 {
                        reached.entry(prog.name.clone()).or_default().insert(i);
                    }
                }
                if salt == 0 && si == 0 {
                    println!(
                        "  {:9} stop {:?} after {} bundles, {} kicks",
                        prog.name, outcome.stop, outcome.bundles_executed,
                        eff_interp.kicks.len()
                    );
                }

                if a.bytes() != b.bytes() {
                    let diffs: Vec<String> = a
                        .bytes()
                        .iter()
                        .zip(b.bytes())
                        .enumerate()
                        .filter(|(_, (x, y))| x != y)
                        .take(12)
                        .map(|(i, (x, y))| format!("[{i:#x}] interp {x:02X} native {y:02X}"))
                        .collect();
                    let n = a
                        .bytes()
                        .iter()
                        .zip(b.bytes())
                        .filter(|(x, y)| x != y)
                        .count();
                    failures.push(format!(
                        "{} entry={entry:#x} xtop={xtop} salt={salt}: state differs in {n} bytes \
                         (interp stop {:?} after {} bundles)\n      {}",
                        prog.name,
                        outcome.stop,
                        outcome.bundles_executed,
                        diffs.join("\n      ")
                    ));
                }
                if eff_interp != eff_native {
                    failures.push(format!(
                        "{} entry={entry:#x} xtop={xtop} salt={salt}: side effects differ; \
                         interp {} kicks, native {} kicks (first divergence at {:?})",
                        prog.name,
                        eff_interp.kicks.len(),
                        eff_native.kicks.len(),
                        eff_interp
                            .kicks
                            .iter()
                            .zip(&eff_native.kicks)
                            .position(|(x, y)| x != y)
                    ));
                }
            }
        }
        }
    }

    // Replay captured states, when a run has produced some. This is the
    // part that reaches the geometry paths; synthetic seeding saturates
    // well short of them.
    if let Ok(cap) = std::env::var("ICORECOMP_VU1_CAPTURE") {
        let probe = VuState::new();
        let caps = load_captures(Path::new(&cap), probe.size());
        println!("replaying {} captured states from {cap}", caps.len());
        for (hash, blob) in &caps {
            let mut a = VuState::new();
            a.load(blob);
            let mut b = VuState::new();
            b.load(blob);

            reset_effects();
            let outcome = match run(&by_hash[hash].1, &mut a, budget) {
                Ok(o) => o,
                Err(e) => {
                    failures.push(format!("capture hash={hash:#010x}: interpreter error: {e}"));
                    continue;
                }
            };
            let eff_interp = take_effects();
            for (i, &c) in outcome.coverage.iter().enumerate() {
                if c > 0 {
                    reached.entry(by_hash[hash].0.clone()).or_default().insert(i);
                }
            }
            unsafe { (native.reset)() };
            let rc = unsafe { d_run(*hash, b.as_mut_ptr()) };
            assert_eq!(rc, 0, "capture references unknown hash {hash:#010x}");
            let eff_native = native.take();
            let tr_native = native.trace();
            let hs_native = native.hashes();
            compared += 1;
            total_bundles += outcome.bundles_executed;
            if a.bytes() != b.bytes() || eff_interp != eff_native {
                // Where the two schedulers first took different paths. The
                // end state says they disagree; this says where.
                let split = outcome
                    .trace
                    .iter()
                    .zip(&tr_native)
                    .position(|(x, y)| x != y);
                let bundles = &by_hash[hash].1;
                #[allow(unused)]
                let disasm = |off: u32| -> String {
                    match bundles.iter().find(|b| b.offset == off) {
                        Some(b) => format!(
                            "{:#06x}  {:38}  {}",
                            b.offset,
                            vu_decode::compat::upper_compat(b)
                                .unwrap_or_else(|| format!(".word {:#010x}", b.upper_raw)),
                            match b.lower {
                                vu_decode::LowerSlot::Loi(v) => format!("loi {v:#010x}"),
                                _ => vu_decode::compat::lower_compat(b)
                                    .unwrap_or_else(|| format!(".word {:#010x}", b.lower_raw)),
                            }
                        ),
                        None => format!("{off:#06x}  <no bundle>"),
                    }
                };
                let where_ = match split {
                    Some(i) => {
                        let lo = i.saturating_sub(3);
                        let ctx: Vec<String> = (lo..i)
                            .map(|k| format!("{:#06x}", outcome.trace[k]))
                            .collect();
                        // The branch is the bundle before the delay slot the
                        // two sides last agreed on.
                        let listing: Vec<String> = (lo..i)
                            .map(|k| disasm(outcome.trace[k]))
                            .chain([
                                format!("interp -> {}", disasm(outcome.trace[i])),
                                format!("native -> {}", disasm(tr_native[i])),
                            ])
                            .collect();
                        format!(
                            "paths split at step {i} (after {}): interp goes to {:#06x}, \
                             native to {:#06x}\n        {}",
                            ctx.join(" -> "),
                            outcome.trace[i],
                            tr_native[i],
                            listing.join("\n        ")
                        )
                    }
                    None => {
                        // Same path, different values. The first bundle whose
                        // entry hash differs is the bundle *after* the one
                        // that produced the wrong value.
                        match outcome
                            .hashes
                            .iter()
                            .zip(&hs_native)
                            .position(|(x, y)| x != y)
                        {
                            Some(i) => format!(
                                "paths identical for {} steps; first differing register \
                                 state on entry to step {i} ({:#06x}), so the value was \
                                 produced by the bundle before it:\n        {}\n        {}",
                                outcome.trace.len().min(tr_native.len()),
                                outcome.trace[i],
                                disasm(outcome.trace[i.saturating_sub(1)]),
                                disasm(outcome.trace[i])
                            ),
                            None => format!(
                                "paths and register states identical for {} steps; \
                                 the divergence is in data memory only",
                                outcome.trace.len().min(tr_native.len())
                            ),
                        }
                    }
                };
                let n = a.bytes().iter().zip(b.bytes()).filter(|(x, y)| x != y).count();
                // Name the differing fields rather than raw offsets, and
                // collapse each 4-byte lane to one line.
                let mut lanes: Vec<usize> = a
                    .bytes()
                    .iter()
                    .zip(b.bytes())
                    .enumerate()
                    .filter(|(_, (x, y))| x != y)
                    .map(|(i, _)| if a.is_vi_offset(i) { i & !1 } else { i & !3 })
                    .collect();
                lanes.dedup();
                let named: Vec<String> = lanes
                    .iter()
                    .take(12)
                    .map(|&off| {
                        if a.is_vi_offset(off) {
                            let ai = u16::from_le_bytes(a.bytes()[off..off + 2].try_into().unwrap());
                            let bi = u16::from_le_bytes(b.bytes()[off..off + 2].try_into().unwrap());
                            return format!(
                                "{} interp {ai:04X} native {bi:04X}",
                                a.field_name(off)
                            );
                        }
                        let ai = u32::from_le_bytes(a.bytes()[off..off + 4].try_into().unwrap());
                        let bi = u32::from_le_bytes(b.bytes()[off..off + 4].try_into().unwrap());
                        format!(
                            "{} interp {:08X} ({}) native {:08X} ({})",
                            a.field_name(off),
                            ai,
                            f32::from_bits(ai),
                            bi,
                            f32::from_bits(bi)
                        )
                    })
                    .collect();
                failures.push(format!(
                    "capture hash={hash:#010x} pc={:#x}: state differs in {n} bytes \
                     ({} lanes), kicks interp {} native {} (interp stop {:?} after {} bundles)\n      {}",
                    u32::from_le_bytes(blob[probe.pc_offset()..probe.pc_offset() + 4]
                        .try_into().unwrap()),
                    lanes.len(),
                    eff_interp.kicks.len(),
                    eff_native.kicks.len(),
                    outcome.stop,
                    outcome.bundles_executed,
                    named.join("\n      ")
                ));
                failures.last_mut().unwrap().push_str(&format!("\n      {where_}"));
            }
        }
    }

    let _ = std::fs::remove_dir_all(&out_dir);

    if !failures.is_empty() {
        // Categorise before truncating: how many failures change the set of
        // primitives actually drawn is the number that matters, not the raw
        // count of states that differ somewhere.
        let n_capture = failures.iter().filter(|f| f.starts_with("capture ")).count();
        let n_kicks = failures.iter().filter(|f| f.contains("kicks interp")
            || f.contains("side effects differ")).count();
        eprintln!(
            "failure breakdown: {} total, {n_capture} from real captured states, \
             {n_kicks} change the XGKICK set (drawn primitives)",
            failures.len()
        );
        let shown = std::env::var("VU_SHOW_FAILURES")
            .ok().and_then(|v| v.parse::<usize>().ok()).unwrap_or(10)
            .min(failures.len());
        panic!(
            "{} of {compared} differential comparisons failed; first {shown}:\n  {}",
            failures.len(),
            failures[..shown].join("\n  ")
        );
    }
    if std::env::var("VU_DUMP").is_ok() {
        for prog in &programs {
            let bundles = vu_decode::decode_program(&prog.image).unwrap();
            let Some(hit) = reached.get(&prog.name) else { continue };
            println!("--- {} reached bundles ---", prog.name);
            for &i in hit {
                let b = &bundles[i];
                println!(
                    "  {:#06x}  {:38}  {}",
                    b.offset,
                    vu_decode::compat::upper_compat(b)
                        .unwrap_or_else(|| format!(".word {:#010x}", b.upper_raw)),
                    match b.lower {
                        vu_decode::LowerSlot::Loi(v) => format!("loi {:#010x}", v),
                        _ => vu_decode::compat::lower_compat(b)
                            .unwrap_or_else(|| format!(".word {:#010x}", b.lower_raw)),
                    }
                );
            }
        }
    }
    // Frontier: conditional branches that were executed but whose target
    // was never reached. These name the gates the seeds cannot get past,
    // which is far more useful than a coverage percentage.
    println!("coverage frontier (executed branches whose target is unreached):");
    for prog in &programs {
        let bundles = vu_decode::decode_program(&prog.image).unwrap();
        let Some(hit) = reached.get(&prog.name) else { continue };
        let mut shown = 0;
        for &i in hit {
            let b = &bundles[i];
            let vu_decode::LowerSlot::Inst(op) = b.lower else { continue };
            let tgt = match op {
                vu_decode::lower::LowerOp::Ibeq { imm11, .. }
                | vu_decode::lower::LowerOp::Ibne { imm11, .. }
                | vu_decode::lower::LowerOp::Ibgtz { imm11, .. }
                | vu_decode::lower::LowerOp::Ibltz { imm11, .. }
                | vu_decode::lower::LowerOp::Iblez { imm11, .. }
                | vu_decode::lower::LowerOp::Ibgez { imm11, .. } => {
                    vu_decode::branch_target(b.offset, imm11)
                }
                _ => continue,
            };
            let ti = (tgt / 8) as usize;
            let fall = i + 2; // branch + delay slot
            let dis = vu_decode::compat::lower_compat(b)
                .unwrap_or_else(|| format!(".word {:#010x}", b.lower_raw));
            let mut note = None;
            if ti < bundles.len() && !hit.contains(&ti) {
                note = Some(format!("target {tgt:#06x} never taken"));
            } else if fall < bundles.len() && !hit.contains(&fall) {
                note = Some(format!(
                    "always taken, fall-through {:#06x} never reached",
                    fall as u32 * 8
                ));
            }
            if let Some(n) = note {
                println!("  {:9} {:#06x} {n}:  {dis}", prog.name, b.offset);
                shown += 1;
                if shown >= 8 {
                    break;
                }
            }
        }
    }
    println!("bundle coverage reached by these seeds:");
    for prog in &programs {
        let n = vu_decode::decode_program(&prog.image).unwrap().len();
        let hit = reached.get(&prog.name).map(|s| s.len()).unwrap_or(0);
        println!(
            "  {:9} {hit:4}/{n:4} bundles ({:.0}%)",
            prog.name,
            100.0 * hit as f64 / n as f64
        );
    }
    // normal_c's six-plane clip cascade, the code the geometry bug lives
    // near: bundle offsets 0x15d0..0x1660.
    if let Some(hit) = reached.get("normal_c") {
        let casc: Vec<usize> = (0x15d0usize / 8..=0x1660 / 8).collect();
        let n_hit = casc.iter().filter(|i| hit.contains(i)).count();
        println!("  normal_c clip cascade 0x15d0-0x1660: {n_hit}/{} bundles reached", casc.len());
    }
    assert!(compared > 0, "no comparisons ran");
    println!(
        "differential: {compared} comparisons across {} programs agree \
         ({total_bundles} bundles executed, {total_kicks} XGKICKs compared)",
        programs.len()
    );
    // A green run that executed almost nothing would prove nothing. Judge
    // that by distinct bundles reached, not by raw bundles executed: a
    // single hot copy loop can run for ages and cover almost no code.
    let worst = programs
        .iter()
        .map(|p| {
            let n = vu_decode::decode_program(&p.image).unwrap().len();
            let hit = reached.get(&p.name).map(|s| s.len()).unwrap_or(0);
            100 * hit / n.max(1)
        })
        .min()
        .unwrap_or(0);
    assert!(
        worst >= 20,
        "differential covers only {worst}% of the least-covered program; \
         the seeds are not exercising enough of the code to be a gate"
    );
}
