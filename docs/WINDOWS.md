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

       build\Release\ico.exe --disc "C:\path\to\Ico (USA).iso"

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

   `dist\windows` then holds four files and one folder, nothing else:

       ico.exe
       libicorecomp-parallel-gs.dll     (icorecomp-parallel-gs.dll on MSVC)
       SDL3.dll
       README.txt
       ui\                              (the menu's documents, stylesheet
                                        and fonts; absent if you configured
                                        with -DICORECOMP_UI=OFF)

   Drop your disc image in beside them as `ico.iso` (or `ico.bin` for a raw
   bin/cue dump) and the folder is self-contained: no config files to
   author, no install step. The runtime resolves its config, saves, log and
   disc probe against the executable's own directory, so it works wherever
   the folder is unzipped and however it is launched. `saves\mc0` and
   `icorecomp.log` are created on first run.

5. Run:

       build\Release\ico.exe

   A window should open via Vulkan (on Windows builds with the live
   backend, an unset `ICORECOMP_GS` defaults to it). Useful environment
   variables:
   - `ICORECOMP_GS=dump` for headless runs (no window)
   - `ICORECOMP_INPUT_SCRIPT=docs\scripts\newgame.pad` to auto-navigate to
     New Game
   - `ICORECOMP_MAX_VBLANKS=N` to stop after N fields
   - `ICORECOMP_WAV_CAPTURE=out.wav` to capture the audio mix
   - `ICORECOMP_VERBOSE=geom` to turn on the vertex-level checker

## Executable icon

By default `ico.exe` carries no icon resource, so Windows shows it with its
own default application icon. To ship it with the save's own PS2
memory-card icon instead (the black silhouette on its icon.sys background),
point `ICORECOMP_DISC` at your disc image and build the `icon` target on a
native (non-cross) configure. That runs the host tool
`icorecomp-icon-extract`, which reads `DFDATAS/DATA.DF`'s `icon.sys` and the
save icon it names, renders it at the sizes a `.ico` wants, and writes
`build/icon/ico.ico` plus a PNG a size for a quick look:

    cmake -B build -DICORECOMP_PARALLEL_GS=ON -DICORECOMP_DISC="C:\path\to\Ico (USA).iso"
    cmake --build build --config Release --target icon
    cmake -B build -DICORECOMP_ICON_FILE=build\icon\ico.ico
    cmake --build build --config Release

Reconfiguring with `ICORECOMP_ICON_FILE` set embeds `ico.ico` as the
`icorecomp-runtime` target's icon resource (Windows only; `enable_language(RC)`
runs only in that branch, so a build with neither variable set, including
CI, configures and builds exactly as before). When cross-compiling from
Linux, `icorecomp-icon-extract` cannot run on the build host: build the
`icon` target on a native configure first, then pass its `ico.ico` as
`ICORECOMP_ICON_FILE` to the cross build. Nothing this produces is ever
committed -- `build/icon` stays under the build directory, and
`tools/check_no_rom.sh` blocks `*.ico` and `*.png` outright.

`ico.exe`'s window carries the same render, read from whatever disc ends up
mounted at run time (`src/runtime/ui/save_icon.cpp`), independent of
whether the exe itself was built with `ICORECOMP_ICON_FILE`.

The geometry, the vertex colours and the background come from the disc. The
camera does not: the one the PS2 browser uses is not documented anywhere
this project has found, so the framing is a stated approximation, written
down in `src/runtime/ui/ps2_icon_render.h`.

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
    ico.exe

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
