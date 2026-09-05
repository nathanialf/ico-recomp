//! Function-entry and jump-table discovery straight from the target ELF.
//!
//! The correlation in `correlate` names and bounds every function it can
//! carry over from the donor listing, but a donor function whose body
//! changed between the two links has no correlated address, and its retail
//! counterpart would otherwise be absorbed into the previous function. This
//! module supplies the entries the ELF itself proves exist. There are five
//! proofs, and `disc::load` uses all five:
//!
//! 1. `jal`: every `jal` target inside `.text` (measured on SCES_507.60:
//!    2931 distinct targets).
//! 2. pointer-after-return: every address materialized by a `lui`/`addiu` or
//!    `lui`/`ori` pair that lands inside `.text` immediately after a
//!    function end, which is how a function used only as a pointer is taken.
//! 3. pointer-with-prologue: every address materialized by such a pair whose
//!    register is never used as a load or store base afterwards and whose
//!    target words are a function prologue (`prologue_at`).
//! 4. data-pointer-with-prologue (`data_pointers_to_prologues`): every
//!    aligned word of an on-disk data section that holds the address of a
//!    prologue. This is the function only a table names.
//! 5. prologues-after-transfers (`prologues_after_transfers`): a prologue
//!    that begins right after an unconditional transfer and its delay slot
//!    and that no branch targets.
//!
//! The ELF entry point is an entry by definition, and a `j` target is a
//! candidate that `disc::load` tests further (`jump_targets` below).
//!
//! A pointer-formed address is accepted on one of two guards. The first is
//! positional: the two words before it are `jr $ra` and its delay slot, so
//! the address is a function boundary. It says the address begins something,
//! not that what it begins is a function: a constant pool laid down after a
//! function end satisfies it too, which is why the register the pair leaves
//! the address in must also never be used as a load or store base
//! (`used_as_memory_base`), the same exclusion the prologue guard carries.
//! The second guard reads the target: a stack frame is allocated there and a
//! register that must survive a call is saved into it, which is a prologue
//! and nothing else. Without a guard a `la` of a jump table or of a
//! mid-function label would split a function in half, which is worse than
//! missing an entry: the emitter would translate a fragment and call it a
//! function.
//!
//! Jump tables are recovered from the read-only data sections: a run of
//! words that all point inside one function's byte range is a switch table,
//! and nothing else in this binary's `.rodata` satisfies that.

use std::collections::{BTreeMap, BTreeSet};

/// `jr $ra`, the whole word (rs=31, all other fields zero).
const JR_RA: u32 = 0x03E0_0008;

/// One `.text` address a `lui`/`addiu`(`ori`) pair forms, and what the
/// scan can say about it.
#[derive(Debug, Clone)]
pub struct PointerSite {
    /// The address the pair forms.
    pub target: u32,
    /// The address of the low half (the `addiu` or `ori`), which is what a
    /// report names so the code taking the pointer can be found.
    pub site: u32,
    /// The register the pair leaves the address in.
    pub reg: u8,
    /// The register is the base of a load or store before it is written
    /// again: the pair formed a data base, not a function pointer.
    pub mem_base: bool,
    /// The words at `target` are a function prologue (`prologue_at`).
    pub prologue: bool,
    /// `target` sits just after a `jr $ra` and its delay slot.
    pub after_return: bool,
}

/// What a straight walk of `.text` proves about function entries.
#[derive(Debug, Clone, Default)]
pub struct TextScan {
    /// Targets of `jal`. Each is a function entry by definition.
    pub jal_targets: BTreeSet<u32>,
    /// Addresses formed by a `lui`/`addiu`(`ori`) pair that land in `.text`
    /// just after a function end. Candidate entries for functions only ever
    /// called through a pointer.
    pub pointer_targets: BTreeSet<u32>,
    /// Addresses formed by such a pair whose register is never a load or
    /// store base afterwards and whose target words are a function
    /// prologue. The other half of the pointer proof, for a function that
    /// does not start at a boundary the previous function's `jr $ra` marks:
    /// the PAL build's `iosThreadMain` coroutine entry at 0x001021A0 is
    /// preceded by a `b` and its delay slot, not by a return.
    pub prologue_targets: BTreeSet<u32>,
    /// Every `.text` address any `lui`/`addiu`(`ori`) pair forms, with what
    /// the scan could tell about it. The whole-`.text` sweep the `ee` run
    /// reports: an address here that no entry proof turned into a function
    /// is an indirect call the runtime cannot resolve.
    pub pointers: Vec<PointerSite>,
    /// Targets of an unconditional `j`. Most are long intra-function
    /// branches, but a tail call is also a `j`, and a function whose only
    /// caller tail-jumps to it has no `jal` anywhere (the matrix composer
    /// at 0x001146F0 is exactly this in the retail build). These are
    /// candidates and nothing more, and weaker candidates than the name
    /// suggests: a label placed right after a `b` or `j` and its delay slot
    /// is the ordinary shape of a rotated loop, so position alone does not
    /// separate a tail-call target from a branch label. The caller decides,
    /// and `disc::load` requires two things of one of these before it
    /// becomes an entry: the position (`follows_a_return`) and either a
    /// prologue at the target (`prologue_at`) or a correlated donor name.
    pub jump_targets: BTreeSet<u32>,
}

/// Walk `words` (the `.text` section, starting at `base`) and collect the
/// entries it proves.
pub fn scan_text(words: &[u32], base: u32) -> TextScan {
    let end = base + (words.len() as u32) * 4;
    let mut out = TextScan::default();

    // Words that end a function: `jr $ra` and the delay slot after it.
    let mut ends_after: BTreeSet<u32> = BTreeSet::new();
    for (i, &w) in words.iter().enumerate() {
        if w == JR_RA {
            // The delay slot is the next word; the function ends after it.
            ends_after.insert(base + (i as u32) * 4 + 8);
        }
    }

    // High halves in flight, per destination register, and the `.text`
    // addresses the pairs form. The two tests that classify a formed
    // address (is the register a memory base afterwards, is the target a
    // prologue) both need a second look at `words`, so the walk collects
    // and the loop after it classifies.
    let mut hi: BTreeMap<u8, u32> = BTreeMap::new();
    let mut formed: Vec<(u32, usize, u8)> = Vec::new();
    for (i, &w) in words.iter().enumerate() {
        let vram = base + (i as u32) * 4;
        let op = (w >> 26) as u8;
        match op {
            // jal
            3 => {
                let target = ((vram + 4) & 0xF000_0000) | ((w & 0x03FF_FFFF) << 2);
                if target >= base && target < end {
                    out.jal_targets.insert(target);
                }
            }
            // j
            2 => {
                let target = ((vram + 4) & 0xF000_0000) | ((w & 0x03FF_FFFF) << 2);
                if target >= base && target < end {
                    out.jump_targets.insert(target);
                }
            }
            // lui
            0x0F => {
                let rt = ((w >> 16) & 0x1F) as u8;
                hi.insert(rt, (w & 0xFFFF) << 16);
            }
            // addiu / addi / ori
            8 | 9 | 0x0D => {
                let rs = ((w >> 21) & 0x1F) as u8;
                let rt = ((w >> 16) & 0x1F) as u8;
                if let Some(&high) = hi.get(&rs) {
                    let imm = w & 0xFFFF;
                    let value = if op == 0x0D {
                        high | imm
                    } else {
                        let signed = imm as i16 as i32;
                        (high as i32).wrapping_add(signed) as u32
                    };
                    if value >= base && value < end && value.is_multiple_of(4) {
                        formed.push((value, i, rt));
                    }
                }
                if rt != rs {
                    hi.remove(&rt);
                }
            }
            // Anything else that writes a GPR invalidates its high half.
            // R-type writes rd.
            0 => {
                let rd = ((w >> 11) & 0x1F) as u8;
                hi.remove(&rd);
            }
            op if opcode_writes_gpr_rt(op) => {
                let rt = ((w >> 16) & 0x1F) as u8;
                hi.remove(&rt);
            }
            _ => {}
        }
    }

    for (target, low, reg) in formed {
        let after_return = ends_after.contains(&target);
        let mem_base = used_as_memory_base(words, low, reg);
        let prologue = prologue_at(words, base, target);
        // Both guards exclude an address the forming code then dereferences
        // with an offset: that is a data base, and this game does put
        // constants inside `.text`, including just after a function end
        // where the positional guard alone would accept them. Measured on
        // SCES_507.60, 2026-09-05: 389 addresses satisfy the positional
        // guard and none of them is a memory base, so this costs no entry
        // today and closes the case where one would be silently wrong.
        if after_return && !mem_base {
            out.pointer_targets.insert(target);
        }
        if prologue && !mem_base {
            out.prologue_targets.insert(target);
        }
        out.pointers.push(PointerSite {
            target,
            site: base + (low as u32) * 4,
            reg,
            mem_base,
            prologue,
            after_return,
        });
    }
    out
}

/// True when `reg`, holding the address a `lui`/`addiu` pair just formed at
/// `low`, is used as the base register of a load or store before anything
/// writes it again.
///
/// This separates `la` of a data object from `la` of a function. A function
/// pointer is stored, passed or called; it is never dereferenced with an
/// offset by the code that formed it. An address that is dereferenced is a
/// data base, and this game does put data inside `.text` (see `ee-emit`'s
/// census), so without this test a table of constants embedded between two
/// functions could be read as an entry.
///
/// The walk stops at the first of: a write to `reg`, a call (which ends the
/// value's life for every caller-saved register and is where a function
/// pointer is normally consumed), an unconditional transfer, or 64
/// instructions. Stopping early can only lose a `mem_base` verdict, and a
/// lost verdict is caught by the prologue test, which the address still has
/// to pass.
fn used_as_memory_base(words: &[u32], low: usize, reg: u8) -> bool {
    let limit = (low + 1 + 64).min(words.len());
    for &w in &words[low + 1..limit] {
        let op = (w >> 26) as u8;
        let rs = ((w >> 21) & 0x1F) as u8;
        if opcode_is_memory(op) {
            if rs == reg {
                return true;
            }
            // A store reads rt and a load writes it; only the write ends
            // the value's life.
            if opcode_writes_gpr_rt(op) && ((w >> 16) & 0x1F) as u8 == reg {
                return false;
            }
            continue;
        }
        // jal, jalr: the call site is where a function pointer is used.
        if op == 3 || (op == 0 && (w & 0x3F) == 0x09) {
            return false;
        }
        if is_unconditional_transfer(w) {
            return false;
        }
        if writes_gpr(w) == Some(reg) {
            return false;
        }
    }
    false
}

/// True when opcode `op` is a load or a store, of any width, to any
/// register file. Every one of them takes its address as `rs` plus a signed
/// 16-bit offset, which is also the field a relink rewrites, so
/// `correlate::mask` masks exactly this set.
pub fn opcode_is_memory(op: u8) -> bool {
    matches!(
        op,
        // lq, sq
        0x1E | 0x1F
        // ldl, ldr
        | 0x1A | 0x1B
        // lb lh lwl lw lbu lhu lwr lwu, sb sh swl sw sdl sdr swr cache
        | 0x20..=0x2F
        // lwc1, pref, ldc1, lqc2, ld
        | 0x31 | 0x33 | 0x35 | 0x36 | 0x37
        // swc1, sdc1, sqc2, sd
        | 0x39 | 0x3D | 0x3E | 0x3F
    )
}

/// The GPR `word` writes, when it writes exactly one and the encoding says
/// which. Conservative: `None` for anything this does not model, which
/// keeps `used_as_memory_base` walking rather than stopping, and a longer
/// walk can only reject a candidate.
fn writes_gpr(word: u32) -> Option<u8> {
    let op = (word >> 26) as u8;
    match op {
        // SPECIAL: everything with a destination writes rd. jr/jalr, the
        // stores to hi/lo and syscall/break do not, and their rd field is
        // zero, which is $zero and harmless to report.
        0 => Some(((word >> 11) & 0x1F) as u8),
        // REGIMM: only the "and link" forms write, and they write $ra.
        1 => {
            let rt = (word >> 16) & 0x1F;
            (matches!(rt, 0x10..=0x13)).then_some(31)
        }
        // jal writes $ra.
        3 => Some(31),
        // addi addiu slti sltiu andi ori xori lui, daddi daddiu
        8..=0x0F | 0x18 | 0x19 => Some(((word >> 16) & 0x1F) as u8),
        // mfc0/mfc1/mfc2 and cfc: rt. The rest of a coprocessor block does
        // not write a GPR.
        0x10..=0x12 if matches!((word >> 21) & 0x1F, 0 | 2) => Some(((word >> 16) & 0x1F) as u8),
        // MMI: pmfhi and friends write rd.
        0x1C => Some(((word >> 11) & 0x1F) as u8),
        _ if opcode_writes_gpr_rt(op) => Some(((word >> 16) & 0x1F) as u8),
        _ => None,
    }
}

/// True when the words at `addr` are a function prologue.
///
/// The predicate, and why each part of it is there:
///
/// * within the first four words there is an `addiu $sp, $sp, -N` (or
///   `daddiu`), N > 0 and a multiple of 16. A function that has a frame
///   allocates it at the top, and the EE ABI keeps the stack 16-byte
///   aligned, so a frame is always a multiple of 16. Requiring a negative
///   immediate excludes the epilogue's matching `addiu $sp, $sp, N`, which
///   is the other place this instruction appears. The allocation is not
///   always the first word: the compiler schedules address forms and loads
///   that do not depend on the frame ahead of it (the PAL build's ending
///   thread entry at 0x0021F1E8 is `lui`, `addiu`, `lw`, then the frame).
///   Each word ahead of the allocation has to be one of those: no control
///   transfer, and no read or write of `$sp`. A word that touches `$sp`
///   before the frame exists is not prologue code, and a transfer means
///   the allocation is a different basic block's.
/// * within the next eight instructions, a `sw`/`sd`/`sq` through `$sp` of
///   `$ra` or of a callee-saved register ($s0-$s7, $fp), or a call. The
///   frame allocation alone is not enough: a mid-function `alloca` would
///   match it. What no code but a prologue does is allocate a frame and
///   then immediately save into it a register whose value the caller owns.
///   A leaf that saves nothing but calls (a tail position, or a call the
///   compiler made after the frame) is the same evidence one step removed,
///   and is accepted for it. Eight instructions is what this build's
///   prologues take to reach the first save: the PAL `iosThreadMain`
///   coroutine entry at 0x001021A0 saves `$fp` two instructions in, and
///   the interleaved `lui` loads in the ones with the widest gap keep it
///   inside eight.
///
/// The two together cannot be produced by data that happens to sit in
/// `.text`: the frame word alone constrains 26 of 32 bits, and the save
/// word constrains the base register and the saved register on top of it.
pub fn prologue_at(words: &[u32], base: u32, addr: u32) -> bool {
    let Some(index) = addr.checked_sub(base).map(|d| (d / 4) as usize) else {
        return false;
    };
    for lead in 0..=3usize {
        let Some(&w) = words.get(index + lead) else {
            return false;
        };
        if allocates_a_frame(w) {
            let frame = index + lead;
            for k in 1..=8usize {
                let Some(&w) = words.get(frame + k) else {
                    return false;
                };
                if saves_a_caller_register(w) || is_call(w) {
                    return true;
                }
            }
            return false;
        }
        if !is_scheduled_setup(w) {
            return false;
        }
    }
    false
}

/// `addiu $sp, $sp, -N` or `daddiu $sp, $sp, -N`, N > 0 and a multiple of
/// 16: the frame allocation of a function prologue.
fn allocates_a_frame(word: u32) -> bool {
    let op = word >> 26;
    if op == 0 {
        // subu / dsubu $sp, $sp, $reg: the frame a compiler allocates when
        // it does not fit a 16-bit immediate. The register was loaded by an
        // ori or lui just before, which is scheduled setup. Measured on
        // SCES_507.60 at 0x001345C8 (a cdvd thread main whose frame is
        // 0x82C0 bytes: ori $t4, 0x82C0; dsubu $sp, $sp, $t4).
        let funct = word & 0x3F;
        return (funct == 0x23 || funct == 0x2F)
            && (word >> 11) & 0x1F == 29
            && (word >> 21) & 0x1F == 29
            && (word >> 16) & 0x1F != 0;
    }
    if !(op == 9 || op == 0x19) {
        return false;
    }
    if (word >> 21) & 0x1F != 29 || (word >> 16) & 0x1F != 29 {
        return false;
    }
    let imm = (word & 0xFFFF) as i16 as i32;
    imm < 0 && (-imm as u32).is_multiple_of(16)
}

/// A word the compiler may schedule ahead of a prologue's frame allocation:
/// no control transfer of any kind, and nothing that reads or writes `$sp`.
fn is_scheduled_setup(word: u32) -> bool {
    // A `nop` ahead of a frame is alignment padding after the previous
    // function, not the prologue's own code: nothing at the top of a
    // function needs a delay. Without this, the padding word before a real
    // entry would pass as an entry of its own, one word early.
    if word == 0 {
        return false;
    }
    if branch_target(word, 0).is_some() || is_call(word) || is_unconditional_transfer(word) {
        return false;
    }
    let op = word >> 26;
    // SPECIAL: jr/jalr, syscall/break, and the two-source ALU forms where
    // rt may be $sp.
    if op == 0 {
        if matches!(word & 0x3F, 0x08 | 0x09 | 0x0C | 0x0D) {
            return false;
        }
        if (word >> 16) & 0x1F == 29 {
            return false;
        }
    }
    // rs is the base of every memory word and the first source of every
    // immediate and register ALU form.
    if (word >> 21) & 0x1F == 29 {
        return false;
    }
    // A store's rt is a source too.
    if opcode_is_memory(op as u8) && !opcode_writes_gpr_rt(op as u8) && (word >> 16) & 0x1F == 29 {
        return false;
    }
    writes_gpr(word) != Some(29)
}

/// Function entries proven by shape alone: a prologue (`prologue_at`) that
/// begins right after an unconditional transfer and its delay slot, with
/// any alignment `nop`s in between, and that no branch anywhere in `.text`
/// targets.
///
/// The fifth proof. Code after an unconditional transfer cannot be reached
/// by falling through, so it starts either a function or a label inside
/// the same function. A label is reached by a branch, and a compiler never
/// allocates a frame and saves a caller-owned register at a label, so a
/// prologue there that no branch targets is a function entry. This is what
/// finds a function whose only callers hold its address in a value computed
/// at run time (the PAL build's actCommonFly at 0x0015F6A0, a thread entry
/// stored by the game into its own thread record), which no jal, no
/// instruction-formed pointer and no data word names.
pub fn prologues_after_transfers(words: &[u32], base: u32) -> BTreeSet<u32> {
    let mut branch_targets: BTreeSet<u32> = BTreeSet::new();
    for (i, &w) in words.iter().enumerate() {
        if let Some(t) = branch_target(w, base + (i as u32) * 4) {
            branch_targets.insert(t);
        }
    }
    let mut out = BTreeSet::new();
    for (i, &w) in words.iter().enumerate() {
        // `is_unconditional_transfer` and nothing else: it already accepts
        // `jr $ra`, and accepting `jr` on any other register would accept a
        // jump-table dispatch, whose delay slot is followed by the first
        // case body rather than by a function entry. Measured on
        // SCES_507.60, 2026-09-05: the broader test admitted no extra entry
        // (3986 either way), so the two functions now agree on what ends a
        // function at no cost.
        if !is_unconditional_transfer(w) {
            continue;
        }
        // Past the delay slot, then past padding.
        let mut j = i + 2;
        while j < words.len() && words[j] == 0 {
            j += 1;
        }
        if j >= words.len() {
            break;
        }
        let addr = base + (j as u32) * 4;
        if branch_targets.contains(&addr) {
            continue;
        }
        // The position already proves the address is entered only by a
        // jump or a call, so the frame allocation alone (within the first
        // four words, with only scheduled setup ahead of it) is the whole
        // structural test here: a save into the frame is not required,
        // because a thread main or a callback may spill only its arguments
        // (actCommonFly stores $a0 first and saves nothing the caller owns
        // within eight words).
        let mut k = j;
        let mut is_entry = false;
        while k < words.len() && k < j + 4 {
            if allocates_a_frame(words[k]) {
                is_entry = true;
                break;
            }
            if !is_scheduled_setup(words[k]) {
                break;
            }
            k += 1;
        }
        if is_entry {
            out.insert(addr);
        }
    }
    out
}

/// The `.text` addresses that aligned words of a data section point at
/// where the words at the target are a function prologue (`prologue_at`),
/// each with the address of the data word that holds it.
///
/// The fourth entry proof. A function that is only ever called through a
/// pointer held in a table (a thread entry, a callback in a state table)
/// has no `jal` anywhere and no `lui`/`addiu` pair forming its address:
/// the code loads the pointer from the table. The PAL build's ending
/// threads are exactly this, one of them at 0x0021F1E8, held at 0x002C4184
/// in `.data`. A data word that happens to read as a `.text` address is
/// possible, which is why the prologue test is required: the frame word
/// alone constrains 26 of 32 bits at the target, and the save word more.
pub fn data_pointers_to_prologues(
    data_words: &[u32],
    data_base: u32,
    text_words: &[u32],
    text_base: u32,
) -> Vec<(u32, u32)> {
    let text_end = text_base + (text_words.len() as u32) * 4;
    let mut out = Vec::new();
    for (i, &v) in data_words.iter().enumerate() {
        if v < text_base || v >= text_end || !v.is_multiple_of(4) {
            continue;
        }
        // Only the prologue test carries a data word. "Right after a
        // return" was tried on 2026-09-05 and rejected: jump-table case
        // labels sit there too, and it split BrainMode_Requset's table from
        // its dispatch.
        if prologue_at(text_words, text_base, v) {
            out.push((v, data_base + (i as u32) * 4));
        }
    }
    out
}

/// True when `word` stores `$ra` or a callee-saved GPR ($s0-$s7, $fp)
/// through `$sp`. Only the GPR stores are accepted: `swc1` and friends
/// number a coprocessor register in the same field, and mixing the two
/// register files into one test would accept a store of `$f31`.
fn saves_a_caller_register(word: u32) -> bool {
    let op = word >> 26;
    // sw, sd, sq
    if !matches!(op, 0x2B | 0x3F | 0x1F) {
        return false;
    }
    if (word >> 21) & 0x1F != 29 {
        return false;
    }
    let rt = (word >> 16) & 0x1F;
    rt == 31 || (16..=23).contains(&rt) || rt == 30
}

/// `jal` or `jalr`.
fn is_call(word: u32) -> bool {
    word >> 26 == 3 || (word >> 26 == 0 && (word & 0x3F) == 0x09)
}

/// True when opcode `op` writes GPR `rt`, so a `%hi` half in flight for that
/// register stops being live at that word.
///
/// Shared with `recomp-cli`'s `symbols --data` scan so the two cannot drift.
/// Loads write `rt`; stores read it. The R5900 quadword pair is `lq`
/// (0b011110) and `sq` (0b011111), and only the load writes a GPR. The
/// coprocessor loads (`lwc1` 0x31, `ldc1` 0x35, `lqc2` 0x36, and the `swc1`
/// / `sdc1` / `sqc2` stores) write a coprocessor register or nothing, so
/// they leave a GPR high half alone.
pub fn opcode_writes_gpr_rt(op: u8) -> bool {
    matches!(
        op,
        // lq
        0x1E
        // ldl, ldr
        | 0x1A | 0x1B
        // lb lh lwl lw lbu lhu lwr lwu
        | 0x20..=0x27
        // ld
        | 0x37
    )
}

/// The target of a PC-relative branch or of a `j`, when `word` is one.
///
/// Used to test whether an address is reachable as a label from inside the
/// function that contains it, which is what separates a function entry from
/// a branch target. Covers every EE branch encoding: the four I-type pairs
/// and their likely forms, REGIMM's conditional branches (rt 0x00-0x03 and
/// 0x10-0x13; REGIMM's other rt values are traps and `mtsab`/`mtsah`), the
/// three coprocessor `bc` blocks, and `j`. `jal`, `jr` and `jalr` are calls
/// and returns, not branches, and are excluded.
pub fn branch_target(word: u32, vram: u32) -> Option<u32> {
    let op = word >> 26;
    let rel = || -> Option<u32> {
        let off = (word & 0xFFFF) as i16 as i32;
        Some((vram.wrapping_add(4) as i32).wrapping_add(off << 2) as u32)
    };
    match op {
        // REGIMM: bltz/bgez/bltzl/bgezl and their "and link" forms.
        1 => {
            let rt = (word >> 16) & 0x1F;
            if matches!(rt, 0x00..=0x03 | 0x10..=0x13) {
                rel()
            } else {
                None
            }
        }
        // j
        2 => Some((vram.wrapping_add(4) & 0xF000_0000) | ((word & 0x03FF_FFFF) << 2)),
        // beq bne blez bgtz, and beql bnel blezl bgtzl
        4..=7 | 0x14..=0x17 => rel(),
        // COP0/COP1/COP2 branch on condition (rs == 8).
        0x10..=0x12 if (word >> 21) & 0x1F == 8 => rel(),
        _ => None,
    }
}

/// True when `addr` sits after an unconditional control transfer and its
/// delay slot, with only zero words (alignment padding) in between. `jr
/// $ra`, `j`, `b` and `eret` all end a function; a conditional branch does
/// not, which is what separates a function entry from a label inside one.
///
/// `words` is a whole section starting at `base`.
pub fn follows_a_return(words: &[u32], base: u32, addr: u32) -> bool {
    let Some(index) = addr.checked_sub(base).map(|d| (d / 4) as usize) else {
        return false;
    };
    if index > words.len() {
        return false;
    }
    // Look back over at most eight words of padding for the delay slot.
    for pad in 0..=8usize {
        let Some(delay) = index.checked_sub(1 + pad) else {
            return false;
        };
        if delay == 0 {
            return false;
        }
        if words[delay + 1..index].iter().any(|&w| w != 0) {
            return false;
        }
        if is_unconditional_transfer(words[delay - 1]) {
            return true;
        }
    }
    false
}

/// The words that transfer control unconditionally: `j`, `jr $ra`, `b`,
/// `bl` (the likely form of `b`) and `eret`. Nothing after one of these and
/// its delay slot is reached by falling into it.
///
/// `b` is `beq $0, $0, off` and `bl` is `beql $0, $0, off`: the assembler
/// spells them as one mnemonic and the encoding is a compare of $0 against
/// itself, which is always taken. Measured on SCES_507.60: the function at
/// 0x0013C720 is entered only by `jal` and the function before it ends with
/// `b` and its delay slot, so leaving these two out rejects a real entry.
///
/// `jr` with any register but $ra is a jump-table dispatch, and the word
/// after its delay slot is the first case body rather than a function
/// entry, so it is deliberately not accepted.
fn is_unconditional_transfer(word: u32) -> bool {
    let op = word >> 26;
    // j
    if op == 2 {
        return true;
    }
    // b / bl: beq or beql with both operands $0.
    if (op == 4 || op == 0x14) && (word >> 21) & 0x1F == 0 && (word >> 16) & 0x1F == 0 {
        return true;
    }
    // SPECIAL jr $ra
    if op == 0 && (word & 0x3F) == 0x08 && (word >> 21) & 0x1F == 31 {
        return true;
    }
    // COP0 eret
    word == 0x4200_0018
}

/// One jump table recovered from a data section.
#[derive(Debug, Clone)]
pub struct ScannedJumpTable {
    pub vram: u32,
    pub targets: Vec<u32>,
}

/// Sorted `(start, end)` byte ranges of the functions a table's targets
/// must land inside.
pub type FunctionRanges = [(u32, u32)];

/// Scan `data` (a read-only data section starting at `base`) for switch
/// tables: runs of at least `min_entries` word-aligned words that all point
/// inside one function's byte range.
///
/// The single-owner requirement is what keeps this from firing on arbitrary
/// data that happens to look like addresses: a float array or a pointer
/// table into `.data` cannot satisfy it, and a table of function pointers
/// would land on function *starts* spread across many functions rather than
/// inside one.
pub fn scan_jump_tables(
    data: &[u8],
    base: u32,
    functions: &FunctionRanges,
    min_entries: usize,
) -> Vec<ScannedJumpTable> {
    let owner_of = |addr: u32| -> Option<usize> {
        let idx = functions.partition_point(|&(start, _)| start <= addr);
        if idx == 0 {
            return None;
        }
        let (start, end) = functions[idx - 1];
        if addr >= start && addr < end {
            Some(idx - 1)
        } else {
            None
        }
    };

    let mut out = Vec::new();
    let count = data.len() / 4;
    let mut i = 0usize;
    while i < count {
        let word = read_u32(data, i * 4);
        let Some(owner) = owner_of(word) else {
            i += 1;
            continue;
        };
        if !word.is_multiple_of(4) {
            i += 1;
            continue;
        }
        let mut j = i + 1;
        let mut targets = vec![word];
        while j < count {
            let w = read_u32(data, j * 4);
            if owner_of(w) == Some(owner) && w.is_multiple_of(4) {
                targets.push(w);
                j += 1;
            } else {
                break;
            }
        }
        if targets.len() >= min_entries {
            out.push(ScannedJumpTable {
                vram: base + (i as u32) * 4,
                targets,
            });
            i = j;
        } else {
            i += 1;
        }
    }
    out
}

fn read_u32(data: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([data[off], data[off + 1], data[off + 2], data[off + 3]])
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_jal_targets_inside_the_section() {
        // jal 0x00100010 at 0x00100000, then padding.
        let base = 0x0010_0000u32;
        let jal = 0x0C00_0000 | ((0x0010_0010u32 >> 2) & 0x03FF_FFFF);
        let words = vec![jal, 0, 0, 0, 0x03E0_0008, 0];
        let scan = scan_text(&words, base);
        assert!(scan.jal_targets.contains(&0x0010_0010));
        assert_eq!(scan.jal_targets.len(), 1);
    }

    #[test]
    fn accepts_a_pointer_only_after_a_function_end() {
        let base = 0x0010_0000u32;
        // 0: jr $ra, 1: delay slot -> a function ends at 0x00100008.
        // 2: lui $a0, 0x10 ; 3: addiu $a0, $a0, 8 -> 0x00100008, accepted.
        // 4: lui $a1, 0x10 ; 5: addiu $a1, $a1, 0x14 -> 0x00100014, which
        // is not just after a function end, so it is rejected.
        let words = vec![
            0x03E0_0008,
            0x0000_0000,
            0x3C04_0010,
            0x2484_0008,
            0x3C05_0010,
            0x24A5_0014,
            0x0000_0000,
        ];
        let scan = scan_text(&words, base);
        assert!(scan.pointer_targets.contains(&0x0010_0008));
        assert!(!scan.pointer_targets.contains(&0x0010_0014));
    }

    #[test]
    fn a_run_pointing_into_one_function_is_a_table() {
        let functions = [(0x0010_0000u32, 0x0010_0100u32), (0x0010_0100, 0x0010_0200)];
        let mut data = Vec::new();
        for a in [0x0010_0010u32, 0x0010_0020, 0x0010_0030, 0x0010_0040] {
            data.extend_from_slice(&a.to_le_bytes());
        }
        // A word pointing into the other function ends the run.
        data.extend_from_slice(&0x0010_0110u32.to_le_bytes());
        let tables = scan_jump_tables(&data, 0x0055_0000, &functions, 3);
        assert_eq!(tables.len(), 1);
        assert_eq!(tables[0].vram, 0x0055_0000);
        assert_eq!(tables[0].targets.len(), 4);
    }

    #[test]
    fn a_load_clobbers_a_high_half_and_a_store_does_not() {
        // lq is a load and sq is a store, and the table used to have them
        // the other way round: a live high half survived a clobbering lq.
        assert!(opcode_writes_gpr_rt(0x1E), "lq writes rt");
        assert!(!opcode_writes_gpr_rt(0x1F), "sq reads rt");
        assert!(opcode_writes_gpr_rt(0x23), "lw writes rt");
        assert!(!opcode_writes_gpr_rt(0x2B), "sw reads rt");
        assert!(!opcode_writes_gpr_rt(0x31), "lwc1 writes an FPR");
        assert!(!opcode_writes_gpr_rt(0x35), "ldc1 writes an FPR");
        assert!(opcode_writes_gpr_rt(0x37), "ld writes rt");
    }

    #[test]
    fn branch_targets_are_decoded_and_calls_are_not() {
        // beq $0,$0,+2 at 0x00100000 -> 0x0010000C.
        assert_eq!(branch_target(0x1000_0002, 0x0010_0000), Some(0x0010_000C));
        // bne $2,$3,-4 at 0x00100010 -> 0x00100004.
        assert_eq!(branch_target(0x1443_FFFC, 0x0010_0010), Some(0x0010_0004));
        // bgez $2,+1 (REGIMM rt=1) at 0x00100000 -> 0x00100008.
        assert_eq!(branch_target(0x0441_0001, 0x0010_0000), Some(0x0010_0008));
        // bltzal (REGIMM rt=0x10) is still a branch target.
        assert_eq!(branch_target(0x0450_0001, 0x0010_0000), Some(0x0010_0008));
        // teqi (REGIMM rt=0x0C) is a trap, not a branch.
        assert_eq!(branch_target(0x044C_0001, 0x0010_0000), None);
        // j 0x00100020.
        assert_eq!(branch_target(0x0804_0008, 0x0010_0000), Some(0x0010_0020));
        // jal is a call, jr and jalr are not direct branches.
        assert_eq!(branch_target(0x0C04_0008, 0x0010_0000), None);
        assert_eq!(branch_target(0x03E0_0008, 0x0010_0000), None);
        // bc1t (COP1, rs == 8).
        assert_eq!(branch_target(0x4501_0001, 0x0010_0000), Some(0x0010_0008));
        // mfc1 (COP1, rs == 0) is not a branch.
        assert_eq!(branch_target(0x4402_0800, 0x0010_0000), None);
    }

    #[test]
    fn an_unconditional_transfer_ends_a_function_and_a_conditional_one_does_not() {
        let base = 0x0010_0000u32;
        // 0: nop, 1: jr $ra, 2: delay slot,
        // 3: an entry, 4: bne $2,$3 (conditional), 5: delay slot,
        // 6: not an entry, 7: jr $t0 (a switch dispatch), 8: delay slot,
        // 9: the first case body, not an entry either,
        // 10: b (beq $0,$0), 11: delay slot,
        // 12: an entry: `b` is unconditional.
        let words = vec![
            0x0000_0000,
            0x03E0_0008,
            0x0000_0000,
            0x0000_0000,
            0x1443_0001,
            0x0000_0000,
            0x0000_0000,
            0x0100_0008,
            0x0000_0000,
            0x0000_0000,
            0x1000_0001,
            0x0000_0000,
            0x0000_0000,
        ];
        assert!(follows_a_return(&words, base, base + 3 * 4));
        assert!(!follows_a_return(&words, base, base + 6 * 4));
        assert!(!follows_a_return(&words, base, base + 9 * 4));
        assert!(follows_a_return(&words, base, base + 12 * 4));
    }

    /// `addiu $sp, $sp, imm`.
    fn frame(imm: i32) -> u32 {
        0x27BD_0000 | ((imm as i16) as u16 as u32)
    }

    /// `sd rt, off($sp)`.
    fn save(rt: u32, off: u32) -> u32 {
        0xFFA0_0000 | (rt << 16) | (off & 0xFFFF)
    }

    /// `lui rt, hi` and `addiu rt, rt, lo` for `value`.
    fn la(reg: u32, value: u32) -> (u32, u32) {
        let lo = (value & 0xFFFF) as i32 as i16;
        let hi = ((value as i32 - lo as i32) >> 16) as u32 & 0xFFFF;
        (
            0x3C00_0000 | (reg << 16) | hi,
            0x2400_0000 | (reg << 21) | (reg << 16) | (lo as u16 as u32),
        )
    }

    #[test]
    fn a_frame_and_a_saved_register_are_a_prologue() {
        let base = 0x0010_0000u32;
        // The shape of the PAL coroutine entry at 0x001021A0: allocate
        // 0xB0, load an address, then save $fp into the new frame.
        let (hi, lo) = la(4, 0x0054_D650);
        let words = vec![frame(-0xB0), hi, save(30, 0x90), lo, 0];
        assert!(prologue_at(&words, base, base));
        // A frame that is not a multiple of 16 is not one this ABI makes.
        let words = vec![frame(-0xB4), hi, save(30, 0x90), lo, 0];
        assert!(!prologue_at(&words, base, base));
        // The epilogue's matching positive adjustment is not a prologue.
        let words = vec![frame(0xB0), hi, save(30, 0x90), lo, 0];
        assert!(!prologue_at(&words, base, base));
        // A frame with nothing saved into it and no call is not proof.
        let mut words = vec![frame(-0x10)];
        words.resize(11, 0);
        assert!(!prologue_at(&words, base, base));
    }

    #[test]
    fn setup_words_may_precede_the_frame_allocation() {
        let base = 0x0010_0000u32;
        // The shape of the PAL ending thread entry at 0x0021F1E8: `lui`,
        // `addiu $7, $0, 10`, `lw $6, lo($2)`, then the frame and `sd $ra`.
        let (hi, _) = la(2, 0x0028_F4C0);
        let words = vec![hi, 0x2407_000A, 0x8C46_F4C0, frame(-0xB0), save(31, 0xA0), 0, 0, 0, 0, 0, 0, 0, 0];
        assert!(prologue_at(&words, base, base));
        // A word that reads $sp before the frame exists is not setup.
        let words = vec![hi, 0x8FA6_0010, 0x8C46_F4C0, frame(-0xB0), save(31, 0xA0), 0, 0, 0, 0, 0, 0, 0, 0];
        assert!(!prologue_at(&words, base, base));
        // A transfer ahead of the frame puts it in another block.
        let words = vec![hi, 0x1000_0001, 0x8C46_F4C0, frame(-0xB0), save(31, 0xA0), 0, 0, 0, 0, 0, 0, 0, 0];
        assert!(!prologue_at(&words, base, base));
        // Four setup words is one too many.
        let words = vec![hi, hi, hi, hi, frame(-0xB0), save(31, 0xA0), 0, 0, 0, 0, 0, 0, 0, 0];
        assert!(!prologue_at(&words, base, base));
    }

    #[test]
    fn a_prologue_after_a_transfer_is_an_entry_unless_branched_to() {
        let base = 0x0010_0000u32;
        // 0: jr $ra, 1: delay slot, 2: nop padding, 3: frame, 4: save $ra,
        // then a `b` back over a second prologue at 8 that is a branch target.
        let mut words = vec![0x03E0_0008, 0, 0, frame(-0x20), save(31, 0x10), 0, 0x1000_0001, 0,
                             frame(-0x10), save(31, 0x0)];
        words.resize(24, 0);
        let found = prologues_after_transfers(&words, base);
        assert!(found.contains(&(base + 3 * 4)));
        // 8 follows the `b` at 6 and its delay slot, but `b +1` at 6 targets 8, so it is a label.
        assert!(!found.contains(&(base + 8 * 4)));
    }

    #[test]
    fn a_data_word_pointing_at_a_prologue_is_an_entry() {
        let base = 0x0010_0000u32;
        let text = vec![0, 0, frame(-0x20), save(31, 0x10), 0, 0, 0, 0, 0, 0, 0, 0];
        let data_base = 0x0020_0000u32;
        // One pointer to the prologue, one to a plain word, one outside
        // .text, one misaligned.
        let data = vec![base + 8, base + 4, 0x0030_0000, base + 9];
        let found = data_pointers_to_prologues(&data, data_base, &text, base);
        assert_eq!(found, vec![(base + 8, data_base)]);
    }

    #[test]
    fn a_pointer_to_a_prologue_is_an_entry_and_a_load_base_is_not() {
        let base = 0x0010_0000u32;
        // 0..3: a function that ends with `jr $ra` + delay slot.
        // 4,5:   `la $6, 0x00100020` (the prologue below).
        // 6,7:   `la $5, 0x00100030` (the constants below), then a load
        //        through $5, which is what a data base looks like.
        // 8,9:   `b` and its delay slot, so the prologue does not follow a
        //        return and only the third rule can prove it.
        // 10,11: `addiu $sp,$sp,-0x20`, `sd $ra,0x10($sp)`.
        // 12,13: two words of embedded data, pointed at from 6,7.
        let (p_hi, p_lo) = la(6, base + 10 * 4);
        let (d_hi, d_lo) = la(5, base + 12 * 4);
        let words = vec![
            0x0000_0000,
            0x0000_0000,
            0x03E0_0008,
            0x0000_0000,
            p_hi,
            p_lo,
            d_hi,
            d_lo,
            // a load through the register the pair just formed
            0x8CA2_0000,
            // b +1 and its delay slot land 10,11 after an unconditional
            // transfer, so the positional rule must not be what fires.
            0x1000_0000,
            frame(-0x20),
            save(31, 0x10),
            0xDEAD_BEEF,
            0xFEED_FACE,
        ];
        let scan = scan_text(&words, base);
        assert!(scan.prologue_targets.contains(&(base + 10 * 4)));
        // The data base is materialized and lands in .text, but it is
        // dereferenced and its target is not a prologue, so it proves
        // nothing. It is still swept up for the report.
        assert!(!scan.prologue_targets.contains(&(base + 12 * 4)));
        let data = scan
            .pointers
            .iter()
            .find(|p| p.target == base + 12 * 4)
            .expect("the data base is in the sweep");
        assert!(data.mem_base);
        assert!(!data.prologue);
    }

    #[test]
    fn a_mid_function_label_is_not_an_entry() {
        let base = 0x0010_0000u32;
        // `la $4, 0x00100010`, where 0x00100010 is a `nop` in the middle of
        // the same function: no prologue, no preceding return.
        let (hi, lo) = la(4, base + 4 * 4);
        let words = vec![hi, lo, 0x0000_0000, 0x0000_0000, 0x0000_0000, 0x0000_0000];
        let scan = scan_text(&words, base);
        assert!(scan.prologue_targets.is_empty());
        assert!(scan.pointer_targets.is_empty());
        assert_eq!(scan.pointers.len(), 1);
        assert_eq!(scan.pointers[0].target, base + 4 * 4);
    }

    #[test]
    fn a_run_spanning_two_functions_is_not_a_table() {
        let functions = [(0x0010_0000u32, 0x0010_0100u32), (0x0010_0100, 0x0010_0200)];
        let mut data = Vec::new();
        for a in [0x0010_0010u32, 0x0010_0110, 0x0010_0020] {
            data.extend_from_slice(&a.to_le_bytes());
        }
        assert!(scan_jump_tables(&data, 0x0055_0000, &functions, 3).is_empty());
    }
}
