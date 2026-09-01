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

4. Package it. This is the step that produces something distributable:

       cmake --install build --config Release --prefix dist\windows

   `dist\windows` then holds four files and nothing else:

       icorecomp-runtime.exe
       libicorecomp-parallel-gs.dll     (icorecomp-parallel-gs.dll on MSVC)
       SDL3.dll
       README.txt

   Drop your disc image in beside them as `ico.iso` (or `ico.bin` for a raw
   bin/cue dump) and the folder is self-contained: no config files to
   author, no install step. The runtime resolves its config, saves, log and
   disc probe against the executable's own directory, so it works wherever
   the folder is unzipped and however it is launched. `saves\mc0` and
   `icorecomp.log` are created on first run.

5. Run:

       build\Release\icorecomp-runtime.exe

   A window should open via Vulkan (on Windows builds with the live
   backend, an unset `ICORECOMP_GS` defaults to it). Useful environment
   variables:
   - `ICORECOMP_GS=dump` for headless runs (no window)
   - `ICORECOMP_INPUT_SCRIPT=docs\scripts\newgame.pad` to auto-navigate to
     New Game
   - `ICORECOMP_MAX_VBLANKS=N` to stop after N fields
   - `ICORECOMP_WAV_CAPTURE=out.wav` to capture the audio mix
   - `ICORECOMP_VERBOSE=geom` to turn on the vertex-level checker

## Capturing a geometry run

The profiler summary counts zone entries, never vertices, so on its own it
cannot tell a rise in "vu1" caused by bigger vertex batches from one caused
by a slower code path, and it cannot see wrong geometry at all. Two things
close that gap and both are in the summary block:

- one line per VU1 microprogram that ran in the window, so a rise attaches
  to a program rather than to an average over five of them. The names live
  in the decomp and are not repeated here; `recomp-cli vu1` prints the hash
  for each name.
- a `geom:` line with vertices, primitives, vertices behind the eye and
  primitives wider than the GS guard band, broken down by microprogram and
  by the MSCAL entry point it was dispatched at. These programs have ten
  entries doing quite different work, so the entry pc is what turns "this
  program emits bad geometry" into a code path.

The summary header now carries absolute field numbers and game time, so a
report of "it goes wrong ten seconds in" can be matched to a window.

The per-microprogram lines are always present. The `geom:` line needs the
checker, which is off by default because it re-parses every GIF packet:

    set ICORECOMP_VERBOSE=geom
    icorecomp-runtime.exe

Play until well past the point where it goes wrong, then close the window
and send `icorecomp.log`. A run made to read frame times should leave
`ICORECOMP_VERBOSE` unset, since the checker costs per field.

Input: any SDL3-supported controller (Xbox, DualSense, DualShock 4) or the
keyboard (arrows d-pad, WASD/IJKL sticks, X/C/Z/V cross/circle/square/
triangle, Q/E/1/3 shoulders, Enter start, Backspace select; see
`src/runtime/host/input.h`).

## Logs

Every Windows run writes `icorecomp.log` next to the executable, replacing
the previous run's. The redirect is at the file-descriptor level, so the
runtime, the GS shared library, SDL and the Vulkan loader all land in the
same file, and it survives the console window closing when the process
dies. The runtime's own messages still echo to the console while it is
alive.

- `ICORECOMP_LOG=C:\path\to\run.log` writes somewhere else. The same
  variable turns the sink on for Linux runs, where it is off by default.
- `ICORECOMP_LOG=-` turns the sink off: console only.

When a run started from Explorer fails, the console stays open with a
"Press Enter to close" prompt and the log path, instead of vanishing with
the failure. A run started from an existing `cmd` or PowerShell prompt
never blocks, so scripts and CI are unaffected.

Status: the Windows build is cross-compile-verified and CI-built; if you hit
anything on a real machine, file `icorecomp.log`.
