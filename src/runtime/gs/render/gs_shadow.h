/* gs/render/gs_shadow.h: the super-sampled shadow of local memory, and the
 * state machine that says which of its pages mean anything.
 *
 * Ours (MIT). The CPU half of display.render_scale; shaders/shadow.comp is
 * the GPU half and shaders/raster.comp is what writes into it.
 *
 * ---- what the shadow is ------------------------------------------------------
 *
 * At render scale N the fine pass keeps N samples per pixel. Those samples
 * have to live somewhere between one batch and the next, because a later
 * primitive blends against what an earlier one left and a later depth test
 * compares against the depth it wrote, and both of those are per sample.
 *
 * The shadow is N copies of the whole of local memory, one plane per sample,
 * in exactly the native swizzle: the word for sample s of native word w is
 * s * GS_VRAM_WORDS + w. Nothing about the addressing changes, so every rule
 * in gs_swizzle.h is used unaltered and a frame buffer, a Z buffer and a
 * texture all land where they always did.
 *
 * The mapping is the identity rather than an allocation table. An allocator
 * keyed by page would save memory for a game that renders into a small part
 * of local memory, and ICO is not one: a 512 by 448 PSMCT32 frame buffer is
 * 112 of the 512 pages and its Z buffer another 112, so a table would be
 * carrying nearly half the store anyway and would put a lookup in the inner
 * loop of the fine pass. What is kept per page is one bit, below.
 *
 * Memory cost, which is the price of the setting and is stated on the log
 * line at startup:
 *
 *   scale 1    nothing, no shadow exists
 *   scale 4    16 MiB
 *   scale 8    32 MiB
 *   scale 16   64 MiB
 *
 * ---- valid, and when a page stops being valid --------------------------------
 *
 * A page's shadow is valid when its N planes hold the samples of that page and
 * not stale data. The rules, and they are the whole state machine:
 *
 *   invalid -> valid   a seed pass broadcast the native page into every
 *                      plane. Run for the pages a batch is about to draw into
 *                      that are not valid yet.
 *   valid -> invalid   anything wrote the native page other than a draw at
 *                      this scale: a HOST to LOCAL transfer, either side of a
 *                      LOCAL to LOCAL, or any other host write. The renderer
 *                      calls invalidate_words over the range it uploaded.
 *   all -> invalid     the scale changed, so the plane count changed and
 *                      nothing in the old shadow addresses the new one.
 *
 * A drop is never a correctness problem: the native copy is resolved after
 * every batch, so it always holds a whole picture, and a dropped page is
 * re-seeded from it before the next draw. What a drop costs is the sub-sample
 * detail of that page, which shows as one batch of edges resolved from N
 * copies of one value rather than from N samples.
 *
 * A read does not invalidate anything. Textures and the CRTC read native
 * local memory, which the resolve keeps current, so a feedback pass sees the
 * same picture at every scale.
 */
#ifndef ICORECOMP_GS_SHADOW_H
#define ICORECOMP_GS_SHADOW_H

#include "gs_draw.h"
#include "gs_swizzle.h"

#include <cstddef>   /* offsetof, for the push block layout assertions */
#include <cstdint>
#include <vector>

namespace gsr {

/* shadow.comp's push constants. Plain scalars, so std140 and std430 agree and
 * the struct can be memcpy'd into the command list; 52 bytes, inside the
 * RHI's 128-byte budget. The field order has to stay in step with the
 * shader. */
struct ShadowPush {
    uint32_t mode;             /* GSR_SHADOW_SEED or GSR_SHADOW_RESOLVE */
    uint32_t samples;
    uint32_t frame_base_block;
    uint32_t frame_bw;
    uint32_t frame_psm;
    uint32_t frame_mask;
    uint32_t z_base_block;
    uint32_t z_psm;
    uint32_t z_write;
    uint32_t x0, y0, x1, y1;   /* the resolve rectangle, inclusive */
};

/* 13 words, in the order shaders/shadow.comp declares them. */
static_assert(sizeof(ShadowPush) == 13 * sizeof(uint32_t),
              "ShadowPush must stay 13 words, in step with shaders/shadow.comp");
static_assert(offsetof(ShadowPush, x0) == 9 * sizeof(uint32_t),
              "the resolve rectangle is the last four words");

enum : uint32_t {
    GSR_SHADOW_SEED    = 0,
    GSR_SHADOW_RESOLVE = 1,
};

/* The sample counts display.render_scale allows. 2 is deliberately absent;
 * docs/SETTINGS.md section 6 says why. */
inline bool gs_scale_allowed(uint32_t samples) {
    return samples == 1 || samples == 4 || samples == 8 || samples == 16;
}

/* Bytes the shadow occupies at a scale. */
inline uint64_t gs_shadow_bytes(uint32_t samples) {
    return samples <= 1 ? 0ull : (uint64_t)samples * (uint64_t)GS_VRAM_BYTES;
}

/* The tile the fine pass runs at for a sample count. A workgroup is always
 * 256 threads and always one thread per sample, so the tile shrinks as the
 * sample count grows and the threadgroup arrays never do:
 *
 *   1 -> 16 by 16    4 -> 8 by 8    8 -> 8 by 4    16 -> 4 by 4
 */
inline void gs_scale_tile(uint32_t samples, uint32_t* tile_w, uint32_t* tile_h) {
    switch (samples) {
        case 4:  *tile_w = 8; *tile_h = 8; break;
        case 8:  *tile_w = 8; *tile_h = 4; break;
        case 16: *tile_w = 4; *tile_h = 4; break;
        default: *tile_w = GSP_TILE_PIXELS; *tile_h = GSP_TILE_PIXELS; break;
    }
}

class ShadowPages {
public:
    /* The scale in force. A change drops every page, because the plane count
     * the addresses are built from changed. Returns true when something was
     * dropped, which is what the caller logs. */
    bool set_samples(uint32_t samples) {
        if (samples == m_samples) return false;
        m_samples = samples;
        const bool had = any_valid();
        invalidate_all();
        return had;
    }
    uint32_t samples() const { return m_samples; }
    bool active() const { return m_samples > 1; }

    bool valid(uint32_t page) const {
        page &= (GSP_PAGE_COUNT - 1u);
        return (m_valid[page >> 5] & (1u << (page & 31u))) != 0;
    }
    void mark_valid(uint32_t page) {
        page &= (GSP_PAGE_COUNT - 1u);
        m_valid[page >> 5] |= 1u << (page & 31u);
    }
    void drop(uint32_t page) {
        page &= (GSP_PAGE_COUNT - 1u);
        m_valid[page >> 5] &= ~(1u << (page & 31u));
    }

    bool any_valid() const {
        for (uint32_t i = 0; i < GSP_PAGE_WORDS; ++i) {
            if (m_valid[i]) return true;
        }
        return false;
    }

    void invalidate_all() {
        for (uint32_t i = 0; i < GSP_PAGE_WORDS; ++i) m_valid[i] = 0;
        ++m_stats.drop_alls;
    }

    /* A native write over [first_word, last_word). Every page it touches
     * loses its shadow. */
    void invalidate_words(uint32_t first_word, uint32_t last_word) {
        if (last_word <= first_word) return;
        const uint32_t first_page = first_word / 2048u;
        const uint32_t last_page = (last_word - 1u) / 2048u;
        for (uint32_t p = first_page; p <= last_page && p < GSP_PAGE_COUNT; ++p) {
            if (valid(p)) ++m_stats.drops;
            drop(p);
        }
    }

    /* The pages of `writes` that have no valid shadow yet, in page order.
     * Marks them valid, because the caller's next act is the seed pass that
     * makes them so. */
    void take_seed_list(const PageSet& writes, std::vector<uint32_t>& out) {
        out.clear();
        for (uint32_t p = 0; p < GSP_PAGE_COUNT; ++p) {
            if ((writes.bits[p >> 5] & (1u << (p & 31u))) == 0) continue;
            if (valid(p)) continue;
            out.push_back(p);
            mark_valid(p);
        }
        m_stats.seeds += out.size();
    }

    /* True when every page of `reads` has a valid shadow. That is the test
     * the high-resolution scanout decision makes: a display buffer the game
     * drew into has one, a display buffer a transfer filled does not. */
    bool all_valid(const PageSet& reads) const {
        for (uint32_t i = 0; i < GSP_PAGE_WORDS; ++i) {
            if (reads.bits[i] & ~m_valid[i]) return false;
        }
        return true;
    }

    struct Stats {
        uint64_t seeds = 0;      /* pages broadcast into the shadow */
        uint64_t drops = 0;      /* pages dropped by a native write */
        uint64_t drop_alls = 0;  /* whole-shadow drops, one per scale change */
        uint64_t resolves = 0;   /* resolve dispatches */
    };
    const Stats& stats() const { return m_stats; }
    void note_resolve() { ++m_stats.resolves; }

private:
    uint32_t m_samples = 1;
    uint32_t m_valid[GSP_PAGE_WORDS] = {};
    Stats m_stats;
};

} // namespace gsr

#endif /* ICORECOMP_GS_SHADOW_H */
