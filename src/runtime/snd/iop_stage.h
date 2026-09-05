/* snd/iop_stage.h: the witness for what the EE staged in virtual IOP RAM.
 *
 * Header only and free of runtime dependencies, so snd/tests/
 * pcm_stream_selftest.cpp can exercise it on its own the way
 * snd/pcm_stream.h is exercised. spu.cpp owns the live instance.
 *
 * What the game does. INFERRED from the disassembly of the functions named
 * below, at their retail addresses; not re-verified instruction by
 * instruction against the ELF here:
 *
 *   soundBDDataSet (PAL 0x00143D68) is the ONLY caller of
 *   SgDmaWrite in this binary outside the vendor library's own SgVabOpen,
 *   which the sound code does not use (soundDataOpenChk calls
 *   SgVabOpenFakeBody directly). It uploads a bank body in chunks of at most
 *   0x78000 bytes, the size of the IOP heap block soundAllocIopHeap took
 *   (iosSifAllocIopHeapDebug(0x78000), s_init.c), and each chunk is two
 *   steps:
 *
 *     1. SgGetDmaTransferStatus(1), which spins until the IOP has finished
 *        the previous chunk, so the staging buffer is free.
 *     2. a raw EE to IOP sceSifSetDma of the chunk into that one heap
 *        address (00143F38..00143F6C: buf = {EE src, D_0063A680, size, 0},
 *        sceSifSetDma(buf, 1), spin on sceSifDmaStat).
 *     3. SgDmaWrite(D_0063A680, spu_dest, size), which becomes the cmd 0x20
 *        record the sndn2 batch carries (00143F90, or 00143FA8 with a
 *        length padded to 0x50 for a tail chunk of 0x40 bytes or less).
 *
 * So every cmd 0x20 names a source range that a SifSetDma must have written
 * first. This map is the record of that: sif/rpc.cpp routes every EE to IOP
 * DMA entry into the sound side already, and a cmd 0x20 whose source was
 * never written names a lost transfer rather than a silent bank.
 *
 * Granularity is 16 bytes, the SPU2 ADPCM block. A write marks every granule
 * it touches, including a partial one, because the point is to catch a
 * transfer that never happened at all, and under-reporting would raise false
 * alarms on an unaligned edge. Both of the game's own sizes are exact here
 * anyway: the heap address is 16-byte aligned and soundBDDataSet rounds every
 * length up to 64 bytes (00143EC0: sra 6, addiu 1, sll 6).
 */
#ifndef ICORECOMP_SND_IOP_STAGE_H
#define ICORECOMP_SND_IOP_STAGE_H

#include <cstdint>
#include <cstring>

/* Virtual IOP RAM is 2 MB (sif/rpc.h RT_IOP_RAM_SIZE; spu.cpp static_asserts
 * the two agree). At a 16-byte granule that is 16 KB of bitmap. */
constexpr uint32_t RT_IOP_STAGE_SIZE = 2u * 1024 * 1024;
constexpr uint32_t RT_IOP_STAGE_GRANULE = 16;

class RtIopStageMap {
public:
    void reset() { std::memset(words_, 0, sizeof(words_)); }

    /* One EE to IOP DMA entry. Marks every granule the range touches. */
    void note_write(uint32_t addr, uint32_t size) { mark(addr, size, true); }

    /* A cmd 0x20 consumed this source range: the EE is free to refill the
     * staging buffer, so the marks go. Clearing on consume is what makes the
     * next transfer's check mean "staged since the last consume". */
    void consume(uint32_t addr, uint32_t size) { mark(addr, size, false); }

    /* Bytes of [addr, addr + size) that lie in a granule some write touched.
     * Rounded out to the granule, so it is an upper bound on what was
     * written and a lower bound on nothing at all. */
    uint32_t staged_bytes(uint32_t addr, uint32_t size) const {
        uint32_t g0, g1;
        if (!granules(addr, size, &g0, &g1)) return 0;
        uint32_t n = 0;
        for (uint32_t g = g0; g <= g1; ++g) n += get(g) ? RT_IOP_STAGE_GRANULE : 0;
        return n;
    }

    bool fully_staged(uint32_t addr, uint32_t size) const {
        uint32_t g0, g1;
        if (!granules(addr, size, &g0, &g1)) return size == 0;
        for (uint32_t g = g0; g <= g1; ++g) {
            if (!get(g)) return false;
        }
        return true;
    }

private:
    static constexpr uint32_t kGranules = RT_IOP_STAGE_SIZE / RT_IOP_STAGE_GRANULE;
    static constexpr uint32_t kWords = kGranules / 32;

    bool get(uint32_t g) const { return (words_[g >> 5] >> (g & 31)) & 1u; }

    /* Granule range covering [addr, addr + size), clipped to IOP RAM.
     * Returns false when the range is empty or entirely outside. */
    static bool granules(uint32_t addr, uint32_t size, uint32_t* g0, uint32_t* g1) {
        if (size == 0 || addr >= RT_IOP_STAGE_SIZE) return false;
        uint64_t end = (uint64_t)addr + size;
        if (end > RT_IOP_STAGE_SIZE) end = RT_IOP_STAGE_SIZE;
        *g0 = addr / RT_IOP_STAGE_GRANULE;
        *g1 = (uint32_t)((end - 1) / RT_IOP_STAGE_GRANULE);
        return true;
    }

    void mark(uint32_t addr, uint32_t size, bool set) {
        uint32_t g0, g1;
        if (!granules(addr, size, &g0, &g1)) return;
        for (uint32_t g = g0; g <= g1; ++g) {
            if (set) words_[g >> 5] |= 1u << (g & 31);
            else words_[g >> 5] &= ~(1u << (g & 31));
        }
    }

    uint32_t words_[kWords] = {};
};

#endif /* ICORECOMP_SND_IOP_STAGE_H */
