# ico-recomp

Static recompilation of ICO (PS2, retail US, `SCUS_971.13`) to a native PC
port. Full design lives in the approved plan (translator + runtime, milestones
P0-P8).

## Hard rules

- No ROM-derived data is ever committed. This includes generated C
  (`generated/`), symbol lists copied from the decomp repo, disassembly, game
  bytes, and content hashes beyond the two ELF/ROM SHA-1 pins.
  `tools/check_no_rom.sh` runs as a pre-commit hook; it is a mechanical gate.
  `config/vendor_names.txt` (addr to Sony SDK name facts) is the one approved
  exception.
- The decomp repo `../ico` is read-only input. Paths and SHA-1 pins live in
  `config/recomp.toml`. Never copy its configs or asm into this repo.
- License hygiene: this repo is MIT. GPL code (PCSX2, ran-j/PS2Recomp) may be
  read as behavioral reference only, never copied or ported line by line.
  paraLLEl-GS is LGPLv3+ and must remain a shared library, pinned as a
  submodule, with local patches carried as patch files.
- Prose style, everywhere (docs, comments, commit messages): plain technical
  writing. No em-dashes. No marketing adjectives.

## ABI contract

`include/recomp_context.h`, `include/recomp_api.h`, and `include/recomp_ops.h`
are the contract between the translator's generated code, the reference
interpreter, and the runtime. Generated code and the interpreter must use the
same `recomp_ops.h` helpers; the three-way verification depends on it.
Changing these headers invalidates generated code. Do it deliberately.

## Translator (tools/recomp, Rust workspace)

- Coverage policy: the decoder is total, but the emitter hard-errors on any
  mnemonic not in the measured census of this one binary. No speculative ops.
- Verification order: `verify-decode` (our disassembly diffed against the
  decomp repo's asm baselines) must stay green. Per-op three-way tests
  (interpreter vs compiled emit) must agree exactly.

## Runtime (src/runtime)

- HLE boundary: vendor EE code runs translated. The kernel is HLE'd at the
  `syscall` instruction. The IOP is HLE'd at the SIF RPC layer. DMA is
  synchronous; completion interrupts are delivered deferred, never inside the
  CHCR write.
- Loud failure beats silent wrongness: unknown syscall numbers, unknown VU1
  upload hashes, and unmapped MMIO are fatal logs with a state dump.
