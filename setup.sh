#!/usr/bin/env bash
# One-shot setup: translate the game and build the runtime.
# Usage: ./setup.sh /path/to/your/Ico_USA.iso    (or .bin from a bin/cue dump)
#
# Prerequisites: git (with submodules), rust (rustup), cmake >= 3.25, a C/C++
# compiler, python3, and a Vulkan driver to play. You also need a checkout of
# the ICO decomp project as a sibling directory ../ico with its base ELF
# extracted (see its README); the translator reads its symbol maps.
set -euo pipefail
cd "$(dirname "$0")"

DISC="${1:-}"
[[ -n "$DISC" && -f "$DISC" ]] || { echo "usage: ./setup.sh /path/to/Ico_USA.iso"; exit 1; }

echo "==> checking inputs"
for tool in cargo cmake python3; do
    command -v "$tool" >/dev/null || { echo "missing: $tool"; exit 1; }
done
[[ -f ../ico/baserom/baseelf.elf ]] || {
    echo "missing: ../ico/baserom/baseelf.elf"
    echo "clone the ICO decomp repo next to this one and run its setup first"
    exit 1
}
git submodule update --init --recursive

echo "==> building the translator"
( cd tools/recomp && cargo build --release )

echo "==> translating the game (output stays on your machine, gitignored)"
tools/recomp/target/release/icorecomp ee --out generated/ee
tools/recomp/target/release/icorecomp vu1 --out generated/vu1

echo "==> building the runtime"
cmake --preset linux-gcc-release -B build/release
cmake --build build/release -j"$(nproc)"

echo "==> writing config/local.toml"
[[ -f config/local.toml ]] || cat > config/local.toml <<EOF
[disc]
path = "$DISC"

[saves]
dir = "saves/mc0"
EOF

cat <<'EOF'

Done. Run the game:

    ICORECOMP_GS=parallel ./build/release/icorecomp-runtime

(omit ICORECOMP_GS=parallel for a headless run; on Windows builds the
window is the default). Controller: any SDL3-supported pad, or keyboard;
see src/runtime/host/input.h for the mapping. Saves land in saves/mc0.
Windows: see docs/WINDOWS.md.
EOF
