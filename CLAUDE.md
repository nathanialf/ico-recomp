# ico-recomp

Static recompilation of ICO (PS2, PAL retail, `SCES_507.60`) to a native PC
port. Full design lives in the approved plan (translator + runtime, milestones
P0-P8).

## Hard rules

- No ROM-derived data is ever committed. This includes generated C
  (`generated/`), symbol lists copied from the decomp repo, disassembly, game
  bytes, and content hashes beyond the boot ELF SHA-1 pin.
  `tools/check_no_rom.sh` runs as a pre-commit hook; it is a mechanical gate.
  There are two approved exceptions, both address facts only:
  `src/runtime/guest/ico_syms.h` (the guest data addresses the runtime reads
  and writes for the mouse pointer on the game's menus, the one projection
  float widescreen scales, the composer entry address the translator's entry
  hook is keyed on, the progress-bit indices the achievement observer
  watches, and the vendor sound library's slot, sequence, vab and .hd
  offsets that the zero-level key-on diagnostic reads), and
  `config/entry_hooks.txt` (one guest function entry address per line, the
  name the disc's own listing gives it, and a one-line reason).
  Both carry addresses and names, plus the offsets and lengths inside a named
  guest object that the runtime itself reads, and nothing else: no
  instruction sequences, no disassembly, no field map of a structure nothing
  here reads. That floor applies to every other file in the tree as well
  (`src/runtime/target.h` is the other one that names addresses); the two
  are exceptions to the no-symbol-list rule, not to the no-disassembly rule.
  `tools/check_no_rom.sh` names the same two files and enforces the
  no-disassembly half mechanically. `config/vendor_names.txt` was a third
  until 2026-09-05: nothing read it, and the only tool that produced it read
  the decomp's US build, so both were removed.
  Where an address was measured rather than named, the file points at where
  the measurement lives outside this repo instead of reproducing it.
- The decomp repo (`/primary/dev/ico`) is a separate project and not an
  input to this one (user ruling, 2026-09-05). Function boundaries, names
  and jump tables come from the retail ELF's own entry proofs and from the
  disc's objdump listing `SRCFILE.TXT` correlated onto it
  (`config/recomp.toml` `[inputs]`). The disc also carries `MAIN.MAP`, which
  is the map of a different link and is read by nothing here. The decomp may
  be read as a behavioural reference the way PCSX2 is; there is no `[decomp]`
  table and the translator no longer has a code path for one, never copy its
  configs or asm.
- License hygiene: this repo is MIT. GPL code (PCSX2, ran-j/PS2Recomp) may be
  read as behavioral reference only, never copied or ported line by line.
  paraLLEl-GS (LGPLv3+) is a behavioral reference only, like PCSX2. While
  it is still built it stays a shared library pinned as a submodule with
  local patches carried as patch files; the GS renderer under
  `src/runtime/gs/render` is this repo's own clean-room code and must not
  copy or port paraLLEl-GS or Granite. Once the native renderer passes the
  parity gate in docs/GS_RENDERER.md, the submodule and patches are
  removed.
- Prose style, everywhere (docs, comments, commit messages): plain technical
  writing. No em-dashes. No marketing adjectives.
- Accuracy decides design questions. When there is a choice between what the
  hardware and the retail game actually do and what is convenient, safe or
  tidy for the port, reproduce the hardware. Clamping, truncating, rounding
  or defaulting a value the game supplied is a divergence, not a safety net:
  guard it with a loud log or a fatal, never by quietly changing it. Where
  accuracy is not yet known, say so in the code rather than substituting a
  plausible value.
- The binary is the authority. Measure the retail ELF and the disc before
  proposing a cause; the decomp may corroborate, never decide. State plainly
  which claims are measured and which are inferred, and prefer one decisive
  measurement over several plausible fixes.

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
  disc's own objdump listing) must stay green. It decodes the words the
  listing prints, which is a different link, so it verifies the decoder
  against binutils and not the retail ELF's own bytes; docs/TARGET.md says
  what that does and does not cover. Per-op three-way tests (interpreter vs
  compiled emit) must agree exactly.

## Runtime (src/runtime)

- HLE boundary: vendor EE code runs translated. The kernel is HLE'd at the
  `syscall` instruction. The IOP is HLE'd at the SIF RPC layer. DMA is
  synchronous; completion interrupts are delivered deferred, never inside the
  CHCR write.
- Loud failure beats silent wrongness: unknown syscall numbers, unknown VU1
  upload hashes, and unmapped MMIO are fatal logs with a state dump.

## Settings and UI

- Host-side only: no setting alters a value the game supplied, with two
  documented exceptions. The first: `display.raster = window` (the default)
  reads the display buffer from its origin, ignoring the DISPFB DBX/DBY
  offset, so the attract movie's whole picture is shown; `crt` keeps
  the hardware read offset and crop. See docs/SETTINGS.md section 6.
  Game speed is out of scope by decision, not by omission. Widescreen is in
  scope by the user's decision on 2026-09-03. It reads the game's own
  projection block at `0x0067BA60` and writes one float in it, the X scale at
  `+0x04`, multiplying it by `(4/3)` over the window's aspect so the game's
  own matrix composer builds a wider frustum from the camera's own field of
  view. Nothing else in that block is touched, no other guest memory is
  written, and no guest code is patched: the translator emits a call to a
  runtime hook at the composer's entry, which is an address fact. 2D keeps
  its 4:3 geometry: the runtime scales sprite X coordinates about the GS 2048
  centre by the same factor in `src/runtime/hw/gif.cpp`, on the host side of
  guest memory, and leaves full-frame passes covering the whole frame. A 2D
  pass the transform judges inside-frame therefore keeps its 4:3 geometry,
  and one it judges full-frame is presented at the target aspect, because
  the scanout aspect follows the setting for every field and nothing at the
  scanout can tell a 3D field from the attract movie, a menu page or a fade.
  Which of those the attract movie, the game's own menus and the fades take
  is not yet measured: the classification rule in `src/runtime/hw/gif.cpp`
  is provisional and is decided by the GS dumps named in docs/SETTINGS.md
  section 6. Achievements are read-only: the observer in
  `src/runtime/guest/achievements.cpp` reads the game's progress bit array at
  `0x002A50C0` and a few words beside it and writes nothing into guest
  memory. Screenshots are host-side and change no value the game
  supplied. `gameplay.*` keys
  are the one class that reshapes an input before the game ever reads it:
  they are opt-in, default off, and touch only what the virtual pad reports
  to the guest. They never patch guest code and never alter a value the game
  itself computed. The mouse pointer on the game's own menus
  (`src/runtime/guest/menu_nav.cpp`) is the one exception, by the user's
  decision on 2026-09-03: it reads the game's own menu scene objects to
  find where the current screen's items are on the picture, and hovering
  one writes the game's own menu selection words, so that the item under
  the cursor becomes the selected item at once, exactly as the game's own
  navigation would have left it. Nothing is authored on the host side and
  there is no rectangle table; every address it reads and writes is listed
  in `src/runtime/guest/ico_syms.h`. Nothing else in guest memory is
  written and no guest code is patched; cross, triangle and the wheel stay
  virtual pad presses.
- For every settings key with an environment-variable twin, the environment
  wins over `settings.json`, logged at startup at `info`, so every existing
  script and CI invocation keeps behaving exactly as it did before
  settings.json existed. `info` is below the shipped default level, so the
  line is in the log of a run at `debug.log_level = "info"` or lower and not
  in a default run's; the menu shows the same fact live, on every
  overridden control, whatever the level.
- Settings handling is never fatal: a bad value keeps its compiled-in
  default with a log naming the value and the allowed range, and an
  unparseable file runs on defaults rather than stopping the run.
- UI assets (documents, stylesheets, fonts) live under `ui/`, not `assets/`
  (`tools/check_no_rom.sh` blocks `assets/*` outright). Stylesheets stay
  within the overlay renderer's RenderInterface subset documented in
  `docs/SETTINGS.md`.
