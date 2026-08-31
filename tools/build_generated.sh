#!/usr/bin/env bash
# Gate B for the translators: every generated TU must compile as C11.
# Compiles generated/ee/*.c and generated/vu1/*.c to objects under
# build/generated/ and reports wall time. -Wall is on for the whole tree;
# warnings are errors so the gate stays mechanical.
#
# Usage: tools/build_generated.sh [-jN]
#
# Linux/bash only. On Windows this gate is not needed: the CMake build
# compiles the same generated TUs through the `generated` / `generated_vu1`
# object libraries (see CMakeLists.txt), with per-compiler flags handled
# there. A python equivalent can replace this script if a native Windows
# gate is ever wanted.
set -euo pipefail

cd "$(dirname "$0")/.."

jobs="${1:--j$(nproc)}"
jobs="${jobs#-j}"

CFLAGS=(-std=c11 -O1 -fno-strict-aliasing -ffp-contract=off -Wall -Werror -c -I include -I generated)

compile_tree() {
    local gen="$1" out="$2"
    mkdir -p "$out"
    find "$gen" -maxdepth 1 -name '*.c' -print0 \
        | xargs -0 -P "$jobs" -I{} bash -c '
            src="$1"; shift
            obj="'"$out"'/$(basename "${src%.c}").o"
            gcc '"${CFLAGS[*]}"' "$src" -o "$obj"
        ' _ {}
    local count
    count=$(find "$gen" -maxdepth 1 -name '*.c' | wc -l)
    echo "build_generated: $gen: $count TUs compiled clean (-Wall -Werror)"
}

found=0
if [[ -d generated/ee ]]; then
    found=1
    SECONDS=0
    compile_tree generated/ee build/generated/ee
    echo "build_generated: generated/ee done in ${SECONDS}s with -j$jobs"
fi
if [[ -d generated/vu1 ]]; then
    found=1
    SECONDS=0
    compile_tree generated/vu1 build/generated/vu1
    echo "build_generated: generated/vu1 done in ${SECONDS}s with -j$jobs"
fi
if (( ! found )); then
    echo "build_generated: no generated/ee or generated/vu1 found; run 'icorecomp ee' / 'icorecomp vu1' first" >&2
    exit 1
fi
