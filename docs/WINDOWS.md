# Building and running on Windows

Requirements:
- Visual Studio 2022 (17.0+) with the C++ workload, or the Build Tools
- CMake 3.25+
- Rust (stable, via rustup)
- Vulkan SDK (https://vulkan.lunarg.com), any recent version
- Git, with submodules: `git clone --recursive <repo>` or
  `git submodule update --init --recursive`
- Your own ICO (USA) disc image (.iso, or raw .bin/.cue) and, for now, a
  checkout of the ICO decomp project as a sibling directory `../ico` with its
  extracted `baserom/baseelf.elf` (the translator reads its symbol maps;
  paths configurable in `config/recomp.toml`)

Steps, from a VS x64 developer prompt in the repo root:

1. Build the translator and generate the game code (written to `generated/`,
   which never leaves your machine):

       cd tools\recomp
       cargo build --release
       cd ..\..
       tools\recomp\target\release\icorecomp.exe ee --out generated\ee
       tools\recomp\target\release\icorecomp.exe vu1 --out generated\vu1

2. Configure and build the runtime with the live renderer:

       cmake -B build -DICORECOMP_PARALLEL_GS=ON
       cmake --build build --config Release

3. Point the runtime at your disc image, either per run:

       build\Release\icorecomp-runtime.exe --disc "C:\path\to\Ico (USA).iso"

   or once, in `config\local.toml`:

       [disc]
       path = "C:/path/to/Ico (USA).iso"   # .bin/.cue also works

       [saves]
       dir = "saves/mc0"

   Without a dev checkout next to the exe the runtime also reads the boot
   ELF straight out of the disc image (SHA-1 checked against the pin), so a
   built exe plus a disc image is self-contained.

4. Run:

       build\Release\icorecomp-runtime.exe

   A window should open via Vulkan (on Windows builds with the live
   backend, an unset `ICORECOMP_GS` defaults to it). Useful environment
   variables:
   - `ICORECOMP_GS=dump` for headless runs (no window)
   - `ICORECOMP_INPUT_SCRIPT=docs\scripts\newgame.pad` to auto-navigate to
     New Game
   - `ICORECOMP_MAX_VBLANKS=N` to stop after N fields
   - `ICORECOMP_WAV_CAPTURE=out.wav` to capture the audio mix

Input: any SDL3-supported controller (Xbox, DualSense, DualShock 4) or the
keyboard (arrows d-pad, WASD/IJKL sticks, X/C/Z/V cross/circle/square/
triangle, Q/E/1/3 shoulders, Enter start, Backspace select; see
`src/runtime/host/input.h`).

Status: the Windows build is cross-compile-verified and CI-built; if you hit
anything on a real machine, capture the console output and file it.
