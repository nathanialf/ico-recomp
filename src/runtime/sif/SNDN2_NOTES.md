# sndn2 wire protocol notes

Facts derived by reverse engineering the EE-side vendor sound library in the
user's own retail game copy (disassembly in the read-only decomp repo, TU
`vendor_258CC0` and neighbors, correlated with the aug6 prototype map's
`sound.o` symbol names) plus one instrumented boot run of this runtime. No
code was copied from anywhere; sndn2 is the game's own driver, not a Sony SDK
module, so there is no SDK source to compare against.

## Topology

- IOP module `SNDN2DRV.IRX` registers one SIF RPC server, id `0x736e646e`
  (ASCII "sndn"). The EE binds one client to it (retail `func_0025C4E0`,
  bind loop with a 10000-iteration delay between retries).
- Only two RPC function numbers exist:
  - `fno 0x65`: remote init, synchronous, 64-byte send and receive.
  - `fno 0x64`: command batch flush, NOWAIT (`SIF_RPC_M_NOWAIT`, no end
    function), variable-size send, 512-byte receive.
- Sound bank payload bytes never travel through RPC. The EE copies them into
  an IOP heap buffer (`sceSifAllocIopHeap` of 480 KB, observed alloc, plus a
  738 KB one for streaming) with raw SIF DMA, then queues a transfer command
  (below) that tells the IOP to consume them.

## EE-side state (retail addresses, aug6 names)

- `D_0071C640` (0x80 bytes): the driver control block. Accessor
  `func_00258C68` returns its address.
  - `+0x38` u16: reverb depth (set by the `soundAllocIopHeap` wrapper).
  - `+0x3A` u16: set to 0x3C at init (tick rate?).
  - `+0x3C` u32: command-ring bank selector, toggled 0/1 at every flush.
  - `+0x40` u32: write index into the current ring bank (max 0xFE).
  - `+0x44` u32: "remote is up" flag, set to 1 by init; the tick and status
    helpers early-out when it is 0.
  - `+0x48` u32: issued-command counter. Incremented once per tagged command
    (retail `func_0025C6D8`).
  - `+0x00/0x08`, `+0x10/0x18`, `+0x20`, `+0x28`: 64-bit level/status values
    that the flush path turns into untagged commands 0xC, 0xD, 0xA, 0xB when
    they change.
- `D_0071C6C0` (0x2000 bytes): double-buffered command ring, two 4 KB banks
  of 16-byte records; slot address = base + (bank << 12) + (index << 4)
  (retail `func_00258CF0`).
- `D_0071E6C0` (0x200 bytes): the status block the IOP writes back.
  `D_0071E8C0` (the "handle" global, accessor `func_00258CE0`) holds its
  uncached alias `0x2071E6C0`; all EE reads of IOP-written status go through
  that pointer.
- `D_0071EB40`: the `SifRpcClientData` for the bind.

## Init (fno 0x65)

Retail `func_0025C2F8` (aug6 `SgSndn2RemoteInit`):
1. Zeroes all of the above plus the voice table (0x30 voices, 0x58-byte
   entries; status bytes +0x50/+0x54/+0x55/+0x56 preset to 0xFF).
2. Stores `0x2071E6C0` into the handle global.
3. Synchronous CALL fno 0x65 with a 64-byte stack buffer:
   word 0 = 0x1E (constant; probably the voice/channel budget),
   word 1 = the caller's init argument, word 4 = 0, rest uninitialized.
   The 64-byte receive lands in the same stack buffer and is never read.
4. Sets `+0x44` = 1, `+0x48` = 0, `+0x40` = 0, `+0x3A` = 0x3C.

## Command records (16 bytes, two layouts)

There are two enqueue functions on the EE side, and they pack differently:

1. Tagged (retail `func_0025C6D8`), used ONLY by the DMA commands 0x20
   (`SgDmaWrite`, retail `func_0025C680`) and 0x21 (`SgDmaRead`, retail
   `func_0025C6B0`):

   ```
   w0 = command id
   w1 = (seq_tag << 8) | a1[23:16]
   w2 = (a1[15:0] << 16) | a2[23:8]
   w3 = (a2[7:0] << 24) | a3[23:0]
   ```

   `seq_tag` is the post-increment value of the issued-command counter
   (`D_0071C640+0x48`), truncated to 24 bits; a1/a2/a3 are 24-bit operands.
   For 0x20: a1 = IOP heap source, a2 = SPU RAM byte destination, a3 = byte
   length.

2. Raw (retail `func_002591F0`), used by every other command: w1..w3 are
   the caller's 32-bit arguments stored verbatim, no tag. Since w1 is
   always a voice/port number (or a packed field whose high byte is a
   voice), `w1 >> 8` can look nonzero; discriminate layouts by command id
   (only 0x20/0x21 are tagged), not by the tag byte.

## Command vocabulary (raw layout, w1/w2/w3 = enqueue args)

Voice commands (w1 = SPU2 voice number, 0..0x2F):
- `0x01` volume: w2 = VOLL, w3 = VOLR. 0..0x3FFF linear; bit 15 selects the
  SPU2 sweep-mode word `(mode << 8) | (vol >> 7)` (emitter retail
  `func_0025AC60`; sweep taken when the tone's sweep byte +0x0A is set).
  The emitter computes the value as the product chain chanVol x expression
  x toneVol x velocity x progVol x masterVol x pan x seqVol.
- `0x02` ADSR: w2 = ADSR1, w3 = ADSR2, the raw SPU2 register words straight
  from the bank's tone record (+0x06/+0x08). `SgSeStopNoRelease`-class
  stops send (0,0) then key-off: release shift 0 = instant cut.
- `0x03` start address: w2 = VAG start, SPU RAM byte address (bank slot
  base + tone's VAG offset << bank shift, computed on the EE).
- `0x04` note params (emitter retail `func_0025AC18`):
  w2 = tone[+2] << 24 | note << 16 | fine << 8 | bend_center,
  w3 = moddepth << 24 | pitch_scale[23:0] (12.12 fixed, 0x1000 = x1.0).
  No absolute pitch is sent; the IOP derives the pitch register from these.
  Observed boot SEs: center 0x58-0x5C, note 0x4A-0x4F, fine small negative,
  scale 0x1000.

Per-tick masks from the flush path (w1 = 0, w2 = voices 0..23, w3 = voices
24..47, both 24-bit halves, level-triggered from control block +0x00..0x28):
- `0x0A` KEY ON (control block +0x20)
- `0x0B` KEY OFF (+0x28)
- `0x0C` effect (reverb) send enable mask (+0x00/+0x08 pair)
- `0x0D` second mode mask (+0x10/+0x18; noise/pmon class, unresolved)

Reverb and output (w1 = core 0/1):
- `0x14` `SgSetReverbEndAddr(core, addr)`: boot sends 0x1FFFFF / 0x1DFFFF.
- `0x15` `SgSetReverbType(core, type)`: boot sends type 4 (studio-large in
  the SPU2 numbering).
- `0x16` `SgSetReverbDepth(core, dL, dR)`: boot sends 0xCCC/0xCCC.
- `0x17` `SgSetReverbDelaytime`, `0x18` `SgSetReverbFeedback` (types 7/8).
- `0x28` `SgSetMasterVol(core, L, R)`: 0..0x3FFF; boot ends at full scale.
- `0x32` parameter write: w1 = 0x0A digital output mode (boot: 0x80);
  w1 = 0x08 observed as a per-voice attribute elsewhere.
- `0x1F` `SgQuit` terminator.

ADPCM streaming (SgStAdpcm*, retail func_0025DCF0..func_0025DFB0):
- `0x3C` Init, `0x3D` Quit (no args).
- `0x3E` Open (retail `func_0025DD20`): w1 = voice << 24 | mode |
  blocksize[15:8] << 8 | spu_addr[23:16], w2 = spu_addr[15:0] << 16 |
  ring_size[23:8], w3 = ring_size[7:0] << 24 | iop_buf[23:0]. The vendor
  loads the ring size once (`lw $10, 0xC($16)`) and packs it twice, into w2
  and into w3's top byte, so both halves are needed to recover it.
  `blocksize` is a separate field (`lw $3, 0x14($16); andi $3, 0xFF00`) and
  only its bits 15:8 survive into w1. `mode` is 0x10000, 0x20000 or 0x40000
  for 1, 2 or 4 channels, so `(w1 >> 16) & 0xFF` reads as an exact channel
  count; anything else is rejected, since a chunk of 0x800/3 would break the
  16-byte VAG alignment decode_stream_block relies on.
  Boot ambience: two voices (0 and 1), nch = 2, blocksize 0x4000,
  spu 0x1E0000/0x1E4000, ring 0x5C000, iop 0x2B8000/0x2B8400.
- Ring layout, from `AdpcmOpen` in the decomp rather than inferred. There
  are exactly two stream slots. `AdpcmUseAreaGet` (`sound/adpcm_init.c`,
  inlined into `AdpcmInterLeaveVolumeSet` in the nonmatching asm) picks a
  free one out of `D_00633CB8[2]` and returns
  `base = D_00633CB0 + slot * 0x5C000`;
  the sound heap block is `0x800 + 2 * 0x5C000` = 755712 bytes. `AdpcmOpen`
  then opens one voice per channel at `base + (0x800 / nch) * ch`, so
  stereo is 0x400 apart inside a 0x800 stride. Only slot 0 has been
  observed; slot 1 would sit at base + 0x5C000. Known gap: the runtime
  models a single slot and recovers its base by stride alignment, so two
  concurrent streams are not represented.
- `0x3F` Close(voice) (w1 = voice number, not a handle; retail
  `func_0025DDB0` passes it in w1 with w2 = w3 = 0).
- `0x40` ChannelVolume(handle, L, R): w1|w2<<24 = 48-bit voice mask handle,
  w3 = L << 16 | R, each < 0x4000.
- `0x41` ChannelPitch(handle, rate): w3 = playback rate in Hz, capped at
  0x2EE00 = 192000 = 4 x 48000 (the SPU2 4x pitch ceiling). The boot
  ambience runs at 0xAC44 = 44100.
- `0x42` Play(handle), `0x43` Stop(handle).

PCM streaming (`SgStPcm*`, retail func_0025E020..func_0025E280). Derived from
the decomp asm plus one Windows run; the attract movie is the only user in
this binary and it drives the block from `ito/mpeg/mv_sub.c`.

- `0x46` Init (retail `func_0025E020`), `0x47` Quit (`func_0025E038`). No
  operands, all three words zero.
- `0x48` Open (`func_0025E050`). The caller passes a four word struct
  {channel, flags, IOP address, ring size}; the emitter rejects a channel
  >= 0x10 or either address word above 0x1FFFFF, then packs
  `w1 = channel << 24 | flags`, `w2 = IOP address`, `w3 = ring size`.
  The movie (`func_0023D8A8`) allocates one 0x6000 byte IOP buffer and opens
  two channels into it, at `buf` and `buf + 0x200`, both with ring 0x6000 and
  flags 0x00010400. Observed: `w1=0x00010400 w2=0x001b0100 w3=0x00006000`
  and `w1=0x01010400 w2=0x001b0300 w3=0x00006000`.
  Bits 23:16 are 0x01; cmd 0x3E's equivalent field is a channel count, but
  that reading does not survive here (both channels sit in one ring 0x200
  apart), so the field is left unexplained rather than given a plausible
  meaning.
- Ring layout. `func_0023DEB0` keeps one write offset (`self+0x4C`, reduced
  modulo the ring size) and `func_0023DB80` fills the ring with a single
  linear wrapping SIF DMA, so the two channels are block interleaved inside
  it: 0x200 bytes of channel 0, then 0x200 of channel 1, period 0x400. The
  runtime derives the base as the lowest opened address and each channel's
  run as the distance to the next channel up, so it assumes no channel count
  (snd/pcm_stream.h). A channel it cannot place that way is left without a
  cursor rather than given a fabricated one.
- Where 0x400 comes from. MEASURED: it is the refill quantum. `func_0023DEB0`
  reduces its free-space figure to a whole number of 0x400 byte units with
  the compiler's signed `x / 0x400 * 0x400` idiom, `sra $2, 10; sll $4, $2,
  10` at 0023DF78 and again at 0023DFDC (the `addiu 0x3FF` / `movn` pair
  ahead of each is the negative-value bias). The 0x200 channel spacing tiles
  a period of 0x400 exactly, and bits 15:8 of the flags word also hold 0x400.
  INFERRED: that bits 15:8 are that interleave block. Cmd 0x3E is not
  evidence for it either way: the runtime reads 0x3E's bits 15:8 into
  `st_blk`, the IOP transfer block the reported cursor is quantized to
  (engine.cpp), while the ADPCM interleave stride there is a separate
  hard-coded 0x800. snd/pcm_stream.h marks the same conclusion INFERRED.
- `0x49` Close(channel) (`func_0025E0C0`): w1 = channel, w2 = w3 = 0.
- `0x4A` ChannelVolume(mask, w2, w3) (`func_0025E1E8`): w1 is a channel mask
  with bits 31:24 required zero, w2 and w3 are each 0..0x7FFF.
  `func_0025E1E8` has four callers, all in `ito/mpeg/mv_sub.c`. The two play
  paths, `func_0023E298` and its near copy `func_0023E368` (restart/resume),
  send (1, 0, vol) and (2, vol, 0), one hard pan per channel, or
  (3, vol/2, vol/2) when the byte at `self+0x58` is set; vol is the word at
  `self+0x5C`. The two teardown paths, `func_0023E228` (0023E240) and
  `func_0023E330` (0023E348), send (3, 0, 0) as a mute immediately before
  Stop(3). Nothing in the decomp names the two operands; the runtime reads
  w2 as left and w3 as right to match cmd 0x01 and cmd 0x40, which puts
  channel 0 on the right. Unresolved: the stereo image may be mirrored.
  The vol the movie carries is 0x3FFF, and it is a constant in the caller,
  not a measurement of anything: `func_0019D678` passes it as StageOrientInit
  argument 7 (`addiu $10, $0, 0x3FFF` at 0019D7F4), StageOrientInit keeps it
  in $22 (0019D2A0) and passes it as argument 3 to `func_0023D8A8` (0019D398),
  which stores it at `self+0x5C` (0023D8CC, 0023D9BC).
- `0x4B` Play(mask) (`func_0025E118`), `0x4C` Stop(mask) (`func_0025E158`).
  Same mask rule. Observed with mask 3.
- `0x4D` (channel, value) (`func_0025E198`): w1 = channel < 0x10,
  w2 <= 0x1FFFFF, w3 = 0. Two callers, both in `ito/mpeg/mv_sub.c` and both
  sending (0, 0) then (1, 0) immediately before Play(3): `func_0023E298`
  (0023E2AC/0023E2B8) and its near copy `func_0023E368`
  (0023E37C/0023E388), the restart/resume path. So the only observed effect
  is starting a channel at the bottom of the ring. Reading w2 as a ring
  offset is inferred; both the zero and the nonzero case are logged, and a
  channel that is not open is left alone.
- `0x4E` (`func_0025E100`): w1 forwarded with no validation. The movie sends
  8, once, after opening both channels. Meaning not established.
- `0x4F` (`func_0025E280`): w1 = mask, w2 <= 0x1FFFFF, w3 < 2. Never sent.
- Handshake. The EE polls how far the driver has consumed the ring with
  retail `func_0025E238`, which reads status block word
  `+0x180 + channel * 4` through the same uncached status pointer
  `func_0025DFB0` uses. `func_0023DEB0` turns it into the refill size as
  `free = ((cursor + ring - write_ptr) - 0x400) mod ring`, rounded down to
  0x400, so the word is a byte OFFSET WITHIN THE RING like the ADPCM cursor
  at +0xC0, not an address, and 0x400 is a deliberate guard band between the
  driver's read point and the EE's write point. Only channel 0 is polled.
  Leaving the word at zero, as this runtime did before the block was
  modelled, tells the movie the driver has consumed nothing.
- Starvation. The driver has no way to stall for the EE: a real IOP hands the
  ring to the SPU2 core input on the hardware's own 48 kHz clock and plays
  whatever the memory holds. The model does the same rather than stopping,
  and counts the blocks the EE failed to refill.
  The sound service never sees the EE's write pointer, because the movie
  fills the ring with raw SIF DMA rather than through this service. The DMA
  does pass through the runtime, though: every EE to IOP transfer entry
  reaches `rt_rpc_on_dma_entry` (sif/rpc.cpp), which calls
  `rt_snd_pcm_note_iop_write`. That is the write-side witness. Each
  interleave block of the ring carries the value of a counter bumped once per
  DMA touching the ring, and a block is counted stale when the play cursor
  enters it and finds the same stamp it left there on the previous lap: a
  whole lap of playback with no write to those bytes. Content is never
  compared, so repeated content (silence, most obviously) is not mistaken for
  starvation, which a per-block content checksum cannot avoid.
- Sample format. Not established from the decomp. The runtime plays the ring
  as 16 bit signed little endian at 48 kHz, 1:1 with its own output, on the
  grounds that the ADPCM family carries an explicit rate command (0x41) for
  its varying source material while this family has none, which only works
  for a driver fixed at the hardware's own rate. engine.cpp logs the first
  bytes and the peak of the first run at Play so a log settles it.

## Pitch unit

The SPU2 pitch register convention (0x1000 = one input sample per 48 kHz
output tick, i.e. 0x1000 plays a 48 kHz encoding at native speed) is
confirmed by `SgStAdpcmChannelPitch`: it passes a raw rate in Hz with an
EE-side cap of 0x2EE00 = 192000 Hz = exactly the hardware's 4x ceiling
(pitch register max 0x3FFF is 4x 0x1000), so the IOP computes
pitch = rate * 0x1000 / 48000. Sequenced voices never receive an absolute
pitch; cmd 0x04 sends note/fine/center plus a 12.12 scale and the IOP does
the note math. The runtime uses the convention note == center => 0x1000
(2^(1/12) per semitone, fine in 1/16 semitone); the boot SEs land at
~0.45x = ~21.5 kHz, consistent with 22.05 kHz source encodings.

## Bank format facts (EE side; nothing IOP-visible)

The .hd header is parsed entirely on the EE (retail bank open rebases table
pointers at header +0x10/+0x18/+0x1C/+0x20/+0x24 into absolute pointers at
+0x30..+0x44). Only the .bd VAG bodies are uploaded to SPU RAM (seven 0x20
transfers for the SE bank, 0x5010..0x1B7250). Note-ons then carry explicit
SPU addresses/ADSR/note data, so this HLE never parses a bank header: the
16-byte tone records (+0x02 center?, +0x03 fine, +0x04 VAG offset in
shifted units, +0x06 ADSR1, +0x08 ADSR2, +0x0A sweep, +0x0D used by the
voice state machine, +0x0F bit 7 reverb flag) stay an EE-side concern.

## Flush (fno 0x64)

Retail `func_00258D10` (aug6 `SgCalledTickProc` body), run from the vblank
path when `+0x44` is set (`func_0025C638`):
1. Runs the 0x30-entry voice state machine, queueing per-voice commands.
2. Queues untagged level commands 0xC/0xD/0xA/0xB if those fields changed.
3. CALL fno 0x64, mode NOWAIT, no end function:
   send = current ring bank, size = write_index * 16 (0 is normal when
   idle), recv = the status block via its uncached alias, recv size 0x200.
4. Resets the write index and toggles the ring bank.

## The ack (what fixed the boot livelock)

- Status block word `+0x1C0`, written by the IOP, must equal the EE's issued
  counter `D_0071C640+0x48` for the library's sync to complete.
- Retail `func_0025C768` (aug6 `SgSndn2RemoteSync(mode)`): mode 1 spins
  until `*(handle+0x1C0) == *(D_0071C640+0x48)` (RAM-only spin; on real
  hardware the vblank interrupt keeps the tick/flush running underneath it),
  mode 0 polls the equality once, other modes return -1.
- The IOP server therefore acknowledges commands by echoing the seq tag of
  the last tagged command it has finished into status `+0x1C0`. Because our
  RPC layer runs handlers synchronously, echoing the last tag seen in the
  batch into the receive buffer is exact.
- 24-bit truncation: the EE compares full 32-bit words, so a real IOP must
  widen the tag; the counter never gets near 2^24 in practice and this HLE
  echoes the 24-bit value (documented limitation in sndn2.cpp).

Beyond the ack, the EE reads the per-voice STREAM READ CURSORS from the
status block: `+0xC0 + (voice % 24) * 4 + (voice / 24) * 0x60` (retail
`func_0025DFB0`, aug6 `SgStAdpcmIopReadAddr`) holds the byte OFFSET WITHIN
THE RING, 0 .. ring - 1, that the driver will consume next for that stream
voice. It is not an address; see the ADPCM streaming section below for the
derivation from `ACTSetEnvAllmighty`. The game's adpcm tick uses the
per-tick delta to schedule ring refills, so the HLE fills these words every
flush (engine.cpp `rt_snd_fill_status`).

## Boot behavior after the fix

`soundDataOpenSync` loads the SE bank in seven 0x20 transfers (tags 1-7),
each preceded by a raw EE-to-IOP copy and followed by a mode-1 sync; then
regular per-field fno 0x64 flushes continue (mostly empty, occasionally
voice commands when the game plays SEs). See the endpoint log excerpt in the
task report / commit message.

## Native engine (src/runtime/snd, src/runtime/host/audio.*)

sndn2.cpp forwards every record to the engine and renders one field of
48 kHz stereo audio per fno 0x64 flush (48000 * 1001 / 60000 = 800.8
frames, fraction carried). Implementation notes and known deviations:

- VAG decode is the public Sony spec, verified bit-exact against the decomp
  repo's tools/decode_vag.py via `ICORECOMP_SND_SELFTEST=prefix` (dumps the
  first keyed-on VAG and its decode; compare with
  snd/tests/vag_compare.py). Dumps are ROM-derived: keep them in /tmp.
- ADSR follows the public SPU2 envelope spec (shift/step/exponential
  flags), clocked at 48 kHz.
- cmd 0x01 sweep-mode volume words are flattened to their 7-bit level (no
  ramp yet). The boot SEs arrive with volume (0,0) from the EE's own
  factor chain, so a faithful boot render is silent SE-wise;
  `ICORECOMP_SND_UNITY_VOL=1` overrides voice/master volumes for pipeline
  verification.
- Reverb is a Schroeder/Moorer send bus keyed off the type/depth commands
  (0x15/0x16) and the 0x0C send-enable mask, not an SPU2 DSP model.
- Streams decode straight out of the virtual-IOP ring (interleave
  de-chunking as above) and play whatever the ring holds. Refill is paced
  entirely by the cursor the runtime reports back at status +0xC0 +
  (v % 24) * 4 + (v / 24) * 0x60, read by retail `func_0025DFB0`. That word
  is a byte OFFSET WITHIN THE RING, 0 .. ring - 1, not an IOP address:
  `ACTSetEnvAllmighty` (the read callback `AdpcmOpen` registers with
  `iosCdvdChgFileName`) keeps its own `PREV` in the same units, takes
  `consumed = CUR - PREV`, refills when that passes 0x1EAAA (a third of the
  ring) or the cursor wraps, reads `consumed` bytes to `ring_base + PREV`,
  then advances `PREV` or resets it to 0 when it would leave the ring.
  Reporting an address there made every refill ask for `address` bytes and
  land back at offset 0, which played the ambience as fragments from all
  over its file.
- The cursor moves in whole transfer blocks (`blocksize`, 0x4000 here), not
  in decoder steps. `func_00132DC0` converts the byte delta to sectors by
  truncation (`sra $22, 11`; the `+0x7FF` applies only to the negative
  branch) while `ACTSetEnvAllmighty` advances PREV by the untruncated
  delta, so a delta that is not a whole number of sectors leaves
  `delta % 2048` bytes of the ring holding the previous lap's audio for
  good. Block granularity is what keeps every delta whole: 0x5C000 is
  exactly 23 blocks of 0x4000, and the 0x1EAAA threshold is first crossed at
  eight blocks, so a steady-state refill is 0x20000 bytes, 64 sectors.
- With the cursor correct, a refill never leaves its ring: the read is
  `PREV .. PREV + consumed` and `PREV + consumed` is either `CUR`, which is
  below the ring size by construction, or exactly the ring size on the wrap
  path. cdvd.cpp therefore writes refills where the game aimed them and
  does not truncate; a read that runs past its iopheap allocation is now
  purely an anomaly signal, and is logged rather than clamped, because IOP
  RAM has no allocator protection and truncating would drop bytes the game
  expects to find.
- Output: host/audio.cpp, SDL3 audio stream at 48 kHz stereo f32 when a
  playback device exists, plus `ICORECOMP_WAV_CAPTURE=path` (48 kHz stereo
  s16, header kept valid incrementally). snd/tests/wav_stats.py prints
  per-second RMS and a distinct-event count for headless checks.
