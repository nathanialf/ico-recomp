#!/usr/bin/env bash
# Gate B for the EE translator: every generated TU must compile as C11.
# Compiles all of generated/ee/*.c to objects under build/generated/ee and
# reports wall time. -Wall is on for the whole tree; warnings are errors so
# the gate stays mechanical.
#
# Usage: tools/build_generated.sh [-jN]
set -euo pipefail

cd "$(dirname "$0")/.."

jobs="${1:--j$(nproc)}"
jobs="${jobs#-j}"

gen="generated/ee"
out="build/generated/ee"

if [[ ! -d "$gen" ]]; then
    echo "build_generated: $gen not found; run 'icorecomp ee' first" >&2
    exit 1
fi

mkdir -p "$out"

CFLAGS=(-std=c11 -O1 -fno-strict-aliasing -Wall -Werror -c -I include -I generated)

SECONDS=0
find "$gen" -maxdepth 1 -name '*.c' -print0 \
    | xargs -0 -P "$jobs" -I{} bash -c '
        src="$1"; shift
        obj="'"$out"'/$(basename "${src%.c}").o"
        gcc '"${CFLAGS[*]}"' "$src" -o "$obj"
    ' _ {}

count=$(find "$gen" -maxdepth 1 -name '*.c' | wc -l)
echo "build_generated: $count TUs compiled clean (-Wall -Werror) in ${SECONDS}s with -j$jobs"
