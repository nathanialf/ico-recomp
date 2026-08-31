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
- `0x3E` Open (retail `func_0025DD20`): w1 = voice << 24 | nch << 16 |
  (blocksize >> 8) << 8 | spu_addr[23:16], w2 = spu_addr[15:0] << 16 |
  ring_size[23:8], w3 = ring_size[7:0] << 24 | iop_buf[23:0].
  Boot ambience: two voices (0 and 1), nch = 2, blocksize 0x4000,
  spu 0x1E0000/0x1E4000, ring 0x5C000, iop 0x2B8000/0x2B8400. The ring
  interleaves the channels in 0x800/nch chunks (stride 0x800).
- `0x3F` Close(voice) (w1 = voice number, not a handle).
- `0x40` ChannelVolume(handle, L, R): w1|w2<<24 = 48-bit voice mask handle,
  w3 = L << 16 | R, each < 0x4000.
- `0x41` ChannelPitch(handle, rate): w3 = playback rate in Hz, capped at
  0x2EE00 = 192000 = 4 x 48000 (the SPU2 4x pitch ceiling). The boot
  ambience runs at 0xAC44 = 44100.
- `0x42` Play(handle), `0x43` Stop(handle).
- `0x46`..`0x4F` `SgStPcm*` family (PCM streaming; not observed).

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
`func_0025DFB0`, aug6 `SgStAdpcmIopReadAddr`) holds the absolute IOP RAM
address the driver will consume next for that stream voice. The game's
adpcm tick uses the per-tick delta to schedule ring refills, so the HLE
fills these words every flush (engine.cpp `rt_snd_fill_status`).

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
  de-chunking as above), report their cursors, and play whatever the ring
  holds; refill DMA from the EE side is the pending piece (ring observed
  all-zero during boot).
- Output: host/audio.cpp, SDL3 audio stream at 48 kHz stereo f32 when a
  playback device exists, plus `ICORECOMP_WAV_CAPTURE=path` (48 kHz stereo
  s16, header kept valid incrementally). snd/tests/wav_stats.py prints
  per-second RMS and a distinct-event count for headless checks.
