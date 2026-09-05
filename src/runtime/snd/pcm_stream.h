/* snd/pcm_stream.h: ring geometry for the SgStPcm command block (0x46-0x4F).
 *
 * Header only and free of runtime dependencies, so snd/tests/
 * pcm_stream_selftest.cpp can exercise the address math on its own the way
 * host/stick_shape_selftest.cpp does. engine.cpp owns the audio side.
 *
 * What the game does. INFERRED from the disassembly of the functions named
 * below, at their retail addresses; not re-verified instruction by
 * instruction against the ELF here:
 *
 *   audioDecCreate (PAL 0x00257550) creates the
 *   attract movie's audio stream. It allocates one IOP buffer of 0x6000
 *   bytes (sceSifAllocIopHeap, result kept at self+0x44, size at self+0x48),
 *   sends cmd 0x46 (SgStPcmInit, no operands), then sends cmd 0x48 twice
 *   through SgStPcmOpen with a four word struct:
 *
 *       [0] channel      0 then 1
 *       [1] 0x00010400   a flags word the caller composes
 *       [2] IOP address  buf     then buf + 0x200
 *       [3] 0x6000       the ring size, the same for both
 *
 *   SgStPcmOpen (PAL 0x00278640, a function entry in the retail ELF)
 *   rejects a channel
 *   >= 0x10 or either address word above 0x1FFFFF, then packs
 *   w1 = (channel << 24) | flags, w2 = struct[2], w3 = struct[3].
 *
 * So both channels live inside ONE 0x6000 byte ring that the EE fills with a
 * single linear wrapping copy (audioDecSendToIOP, PAL 0x00257B58, keeps one
 * write offset at self+0x4C and reduces it modulo self+0x48; sendToIOP2area
 * does the SIF DMA in at most two segments). Two channels 0x200 apart inside
 * one linearly written ring is a block interleave: 0x200 bytes of channel 0,
 * 0x200 bytes of channel 1, repeating with a period of 0x400.
 *
 * Where 0x400 comes from. It is the quantum the EE rounds every refill to:
 * audioDecSendToIOP reduces the free-space figure with the compiler's signed
 * `x / 0x400 * 0x400` idiom (`sra $2, 10; sll $4, $2, 10` at 00257C20 and
 * again at 00257C84; the addiu 0x3FF / movn pair ahead of each is the bias
 * for a negative value). The 0x200 channel spacing tiles a period of 0x400
 * exactly, and bits 15:8 of the flags word also hold 0x400, so the same
 * number arrives three independent ways.
 *
 * MEASURED: the addresses, the ring size, the 0x400 refill quantum, the
 * linear EE fill. INFERRED: that the 0x200 spacing means block interleave
 * rather than some other driver-side layout, and that bits 15:8 of the flags
 * word are that interleave block. Cmd 0x3E is not evidence for the second
 * one: engine.cpp reads 0x3E's bits 15:8 into st_blk, the IOP transfer block
 * the reported cursor is quantized to, while the ADPCM interleave stride is
 * a separate hard-coded 0x800 (engine.cpp, cmd 0x3E).
 *
 * Nothing here assumes a channel count. The base of the shared ring is the
 * lowest address the game opened, and a channel's contiguous run is the
 * distance to the next channel above it, both read off the open commands.
 */
#ifndef ICORECOMP_SND_PCM_STREAM_H
#define ICORECOMP_SND_PCM_STREAM_H

#include <cstdint>

/* SgStPcmOpen (open) and SgStPcmIopReadAddr (cursor read) both bound the
 * channel with `sltiu $2, $x, 0x10`, so the driver has 16 stream channels. */
constexpr int RT_PCM_CHANNELS = 16;

struct RtPcmChannel {
    bool open = false;
    bool playing = false;

    /* Straight from cmd 0x48, unmodified. */
    uint32_t flags = 0;   /* w1 bits 23:0, the caller's composed word */
    uint32_t iop_buf = 0; /* w2, this channel's first byte in IOP RAM */
    uint32_t ring = 0;    /* w3, byte size of the shared ring */

    /* Derived by rt_pcm_regroup over the whole open set. */
    uint32_t block = 0;   /* interleave period, flags & 0xFF00 */
    uint32_t base = 0;    /* ring base, the lowest open channel's iop_buf */
    uint32_t slot = 0;    /* iop_buf - base, offset inside one block */
    uint32_t chunk = 0;   /* contiguous bytes per block owned by this channel */
    uint32_t lap = 0;     /* own bytes per pass of the ring, ring/block * chunk */
    /* False when this channel's placement is not trustworthy: see
     * rt_pcm_regroup. rt_pcm_addr and rt_pcm_cursor still return a value for
     * such a channel, but it is fabricated and must not be handed to the
     * guest. */
    bool consistent = false;

    uint32_t pos = 0;     /* byte offset into this channel's own lap of the ring */
    uint64_t consumed = 0; /* total bytes played, never wrapped, for rate checks */
    uint16_t voll = 0, volr = 0; /* cmd 0x4A, 0..0x7FFF per SgStPcmVolume */
};

/* Recomputes base/slot/chunk/lap across every open channel and sets each
 * channel's `consistent`. Call after any open or close. Returns false when
 * the open set is not self consistent: differing ring sizes or block sizes, a
 * ring the block does not tile, two channels at the same address, or a
 * channel whose run is not a whole number of 16 bit samples. The caller logs,
 * since guessing a repair would substitute a plausible value for a measured
 * one. */
inline bool rt_pcm_regroup(RtPcmChannel* ch, int n) {
    uint32_t base = 0;
    uint32_t ring = 0;
    uint32_t block = 0;
    bool any = false;
    bool agree = true;
    for (int i = 0; i < n; ++i) {
        if (!ch[i].open) continue;
        if (!any) {
            base = ch[i].iop_buf;
            ring = ch[i].ring;
            block = ch[i].block;
            any = true;
            continue;
        }
        if (ch[i].iop_buf < base) base = ch[i].iop_buf;
        if (ch[i].ring != ring || ch[i].block != block) agree = false;
    }
    if (!any) return true;
    /* Whole-set geometry. A block that does not tile the ring makes every
     * channel's lap and every reported cursor meaningless, so it disqualifies
     * all of them, not just one. */
    if (block == 0 || ring == 0 || ring % block != 0) agree = false;

    bool consistent = agree;
    for (int i = 0; i < n; ++i) {
        if (!ch[i].open) continue;
        ch[i].base = base;
        ch[i].slot = ch[i].iop_buf - base;
        /* The channel's run ends at the next open channel above it inside the
         * same block, or at the end of the block. */
        uint32_t end = block;
        bool own = agree;
        for (int j = 0; j < n; ++j) {
            if (j == i || !ch[j].open) continue;
            uint32_t s = ch[j].iop_buf - base;
            /* Two channels opened at one address own the same bytes; there is
             * no run to derive and one of them is a mistake. */
            if (s == ch[i].slot) own = false;
            else if (s > ch[i].slot && s < end) end = s;
        }
        ch[i].chunk = end > ch[i].slot ? end - ch[i].slot : 0;
        if (ch[i].chunk == 0 || (ch[i].chunk & 1) != 0) own = false;
        /* Computed even when the block does not tile the ring: mix_pcm still
         * needs a wrap period to keep pos bounded, and ring / block truncated
         * is the same one the mixer used before this field existed. */
        ch[i].lap = block ? (ring / block) * ch[i].chunk : 0;
        ch[i].consistent = own;
        if (!own) consistent = false;
    }
    return consistent;
}

/* Absolute IOP RAM address of stream byte i for this channel: runs of `chunk`
 * bytes at `block` spacing, wrapping at the shared ring size. Same shape as
 * engine.cpp's stream_addr for the ADPCM voices. An i at or beyond one lap
 * folds back onto the same address as i - lap, which is what lets the health
 * scan read a whole run starting from any pos. */
inline uint32_t rt_pcm_addr(const RtPcmChannel& c, uint32_t i) {
    if (c.chunk == 0 || c.block == 0) return c.iop_buf;
    uint32_t off = c.slot + (i / c.chunk) * c.block + (i % c.chunk);
    return c.base + (c.ring ? off % c.ring : off);
}

/* Byte offset within the shared ring of the next byte this channel will
 * consume, rounded down to the interleave block. This is the word the EE
 * polls: SgStPcmIopReadAddr (PAL 0x00278828, a function entry in the
 * retail ELF) returns
 * status_block[0x180 + channel * 4], and audioDecSendToIOP uses it as
 *
 *     free = ((cursor + ring - write_ptr) - 0x400) mod ring, rounded to 0x400
 *
 * so it is an offset in the same units as the EE's own write pointer (which
 * that function keeps reduced modulo the ring size), not an address. The
 * block rounding matches the granularity the EE refills at and keeps the
 * 0x400 guard band meaningful.
 *
 * Only meaningful for a channel with `consistent` set. For any other channel
 * this is arithmetic over a placement that was never derived, so the caller
 * must not report it to the guest. */
inline uint32_t rt_pcm_cursor(const RtPcmChannel& c) {
    uint32_t off = rt_pcm_addr(c, c.pos) - c.base;
    return c.block ? off - (off % c.block) : off;
}

#endif /* ICORECOMP_SND_PCM_STREAM_H */
