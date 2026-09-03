#!/usr/bin/env bash
# Mechanical IP gate: refuse to commit game data or ROM-derived output.
# Installed as .git/hooks/pre-commit by tools/install_hooks.sh; also run in CI
# against the full tree (`tools/check_no_rom.sh --all`).
set -euo pipefail

fail=0
err() { echo "check_no_rom: BLOCKED: $1" >&2; fail=1; }

if [[ "${1:-}" == "--all" ]]; then
    mapfile -t files < <(git ls-files)
else
    mapfile -t files < <(git diff --cached --name-only --diff-filter=ACMR)
fi

for f in "${files[@]}"; do
    [[ -e "$f" ]] || continue

    # Forbidden paths: translated output and any game-data directories.
    case "$f" in
        generated/*|gen/*|baserom/*|assets/*|saves/*)
            err "$f (ROM-derived or game-data path)" ;;
    esac

    # Forbidden extensions: game binaries, disc images, extracted assets.
    case "${f,,}" in
        *.iso|*.bin|*.cue|*.elf|*.rom|*.irx|*.img|*.vag|*.tm2|*.pss|*.hd|*.bd|*.int|*.df|*.png)
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
    fi
done

if (( fail )); then
    echo "check_no_rom: commit rejected. This repository must contain no game data." >&2
    exit 1
fi
