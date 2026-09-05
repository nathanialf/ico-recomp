/* gs/render/gs_vram.h: GS local memory and the transfer (TRXDIR) engine.
 *
 * Ours (MIT). Addressing comes from gs_swizzle.h; the register layouts come
 * from gs_regs.h. Behaviour is the GS User's Manual's transmission chapter.
 *
 * Where the authoritative copy lives, and why:
 *
 *   Neither copy is the authority on its own. The three transfer directions
 *   and the CLUT loads are CPU work over the host copy below; the rasteriser
 *   writes the device buffer. gs_native.cpp reconciles the two at the batch
 *   boundary in both directions: the words this class marks dirty are
 *   uploaded before a batch reads them, and the words the GPU wrote are read
 *   back before the host reads them. The dirty range tracked here is the
 *   seam that reconciliation happens at.
 *
 * The 4 MiB wraps. A buffer base plus a page offset that runs past the end of
 * local memory addresses the start of it again, which is what the hardware
 * does and what gs_swizzle.h's block mask reproduces.
 */
#ifndef ICORECOMP_GS_VRAM_H
#define ICORECOMP_GS_VRAM_H

#include "gs_regs.h"
#include "gs_swizzle.h"

#include <cstdint>
#include <vector>

namespace gsr {

/* Bits one pixel occupies in a HOST transfer stream. Not the same as
 * gs_addr_bits(): PSMCT24 addresses a whole word but transfers 24 bits, and
 * the three H formats address a whole word but transfer only their own
 * field. */
inline uint32_t gs_transfer_bits(uint32_t psm) {
    switch (psm) {
        case GS_PSMCT32: case GS_PSMZ32:   return 32;
        case GS_PSMCT24: case GS_PSMZ24:   return 24;
        case GS_PSMCT16: case GS_PSMCT16S:
        case GS_PSMZ16:  case GS_PSMZ16S:  return 16;
        case GS_PSMT8:   case GS_PSMT8H:   return 8;
        case GS_PSMT4:   case GS_PSMT4HL:
        case GS_PSMT4HH:                   return 4;
        default:                           return 0;
    }
}

class LocalMemory {
public:
    LocalMemory() : m_words(GS_VRAM_WORDS, 0u) {}

    const uint32_t* words() const { return m_words.data(); }
    uint32_t* words() { return m_words.data(); }

    /* Reads one pixel in its stored form: the 32-bit word for the 32-bit
     * formats, the 16-bit value for the 16-bit ones, the index for the
     * indexed ones. No colour expansion happens here. */
    uint32_t read_pixel(uint32_t psm, uint32_t base_block, uint32_t fbw,
                        uint32_t x, uint32_t y) const {
        const uint32_t addr = gs_pixel_addr(psm, base_block, fbw, x, y);
        const uint32_t bits = gs_addr_bits(psm);
        const uint32_t word = (addr * bits) >> 5;
        const uint32_t v = m_words[word & (GS_VRAM_WORDS - 1)];
        if (bits == 32) {
            const uint32_t mask = gs_word_mask(psm);
            return (v & mask) >> gs_word_shift(psm);
        }
        const uint32_t shift = (addr * bits) & 31u;
        return (v >> shift) & ((1u << bits) - 1u);
    }

    /* Writes one pixel, leaving the bits the format does not own alone. That
     * matters for PSMCT24 (the top byte belongs to whatever wrote it last)
     * and for the three H formats, which share a word with a colour. */
    void write_pixel(uint32_t psm, uint32_t base_block, uint32_t fbw,
                     uint32_t x, uint32_t y, uint32_t value) {
        const uint32_t addr = gs_pixel_addr(psm, base_block, fbw, x, y);
        const uint32_t bits = gs_addr_bits(psm);
        const uint32_t word = ((addr * bits) >> 5) & (GS_VRAM_WORDS - 1);
        if (bits == 32) {
            const uint32_t mask = gs_word_mask(psm);
            const uint32_t shift = gs_word_shift(psm);
            m_words[word] = (m_words[word] & ~mask) | ((value << shift) & mask);
        } else {
            const uint32_t shift = (addr * bits) & 31u;
            const uint32_t mask = ((1u << bits) - 1u) << shift;
            m_words[word] = (m_words[word] & ~mask) | ((value << shift) & mask);
        }
        mark_dirty(word);
    }

    /* ---- block ownership --------------------------------------------------
     *
     * Local memory has two copies in this renderer: this host store, and the
     * device buffer the shaders read and write. A word can be newer in
     * either. Until 2026-09-05 each side was tracked as one min and max span,
     * which cannot express "the host owns these words and the device owns
     * those", and the measured cost of that was two faults at once. A device
     * readback overwrote a fresh PSMT4 font upload with the device's older
     * copy, so every glyph sprite read leftovers as indices; and every
     * reconciliation moved the whole span, 1.7 MB at a time, about fourteen
     * times a field, which is the 17 ms per field of gswait.
     *
     * The unit is the GS block: 256 bytes, 64 words, 16384 of them in 4 MiB.
     * It is the smallest unit the swizzle keeps contiguous, so a run of
     * blocks is a run of words and one run is one copy.
     *
     * Two bitmaps, with the invariant that a block is in at most one:
     *
     *   host_dirty      the host store holds words the device does not
     *   device_written  the device holds words the host store does not
     *
     * A block in neither is one the two copies agree about. A block in both
     * is a word lost, and both_owners() counts it so it can never be silent.
     * The spans are kept beside the bitmaps purely as a fast reject, so a
     * query over a range nothing has touched costs two comparisons. */
    static constexpr uint32_t kBlockWords = 64;
    static constexpr uint32_t kBlocks = GS_VRAM_WORDS / kBlockWords;
    static constexpr uint32_t kBitWords = kBlocks / 64;

    static uint32_t block_of(uint32_t word) { return word / kBlockWords; }
    static uint32_t block_end_of(uint32_t word_end) {
        return (word_end + kBlockWords - 1) / kBlockWords;
    }
    static uint32_t block_word(uint32_t block) { return block * kBlockWords; }

    bool host_block(uint32_t b) const { return (m_host[b >> 6] >> (b & 63)) & 1u; }
    bool device_block(uint32_t b) const { return (m_dev[b >> 6] >> (b & 63)) & 1u; }

    bool any_host() const { return m_host_lo < m_host_hi; }
    bool any_device() const { return m_dev_lo < m_dev_hi; }
    uint32_t host_lo() const { return m_host_lo; }
    uint32_t host_hi() const { return m_host_hi; }
    uint32_t device_lo() const { return m_dev_lo; }
    uint32_t device_hi() const { return m_dev_hi; }
    uint64_t both_owners() const { return m_both; }

    /* Rule 1's second half: the host has written these words. The device bit
     * is deliberately not cleared here, because clearing it would throw away
     * the device's copy of a block nothing has reconciled; the caller
     * reconciles first, and a block that ends up in both bitmaps is counted
     * rather than resolved by guessing. */
    void note_host_words(uint32_t first, uint32_t last) {
        if (first >= last) return;
        set_run(m_host, block_of(first), block_end_of(last), &m_host_lo, &m_host_hi);
        count_both(block_of(first), block_end_of(last));
    }

    /* Rule 4: the device has written these words, so the host store's copy of
     * them is stale. Any host-dirty block in the range would be a word lost;
     * the caller uploads before it draws, so there should be none, and
     * both_owners() counts any there are. */
    void note_device_words(uint32_t first, uint32_t last) {
        if (first >= last) return;
        set_run(m_dev, block_of(first), block_end_of(last), &m_dev_lo, &m_dev_hi);
        count_both(block_of(first), block_end_of(last));
    }

    /* Rule 3: these blocks' host words are on the device now. */
    void clear_host_blocks(uint32_t b0, uint32_t b1) { clear_run(m_host, b0, b1); }
    /* Rule 2: these blocks' device words are in the host store now. */
    void clear_device_blocks(uint32_t b0, uint32_t b1) { clear_run(m_dev, b0, b1); }

    void mark_all_dirty() {
        for (uint64_t& w : m_host) w = ~0ull;
        for (uint64_t& w : m_dev) w = 0ull;
        m_host_lo = 0;
        m_host_hi = kBlocks;
        m_dev_lo = kBlocks;
        m_dev_hi = 0;
    }

    /* The next run of set bits at or after `from`, as [b0, b1), within
     * [from, end). False when there is none. Runs are what make a
     * reconciliation one copy per run instead of one per block. */
    static bool next_run(const uint64_t* bits, uint32_t from, uint32_t end,
                         uint32_t* b0, uint32_t* b1) {
        uint32_t b = from;
        while (b < end && !((bits[b >> 6] >> (b & 63)) & 1u)) ++b;
        if (b >= end) return false;
        *b0 = b;
        while (b < end && ((bits[b >> 6] >> (b & 63)) & 1u)) ++b;
        *b1 = b;
        return true;
    }
    const uint64_t* host_bits() const { return m_host.data(); }
    const uint64_t* device_bits() const { return m_dev.data(); }

private:
    void mark_dirty(uint32_t word) {
        const uint32_t b = block_of(word);
        m_host[b >> 6] |= 1ull << (b & 63);
        if (b < m_host_lo) m_host_lo = b;
        if (b + 1 > m_host_hi) m_host_hi = b + 1;
        if ((m_dev[b >> 6] >> (b & 63)) & 1u) ++m_both;
    }

    static void set_run(std::vector<uint64_t>& bits, uint32_t b0, uint32_t b1,
                        uint32_t* lo, uint32_t* hi) {
        if (b1 > kBlocks) b1 = kBlocks;
        for (uint32_t b = b0; b < b1; ++b) bits[b >> 6] |= 1ull << (b & 63);
        if (b0 < *lo) *lo = b0;
        if (b1 > *hi) *hi = b1;
    }
    static void clear_run(std::vector<uint64_t>& bits, uint32_t b0, uint32_t b1) {
        if (b1 > kBlocks) b1 = kBlocks;
        for (uint32_t b = b0; b < b1; ++b) bits[b >> 6] &= ~(1ull << (b & 63));
    }
    void count_both(uint32_t b0, uint32_t b1) {
        if (b1 > kBlocks) b1 = kBlocks;
        for (uint32_t b = b0; b < b1; ++b) {
            if (host_block(b) && device_block(b)) ++m_both;
        }
    }

    std::vector<uint32_t> m_words;
    std::vector<uint64_t> m_host = std::vector<uint64_t>(kBitWords, 0ull);
    std::vector<uint64_t> m_dev = std::vector<uint64_t>(kBitWords, 0ull);
    uint32_t m_host_lo = kBlocks;
    uint32_t m_host_hi = 0;
    uint32_t m_dev_lo = kBlocks;
    uint32_t m_dev_hi = 0;
    uint64_t m_both = 0;
};

/* The transfer engine.
 *
 * A transfer is armed by a TRXDIR write and described by the BITBLTBUF,
 * TRXPOS and TRXREG values standing at that moment, which is why this takes
 * the register file rather than a parameter list: the game may write those
 * three in any order and reuse them across transfers.
 *
 * HOST to LOCAL is the only direction that consumes a data stream. The
 * stream arrives as HWREG image data (GIF FLG=2) and may be split across any
 * number of packets, so the engine keeps its position between calls.
 *
 * LOCAL to HOST fills a byte buffer the caller drains. Nothing in this
 * milestone reads it back into the guest; it exists because the direction is
 * part of the register contract and a transfer the renderer silently ignored
 * would be a wrong picture later, not an error now. */
class TransferEngine {
public:
    /* Called on every TRXDIR write. Local-to-local runs to completion here;
     * the other two directions arm a stream. */
    void trxdir(uint64_t value, const RegisterFile& regs, LocalMemory& mem);

    /* HWREG image payload while a host-to-local transfer is armed. Returns
     * the number of qwords consumed, which is fewer than offered when the
     * transfer completes mid-packet. */
    uint32_t host_to_local_data(const uint8_t* data, uint32_t qwords, LocalMemory& mem);

    bool armed_host_to_local() const { return m_mode == GS_XDIR_HOST_TO_LOCAL && m_active; }
    bool armed_local_to_host() const { return m_mode == GS_XDIR_LOCAL_TO_HOST && m_active; }

    /* There is no readback accessor and no readback buffer.
     *
     * A LOCAL to HOST transfer is read back through the GS FIFO at
     * 0x12001000, and this runtime has no reader for it: nothing in
     * hw/gspriv.cpp or anywhere else consumes one. The bytes were being
     * accumulated into a vector on the promise that the caller drained it,
     * and no caller was ever written, so a guest reading local memory once a
     * field grew the vector by the rectangle's size every field until the
     * process was killed, and the read was silently wrong as well.
     *
     * local_to_host now counts the pixels, says once that the direction is
     * not delivered, and keeps nothing. When a FIFO reader arrives it takes
     * the packing loop with it; until then the counter and the log line are
     * what say the transfer happened. */

    /* Counters for the end-of-run report. */
    uint64_t host_to_local_pixels = 0;
    uint64_t local_to_local_pixels = 0;
    uint64_t local_to_host_pixels = 0;
    uint64_t overrun_qwords = 0;   /* image data with no transfer armed */
    uint64_t unsupported_psm = 0;  /* a transfer format with no bit width */

private:
    void local_to_local(LocalMemory& mem);
    void local_to_host(LocalMemory& mem);

    bool m_active = false;
    uint32_t m_mode = GS_XDIR_STOP;
    /* The transfer parameters latched at the TRXDIR write. */
    Bitbltbuf m_buf{};
    Trxpos m_pos{};
    Trxreg m_reg{};
    /* Host-to-local stream position: the next pixel index inside the
     * RRW x RRH rectangle, and the leftover bits of the previous qword for
     * the formats whose pixels straddle a byte or a qword boundary. */
    uint64_t m_pixel = 0;
    uint64_t m_bit_carry_value = 0;
    uint32_t m_bit_carry_bits = 0;
};

} // namespace gsr

#endif /* ICORECOMP_GS_VRAM_H */
