# third_party

Vendored dependencies. Licensing rules for this directory are set in the
top-level CLAUDE.md; the short version: this repo is MIT, so anything that is
not MIT-compatible must stay isolated and clearly labeled.

## minicoro

Single-header stackful coroutine library (public domain / MIT-0). Used by the
EE scheduler (src/runtime/ee/sched.cpp).

## SDL

SDL3, the window, input and audio library. The executable owns the one window
of the run (`src/runtime/host/window_service.cpp`) and hands it to whichever
GS backend presents into it.

- Upstream: https://github.com/libsdl-org/SDL
- Pinned commit: 8586f63d2a900303701676a253a3e18593a6a6ae
  (release-3.4.0-1161-g8586f63d2) as a git submodule. That is the same commit
  Granite vendors as `parallel-gs/Granite/third_party/sdl3`, deliberately: a
  tree with both submodules compiles one SDL rather than two revisions of it.
  Granite's copy is the fallback when `third_party/SDL` is not checked out.
- License: Zlib (see SDL/LICENSE.txt), which is MIT-compatible.

How it is built:

- As a shared library (`SDL_SHARED`, `SDL_STATIC` off), because the window is
  created in the MIT executable and `libicorecomp-parallel-gs` presents into a
  surface made from it: the two must be the same SDL instance. The .so or DLL
  ships beside the executable.
- Subsystems this port does not use are turned off at configure time
  (CMakeLists.txt); audio, dialog, filesystem and misc stay on, for
  `host/audio.cpp` and the launcher's disc picker.
- Compiled unmodified. No patches are carried.

## volk

The Vulkan meta-loader: it resolves every Vulkan entry point at run time
instead of linking an import library. The clean-room renderer's Vulkan RHI
(`src/runtime/rhi`) is built on it, and on macOS it is what finds the
loader that MoltenVK sits behind.

- Upstream: https://github.com/zeux/volk
- Pinned commit: 776893306c5d3b22b6185b5d4a258b81d94572bf
  (tag `vulkan-sdk-1.4.357.0`, 2026-07-18) as a git submodule, matched to
  the Vulkan-Headers pin below: volk generates its loader from that
  version's registry, so the two move together.
- License: MIT (see volk/LICENSE.md), Copyright (c) 2018-2026 Arseny
  Kapoulkine.

How it is built:

- Compiled unmodified as a static library, `icorecomp-volk`, into the `ico`
  executable and into `icorecomp-gs-replay` (CMakeLists.txt). Built with
  hidden visibility, because `libicorecomp-parallel-gs` carries Granite's
  own copy of volk and two copies with default visibility would resolve to
  one set of function pointers across the shared-library boundary.
- No patches are carried.

## Vulkan-Headers

The Khronos Vulkan headers volk and the RHI compile against, so the build
does not depend on a Vulkan SDK being installed on the machine.

- Upstream: https://github.com/KhronosGroup/Vulkan-Headers
- Pinned commit: e3b1eec08173d6b825cd3ac88c885a63b621504a (tag `v1.4.357`,
  "Update for Vulkan-Docs 1.4.357", 2026-07-17) as a git submodule.
- License: Apache-2.0 OR MIT, at the user's choice, as the files' own
  SPDX identifiers state (see Vulkan-Headers/LICENSE.md and
  Vulkan-Headers/LICENSES/). This repo elects MIT, which is the
  MIT-compatible half; the Apache-2.0 half is stated here because it is
  the only non-MIT, non-Zlib license in the new set and so the one most
  worth recording.
- Headers only: nothing is compiled from this submodule and nothing from it
  ships in the package. It contributes an include directory to
  `icorecomp-volk` and to the RHI.
- No patches are carried.

## glslang and SPIRV-Cross (not submodules)

Neither is vendored. `tools/gen_gs_shaders.sh` uses them to compile the
renderer's GLSL compute shaders to SPIR-V and, for the D3D12 backend, to
cross-compile that SPIR-V to HLSL. The results are committed, so a normal
build needs neither tool; they are prerequisites for regenerating shaders
only. See docs/GS_RENDERER.md.

## parallel-gs

paraLLEl-GS, a PlayStation 2 Graphics Synthesizer implementation on Vulkan
compute, by Arntzen Software AS.

- Upstream: https://github.com/Arntzen-Software/parallel-gs
- Pinned commit: 65229b0d0b2e0755df4f3c907b364897126d0bec ("Rebase PCSX2
  integration on v2.8.0", 2026-08-29) as a git submodule, with its Granite
  submodule initialized recursively.
- License: LGPL-3.0-or-later (see parallel-gs/COPYING.LGPLv3). Granite and
  its third_party components carry their own licenses (MIT and similar).

LGPL compliance in this build:

- parallel-gs is compiled unmodified (PARALLEL_GS_STANDALONE mode) into a
  single shared library, `libicorecomp-parallel-gs.so`, together with the
  Granite pieces it needs. The MIT runtime executable links that shared
  library dynamically; the LGPL boundary is the .so, which a user can rebuild
  or replace independently of the runtime.
- No parallel-gs or Granite sources are edited in-tree. If local changes are
  ever required they are carried as patch files under third_party/patches/
  and applied at configure time. No patches are currently carried.
- GPL projects (PCSX2, ran-j/PS2Recomp) are behavioral reference only and no
  code from them is present here.

## rmlui

RmlUi, the HTML/CSS-style user interface library, used for the settings menu,
the launcher and the credits documents under `ui/`.

- Upstream: https://github.com/mikke89/RmlUi
- Pinned commit: ba95ffe8bfb6370efb2cdcca927eaad4710c5413 (tag 6.3, "RmlUi
  release 6.3", 2026-08-22) as a git submodule.
- License: MIT (see rmlui/LICENSE.txt). Copyright (c) 2008-2014 CodePoint
  Ltd, Shift Technology Ltd, and contributors; Copyright (c) 2019-2026 The
  RmlUi Team, and contributors.

How it is built and used:

- Compiled unmodified as a static library (`RmlUi::Core` only; the debugger
  target is EXCLUDE_FROM_ALL and nothing links it) into the `ico` executable
  (CMake target `icorecomp-runtime`), which is MIT. Samples, Lua bindings,
  the SVG and Lottie plugins and Tracy profiling are all off
  (CMakeLists.txt).
- The library never sees Vulkan. `src/runtime/ui/ui_render.cpp` implements
  `Rml::RenderInterface` over the overlay ABI in
  `src/runtime/gs/gs_parallel_api.h`, which is plain POD: vertices, indices,
  draw commands and texture ids. That POD boundary is the only thing that
  crosses into the LGPL shared library.
- `src/runtime/ui/ui_events.cpp` carries the SDL3 key, mouse and modifier
  mapping ported from RmlUi 6.3's `Backends/RmlUi_Platform_SDL.cpp`, with the
  copyright notice repeated at the top of that file.

## freetype

FreeType 2, the font rasterizer RmlUi's default font engine uses.

- Upstream: https://github.com/freetype/freetype
- Pinned commit: 42608f77f20749dd6ddc9e0536788eaad70ea4b5 (tag VER-2-13-3,
  version 2.13.3, 2024-08-11) as a git submodule.
- License: dual, the FreeType License (docs/FTL.TXT) or GPLv2 (docs/GPLv2.TXT),
  the choice being the user's. This repo elects the FreeType License, which is
  the MIT-compatible half; no GPLv2 terms apply to this build.
- The FTL requires a credit in the documentation of a binary distribution.
  packaging/README.txt.in carries it.

How it is built:

- Compiled unmodified as a static library into the `ico` executable (CMake
  target `icorecomp-runtime`), with every optional dependency turned off:
  `FT_DISABLE_ZLIB`, `FT_DISABLE_BZIP2`, `FT_DISABLE_PNG`,
  `FT_DISABLE_HARFBUZZ` and `FT_DISABLE_BROTLI` (CMakeLists.txt). The port
  ships two TrueType fonts and needs nothing else, so the build has no
  external font dependencies.
- `SKIP_INSTALL_ALL` keeps FreeType out of the packaged folder; only the
  linked objects inside the executable ship.

## ui/fonts (not in this directory)

The two fonts under `ui/fonts` are not vendored source but are third-party
assets and are licensed under the SIL Open Font License, Version 1.1. Their
notices ship beside them and are installed with the package:

- JetBrains Mono (`JetBrainsMono-Regular.ttf`, notice in
  `JetBrainsMono-OFL.txt`), Copyright 2020 The JetBrains Mono Project Authors.
- Playfair Display (`PlayfairDisplay[wght].ttf`, notice in
  `PlayfairDisplay-OFL.txt`), Copyright 2017 The Playfair Display Project
  Authors, with Reserved Font Name "Playfair Display".

## DirectX Shader Compiler (not vendored, not committed)

The D3D12 backend's shaders are HLSL cross-compiled from this project's own
GLSL. They reach the GPU as DXIL, which a driver rejects unless the container
carries a signature.

Nothing from the DirectX Shader Compiler is committed here. Two copies of one
release are cached outside the tree, and `.cache/` is in `.gitignore`:

- `.cache/dxc/bin/x64/dxcompiler.dll` and `dxil.dll`, from the Windows zip of
  release **v1.9.2607** (`dxc_2026_07_29.zip`,
  <https://github.com/microsoft/DirectXShaderCompiler/releases/tag/v1.9.2607>).
  CMake's `ICORECOMP_DXC_DIR` installs the pair beside `ico.exe` as the
  run-time fallback for a build with no compiled-in DXIL.
- `.cache/dxc-linux/`, from `linux_dxc_2026_07_29.x86_x64.tar.gz` of the same
  release, holding `bin/dxc`, `lib/libdxcompiler.so` and `lib/libdxil.so`.
  `tools/gen_gs_shaders_dxil.sh` uses it to produce
  `src/runtime/rhi/rhi_shaders_dxil.h` on Linux; the containers it writes are
  signed (the script checks the container hash and fails if it is zero).

Licences, both carried in the release archives as `LICENSE-LLVM.txt` and
`LICENSE-MS.txt`:

- `dxcompiler.dll` / `libdxcompiler.so` is the University of Illinois/NCSA
  Open Source Licence with the LLVM exceptions. Permissive, redistributable,
  attribution in the binary distribution.
- `dxil.dll` / `libdxil.so` is Microsoft's, redistributable under the terms in
  `LICENSE-MS.txt` (the DirectX Shader Compiler binary licence, which permits
  distributing it with an application).

Both Windows DLLs import `MSVCP140.dll`, `VCRUNTIME140.dll` and
`VCRUNTIME140_1.dll` (measured with `objdump -p`), so a machine without the
Visual C++ 2015-2022 redistributable fails to load them with
`ERROR_MOD_NOT_FOUND`. That is the reason the compiled-in DXIL is the shipping
path and the DLLs are only a fallback.

## MoltenVK (not vendored, not committed, redistributed in the macOS package)

Khronos' implementation of Vulkan on Metal. Nothing from it is in this tree
and nothing links against it: it is loaded at run time by `dlopen`, so the
macOS build needs no Vulkan SDK. It is redistributed all the same, because a
self-contained `ICO Recomp.app` has to carry a driver.

Three files from one MoltenVK release are copied into the bundle, by the
configure variables `ICORECOMP_MOLTENVK_DYLIB`, `ICORECOMP_MOLTENVK_ICD` and
`ICORECOMP_MOLTENVK_LICENSE`:

- `Contents/MacOS/libMoltenVK.dylib`, the driver.
- `Contents/Resources/vulkan/icd.d/MoltenVK_icd.json`, the release's own ICD
  manifest with only its `library_path` rewritten to point back into the
  bundle. Its `api_version` is left as the release wrote it.
- `Contents/Resources/MoltenVK-LICENSE.txt`.

- Upstream: <https://github.com/KhronosGroup/MoltenVK>
- Not pinned in this repository. CI takes Homebrew's `molten-vk` and records
  its version in the job log, so the artifact of any run is traceable to the
  MoltenVK that went into it. Pinning means downloading the
  `MoltenVK-macos.tar` asset of a tagged release and pointing the same three
  variables at what it unpacks.
- License: Apache-2.0, which is MIT-compatible and requires the license text
  to travel with a redistributed binary. That is what the third file is for,
  and CMake warns loudly when the dylib is bundled without it.

docs/MACOS.md, "MoltenVK in the bundle", is why a launcher script is needed
before the copy in the bundle is the one that gets loaded.

## patches

Patch files applied to submodules at configure time. Currently empty.
