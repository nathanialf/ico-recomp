# macOS

## What this is

An Apple Silicon (arm64) build of the port, targeting macOS 14 and newer,
shipped without ever having been run. Nobody working on this project owns a
Mac. Everything below separates what is checked mechanically from what is
only reasoned about, because the difference matters more here than on the
platforms the port is actually played on.

Scope of this stage:

- arm64 only. There is no x86_64 macOS build and no universal binary.
- The shipping renderer is paraLLEl-GS over MoltenVK, the same shared library
  the Linux and Windows builds use, and it is the only one a player gets.
  The clean-room native renderer builds here too, and since 2026-09-04 it has
  a Metal backend (`src/runtime/rhi/metal/`), but it was withdrawn from
  `settings.json` and from the menu on 2026-09-05 and is now reachable only
  through `ICORECOMP_GS_BACKEND`, for the replay tool and CI. Nothing about
  the Metal backend has run; see "The Metal backend" below and the section
  of the same name in docs/GS_RENDERER.md.
- The package is an app bundle, `ICO Recomp.app`, signed ad-hoc.
- The bundle is self-contained. A user downloads it and their own disc image
  and needs nothing else: no Vulkan SDK, no Homebrew. "MoltenVK in the
  bundle" below is how that is arranged and what it costs.

## What is verified, and by what

CI job `runtime-macos-arm64` (macos-14, `continue-on-error: true`) builds
the `macos-arm64-stub` preset with the RmlUi UI and runs every selftest that
needs no disc image:

    icorecomp-settings-selftest      icorecomp-log-selftest
    icorecomp-mc-selftest            icorecomp-menu-nav-selftest
    icorecomp-snd-pcm-selftest       icorecomp-stick-shape-selftest
    icorecomp-mouse-look-selftest    icorecomp-ipu-selftest
    icorecomp-vif1-selftest          icorecomp-gsring-selftest
    icorecomp-gif-widescreen-selftest icorecomp-video-mode-selftest
    icorecomp-osd-config-selftest    icorecomp-achievements-selftest

`icorecomp-title-logo-selftest` is excluded: it mounts the user's disc image,
which CI does not have.

The former CI job `runtime-macos-arm64-pgs` (removed 2026-09-05, never green;
steps in the workflow's history at commit 9096334) built the `macos-arm64-release` preset,
which is the full live renderer plus the clean-room native renderer and its
Metal backend, with Homebrew's MoltenVK handed to the configure. It then does
five things: checks the configure log for `icorecomp: Metal backend enabled`,
builds and runs the four disc-free GS selftests
(`icorecomp-gs-swizzle-selftest`, `icorecomp-gif-decode-selftest`,
`icorecomp-gs-raster-selftest`, `icorecomp-gs-texture-selftest`), runs
`icorecomp-gs-replay --probe`, installs the bundle and checks its layout, and
uploads the signed `.app` as a build artifact.

The layout check is the mechanical half of "self-contained": that
`Contents/MacOS` holds the launcher, the runtime binary, `libMoltenVK.dylib`
and the GS library, that `Contents/Resources/vulkan/icd.d/MoltenVK_icd.json`
is there and its `library_path` resolves to the dylib inside the bundle
rather than to the Homebrew prefix it was copied from, that
`CFBundleExecutable` names the launcher, and that `codesign --verify --deep
--strict` accepts the result.

The artifact is made with `ditto -c -k --sequesterRsrc --keepParent`, not
with `zip` and not by handing the `.app` directory to `upload-artifact`:
`ditto` keeps the mode bits, the extended attributes and the code signature,
and the artifact upload keeps none of them. A downloaded copy that had lost
the execute bit on `Contents/MacOS/ico` would fail to launch and would read
as a bug in the port.

Between them these prove: the runtime, the UI, the GS shared library and the
Metal backend's three Objective-C++ translation units compile and link under
Apple clang for arm64; the IPU, VIF1, GS ring, sound mixer, memory card,
settings, menu navigation, input and GS selftests produce the same answers on
arm64 that they produce on x86-64; and the install step lays out the bundle,
puts a driver in it and signs it.

Both macOS jobs are `continue-on-error` until their first green run, which is
the rule the lavapipe and WARP shader jobs follow. The flag comes off the
first time each passes.

## What is not verified

- **A rendered frame.** No frame from this build has been looked at by
  anyone. The renderer may come up and still be wrong.
- **Anything that needs the disc.** CI has no disc image, so the loader,
  the translated EE code and the whole boot path are untested here.
- **Denormal handling.** `set_fpu_ftz_daz` (`src/runtime/main.cpp`) sets
  FPCR.FZ and FPCR.FZ16 on arm64, which together cover what MXCSR FTZ and
  DAZ cover on x86. That the tier-0 FPU model then behaves identically is
  inferred from the architecture manuals, not measured. The measurement that
  would settle it is the per-op three-way test suite run on an arm64 host.
- **Whether headless rendering works on CI at all.** That is what the probe
  measures, and its result is the thing to read first in the job log.
- **Every run-time behaviour of the Metal backend.** The CI job proves it
  compiles and links, and nothing else. See below.
- **That the bundle launches.** CI checks the bundle's layout and that
  `codesign --verify` accepts it. Nothing here execs it. The launcher script
  below is the part most worth doubting: a bundle whose `CFBundleExecutable`
  is a shell script is signed into `Contents/_CodeSignature` rather than
  embedded, and whether Finder, Gatekeeper and the window server are all
  content with that has not been observed.

## MoltenVK in the bundle

The goal is that a user downloads `ICO Recomp.app`, supplies a disc image and
runs it, with nothing installed. That needs the Metal-backed Vulkan driver,
`libMoltenVK.dylib`, inside the bundle and actually reached by whichever
renderer the run selected. The three renderers take three different paths to
it:

| renderer (`ICORECOMP_GS_BACKEND`) | what opens Vulkan | what it needs from the bundle |
| --- | --- | --- |
| `parallel-gs` (and `auto`, so every shipped run) | Granite, `dlopen` | `GRANITE_VULKAN_LIBRARY` |
| `vulkan` | volk, `dlopen` | `DYLD_LIBRARY_PATH` |
| `metal` | nothing | nothing |

Only the first row is a player's path: there is no settings key for this any
more, so `auto` is what every shipped run resolves. The other two are
reached by setting `ICORECOMP_GS_BACKEND`, and the bundle is arranged for
all three so that a developer run of either does not need a Vulkan SDK.

`metal` is the easy case and the only one that needs no arrangement at all:
`src/runtime/rhi/metal/` talks to Metal directly and never loads a Vulkan
library. The other two both `dlopen` a **leaf name**:

    Granite   "libvulkan.1.dylib", then "libMoltenVK.dylib"
              (third_party/parallel-gs/Granite/vulkan/context.cpp:220-244)
    volk      "libvulkan.dylib", "libvulkan.1.dylib", then
              "libMoltenVK.dylib" (third_party/volk/volk.c)

dyld resolves a leaf name against `DYLD_LIBRARY_PATH`, then
`DYLD_FALLBACK_LIBRARY_PATH` (`$HOME/lib`, `/usr/local/lib`, `/lib`,
`/usr/lib`), then the working directory. It does **not** consult the calling
image's `LC_RPATH`, so the `@loader_path` the executable is linked with does
nothing here and a `libMoltenVK.dylib` sitting beside the executable is not
found. An app launched from Finder has `/` as its working directory. Homebrew
on Apple Silicon installs under `/opt/homebrew`, which is in none of those
lists either, which is why the CI probe step has always had to set
`DYLD_LIBRARY_PATH` by hand.

So the environment is the only thing that reaches those two calls, and it has
to be set before the process starts. That rules out both of the obvious
places:

- The runtime cannot do it. dyld reads `DYLD_LIBRARY_PATH` once, at launch; a
  `setenv` from inside `main` is too late.
- `Info.plist`'s `LSEnvironment` cannot do it. Its values are literal strings
  with no expansion of the bundle's own location, and the path wanted here is
  wherever the user put the app. An absolute path baked in at install time
  would be right on the build machine and wrong everywhere else, which is
  worse than nothing because it would read as a working setting.

What is left is a launcher. `packaging/macos-launch.sh.in` becomes
`Contents/MacOS/ico-launch`, and `CFBundleExecutable` names it. It works its
own directory out from `$0`, and then, without overriding anything the user
already set:

    DYLD_LIBRARY_PATH   <bundle>/Contents/MacOS, for volk and for anything
                        else opened by leaf name inside the bundle
    GRANITE_VULKAN_LIBRARY
                        the full path to the bundled dylib. Granite checks
                        this before it tries any leaf name and dlopens it
                        directly, so the paraLLEl-GS backend reaches MoltenVK
                        with no Vulkan loader in between
    VK_DRIVER_FILES     the bundled ICD manifest, for a machine that does
    VK_ICD_FILENAMES    have a loader installed. Without these two it would
                        enumerate its own drivers and never the one here.
                        VK_DRIVER_FILES is the current name and
                        VK_ICD_FILENAMES the one loaders before 1.3.207 read

then `exec`s `Contents/MacOS/ico`. The runtime is unchanged: `rt_exe_dir`
still reads `_NSGetExecutablePath`, which after the `exec` is the real
binary, so `ui/`, `saves/`, `settings.json`, the log and the disc image are
resolved exactly as before.

Three consequences worth stating plainly:

1. **No hardened runtime.** dyld ignores every `DYLD_` variable for a
   hardened process, so `codesign --options runtime` would silently undo the
   first line above. The ad-hoc signature CMake applies carries no such flag,
   and CMakeLists.txt says so where it signs.
2. **The runtime binary is signed on its own**, before the bundle. With a
   script as the main executable, `ico` is a nested Mach-O that the bundle
   pass would seal as a resource without signing, and the arm64 kernel
   refuses to exec an unsigned Mach-O.
3. **No validation layers on the paraLLEl-GS path.** Layers are the loader's
   job, and `GRANITE_VULKAN_LIBRARY` skips the loader. A developer who wants
   them installs the LunarG SDK and sets `GRANITE_VULKAN_LIBRARY` themselves;
   the launcher will not overwrite it.

The ICD manifest is copied from the same MoltenVK release as the dylib and
rewritten, not written from scratch: its `api_version` is a property of that
build of MoltenVK and inventing one would be substituting a plausible value
for a measured one. Only `library_path` is changed, to
`../../../MacOS/libMoltenVK.dylib`, which the loader resolves against the
manifest's own directory and so survives the user moving the app. If the
rewrite does not match, the configure fails rather than shipping a manifest
pointing at the build machine.

MoltenVK is Apache-2.0, so its `LICENSE` is installed as
`Contents/Resources/MoltenVK-LICENSE.txt`. CMake warns loudly if the dylib is
bundled without it.

None of this is installed by a build that bundles no MoltenVK. With
`ICORECOMP_MOLTENVK_DYLIB` empty there is no launcher, `CFBundleExecutable`
is the binary, and the bundle behaves exactly as it did before: the user
supplies their own Vulkan SDK or Homebrew `molten-vk`, and the dlopen
problem above is theirs.

## The Metal backend

`src/runtime/rhi/metal/` is a third backend for the clean-room native
renderer, beside Vulkan and D3D12. Objective-C++ with ARC, targeting the
macOS 14 SDK, requiring `MTLGPUFamilyApple7` or `MTLGPUFamilyMac2`, with the
shaders cross-compiled from the same SPIR-V the other two backends use.
docs/GS_RENDERER.md, section "The Metal backend", is the full account: the
argument index table, what each present mode really does, the shader path and
the list of what the macOS CI job settles.

**It ships uncompiled.** No macOS toolchain exists on the machines this
project is developed on, so not one line of it had been through a compiler
when it was written. The CI job's "check the Metal backend compiled in" step
is the first mechanical statement anyone gets about it, and a build failure
there is the expected first result, not a surprise.

`ICORECOMP_GS_BACKEND=metal` selects it (`src/runtime/gs/gs_select.cpp`).
There is no settings key: `display.backend` was retired on 2026-09-05 and
the menu offers no renderer choice. `auto` does not select it and will not
until the native renderer passes its parity gate. Running the dump
corpus through the Metal backend on a `macos-14` runner and comparing the
picture against the Vulkan reference is the step that would turn most of the
"not verified" list into a measurement. That is the same check WARP gives the
D3D12 backend on `windows-latest`, and this project cannot run it: the
runner's GPU access is exactly what the probe below is there to report on.

## The probe

    icorecomp-gs-replay --probe

Creates the headless Vulkan device the live backend would create, then
prints the physical device name, its Vulkan API version, the raw driver
version, and one PASS/FAIL line per requirement paraLLEl-GS makes of a
device (`third_party/parallel-gs/gs/gs_renderer.cpp:800-831`):
descriptorIndexing, timelineSemaphore, bufferDeviceAddress,
storageBuffer8BitAccess, storageBuffer16BitAccess, shaderInt16,
scalarBlockLayout, the arithmetic/shuffle/vote/ballot/basic subgroup
operations, subgroup size control across 4 to 64 invocations, and at least
32 KiB of compute shared memory.

Exit status is 0 whenever a device enumerated, whether or not it passed (a
device that fails is a report, not a tool failure), and 2 when none did, in
which case the only line printed is `no Vulkan device`.

The same result is available as a C struct, `RtPgsProbe`, through
`rt_pgs_probe` in `src/runtime/gs/gs_probe_api.h`. The runtime runs it once
at startup, before anything has opened a Vulkan device, logs the two lines
below at `info`, and shows the same two strings on the menu's
Display tab as read-only rows, `Renderer` and `Feature support`. A user who
cannot run a command line reads them there instead of running the tool.
`gs_probe_api.h` is a header of its own rather than part of
`gs_parallel_api.h` because the probe is the one part of the ABI a caller
uses without an `RtPgs` instance; the header says so itself.

## The three macOS-specific fixes in the runtime

Each of these is a place where the existing code would have made the wrong
decision on macOS, not a new feature:

1. **Backend default** (`src/runtime/gs/gs_select.cpp`). An unset
   `ICORECOMP_GS` used to mean "live backend" on Windows and "headless dump"
   everywhere else. A bundle launched from Finder has no environment, so
   macOS joins the Windows rule: unset means play in a window.
2. **Descriptor buffer** (`src/runtime/gs/gs_pgs_context.h`). MoltenVK does
   not implement `VK_EXT_descriptor_buffer`, so the context creation flag is
   cleared on macOS before the device is created rather than after a device
   turns out not to support it. `ICORECOMP_GS_NO_DESCBUF` still means what it
   means everywhere else.
3. **Display detection.** Window presentation used to be gated inside
   `gs_parallel_lib.cpp` on `DISPLAY` or `WAYLAND_DISPLAY`. Neither exists on
   macOS, so every run there would have gone headless. That guess is gone:
   the executable creates the window itself
   (`src/runtime/host/window_service.cpp`) and a run is windowed exactly when
   `SDL_CreateWindow` succeeded, on every platform.
   `ICORECOMP_GS_HEADLESS=1` is still how a headless run is asked for, and it
   is read in `gs/gs_select.cpp` before the window is created.

Two more changes are structural rather than fixes: `rt_exe_dir` and
`rt_exe_identity` read the executable's path through `_NSGetExecutablePath`
(there is no `/proc/self/exe`), and the per-user config and state
directories are both `~/Library/Application Support/icorecomp`. Both are in
`src/runtime/host/portable.h`.

Upstream already carries the rest: Granite dlopens `libvulkan.1.dylib` then
`libMoltenVK.dylib` under `__APPLE__` and enables
`VK_KHR_portability_enumeration`
(`third_party/parallel-gs/Granite/vulkan/context.cpp`), and paraLLEl-GS
disables hierarchical binning under `__APPLE__` because Metal drivers
mishandle it (`third_party/parallel-gs/gs/gs_renderer.cpp`). No local patch
file was needed.

## Building

    cmake --preset macos-arm64-release
    cmake --build --preset macos-arm64-release -j4
    cmake --install build/macos-arm64-release --prefix dist/macos

`macos-arm64-stub` is the same thing with the live renderer off, which is
what CI's cheap job builds.

MoltenVK is loaded at run time, not linked, so the build itself needs no
Vulkan SDK. A shipping build hands it three files, all from the same MoltenVK
release, which is what makes the package self-contained:

    cmake --preset macos-arm64-release \
        -DICORECOMP_MOLTENVK_DYLIB=$(brew --prefix molten-vk)/lib/libMoltenVK.dylib \
        -DICORECOMP_MOLTENVK_ICD=$(brew --prefix molten-vk)/share/vulkan/icd.d/MoltenVK_icd.json \
        -DICORECOMP_MOLTENVK_LICENSE=$(brew --prefix molten-vk)/LICENSE

`ICORECOMP_MOLTENVK_ICD` defaults to whatever sits beside the dylib or one
directory up under `share/vulkan/icd.d`, so on a normal Homebrew or LunarG
layout only the first is needed. Setting the dylib is also what installs the
launcher and switches `CFBundleExecutable` to it; see "MoltenVK in the
bundle". Leave all three empty and the bundle is the old one, which needs the
user to have a Vulkan SDK or Homebrew `molten-vk` of their own. CI finds the
three by `find` under the Homebrew prefix rather than hardcoding the layout,
and fails the job if any is missing.

`-DICORECOMP_DISC=<image>` and `--target icon` render the bundle icon from
the save icon on the user's own disc, exactly as the Windows `.ico` is
rendered. It is written as `icorecomp.icns` in the build directory and
installed into `Contents/Resources`. Like every other file rendered from the
disc, it is never committed, and two things keep it out: the extension gate
in `tools/check_no_rom.sh` lists `.icns` alongside `.ico`, and
`icorecomp-icon-extract` refuses to write under the source root.

## The bundle layout

    ICO Recomp.app/Contents/Info.plist
    ICO Recomp.app/Contents/Resources/icorecomp.icns
    ICO Recomp.app/Contents/Resources/MoltenVK-LICENSE.txt
    ICO Recomp.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json
    ICO Recomp.app/Contents/MacOS/ico-launch
    ICO Recomp.app/Contents/MacOS/ico
    ICO Recomp.app/Contents/MacOS/libMoltenVK.dylib
    ICO Recomp.app/Contents/MacOS/libicorecomp-parallel-gs.dylib
    ICO Recomp.app/Contents/MacOS/libSDL3.*.dylib
    ICO Recomp.app/Contents/MacOS/ui/

`Contents/Resources` holds the three files nothing at run time resolves
against `rt_base_dir`: the icon Finder reads, the ICD manifest the Vulkan
loader reads, and a license. Everything else is flat in `Contents/MacOS`.

Flat inside `Contents/MacOS` on purpose. The runtime resolves `ui/`,
`saves/`, `screenshots/`, `settings.json`, the log and the disc image
against the executable's own directory (`rt_base_dir`), so putting the
executable anywhere else in the bundle would break all of them at once. The
executable's install rpath is `@loader_path`, the Mach-O spelling of the
`$ORIGIN` the Linux package uses.

Writing into the bundle breaks its seal, and that is by design here rather
than an oversight. The disc image goes into `Contents/MacOS`, and the run
writes `settings.json`, `saves/`, `screenshots/` and `icorecomp.log` there
too, so the bundle's contents stop matching the signature `cmake --install`
applied and CI verifies. It does not matter for an ad-hoc signature: the
arm64 kernel checks the signature of the Mach-O it executes, which nothing
here rewrites, not the seal over the whole directory. It would matter for a
Developer ID signature with notarisation, where a broken seal is a launch
refusal. If this port is ever notarised, the disc image and the writable
state have to move out of the bundle first, to `~/Library/Application
Support` or a folder the user picks.

The bundle is signed ad-hoc (`codesign --force --sign -`), dylibs first and
the bundle last, because signing the bundle seals what its contents are.
This is not a distribution nicety: the arm64 kernel refuses to execute an
unsigned Mach-O at all. It is not a Developer ID signature and it is not
notarised, so Gatekeeper still refuses a downloaded copy on a double click;
the packaged README says what a user does about that.

## What a user needs

Two things, and nothing else: `ICO Recomp.app` and their own ICO PAL disc
image. No Vulkan SDK, no Homebrew, no runtime to install. Apple Silicon,
macOS 14 or newer.

1. Right-click the app, choose Show Package Contents, and put the image in
   `Contents/MacOS` as `ico.iso` (a raw dump works as `ico.bin`).
2. Clear the quarantine flag, from a terminal in the folder holding the app:

       xattr -dr com.apple.quarantine "ICO Recomp.app"

   The bundle is ad-hoc signed, not Developer ID signed and not notarised,
   so Gatekeeper refuses a downloaded copy on a double click. Right-click and
   Open gets past that dialog, but the quarantine flag has a second effect
   that matters more here: macOS runs a quarantined app from a read-only copy
   at a path of its own choosing, so `settings.json`, `saves/` and the log go
   to `~/Library/Application Support/icorecomp` instead of `Contents/MacOS`,
   and the disc image placed in step 1 may not be found at all. Clearing the
   flag puts all of it back beside the executable.
3. Double-click the app.

There is no renderer setting to try. The renderer is paraLLEl-GS over the
bundled MoltenVK; `display.backend` was retired on 2026-09-05 and the menu's
Display tab offers no choice, so a `backend` key left in a `settings.json`
is named once at info as no longer read and otherwise ignored.
`ICORECOMP_GS_BACKEND` still resolves one for a developer run: on macOS
`parallel-gs` (what `auto` resolves to), `vulkan` (the clean-room renderer
over the bundled MoltenVK) and `metal` (the clean-room renderer straight on
Metal). The last two have never rendered a frame on a Mac.

The log is `icorecomp.log` beside the executable, in `Contents/MacOS`, or
under `~/Library/Application Support/icorecomp` if that folder cannot be
written. Its first line names the file it ended up in.

## Reporting back

The port writes `icorecomp.log` next to the executable, or under
`~/Library/Application Support/icorecomp` when the bundle is quarantined and
its own folder is not writable. The lines worth sending, in order:

- The `main` line naming the executable and its FPCR value. On arm64 it
  reads `FPCR FZ+FZ16 set`; that line existing at all says the arm64 branch
  compiled in rather than the unknown-architecture warning.
- The `gs` line saying which backend an unset `ICORECOMP_GS` selected, and
  the `GS backend:` line saying which live renderer the run resolved to, and
  from where.
- The `Renderer:` and `Feature support:` lines. These describe the device the
  backend this run actually created: the device with its driver and API
  version, and either `all required features present` or the list of
  requirements it missed. They are also the two read-only rows at the bottom
  of the menu's Display tab (open the menu with F1), so a report can
  be a photograph of that pane rather than a log file.
- The `window created for` line, which says the executable opened a window
  and for which backend, or the `SDL_Init(VIDEO) failed` /
  `SDL_CreateWindow failed` line that says why it did not.
- The `paraLLEl-GS` line naming the device the renderer came up on, or the
  fatal saying no Vulkan loader or no usable device was found.
- The full output of `icorecomp-gs-replay --probe`, run from
  `Contents/MacOS`. On a machine where the game does not render, that output
  is the difference between "this Mac cannot run the renderer" and "the port
  is wrong".
