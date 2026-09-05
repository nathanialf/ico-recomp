#!/usr/bin/env bash
# Mechanical IP gate: refuse to commit game data or ROM-derived output.
# Installed as .git/hooks/pre-commit by tools/install_hooks.sh; also run in CI
# against the full tree (`tools/check_no_rom.sh --all`).
set -euo pipefail

fail=0
err() { echo "check_no_rom: BLOCKED: $1" >&2; fail=1; }

# Operand-level disassembly: one line that is nothing but a MIPS instruction
# with register operands, with or without an address, a raw-word prefix or a
# leading comment marker. That is the shape of a listing (spimdisasm,
# objdump, a hand-copied window of one), and a listing is ROM-derived output
# whatever file it is pasted into. The rule in CLAUDE.md is that the two
# approved exception files (src/runtime/guest/ico_syms.h and
# config/entry_hooks.txt) carry addresses and names and nothing else, and
# this is what makes that mechanical. The same rule applies to every other
# file in the tree: it is a floor, not a licence for the two.
#
# It matches the whole line on purpose. The repository does quote single
# instructions inside prose, in comments that say how a fact was measured
# ("StageOrientInit is called with `addiu $10, $0, 0x3FFF`"), and that is not
# a listing: the instruction sits inside a sentence, in quotes or brackets,
# so the line does not match. Zero lines in the tree match this today. If a
# new one does, the fix is to state the fact and cite where the measurement
# lives, not to reformat the line.
_MNEM='(add|addi|addiu|addu|and|andi|beq|beql|bgez|bgtz|blez|bltz|bne|bnel|daddi|daddiu|daddu|dsll|dsll32|dsra|dsra32|dsrl|dsrl32|dsub|dsubu|jal|jalr|jr|lb|lbu|ld|ldl|ldr|lh|lhu|lq|lui|lw|lwc1|lwl|lwr|lwu|mfc0|mfc1|mtc0|mtc1|mult|multu|move|neg|negu|nor|or|ori|sb|sd|sh|sll|slt|slti|sltiu|sltu|sq|sra|srl|sub|subu|sw|swc1|swl|swr|xor|xori)'
# One operand: a register, a number, or a number(register) memory reference.
_OPND='(\$[a-z0-9]{1,3}|-?(0x)?[0-9A-Fa-f]+(\(\$[a-z0-9]{1,3}\))?)'
# Optional prefix: leading blanks, a comment marker, and a run of nothing but
# hex digits, blanks, colons, slashes and stars (an address, raw words, or a
# /* ... */ listing column). Prose cannot pass this: it has other letters.
_PFX='^[[:blank:]]*((#|//|;)[[:blank:]]*)?([0-9A-Fa-f/*:[:blank:]]*[[:blank:]])?'
ASM_LINE="${_PFX}${_MNEM}"'[[:blank:]]+\$[a-z0-9]{1,3}[[:blank:]]*,[[:blank:]]*'"${_OPND}"'([[:blank:]]*,[[:blank:]]*'"${_OPND}"')?[[:blank:]]*$'

if [[ "${1:-}" == "--all" ]]; then
    mapfile -t files < <(git ls-files)
else
    mapfile -t files < <(git diff --cached --name-only --diff-filter=ACMR)
fi

for f in "${files[@]}"; do
    [[ -e "$f" ]] || continue

    # Forbidden paths: translated output and any game-data directories.
    case "$f" in
        generated/*|gen/*|baserom/*|assets/*|saves/*|screenshots/*)
            err "$f (ROM-derived or game-data path)" ;;
    esac

    # Forbidden extensions: game binaries, disc images, extracted assets.
    # .ico and .icns are the rendered save icon, an image produced from the disc at
    # package time.
    case "${f,,}" in
        *.iso|*.bin|*.cue|*.elf|*.rom|*.irx|*.img|*.vag|*.tm2|*.pss|*.hd|*.bd|*.int|*.df|*.png|*.ico|*.icns)
            err "$f (game-data file extension)" ;;
    esac

    # Size gate: nothing in this repo legitimately exceeds 512 KB.
    if [[ -f "$f" ]]; then
        size=$(wc -c < "$f")
        if (( size > 524288 )); then
            err "$f (${size} bytes > 512 KB; large blobs are not committable)"
        fi
    fi

    # Disassembly / raw-bytes heuristics in text files.
    if [[ -f "$f" && "$f" != tools/check_no_rom.sh ]]; then
        if grep -qE '^\s*\.word\s+0x[0-9A-Fa-f]{8}\s*$' "$f" 2>/dev/null; then
            if (( $(grep -cE '^\s*\.word\s+0x[0-9A-Fa-f]{8}\s*$' "$f") > 16 )); then
                err "$f (contains a raw .word blob; looks like extracted game bytes)"
            fi
        fi
        if line=$(grep -nE "$ASM_LINE" "$f" 2>/dev/null | head -1); then
            if [[ -n "$line" ]]; then
                err "$f:${line%%:*} (operand-level disassembly: a line that is a MIPS instruction with register operands). Addresses and names are committable; instructions are not."
            fi
        fi

        # config/ is stricter still. Every file there declares itself to hold
        # address facts only, so a register operand of any kind, in prose or
        # not, is out: there is no reading of a config file in which naming a
        # machine register is an address fact. Nothing under config/ names one
        # today.
        if [[ "$f" == config/* ]]; then
            if line=$(grep -nE '\$(zero|at|v[01]|a[0-3]|t[0-9]|s[0-7]|k[01]|gp|sp|fp|ra|[0-9]{1,2})([^a-zA-Z0-9_]|$)' "$f" 2>/dev/null | head -1); then
                if [[ -n "$line" ]]; then
                    err "$f:${line%%:*} (a machine register operand under config/, which holds address facts only)"
                fi
            fi
        fi
    fi
done

if (( fail )); then
    echo "check_no_rom: commit rejected. This repository must contain no game data." >&2
    exit 1
fi
