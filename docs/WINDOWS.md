# Building and running on Windows

Requirements:
- Visual Studio 2022 (17.0+) with the C++ workload, or the Build Tools
- CMake 3.25+
- Rust (stable, via rustup)
- Vulkan SDK (https://vulkan.lunarg.com), any recent version
- Git, with submodules: `git clone --recursive <repo>` or
  `git submodule update --init --recursive`. They are RmlUi and FreeType for
  the menu, SDL for the window, input and audio, volk and Vulkan-Headers for
  the clean-room renderer, and paraLLEl-GS for the shipped one. Licenses and
  pins are recorded per dependency in `third_party/README.md`.
- Your own PAL ICO disc image (.iso, or raw .bin/.cue). The translator's two
  inputs are copied out of it: the boot ELF `SCES_507.60` and the objdump
  listing `SRCFILE.TXT` the disc carries. Nothing else is needed and nothing
  else is read.

One build serves one retail disc, because the generated C is a translation of
one ELF: `SCES_507.60`, the PAL disc (SCES-50760). `docs/TARGET.md` has the
translator side.

Steps, from a VS x64 developer prompt in the repo root:

1. Extract the translator's inputs from your disc image into `baserom\pal`,
   which is where `config/recomp.toml` looks and which is gitignored:

       python3 tools\extract_disc_files.py D:\path\to\Ico_PAL.iso baserom\pal

2. Build the translator and generate the game code (written to `generated/`,
   which never leaves your machine):

       cd tools\recomp
       cargo build --release
       cd ..\..
       tools\recomp\target\release\icorecomp.exe ee --config config\recomp.toml
       tools\recomp\target\release\icorecomp.exe vu1 --config config\recomp.toml

   They write `generated\ee` and `generated\vu1`, which is where the build
   looks; `--out` names somewhere else.

3. Configure and build the runtime with the live renderer:

       cmake -B build -DICORECOMP_PARALLEL_GS=ON
       cmake --build build --config Release

4. Point the runtime at your disc image, either per run:

       build\Release\ico.exe --disc "C:\path\to\Ico (PAL).iso"

   or once, in `config\local.toml`:

       [disc]
       path = "C:/path/to/Ico (PAL).iso"   # .bin/.cue also works

       [saves]
       dir = "saves/mc0"

   Without a dev checkout next to the exe the runtime also reads the boot
   ELF straight out of the disc image (SHA-1 checked against the pin), so a
   built exe plus a disc image is self-contained.

5. Package it. This is the step that produces something distributable:

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

6. Run:

       build\Release\ico.exe

   A window should open via Vulkan (on Windows builds with the live
   backend, an unset `ICORECOMP_GS` defaults to it). That is paraLLEl-GS,
   and it is the only renderer a player gets: the clean-room renderer under
   `src/runtime/gs/render` still builds here, but it was withdrawn from
   `settings.json` and from the menu on 2026-09-05 and has not passed the
   parity gate in `docs/GS_RENDERER.md`. `ICORECOMP_GS_BACKEND=vulkan` or
   `=d3d12` is what still reaches it, for the replay tool and for CI.
   Useful environment variables:
   - `ICORECOMP_GS=dump` for headless runs (no window)
   - `ICORECOMP_INPUT_SCRIPT=docs\scripts\newgame.pad` to auto-navigate to
     New Game
   - `ICORECOMP_MAX_VBLANKS=N` to stop after N fields
   - `ICORECOMP_WAV_CAPTURE=out.wav` to capture the audio mix
   - `ICORECOMP_VERBOSE=geom` to turn on the vertex-level checker

## What a user needs

Nothing beyond the folder from step 5 and their own ICO PAL disc image. No
Vulkan SDK, no Visual C++ redistributable, no installer: unzip, drop the
image in as `ico.iso` (or `ico.bin`), double-click `ico.exe`. Requirements
are Windows 10 1809 or newer, x86_64, and a GPU driver with Vulkan 1.1; the
renderer names the device it picked in the log.

The package holds no `dxcompiler.dll` and no `dxil.dll`. It stopped
carrying them on 2026-09-05, when the native renderers were withdrawn from
the player's build: the D3D12 backend was their only user, so
`CMakeLists.txt` no longer installs them and the `package-windows-cross` CI
job asserts that the folder does not have them. See "The DXC DLLs, for the
replay tool and CI" below for what a developer build still does with them.

There is no renderer setting to try. The renderer is paraLLEl-GS,
`display.backend` was retired on 2026-09-05, and the menu offers no choice:
a `backend` key left in a `settings.json` is named once at info as no longer
read and otherwise ignored. `ICORECOMP_GS_BACKEND` (`auto`, `parallel-gs`,
`vulkan`, `d3d12`, `metal`) still resolves a renderer for the replay tool and
CI (`src/runtime/gs/gs_select.cpp`); it is not a twin of any settings key.

The log is `icorecomp.log` in the same folder, with the previous run's kept
as `icorecomp.prev.log`. If the folder cannot be written it goes to
`%LOCALAPPDATA%\icorecomp` and then to the temp folder; its first line names
the file it ended up in. "Logs" below is the full account.

## The executable icon, and why it is a required step

`ico.exe` ships with the save's own PS2 memory-card icon (the black
silhouette on its `icon.sys` background) as its Windows icon resource. That
resource is not produced by the ordinary build: it is rendered from the
user's own disc by the host tool `icorecomp-icon-extract`, and a build
configured without it produces an `ico.exe` carrying Windows' default
application icon. Producing the packaged exe means running the icon step
first. It has been left out once already.

The tool reads `DFDATAS/DATA.DF`'s `icon.sys` and the save icon it names,
renders it at the sizes a `.ico` wants, and writes `ico.ico` plus a PNG for a
quick look. It cannot run on the build host of a cross build, so the render
is a native build and the cross build only consumes its output.

The whole flow, cross-compiling from Linux:

    # 1. a native configure, only to build and run the extractor
    cmake --preset linux-gcc-release
    cmake --build build/linux-gcc-release --target icorecomp-icon-extract -j2
    ./build/linux-gcc-release/icorecomp-icon-extract \
        /path/to/Ico_PAL.iso ~/ico-icon/ico.ico

    # 2. the cross configure, given that file
    cmake --preset windows-mingw-cross \
        -DICORECOMP_ICON_FILE="$HOME/ico-icon/ico.ico"
    cmake --build build/windows-mingw-cross -j2

The output path must be outside the repository: the `.ico` is game-derived
pixels, `tools/check_no_rom.sh` blocks `*.ico` and `*.png` outright, and the
tool refuses an output path that resolves under the source tree (the
top-level `build/` directory, which is gitignored and can hold nothing
committable, is the one place inside it that is allowed). On a native
Windows or Linux configure, `-DICORECOMP_DISC=<image>` plus
`cmake --build build --target icon` runs the same tool into the build tree.

A cross configure with `ICORECOMP_ICON_FILE` empty prints a loud CMake
warning saying the exe will carry no icon, so the missing step is visible at
configure time rather than at the end of a package.

Reconfiguring with `ICORECOMP_ICON_FILE` set embeds `ico.ico` as the
`icorecomp-runtime` target's icon resource (Windows only; `enable_language(RC)`
runs only in that branch, so a build with neither variable set, including
CI, configures and builds exactly as before). The `.rc` names the file by
path, and the build ties the compiled resource to the icon's own bytes, so a
re-rendered `ico.ico` does not leave the exe carrying the previous one.

Checking a finished exe: `x86_64-w64-mingw32-objdump -h dist/windows/ico.exe`
lists an `.rsrc` section, and a resource lister (icoutils' `wrestool -l`, for
example) names the `RT_GROUP_ICON` entry, resource type 14, inside it.
Explorer showing the silhouette on the file is the same check by eye.

`ico.exe`'s window carries the same render, read from whatever disc ends up
mounted at run time (`src/runtime/ui/save_icon.cpp`), independent of
whether the exe itself was built with `ICORECOMP_ICON_FILE`.

The geometry, the vertex colours and the background come from the disc. The
camera does not: the one the PS2 browser uses is not documented anywhere
this project has found, so the framing is a stated approximation, written
down in `src/runtime/ui/ps2_icon_render.h`.

## The DXC DLLs, for the replay tool and CI

`dxcompiler.dll` and `dxil.dll` are the Microsoft DirectX Shader Compiler,
from release v1.9.2607 of
https://github.com/microsoft/DirectXShaderCompiler. They are the run-time
HLSL fallback for the native D3D12 backend, used only when a build carries
no compiled-in `src/runtime/rhi/rhi_shaders_dxil.h`. Nothing a player runs
loads them: the native renderers are not offered in the shipped build, and
since 2026-09-05 `CMakeLists.txt` installs neither DLL.

What is left is the developer path. `-DICORECOMP_DXC_DIR=<bin/x64 of a DXC
release>` still points the configure at a pair on disk, and the configure
also needs that release's `inc/dxcapi.h` on the include path for the D3D12
backend to be enabled at all. That is what the `package-windows-cross` CI
job unpacks the release for, and what the `shader-blobs` job uses the Linux
`dxc` binary for. To use the fallback in a local D3D12 run of
`icorecomp-gs-replay`, copy the two DLLs beside the executable yourself:
the backend loads `dxcompiler.dll` by full path from the executable's own
directory, and `dxcompiler.dll` then finds `dxil.dll` by base name in that
same directory (`src/runtime/rhi/d3d12/rhi_d3d12_loader.cpp`).

Licences, since a build that does copy them redistributes them:
`dxcompiler.dll` is under the University of Illinois/NCSA Open Source
Licence with the LLVM exceptions, and `dxil.dll` is Microsoft's, under the
terms of that release's `LICENSE-MS.txt`. Both import `MSVCP140.dll`,
`VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`, so a machine with no Visual
C++ 2015-2022 redistributable fails their load with Win32 error 126, which
reads as "file not found" in every message that does not print the number.
https://aka.ms/vs/17/release/vc_redist.x64.exe is the redistributable.

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
keyboard. The defaults live in one place, `kKeyboardBinds` in
`src/runtime/host/settings.cpp`, and every slot is rebindable from the
menu's Input tab: arrows d-pad, WASD/IJKL sticks, X/C/Z cross/circle/square
and Space triangle, Q/E shoulders and 1/3 triggers, T/Y stick clicks, Enter
start, Backspace select, F1 menu, F12 screenshot. On a gamepad the menu is
Guide by default and the screenshot key ships unbound.

## Logs

Every Windows run writes `icorecomp.log` next to the executable. The
previous run's file is renamed to `icorecomp.prev.log` first, so the run
before the one that went wrong is still there. The redirect is at the
file-descriptor level, so the runtime, the GS shared library, SDL and the
Vulkan loader all land in the same file, and it survives the console window
closing when the process dies.

- `ICORECOMP_LOG=C:\path\to\run.log` writes somewhere else. The same
  variable turns the sink on for Linux runs, where it is off by default.
- `ICORECOMP_LOG=-` turns the sink off: console only.
- `debug.log_file` in `settings.json` is the same switch without an
  environment variable. `ICORECOMP_LOG` wins over it for the whole run.

Every line carries a level: error, warn, info or debug. `debug.log_level`
(environment twin `ICORECOMP_LOG_LEVEL`) sets the floor, and the shipped
default is `warn`, so a default run's log holds errors, warnings and the
startup prologue. `info` adds the startup facts and the per-summary lines;
`debug` adds everything, including channels that cost frame time.
`ICORECOMP_VERBOSE` names single channels whose debug lines pass whatever
the floor is, which is how one subsystem is traced without paying for the
rest. It is environment-only: `debug.verbose` was retired as a settings key
and is named once at info if a `settings.json` still holds it.
`debug.log_level` is applied live from the menu; see docs/SETTINGS.md
sections 2, 5 and 9 for the keys and for the phrases worth grepping.

## What to send when it breaks

Send `icorecomp.log`. Send the whole file, not the last few lines, and send
`icorecomp.prev.log` beside it if the run that went wrong was followed by
another one.

That file is the report. It is enough on its own because every run ends with
one block that says how it ended:

```
---- end of run ----
reason: ...
ended in phase: ...
state: ...
wall time: ...
log: ...
---- end of run ----
```

Grep `---- end of run ----` and read the `reason` and `ended in phase`
lines. There is no run without that block: it is written by every exit
path, including a fatal, a crash handler, `abort`, the window closing, and
the `atexit` chain catching an exit that named no reason. It is at warn
whenever the ending was not something the player asked for, so it is in a
default run's log without changing any setting.

If the run faulted, the file also holds a `---- crash ----` block written by
the faulting thread itself with the exception code, the address, the module
and offset the fault falls in, the registers and a backtrace. Send that too;
it is in the same file.

What is NOT needed, and what nobody should be asked to do before filing:

- Setting an `ICORECOMP_*` environment variable. Everything above is on by
  default.
- Turning `debug.log_level` up. `warn`, the shipped default, already holds
  the end-of-run block, the crash block, every error and every warning.
  `info` and `debug` are for a second run once someone has read the first
  log and knows what to ask for.
- Reproducing it. The log from the run that failed is worth more than a
  description of it.

`docs/SETTINGS.md` section 9 lists every phrase in the two blocks and what
each one means.

## The console, and what a failure looks like

`ico.exe` is a GUI-subsystem binary. Started from Explorer it has no console
at all: nothing is printed anywhere, and everything goes to the log file.
When such a run fails, the failure is shown in a message box carrying the
first lines of what went wrong and the path to the log, because a fatal that
reaches nobody is worse than no fatal.

- Started from an existing `cmd` or PowerShell prompt, it attaches to that
  console, echoes to it as it always did, and never blocks on exit, so
  scripts and CI are unaffected.
- `debug.console = true` in `settings.json` allocates a console for a
  double-clicked run. That run does hold its console open on a fatal with a
  "Press Enter to close" prompt and the log path, since the console it owns
  would otherwise vanish with the process. This is the one setting read
  straight off disk before logging exists
  (`rt_settings_peek_boot`), because a console cannot be attached to a
  process that has already decided it has none.

Status: the Windows build is cross-compile-verified and CI-built; if you hit
anything on a real machine, file `icorecomp.log` (see "What to send when it
breaks" above).
