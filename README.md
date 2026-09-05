# ico-recomp

Static recompilation of ICO (PlayStation 2) to a native executable. The
approach follows [N64Recomp](https://github.com/N64Recomp/N64Recomp): every EE
(R5900) instruction in the game binary is translated to C ahead of time,
compiled for the host, and linked against a runtime that implements the PS2
hardware the code expects (DMAC, VIF1, GS, kernel and IOP services as HLE).
The five VU1 microprograms are statically recompiled as well.

One build serves one retail disc, because the generated C is a translation of
one ELF: the PAL disc (SCES-50760), boot ELF `SCES_507.60`. `docs/TARGET.md`
has the target's facts, its inputs and how each was measured.

Two GS renderers are in the tree. The shipped one is
[paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), a shared
library. The other is this repository's own clean-room renderer under
`src/runtime/gs/render`, with a Vulkan and a D3D12 backend over the RHI in
`src/runtime/rhi`. It has not passed the parity gate in `docs/GS_RENDERER.md`
and is unverified against the game, so the automatic choice never lands on it:
a run takes paraLLEl-GS unless `ICORECOMP_GS_BACKEND` names one of the others
by hand. There is no settings key for it.

Platforms: Linux and Windows are built in CI. macOS builds and packages as an
app bundle (`docs/MACOS.md`), on CI jobs that are informational, because
nobody on the project has the hardware to run the result.

Status: pre-alpha. On the shipped paraLLEl-GS backend the PAL disc boots,
draws and plays with sound, measured on 2026-09-04. Nothing has been played
through, and the clean-room renderer has not drawn a game frame.

## No game data

- No game assets, code, symbols, or ROM-derived bytes are committed. This
  includes the generated C output, which is produced locally at build time and
  gitignored.
- Building and running requires a legally acquired copy of the PAL release of
  ICO. The boot ELF is SHA-1 gated against the pin, so a disc from another
  region is refused rather than mistranslated.
- `tools/check_no_rom.sh` runs as a pre-commit hook and in CI to enforce this.

## What the translator reads

Two files, both off your own disc, both named by `[inputs]` in
`config/recomp.toml`:

- `SCES_507.60`, the retail boot ELF. Its own instructions prove where the
  functions are: the targets of calls, the addresses code and data hold
  pointers to, and the shape of a prologue after an unconditional transfer.
- `SRCFILE.TXT`, an objdump listing of a late development build that the disc
  still carries. It is a different link, so its addresses are not this ELF's
  except where the two happen to coincide; its function bodies are
  transplanted onto the retail `.text` by masked-instruction fingerprint,
  which is what carries the names, the source files and the boundaries the
  ELF alone cannot prove.

`setup.sh` copies both out of the disc image you give it. Nothing else is an
input. In particular the ICO matching-decomp project is a separate project
and not an input to this one: it may be read as a behavioural reference, the
way PCSX2 is, and nothing from it is copied or configured here.
`docs/TARGET.md` has the detail, including what each rule is measured to
find.

## Building

You need your own PAL ICO disc image (SCES-50760). Then:

    ./setup.sh /path/to/Ico_PAL.iso

extracts the two input files into `baserom/pal/` (gitignored), builds the
translator, translates the game locally, builds the runtime, and prints how
to run. Windows: see `docs/WINDOWS.md`. macOS: see `docs/MACOS.md`.
The target's own facts and inputs: see `docs/TARGET.md`.

Clone with submodules (`git clone --recursive`, or `git submodule update
--init --recursive`): RmlUi and FreeType for the menu, SDL for the window,
input and audio, volk and Vulkan-Headers for the native renderer, and
paraLLEl-GS for the shipped one. Regenerating the native renderer's shaders
also needs glslang and SPIRV-Cross on the machine, but the results are
committed, so an ordinary build does not.

Input is keyboard, gamepad, or keyboard and mouse: mouse look drives the
camera stick and the mouse can point and click on the game's own menus.
Every binding and the mouse settings live in the in-game menu (F1) and in
`settings.json`; see `docs/SETTINGS.md`.

## Layout

- `include/`: ABI contract between generated code, the reference interpreter,
  and the runtime (`recomp_context.h`, `recomp_api.h`, `recomp_ops.h`)
- `tools/recomp/`: the translator. Rust workspace with ELF ingest, R5900/VU
  decoders, C emitters, and a reference interpreter
- `src/runtime/`: runtime and port shell (kernel HLE, DMAC/VIF1, GS backend,
  IOP services, host layer)
- `src/runtime/gs/render/`: the clean-room GS renderer (`docs/GS_RENDERER.md`)
- `src/runtime/rhi/`: the Vulkan and D3D12 backends it draws through
- `ui/`: the menu, launcher and credits documents, their stylesheets and fonts
- `third_party/`: vendored dependencies, one section each in
  `third_party/README.md`

## License

MIT for everything in this repository, including the renderer under
`src/runtime/gs/render`, which is this project's own clean-room code and must
not copy or port paraLLEl-GS or Granite. `third_party/` dependencies keep
their own licenses, recorded per dependency in `third_party/README.md`:

- paraLLEl-GS: LGPL-3.0-or-later, kept as a shared library
- Granite, minicoro, RmlUi, volk: MIT
- Vulkan-Headers: Apache-2.0 OR MIT, this repository electing MIT
- SDL: Zlib
- FreeType: FreeType License or GPLv2, this repository electing the FreeType
  License
- The two fonts under `ui/fonts`: SIL Open Font License 1.1
