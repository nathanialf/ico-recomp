//! `verify-decode`: diff our R5900 disassembly against the decomp repo's
//! splat/spimdisasm asm baselines.
//!
//! Baseline lines look like:
//!   `    /* 97A38 00197A38 E0FFBD27 */  addiu      $29, $29, -0x20`
//! (rom offset, vram, raw word in memory byte order, then text). Some
//! instructions old GNU as cannot assemble appear as
//!   `.word 0x46000064 # cvt.w.s $f1, $f0 # ...`
//! and a handful of handwritten data words in .text appear as
//!   `.word 0x00FF00FF /* invalid instruction */`.
//!
//! For each instruction we decode the raw word, format it with our
//! canonical formatter, and compare against the baseline text after
//! normalization (register aliases, relocation expressions, labels).
//! Nothing from the decomp repo is copied anywhere; it is only read.

use std::collections::HashMap;
use std::fmt::Write as _;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Instant;

use anyhow::{bail, Context, Result};
use rayon::prelude::*;
use serde::Deserialize;

use r5900_decode::{decode, Insn, Kind, Operand};

// ------------------------------------------------------------------ config

#[derive(Deserialize)]
struct Config {
    decomp: Decomp,
    #[serde(default)]
    target: Target,
}

#[derive(Deserialize)]
struct Decomp {
    root: String,
    #[serde(default = "default_asm_dir")]
    asm_dir: String,
    symbol_addrs: Option<String>,
}

fn default_asm_dir() -> String {
    "asm".into()
}

#[derive(Deserialize, Default)]
struct Target {
    gp: Option<u32>,
}

// ------------------------------------------------------------------ driver

pub fn run(config_path: &Path, max_diffs: usize) -> Result<bool> {
    let start = Instant::now();
    let raw = fs::read_to_string(config_path)
        .with_context(|| format!("reading {}", config_path.display()))?;
    let cfg: Config = toml::from_str(&raw).context("parsing recomp.toml")?;

    // decomp.root is relative to the repository root (the parent of the
    // config file's directory).
    let repo_root = config_path
        .canonicalize()
        .with_context(|| format!("resolving {}", config_path.display()))?
        .parent()
        .and_then(Path::parent)
        .map(Path::to_path_buf)
        .context("config path has no parent")?;
    let decomp_root = repo_root.join(&cfg.decomp.root);
    let asm_dir = decomp_root.join(&cfg.decomp.asm_dir);
    if !asm_dir.is_dir() {
        bail!("asm dir not found: {}", asm_dir.display());
    }
    let gp = cfg.target.gp.unwrap_or(0) as i64;

    let mut files = Vec::new();
    collect_s_files(&asm_dir, &mut files)?;
    files.sort();

    // Pass 1: symbol table (glabel/dlabel scan plus symbol_addrs).
    let mut symtab: HashMap<String, u32> = HashMap::new();
    if let Some(rel) = &cfg.decomp.symbol_addrs {
        let p = decomp_root.join(rel);
        if let Ok(text) = fs::read_to_string(&p) {
            parse_symbol_addrs(&text, &mut symtab);
        }
    }
    let per_file: Vec<Vec<(String, u32)>> = files
        .par_iter()
        .map(|p| scan_labels(p))
        .collect();
    for v in per_file {
        for (name, vram) in v {
            symtab.entry(name).or_insert(vram);
        }
    }

    // Pass 2: verify.
    let ctx = EvalCtx { symtab: &symtab, gp };
    let reports: Vec<FileReport> = files
        .par_iter()
        .map(|p| verify_file(p, &ctx))
        .collect();

    let mut total = 0usize;
    let mut compat = 0usize;
    let mut skipped = 0usize;
    let mut diffs: Vec<Diff> = Vec::new();
    for r in reports {
        total += r.total;
        compat += r.compat;
        skipped += r.skipped_words;
        diffs.extend(r.diffs);
    }
    diffs.sort_by_key(|d| d.vram);

    println!(
        "verify-decode: {} files, {} instructions verified ({} via .word compat), {} data words skipped, {:.2}s",
        files.len(),
        total,
        compat,
        skipped,
        start.elapsed().as_secs_f64()
    );
    if diffs.is_empty() {
        println!("mismatches: 0");
        return Ok(true);
    }

    let mut by_mnem: HashMap<String, usize> = HashMap::new();
    for d in &diffs {
        *by_mnem.entry(d.group.clone()).or_default() += 1;
    }
    let mut groups: Vec<(String, usize)> = by_mnem.into_iter().collect();
    groups.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));

    println!("mismatches: {}", diffs.len());
    println!("by mnemonic:");
    for (m, n) in &groups {
        println!("  {n:8}  {m}");
    }
    println!("first {} diffs:", max_diffs.min(diffs.len()));
    for d in diffs.iter().take(max_diffs) {
        println!(
            "  {}:{:08X} word={:08X}\n    ours:   {}\n    theirs: {}\n    note:   {}",
            d.file.display(),
            d.vram,
            d.word,
            d.ours,
            d.theirs,
            d.note
        );
    }
    Ok(false)
}

fn collect_s_files(dir: &Path, out: &mut Vec<PathBuf>) -> Result<()> {
    for entry in fs::read_dir(dir).with_context(|| format!("reading {}", dir.display()))? {
        let entry = entry?;
        let path = entry.path();
        if path.is_dir() {
            collect_s_files(&path, out)?;
        } else if path.extension().is_some_and(|e| e == "s") {
            out.push(path);
        }
    }
    Ok(())
}

// ------------------------------------------------------------- symbol scan

fn parse_symbol_addrs(text: &str, out: &mut HashMap<String, u32>) {
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with("//") {
            continue;
        }
        let Some((name, rest)) = line.split_once('=') else {
            continue;
        };
        let name = name.trim();
        let rest = rest.trim();
        let value = rest
            .split(|c: char| c == ';' || c.is_whitespace())
            .next()
            .unwrap_or("");
        if let Some(hex) = value.strip_prefix("0x") {
            if let Ok(v) = u32::from_str_radix(hex, 16) {
                out.insert(name.to_string(), v);
            }
        }
    }
}

/// Collect `glabel`/`dlabel` names with the vram of the next data/insn line.
fn scan_labels(path: &Path) -> Vec<(String, u32)> {
    let Ok(bytes) = fs::read(path) else {
        return Vec::new();
    };
    let text = String::from_utf8_lossy(&bytes);
    let mut out = Vec::new();
    let mut pending: Option<String> = None;
    for line in text.lines() {
        let t = line.trim_start();
        if let Some(rest) = t.strip_prefix("glabel ").or_else(|| t.strip_prefix("dlabel ")) {
            let name = rest.split_whitespace().next().unwrap_or("");
            if !name.is_empty() {
                pending = Some(name.to_string());
            }
            continue;
        }
        if pending.is_some() {
            if let Some(fields) = comment_fields(t) {
                if let Some(vram) = fields.get(1).and_then(|s| u32::from_str_radix(s, 16).ok()) {
                    out.push((pending.take().unwrap(), vram));
                }
            }
        }
    }
    out
}

/// If the line starts with `/* ... */`, return the whitespace-separated
/// fields inside the comment.
fn comment_fields(t: &str) -> Option<Vec<&str>> {
    let rest = t.strip_prefix("/*")?;
    let end = rest.find("*/")?;
    Some(rest[..end].split_whitespace().collect())
}

// ------------------------------------------------------------------ verify

struct EvalCtx<'a> {
    symtab: &'a HashMap<String, u32>,
    gp: i64,
}

struct Diff {
    file: PathBuf,
    vram: u32,
    word: u32,
    ours: String,
    theirs: String,
    note: String,
    group: String,
}

#[derive(Default)]
struct FileReport {
    total: usize,
    compat: usize,
    skipped_words: usize,
    diffs: Vec<Diff>,
}

enum LineKind<'a> {
    /// Ordinary instruction line: vram, word, baseline text.
    Insn(u32, u32, &'a str),
    /// `.word` with an instruction comment (old-as compat).
    Compat(u32, u32, &'a str),
    /// `.word` explicitly marked invalid.
    ExpectInvalid(u32, u32),
    /// Anything else.
    Other,
}

fn classify_line(t: &str) -> LineKind<'_> {
    let Some(stripped) = t.strip_prefix("/*") else {
        return LineKind::Other;
    };
    let Some(end) = stripped.find("*/") else {
        return LineKind::Other;
    };
    let comment = &stripped[..end];
    let rest = stripped[end + 2..].trim();
    let mut fields = comment.split_whitespace();
    let (Some(_off), Some(vram_s), Some(word_s), None) =
        (fields.next(), fields.next(), fields.next(), fields.next())
    else {
        return LineKind::Other;
    };
    let (Ok(vram), Ok(raw)) = (
        u32::from_str_radix(vram_s, 16),
        u32::from_str_radix(word_s, 16),
    ) else {
        return LineKind::Other;
    };
    // The raw word is displayed in memory byte order (little endian).
    let word = raw.swap_bytes();
    if rest.is_empty() {
        return LineKind::Other;
    }
    if let Some(after) = rest.strip_prefix(".word") {
        let after = after.trim_start();
        if rest.contains("/* invalid instruction */") {
            return LineKind::ExpectInvalid(vram, word);
        }
        if let Some(hash) = after.find('#') {
            let expected = after[hash + 1..].trim();
            let expected = match expected.find('#') {
                Some(h2) => expected[..h2].trim(),
                None => expected,
            };
            if expected.starts_with("INVALID") {
                return LineKind::ExpectInvalid(vram, word);
            }
            return LineKind::Compat(vram, word, expected);
        }
        return LineKind::Other; // plain data word
    }
    if rest.starts_with('.') {
        return LineKind::Other; // directive
    }
    // Strip trailing block comments ("/* handwritten instruction */" etc).
    let text = match rest.find("/*") {
        Some(p) => rest[..p].trim_end(),
        None => rest,
    };
    LineKind::Insn(vram, word, text)
}

fn verify_file(path: &Path, ctx: &EvalCtx) -> FileReport {
    let mut report = FileReport::default();
    let Ok(bytes) = fs::read(path) else {
        return report;
    };
    let text = String::from_utf8_lossy(&bytes);
    let mut in_text = true;
    for line in text.lines() {
        let t = line.trim_start();
        if let Some(rest) = t.strip_prefix(".section") {
            in_text = rest.contains(".text");
            continue;
        }
        if !in_text {
            continue;
        }
        match classify_line(t) {
            LineKind::Other => {
                if t.starts_with("/*") && t.contains(".word") && comment_fields(t).is_some() {
                    report.skipped_words += 1;
                }
            }
            LineKind::ExpectInvalid(vram, word) => {
                report.total += 1;
                report.compat += 1;
                let insn = decode(word, vram);
                if insn.is_valid() {
                    report.diffs.push(Diff {
                        file: path.to_path_buf(),
                        vram,
                        word,
                        ours: insn.to_string(),
                        theirs: ".word (invalid instruction)".into(),
                        note: "baseline marks this word invalid, we decoded it".into(),
                        group: "<invalid>".into(),
                    });
                }
            }
            LineKind::Compat(vram, word, expected) => {
                report.total += 1;
                report.compat += 1;
                let insn = decode(word, vram);
                if let Some(d) = check_insn(&insn, expected, ctx, path, vram, word) {
                    report.diffs.push(d);
                }
            }
            LineKind::Insn(vram, word, text) => {
                report.total += 1;
                let insn = decode(word, vram);
                if let Some(d) = check_insn(&insn, text, ctx, path, vram, word) {
                    report.diffs.push(d);
                }
            }
        }
    }
    report
}

// -------------------------------------------------------------- comparison

fn check_insn(
    insn: &Insn,
    baseline: &str,
    ctx: &EvalCtx,
    path: &Path,
    vram: u32,
    word: u32,
) -> Option<Diff> {
    let norm = normalize(baseline);
    let mut parts = norm.splitn(2, ' ');
    let their_mnem = parts.next().unwrap_or("");
    let their_ops: Vec<&str> = parts
        .next()
        .map(|s| s.split(',').map(str::trim).collect())
        .unwrap_or_default();

    let fail = |note: String| {
        Some(Diff {
            file: path.to_path_buf(),
            vram,
            word,
            ours: insn.to_string(),
            theirs: baseline.trim().to_string(),
            note,
            group: their_mnem.to_string(),
        })
    };

    let Kind::Op { mnemonic, operands } = &insn.kind else {
        return fail("our decoder rejects this word".into());
    };
    if mnemonic != their_mnem {
        return fail("mnemonic differs".into());
    }
    if operands.len() != their_ops.len() {
        return fail(format!(
            "operand count differs ({} vs {})",
            operands.len(),
            their_ops.len()
        ));
    }
    for (ours, theirs) in operands.iter().zip(&their_ops) {
        let ours_str = ours.to_string();
        if ours_str == *theirs {
            continue;
        }
        match compare_operand(ours, theirs, ctx) {
            Ok(()) => {}
            Err(why) => {
                return fail(format!("operand `{theirs}` vs `{ours_str}`: {why}"));
            }
        }
    }
    None
}

fn compare_operand(ours: &Operand, theirs: &str, ctx: &EvalCtx) -> Result<(), String> {
    match *ours {
        Operand::Target(addr) => {
            let v = eval_expr(theirs, ctx).ok_or("unresolved target")?;
            if v == addr as i64 {
                Ok(())
            } else {
                Err(format!("target 0x{v:X} != 0x{addr:X}"))
            }
        }
        Operand::Imm(val) => cmp16(theirs, val as i64, ctx),
        Operand::UImm(val) => cmp16(theirs, val as i64, ctx),
        Operand::Mem { offset, base } => {
            let (off_s, base_s) = split_mem(theirs).ok_or("bad memory operand")?;
            let breg = parse_gpr(base_s).ok_or("bad base register")?;
            if breg != base {
                return Err(format!("base ${breg} != ${base}"));
            }
            cmp16(off_s, offset as i64, ctx)
        }
        _ => Err("no value-level comparison for this operand kind".into()),
    }
}

/// Compare a 16-bit encoded field: expression value and ours must agree
/// modulo 2^16 (both derive from the same field; signedness of display
/// differs between reloc expressions and raw values).
fn cmp16(theirs: &str, ours: i64, ctx: &EvalCtx) -> Result<(), String> {
    let v = eval_expr(theirs, ctx).ok_or("unresolved expression")?;
    if (v as u64) & 0xFFFF == (ours as u64) & 0xFFFF {
        Ok(())
    } else {
        Err(format!("value 0x{:X} != 0x{:X} (mod 2^16)", v, ours))
    }
}

fn split_mem(s: &str) -> Option<(&str, &str)> {
    let open = s.rfind('(')?;
    let close = s.rfind(')')?;
    if close < open {
        return None;
    }
    Some((&s[..open], &s[open + 1..close]))
}

fn parse_gpr(s: &str) -> Option<u8> {
    let s = s.strip_prefix('$')?;
    s.parse::<u8>().ok().filter(|&n| n < 32)
}

// ---------------------------------------------------------- normalization

/// Map an ABI register name to its numeric index.
fn named_gpr(name: &str) -> Option<u8> {
    Some(match name {
        "zero" => 0,
        "at" => 1,
        "v0" => 2,
        "v1" => 3,
        "a0" => 4,
        "a1" => 5,
        "a2" => 6,
        "a3" => 7,
        "t0" => 8,
        "t1" => 9,
        "t2" => 10,
        "t3" => 11,
        "t4" => 12,
        "t5" => 13,
        "t6" => 14,
        "t7" => 15,
        "s0" => 16,
        "s1" => 17,
        "s2" => 18,
        "s3" => 19,
        "s4" => 20,
        "s5" => 21,
        "s6" => 22,
        "s7" => 23,
        "t8" => 24,
        "t9" => 25,
        "k0" => 26,
        "k1" => 27,
        "gp" => 28,
        "sp" => 29,
        "fp" | "s8" => 30,
        "ra" => 31,
        _ => return None,
    })
}

/// Normalize baseline text: collapse whitespace, rewrite `$abi` register
/// names to numeric form, and strip the `$` some rabbitizer versions put
/// on Q/R/I/ACC/FpcCsr.
fn normalize(text: &str) -> String {
    let collapsed: Vec<&str> = text.split_whitespace().collect();
    let joined = collapsed.join(" ");
    let mut out = String::with_capacity(joined.len());
    let bytes = joined.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'$' {
            let start = i + 1;
            let mut j = start;
            while j < bytes.len() && (bytes[j] as char).is_ascii_alphanumeric() {
                j += 1;
            }
            let name = &joined[start..j];
            if let Some(n) = named_gpr(name) {
                let _ = write!(out, "${n}");
            } else {
                match name {
                    "Q" | "R" | "I" | "ACC" => out.push_str(name),
                    "FpcCsr" => out.push_str("$31"),
                    _ => {
                        out.push('$');
                        out.push_str(name);
                    }
                }
            }
            i = j;
        } else {
            out.push(bytes[i] as char);
            i += 1;
        }
    }
    out
}

// ------------------------------------------------------------- expressions

/// Evaluate a baseline operand expression to an integer:
/// `%hi(sym)`, `%lo(sym + 0x4)`, `%gp_rel(sym)`, `(0x1234 >> 16)`,
/// `(0x12345678 & 0xFFFF)`, plain numbers, symbols, `.L`/`func_`/`D_`/
/// `jtbl_` pattern labels.
fn eval_expr(s: &str, ctx: &EvalCtx) -> Option<i64> {
    let s = s.trim();
    if let Some(inner) = strip_call(s, "%hi(") {
        let v = eval_sym_expr(inner, ctx)?;
        return Some((v.wrapping_add(0x8000) >> 16) & 0xFFFF);
    }
    if let Some(inner) = strip_call(s, "%lo(") {
        let v = eval_sym_expr(inner, ctx)?;
        return Some(v & 0xFFFF);
    }
    if let Some(inner) = strip_call(s, "%gp_rel(") {
        let v = eval_sym_expr(inner, ctx)?;
        return Some((v - ctx.gp) & 0xFFFF);
    }
    if let Some(inner) = s.strip_prefix('(').and_then(|r| r.strip_suffix(')')) {
        for (tok, f) in [
            (" >> ", (|a, b| a >> b) as fn(i64, i64) -> i64),
            (" << ", |a, b| a << b),
            (" & ", |a, b| a & b),
            (" | ", |a, b| a | b),
        ] {
            if let Some((l, r)) = inner.split_once(tok) {
                let a = eval_expr(l, ctx)?;
                let b = eval_expr(r, ctx)?;
                return Some(f(a, b));
            }
        }
        return eval_sym_expr(inner, ctx);
    }
    if let Some(v) = parse_int(s) {
        return Some(v);
    }
    eval_sym_expr(s, ctx)
}

fn strip_call<'a>(s: &'a str, prefix: &str) -> Option<&'a str> {
    s.strip_prefix(prefix)?.strip_suffix(')')
}

/// `NAME`, `NAME + 0x4`, `NAME - 0x4`, or a plain number.
fn eval_sym_expr(s: &str, ctx: &EvalCtx) -> Option<i64> {
    let s = s.trim();
    if let Some((l, r)) = s.split_once(" + ") {
        return Some(eval_sym_expr(l, ctx)? + parse_int(r.trim())?);
    }
    if let Some((l, r)) = s.split_once(" - ") {
        return Some(eval_sym_expr(l, ctx)? - parse_int(r.trim())?);
    }
    if let Some(v) = parse_int(s) {
        return Some(v);
    }
    resolve_symbol(s, ctx)
}

fn parse_int(s: &str) -> Option<i64> {
    let (neg, s) = match s.strip_prefix('-') {
        Some(r) => (true, r),
        None => (false, s),
    };
    let v = if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        i64::from_str_radix(hex, 16).ok()?
    } else if s.chars().all(|c| c.is_ascii_digit()) && !s.is_empty() {
        s.parse::<i64>().ok()?
    } else {
        return None;
    };
    Some(if neg { -v } else { v })
}

fn resolve_symbol(name: &str, ctx: &EvalCtx) -> Option<i64> {
    if let Some(&v) = ctx.symtab.get(name) {
        return Some(v as i64);
    }
    if let Some(hex) = name.strip_prefix(".L") {
        return i64::from_str_radix(hex, 16).ok();
    }
    for prefix in ["D_", "func_", "jtbl_"] {
        if let Some(hex) = name.strip_prefix(prefix) {
            return i64::from_str_radix(hex, 16).ok();
        }
    }
    None
}
