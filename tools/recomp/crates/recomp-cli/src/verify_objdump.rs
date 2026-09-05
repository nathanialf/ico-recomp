//! `verify-decode` for a disc target: diff our R5900 disassembly against the
//! GNU objdump listing the disc carries (`SRCFILE.TXT`).
//!
//! This works even though the listing is of a different link from the
//! shipped ELF, because every line carries the instruction word it is
//! describing. We decode that word at the address the line gives and compare
//! against the text on the same line. Nothing is read from the ELF here, so
//! the build mismatch does not enter into it, and the check is a genuinely
//! independent one: the primary `verify-decode` diffs against spimdisasm's
//! output, this diffs against binutils'.
//!
//! Normalizations, and why each is a spelling difference rather than a
//! disagreement:
//!
//! 1. Layout. objdump separates the mnemonic from the operands with a tab
//!    and the operands with bare commas; we use a space and ", ".
//! 2. Register names. objdump prints ABI names (`$v0`, `$sp`, `$fp`); we
//!    print numbers (`$2`, `$29`, `$30`). The mapping is fixed by the ABI.
//! 3. Immediate radix and signedness. objdump prints `addiu $v0,$v0,2304`
//!    in signed decimal and `lui $v0,0x64` in hex; we print hex throughout.
//!    Compared as values modulo 2^16, which is what the encoded field holds.
//! 4. Branch and jump targets. objdump prints the absolute address in bare
//!    hex followed by a `<symbol+0xoffset>` comment; we print `.LXXXXXXXX`.
//!    Compared as values, and the comment is dropped.
//! 5. VU and COP register spelling. objdump prints VU float registers with
//!    no `$` (`vf3`, `vf3z`), VU integer registers as bare numbers
//!    (`cfc2.ni $v0,$22`), and COP0 registers as bare numbers (`$12`); we
//!    print `$vf3`, `$vf3z`, `$vi22` and `$12`. Compared as register
//!    numbers and, for a broadcast field, as the same component letter.
//! 6. The COP2 interlock suffix. We spell the interlock bit as a `.i` /
//!    `.ni` suffix on the mnemonic; binutils has no spelling for it at all
//!    and prints the bare `qmfc2`. The suffix is dropped before comparing,
//!    which means this check cannot verify that one bit against objdump.
//!    Nothing else can either: the information is absent from the listing.
//! 7. `syscall` and `break` code fields. objdump prints one combined hex
//!    code and omits it entirely when it is zero; we print the two encoded
//!    subfields in decimal. Compared as the 20-bit code the word holds.
//! 8. `mult`, `multu`, `div` and friends. objdump prints the two-operand
//!    form when the destination register field is `$zero`; we always print
//!    the destination. Accepted only when our destination really is `$0`.
//! 9. Data words inside `.text`. Neither disassembler can decode them:
//!    objdump prints the bare value, we print `.word`. Agreement, not a
//!    disagreement, when the bare value is the word.
//! 10. The VU destination-field suffix. Both disassemblers spell the four
//!     field bits as letters, in different orders: binutils writes them
//!     `w`-first (`vmulw.wxyz`), spimdisasm, whose conventions our formatter
//!     follows, writes them in component order (`vmulw.xyzw`). The letters
//!     are sorted into component order on both sides before the mnemonics
//!     are compared, so the set of bits is compared and the spelling is not.
//! 11. Pseudo-instructions. objdump prints `nop`, `move`, `b`, `beqz`,
//!     `bnez`, `li`, `bal`, `neg`, `negu` and `not` for encodings that have
//!     no such opcode. Each is checked against the *encoding*, not against
//!     our text: `move $a0,$s0` is accepted only when our decode is an
//!     add/or-family instruction whose destination is $4, one source is $16
//!     and the other is $0. A wrong decode still fails, because the register
//!     numbers being checked come from objdump's text, not from ours.
//!
//! A line whose objdump text uses a form this file does not model is
//! counted and reported separately from a mismatch. Calling an unmodelled
//! spelling a decoder disagreement would be as wrong as hiding one, so that
//! bucket exists; but it is opt-in, not a default. Only a spelling listed
//! in `UNMODELLED_FORMS`, with the reason it cannot be compared against an
//! encoding, lands there. Everything else is a mismatch, including two
//! zero-operand mnemonics that disagree, which used to be filed as
//! unmodelled and could therefore hide a decoder regression behind an
//! exit code of 0.

use std::collections::HashMap;
use std::path::Path;
use std::time::Instant;

use anyhow::Result;

use ingest::{Objdump, RecompConfig};
use r5900_decode::{decode, Insn, Kind, Operand};

struct Diff {
    vram: u32,
    word: u32,
    ours: String,
    theirs: String,
    note: String,
    group: String,
}

pub fn run(config_path: &Path, max_diffs: usize) -> Result<bool> {
    let start = Instant::now();
    let cfg = RecompConfig::load(config_path)?;
    let disc = &cfg.disc;
    let listing = Objdump::load(&disc.objdump_path)?;

    let mut total = 0usize;
    let mut unmodelled: HashMap<String, usize> = HashMap::new();
    let mut diffs: Vec<Diff> = Vec::new();

    for f in listing.section_functions(".text") {
        for insn in &f.insns {
            let Some(text) = &insn.text else { continue };
            total += 1;
            let ours = decode(insn.word, insn.vram);
            match compare(&ours, text) {
                Verdict::Agree => {}
                Verdict::Unmodelled(form) => *unmodelled.entry(form).or_default() += 1,
                Verdict::Differs(note) => diffs.push(Diff {
                    vram: insn.vram,
                    word: insn.word,
                    ours: ours.to_string(),
                    theirs: text.replace('\t', " "),
                    note,
                    group: their_mnemonic(text).to_string(),
                }),
            }
        }
    }

    diffs.sort_by_key(|d| d.vram);
    let unmodelled_total: usize = unmodelled.values().sum();
    println!(
        "verify-decode: {} instructions verified against {}, {} in objdump forms this \
         check does not model, {:.2}s",
        total,
        disc.objdump_path.display(),
        unmodelled_total,
        start.elapsed().as_secs_f64()
    );
    if unmodelled_total > 0 {
        let mut forms: Vec<(&String, &usize)> = unmodelled.iter().collect();
        forms.sort_by(|a, b| b.1.cmp(a.1).then(a.0.cmp(b.0)));
        println!("unmodelled objdump forms:");
        for (form, n) in forms.iter().take(40) {
            println!("  {n:8}  {form}");
        }
    } else {
        println!("unmodelled objdump forms: none");
    }
    if diffs.is_empty() {
        println!("mismatches: 0");
        return Ok(true);
    }

    let mut by_mnem: HashMap<&str, usize> = HashMap::new();
    for d in &diffs {
        *by_mnem.entry(d.group.as_str()).or_default() += 1;
    }
    let mut groups: Vec<(&str, usize)> = by_mnem.into_iter().collect();
    groups.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(b.0)));
    println!("mismatches: {}", diffs.len());
    println!("by mnemonic:");
    for (m, n) in &groups {
        println!("  {n:8}  {m}");
    }
    println!("first {} diffs:", max_diffs.min(diffs.len()));
    for d in diffs.iter().take(max_diffs) {
        println!(
            "  {:08X} word={:08X}\n    ours:   {}\n    theirs: {}\n    note:   {}",
            d.vram, d.word, d.ours, d.theirs, d.note
        );
    }
    Ok(false)
}

/// objdump spellings this check knowingly does not model, each paired with
/// the reason it cannot be checked against the encoded word. A form listed
/// here is counted, printed in the histogram, and does not fail the run.
/// Anything not listed is a mismatch. The list is empty: every objdump form
/// this listing contains is either modelled or an equivalence above.
const UNMODELLED_FORMS: &[(&str, &str)] = &[];

enum Verdict {
    Agree,
    /// objdump used a spelling this check does not model. Reported, never
    /// silently accepted and never counted as a disagreement.
    Unmodelled(String),
    Differs(String),
}

/// Drop the COP2 interlock suffix, which binutils never prints, and sort a
/// trailing VU destination-field suffix into component order, so binutils'
/// `w`-first spelling and our component-order spelling compare equal. Any
/// other suffix, and any mnemonic without one, is returned unchanged.
///
/// Applied to both sides, so the two spellings meet in the middle rather
/// than one being rewritten into the other.
fn canonical_mnemonic(mnemonic: &str) -> String {
    let mnemonic = mnemonic
        .strip_suffix(".ni")
        .or_else(|| mnemonic.strip_suffix(".i"))
        .unwrap_or(mnemonic);
    let Some((base, suffix)) = mnemonic.rsplit_once('.') else {
        return mnemonic.to_string();
    };
    if suffix.is_empty() || suffix.len() > 4 {
        return mnemonic.to_string();
    }
    let mut bits = [false; 4];
    for ch in suffix.chars() {
        let Some(i) = "xyzw".find(ch) else {
            return mnemonic.to_string();
        };
        if bits[i] {
            return mnemonic.to_string();
        }
        bits[i] = true;
    }
    let ordered: String = "xyzw"
        .chars()
        .zip(bits)
        .filter_map(|(c, on)| on.then_some(c))
        .collect();
    format!("{base}.{ordered}")
}

fn their_mnemonic(text: &str) -> &str {
    text.split(['\t', ' '])
        .next()
        .unwrap_or("")
}

fn compare(ours: &Insn, theirs: &str) -> Verdict {
    let (their_mnem, their_ops) = split_objdump(theirs);
    let Kind::Op { mnemonic, operands } = &ours.kind else {
        // A data word inside .text. objdump prints the bare value; we print
        // `.word`. Both are saying the same thing: this is not an
        // instruction.
        if their_ops.is_empty() {
            if let Ok(v) = u32::from_str_radix(their_mnem.trim_start_matches("0x"), 16) {
                if v == ours.word {
                    return Verdict::Agree;
                }
            }
        }
        return Verdict::Differs("our decoder rejects this word".into());
    };

    if let Some(verdict) = compare_pseudo(ours.word, mnemonic, operands, their_mnem, &their_ops) {
        return verdict;
    }

    // The COP2 interlock bit has no spelling in binutils' output, so a
    // bare `qmfc2` may be our `qmfc2.i` or our `qmfc2.ni`.
    if canonical_mnemonic(mnemonic) != canonical_mnemonic(their_mnem) {
        return Verdict::Differs(format!("mnemonic `{their_mnem}` vs `{mnemonic}`"));
    }

    if matches!(their_mnem, "syscall" | "break") {
        return compare_trap_code(operands, &their_ops);
    }

    // objdump's two-operand `mult $a0,$v1`: the destination field is $zero
    // and it does not print it.
    let operands: &[Operand] = if operands.len() == their_ops.len() + 1
        && matches!(operands.first(), Some(Operand::Gpr(0)))
    {
        &operands[1..]
    } else {
        operands
    };

    if operands.len() != their_ops.len() {
        return Verdict::Differs(format!(
            "operand count differs ({} vs {})",
            operands.len(),
            their_ops.len()
        ));
    }
    for (o, t) in operands.iter().zip(&their_ops) {
        if let Err(why) = same_operand(o, t) {
            return Verdict::Differs(format!("operand `{t}` vs `{o}`: {why}"));
        }
    }
    Verdict::Agree
}

/// `syscall` and `break` carry a 20-bit code. We print it as the two
/// encoded subfields in decimal; objdump prints one hex value, or nothing
/// when the code is zero. Both are compared as the code itself.
fn compare_trap_code(ours: &[Operand], theirs: &[&str]) -> Verdict {
    let mut our_code: u64 = 0;
    for op in ours {
        match op {
            Operand::Dec(v) => our_code = (our_code << 10) | u64::from(*v),
            other => {
                return Verdict::Differs(format!("unexpected trap operand `{other}`"));
            }
        }
    }
    let their_code = match theirs.first() {
        None => 0,
        Some(t) => match parse_int(t) {
            Some(v) => v as u64,
            None => return Verdict::Differs(format!("unparsed trap code `{t}`")),
        },
    };
    if our_code == their_code {
        Verdict::Agree
    } else {
        Verdict::Differs(format!("trap code {their_code:#x} != {our_code:#x}"))
    }
}

/// Split `lui\t$v0,0x64` into ("lui", ["$v0", "0x64"]), dropping the
/// `<symbol+0x..>` comment objdump appends to branch and jump targets.
fn split_objdump(text: &str) -> (&str, Vec<&str>) {
    let text = match text.find(" <") {
        Some(p) => &text[..p],
        None => text,
    };
    let text = text.trim();
    let (mnem, rest) = match text.find(['\t', ' ']) {
        Some(p) => (&text[..p], text[p..].trim()),
        None => (text, ""),
    };
    if rest.is_empty() {
        return (mnem, Vec::new());
    }
    (mnem, rest.split(',').map(str::trim).collect())
}

/// Value-level operand comparison. Registers must be the same register;
/// immediates and targets must be the same value, whatever radix each side
/// chose to print it in.
fn same_operand(ours: &Operand, theirs: &str) -> Result<(), String> {
    match *ours {
        Operand::Gpr(n) => match gpr(theirs) {
            Some(t) if t == n => Ok(()),
            Some(t) => Err(format!("register ${t} != ${n}")),
            None => Err("not a general purpose register".into()),
        },
        Operand::Fpr(n) => match theirs.strip_prefix("$f").and_then(|s| s.parse::<u8>().ok()) {
            Some(t) if t == n => Ok(()),
            _ => Err(format!("not $f{n}")),
        },
        // objdump prints a branch or jump target as bare hex with no
        // prefix, so it must be read as hex even when every character
        // happens to be a decimal digit (`100018` is 0x100018).
        Operand::Target(addr) => match u32::from_str_radix(theirs.trim(), 16) {
            Ok(v) if v == addr => Ok(()),
            Ok(v) => Err(format!("target 0x{v:X} != 0x{addr:X}")),
            Err(_) => Err("unparsed target".into()),
        },
        Operand::Imm(v) => cmp16(theirs, v as i64),
        Operand::UImm(v) => cmp16(theirs, v as i64),
        Operand::Dec(v) => match parse_int(theirs) {
            Some(t) if t == v as i64 => Ok(()),
            Some(t) => Err(format!("{t} != {v}")),
            None => Err("unparsed immediate".into()),
        },
        Operand::Hex2(v) => match parse_int(theirs) {
            Some(t) if t == v as i64 => Ok(()),
            Some(t) => Err(format!("0x{t:X} != 0x{v:02X}")),
            None => Err("unparsed immediate".into()),
        },
        Operand::Cop0(n) => match theirs.trim_start_matches('$').parse::<u8>() {
            Ok(t) if t == n => Ok(()),
            _ => Err(format!("not COP0 register ${n}")),
        },
        Operand::Vf(n) => match vf_operand(theirs) {
            Some((t, None)) if t == n => Ok(()),
            _ => Err(format!("not $vf{n}")),
        },
        Operand::VfComp(n, c) => match vf_operand(theirs) {
            Some((t, Some(tc))) if t == n && tc == c => Ok(()),
            _ => Err(format!("not $vf{n}{c}")),
        },
        Operand::Vi(n) => match vi_operand(theirs) {
            Some(t) if t == n => Ok(()),
            _ => Err(format!("not $vi{n}")),
        },
        Operand::ViInc(n) | Operand::ViDec(n) | Operand::ViInd(n) => {
            let inner = theirs
                .trim()
                .trim_start_matches('(')
                .trim_end_matches(')')
                .trim_start_matches("--")
                .trim_end_matches("++");
            match vi_operand(inner) {
                Some(t) if t == n => Ok(()),
                _ => Err(format!("not an addressing form on $vi{n}")),
            }
        }
        Operand::Acc | Operand::Q | Operand::R | Operand::I => {
            let want = ours.to_string();
            if theirs.trim().trim_start_matches('$').eq_ignore_ascii_case(&want) {
                Ok(())
            } else {
                Err(format!("not {want}"))
            }
        }
        Operand::Mem { offset, base } => {
            let (off, b) = split_mem(theirs).ok_or("bad memory operand")?;
            let breg = gpr(b).ok_or("bad base register")?;
            if breg != base {
                return Err(format!("base ${breg} != ${base}"));
            }
            cmp16(off, offset as i64)
        }
        _ => Err("no value-level comparison for this operand kind".into()),
    }
}

/// Both sides encode the same 16-bit field; only the display signedness and
/// radix differ, so agreement modulo 2^16 is exact agreement on the field.
fn cmp16(theirs: &str, ours: i64) -> Result<(), String> {
    match parse_int(theirs) {
        Some(v) if (v as u64) & 0xFFFF == (ours as u64) & 0xFFFF => Ok(()),
        Some(v) => Err(format!("0x{:X} != 0x{:X} (mod 2^16)", v, ours)),
        None => Err("unparsed immediate".into()),
    }
}

/// `vf3`, `$vf3`, `vf3z`, `$vf3z` -> (3, None) / (3, Some('z')).
fn vf_operand(s: &str) -> Option<(u8, Option<char>)> {
    let s = s.trim().trim_start_matches('$').strip_prefix("vf")?;
    let digits: String = s.chars().take_while(|c| c.is_ascii_digit()).collect();
    if digits.is_empty() {
        return None;
    }
    let index = digits.parse::<u8>().ok()?;
    let rest = &s[digits.len()..];
    match rest.len() {
        0 => Some((index, None)),
        1 => Some((index, rest.chars().next())),
        _ => None,
    }
}

/// `$vi13`, `vi13`, or the bare `$22` objdump prints for a COP2 control
/// register.
fn vi_operand(s: &str) -> Option<u8> {
    let s = s.trim().trim_start_matches('$');
    let s = s.strip_prefix("vi").unwrap_or(s);
    s.parse::<u8>().ok().filter(|&n| n < 32)
}

fn split_mem(s: &str) -> Option<(&str, &str)> {
    let open = s.rfind('(')?;
    let close = s.rfind(')')?;
    (close > open).then(|| (&s[..open], &s[open + 1..close]))
}

fn parse_int(s: &str) -> Option<i64> {
    let s = s.trim();
    let (neg, s) = match s.strip_prefix('-') {
        Some(r) => (true, r),
        None => (false, s),
    };
    let v = if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        i64::from_str_radix(hex, 16).ok()?
    } else if !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit()) {
        s.parse::<i64>().ok()?
    } else {
        // A bare branch target is printed as hex with no prefix.
        i64::from_str_radix(s, 16).ok()?
    };
    Some(if neg { -v } else { v })
}

/// ABI or numeric register name to its index.
fn gpr(name: &str) -> Option<u8> {
    let n = name.strip_prefix('$')?;
    if let Ok(v) = n.parse::<u8>() {
        return (v < 32).then_some(v);
    }
    Some(match n {
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

/// Handle the encodings objdump prints as a pseudo-instruction. Returns
/// `None` when objdump's mnemonic is not one of them, so the caller falls
/// through to the ordinary comparison.
///
/// Every rule below is expressed in terms of the registers objdump printed
/// and the fields of the encoded word, and then requires our own decode to
/// agree. None of them accepts an instruction on the strength of our own
/// output alone.
fn compare_pseudo(
    word: u32,
    our_mnem: &str,
    our_ops: &[Operand],
    their_mnem: &str,
    their_ops: &[&str],
) -> Option<Verdict> {
    // When both disassemblers reach for the same pseudo (spimdisasm, whose
    // conventions our formatter follows, prints `nop`, `beqz` and `bnez`
    // too), there is nothing to reconcile: fall through to the ordinary
    // operand comparison, which is stricter than any rule below.
    if our_mnem == their_mnem {
        return None;
    }

    let rs = ((word >> 21) & 0x1F) as u8;
    let rt = ((word >> 16) & 0x1F) as u8;
    let rd = ((word >> 11) & 0x1F) as u8;

    let differs = |why: String| Some(Verdict::Differs(why));
    let agree_if = |cond: bool, why: &str| {
        if cond {
            Some(Verdict::Agree)
        } else {
            Some(Verdict::Differs(format!("objdump pseudo `{their_mnem}`: {why}")))
        }
    };

    match their_mnem {
        // A word printed as `nop` that our decoder calls something else.
        "nop" => agree_if(word == 0, "word is not zero"),
        // `move rd,rs`: an add/or-family instruction with a zero source.
        "move" | "dmove" => {
            if their_ops.len() != 2 {
                return differs("move takes two operands".into());
            }
            let (Some(d), Some(s)) = (gpr(their_ops[0]), gpr(their_ops[1])) else {
                return differs("move operands are not registers".into());
            };
            let family = matches!(our_mnem, "addu" | "daddu" | "or" | "add" | "dadd");
            let zero_source = (rs == 0 && rt == s) || (rt == 0 && rs == s);
            agree_if(
                family && rd == d && zero_source,
                "not an add/or-family instruction from that register with a zero source",
            )
        }
        // `b target`: beq $zero,$zero. `bal target`: bgezal $zero.
        "b" => agree_if(
            our_mnem == "beq" && rs == 0 && rt == 0,
            "not beq $zero, $zero",
        ),
        "bal" => agree_if(
            our_mnem == "bgezal" && rs == 0,
            "not bgezal $zero",
        ),
        // `beqz rs,target` / `bnez rs,target`: beq/bne against $zero.
        "beqz" | "bnez" | "beqzl" | "bnezl" => {
            let Some(r) = their_ops.first().and_then(|s| gpr(s)) else {
                return differs("first operand is not a register".into());
            };
            let base = match their_mnem {
                "beqz" => "beq",
                "bnez" => "bne",
                "beqzl" => "beql",
                _ => "bnel",
            };
            agree_if(
                our_mnem == base && rt == 0 && rs == r,
                "not the zero-compare form of that branch",
            )
        }
        // `li rt,imm`: addiu or ori from $zero.
        "li" => {
            let Some(r) = their_ops.first().and_then(|s| gpr(s)) else {
                return differs("first operand is not a register".into());
            };
            agree_if(
                matches!(our_mnem, "addiu" | "ori" | "addi" | "daddiu") && rs == 0 && rt == r,
                "not an immediate from $zero",
            )
        }
        // `neg`/`negu`: sub/subu from $zero. `not`: nor with $zero.
        "neg" | "negu" | "dneg" | "dnegu" => {
            let Some(d) = their_ops.first().and_then(|s| gpr(s)) else {
                return differs("first operand is not a register".into());
            };
            agree_if(
                matches!(our_mnem, "sub" | "subu" | "dsub" | "dsubu") && rs == 0 && rd == d,
                "not a subtract from $zero",
            )
        }
        "not" => {
            let Some(d) = their_ops.first().and_then(|s| gpr(s)) else {
                return differs("first operand is not a register".into());
            };
            agree_if(
                our_mnem == "nor" && rd == d && (rs == 0 || rt == 0),
                "not a nor with $zero",
            )
        }
        _ => {
            // objdump printed something with no operands and no matching
            // mnemonic of ours. That is a decoder disagreement unless this
            // file says in `UNMODELLED_FORMS` why that spelling cannot be
            // checked against the encoding; returning `None` here sends it
            // to the mnemonic comparison, which reports the mismatch.
            if our_ops.is_empty() && their_ops.is_empty() && our_mnem != their_mnem {
                if let Some((_, why)) = UNMODELLED_FORMS.iter().find(|(m, _)| *m == their_mnem) {
                    return Some(Verdict::Unmodelled(format!(
                        "{their_mnem} (ours: {our_mnem}): {why}"
                    )));
                }
            }
            None
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn verdict(word: u32, vram: u32, theirs: &str) -> &'static str {
        match compare(&decode(word, vram), theirs) {
            Verdict::Agree => "agree",
            Verdict::Unmodelled(_) => "unmodelled",
            Verdict::Differs(_) => "differs",
        }
    }

    #[test]
    fn a_zero_operand_mnemonic_disagreement_is_a_mismatch() {
        // objdump's `sync` against our `eret`: two zero-operand mnemonics
        // that disagree. This used to be filed as an unmodelled form, which
        // let a decoder regression pass with an exit code of 0.
        assert_eq!(verdict(0x4200_0018, 0x100010, "sync"), "differs");
    }

    #[test]
    fn abi_names_and_decimal_immediates_agree() {
        // addiu $v0,$v0,2304 == addiu $2, $2, 0x900
        assert_eq!(verdict(0x2442_0900, 0x100010, "addiu\t$v0,$v0,2304"), "agree");
        // a lui, whose immediate objdump prints in hex like ours
        assert_eq!(verdict(0x3C03_0074, 0x10000c, "lui\t$v1,0x74"), "agree");
        // sq $zero,0($v0)
        assert_eq!(verdict(0x7C40_0000, 0x100018, "sq\t$zero,0($v0)"), "agree");
        // A negative immediate printed in decimal.
        assert_eq!(
            verdict(0x2463_A998, 0x100014, "addiu\t$v1,$v1,-22120"),
            "agree"
        );
    }

    #[test]
    fn branch_targets_and_their_symbol_comments_agree() {
        // bnez $at,100018 <_start+0x10>  ==  bne $1, $0, .L00100018
        assert_eq!(
            verdict(0x1420_FFFA, 0x10002c, "bnez\t$at,100018 <_start+0x10>"),
            "agree"
        );
    }

    #[test]
    fn pseudo_instructions_are_checked_against_the_encoding() {
        // move $a0,$s0 is daddu $4, $16, $0.
        assert_eq!(verdict(0x0200_202D, 0x28db1c, "move\t$a0,$s0"), "agree");
        // The same text against a different destination register must fail.
        assert_eq!(verdict(0x0200_282D, 0x28db1c, "move\t$a0,$s0"), "differs");
        assert_eq!(verdict(0x0000_0000, 0x100000, "nop"), "agree");
        assert_eq!(verdict(0x2402_0001, 0x100000, "li\t$v0,1"), "agree");
    }

    #[test]
    fn destination_field_suffixes_compare_as_sets_not_spellings() {
        assert_eq!(canonical_mnemonic("vmulw.wxyz"), "vmulw.xyzw");
        assert_eq!(canonical_mnemonic("vmulw.xyzw"), "vmulw.xyzw");
        assert_eq!(canonical_mnemonic("vsub.wz"), "vsub.zw");
        // The interlock suffix is dropped on both sides, so a bare
        // `qmfc2` and our `qmfc2.ni` meet.
        assert_eq!(canonical_mnemonic("qmfc2.ni"), "qmfc2");
        assert_eq!(canonical_mnemonic("qmfc2"), "qmfc2");
        assert_eq!(canonical_mnemonic("addiu"), "addiu");
    }

    #[test]
    fn a_real_disagreement_is_reported() {
        // Claim an lui is an addiu.
        assert_eq!(verdict(0x3C03_0074, 0x10000c, "addiu\t$v1,$v1,0x74"), "differs");
    }
}
