# ico-recomp

Static recompilation of ICO (PlayStation 2, retail US, boot ELF `SCUS_971.13`)
to a native PC executable. The approach follows
[N64Recomp](https://github.com/N64Recomp/N64Recomp): every EE (R5900)
instruction in the game binary is translated to C ahead of time, compiled for
the host, and linked against a runtime that implements the PS2 hardware the
code expects (DMAC, VIF1, GS via
[paraLLEl-GS](https://github.com/Arntzen-Software/parallel-gs), kernel and IOP
services as HLE). The five VU1 microprograms are statically recompiled as well.

Status: pre-alpha. Nothing is playable yet.

## No game data

- No game assets, code, symbols, or ROM-derived bytes are committed. This
  includes the generated C output, which is produced locally at build time and
  gitignored.
- Building and running requires a legally acquired copy of ICO (USA). Other
  regions are unsupported; the ELF is SHA-1 gated.
- `tools/check_no_rom.sh` runs as a pre-commit hook and in CI to enforce this.

## Relationship to the ICO decompilation

Translation inputs (function map, translation-unit boundaries, VU1 microcode
sources) come from a sibling checkout of the ICO matching-decomp project,
consumed read-only. Paths are configured in `config/recomp.toml`. Nothing from
that checkout is copied into this repository.

## Building

You need your own ICO (USA) disc image and a sibling checkout of the ICO
decomp project (`../ico`) with its base ELF extracted. Then:

    ./setup.sh /path/to/Ico_USA.iso

builds the translator, translates the game locally, builds the runtime, and
prints how to run. Windows: see `docs/WINDOWS.md`.

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
- `third_party/parallel-gs/`: GS renderer (LGPLv3+, built as a shared library)

## License

MIT for everything in this repository. `third_party/` dependencies keep their
own licenses (paraLLEl-GS: LGPLv3+; Granite, minicoro: MIT).
