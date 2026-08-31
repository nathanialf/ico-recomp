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

## Command records (16 bytes, packed by retail func_0025C6D8)

```
w0 = command id
w1 = (seq_tag << 8) | a1[23:16]
w2 = (a1[15:0] << 16) | a2[23:8]
w3 = (a2[7:0] << 24) | a3[23:0]
```

`seq_tag` is the post-increment value of the issued-command counter
(`D_0071C640+0x48`), truncated to 24 bits on the wire. Commands queued
directly by the flush path (ids 0xA-0xD) carry `seq_tag` = 0 and do not
advance the counter. a1/a2/a3 are 24-bit operands.

Command ids with an identified EE call site:
- `0x20` (retail `func_0025C680`, tagged): bank data transfer.
  a1 = source address inside the IOP heap buffer, a2 = destination (SPU RAM
  byte address, monotonically increasing across a bank load), a3 = byte
  length. Issued by `soundDataOpenSync` after each raw EE-to-IOP copy chunk.
- `0x21` (retail `func_0025C6B0`, tagged): same packing, transfer variant
  (not observed during boot; direction or target differs).
- `0x16` (retail `func_0025CCE0(ch, l, r)`): master volume per output port.
- `0x28` (retail `func_0025CE78(ch, l, r)`): second volume pair per port
  (observed at boot with 0x3FFF/0x3FFF).
- `0x0A/0x0B/0x0C/0x0D`: untagged per-tick level updates from the control
  block fields listed above.
- Observed at boot without a traced call site: `0x32`, `0x14`, `0x15`,
  `0x3C` (one-time setup batch) and `0x01`-`0x04` (per-voice key/pitch/
  volume traffic from the voice state machine, retail `func_00258D10` and
  its jump-table helpers).

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

Other status-block fields observed to be read by the EE (`+0x10` as a
pointer in voice states 0xB0/0xF0) are not needed during boot and stay zero
in this HLE; the loud per-command log will surface any state that starts
depending on them.

## Boot behavior after the fix

`soundDataOpenSync` loads the SE bank in seven 0x20 transfers (tags 1-7),
each preceded by a raw EE-to-IOP copy and followed by a mode-1 sync; then
regular per-field fno 0x64 flushes continue (mostly empty, occasionally
voice commands when the game plays SEs). See the endpoint log excerpt in the
task report / commit message.
