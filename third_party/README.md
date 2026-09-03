# third_party

Vendored dependencies. Licensing rules for this directory are set in the
top-level CLAUDE.md; the short version: this repo is MIT, so anything that is
not MIT-compatible must stay isolated and clearly labeled.

## minicoro

Single-header stackful coroutine library (public domain / MIT-0). Used by the
EE scheduler (src/runtime/ee/sched.cpp).

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

## patches

Patch files applied to submodules at configure time. Currently empty.
