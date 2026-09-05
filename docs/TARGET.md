# The target

This port targets one retail disc: `SCES_507.60`, the European ICO disc
(SCES-50760). One build serves it and nothing else, because the generated C
is a translation of that one ELF.

This file says what that target is, which of its facts are measured and which
are inferred, and how to build it.

## Building

    cargo run -p recomp-cli -- ee
    cargo run -p recomp-cli -- vu1
    cmake --preset linux-gcc-release
    cmake --build build/linux-gcc-release -j2

Both translator stages default `--config` to `config/recomp.toml` and default
their output to `generated/ee` and `generated/vu1`, so `--config` and `--out`
are only needed to name something else. An output directory must sit under
`generated/`, which the translator checks before it writes anything.

A configure that finds no translated C in `generated/ee` says so and builds
in stub mode (`src/runtime/main.cpp`), which is what a runtime-only checkout
and the disc-free selftests rely on. Such a build runs no guest code.

## The target's facts

`src/runtime/target.h` holds them in one place:

| fact | value |
|---|---|
| boot ELF | `SCES_507.60` |
| SHA-1 pin | `da3644c5...` |
| entry | `0x00100008` |
| PT_LOAD vaddr | `0x00100000` |
| gp | `0x00640AF0` |
| config file | `config/recomp.toml` |
| generated C | `generated/ee` |
| generated VU1 | `generated/vu1` |

Measured on 2026-09-04 from the retail disc: the SHA-1 of the boot ELF as it
sits on the disc, `e_entry` and the single `PT_LOAD`'s `p_vaddr` from its ELF
header, and the gp from the ELF's own crt0 (`lui $a0,0x64`,
`addiu $a0,$a0,0x0AF0`, `move $gp,$a0`, so `0x00640AF0`, which is also the gp
crt0 hands RFU060 and what `config/recomp.toml` reads out of the ELF's
`.reginfo`). `SYSTEM.CNF` on that disc reads `BOOT2 = cdrom0:\SCES_507.60;1`,
`VER = 1.00`, `VMODE = PAL`.

Mounting an image with no `SCES_507.60` on it fails, and the message says the
image is not an ICO PAL (SCES-50760) disc image rather than reporting a
corrupt image.

### Disc layout

The disc carries `DFDATAS/DATA.DF` (the game's one data archive, 867 MB, the
extra languages included), the IOP modules, `MCXMAN.IRX`, `MCXSERV.IRX`,
`PANICSYS.IRX`, `DUMMY.TXT`, `TRTABLE.BIN` and three build artifacts
(`MAIN.MAP`, `SRCFILE.TXT`, `TRFILE.TXT`). None of that reaches the runtime:
the IOP is HLE'd at the SIF RPC layer and `LoadModule` loads nothing
(`src/runtime/sif/cdvd.cpp`), so a module's name, size and version are never
read.

`SRCFILE.TXT` is a full objdump of a build of the game, and it is **not** the
shipped ELF. Its crt0 differs from `SCES_507.60`'s at the first instruction
that carries an address, and its `.text` ends at 0x28DB34 against the retail
ELF's 0x289BC4. Its addresses are therefore not this ELF's addresses, except
where the two links happen to coincide, which they do at the bottom of
`.text`: 150 donor functions reproduce, masked, at delta 0. Behaviour read
out of it (which branch does what) is a fair reading of the same source;
an address has to be established against `SCES_507.60` itself, which is what
the correlation below does.

### The translator's inputs

`config/recomp.toml` names one input table, `[inputs]`, and it names two
files, both of which `setup.sh` copies out of the user's own disc image into
`baserom/pal` (gitignored):

| file | what it is |
|---|---|
| `SCES_507.60` | the retail boot ELF, SHA-1 pinned in `[pins]` |
| `SRCFILE.TXT` | the objdump listing of a late development link |

Nothing else is read. The disc's other two build artifacts, `MAIN.MAP` and
`TRFILE.TXT`, are described above and are opened by nothing here; `setup.sh`
does not even extract them. The ICO decomp is a separate project and not an
input (the user's ruling of 2026-09-05, in CLAUDE.md): there is no `[decomp]`
table and the translator has no code path for one. It may be read as a
behavioural reference the way PCSX2 is.

The function table is built in two halves that answer different questions.

**Where the functions are** is answered by the ELF, by five proofs, in the
words the ingest uses:

- `jal`: the address is the target of a `jal` inside `.text`. A call names
  its callee.
- `pointer-after-return`: the address is materialized by a `lui`/`addiu` or
  `lui`/`ori` pair and lands immediately after a `jr $ra` and its delay
  slot, which is how a function used only as a pointer is taken, and the
  register the pair leaves it in is never used as the base of a load or a
  store afterwards.
- `pointer-with-prologue`: the address is materialized by such a pair, the
  register is again never used as a memory base, and the words at the
  address are a function prologue.
- `data-pointer-with-prologue`: an aligned word of an on-disk section that
  is not `.text` holds the address, and the words at the address are a
  function prologue. This is the function only a table names: no `jal` and
  no `lui`/`addiu` pair anywhere, because the code loads the pointer out of
  the table. Added on 2026-09-04 after the PAL boot died on `bad indirect
  call: target=0x0021f1e8 caller 0x0013f558`; 0x0021F1E8 is held as a plain
  data word at 0x002C4184, which neither of the two pointer proofs can see.
- `prologues-after-transfers`: a prologue that begins right after an
  unconditional transfer and its delay slot, with only alignment padding in
  between, and that no branch anywhere in `.text` targets. This is the
  function whose only callers hold its address in a value computed at run
  time and stored into a structure, which no `jal`, no formed pointer and no
  data word names. `actCommonFly` at 0x0015F6A0, a thread entry the game
  puts into its own thread record, is one.

The ELF entry point from `[target].entry` is an entry by definition. A `j`
target is a candidate and not a proof: most `j`s are long intra-function
branches, but a tail call is also a `j`, and a function whose only caller
tail-jumps to it has no `jal` anywhere. The matrix composer at 0x001146F0 is
exactly that, and the widescreen entry hook is keyed on it. Such an address
becomes an entry only when the position (it follows an unconditional
transfer and its delay slot) is joined by evidence about the address itself:
a prologue there, or a donor function the correlation placed on it. Position
alone is not enough, because a label right after a `b` and its delay slot is
the ordinary shape of a rotated loop, and because
`prologues-after-transfers` explicitly refuses an address any branch or `j`
reaches. Measured on the 2026-09-05 run: 149 `j` targets pass the joined
test and become entries, 1 is dropped for having neither a prologue nor a
name.

What "proves" means is narrower than "a word decoded as a `jal`". `.text` is
not all instructions: this game embeds constants inside `.text` functions,
and one data word whose top six bits happen to read as `jal` would otherwise
manufacture an entry and split a function at a non-boundary. So every proof
carries a second, independent test, and an address needs one of the two
forms of it:

- positional: the address follows an unconditional transfer (`j`, `b`,
  `jr $ra`, `eret`) and its delay slot, with only alignment padding in
  between, which is where a function boundary is.
- structural: the first word at the address is `addiu $sp, $sp, -N` (or
  `daddiu`, or the `dsubu` form a frame too big for a 16-bit immediate
  takes) with N a multiple of 16, and within the next eight instructions
  there is a `sw`/`sd`/`sq` through `$sp` of `$ra` or of a callee-saved
  register (`$s0`-`$s7`, `$fp`), or a call. The frame word alone is not
  enough, because an epilogue's `addiu $sp, $sp, N` and a mid-function
  `alloca` also adjust the stack: the negative immediate excludes the
  epilogue, and the save excludes the rest, because nothing but a prologue
  allocates a frame and immediately saves into it a register whose value the
  caller owns. A leaf that saves nothing is accepted on a call in the same
  window. Eight instructions is what this build's widest prologue takes to
  reach its first save. The frame allocation is not always the first word:
  up to three scheduled setup words are allowed ahead of it, each with no
  control transfer and no read or write of `$sp`, because the compiler
  hoists address forms and loads that do not depend on the frame (the ending
  thread entry at 0x0021F1E8 is `lui`, `addiu`, `lw`, then the frame).

Both pointer proofs also require that the register the pair leaves the
address in is never used as the base of a load or a store before something
writes it again. That is what separates `la` of a function from `la` of a
data object: a function pointer is stored, passed or called, never
dereferenced with an offset by the code that formed it, and this game does
put data inside `.text`, including immediately after a function end where
the positional test alone would accept it. Measured on 2026-09-05: 389
addresses satisfy the positional test and none of them is a memory base, so
the exclusion costs no entry on this binary and closes a case that would
otherwise be silently wrong. `data-pointer-with-prologue` rests on the
structural test and on nothing else: a data word carries no position in the
instruction stream to test.

The structural form is what the positional one cannot reach: a function can
follow another function's `b` into a tail, or a lone `jr $ra`. The PAL
build's coroutine entry at 0x001021A0, which `iosThreadMain` calls through
the thread struct's function pointer at +0x38, is preceded by a `b` and its
delay slot; the alarm callback at 0x00265AE8, which `SetAlarm` is handed at
0x00265B50, is preceded by a lone `jr $ra` whose delay slot is the
callback's own first instruction. Neither would have an entry on position
alone, and an indirect call to either was the runtime's `bad indirect call`
fatal.

**What the functions are called** is answered by the listing. Its function
bodies are transplanted onto the retail `.text` by masked-instruction
fingerprint (`tools/recomp/crates/ingest/src/correlate.rs`). The mask erases
exactly the fields a relink rewrites: a `j`/`jal` target, a `lui`
immediate, an `addi`/`addiu`/`ori` immediate, and the displacement of every
load and store of any width, to any register file. Branch offsets are
deliberately not masked: they are PC-relative and internal to the function,
so a relink leaves them alone and they are the strongest discriminator
available. A donor function whose first eight masked instructions match one
position in the retail `.text`, and whose whole body then reproduces there,
is an anchor. A function between two anchors that agree on one donor-to-
target delta is placed at that delta if its body reproduces; outside the
anchored range the nearest single anchor's delta is tried, and with no
anchors at all, delta 0. Those are hypotheses only: the body check is the
proof, and a wrong hypothesis cannot pass it.

Measured on the 2026-09-05 run: 5533 of 5855 donor functions placed, 3772 of
them as unique-fingerprint anchors and 1761 by neighbour delta, 322
unresolved (the function is gone from the retail build or its body genuinely
differs), 0 anchors dropped as non-monotonic. That gives 5743 functions,
5454 of them named from the listing and 289 provisional `func_XXXXXXXX`, in
346 translation units, with 110 jump tables recovered from `.rodata`.
79 donor labels were folded back into the function that contains them: the
dev build exported assembly-local labels (`sceSifWriteBackDCache` is
followed by `loop1`, `eight`, `loop8`, `last`), and taking every label as an
entry would chop a routine into fragments.

Two facts about a boundary are checked before the entry list is used, and
both are loud:

- an entry that is the delay slot of the control instruction before it. Both
  facts are true and they cannot both be described: something enters at the
  address, and the instruction at address-4 executes that word before it
  transfers. The entry is placed one word later instead, and the runtime's
  alarm HLE steps the shared instruction itself and warns once
  (`src/runtime/ee/alarms.cpp`). The donor name follows the entry. There are
  two on SCES_507.60: 0x00265A8C to 0x00265A90, and the `SetAlarm` callback
  the listing calls `CB_DelayTh`, 0x00265AE8 to 0x00265AEC. A `jal` target
  in this position, or one in the last word of `.text`, is a hard error
  instead: neither reading can be described and the ingest says so rather
  than guessing.
- a branch that crosses a function boundary backwards into an entry. The
  emitter merges two functions into one group only when a branch target is
  not an exact entry, so a branch to another function's entry becomes
  `CF_x(ctx); return;`. Forwards that is harmless: the guest runs the
  prefix, then the later function, then returns to the caller, and the
  emitted form does the same thing one frame deeper, with the branch site
  unreachable afterwards (16 of these on this binary, in the vendor libc
  block). Backwards it is a loop, and each iteration would add a native
  frame that never unwinds. That is a hard error naming every site. There
  are none on this binary, and the run prints the count even when it is
  zero, so that a change to the proof set cannot reintroduce one silently.

The `ee` run prints all of the above and writes it, followed by the
whole-`.text` pointer sweep, to `entry_gaps.txt` in the output directory,
which is under the gitignored `generated/` tree. The sweep is every address
a `lui`/`addiu` pair forms inside `.text` that no proof turned into a
function entry, with the instruction that forms it, the function it is
inside, and what the words there look like. Nothing is done with it. It is
there because every function pointer the runtime can be asked to call has to
resolve to a translated function, and an address in that list is either data
or an entry these proofs do not find. The runtime's `bad indirect call`
fatal names the function the target is inside and points at this file. On
SCES_507.60 the sweep lists 13 addresses.

### Verifying the decode

    cargo run -p recomp-cli -- verify-decode

diffs our R5900 disassembly against `SRCFILE.TXT`, and defaults `--config` to
`config/recomp.toml`. It normalizes the eleven places where binutils and our
formatter spell the same encoding differently, listed in the header of
`tools/recomp/crates/recomp-cli/src/verify_objdump.rs`. Every one is checked
against the encoded word rather than against our own text. A spelling it does
not model is counted and printed in its own histogram rather than being
called a disagreement, but only when the file lists that spelling with the
reason it cannot be compared against an encoding; the list is empty today.
Anything else is a mismatch and fails the run. As of 2026-09-05 the run reads
403684 instructions verified, 0 unmodelled forms, 0 mismatches, exit 0.

What this does and does not cover, because "verify-decode must stay green"
now means something narrower than it did when the decomp's asm baselines were
an input. The check decodes the word each listing line prints and compares
our text against binutils' for that word. It reads no byte of `SCES_507.60`,
so a word that exists only in the retail link is never put through it.
Measured on 2026-09-05: 59176 of the 403185 word positions in the retail
`.text` (14.68%) hold a word that appears nowhere in the listing. Grouped by
encoding form rather than by word, though, the difference is entirely
relink-rewritten immediates: the retail `.text` uses 849 forms and the
listing covers all of them. So the decoder's coverage is effectively
complete, and what the check cannot see is a decoding that depends on an
immediate's value, not a whole instruction.

The other half of the verification is the per-op three-way test
(`tools/recomp/crates/ee-interp/tests/threeway.rs`): the reference
interpreter and the compiled emit must agree exactly, instruction by
instruction. Between them they cover what the objdump check cannot: that the
text we print is right, and that what we emit for it does the right thing.

### Where the inputs live

`[inputs].root` is `baserom/pal`, relative, so a fresh clone works with no
local edit; a relative root is resolved against this repository's root and an
absolute one is taken as given. `ICORECOMP_PAL_ROOT` wins over the file and
says so on stderr.

A relative value in the environment variable is taken as relative to the
working directory the command runs in, while a relative `root` in the config
file is relative to this repository's root. The override notice is printed
once per run, at the first config load, however many times a command loads
its config.

### Guest addresses

`src/runtime/guest/ico_syms.h` was filled on 2026-09-04 from `SCES_507.60`
itself. Four methods produced it, named per constant in that header. The
first is written in the past tense because the subcommand that ran it is
gone: `icorecomp symbols` and `icorecomp diff-targets` both existed only to
correlate addresses from an earlier build onto this one, both needed a second
config to do it, and both were removed when this became the only target.
Nothing in the tree runs them today.

- `symbols` reported this build's address for a datum by correlating the code
  sites that materialize it, and said how many of those sites agreed. Only
  full agreement was accepted, and the count is recorded per constant in the
  header, in the form `correlation: N/N sites`.
- the RetroAchievements set for this disc (game 1319: the `r=patch`
  conditions and the `r=codenotes2` notes, retrieved 2026-09-04), whose
  addresses were measured on this build by that community.
- the decomp's own `config/symbol_addrs.pal.txt` and its `asm/` data
  references, which are an independent reading of the same addresses.
- the instructions themselves, decoded out of `SCES_507.60`. `symbols` only
  counted sites inside a function that correlated whole onto this `.text`,
  and the layout and menu functions do not correlate: they carry code the
  correlated build's do not. Where that happened, the instruction's masked
  window (the mask in `tools/recomp/crates/ingest/src/correlate.rs`) was
  searched for in this `.text` and the instruction at the matching position
  decoded.

Where two of them name the same word they agree, which is the strongest kind
of fact in that header: the progress bit array, the video mode word and the
stage id are each named by both.

What stayed a sentinel, and why:

| constant | why |
|---|---|
| `RT_ICO_LAYOUT_GAMEPLAY`, `RT_ICO_LAYOUT_GAMEOVER` | layout ids are values, not addresses; nothing to correlate. Both come off a run's diagnostic log |

Each of the three features that read guest memory has its own gate, because
what is resolved differs per feature:

- the mouse pointer (`guest/menu_nav.cpp`): **on since 2026-09-04**.
  `RT_ICO_NAV_SWALLOW` is `0x0063B620`, measured at 11 of 11 sites, and every
  layout and scene object offset the pointer walks was read off this build's
  own code: the scene object is 0x70 bytes, the hidden flag is bit 4 (0x10),
  the memory card screen's first object is 0x18B, and the layout table has
  0x50 entries. `icorecomp-menu-nav-selftest` builds the module against these
  constants and runs the whole selftest on them, so the offsets and the
  hidden bit are exercised rather than only described. It has now also run
  against the disc: on the 2026-09-04 22:07 run the pointer took the mouse on
  the language boot screen and moved the game's own selection between its five
  items. Nothing past the boot screens has been exercised on the disc.
- `display.widescreen` (`guest/widescreen.cpp`): **on**. The composer entry
  (`0x001146F0`) and the projection block (`0x0067BA60`) were found by the
  code that builds the block rather than by name, by the method described
  above. The translator's own correlation names that entry
  `gsb_SetVSMatrixSub`, from the disc listing's donor body at 0x001145E8:
  since the correlation's mask covers every memory displacement (2026-09-05)
  the two bodies agree on all 270 instructions, and `generated/ee/funcs.h`
  carries the name. `config/entry_hooks.txt` carries the address and the name
  and points here for the method.
- the achievement observer (`guest/achievements.cpp`): **on**, with eleven of
  the sixteen trophies mapped to a progress bit from the RA set, the two time
  trophies judged on the game's own frame counter, and the host playtime
  counter left at 0 because the gameplay layout id is not known.
  docs/ACHIEVEMENTS.md carries the whole table.
  `icorecomp-achievements-selftest` builds the observer against these
  constants and runs the whole selftest on them, including the eleven-entry
  bit table and the 0x32-byte array. On the disc the observer resolves its
  addresses and seeds its baseline (`field 1: progress baseline seeded, 0
  bit(s) set` on the 2026-09-04 22:07 run); no trophy has been seen to
  unlock, because no run has got that far.

No sentinel was replaced by a plausible value, and no address here is another
build's address plus a delta. A structure offset carried from the decomp says
so where it is used; every offset the menu pointer walks, and the progress
array's length (0x32 here, measured rather than carried from the earlier
reading's 0x2E), was read off this build's own code on 2026-09-04.

The VU1 microprogram registry needs nothing of its own: `rt_vu1_register_all`
comes from the generated VU1 code, so a build registers the microprograms its
own translator run produced and only those. An upload whose hash matches
nothing in the registry is already a loud warn and a skipped MSCAL
(`src/runtime/hw/vu1rt.cpp`), which is the same sentinel discipline: no
plausible substitute, and the log names the hash.

## The field rate follows the programmed video mode

The field period is not a constant: `src/runtime/video_mode.h` and
`video_mode.cpp` hold it, and `rt_gs_program_crt`
(`src/runtime/hw/gspriv.cpp`) is the only thing that sets it, from the CMOD
the game asked `SetGsCrt` for. Both modes belong to this target, because the
game's own display option programs either one (see "The picture" below):
NTSC timing here is a run-time mode, not another build.

Hardware facts (ps2tek "GS privileged registers" for CMOD and the analog
video clock; the analog broadcast standards for the rest):

| | NTSC | PAL |
|---|---|---|
| CMOD | 2 | 3 |
| lines per frame | 525 | 625 |
| active lines | 480 | 576 |
| field rate | 59.94 Hz | 50 Hz |
| bus cycles per field | 2460060 | 2949120 |
| bus cycles per H-blank | 9371 | 9437 |
| bus cycles per vblank | 206184 | 226488 |

The EE bus clock is 147.456 MHz in both modes, so a field is
147456000 / 59.94 = 2460060 cycles on NTSC and 147456000 / 50 = 2949120 on
PAL, the second with no remainder. An H-blank is derived from the field
rather than from the line rate directly, so the alarm clock, the H-blank
timers and the vblank timeline all count the same line: 2460060 * 2 / 525 =
9371 and 2949120 * 2 / 625 = 9437. The vblank is the frame's blanking lines
split between its two fields, (525 - 480) / 2 = 22 and (625 - 576) / 2 = 24.

Every NTSC number above is the number this runtime used before the period
became a variable, to the cycle. `icorecomp-video-mode-selftest` asserts each
of them against the literal rather than against the derivation, and exercises
the switch in both directions.

What reads the current period rather than a constant: the vblank timeline
and the field edge (`ee/sched.cpp`), the HBLNK timer prescale
(`ee/timers.cpp`), the kernel alarm clock (`ee/alarms.cpp`), the pad tick
(`sif/pad.cpp`), the audio mixer's frames per field (`snd/engine.cpp`: 800.8
on NTSC, 960 exactly on PAL), the achievement observer's guest clock, the
profile summary, and the frame pacer's default period
(`debug.fps_limit_hz`, see docs/SETTINGS.md).

Two things change at the moment the mode does. The vblank timeline restarts:
the field the game was in ends at the `SetGsCrt` write and the next field
edge is one whole field of the new mode away, which is what the GS CRTC does
when SMODE1 changes. And every timer on the HBLNK prescale is re-based, its
current COUNT becoming the new base, so the new line length does not rewrite
the count the game has already read. Both are logged.

Not changed: the audio cushion (`RT_AUDIO_CUSHION_FRAMES`, 4800 frames)
stays a duration, 100 ms at 48 kHz, because what it buys is host scheduling
latency, which does not change when the guest changes video mode. It is six
NTSC fields of mix or five PAL ones.

Inferred, not measured: the mode the timeline runs at before the game's first
`SetGsCrt`. A console powers its GS up in its own region's mode, so this
build starts at PAL. Every retail path reaches `SetGsCrt` within the first
few fields of boot, so this only decides the length of those fields.

Also inferred: an alarm armed before a mode change keeps the duration the
mode it was armed in gave it, because an alarm is converted to an absolute
cycle when it is armed. Nothing in the retail game arms an alarm across the
boot `SetGsCrt`, and an alarm here is a few milliseconds at most.

## The picture

The game's own display option is one config word, read in its `gsb_Init`: one
value gives a 512x448 buffer and `SetGsCrt` mode 2 (NTSC), the other gives
512x512 and mode 3 (PAL). That is the 50 Hz / 60 Hz choice the player makes,
and it is why the field rate has to follow the mode the game programmed
rather than the region of the disc. Read off `SRCFILE.TXT`, so it is
behaviour, not an address: see the note above on what that file is.

Scanout: `scanout_display_aspect` (`gs/gs_parallel_scanout.cpp`) is mode
independent. It derives the aspect from the renderer's mode area, and every
NTSC and PAL mode area is the active area of an analog set, which is
displayed 4:3; the PAL non-overscan area is 640x256 per field against NTSC's
640x224, and the fraction form cancels the difference. `rt_gs_program_crt` is
not fatal on either of these two modes: its fatal is for the DTV and VESA
modes, which this runtime does not model and no retail path programs.

`display.raster = window` grows the frame to the display window, so its
aspect comes from the registers the game programmed: DW+1 clocks against
2560, and DH+1 frame lines against the frame lines of the 4:3 mode area,
448 on NTSC and 512 on PAL. The clock reference is 2560 in both modes,
because 640 pixels at the standard clock divider is 2560 DW units either
way. So gameplay comes out 4:3, and a 720x576 picture comes out 4:3 as well,
where a 720x480 one comes out 1.4 from the same formula.

The movie's display object is given 720 and 288 unconditionally, with no
branch on the display option: `mv_disp`'s `setDispEnv` stores them at
`0x00258234` and `0x00258238` (measured on `SCES_507.60`, not read off
`SRCFILE.TXT` as this paragraph once was). Reading that pair as 720x288 per
field, that is 720x576, is what the rest of this paragraph assumes, and it is
NOT confirmed. The PAL disc carries a 720x576 movie set and two 720x480
sets, and the same display option that picks the video mode picks which the
player asks for. See "The IPU and the movie" below for the measurement and
for what settles it.
A 720x576 picture fills its 4:3 area where a 720x480 one does not, which is
why the same formula gives those two different aspects.
`display.deinterlace` works off the field the renderer returns and its phase,
neither of which is mode specific.

The IPU path has no geometry of its own. It decodes what the stream says and
moves what the guest's own transfer sizes ask for, so a 720x576 frame is
1620 macroblocks where a 720x480 one is 1350, and nothing in
`src/runtime/hw/ipu.cpp` has to change.

## What the PAL binary does differently

The runtime's kernel HLE, memory map, DMA, GS privileged registers, VIF1,
GIF, IPU, VU1 and the translator's census were all measured against
`SCUS_971.13`, the US boot ELF. This section is the measured comparison
against `SCES_507.60`, and it names the log line each difference produces so
a run can be checked against it. Every row below was measured off the two
ELFs (`/primary/dev/ico/baserom/pal/baseelf.elf` and the US snapshot's
`baserom/baseelf.elf`) and the two decomps, on 2026-09-04, unless it says
otherwise.

### Syscalls

Both ELFs carry the same libkernel stub table and the same 142 distinct
syscall numbers in `.text`. What differs is which stubs anything calls. The
reachable set was measured by counting `jal`/`j` targets at each stub's entry
(the instruction before its `syscall`), which resolves every site in both
binaries with none left unattributed.

| syscall | name | PAL callers | US callers | runtime |
|---|---|---|---|---|
| 0x4B (75) | `GetOsdConfigParam` | 8 | 0 | handled, `ee/syscalls.cpp` |
| 0x6F (111) | `GetOsdConfigParam2` | 3 | 0 | added, `ee/syscalls.cpp` |
| 0x40 (64) | `CreateSema` | 67 | 68 | unchanged |
| 0x41 (65) | `DeleteSema` | 110 | 111 | unchanged |
| 0x42 (66) | `SignalSema` | 101 | 99 | unchanged |
| 0x44 (68) | `WaitSema` | 84 | 85 | unchanged |
| 0x64 (100) | `FlushCache` | 40 | 42 | unchanged |
| 0x0D (13) | `SetVTLBRefillHandler` | 4 | 4 | not in the table, see below |
| 0x0E (14) | `SetVCommonHandler` | 1 | 1 | not in the table, see below |

Every other reachable number has the same caller count in both, and every one
of them is in the runtime's table. The two OSD numbers are the only ones the
PAL build reaches and the US build does not.

`GetOsdConfigParam` is why this port has a `system.language` setting at all.
The chain is `kanbanBootMcCheck` -> `sceScfGetLanguage` (jal at `0x001B9614`)
-> `GetOsdConfigParam` (`../ico
asm/nonmatchings/src/kanbanBoot/kanbanBootMcCheck.s` and
`asm/nonmatchings/src/cod/vendor_272338/sceScfGetLanguage.s`). The library
reads a version at bits 13..15 and, when it is nonzero, a language at bits
16..20; the boot then tests `language - 1 < 5` at `0x001B9624` and indexes a
five-entry table, so 1..5 select a language and anything else keeps the
compiled-in default. The US build calls the stub from nowhere, so on that
disc this word is never read. Log line, once per run:

    [syscall] PAL: the game asks the kernel for its OSD configuration. ...

followed by one `GetOsdConfigParam(0x........): version 1, language N ->
0x........` per call. `icorecomp-osd-config-selftest` asserts that the word
the runtime builds reads back through the library's own arithmetic, for all
five languages, and that a zero word does not.

`GetOsdConfigParam2` is reached from three more readers in the same vendor
object (`sceScfGetDateNotation` `0x00272B08`, `sceScfGetSummerTime`
`0x00272B88`, `sceScfGetTimeNotation` `0x00272C08`), all of which take the
same nonzero-version branch. Measured: of the three, only
`sceScfGetSummerTime` has a caller at all (`sceScfGetLocalTimefromRTC`,
`0x002731C0`), and that function has no caller itself, so no reachable path
in this build enters it. It is implemented rather than left as an
unknown-syscall stop, and it does not invent a value: it writes zeros and
says so.

    [syscall] GetOsdConfigParam2(0x........, num=1, offset=1): NOT MODELED, ...

`SetVTLBRefillHandler` and `SetVCommonHandler` are called only from
`SetTLBHandler` (`0x00264E58`) and `SetDebugHandler` (`0x00264EB8`).
Measured in both ELFs: `SetTLBHandler` has no caller, and `SetDebugHandler`'s
two callers are `debugEEExceptionMain` and `debugExceptionInit`, which have
no callers either. Both numbers are statically dead in both builds, so
neither is added to the table: the coverage policy is that nothing
speculative reaches it. If either is ever reached it stops the run with the
usual line, which is the intended outcome:

    [syscall] FATAL unknown syscall number 13 ...

### Kernel entry points

`InitAlarm` (`0x00100C90`,
`../ico asm/nonmatchings/src/cod/vendor_100110/InitAlarm.s`) is the only
caller of `GetEntryAddress`. It reads a static table of syscall numbers, at
`0x0028F470` on PAL and `0x00274E70` on US, and installs the first two
without asking the kernel, then asks for the rest one at a time. Those two
tables are byte-identical, so the requested set is the same on both:

| step | PAL | US |
|---|---|---|
| `SetSyscall(0x5A, 0x00100C48)` | yes | yes |
| `SetSyscall(0x5B, 0x80076000)` | yes | yes |
| `GetEntryAddress` indices | 0xFC, 0xFE, 0xFD, 0xFF, 0x12C, 0x08 | same |
| `Copy(0x80076000, ..., 0x740)` | yes | yes |
| `Copy(0x00082000, ..., 0x20)` | yes | yes |

The guard `InitAlarm` opens with is a read of `T3_MODE` (`0x10001810`) and a
test of bit 8; the whole body is skipped when it is set. `ee/timers.cpp`
decodes that register, so the read is not a wild access.

There are no real kernel entry points behind an HLE kernel, so each index is
answered with `0x00080000`, zeroed kernel-reserved RAM. That is not silent:
each distinct index logs its own line naming what was asked and what was
given.

    [syscall] GetEntryAddress(0xfc): NOT MODELED, the HLE kernel has no entry
    point for that index; guest asked for kernel entry 0xfc and was given
    0x00080000 ...

Six such lines at boot, and then nothing. The vectors `InitAlarm` records
through `SetSyscall` are never jumped to, because `rt_syscall` dispatches
from its own table.

### The kernel ROM, region and video mode

Measured, and it is a PAL-only path. `sceScfGetLanguage` calls `IsT10K`
(`0x00272918`) before it reads the OSD word. `IsT10K` calls `GetRomName`
(`0x00272878`), which opens `rom0:ROMVER` (the string at `0x006371D0`) with
`sceOpen`, reads 14 bytes with `sceRead` and closes it, then tests byte 4 of
what it read against `'T'`. Nothing else in either build reads the boot ROM:
neither ELF forms an address in `0x1FC00000`/`0xBFC00000` (the two `lui
$at, 0xBFC0` in the PAL `.text` are constant material inside
`chain_simulate_hangstart` and `chain_simulate_term_swingready`, not
addresses), and neither forms an address in the `0x1F800000` or `0x1F900000`
IOP register windows. There is no `sceGsGetVideoMode` in either build, and
nothing asks the kernel for a region: the video mode comes from the game's
own display option through `SetGsCrt`, which is the section above.

`GetRomName` is the `sceOpen` a normal PAL boot reaches, and it is measured:
in the 2026-09-04 22:07 run the fileio BIND and its `fno=0xff` version query
sit between the two `GetOsdConfigParam` lines that `sceScfGetLanguage`
produces (`GetOsdConfigParam`, `IsT10K`, `GetOsdConfigParam`, in that order
at `0x00272960`, `0x00272968` and `0x00272980`), and the language screen is
drawn on the next line. The other seven callers of `sceOpen` are
`debugSceOpen`, `initLineTraceTable`, `traceLine` and friends; of those,
`gsb_LoadStageSettings` (`0x001163B0`) opens a per-stage settings file
through `debugSceOpen` and skips the read when the open returns -1
(`0x001163F4`), and nothing in `.text` calls it: its address is held in a
data table at `0x00290894`, so whether it runs is not settled here. The US
build never calls
`sceScfGetLanguage`, so it never calls `GetRomName` either; whether a US run
opens a file through the IOP fileio service by some other caller is
unmeasured.

`sceOpen` goes through `sceFsInit` (`0x00260C70`), which binds SIF RPC
service `0x80000001`, the IOP fileio server, and adds a SIF command handler
for `0x80000011`. That bind used to be a fatal, because no service was
registered for it; "The fileio server, and why PAL reaches it" below covers
the fix. This is the caller that runs on a normal PAL boot rather than only
after a guest exception: it is inside the memory card check's language step.

The answer the stub gives, -1, is the right one and not a substitution.
`GetRomName` handles a failed open: it prints `Can't open rom0:ROMVER` and
leaves its 14-byte buffer at `0x0054CB68` zeroed, so `IsT10K`'s test of byte
4 against `'T'` fails and `sceScfGetLanguage` takes the branch that reads
the OSD configuration word, which is the branch a retail console takes.
Inventing a `ROMVER` string would be a substitution, and a consequential
one: a string whose byte 4 is `'T'` would send the game to a different
language source entirely (`0x0054CB64`, a devkit override byte in `.bss`).

### The memory map

| fact | PAL | US |
|---|---|---|
| `e_entry` | `0x00100008` | `0x00100008` |
| PT_LOAD count | 1 | 1 |
| `p_vaddr` | `0x00100000` | `0x00100000` |
| `p_filesz` | `0x0053C0F6` | `0x00533BC6` |
| `p_memsz` | `0x00636198` | `0x0061EB98` |
| image end | `0x00736198` | `0x0071EB98` |
| `.text` end | `0x00289BC4` | `0x0026F5D4` |
| `.vutext` end | `0x0028ECB0` | `0x002746C0` |
| `.bss` | `0x0063C600`, `0x0F9B98` | `0x00634000`, `0x000EAB98` |
| gp (`.reginfo`, and crt0) | `0x00640AF0` | `0x006388F0` |
| `SetupThread` stack, size | `0x01FF0000`, `0x00010000` | same |
| `SetupHeap` base, size | `0x00736198`, `0x00010000` | `0x0071EB98`, `0x00010000` |

The image is 95744 bytes larger and everything after `.text` moves, but the
shape is the same: one PT_LOAD, the same entry, the same stack top and the
same stack and heap sizes, all inside the 32 MB `mem.cpp` maps.

**Fixed.** `RECOMP_TEXT_LIMIT` in `include/recomp_api.h` was `0x00280000`,
which is past the US `.vutext` end and 100 functions short of the PAL
`.text` end. The generated `funcs_table.c` indexes `g_functab_orig` with
`RECOMP_FUNC_IDX` and no bound test, so on PAL those 100 entries were writes
past the end of the array, not calls the dispatch would have declined. The
limit is now `0x00290000`, past `.vutext`'s `0x0028ECB0`, and the emitter
now writes a `#error` into `funcs_table.c` keyed on the highest translated
entry in whatever target it was run against, so the same mismatch fails the
build instead of the run
(`tools/recomp/crates/ee-emit/src/lib.rs`, the block before
`g_functab_init`). The window a build actually carries is named in the log:

    [loader] function table window: [0x00100000, 0x00290000), 393216 slots

Addresses outside PT_LOAD, measured by resolving every `lui`-seeded constant
in each `.text` through the following `ori`/`addiu` within its basic block
and keeping only those that become the base of a load or a store: the two
binaries reach **the same set, register for register and count for count**.
57 EE bus registers and FIFO windows, 10 GS privileged registers, 15
scratchpad addresses all inside `0x70000000..0x70000064`, and one VU memory
base, `0x1100C000`. Nothing PAL forms lands outside what `mem.cpp` maps or
what `mmio.cpp` accepts, so the wild-access fatal has nothing new to catch.

Regions neither binary reaches, contrary to what a `lui`-only scan suggests:
kernel RAM `0x00080000..0x00100000` (the constants there are integer
material, never a base), the IOP RAM window at `0x1C000000` (the one
`0x1C008000` in each is ORed into a GS register value two instructions
later), the `0x1F800000` and `0x1F900000` IOP register windows (zero
occurrences of any kind in either), and `0xA0000000`. `0x20000000` is used
as an uncached alias on RAM pointers in both, about 130 sites each.

**Fixed.** Two registers inside the EE window are claimed by no module in
`hw/` or `ee/`, in both binaries: `0x1000F130` and `0x1000F180`, the EE
debug console's TX-ready word and character port, read and written by
libkernel's `kputchar` (`0x001010C8` on PAL). A read gave 0 and a write was
dropped, with nothing but an ordinary access line to say so.
`src/runtime/mmio.cpp` now names an unclaimed register once, saying what
the guest asked for and what it was given:

    [mmio] read32 of hardware register 0x1000f130 (physical 0x1000f130):
    NOT MODELED, no module owns this register; guest asked for its value
    and was given 0

Returning 0 there is what makes `kputchar`'s `andi 0x8000` fail and its wait
loop fall straight through, so the console output is discarded rather than
spun on. A line naming any third address is new behaviour: measured, these
two are the only ones either binary reaches.

### Hardware registers and the values written to them

The multiset of (register, store opcode, constant) over every store whose
address resolves into `0x10000000..0x13000000` is **identical between the
two binaries**. There is no EE hardware register the PAL build programs with
a different constant.

| surface | what both binaries do |
|---|---|
| timers | T0 and T1 only. `T0_MODE = T1_MODE = 0x82` (CLKS 2, BUSCLK/256; CUE set; every interrupt enable clear). COUNT zeroed at start. No `T_COMP` or `T_HOLD` write anywhere. T2 never referenced. T3 read only, `T3_MODE & 0x100` in `InitAlarm` |
| INTC | `I_MASK` never written directly; masking goes through `_EnableIntc` / `_DisableIntc` / `AddIntcHandler`. `I_STAT` written only with `0x4`, the vblank-start acknowledge in `VSync`/`VSync2` |
| DMAC | `D_CTRL |= 3` (DMAE, RELE) then `|= 1`. CHCR written for D1 VIF1, D2 GIF, D3 fromIPU, D4 toIPU, D8 fromSPR, D9 toSPR. D5 SIF0 CHCR read, never written. D0, D6 and D7 never touched |
| GS privileged | 10 registers written, all through `sceGsPutDispEnv` or `sceGsResetGraph`: PMODE, SMODE2, DISPFB1/2, DISPLAY1/2, EXTDATA, BGCOLOR, CSR, BUSDIR |
| SMODE1, SRFSH, SYNCH1, SYNCH2, SYNCV, IMR | **never written directly by either binary.** The CRT timing comes from `SetGsCrt` and IMR from the `GsPutIMR` syscall, which is what `hw/gspriv.cpp` already assumed |

`SetGsCrt` (syscall 2) has exactly one call site in each binary, and it is a
tail call rather than a `jal`, inside `sceGsResetGraph`: `j 0x00100120` at
`0x0025B75C` on PAL, at `0x00241C04` on US, with the arguments masked three
instructions earlier (`interlace & 1`, `mode & 0xFF`, `ffmd & 1`).

### The display option: 50 Hz, a mode that can change, and a second movie set

This is the difference that matters, and it is one data word. It decides the
video mode, the framebuffer size, which movie set the player asks for, and
whether the mode can change at all.

| | PAL | US |
|---|---|---|
| display-option word | `0x0028F4C0` | `0x00274EC0` |
| its shipped value | **`1`** | **`0`** |
| word `+4` (interlace selector) | `2` | `2` |
| boot `SetGsCrt(interlace, mode, ffmd)` | **`(0, 3, 1)`**, PAL, 512x512 | `(0, 2, 1)`, NTSC, 512x448 |
| stores to that word in `.text` | **3**, all in `kanbanBootMcCheck` | **none** |
| read sites | 553 | fewer, and none in the movie path |
| movie display setup | reads the word, picks mode 3 or 2 | hardcodes mode 2 |
| movie frame height | reads the word, 576 or 480 | 480 |

`gsb_Init` reads the word and branches: `0` gives a 512x448 buffer and mode
2, `1` gives 512x512 and mode 3, and `gsb_ResetGSSystem` computes
`mode = word + 2` from the same place. The PAL disc ships that word as 1, so
the boot `SetGsCrt` asks for PAL, where the US disc's asks for NTSC.

The three stores are inside `kanbanBootMcCheck`
(`../ico asm/nonmatchings/src/kanbanBoot/kanbanBootMcCheck.s`). Two of them
are the menu: `0x001B97CC` writes 1 and `0x001B97D0` writes 0, reached from
menu item ids 33 and 34, and `gsResetFunc` is called right after
(`0x001B97E8`) when the value changed. The third, `0x001B95B4`, is not the
menu at all: it restores the word from the memory card's product block, word
`+0x1EC` of the 0x1F0-byte record, with the camera setting from `+0x1E8`,
and calls `gsResetFunc` the same way. The menu is the disc's own 50 Hz /
60 Hz option, and it means the video mode can change **while the game is
running** on PAL, where on US it was fixed at boot and could never change.
Everything the runtime derives from the field rate already reads the current mode rather than a
constant (see "The field rate follows the programmed video mode" above), and
`rt_gs_program_crt` already accepts mode 3 and calls
`rt_video_set_mode(RT_VIDEO_PAL)`. What changes is that both paths are now
live. The log lines that say so:

    [gs] SetGsCrt: SMODE1=0x00006100 SMODE2=0x2 programmed (PAL, progressive, ffmd=1)
    [sched] video mode change at vclk N: next field edge at M (2949120 cycles per field)

and the second one appearing a second time, mid-run, is the option being
used. The first of the two is measured: the 2026-09-04 22:07 run programs
`SetGsCrt(interlace=0, mode=0x3, ffmd=1)` and the video mode line that
follows reports the PAL field at 2949120 cycles. The boot is therefore
progressive PAL, not interlaced. The mid-run change to NTSC was seen on the
user's own 60 Hz run of the same day and is not in a log in this repository.

### The two boot screens, and when they appear

The 50/60 Hz screen and the language screen are not part of every boot. Both
belong to one state machine: `kanbanBootStart` (0x001B9A98) arms it from
`InitIcoMisc` (jal at 0x001B81B4), and `kanbanBootMain` (0x001B9920) steps it
each field from `ExecIcoMisc` (jal at 0x001B7E8C), calling
`kanbanBootMcCheck` (0x001B92D0). What decides is the memory card:

- state 2 calls `iosMcChdirProduct` (0x001B9474) and state 0x5F
  `iosMcLoadProductBlock` (0x001B954C).
- if the product block reads back (its result word `+0x10` is 0), the code at
  0x001B9584 takes the video mode word out of block offset `+0x1EC` into
  `0x0028F4C0` and the camera setting out of `+0x1E8`, calls `gsResetFunc`,
  and the machine runs 0x60 -> 0x61 -> 0xBE. Neither screen is drawn.
- if it does not, the machine goes to 0x64 and 0x65, where
  `sceScfGetLanguage` (jal at 0x001B9614) supplies the default and
  `kanbanReqAdd(0, 2)` puts the language screen up; state 0x66 reads the
  chosen item's `+0x2C` (ids 0x1A to 0x1E). The 50/60 Hz screen follows at
  state 0xC8 (`kanbanReqAdd(1, 2)` at 0x001B9770), gated at state 0xC2 on the
  word the language step wrote being non-zero, so it is on the same failure
  path; state 0xC9 turns item ids 0x21 and 0x22 into 1 and 0 in `0x0028F4C0`.

Measured on the 2026-09-04 22:07 run, on a card with no save: `ChDir` returns
-4, and the language screen is drawn (`guest menu: kanban layout 0x0 (the
language screen): 5 items`). That run did not go past it, so the 50/60 Hz
screen being drawn on the same path is read off the asm rather than measured.
The default the language screen opens on comes from `sceScfGetLanguage`,
which is `GetOsdConfigParam` (see "The kernel ROM, region and video mode").
The mouse pointer handles both screens; docs/SETTINGS.md section 10 has how.

The movie path differs in code, not just data. PAL's `dispCreate`
(`../ico asm/nonmatchings/ito/mpeg/mv_disp/dispCreate.s`, `0x002584AC` to
`0x002584CC`) loads the same display word and selects mode 3 or mode 2 with
a `movz`; the US build's counterpart at `0x0023E83C` writes `li a2, 2`, NTSC,
unconditionally. Both force `interlace = 1` there, which the two `gsb_*`
callers do not.

`SMODE2` is never a literal: `sceGsSetDefDispEnv` builds it from the
interlace and FFMD fields as 2 (non-interlace, FRAME), 3 (interlace, FRAME)
or 1 (interlace, FIELD), the same three values in both binaries. With the
shipped data the `gsb_*` path gives 2 in both and the movie path gives 3 in
both.

### Threads, semaphores, alarms and interrupt handlers

Every kernel call in this group has the same number of call sites in the two
binaries, and the constants they pass are the same. The functions they sit in
correspond one for one; only the names differ, because the PAL decomp names
several that the US symbol list numbers.

| call | PAL sites | US sites | constants |
|---|---|---|---|
| `CreateThread` | 6 | 6 | parameter block built on the stack at every site in both, so entry, priority and stack size are run-time values |
| `StartThread` | 4 | 4 | |
| `DeleteThread` | 2 | 2 | |
| `ExitDeleteThread` | 1 | 1 | |
| `CreateSema` | 67 | 68 | |
| `DeleteSema` | 110 | 111 | |
| `SignalSema` | 101 | 99 | |
| `WaitSema` | 84 | 85 | |
| `RotateThreadReadyQueue` | 2 | 2 | one in libkernel, one in the game's own thread switch |
| `SetAlarm` | 2 | 2 | both in delay helpers, `sceCdDelayThread` and `mcDelayThread` on PAL |
| `AddIntcHandler` | 2 | 2 | cause **2** (VBLANK_START) at the boot site in both; the other passes a run-time cause |
| `AddDmacHandler` | 5 | 5 | channels **2** (GIF), **1** (VIF1, debug), **5** (SIF0), **3** (fromIPU), **4** (toIPU), in that order, in both |

The DMAC channels the handlers are registered for are exactly the channels
whose CHCR the two binaries write, which is the same set on both sides.

**Needs a run to settle.** Thread entry addresses, priorities and stack
sizes: `CreateThread`'s argument block is filled on the stack at all six
sites in both binaries, so no static reading gives them. A run prints them,
both at creation and in the inventory dump:

    [sched] boot: thread 1 entry=0x........ prio=0 sp=0x........ gp=0x........
    [sched]   thread N <state> prio=P entry=0x........ stack=0x........+0x.... gp=0x........

The PAL numbers to compare against the US ones are those lines. A run also
settles whether any alarm tick count or vblank-wait loop differs with the
field rate: nothing static distinguishes them, because both binaries pass
run-time values to `SetAlarm`, and the runtime already converts a tick count
through the current mode's H-blank period (`ee/alarms.cpp`).

### VU1, VIF1, GIF and the translator's census

| fact | PAL | US |
|---|---|---|
| `.vutext` size | `0x50E0` | `0x50E0` |
| `.vutext` SHA-1 | `4098378a...` | `4098378a...` |
| `.vudata` size | 0 | 0 |
| mnemonics in the census | 290 | 290 |
| `.text` instructions | 403185 | 376181 |

The five VU1 microprograms are byte-identical, measured as the SHA-1 of the
whole `.vutext` section of each ELF; only its address moves. The EE mnemonic
census is the same set of 290 mnemonics with no member on either side that
the other lacks (`icorecomp ee --census` over each ELF), so the emitter's
coverage policy, which hard-errors on any mnemonic outside the measured
census, admits the PAL binary with no new op. The PAL `.text` is 27004
instructions larger.

`hw/vif1.cpp` decodes the whole VIF1 command set, not a measured subset of
it, so there is no VIF code a different build of the same engine could
introduce; the same is true of the UNPACK modes, which are decoded from
`vn`/`vl` rather than from a table of the ones this game was seen to use.

### The IPU and the movie

The PAL disc does not carry one movie set, it carries three. Measured by
scanning the retail PAL disc image for MPEG-2 sequence headers and decoding
each one's size, aspect and frame rate code:

| set | picture | aspect code | frame rate code | bit rate | headers on the disc |
|---|---|---|---|---|---|
| the US disc's set | 720x480 | 2 (4:3) | 4 (29.97 Hz) | 4.5 Mbit/s | 280 |
| PAL 50 Hz | **720x576** | 2 | **3 (25 Hz)** | 6 Mbit/s | 234 |
| PAL 60 Hz | 720x480 | 2 | **3 (25 Hz)** | 6 Mbit/s | 233 |

The US disc carries only the first, at the same header count and the same
bit rate, so that set is the same movie set on both discs.

Which one plays follows the same display word at `0x0028F4C0` that selects
the video mode. Measured on the PAL ELF, in the movie player's caller at
`0x00101E54`: `li s1, 480`, then a load of that word, then `li v0, 576` at
`0x00101E74` and `movn s1, v0, a0` at `0x00101E84`, so the requested frame
height is 576 when the display option is set and 480 when it is not. The US
build has `li a2, 480` at `0x00101E6C` and no branch. The macroblock count
follows: `sceMpegGetPicture` is called with `li a2, 0x654` (1620) at
`0x001A6F20` on PAL against `li a2, 0x546` (1350) at `0x0019E0F0` on US.

| quantity | PAL 576-line | 480-line (both discs) |
|---|---|---|
| macroblocks per frame | 1620 (45x36) | 1350 (45x30) |
| RAW8 into ch4 per frame | 1620 x 384 B = 38880 qw | 32400 qw |
| RGB32 out of ch3 per frame | 1620 x 1024 B = 103680 qw | 86400 qw |
| CSC runs (the library splits at 1023) | 1023 + 597 | 1023 + 327 |
| RGB frame bytes | 1658880 | 1382400 |

**`src/runtime/hw/ipu.cpp` needs no geometry change.** Its input and output
are growable buffers, CSC's macroblock count is taken from the command word
(`mbc = val & 0x7FF`), and a macroblock is a fixed 384 bytes in and 1024 out
whatever the picture is. There is no 480, no stride and no frame rate in its
executable code. Its every fatal (BDEC with MP1, CSC with OFM=1, IDEC, PACK,
the per-command bit budget, the chain-tag ceiling) is a property of the MPEG
library, and that library is the same object in both builds: 337
instructions differ across the whole of `libmpeg.a` and `libipu.a` and every
one of them is a relocated data address.

**Fixed.** Three things in those two files were US-shaped:

- `hw/ipu_selftest.cpp`'s register-access benchmark divided its budget by
  `59.94`, the only live NTSC constant in the IPU code. It now reports the
  cost against both field rates, since the PAL disc plays a movie in either
  mode:

      [selftest] register access benchmark: N accesses ... = X ns per access;
      at two million accesses a second that is A ms of host time per NTSC
      field and B ms per PAL field

- `hw/ipu.cpp`'s two comments describing the CSC feed said "720x480, 1350
  macroblocks, 32400 quadwords, runs of up to 0x147". The run length is
  1023, not 0x147 (`div a1, 1023` in `_doCSC2`, `0x00271B68`), and the frame
  is one of two sizes on this disc. Both now say so, and the decoder names
  what it saw once per run:

      [ipu] first CSC: guest asked for N macroblocks in one command. This
      disc's movies are 720x480 (1350 macroblocks per frame) and 720x576
      (1620); the run's display option picks which ...

- Every provenance citation in `hw/ipu.cpp` and `hw/ipu_selftest.cpp` named
  US addresses and US placeholder function names (`func_0023FDF0`,
  `func_00240090`, `func_00240218`, `func_002407C0`, `func_002586F8`,
  `func_002587E0`, under `asm/nonmatchings/src/GobjProc/` and
  `src/cod/vendor_2575C0/`), none of which exists in the PAL decomp. All 28
  instruction addresses and all six names were retargeted, and each
  translation was checked by comparing the instruction word at the US
  address against the word at the PAL address with the immediate and
  jump-target fields masked. All 28 agree.

| US | PAL |
|---|---|
| `func_0023FDF0` | `viBufAddDMA` `0x00259948` (`ito/mpeg/mv_vibuf`) |
| `func_00240090` | `viBufStopDMA` `0x00259BE8` |
| `func_00240218` | `viBufRestartDMA` `0x00259D70` |
| `func_002407C0` | `viBufGetTs` `0x0025A318` |
| `func_002586F8` | `sceIpuStopDMA` `0x00272338` (`src/cod/vendor_272338`) |
| `func_002587E0` | `sceIpuRestartDMA` `0x00272420` |

`icorecomp-ipu-selftest` reads its stream off the user's disc image and now
reports the aspect and frame rate codes alongside the size, so the
measurement is re-made every time it runs. On the PAL image it finds the
first program stream in `DFDATAS/DATA.DF` and reports

    [selftest] sequence: 720x480 (45x30 macroblocks), aspect code 2,
    frame rate code 3

which is the PAL 60 Hz set. The selftest walks to the first stream it finds,
so it does not by itself prove which set a given boot plays.

**Open, and it needs a run to settle.** The display side. `setDispEnv`
(`../ico asm/nonmatchings/ito/mpeg/mv_disp/setDispEnv.s`, `0x00258208`)
stores `0x2D0` (720) and `0x120` (288) at `0x00258234` and `0x00258238`, and
a page count of 108 at `0x00258254`; the US build computes the same three
from its arguments and gets 720, 240 and 96 (`0x0023E5F4` onward). 108 pages
is `ceil(720/64) * ceil(288/32)`, so PAL's field buffer is 288 lines where
the US one is 256, and the second field's Y offset is +288 against +256.
That is consistent with a 576-line frame presented as two 288-line fields,
and inconsistent with the 480-line set the same binary can also play. Which
of the two the retail attract sequence actually uses, and whether the
480-line set is reached at all on the PAL disc, is a run: the lines to read
are the one above from the decoder and the `SetGsCrt` line from `gs`. Until
then the aspect argument in "The picture" stands as unconfirmed.

### What the PAL build reaches of the runtime's own gaps

The question this section answers is the one the function-level diff between
the two builds raises: of the functions that are new or changed in PAL,
which call into something the runtime HLEs, and is each of those handled.
It is answered by surface rather than by function, because a name-level diff
of the two symbol lists is not evidence: 2971 of PAL's 5527 named functions
have no name in the US list, and almost all of that is the PAL decomp naming
things the US snapshot numbers (`iosThreadCreateS` against `func_0013D550`,
`viBufStopDMA` against `func_00240090`), not new code.

Taken by surface, the answer is that every HLE call site in the PAL binary
has a US counterpart with the same constants, with two exceptions, both
named above: the two OSD syscalls, and `dispCreate`'s mode-aware
`sceGsResetGraph` call. The counts that carry that claim are the syscall
table in "Syscalls", the register table in "Hardware registers and the
values written to them", and the call-site table in "Threads, semaphores,
alarms and interrupt handlers". The regions the two binaries address are
the same, the registers are the same, the values are the same, and the
kernel calls are the same.


`rt_unimplemented` routing is identical in the two binaries, measured by
running the emitter over each ELF: 5 mnemonics, 12 sites, `3 eret`, `2 tlbp`,
`3 tlbr`, `3 tlbwi`, `1 tlbwr`. They sit in the same nine kernel functions on
both sides (`_DumpTLB`, `kPutTLBEntry`, `kSetTLBEntry`, `kGetTLBEntry`,
`kProbeTLBEntry`, `kExpandScratchPad` and the exception handler at
`0x00265040` on PAL; the US ELF has the same nine at `0x0024AF90` onward,
unnamed). None of the nine has a `jal` or a `j` anywhere in either `.text`,
and the entry-point wrappers around them (`PutTLBEntry`, `SetTLBEntry`,
`GetTLBEntry`, `ProbeTLBEntry`, `ExpandScratchPad`, `InitTLBFunctions`,
`EnableCache`, `DisableCache`) have none either. The one reference to any of
them from outside `.text` is a pointer to `kExpandScratchPad` in a table at
`0x0054A404`. So the PAL build reaches no more of the emitter's routed set
than the US build did, which is none.

Of the runtime's own "NOT MODELED" paths, the two this comparison adds are
both in `ee/syscalls.cpp` and both PAL-only in origin
(`GetOsdConfigParam2`, and `GetEntryAddress`'s per-index line, which the US
build reaches too but only logged once). The rest are unchanged: `sndn2`,
`mcserv`, `padman`, `cdvd` scmd/ncmd, `iopheap`, `loadfile`, `snd` command
words, the DMAC stall and MFIFO stubs, and `Deci2Call`. Nothing in this
comparison changed which of those the game reaches.

## What the PAL disc and its IOP modules do differently

The runtime HLEs the IOP at the SIF RPC layer, and every packet layout in
`src/runtime/sif/` was originally measured against the US disc and
`SCUS_971.13`. This section is the measured re-check against the PAL disc
and `SCES_507.60`, done on 2026-09-04. The discs were read with an ISO 9660
reader in a scratch directory; nothing was copied into the tree.

The headline result is that there is no IOP RPC protocol difference at all.
Every Sony IRX the game loads is byte for byte identical on the two discs,
including the IOPRP image that replaces the IOP kernel, and every EE-side
client function that speaks to them is identical modulo relocation. The
differences that exist are in what the discs carry, what the game names, and
one PAL-only code path.

### The IRX modules on each disc

File sizes from the ISO 9660 directory records; module name and version from
the 16-bit version field of each file's `.iopmod` section.

| file | `.iopmod` name | version | PAL bytes | US bytes | same file |
|---|---|---|---|---|---|
| `IOPRP224.IMG` | (IOP kernel image) | SDK 2.2.4 | 201065 | 201065 | yes |
| `SIO2MAN.IRX` | `sio2man` | 2.4 | 6161 | 6161 | yes |
| `PADMAN.IRX` | `padman` | 4.2 | 43861 | 43861 | yes |
| `MCMAN.IRX` | `mcman_tool` | 2.21 | 87789 | 87789 | yes |
| `MCSERV.IRX` | `mcserv` | 2.12 | 6777 | 6777 | yes |
| `LIBSD.IRX` | `Sound_Device_Library` | 1.4 | 26285 | 26285 | yes |
| `SNDN2DRV.IRX` | `sndn2_driver` | 1.0 | 20941 | 20925 | no |
| `MCXMAN.IRX` | `mcxman` | 2.1 | 5745 | absent | PAL only |
| `MCXSERV.IRX` | `mcxserv` | 2.1 | 4953 | absent | PAL only |
| `PANICSYS.IRX` | `System_Panic_Reporter` | 1.1 | 3465 | absent | PAL only |

"Same file" is a SHA-1 comparison of the whole file, not just the header.
Because `IOPRP224.IMG` is identical, the rebooted IOP kernel, `cdvdman`,
`cdvdfsv` and `sifcmd` are the same on both discs, so there is no cdvd, N
command or S command protocol difference to model.

`MCXMAN`, `MCXSERV` and `PANICSYS` are on the PAL disc but no string in
`SCES_507.60` names any of them. The ELF names exactly six IRX paths plus
the IOPRP image, and it is the same six the US ELF names, in the same order.
Nothing loads them, so no server of theirs is ever bound. They are mastering
payload, not a protocol difference.

`SNDN2DRV.IRX` is the game's own sound server rather than a Sony one, and it
is the one file that differs. Its `.iopmod` header is identical on both
discs (same name, same version 1.0, same 14080-byte text, 1600-byte data and
19664-byte bss), and its `.text` and `.data` are the same size. The 16 extra
bytes are two extra `.rel.text` relocation entries. Its `.text` contents do
differ, so it is a different build, but what the runtime models is the
EE-side client protocol, and that is identical: see the RPC table below.

### The RPC surface

Every `sceSifBindRpc` server id and every `sceSifCallRpc` function number was
enumerated out of both ELFs. The sets are identical, with the same number of
reference sites for each id. The runtime registers a service for every id
either binary binds.

| server id | bound by | runtime |
|---|---|---|
| `0x736E646E` (`"sndn"`) | `SgSndn2RemoteInit` (PAL 0x00276AD0) | `sif/sndn2.cpp` |
| `0x80000001` | `sceFsInit` (PAL 0x00260C70) | added, loud stub, `sif/cdvd.cpp` |
| `0x80000003` | `sceSifInitIopHeap` (PAL 0x00263C40) | `sif/cdvd.cpp` |
| `0x80000006` | `_lf_bind` (PAL, libkernl) | `sif/cdvd.cpp` |
| `0x80000100` / `0x80000101` | `scePadInit` (PAL 0x00267C00) | `sif/pad.cpp` |
| `0x80000400` | `sceMcInit` (PAL 0x00268E78) | `sif/mc.cpp` |
| `0x80000592` | `sceCdInit` | `sif/cdvd.cpp` |
| `0x80000593` | `_sceCd_scmd_prechk` | `sif/cdvd.cpp` |
| `0x80000595` | `_sceCd_ncmd_prechk` | `sif/cdvd.cpp` |
| `0x80000596` | `PowerOffCB` | loud stub |
| `0x80000597` | `sceCdSearchFile` | `sif/cdvd.cpp` |
| `0x8000059A` | `sceCdDiskReady` | `sif/cdvd.cpp` |

The `sceSifCallRpc` function numbers match one for one as well, across
libcdvd (`sceCdRead` 0x01, `sceCdStream` 0x09, `sceCdStatus` 0x0C,
`sceCdReadIOPm` 0x0D, `sceCdNcmdDiskReady` 0x0E, `sceCdBreak` 0x16,
`sceCdMmode` 0x22, `sceCdGetDiskType` 0x03, `sceCdGetError` 0x04,
`sceCdReadClock` 0x01 on the S command server), libmc (0x01 through 0x15),
libpad (fno 1 for every call, with the command in word 0 of a 128-byte
block) and libsndn2 (0x64 batch, 0x65 init).

EE-side client code was compared instruction by instruction with immediates
and relocated fields masked out. All 29 `libpad.a` functions, all 26
`libmc.a` functions and all 61 `libsndn2.a` functions are identical. 36 of
the 37 `libcdvd.a` functions are identical; the exception is
`sceCdReadClock`, where the PAL build links a revision carrying two extra
`scePrintf` debug calls the US build does not have (`Libcdvd call Clock read
1` and `2`, present in the PAL ELF's rodata and absent from the US ELF's).
The wire protocol is unchanged: read off the PAL asm directly, it is still
fno 1 on server `0x80000593` with no send data and a 16-byte reply whose
word 0 is the result and whose bytes 4 through 11 are the `sceCdCLOCK`, which
is what `svc_scmd` already answers. `sceCdReadClock` is live on PAL: it is
called from `appendLogFile`, `la_save_processing` and
`la_system_save_processing`.

### The two sifcmd packets the runtime used to ignore

The log lines `unhandled sifcmd packet cid=0x80000003 size=104` and
`sifcmd INIT_CMD with opt=1: no response modeled` appeared on both discs.
Both were reading the protocol wrong, and both are now handled. Neither is a
PAL/US difference: the relevant libkernl functions are identical on the two
ELFs. They are recorded here because this pass is what settled them.

`cid=0x80000003` is `SIF_CMD_RESET_CMD`, the IOP reboot. Layout read off
`sceSifResetIop` (PAL 0x00264838): 104 bytes, argument length at +0x10, mode
at +0x14, NUL terminated image path from +0x18, capped at 0x50 bytes. The
only caller is `sceSifRebootIop` (PAL 0x002649D8) from `file_Init` (PAL 0x0010ECA8, at
0x0010ED58). The argument is not the image path alone: measured off the
packet on the 2026-09-04 22:07 run it is `rom0:UDNL cdrom0:\IOPRP224.IMG;1`,
arg_len 32, the `rom0:UDNL ` prefix being the rodata string at `0x00636720`. The EE does wait on it,
but through SMFLAG rather than a reply: right after the DMA it clears
`SIF_STAT_SIFINIT` and `SIF_STAT_CMDINIT`, then spins in `sceSifSyncIop`
until `SIF_STAT_BOOTEND` appears and in `sceSifInitCmd` until
`SIF_STAT_CMDINIT` appears.

That exposed a second error. SMFLAG is the IOP's register, so an EE write to
it clears the bits written, and the two writers in `SCES_507.60` are both
clears (`sceSifResetIop` and `sceSifSyncIop`, PAL 0x00264990; those are the
only `sceSifSetReg` sites in the whole PAL `.text` whose register argument is
4). The runtime was ORing instead, which left the bits standing through the
reset and made both spins pass for the wrong reason. `sif/sif.cpp` now
clears on an EE write to SMFLAG and sets on an EE write to MSFLAG, and
`rpc.cpp` answers `RESET_CMD` with a deferred `rt_sif_iop_boot_end()` that
puts the three bits back. Deferred is load bearing: setting them inside the
`SifSetDma` would be undone by the EE's own clears a few instructions later.

`INIT_CMD` arrives in two forms and the reply was going to the wrong one.
`sceSifInitCmd` (PAL 0x00265418) sends opt 0, 20 bytes, with the EE's sifcmd
receive buffer at +0x10, and waits for nothing. `sceSifInitRpc` (PAL
0x0025F770) sends opt 1, 16 bytes, and then spins at PAL 0x0025F8C8 on
`sceSifGetSreg(0)` until it reads nonzero. The `SET_SREG(0,1)` reply belongs
to the opt 1 packet. The runtime was answering opt 0 and logging opt 1 as
unmodeled; the boot survived only because `sceSifInitRpc` calls
`sceSifInitCmd` first, so the reply to the wrong packet happened to land
before the spin. The handshake runs twice per boot, because
`sceSifRebootIop` clears the "rpc up" flag through `sceSifExitRpc` (PAL
0x0025F910).

Both corrections are covered by `icorecomp-sif-selftest`
(`src/runtime/sif/sif_selftest.cpp`), which drives `file_Init`'s exact
sequence against stubbed runtime services and checks that every spin in it
can complete. Twenty checks, all passing. They cannot be checked by running
the port, because in the working order both are satisfied by accident.

### The fileio server, and why PAL reaches it

`sceFsInit` binds server id `0x80000001`, and the runtime registered no
service for it, so a BIND was a fatal. Two paths reach it, and the first is
not an error path at all.

Measured, on the 2026-09-04 22:07 run: a normal boot binds it once and sends
one `fno=0xff`, from `GetRomName`'s `sceOpen` of `rom0:ROMVER` inside
`sceScfGetLanguage`, with every thread still alive afterwards.

    [rpc] BIND sid=0x80000001 (fileio) client=0x0072d500 -> server=0x070380
    [rpc] CALL #866 fileio fno=0xff send=4 recv=0x0072ce80+4 rmode=1 thread=13

The second path is the EE exception handler `debugEEExceptionMain`, which
calls `initLineTraceTable` (PAL 0x001B4FA0) to open `TRTABLE.BIN` off the
disc root and turn a faulting PC into a source line. It destroys every other
thread first, so the thread inventory tells the two apart.

Both ELFs contain that code and both name `TRTABLE.BIN`, `SRCFILE.TXT` and
`TRFILE.TXT`. Only the PAL disc ships them, along with `MAIN.MAP`, the
linker map. The US master left all four off. So on PAL the exception path
can actually do something, and on US it could only fail at the open. Either
way the runtime answered the first `sceOpen` of the run, and a guest crash
with it, with a fatal about an unbound RPC server.
`sif/cdvd.cpp` now registers `0x80000001` as a loud stub that answers -1,
which the EE library turns into a failed open: `GetRomName` prints
`Can't open rom0:ROMVER` and leaves its buffer zeroed, which is the branch a
retail console takes, and the exception handler skips the source line and
still prints its register dump.

### Pad, memory card, sound

Pad: `PADMAN.IRX` is the same file on both discs and its `.iopmod` version is
0x0402. The runtime reported 4.0 from `GET_MODVER`; it now reports 4.2, the
version the mounted disc's module actually carries. Only the major is load
bearing: `scePadInit` (PAL 0x00267C00) shifts the word right by 8 and
refuses to continue unless the result is 4. The DS2 init state machine, the
`pad_data` layout, the actuator path and the second port are all driven by
`libpad.a`, which is identical on the two ELFs, so none of them changes.

Memory card: `libmc.a` is identical on the two ELFs and `MCMAN.IRX` /
`MCSERV.IRX` are the same files, so the mcserv protocol, the 0x1F0 slot
record and the FAT emulation in `sif/mc.cpp` are unchanged. What changes is
the directory name the game asks for: `BESCES-50760ico` on PAL against
`BASCUS-97113ico` on US, with the same two files inside (`icon.sys` and
`boy_blk.ico`). The runtime takes that name from the guest's own `sceMcMkdir`
and `sceMcChdir` arguments and never spells it itself, so nothing needed
changing; `mc_selftest.cpp` exercises the PAL name. On a card with no save
the `ChDir` fails, measured as `[mc] ChDir port=0 '/BESCES-50760ico' -> -4
(cwd=/)` on the 2026-09-04 22:07 run: `sceMcResNoEntry`, the libmc code for
a directory that is not there. That failure is what sends the boot to the
two screens described in "The two boot screens, and when they appear".

Nothing on the boot path ever creates that directory. `kanbanBootMcCheck`
runs the check twice (once before the language screen and once after the
50/60 Hz screen) and clears bit 1 of the request flags both times
(0x001B9464), so it issues `ChDir` and never `Mkdir`; on the second failure
it goes to state 0x12D and puts up the memory card message, and the item the
player picks there only decides between leaving the check (0x29) and running
it again. That is why a boot-to-gameplay run shows two `ChDir -4` lines and
no `Mkdir`, `Open` or `Write` at all.

The message itself is layout record 5, `la_boot_confirm_memory_card`
(`kanbanReqAdd` at 0x001B8738 indexes the 0x38-byte layout array at
0x00533FE8 by its first argument), with the text behind it chosen by
`D_0063C3A8`: record 3 for no card, record 4 when the card has fewer than
0x168 free blocks. Its two items are 0x29 and 0x2A; state 0x12E compares the
chosen one against 0x29 at 0x001B98B8 and, on a match, ends the check and
lets the boot continue, and on 0x2A goes back to state 1 and runs the card
check again. Which of the two is worded "yes" is inferred from the layout
geometry and the confirm targets, not read: the words live in a texture.

The directory is created by the save path, and the player reaches it by
saving in the game. `func_001537B0` calls `lt_switch_layout(0x1C)` at
0x001549F0 once the boy and the girl are both parented to the save couch for
0x3C fields, which runs the confirm prompt (record 0x1C) and then the chain
`0x1E la_save_game_memory_card_check` -> `0x21 la_mc_save_file_select` ->
`0x22 la_save_start_check`. On a card whose slot bitmask is empty,
`la_save_start_check` returns 0x27 (0x001BC858), which is
`la_system_save_processing` (0x001BCBA8): it writes `icon.sys`,
`boy_blk.ico`, the product block and then ten game blocks (its state 0 seeds
the index from 0 and its state 10 at 0x001BCE74 re-enters state 7 with the
index incremented while it is below 10), and returns 0x26,
`la_save_processing`, which writes the chosen slot. The only other entry to
0x27 is `la_format_processing` returning it at 0x001BE768. Nothing on the
title screen, the option screen or the boot prompt reaches it; a run scanning
every `jal lt_switch_layout` found no caller passing 0x27.

`iosMcMgrSaveSeg` (0x00138FE0) sets bit 1 before calling `iosMcMgrChdirProduct` (0x00138D40), so every
segment it writes runs `GetInfo`, `Mkdir`, `ChDir`, `Open` with mode 0x203,
one or more `Write`s, `Flush` and `Close`. The segment table at 0x0029B590
names the files: segment 1 `icon.sys`, segments 2 to 4 `boy_blk.ico`,
segment 0 the product block (the save directory's own name again) and
segment 5 `game.` plus a `%3.3d` index. Segments outside 1..4 get a trailing
4-byte checksum word after the body. The sizes come from the icon
descriptors at 0x0055F70C: `icon.sys` is 964 bytes and `boy_blk.ico` is
95624, both streamed off the disc, and the product block is the 0x1F0-byte
slot record.

Two results carry the whole sequence. `Mkdir` on a name that already exists
must answer -4: `iosMcMgrChdirProduct` continues on 0 and on -4 and aborts
the save on anything else, and every segment after the first hits that case.
`ChDir` on a directory that is not there must answer -4 as well, which the
same function rewrites to -14 so its caller can tell "no save yet" apart from
a card error. Both match mcman (ps2sdk `iop/memorycard/mcman/src/ps2mc_fio.c`,
`mcman_open` and `mcman_chdir`, read as a behavioural reference), and
`mc_selftest.cpp` replays the whole create sequence, thirteen files including
the ten game blocks, and checks each result and each resulting length.

`sif/mc.cpp` logs every step of that sequence at info level, names the card
path on `Write`, `Flush` and `Close`, reports the byte and call totals when a
written file is closed, says once when the guest first creates anything on
the card, and warns whenever a result is not the one mcman would have given:
a `Mkdir` that is neither 0 nor -4, a failed create-`Open`, a short `Write`,
or an operation on a handle that is not open. So a plain run's log answers,
by itself, whether the game attempted a save and what each step returned.

Sound: the SNDN2DRV protocol is unchanged despite the different module
build. Read off the PAL asm: `_SgInit` (PAL 0x002768E8) sends fno 0x65
synchronously with a 64-byte send and receive whose word 0 is 0x1E, and
`_SgCalledTickProc` (PAL 0x00273300) sends fno 0x64 with mode 1, which is
exactly what `sif/sndn2.cpp` parses. The voice count 0x1E, the batch record
layout, the ack word and the SPU upload path all match. The 50 Hz field
step is not an IOP-side fact and needed nothing here: the batch is flushed by
the game's own per-field call, and `snd/engine.cpp` already sizes each
flush from the programmed video mode (960 frames exactly per PAL field
against 800.80 on NTSC), so the stream ring paces itself off the guest.

The bank upload path is the same too, and it is now measured rather than
assumed. `sound/s_init.c soundBDDataSet` (PAL 0x00143D68) is the only caller
of `SgDmaWrite` the game uses; for each chunk it waits on the ack word,
copies the bytes into its one 0x78000-byte IOP heap block with a raw
`sceSifSetDma`, and queues the cmd 0x20 that tells the driver to move them
into SPU RAM. A PAL boot issues seven of them, all reading IOP 0x090000 and
filling SPU 0x005010..0x1b8210, while the loader streams `COMMON.DF`, well
before the language screen. `sif/sndn2.cpp` now checks each transfer's
source against what an EE to IOP DMA actually wrote (`snd/iop_stage.h`) and
warns when bytes are missing, and `snd/engine.cpp` answers "was a bank
uploaded here" from that transfer history rather than from the bytes at the
address, since a zero run inside a bank is ordinary content.

SE volume on the boot screens is a separate question, and the factor chain
behind it is now traced end to end in `src/runtime/sif/SNDN2_NOTES.md`.
`_SgSeqSeVolume` (PAL 0x00275250) multiplies six slot-context bytes, a pan
byte from the ELF table D_0054CB78 and the game's own per-slot level, and
each of those comes from the bank's .hd image in EE RAM or from a constant
in the retail ELF. Nothing this runtime answers is in the chain: the OSD
config word reaches only `sceScfGetLanguage` and only from
`kanbanBootMcCheck`, the memory card game block applies one sound value
(`soundOutputModeSet`) and no level, and the cursor SE takes neither the 3D
attenuation branch nor a fade at key-on. The arithmetic is not the cause
either: the retail `__muldi3` (PAL 0x0027C198) was stepped through the
reference interpreter from the decomp's own encodings and matches a native
64-bit multiply for every operand pair tried, with the whole eight-call
chain yielding 0x3C94 at full scale, and the per-op three-way suite is
green. So the zero is neither what a console computes on these screens nor
a value the runtime supplied, and what remains is one of the .hd bytes
reading zero in EE RAM.

That is now measured, and it was none of them. MEASURED on the PAL boot,
2026-09-05: every bank byte behind voice 4's level is intact (expression
0x64, channel volume 0x64, tone volume 0x1E, velocity 0x5F, program volume
0x7F, SE master 0x7F, pan 0x7878, and the SE table's bytes read the same
live). The only zero is the caller level, the word `SgSetSeVolDirect` (PAL
0x00277FB8) puts in the sequence entry, which `SgSePlay` had seeded with
0x1000. So the bank and the read path that delivered it are both cleared,
and the question is now the game's own level: `soundSeVolSet` (PAL
0x001443F0) forms it from three inputs of the game's SE slot, and the
generated C for that whole path was read against the retail instructions
and matches. `src/runtime/sif/SNDN2_NOTES.md` carries the chain and what
the next log will name.

Which one is now measured rather than argued. `src/runtime/guest/ico_syms.h`
carries a sound block with the three table addresses the factors live behind
(slot context 0x00731C00 stride 0x58, sequence context 0x00732C80 stride
0x54, vab table 0x00731600 stride 0x0C) and the .hd's +0x20 / +0x40 SE-table
pair, each decoded out of SCES_507.60 after the decomp's listing was matched
against it word for word. `snd/engine.cpp` reads them when cmd 0x01 hands a
voice a level of zero and logs the eight factors, which of them are zero,
and for the SE-table terms both the EE address and the .hd file offset.
Guest memory is read, never written. The .hd itself comes from
`ReadSoundHdFile` (PAL 0x001AB1E8), which mallocs an EE buffer and fills it
with one `iosCdvdHandlerRead` of the packed file entry (the boot banks come
from COMMON.DF, DATA.DF entry 0), so a zero at a named
file offset separates a byte the disc carries from a byte the runtime's read
path failed to deliver.

### CDVD, files and DATA.DF

`DFDATAS/DATA.DF` has the same outer table format on both discs: a u32 entry
count then that many 40-byte records of `{char name[32]; u32 offset; u32
size}`. `ui/data_df.cpp` reads the PAL container correctly with no change.
What differs is the entry set: 193 entries on PAL against 172 on US, 168 in
common. The PAL-only names are the ten per-language text blobs
`data_EG01/02`, `data_FR01/02`, `data_GR01/02`, `data_IT01/02` and
`data_SP01/02` where the US disc has a single `data.jim`, eleven extra `.int`
scripts, and two extra attract movies, `pal_advertise.pss` and
`pal_advertise576.pss`, beside the `advertise.pss` both discs carry. So the
language variation is inside the container, selected by entry name, and
there are still no per-language files on the disc itself.

Sector addressing, `sceCdStRead` and the banked stream model are unaffected:
the streaming client is `libcdvd.a`, identical on the two ELFs, and the ring
geometry comes from the game's own `iosCdvdMgrStStart` (PAL 0x001336D0) and
`iosCdvdDirectStOpen` (PAL 0x00135138), which the masked comparison also
finds identical. The PAL disc is larger (443216 sectors against 271531) but
the runtime's ISO layer reads the mounted image's own directory records, so
size is not a model input.

### The pack loader reads through `sceCdRead` on PAL, `sceCdStRead` on US

This is a real code difference between the two ELFs, not a data one, and it
is the first place a PAL boot goes somewhere a US boot never went.

On US the unifile/pack loader reads the DATA.DF index through the CD stream:
the loop at US 0x00131C90 calls `sceCdStRead` directly, and the working US
log shows `sceCdStINIT` / `sceCdStSTART` / a run of `sceCdStREAD` right after
the `searchfile` hit.

On PAL the same load goes
`iosCdvdMgrLoad` (0x001338A8) -> `iosCdvdMgrStStart` (0x001336D0) ->
`iosMsgSend` on the stream request queue -> a dedicated producer thread
running `func_00133250` (0x00133250, created in `func_001345C8` at
0x00134654, priority 0x1B, stack 0x006B8830+0x4000), which calls
**`sceCdRead`** (call site 0x001333FC) in chunks capped at 16 sectors
(0x001333B8-0x001333D0), each followed by `sceCdSync(0)` and
`sceCdGetError`. The consumer side is `iosCdStRead` (0x00133ED8), which sets
the "notify me" flag `D_0063A388` and blocks in `iosMsgRecv` on the notify
queue `D_0029B3E0` at 0x00133F90; the producer sends it a token at
0x00133494 after each chunk. The whole game is parked behind that: the CDVD
manager thread only reaches `SignalSema(IosSndLock)` (0x0013469C) after the
load returns, the StageManager thread is in `WaitSema(IosSndLock)`
(0x001A8B14), and the game main thread is in `WaitSema(systemFault)`
(0x00101DC4).

`iosCdvdMgrStStart` sets the remaining-sector count to 423430 and the ring
to 896 sectors, so a healthy PAL boot issues **56 consecutive 16-sector
`sceCdRead` calls** before the ring fills. One read followed by silence is
the producer thread having stopped, not the load having finished. The two
boot screens are downstream of this load, but their absence is not on its
own evidence of a stall: `kanbanBootStart` (0x001B9A98) arms the state
machine from `InitIcoMisc` well after the load, and `kanbanBootMcCheck`
(0x001B92D0) only draws them when the memory card has no product block to
read (see "The two boot screens, and when they appear" above). What a run
with zero GIF packets has not reached is any drawing at all.

That producer thread is also why `sceCdNcmdDiskReady` had to be modeled
exactly. `sceCdRead` (PAL 0x00266F20, at 0x00266F68 to 0x00266F74) sends fno
0x0E first unless `_sceCd_ee_read_mode` bit 0 is set, and returns 0, failed,
the moment the answer is 6. It does not retry. `func_00133250` asserts on a
failed `sceCdRead` at 0x0013340C, and the retail assert sink
`debug_assertMessage` (0x001B6230) is a branch to itself, so the thread spins
at priority 27 for ever and the CDVD manager at 28 never runs again. That was
the 2026-09-04 boot freeze. The answers are `SCECdComplete` (2) and
`SCECdNotReady` (6), per ps2sdk `cdvdfsv_rpc5_0E_diskready` in
`iop/cdvd/cdvdfsv/src/cdvdfsv.c`, not a drive status word; and since
`cdvdfsv_rpc5_01_readee` sends the read's RPC END only after the last
sector's DMA, a poll issued after that END is answered 2 on hardware. The
runtime now holds the read's END for the transfer time instead of holding the
drive busy past it, and every 6 it does answer is a warn line naming the
`sceCdRead` it will fail.

`debug_assertMessage` itself is now loud rather than a silent spin:
`config/entry_hooks.txt` names it, and `src/runtime/hooks.cpp` logs the
guest's file, line and message at error and ends the run with `rt_fatal`.

### Log lines to check a run against

| difference | log line |
|---|---|
| PAL loads the DATA.DF index through `sceCdRead`, not the stream | `[cdvd] ncmd fno=0x01 sceCdRead(lbn=19771 sectors=16 buf=0x00319a00 mode=0x000100) intr=0x0054b640 pos=0x0054b700`, repeated (56 of them on a healthy boot); the US build shows `[cdvd] ncmd fno=0x09 sceCdStSTART(...)` and a run of `sceCdStREAD` here instead |
| IOP reboot handled | `[rpc] sifcmd RESET_CMD (IOP reboot, size=104): arg_len=32 mode=0 image="rom0:UDNL cdrom0:\IOPRP224.IMG;1"` |
| reboot answered on the timeline | `[sif] virtual IOP reboot in progress` followed, at the guest's first SMFLAG read after its own clears, by `[sif] virtual IOP reboot complete at the EE's first SMFLAG read: SMFLAG=0x70000` |
| INIT_CMD split | `[rpc] sifcmd INIT_CMD opt=0 (sceSifInitCmd, size=20): EE sifcmd receive buffer = ...` then `[rpc] sifcmd INIT_CMD opt=1 (sceSifInitRpc, size=16): SET_SREG(0,1) queued` |
| each of these appears twice per boot | the handshake runs again after the reset |
| module load named against the disc | `[iopmod] loadfile fno=0 LoadModule('cdrom0:\PADMAN.IRX;1' ...) ... version 4.2 ...` |
| a module outside the measured set | `[iopmod] WARNING loadfile fno=0 LoadModule('...') ...: this path is NOT in the measured module set of SCES_507.60` |
| padman version | `[pad] GET_MODVER -> 4.2 (PADMAN.IRX .iopmod version 0x0402 on the mounted disc; ...)` |
| DATA.DF entry count (193 on PAL, 172 on US) | `[ui] DATA.DF: outer table has 193 entries; ...` |
| fileio reached (once on any normal boot, from `GetRomName`; again with every other thread destroyed after a guest crash) | `[iopmod] WARNING fileio (0x80000001) fno=0xff NOT MODELED ...: answered -1 (a failed open)` |
| any unmodeled sifcmd left | `[rpc] WARNING sifcmd packet cid=... NOT MODELED: ignored, no reply sent` |
| any unmodeled INIT_CMD opt | `[rpc] WARNING sifcmd INIT_CMD with opt=... NOT MODELED: no reply sent` |

Neither of the two `NOT MODELED` sifcmd lines should appear on a PAL run:
the only cids `SCES_507.60` sends are `INIT_CMD`, `RESET_CMD` and the RPC
BIND/CALL/RDATA ids, and the only opts it sends are 0 and 1. If either
appears, the packet it names is new information and the line carries the cid,
the opt and the size needed to model it.

### The boot handshake, settled by a run

The reboot and INIT_CMD corrections changed the order in which the boot
handshake's spins are released, and the 2026-09-04 22:07 run shows the whole
sequence in the order this section predicted: `INIT_CMD opt=0` then
`opt=1` with `SET_SREG(0,1) queued`, then `RESET_CMD` with the image path
above, then `virtual IOP reboot complete at the EE's first SMFLAG read:
SMFLAG=0x70000`, then the `opt=0` / `opt=1` pair a second time. Nothing here
is waiting on a run any more.

## What CI checks, and what it does not

The `translator` job runs `cargo test --release` and
`cargo clippy --all-targets -- -D warnings` on a runner with no disc and no
retail ELF. Three tests need the boot ELF and skip with a notice when
`[inputs]` does not resolve to one: the VU decoder's ground truth against
`.vutext`, the VU emitter's smoke gate, and the VU interpreter/compiled-emit
differential. So what CI actually verifies is the parsers and the emitter's
unit behaviour: the objdump parser against its own sample, the correlation
and the boundary hazard check against synthetic inputs, the entry hook table
parser against its own strings, the emitter's hook placement in single and
merged groups, the `--out` gate, and the config file parsing.

What CI cannot check, and what is therefore run by hand before a translator
change lands: `verify-decode`, the ingest and its correlation against a real
`.text`, the three ELF-gated tests above, and the two translator stages. That
the `ee` output is unchanged is checked the same way, by diffing the
generated tree against the previous one.

## What is not done yet

- The mouse pointer on the game's own menus is unexercised: its addresses and
  offsets are measured, but nothing has confirmed them from a run.
- The gameplay and game-over layout ids, and with them the host playtime
  counter and Unscathed Escape. Both come off one run with
  the log kept (the progress-bit log is always on at info).
- Bench Warmer's bit, which no source names.
- The claims in this file are measurements off the disc's own files and
  derivations from the analog standards, except where one says it was
  measured in a run. On the shipped paraLLEl-GS backend the PAL disc boots,
  draws and plays with sound (2026-09-04); nothing has been played through.

### Interrupt enable is per thread

The PAL loader's disc producer (ios/cdvd/func_00133250, PAL 0x00133250)
runs `di` after its first read and then waits in sceCdSync's
sceCdDelayThread loop for the read's RPC END, which the guest's SIF0
interrupt handler processes. On the EE the Status.EIE bit is part of each
thread's saved context, so the other threads keep taking interrupts while
that thread sleeps. The runtime used to keep one global flag, so a thread
that blocked with interrupts disabled held them off for the whole machine
and the boot idled after the first read. The flag is now saved and restored
per thread (EEThread::eie in ee/sched.cpp) and enabled while the scheduler
runs. Log lines: `[intc] DI by thread N (interrupts disabled for that
thread only ...)` and `[intc] EI by thread N ...`. The US build never ran
`di` on this path, which is why the global flag never showed.
