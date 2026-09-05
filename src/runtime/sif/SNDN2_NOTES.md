# sndn2 wire protocol notes

Facts derived by reverse engineering the EE-side vendor sound library in the
user's own retail game copy (disassembly in the read-only decomp repo, TUs
`src/cod/vendor_272338` and `src/cod/vendor_276AD0`) plus one instrumented
boot run of this runtime. Every address and name below is the PAL build's
(SCES_507.60), as the decomp names it in `config/symbol_addrs.pal.txt`. No
code was copied from anywhere; sndn2 is the game's own driver, not a Sony SDK
module, so there is no SDK source to compare against.

## Topology

- IOP module `SNDN2DRV.IRX` registers one SIF RPC server, id `0x736e646e`
  (ASCII "sndn"). The EE binds one client to it (`SgSndn2RemoteInit`, PAL
  0x00276AD0, bind loop with a 10000-iteration delay between retries).
- Only two RPC function numbers exist:
  - `fno 0x65`: remote init, synchronous, 64-byte send and receive.
  - `fno 0x64`: command batch flush, NOWAIT (`SIF_RPC_M_NOWAIT`, no end
    function), variable-size send, 512-byte receive.
- Sound bank payload bytes never travel through RPC. The EE copies them into
  an IOP heap buffer (`sceSifAllocIopHeap` of 480 KB, observed alloc, plus a
  738 KB one for streaming) with raw SIF DMA, then queues a transfer command
  (below) that tells the IOP to consume them.

## EE-side state

- `D_00733C40` (0x80 bytes): the driver control block. Accessor
  `_SgGetComContext` (PAL 0x00273258) returns its address.
  - `+0x38` u16: output mode, written by `SgSetOutputMode` (PAL 0x00277318,
    at 0027732C) and by nothing else. An earlier note here called it the
    reverb depth; that was wrong. `_SgSeqSeVolume` reads it, and a value
    of 1 folds a voice to mono by taking min(|L|, |R|) for both channels
    (PAL 0x00275318..00275364). The game sets it from
    `soundOutputModeSet`, whose callers are the options screen and the
    memory card game block.
  - `+0x3A` u16: set to 0x3C at init (tick rate?).
  - `+0x3C` u32: command-ring bank selector, toggled 0/1 at every flush.
  - `+0x40` u32: write index into the current ring bank (max 0xFE).
  - `+0x44` u32: "remote is up" flag, set to 1 by init; the tick and status
    helpers early-out when it is 0.
  - `+0x48` u32: issued-command counter. Incremented once per tagged command
    (`_SgDmaCommon`, PAL 0x00276CC8).
  - `+0x00/0x08`, `+0x10/0x18`, `+0x20`, `+0x28`: 64-bit level/status values
    that the flush path turns into untagged commands 0xC, 0xD, 0xA, 0xB when
    they change.
- `D_00733CC0` (0x2000 bytes): double-buffered command ring, two 4 KB banks
  of 16-byte records; slot address = base + (bank << 12) + (index << 4)
  (`_SgGetPacketCntext`, PAL 0x002732E0).
- `D_00735CC0` (0x200 bytes): the status block the IOP writes back.
  `D_00735EC0` (the "handle" global, accessor `_SgGetIop2EeContext`, PAL
  0x002732D0) holds its uncached alias `0x20735CC0`; all EE reads of
  IOP-written status go through that pointer.
- `D_00736140`: the `SifRpcClientData` for the bind.

## Init (fno 0x65)

`_SgInit` (PAL 0x002768E8):
1. Zeroes all of the above plus the voice table (0x30 voices, 0x58-byte
   entries; status bytes +0x50/+0x54/+0x55/+0x56 preset to 0xFF).
2. Stores `0x20735CC0` into the handle global.
3. Synchronous CALL fno 0x65 with a 64-byte stack buffer:
   word 0 = 0x1E (constant; probably the voice/channel budget),
   word 1 = the caller's init argument, word 4 = 0, rest uninitialized.
   The 64-byte receive lands in the same stack buffer and is never read.
4. Sets `+0x44` = 1, `+0x48` = 0, `+0x40` = 0, `+0x3A` = 0x3C.

## Command records (16 bytes, two layouts)

There are two enqueue functions on the EE side, and they pack differently:

1. Tagged (`_SgDmaCommon`), used ONLY by the DMA commands 0x20
   (`SgDmaWrite`, PAL 0x00276C70) and 0x21 (`SgDmaRead`, PAL 0x00276CA0):

   ```
   w0 = command id
   w1 = (seq_tag << 8) | a1[23:16]
   w2 = (a1[15:0] << 16) | a2[23:8]
   w3 = (a2[7:0] << 24) | a3[23:0]
   ```

   `seq_tag` is the post-increment value of the issued-command counter
   (`D_00733C40+0x48`), truncated to 24 bits; a1/a2/a3 are 24-bit operands.
   For 0x20: a1 = IOP heap source, a2 = SPU RAM byte destination, a3 = byte
   length.

2. Raw (`_SgSetPkAdd`, PAL 0x002737E0), used by every other command: w1..w3
   are
   the caller's 32-bit arguments stored verbatim, no tag. Since w1 is
   always a voice/port number (or a packed field whose high byte is a
   voice), `w1 >> 8` can look nonzero; discriminate layouts by command id
   (only 0x20/0x21 are tagged), not by the tag byte.

## Command vocabulary (raw layout, w1/w2/w3 = enqueue args)

Voice commands (w1 = SPU2 voice number, 0..0x2F):
- `0x01` volume: w2 = VOLL, w3 = VOLR. 0..0x3FFF linear; bit 15 selects the
  SPU2 sweep-mode word `(mode << 8) | (vol >> 7)` (emitter retail
  `_SgSeqSeVolume`, PAL 0x00275250; sweep taken when the tone's sweep byte
  +0x0A is set).
  The emitter builds the value as a product of six slot-context bytes, a
  pan byte and the game's own per-slot level; every term is named with
  its source under "Native engine" below.
- `0x02` ADSR: w2 = ADSR1, w3 = ADSR2, the raw SPU2 register words straight
  from the bank's tone record (+0x06/+0x08). The "stop without release"
  callers send (0,0) then key-off: release shift 0 = instant cut. That name
  came from the US-era notes as `SgSeStopNoRelease` and has no symbol in the
  PAL decomp; the nearest PAL name is `soundSeDefStopNoRelease`
  (0x00145630, sound/s_init, itself provisional), which has not been
  measured to be the same function, so the class is described rather than
  named here.
- `0x03` start address: w2 = VAG start, SPU RAM byte address (bank slot
  base + tone's VAG offset << bank shift, computed on the EE).
- `0x04` note params (emitter `_SgPitchTableVag`, PAL 0x00275208):
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

ADPCM streaming (SgStAdpcm*, PAL 0x002782E0 `SgStAdpcmInit` through
0x002785A0 `SgStAdpcmIopReadAddr`):
- `0x3C` Init, `0x3D` Quit (no args).
- `0x3E` Open (`SgStAdpcmOpen`, PAL 0x00278310): w1 = voice << 24 | mode |
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
  are exactly two stream slots. `AdpcmIopBuffAlloc` (`sound/adpcm_init.c`)
  picks a free one out of `D_0063C1C0[2]` and returns
  `base = D_0063C1B8 + slot * 0x5C000`;
  the sound heap block is `0x800 + 2 * 0x5C000` = 755712 bytes. `AdpcmOpen`
  then opens one voice per channel at `base + (0x800 / nch) * ch`, so
  stereo is 0x400 apart inside a 0x800 stride. Only slot 0 has been
  observed; slot 1 would sit at base + 0x5C000. Known gap: the runtime
  models a single slot and recovers its base by stride alignment, so two
  concurrent streams are not represented.
- `0x3F` Close(voice) (w1 = voice number, not a handle; retail
  `SgStAdpcmClose`, PAL 0x002783A0, passes it in w1 with w2 = w3 = 0).
- `0x40` ChannelVolume(handle, L, R): w1|w2<<24 = 48-bit voice mask handle,
  w3 = L << 16 | R, each < 0x4000.
- `0x41` ChannelPitch(handle, rate): w3 = playback rate in Hz, capped at
  0x2EE00 = 192000 = 4 x 48000 (the SPU2 4x pitch ceiling). The boot
  ambience runs at 0xAC44 = 44100.
- `0x42` Play(handle), `0x43` Stop(handle).

PCM streaming (`SgStPcm*`, PAL 0x00278610 `SgStPcmInit` through 0x00278870
`SgStPcmBufMode`). Derived from the decomp asm plus one Windows run; the
attract movie is the only user in this binary and it drives the block from
`ito/mpeg/mv_audiodec.c`.

- `0x46` Init (`SgStPcmInit`), `0x47` Quit (`SgStPcmQuit`, PAL 0x00278628).
  No operands, all three words zero.
- `0x48` Open (`SgStPcmOpen`, PAL 0x00278640). The caller passes a four word
  struct {channel, flags, IOP address, ring size}; the emitter rejects a
  channel >= 0x10 or either address word above 0x1FFFFF, then packs
  `w1 = channel << 24 | flags`, `w2 = IOP address`, `w3 = ring size`.
  The movie (`audioDecCreate`, PAL 0x00257550) allocates one 0x6000 byte IOP
  buffer and opens two channels into it, at `buf` and `buf + 0x200`, both
  with ring 0x6000 and flags 0x00010400. Observed:
  `w1=0x00010400 w2=0x001b0100 w3=0x00006000` and
  `w1=0x01010400 w2=0x001b0300 w3=0x00006000`.
  Bits 23:16 are 0x01; cmd 0x3E's equivalent field is a channel count, but
  that reading does not survive here (both channels sit in one ring 0x200
  apart), so the field is left unexplained rather than given a plausible
  meaning.
- Ring layout. `audioDecSendToIOP` (PAL 0x00257B58) keeps one write offset
  (`self+0x4C`, reduced modulo the ring size) and `sendToIOP2area` (PAL
  0x00257828) fills the ring with a single
  linear wrapping SIF DMA, so the two channels are block interleaved inside
  it: 0x200 bytes of channel 0, then 0x200 of channel 1, period 0x400. The
  runtime derives the base as the lowest opened address and each channel's
  run as the distance to the next channel up, so it assumes no channel count
  (snd/pcm_stream.h). A channel it cannot place that way is left without a
  cursor rather than given a fabricated one.
- Where 0x400 comes from. MEASURED: it is the refill quantum.
  `audioDecSendToIOP` reduces its free-space figure to a whole number of
  0x400 byte units with the compiler's signed `x / 0x400 * 0x400` idiom,
  `sra $2, 10; sll $4, $2, 10` at 00257C20 and again at 00257C84 (the
  `addiu 0x3FF` / `movn` pair
  ahead of each is the negative-value bias). The 0x200 channel spacing tiles
  a period of 0x400 exactly, and bits 15:8 of the flags word also hold 0x400.
  INFERRED: that bits 15:8 are that interleave block. Cmd 0x3E is not
  evidence for it either way: the runtime reads 0x3E's bits 15:8 into
  `st_blk`, the IOP transfer block the reported cursor is quantized to
  (engine.cpp), while the ADPCM interleave stride there is a separate
  hard-coded 0x800. snd/pcm_stream.h marks the same conclusion INFERRED.
- `0x49` Close(channel) (`SgStPcmClose`, PAL 0x002786B0): w1 = channel,
  w2 = w3 = 0.
- `0x4A` ChannelVolume(mask, w2, w3) (`SgStPcmVolume`, PAL 0x002787D8): w1 is
  a channel mask with bits 31:24 required zero, w2 and w3 are each 0..0x7FFF.
  `SgStPcmVolume` has four callers, all in `ito/mpeg/mv_audiodec.c`. The two
  play paths, `audioDecStart` (PAL 0x00257F28) and its near copy
  `audioDecResume` (PAL 0x00257FF8, restart/resume), send (1, 0, vol) and
  (2, vol, 0), one hard pan per channel, or (3, vol/2, vol/2) when the byte
  at `self+0x58` is set; vol is the word at `self+0x5C`. The two teardown
  paths, `audioDecReset` (PAL 0x00257EB8, at 00257ED0) and `audioDecPause`
  (PAL 0x00257FC0, at 00257FD8), send (3, 0, 0) as a mute immediately before
  Stop(3). Nothing in the decomp names the two operands; the runtime reads
  w2 as left and w3 as right to match cmd 0x01 and cmd 0x40, which puts
  channel 0 on the right. Unresolved: the stereo image may be mirrored.
  The vol the movie carries is 0x3FFF, and it is a constant in the caller,
  not a measurement of anything: `movie_init` (PAL 0x001A64C0) passes it as
  `initAll` argument 7 (`addiu $10, $0, 0x3FFF` at 001A663C); `initAll` (PAL
  0x001A60C0, a name the decomp marks provisional) keeps it in $22 (001A60E8)
  and passes it as argument 3 to `audioDecCreate` (001A61E0), which stores it
  at `self+0x5C` (00257574, 00257664).
- `0x4B` Play(mask) (`SgStPcmPlay`, PAL 0x00278708), `0x4C` Stop(mask)
  (`SgStPcmStop`, PAL 0x00278748). Same mask rule. Observed with mask 3.
- `0x4D` (channel, value) (`SgStPcmLseek`, PAL 0x00278788): w1 = channel
  < 0x10, w2 <= 0x1FFFFF, w3 = 0. Two callers, both in
  `ito/mpeg/mv_audiodec.c` and
  both sending (0, 0) then (1, 0) immediately before Play(3): `audioDecStart`
  (00257F3C/00257F48) and its near copy `audioDecResume`
  (0025800C/00258018), the restart/resume path. So the only observed effect
  is starting a channel at the bottom of the ring. Reading w2 as a ring
  offset is inferred; both the zero and the nonzero case are logged, and a
  channel that is not open is left alone.
- `0x4E` (`SgStPcmSetEffect`, PAL 0x002786F0): w1 forwarded with no
  validation. The movie sends 8, once, after opening both channels. Meaning
  not established.
- `0x4F` (`SgStPcmBufMode`): w1 = mask, w2 <= 0x1FFFFF, w3 < 2. Never sent.
- Handshake. The EE polls how far the driver has consumed the ring with
  `SgStPcmIopReadAddr` (PAL 0x00278828), which reads status block word
  `+0x180 + channel * 4` through the same uncached status pointer
  `SgStAdpcmIopReadAddr` uses. `audioDecSendToIOP` turns it into the refill
  size as
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
transfers on the boot path, 0x5010..0x1B7250 on the US build and
0x005010..0x1b8210 on PAL, each measured on its own binary). Note-ons then
carry explicit SPU addresses/ADSR/note data, so this HLE never parses a bank
header: the
16-byte tone records (+0x02 center?, +0x03 fine, +0x04 VAG offset in
shifted units, +0x06 ADSR1, +0x08 ADSR2, +0x0A sweep, +0x0D used by the
voice state machine, +0x0F bit 7 reverb flag) stay an EE-side concern.

## Flush (fno 0x64)

`_SgCalledTickProc` (PAL 0x00273300), run from the vblank path when `+0x44`
is set (`SgCalledTickProc`, PAL 0x00276C28):
1. Runs the 0x30-entry voice state machine, queueing per-voice commands.
2. Queues untagged level commands 0xC/0xD/0xA/0xB if those fields changed.
3. CALL fno 0x64, mode NOWAIT, no end function:
   send = current ring bank, size = write_index * 16 (0 is normal when
   idle), recv = the status block via its uncached alias, recv size 0x200.
4. Resets the write index and toggles the ring bank.

## The ack (what fixed the boot livelock)

- Status block word `+0x1C0`, written by the IOP, must equal the EE's issued
  counter `D_00733C40+0x48` for the library's sync to complete.
- `SgGetDmaTransferStatus(mode)` (PAL 0x00276D58): mode 1 spins
  until `*(handle+0x1C0) == *(D_00733C40+0x48)` (RAM-only spin; on real
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
status block: `+0xC0 + (voice % 24) * 4 + (voice / 24) * 0x60`
(`SgStAdpcmIopReadAddr`, PAL 0x002785A0) holds the byte OFFSET WITHIN
THE RING, 0 .. ring - 1, that the driver will consume next for that stream
voice. It is not an address; see the ADPCM streaming section below for the
derivation from `adpcmTickProc`. The game's adpcm tick uses the
per-tick delta to schedule ring refills, so the HLE fills these words every
flush (engine.cpp `rt_snd_fill_status`).

## Bank upload path (who calls cmd 0x20, and with what)

Read off the PAL decomp. `SgDmaWrite` (PAL 0x00276C70) has exactly two
callers in this binary: the vendor library's own `SgVabOpen`
(asm/matchings/src/cod/vendor_276AD0/SgVabOpen.s at 00276E2C), which the
game's sound code never uses because `soundDataOpenChk` calls
`SgVabOpenFakeBody` directly, and `sound/s_init.c soundBDDataSet` (PAL
0x00143D68), at 00143F90 and 00143FA8. So every bank byte in SPU RAM came
through `soundBDDataSet`.

`soundBDDataSet(ee_data, num, bank, a3, a4, size)`:

1. Finds or creates the 0x30-byte data record in `D_006BF570` (0x10 slots),
   stores `ee_data` at `+0x08`, and rounds `size` up to 64 bytes
   (00143EC0: `sra 6`, `addiu 1`, `sll 6`).
2. `soundBufAlloc(record, rounded)` reserves the SPU RAM span and leaves its
   base at `record+0x18`. The allocator hands out increasing byte addresses
   from 0x5010.
3. Loop, chunk = min(0x78000, remaining), where 0x78000 is the IOP heap
   block `soundAllocIopHeap` took (`iosSifAllocIopHeapDebug(0x78000, ...)`,
   s_init.c; the runtime answers that alloc at IOP 0x090000):
   - `SgGetDmaTransferStatus(1)` (00143EFC), which spins until the ack word
     at status `+0x1C0` catches the issued counter, so the IOP is done with
     the staging buffer.
   - a raw EE to IOP `sceSifSetDma` of the chunk into that one heap address
     (00143F38..00143F6C, descriptor `{ee_data + off, heap, chunk, 0}`,
     count 1, then a spin on `sceSifDmaStat`).
   - `SgDmaWrite(heap, spu_base + off, chunk)` (00143F90), or with the
     length padded to 0x50 when the chunk is 0x40 bytes or less (00143FA8).
   - `SgGetDmaTransferStatus(1)` again when the record's kind (`+0x04`) is 1.
4. `soundDataOpenChk(record)`, which calls `SgVabOpenFakeBody(record+0x0C,
   record+0x18)` once both the .hd pointer (`+0x0C`, set by
   `soundHDDataSet`) and the .bd pointer (`+0x08`) are present, stores the
   vab id at `+0x28`, and for kind 0 sends `SgSetSeMasterVol(vab, 0x7F)`.

The staging step is what `rt_spu_staged_bytes` (snd/spu.cpp, snd/iop_stage.h)
witnesses: sif/rpc.cpp already routes every EE to IOP DMA entry into the
sound side, so a cmd 0x20 whose source range was not written since the last
transfer consumed it is a transfer this runtime lost, and sndn2.cpp warns
with both byte counts rather than copying stale staging memory in silence.

## Boot behavior after the fix

`soundDataOpenSync` loads the SE bank in seven 0x20 transfers (tags 1-7),
each preceded by a raw EE-to-IOP copy and followed by a mode-1 sync; then
regular per-field fno 0x64 flushes continue (mostly empty, occasionally
voice commands when the game plays SEs). See the endpoint log excerpt in the
task report / commit message.

MEASURED on the PAL boot path, `dist/logs/handoff-2026-09-04/
native-crosscheck.log`: seven cmd 0x20 transfers, all reading IOP 0x090000
(the 0x78000 heap block, allocated there in the same log), filling SPU
0x005010..0x1b8210 contiguously with 237184, 322688, 209536, 136960, 34176,
434176 and 407552 bytes. Sizes below 0x78000 mean one chunk per
`soundBDDataSet` call, so these are seven bank bodies, not chunks of one.
They all land while the loader is streaming `COMMON.DF` (DATA.DF entry 0,
container offset 0x2000, disc LBA 19775), long before the language screen.
The 896-sector read at LBA 21544 that follows the screen is the start of
`STGLOG.DF`, entry 2, and has nothing to do with the sound banks.

That measurement retired an inference the engine used to print. Voice 4's
key-on start address 0x081f70 sits inside transfer #12 (0x03ee90..0x08db10),
yet the key-on line said "SPU RAM zero there: no bank uploaded" on the
strength of 16 zero bytes at that address. Sixteen zero bytes are one ADPCM
block and say nothing about whether a bank arrived. The engine now reports
the transfer that covered the address (`rt_spu_covered_by`) as the verdict
and the nonzero byte count over the first 256 bytes as a separate figure,
and warns only when no transfer ever covered the address.

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
  ramp yet). The boot SEs arrive with volume (0,0), so a faithful boot
  render is silent SE-wise. There is no override for this: a switch that
  replaced the game's own volume words with constants was removed on
  2026-09-05, because no setting alters a value the game supplied. The
  zero-level diagnostic in `snd/engine.cpp` reports why the level is zero
  instead.

  Where that zero comes from, traced through the decomp. `_SgSeqSeVolume`
  (PAL 0x00275250) takes a0 = the voice number and a1 = the SE's sequence
  context entry (`_SgGetSeqContext`, PAL 0x00273240, base D_00732C80, stride
  0x54). It reads six u16 fields of that voice's slot context
  (`_SgGetSlotContext`, PAL 0x00273228, base D_00731C00, stride 0x58) at
  +0x16, +0x22, +0x1C, +0x1A, +0x18 and +0x1E, then the high byte of slot
  +0x20 for the left channel and its low byte for the right, then the
  sequence entry's word at +0x44 (left) and +0x48 (right), shifts the 64-bit
  product right by 46 (`dsra32 $2, $2, 14`) and halves the 16-bit result
  before sending it as cmd 0x01. Seven factors of 0..0x7F and one of
  0..0x1000 is exactly the scale that fills 0x3FFF: with every byte at 0x7F
  and the caller word at 0x1000 the formula yields 0x3C94.

  Every factor for the boot cursor SE, and where it comes from. The SE the
  kanban screens play is `CUR_SE` (`src/layout_action.c`), which is
  `soundSeDefPlay(0x19B, -2, 0, 0)` -> `_soundSeDefPlay` (PAL 0x00145048)
  -> `SgSePlay` (PAL 0x00277BB8). Entry 0x19B of the SE definition table
  D_005D6DB0 (0x3C stride) is named "move" and carries volume rate 1.0f at
  +0x24 and 0 at +0x38, both read out of the retail PAL ELF.

  - slot +0x1A: the note-on velocity, byte 2 of the SE sequence event
    (`_SgSeMain`, PAL 0x00273878, at 00273A50). The note byte beside it is
    what produced the key-on's pitch, so the event is intact in the log.
  - slot +0x1C: tone record +0x0B, the same 16-byte record whose +0x06/+0x08
    produced the key-on's ADSR and whose +0x04 produced its VAG address, so
    the record is intact too (00273A58).
  - slot +0x18: program record +0x01, the record the tone hangs off
    (00273A40).
  - slot +0x20: D_0054CB78[tone[+0x0C] >> 2], a 32-entry pan law in the ELF
    (00273A78). Read from the ELF: every entry has one byte in 0x78..0x7F
    and the other in 0x00..0x78, and the two are never zero together, so pan
    alone cannot zero both channels.
  - slot +0x1E: byte 0 of the SE table, `*(u8 *)hd[+0x40]` (00273A60).
    `_SgSetRealtimeVolume` (PAL 0x00274840; 0x00273650 named in the older
    note is its call site inside `_SgCalledTickProc`) overwrites that byte
    with `_SgGetSeVolValue(vab) & 0x7F` every tick (00274918), and the only
    writer of that value is `SgSetSeMasterVol(vab, 0x7F)` from
    `soundDataOpenChk` for a record whose +0x04 is 0 (00143868), so it is
    0x7F once the bank is open.
  - slot +0x16: SE table byte `hd[+0x40] + slot * 0x10 + 0x1E` (00273A38).
  - slot +0x22: SE table byte `hd[+0x40] + slot * 0x10 + 0x13` (00273A8C).
  - seq +0x44 and +0x48: `SgSetSeVolDirect` (PAL 0x00277FB8) writes them
    from `soundSeVolSet` (PAL 0x001443F0), which computes slot[+0x12] *
    slot[+0x18], with slot[+0x12] = 0x1000 written by `sound3DParamSet`
    (PAL 0x00144C78) at entry and slot[+0x18] = the SE definition's 1.0f
    volume rate, clamped to 0x1000. `SgSePlay` had already seeded both with
    0x1000 (00277D58/00277D5C).

  The SE table pointer is `hd[+0x40] = hd + hd[+0x20]`, one of the five
  offsets `SgVabOpenFakeBody` (PAL 0x00276E58) rebases at bank open
  (00276EE8..00276F24). For an SE the sequence's channel index is the
  sequence slot number, not a MIDI channel (`_SgTableEnvAdd`, PAL
  0x00274A20, at 00274ADC), so INFERRED that region is a 0x30-entry array of
  16-byte per-slot blocks behind a leading master volume byte. MEASURED over
  the vendor library's byte stores: no `sb` writes an SE entry's +0x13 or
  +0x1E. The only two that write those offsets at all are `_SgContVol` (PAL
  0x00275A98, at 00275BB8) and `_SgSetRealtimeVolume` (00274960), and both
  are on their BGM branch, which addresses the BGM sequence's own table at
  seq +0x08. So for an SE those two factors are bank file bytes and nothing
  else.

  What the runtime supplies to that chain: nothing. No syscall answer and no
  RPC reply reaches it. The console OSD word is read only by
  `sceScfGetLanguage` and only from `kanbanBootMcCheck`; no `sceScf`
  accessor is called from any sound path. The memory card game block
  (`ios/mcard gameblock_read`, PAL 0x0013A638) applies exactly one sound
  value, `soundOutputModeSet(block[+0x194])`, whose global only reaches
  `SgSetOutputMode`; no level is in the block, and the block is a save load
  rather than a boot step. `soundReverbDepthSet` has two callers, a stage
  effect and a debug menu, neither on these screens. No 3D attenuation
  applies: `sound3DParamSet` takes its distance branch only when the SE
  definition's +0x38 bit 7 is set and the caller passed a position, and
  0x19B has neither. No fade applies at key-on: the ramp
  `_SgSetRealtimeTickProc` (PAL 0x00274138) drives writes slot +0x1A from
  `_SgfadeParam`, and it runs only once `_SgContVol` has set the slot's 0x40
  flag, which is after the note is already on.

  The arithmetic is not the explanation either. MEASURED: the retail
  `__muldi3` (PAL 0x0027C198, 22 instructions, branch free) was decoded from
  the decomp's own encodings and stepped through this repo's reference
  interpreter for ten operand pairs, including negative operands and
  products above 2^32, and every result matched a native 64-bit multiply bit
  for bit; the whole eight-call `_SgSeqSeVolume` chain with every byte at
  0x7F and the caller word at 0x1000 came out as 0x3C94, and the same chain
  with one factor zeroed came out as 0. `cargo test -p ee-interp --test
  threeway` is green, and that suite compares the interpreter against the
  compiled emitted C over the full context per instruction, so the emitted
  `mult`, `mult1`, `multu`, `mfhi`, `mflo`, `dsll32`, `dsrl32` and `dsra32`
  agree with the interpreter that was just measured. The earlier inference
  that the emitter drops the high half is retired.

  Verdict: volume 0/0 is not what a console computes on these screens, and
  it is not a value this runtime supplied. Every factor is a byte of the
  bank's .hd image in EE RAM or a constant in the ELF, and every ELF-side
  one has been read and is nonzero. What is left is a .hd byte reading zero
  in EE RAM when it should not. The two SE table bytes, `hd[+0x40] + slot *
  0x10 + 0x13` and `+ 0x1E`, are the only factors in the chain that no other
  observation in the log has already shown to be intact, and the program
  record's +0x01 is the next candidate after them.

  The measurement that names it is now in the runtime. When cmd 0x01 hands a
  voice a level of zero, `snd/engine.cpp` reads the eight factors out of the
  game's own tables and logs them at warn, at most twice per voice, together
  with which of them are zero. It is a read; nothing writes guest memory.
  The addresses it needs are in `src/runtime/guest/ico_syms.h`, in the sound
  block: slot context 0x00731C00 stride 0x58, sequence context 0x00732C80
  stride 0x54, vab table 0x00731600 stride 0x0C, vab entry +0x00 = the .hd
  image, and the .hd's own +0x20 (the SE table's file offset) and +0x40 (its
  rebased pointer). Every one was decoded out of SCES_507.60 after the
  decomp's listing for the function was matched against it word for word;
  the header names the instruction for each.

  Where those bytes came from, so a zero can be tested against the read
  path. `ReadSoundHdFile` (PAL 0x001AB1E8) is the "hd" entry of the
  char-file handler table at 0x0055F9D4: it takes an EE buffer from
  `iosMallocDebug`, fills it with one `iosCdvdHandlerRead` of the packed
  file entry, and passes it to `soundHDDataSet` (PAL 0x00146248), which is what
  puts the pointer in the sound data record `soundDataOpenChk` opens the
  bank from. So the .hd is a heap allocation, not a fixed address, and the
  handle on the read path is the file offset rather than the EE address: the
  report prints both. On the PAL boot the outer container is
  `DFDATAS/DATA.DF` at LBA 19771, whose pack loader reads 16-sector chunks
  into the staging buffers at 0x00319a00 and 0x00321a00 (the `sceCdRead`
  lines in the cdvd log). MEASURED above: the seven bank bodies land while
  the loader streams `COMMON.DF`, DATA.DF entry 0; the .hd of the same
  record goes through the same char-file reader, though only the .bd side of
  that was timed. The 896-sector read at LBA 21544 in the same log lands
  after both key-ons and is `STGLOG.DF`, so it is not a candidate.

  MEASURED on the PAL boot, 2026-09-05 (dist/logs/handoff-2026-09-04/
  native-tracker.log, voice 4): every bank byte in the product is intact.
  expression(+0x16)=0x64 chanvol(+0x22)=0x64 tonevol(+0x1C)=0x1E
  velocity(+0x1A)=0x5F progvol(+0x18)=0x7F semaster(+0x1E)=0x7F
  pan(+0x20)=0x7878, and the SE table's own bytes read the same live
  (expression 0x64, volume 0x64, master 0x7F at .hd offsets 0x0014DA,
  0x0014CF and 0x0014BC of the image at EE 0x01ACADD0). The one zero is the
  caller level, sequence entry 0's +0x44 and +0x48. So the bank, the read
  path that delivered it and the whole `_SgSeqSeVolume` product are cleared;
  the question moved to the game's own level.

  Where that level comes from. `SgSePlay` seeds the sequence entry's +0x40,
  +0x44 and +0x48 with `$30`, and `$30` is 0x1000: `addiu $30, $0, 0x1000`
  at 0x00277C3C, stored at 0x00277D54, 0x00277D58 and 0x00277D5C. So a zero
  there is not a missing write, it is a later one. The only stores to those
  two words anywhere in the vendor library are `_SgInit`'s clear,
  `SgBgmOpen`, `SgSePlay`'s seed and `SgSetSeVolDirect` (PAL 0x00277FB8, at
  0x00278018 and 0x00278020), and for a sequence that is playing an SE only
  the last applies. `SgSetSeVolDirect` has two callers, both inside
  `sound3DParamSet` (PAL 0x00144C78): a hard (0, 0) at 0x00144D4C, and
  `soundSeVolSet` (PAL 0x001443F0).

  `soundSeVolSet`'s three inputs, and the polarity of its first test.
  `lui $3, 0x2000` at 0x001443F4, `and` at 0x00144410, `beqz $2` at
  0x00144414 branching to 0x00144424: the COMPUTE path is taken when bit 29
  of the slot's +0x04 is CLEAR, and a SET bit is the mute that sends (0, 0).
  It then forms L from `(s16)slot[+0x12] * (f32)slot[+0x18]` and R from
  `+0x14` with the same float, multiplies both by the global float at
  0x0063A64C only when `slot[+0x08] == -1` and `slot[+0x3C] != 0`
  (0x00144464, 0x0014446C, 0x00144474), clamps to 0..0x1000 with a negative
  value going to 0 at 0x001444A0, and slews by at most 0x100 per call unless
  the previous value is -1.

  What the boot cursor SE puts in those inputs, from the decomp. The SE is
  `CUR_SE` (`src/layout_action.c`), `soundSeDefPlay(0x19B, -2, 0, 0)`;
  INFERRED that this is the SE the kanban boot screens play, and the SE
  number the diagnostic now prints is what settles it.
  `sound3DParamSet` writes 0x1000 into +0x12 and +0x14 on entry, the second
  store in the delay slot of its own first branch so both are
  unconditional (0x00144C90, 0x00144C9C). `_soundSeDefPlay` stores the
  caller's float into +0x18 only when it is not negative, and
  `soundSeDefPlay` passes -1.0f (`lui $1, 0xBF80` / `mtc1 $1, $f12` at
  0x0014650C and 0x00146510), so the field should take the SE definition's
  own +0x24, which for entry 0x19B ("move") is 1.0f in the retail ELF.
  Bit 29 is cleared by `_soundSeDefPlay` at 0x001453C8 and by
  `soundReqTickProc` (PAL 0x00146778) on every frame for a slot whose +0x08
  is -1 or -2 (0x00146828, 0x00146830, then 0x00146840); it is only set for
  a slot that is neither (0x0014683C). This SE passes -2. The pad is not in
  the chain either: `_soundSeDefPlay` calls `iosPadActRequest` only when the
  SE definition's +0x36 is nonzero (0x001454BC), and entry 0x19B's is zero,
  so +0x0C stays 0 and `soundSeVolSet`'s `iosPadActVolumeSet` tail is
  skipped. The one path that can zero the level as a matter of game state is
  the global at 0x0063A64C, whose only writer is `ExecIcoMisc` and one of
  whose three stores is a plain zero (0x001B7F14); it needs +0x08 == -1 and
  +0x3C != 0, and this SE has -2 and 0.

  MEASURED on the generated C, 2026-09-05: `F_soundSeDefPlay`,
  `F__soundSeDefPlay` (including the `c.lt.s` at 0x00145364 and the `bc1f`
  at 0x0014537C that pick the rate, which emit as `rc_fcr31_cond(...,
  rc_fc_lt(f20, f0))` and `(fcr31 & 0x00800000) == 0`),
  `F_sound3DParamSet`, `F_soundSeVolSet` and `F_soundReqTickProc`
  (including the `beql` nullification at 0x00146828) were read against the
  retail instructions and all match. So the emitter is not putting a zero
  in any of the three inputs by mistranslation.

  Not settled, and the next log settles it: which of the three inputs is
  actually zero at the moment the level is sent. `snd/engine.cpp` now
  follows the zero back. When the caller level is the zero factor it finds
  the slot of the game's own SE table (0x006BF870, 0x30 entries of 0x40)
  that holds this sequence entry's index at +0x10, and logs the flag word,
  +0x08, +0x0C, the two 3D scales, the volume rate as bits and as a value,
  the SE number derived from +0x38, the SE definition's own rate, and the
  global at 0x0063A64C, with a verdict line naming which one did it. A rate
  of exactly -1.0f is called out for what it would be: not a rate the game
  stores but `soundSeDefPlay`'s own sentinel argument surviving into the
  field. All of it is read; nothing writes guest memory.

- Reverb is a Schroeder/Moorer send bus keyed off the type/depth commands
  (0x15/0x16) and the 0x0C send-enable mask, not an SPU2 DSP model.
- Streams decode straight out of the virtual-IOP ring (interleave
  de-chunking as above) and play whatever the ring holds. Refill is paced
  entirely by the cursor the runtime reports back at status +0xC0 +
  (v % 24) * 4 + (v / 24) * 0x60, read by `SgStAdpcmIopReadAddr`. That word
  is a byte OFFSET WITHIN THE RING, 0 .. ring - 1, not an IOP address:
  `adpcmTickProc` (PAL 0x00143430, `sound/adpcm_init`; `adpcmDataSet`
  registers it with `iosCdvdBackGroundMgrAdd`) keeps its own `PREV` in the
  same units, takes
  `consumed = CUR - PREV`, refills when that passes 0x1EAAA (a third of the
  ring) or the cursor wraps, reads `consumed` bytes to `ring_base + PREV`,
  then advances `PREV` or resets it to 0 when it would leave the ring.
  Reporting an address there made every refill ask for `address` bytes and
  land back at offset 0, which played the ambience as fragments from all
  over its file.
- The cursor moves in whole transfer blocks (`blocksize`, 0x4000 here), not
  in decoder steps. `iosCdvdBackGroundReadIOPm` (PAL 0x00134F58, `ios/cdvd`,
  a name the decomp marks provisional) converts the byte delta to sectors by
  truncation (`sra $22, 11`; the `+0x7FF` applies only to the negative
  branch) while `adpcmTickProc` advances PREV by the untruncated
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
