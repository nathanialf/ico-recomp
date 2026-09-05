#!/usr/bin/env bash
# One-shot setup: translate the game and build the runtime.
# Usage: ./setup.sh /path/to/your/Ico_PAL.iso    (or .bin from a bin/cue dump)
#
# Prerequisites: git (with submodules), rust (rustup), cmake >= 3.25, a C/C++
# compiler, python3, and a Vulkan driver to play.
#
# The only game data this needs is your own PAL disc image (SCES-50760). Two
# files are copied out of it into baserom/pal/, which .gitignore ignores:
# the boot ELF SCES_507.60 and SRCFILE.TXT, the objdump listing the
# development build left on the disc. config/recomp.toml names that directory
# as [inputs].root, relative to this repository, so nothing has to be edited
# for a first build. Keep the files somewhere else by setting
# ICORECOMP_PAL_ROOT to that directory, which wins over the config file and
# says so when it does.
set -euo pipefail
cd "$(dirname "$0")"

DISC="${1:-}"
[[ -n "$DISC" && -f "$DISC" ]] || { echo "usage: ./setup.sh /path/to/Ico_PAL.iso"; exit 1; }

INPUTS="${ICORECOMP_PAL_ROOT:-baserom/pal}"

echo "==> checking inputs"
for tool in cargo cmake python3; do
    command -v "$tool" >/dev/null || { echo "missing: $tool"; exit 1; }
done

echo "==> extracting the translator's inputs from $DISC"
# Both files come off the disc image the user supplied; nothing is taken from
# anywhere else and nothing extracted is committable (baserom/ is gitignored).
python3 tools/extract_disc_files.py "$DISC" "$INPUTS"

# Fetches every submodule .gitmodules names: third_party/parallel-gs (which
# pulls in its own Granite submodule), third_party/rmlui, third_party/freetype,
# third_party/volk, third_party/Vulkan-Headers and third_party/SDL.
git submodule update --init --recursive

echo "==> building the translator"
( cd tools/recomp && cargo build --release )

echo "==> translating the game (output stays on your machine, gitignored)"
tools/recomp/target/release/icorecomp ee  --config config/recomp.toml
tools/recomp/target/release/icorecomp vu1 --config config/recomp.toml

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

    ICORECOMP_GS=parallel ./build/release/ico

(omit ICORECOMP_GS=parallel for a headless run; on Windows builds the
window is the default). Controller: any SDL3-supported pad, or keyboard;
see src/runtime/host/input.h for the mapping. Saves land in saves/mc0.
Windows: see docs/WINDOWS.md.
EOF
