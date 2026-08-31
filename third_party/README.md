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

## patches

Patch files applied to submodules at configure time. Currently empty.
