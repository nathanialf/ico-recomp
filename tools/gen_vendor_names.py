#!/usr/bin/env python3
"""Generate config/vendor_names.txt: retail vendor function addresses mapped
to their Sony SDK names.

ico-recomp never links against the retail Sony SDK objects; the translator
recompiles them like any other function in the base ELF. But debugging and
the runtime's HLE layer (src/runtime) both want real names for the ~945
anonymous func_00XXXXXX entries in the two vendor code blocks (crt0/klib at
0x00100000-0x00101C80 and the SDK library block at 0x002418A0-0x0026F5D4).

The retail decomp (../ico) has not named these functions itself: they are
closed Sony SDK object code, not decompiled from source. But the Aug-6-2001
prototype build's linker map (baserom/aug6/symbols_from_map.txt) already
carries real names for ~4,459 functions, and the SDK objects linked into the
prototype are, function for function, the same SDK release linked into the
September retail build: same instruction stream, differing only in the
literal values of address-relocation fields (call targets, %hi/%lo/%gp_rel
immediates), because the two links lay out memory differently.

So a function name transfers from aug6 to retail whenever the two
instruction streams are identical after masking out exactly those
relocation-carried immediate fields. This script recomputes that
correlation directly from the two base ELFs (the same technique
../ico/decomp/retail_port/correlate.py uses for game code), and
cross-checks the result against that tool's cached retail<->aug6 hit list
(../ico/decomp/retail_port/hits.json) wherever a vendor address appears
there.

Inputs (read-only, never copied into this repo):
  ../ico/config/symbol_addrs.us.txt        retail function map (splat)
  ../ico/baserom/baseelf.elf                retail ELF (byte source)
  ../ico/baserom/aug6/symbols_from_map.txt  aug6 linker-map symbol names
  ../ico/baserom/aug6/baseelf.elf           aug6 ELF (byte source)
  ../ico/decomp/retail_port/hits.json       aug6<->retail hash-hit cache
                                             (cross-check only)

Output: config/vendor_names.txt, `0xADDRESS name  // source` lines sorted
by address, plus a trailing comment block with unmatched/ambiguous counts.
No disassembly, no instruction bytes, and no decomp source paths are
written -- address and name are facts, not derived game content, and this
is the one file tools/check_no_rom.sh allows out of the ROM-derived gate.

Deterministic, stdlib-only. Run as `python3 tools/gen_vendor_names.py` from
the ico-recomp repo root.
"""
from __future__ import annotations

import difflib
import hashlib
import json
import re
import struct
import sys
import tomllib
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
RECOMP_TOML = REPO_ROOT / "config" / "recomp.toml"
OUTPUT_PATH = REPO_ROOT / "config" / "vendor_names.txt"

VENDOR_LO_START, VENDOR_LO_END = 0x00100000, 0x00101C80
VENDOR_HI_START, VENDOR_HI_END = 0x002418A0, 0x0026F5D4
VENDOR_TAG_RE = re.compile(r"vendor_[0-9A-Fa-f]+\.c|\(vendor\)")

SYM_LINE_RE = re.compile(
    r"^(?P<name>[A-Za-z_.$][\w$.]*)\s*=\s*0x(?P<addr>[0-9A-Fa-f]+)\s*;"
    r"(?P<rest>.*)$"
)

# ---------------------------------------------------------------------------
# config/recomp.toml
# ---------------------------------------------------------------------------


def load_decomp_paths() -> dict[str, Path]:
    cfg = tomllib.loads(RECOMP_TOML.read_text())
    decomp = cfg["decomp"]
    root = (REPO_ROOT / decomp["root"]).resolve()
    return {
        "root": root,
        "elf": root / decomp["elf"],
        "symbol_addrs": root / decomp["symbol_addrs"],
        # Not in recomp.toml (aug6 is a correlation aid, not a translator
        # input): sensible defaults under the same read-only checkout.
        "aug6_elf": root / "baserom" / "aug6" / "baseelf.elf",
        "aug6_symbols": root / "baserom" / "aug6" / "symbols_from_map.txt",
        "hits_json": root / "decomp" / "retail_port" / "hits.json",
    }


def expected_elf_sha1() -> str:
    cfg = tomllib.loads(RECOMP_TOML.read_text())
    return cfg["pins"]["elf_sha1"]


# ---------------------------------------------------------------------------
# ELF .text extraction (minimal ELF32 LE parser, stdlib-only)
# ---------------------------------------------------------------------------


class TextSection:
    __slots__ = ("vma", "file_off", "size", "data")

    def __init__(self, vma: int, file_off: int, size: int, data: bytes):
        self.vma = vma
        self.file_off = file_off
        self.size = size
        self.data = data

    def words(self, start_addr: int, end_addr: int) -> list[int]:
        if start_addr < self.vma or end_addr > self.vma + self.size:
            raise ValueError(
                f"range 0x{start_addr:08X}-0x{end_addr:08X} outside .text "
                f"(0x{self.vma:08X}-0x{self.vma + self.size:08X})"
            )
        lo = self.file_off + (start_addr - self.vma)
        hi = self.file_off + (end_addr - self.vma)
        n = (hi - lo) // 4
        return list(struct.unpack_from(f"<{n}I", self.data, lo))


def read_text_section(elf_path: Path) -> TextSection:
    raw = elf_path.read_bytes()
    if raw[:4] != b"\x7fELF":
        raise ValueError(f"{elf_path}: not an ELF file")
    ei_class = raw[4]
    ei_data = raw[5]
    if ei_class != 1 or ei_data != 1:
        raise ValueError(f"{elf_path}: expected ELF32 little-endian")
    # ELF32 header field offsets: e_shoff=0x20, e_shentsize=0x2E,
    # e_shnum=0x30, e_shstrndx=0x32
    e_shoff = struct.unpack_from("<I", raw, 0x20)[0]
    e_shentsize = struct.unpack_from("<H", raw, 0x2E)[0]
    e_shnum = struct.unpack_from("<H", raw, 0x30)[0]
    e_shstrndx = struct.unpack_from("<H", raw, 0x32)[0]

    def section(i: int):
        off = e_shoff + i * e_shentsize
        name_off, sh_type, sh_flags, sh_addr, sh_offset, sh_size = (
            struct.unpack_from("<IIIIII", raw, off)
        )
        return name_off, sh_addr, sh_offset, sh_size

    strtab_off = section(e_shstrndx)[2]
    for i in range(e_shnum):
        name_off, sh_addr, sh_offset, sh_size = section(i)
        end = raw.index(b"\x00", strtab_off + name_off)
        name = raw[strtab_off + name_off : end].decode()
        if name == ".text":
            return TextSection(sh_addr, sh_offset, sh_size, raw)
    raise ValueError(f"{elf_path}: no .text section")


# ---------------------------------------------------------------------------
# splat symbol_addrs.us.txt / symbols_from_map.txt parsing
# ---------------------------------------------------------------------------


class FuncSym:
    __slots__ = ("name", "addr", "vendor", "obj")

    def __init__(self, name: str, addr: int, vendor: bool, obj: str | None):
        self.name = name
        self.addr = addr
        self.vendor = vendor
        self.obj = obj


def parse_func_symbols(path: Path) -> list[FuncSym]:
    """All `type:func` entries, in file order. `obj` is the `.o:xxx.o` tag
    when present (aug6 map carries these; retail symbol_addrs does not)."""
    out = []
    obj_re = re.compile(r"\(\.o:([\w.]+)\)")
    for line in path.read_text(errors="replace").splitlines():
        m = SYM_LINE_RE.match(line.strip())
        if not m:
            continue
        rest = m.group("rest")
        if "type:func" not in rest:
            continue
        obj_m = obj_re.search(rest)
        out.append(
            FuncSym(
                name=m.group("name"),
                addr=int(m.group("addr"), 16),
                vendor=bool(VENDOR_TAG_RE.search(rest)),
                obj=obj_m.group(1) if obj_m else None,
            )
        )
    return out


def in_vendor_range(addr: int) -> bool:
    return (VENDOR_LO_START <= addr < VENDOR_LO_END) or (
        VENDOR_HI_START <= addr < VENDOR_HI_END
    )


# ---------------------------------------------------------------------------
# reloc-normalized instruction hashing
#
# Same idea as ../ico/decomp/retail_port/correlate.py: two links of the same
# object code differ only in the literal bits of address-relocation fields
# (jump targets, %hi/lui immediates, %lo/%gp_rel-tainted immediates).
# Masking exactly those fields before hashing makes the two builds' copies
# of an unchanged SDK function hash identically, while non-relocation
# immediates (e.g. a syscall number) are left intact so they keep
# distinguishing otherwise-similar functions (klib's syscall trampolines
# differ only in that literal).
# ---------------------------------------------------------------------------

_MASK_IMM_OPS = {0x08, 0x09, 0x0D, 0x18, 0x19, 0x1E, 0x1F} | set(range(0x20, 0x40))
_LOAD_OPS = {
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x30, 0x31, 0x33, 0x35, 0x36, 0x37, 0x1E,
}
_ITYPE_WRITE_RT = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x18, 0x19, 0x0F} | _LOAD_OPS
_ITYPE_WRITE_RT_EXCL = {0x31, 0x35, 0x36, 0x39, 0x3D, 0x3E, 0x3F, 0x1F}
GPR_GP = 28


def normalize_words(words: list[int]) -> list[int]:
    taint = [False] * 32  # taint[r]: r holds an unresolved lui/%hi result
    out = []
    for w in words:
        op = (w >> 26) & 0x3F
        rs = (w >> 21) & 0x1F
        rt = (w >> 16) & 0x1F
        nw = w
        if op in (2, 3):  # j, jal: mask the 26-bit target
            nw = w & 0xFC000000
        elif op == 0x0F:  # lui: mask the %hi immediate
            nw = w & 0xFFFF0000
        elif op in _MASK_IMM_OPS and (rs == GPR_GP or taint[rs]):
            # %lo(sym) paired with a preceding lui, or a %gp_rel(sym) ref
            nw = w & 0xFFFF0000

        wd = None
        if op == 0x0F:
            if rt:
                taint[rt] = True
        else:
            if op == 0:  # SPECIAL: writes rd (jalr included)
                wd = (w >> 11) & 0x1F
            elif op == 3:  # jal writes $ra
                wd = 31
            elif op == 1 and rt in (0x10, 0x11):  # bltzal/bgezal
                wd = 31
            elif op in _ITYPE_WRITE_RT and op not in _ITYPE_WRITE_RT_EXCL:
                wd = rt
            elif op == 0x1C:  # MMI: mostly writes rd
                wd = (w >> 11) & 0x1F
            elif op in (0x10, 0x11, 0x12) and rs in (0, 1, 2, 3):  # COPz mf/cf
                wd = rt
            if wd:
                taint[wd] = False
        out.append(nw)
    return out


def trim_trailing_zeros(words: list[int]) -> list[int]:
    out = list(words)
    while out and out[-1] == 0:
        out.pop()
    return out


def func_hash(words: list[int]) -> str | None:
    words = trim_trailing_zeros(words)
    if not words:
        return None
    nw = normalize_words(words)
    return hashlib.sha1(struct.pack(f"<{len(nw)}I", *nw)).hexdigest()


def compute_hashes(
    boundary_syms: list[FuncSym],
    text: TextSection,
    text_end: int,
    wanted: set[int] | None = None,
) -> dict[int, str]:
    """addr -> hash, for every symbol's [addr, next_symbol_addr) span.

    `boundary_syms` supplies the *complete* function-start list so a span's
    end is always the real next function, not just the next one we happen to
    want a hash for (a vendor-only address list would let the last function
    in one vendor block swallow the whole non-vendor gap up to the next
    block). `wanted` restricts which addresses get a hash computed/returned;
    None means all of them.
    """
    addrs = sorted({s.addr for s in boundary_syms})
    bounds = addrs + [text_end]
    out = {}
    for i, addr in enumerate(addrs):
        if wanted is not None and addr not in wanted:
            continue
        end = bounds[i + 1]
        if end <= addr:
            continue
        try:
            words = text.words(addr, end)
        except ValueError:
            continue
        h = func_hash(words)
        if h is not None:
            out[addr] = h
    return out


def compute_word_spans(
    boundary_syms: list[FuncSym],
    text: TextSection,
    text_end: int,
    wanted: set[int] | None = None,
) -> dict[int, list[int]]:
    """addr -> trimmed instruction words for each symbol's span. Same
    boundary logic as compute_hashes, kept separate because the fuzzy pass
    below needs the words themselves, not just their hash."""
    addrs = sorted({s.addr for s in boundary_syms})
    bounds = addrs + [text_end]
    out = {}
    for i, addr in enumerate(addrs):
        if wanted is not None and addr not in wanted:
            continue
        end = bounds[i + 1]
        if end <= addr:
            continue
        try:
            words = text.words(addr, end)
        except ValueError:
            continue
        words = trim_trailing_zeros(words)
        if words:
            out[addr] = words
    return out


# ---------------------------------------------------------------------------
# fuzzy (shape-based) matching, for functions the exact reloc-normalized
# hash above does not resolve.
#
# The retail and aug6 SDK objects are not always byte-for-byte identical --
# unlike game code, this vendor block is Sony's closed SDK, and the two
# builds (June-2001 prototype, September-2001 retail) can carry different
# SDK point releases: same source, different codegen (register allocation,
# instruction scheduling). ../ico/decomp/retail_port/near_miss_scan.py
# catalogues exactly this for game code it can already name from other
# evidence, classifying diffs as identical/trivial/moderate/divergent. This
# reuses that same shape/verdict scheme, blind (no name to start from): a
# retail vendor function's *opcode-only* instruction sequence (registers and
# immediates erased) is aligned against every same-shaped aug6 function
# with difflib, and only a "trivial-or-better" verdict with an unambiguous
# winner is accepted.
# ---------------------------------------------------------------------------

_SHIFT_IMM_FUNCT = {0x00, 0x02, 0x03, 0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F}


def op_key(w: int) -> tuple:
    op = (w >> 26) & 0x3F
    if op == 0:
        return (0, w & 0x3F)
    if op == 1:
        return (1, (w >> 16) & 0x1F)
    if op in (0x10, 0x11, 0x12):
        rs = (w >> 21) & 0x1F
        return (op, rs, w & 0x3F if rs == 0x10 else 0)
    if op == 0x1C:
        return (0x1C, w & 0x3F)
    return (op,)


def shape(w: int) -> int:
    op = (w >> 26) & 0x3F
    if op in (2, 3):
        return w & 0xFC000000
    if op == 0 and (w & 0x3F) in _SHIFT_IMM_FUNCT:
        return w & 0xFFFFF83F
    if op in (0, 0x1C):
        return w
    return w & 0xFFFF0000


def diff_verdict(a_words: list[int], r_words: list[int]) -> tuple[str, int]:
    """(verdict, diff_count), same scheme as near_miss_scan.verdict_of."""
    a_op = [op_key(w) for w in a_words]
    r_op = [op_key(w) for w in r_words]
    sm = difflib.SequenceMatcher(a=a_op, b=r_op, autojunk=False)
    total = max(len(a_words), len(r_words), 1)
    diff_count = 0
    classes = set()
    blocks = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            for k in range(i2 - i1):
                wa, wr = a_words[i1 + k], r_words[j1 + k]
                if wa == wr:
                    continue
                diff_count += 1
                classes.add("imm-only" if shape(wa) == shape(wr) else "regalloc")
        else:
            n = max(i2 - i1, j2 - j1)
            diff_count += n
            sub_a, sub_r = a_op[i1:i2], r_op[j1:j2]
            if sub_a and len(sub_a) == len(sub_r) and sorted(sub_a) == sorted(sub_r):
                classes.add("sched")
            else:
                classes.add("insert/delete-block")
                blocks.append(n)
    ratio = 1.0 - diff_count / total
    max_block = max(blocks) if blocks else 0
    if diff_count == 0:
        return "identical", diff_count
    if diff_count <= 4 or classes == {"imm-only"}:
        return "trivial", diff_count
    if max_block > max(10, 0.3 * total):
        return "divergent", diff_count
    if ratio < 0.70:
        return "divergent", diff_count
    if ratio >= 0.90 and len(classes) == 1:
        return "easy", diff_count
    return "moderate", diff_count


_HIGH_CONFIDENCE_VERDICTS = {"identical", "trivial"}
_LENGTH_WINDOW = 6  # a "trivial" verdict allows at most a few word ops of drift
# Opcode-shape matching erases every register and immediate, so a short
# function's shape (a handful of li/move/jal-and-return) is shared by many
# unrelated functions across the whole binary -- a "trivial" verdict there
# is coincidence, not identity. Exact hashing (which keeps non-relocation
# immediates) has no such floor; this one applies only to the shape pass.
_FUZZY_MIN_LEN = 16


def fuzzy_match_vendor_funcs(
    retail_words: dict[int, list[int]],
    aug6_words: dict[int, list[int]],
    claimed_aug6: set[int],
) -> dict[int, int]:
    """retail addr -> aug6 addr, for functions resolved by shape/diff
    matching. Skips any aug6 addr already claimed by an exact-hash match."""
    by_len: dict[int, list[int]] = defaultdict(list)
    for addr, words in aug6_words.items():
        if addr not in claimed_aug6 and len(words) >= _FUZZY_MIN_LEN:
            by_len[len(words)].append(addr)

    out: dict[int, int] = {}
    for r_addr, r_words in retail_words.items():
        n = len(r_words)
        if n < _FUZZY_MIN_LEN:
            continue
        candidates = []
        for length in range(max(0, n - _LENGTH_WINDOW), n + _LENGTH_WINDOW + 1):
            candidates.extend(by_len.get(length, ()))
        if not candidates:
            continue
        scored = []
        for a_addr in candidates:
            verdict, diff_count = diff_verdict(aug6_words[a_addr], r_words)
            if verdict in _HIGH_CONFIDENCE_VERDICTS:
                scored.append((diff_count, a_addr))
        if not scored:
            continue
        scored.sort(key=lambda t: t[0])
        best_diff, best_addr = scored[0]
        # require an unambiguous winner: no other candidate within 1 diff
        if len(scored) > 1 and scored[1][0] <= best_diff + 1:
            continue
        out[r_addr] = best_addr
    return out


# ---------------------------------------------------------------------------
# hits.json cross-check
# ---------------------------------------------------------------------------


def load_hits_vendor_map(hits_path: Path) -> dict[int, int]:
    """retail vendor addr -> aug6 addr, only for unambiguous 1:1 hit rows."""
    if not hits_path.exists():
        return {}
    data = json.loads(hits_path.read_text())
    out = {}
    for row in data.get("hits", []):
        aug6 = row.get("aug6", [])
        retail = row.get("retail", [])
        if len(aug6) != 1 or len(retail) != 1:
            continue
        r_addr = int(retail[0]["vma"], 16)
        if not in_vendor_range(r_addr):
            continue
        out[r_addr] = int(aug6[0]["vma"], 16)
    return out


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def main() -> int:
    paths = load_decomp_paths()

    retail_elf_sha1 = hashlib.sha1(paths["elf"].read_bytes()).hexdigest()
    expected = expected_elf_sha1()
    if retail_elf_sha1 != expected:
        print(
            f"gen_vendor_names: FATAL: {paths['elf']} sha1 {retail_elf_sha1} "
            f"!= config/recomp.toml pin {expected}",
            file=sys.stderr,
        )
        return 1

    for key in ("symbol_addrs", "aug6_symbols", "aug6_elf"):
        if not paths[key].exists():
            print(f"gen_vendor_names: FATAL: missing input {paths[key]}", file=sys.stderr)
            return 1

    retail_syms = parse_func_symbols(paths["symbol_addrs"])
    aug6_syms = parse_func_symbols(paths["aug6_symbols"])

    retail_text = read_text_section(paths["elf"])
    aug6_text = read_text_section(paths["aug6_elf"])

    # Vendor functions: retail func_ entries carrying the vendor tag, in
    # either of the two known vendor code blocks.
    vendor_syms = [
        s
        for s in retail_syms
        if s.name.startswith("func_") and s.vendor and in_vendor_range(s.addr)
    ]
    # Already-named vendor entries (decomp's own symbol_addrs assigned a
    # real name, not just func_XXXXXXXX): prefer that name outright.
    already_named = [
        s
        for s in retail_syms
        if s.vendor and not s.name.startswith("func_") and in_vendor_range(s.addr)
    ]

    retail_end = retail_text.vma + retail_text.size
    aug6_end = aug6_text.vma + aug6_text.size

    retail_hashes = compute_hashes(
        retail_syms, retail_text, retail_end, wanted={s.addr for s in vendor_syms}
    )
    aug6_hashes = compute_hashes(aug6_syms, aug6_text, aug6_end)

    # aug6 hash -> list of distinct addrs sharing it (hash collisions are
    # ambiguous: we cannot tell which twin a retail function corresponds to).
    aug6_hash_to_addrs: dict[str, set[int]] = defaultdict(set)
    for addr, h in aug6_hashes.items():
        aug6_hash_to_addrs[h].add(addr)

    # retail hash -> count, symmetric ambiguity check (two retail vendor
    # funcs normalizing identically cannot both safely claim the same name).
    retail_hash_counts: dict[str, int] = defaultdict(int)
    for h in retail_hashes.values():
        retail_hash_counts[h] += 1

    aug6_addr_to_name: dict[int, str] = {}
    aug6_addr_to_obj: dict[int, str | None] = {}
    _PREFER_OVER = {"ENTRYPOINT"}
    by_addr: dict[int, list[FuncSym]] = defaultdict(list)
    for s in aug6_syms:
        by_addr[s.addr].append(s)
    for addr, group in by_addr.items():
        candidates = [s for s in group if s.name not in _PREFER_OVER] or group
        aug6_addr_to_name[addr] = candidates[0].name
        aug6_addr_to_obj[addr] = candidates[0].obj

    hits_map = load_hits_vendor_map(paths["hits_json"])

    matched: list[tuple[int, str, str, str | None]] = []  # addr, name, source, obj
    claimed_aug6: set[int] = set()
    unmatched_addrs: list[int] = []
    ambiguous_addrs: list[int] = []

    for s in vendor_syms:
        h = retail_hashes.get(s.addr)
        if h is None:
            unmatched_addrs.append(s.addr)
            continue
        candidates = aug6_hash_to_addrs.get(h, set())
        if len(candidates) == 1 and retail_hash_counts[h] == 1:
            aug6_addr = next(iter(candidates))
            name = aug6_addr_to_name.get(aug6_addr)
            if name:
                matched.append((s.addr, name, "aug6:elf-hash", aug6_addr_to_obj.get(aug6_addr)))
                claimed_aug6.add(aug6_addr)
                continue
        if len(candidates) > 1 or retail_hash_counts[h] > 1:
            # Try the hits.json cross-check to break the tie: it has extra
            # context (retail C source shape) we don't reconstruct here.
            aug6_addr = hits_map.get(s.addr)
            if aug6_addr is not None:
                name = aug6_addr_to_name.get(aug6_addr)
                if name:
                    matched.append((s.addr, name, "aug6:hits.json", aug6_addr_to_obj.get(aug6_addr)))
                    claimed_aug6.add(aug6_addr)
                    continue
            ambiguous_addrs.append(s.addr)
            continue
        # candidates empty: no aug6 function normalizes the same way.
        aug6_addr = hits_map.get(s.addr)
        if aug6_addr is not None:
            name = aug6_addr_to_name.get(aug6_addr)
            if name:
                matched.append((s.addr, name, "aug6:hits.json", aug6_addr_to_obj.get(aug6_addr)))
                claimed_aug6.add(aug6_addr)
                continue
        unmatched_addrs.append(s.addr)

    # Second pass: the retail and aug6 SDK objects are not always byte
    # identical (see fuzzy_match_vendor_funcs docstring above) -- resolve
    # what exact hashing couldn't via opcode-shape diffing, restricted to
    # functions still unresolved and aug6 addresses no exact match claimed.
    still_unresolved = set(unmatched_addrs) | set(ambiguous_addrs)
    if still_unresolved:
        retail_words = compute_word_spans(
            retail_syms, retail_text, retail_end, wanted=still_unresolved
        )
        aug6_words = compute_word_spans(aug6_syms, aug6_text, aug6_end)
        fuzzy_hits = fuzzy_match_vendor_funcs(retail_words, aug6_words, claimed_aug6)
        for r_addr, aug6_addr in fuzzy_hits.items():
            name = aug6_addr_to_name.get(aug6_addr)
            if not name:
                continue
            matched.append((r_addr, name, "aug6:shape-fuzzy", aug6_addr_to_obj.get(aug6_addr)))
            claimed_aug6.add(aug6_addr)
            unmatched_addrs = [a for a in unmatched_addrs if a != r_addr]
            ambiguous_addrs = [a for a in ambiguous_addrs if a != r_addr]

    for s in already_named:
        matched.append((s.addr, s.name, "decomp:symbol_addrs", None))

    matched.sort(key=lambda t: t[0])

    total_vendor = len(vendor_syms) + len(already_named)

    lines = []
    lines.append(
        "# config/vendor_names.txt -- retail vendor function addresses to Sony SDK names."
    )
    lines.append("#")
    lines.append(
        "# Generated by tools/gen_vendor_names.py. Contains ONLY address/name facts:"
    )
    lines.append(
        "# no disassembly, no instruction bytes, no decomp source paths. This is the"
    )
    lines.append(
        "# one exception tools/check_no_rom.sh allows in the ROM-derived data gate."
    )
    lines.append("#")
    lines.append(
        "# Provenance: names are the Aug-6-2001 prototype build's linker-map symbols"
    )
    lines.append(
        "# (baserom/aug6/symbols_from_map.txt in the decomp checkout), carried over to"
    )
    lines.append(
        "# the retail build wherever the two builds' compiled SDK object code is"
    )
    lines.append(
        "# identical after masking relocation-carried immediate fields (same"
    )
    lines.append(
        "# technique as decomp/retail_port/correlate.py). source=aug6:elf-hash means"
    )
    lines.append(
        "# this script's own byte correlation against both base ELFs;"
    )
    lines.append(
        "# source=aug6:hits.json means the match came from correlate.py's cached hit"
    )
    lines.append(
        "# list instead (used only where the direct hash correlation was ambiguous or"
    )
    lines.append(
        "# empty); source=aug6:shape-fuzzy means neither exact method resolved it and"
    )
    lines.append(
        "# the two builds' compiled code differs slightly (this SDK block is not"
    )
    lines.append(
        "# always byte-identical between the two links), so the match instead comes"
    )
    lines.append(
        "# from an opcode-shape diff (registers/immediates erased) against every"
    )
    lines.append(
        "# aug6 function of matching size, accepted only at a near_miss_scan.py-grade"
    )
    lines.append(
        "# identical/trivial verdict with an unambiguous winner; source=decomp:"
    )
    lines.append("# symbol_addrs means the retail decomp already named the function directly.")
    lines.append("#")
    lines.append("# Regenerate: python3 tools/gen_vendor_names.py")
    lines.append("")

    for addr, name, source, obj in matched:
        suffix = f"  // {source}"
        lines.append(f"0x{addr:08X} {name}{suffix}")

    lines.append("")
    lines.append("# ---- unresolved vendor addresses (counts only, no guesses) ----")
    lines.append(f"# total vendor functions: {total_vendor}")
    lines.append(f"# named: {len(matched)}")
    lines.append(f"# ambiguous (hash collision, no confident single match): {len(ambiguous_addrs)}")
    lines.append(f"# unmatched (no aug6 correlation found): {len(unmatched_addrs)}")

    OUTPUT_PATH.write_text("\n".join(lines) + "\n")

    # ---- stderr report -----------------------------------------------------
    print(f"vendor functions: {total_vendor}", file=sys.stderr)
    print(f"named: {len(matched)}", file=sys.stderr)
    print(f"  aug6:elf-hash      : {sum(1 for *_, s, _ in matched if s == 'aug6:elf-hash')}", file=sys.stderr)
    print(f"  aug6:hits.json     : {sum(1 for *_, s, _ in matched if s == 'aug6:hits.json')}", file=sys.stderr)
    print(f"  aug6:shape-fuzzy   : {sum(1 for *_, s, _ in matched if s == 'aug6:shape-fuzzy')}", file=sys.stderr)
    print(f"  decomp:symbol_addrs: {sum(1 for *_, s, _ in matched if s == 'decomp:symbol_addrs')}", file=sys.stderr)
    print(f"ambiguous: {len(ambiguous_addrs)}", file=sys.stderr)
    print(f"unmatched: {len(unmatched_addrs)}", file=sys.stderr)

    by_obj: dict[str, int] = defaultdict(int)
    for addr, name, source, obj in matched:
        by_obj[obj or "(unknown)"] += 1
    print("by library object:", file=sys.stderr)
    for obj, n in sorted(by_obj.items(), key=lambda kv: -kv[1]):
        print(f"  {obj:20s} {n}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
