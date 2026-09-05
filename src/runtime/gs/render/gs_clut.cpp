/* gs/render/gs_clut.cpp: the CLUT buffer and its load rules. See gs_clut.h.
 *
 * Ours (MIT). Written from the GS User's Manual's CLUT chapter.
 */
#include "gs_clut.h"

/* For gs_buffer_word_range, which source_word_range uses so that a CLUT's
 * source range is described the same way every other buffer's is. */
#include "gs_draw.h"

#include "../../runtime.h"

namespace gsr {

namespace {

/* TEXCLUT, which only CSM2 reads:
 *
 *   CBW 0..5 (buffer width, 64-texel units)   COU 6..11 (offset, 16 texels)
 *   COV 12..21 (offset, in lines)
 */
struct Texclut {
    uint32_t cbw, cou, cov;
};

Texclut decode_texclut(uint64_t v) {
    Texclut t{};
    t.cbw = (uint32_t)(v & 0x3Full);
    t.cou = (uint32_t)((v >> 6) & 0x3Full);
    t.cov = (uint32_t)((v >> 12) & 0x3FFull);
    return t;
}

/* The number of entries a palette has: a 4-bit texture reads 16 of them and
 * an 8-bit texture 256. Any other texture format loads nothing, because the
 * manual defines a CLUT only for the indexed formats. */
uint32_t clut_entries(uint32_t psm) {
    const uint32_t bits = gs_tex_index_bits(psm);
    if (bits == 4) return 16;
    if (bits == 8) return 256;
    return 0;
}

/* CSM1's source rectangle: a 256-entry palette is a 16 by 16 region and a
 * 16-entry palette an 8 by 2 one, both at CBP, both read through the CLUT's
 * own pixel format. Buffer width is one 64-texel unit: both rectangles fit
 * inside the first block column of a page, so the width only decides
 * addressing that neither of them reaches. */
uint32_t csm1_width(uint32_t entries) { return entries == 16 ? 8u : 16u; }

} // namespace

bool ClutCache::would_load(uint64_t tex0) const {
    const uint32_t hi = (uint32_t)(tex0 >> 32);
    const uint32_t cld = gs_tex0_cld(hi);
    const uint32_t cbp = gs_tex0_cbp(hi);
    /* The manual's CLD table:
     *
     *   0  keep the buffer standing
     *   1  load
     *   2  load, and remember CBP in CBP0
     *   3  load, and remember CBP in CBP1
     *   4  load only when CBP differs from CBP0, then remember it there
     *   5  load only when CBP differs from CBP1, then remember it there
     *
     * 6 and 7 are not in the table. They load, and say so once, rather than
     * being silently treated as one of the six. */
    if (cld == 0) return false;
    if (cld == 4) return cbp != m_cbp0;
    if (cld == 5) return cbp != m_cbp1;
    return true;
}

void ClutCache::tex0_written(uint64_t tex0, uint64_t texclut) {
    ++m_stats.writes;
    const uint32_t hi = (uint32_t)(tex0 >> 32);
    const uint32_t cld = gs_tex0_cld(hi);
    const uint32_t cbp = gs_tex0_cbp(hi);
    const bool doload = would_load(tex0);

    if (cld == 6 || cld == 7) {
        ++m_stats.unknown_cld;
        if (!m_said_unknown_cld) {
            m_said_unknown_cld = true;
            rt_log_warn("gsr", "TEX0 CLD %u is not in the manual's table; the CLUT is "
                               "loaded, which is what CLD 1 does", cld);
        }
    }

    /* The comparison happens against the value standing before this write,
     * and the store happens whether or not the load did. */
    if (cld == 2 || cld == 4) m_cbp0 = cbp;
    if (cld == 3 || cld == 5) m_cbp1 = cbp;

    if (!doload) {
        ++m_stats.skipped;
        return;
    }
    load(tex0, texclut);
}

void ClutCache::store_entry(uint32_t entry, uint32_t value, uint32_t cpsm) {
    if (cpsm == GS_PSMCT32) {
        m_words[entry & (GS_CLUT_WORDS - 1u)] = value;
        return;
    }
    /* A 16-bit CLUT holds 512 half-word entries in the same kilobyte. */
    const uint32_t word = (entry >> 1) & (GS_CLUT_WORDS - 1u);
    const uint32_t shift = (entry & 1u) * 16u;
    m_words[word] = (m_words[word] & ~(0xFFFFu << shift))
                  | ((value & 0xFFFFu) << shift);
}

void ClutCache::load(uint64_t tex0, uint64_t texclut) {
    if (!m_mem) return;
    const uint32_t lo = (uint32_t)tex0;
    const uint32_t hi = (uint32_t)(tex0 >> 32);
    const uint32_t psm = gs_tex0_psm(lo);
    const uint32_t entries = clut_entries(psm);
    if (entries == 0) {
        /* A CLUT load asked for by a texture format that has no palette.
         * Nothing is loaded and nothing is invented; the buffer keeps what
         * it held. */
        ++m_stats.skipped;
        return;
    }
    const uint32_t cbp = gs_tex0_cbp(hi);
    const uint32_t cpsm = gs_tex0_cpsm(hi);
    const uint32_t csm = gs_tex0_csm(hi);
    const uint32_t csa = gs_tex0_csa(hi);

    if (csm == 0) {
        /* CSM1. The source is read in raster order and stored in the buffer
         * in that same order, so the 16 by 16 arrangement lives entirely on
         * the read side: gs_texture.h's gs_clut_slot_csm1_8() is what turns
         * a texel index into the slot this loop filled. A 16-entry palette
         * has no such permutation and CSA picks the group of 16 it lands
         * in. */
        const uint32_t w = csm1_width(entries);
        const uint32_t h = entries / w;
        /* A 256-entry palette starts at entry 0 and CSA is not applied. That
         * is inferred, not measured: gs_texture.h's gs_clut_entry() says why
         * and docs/GS_RENDERER.md lists it. The read side makes the same
         * choice, so the two agree whichever way it is settled. */
        const uint32_t base_entry = (entries == 16) ? (csa * 16u) : 0u;
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const uint32_t v = m_mem->read_pixel(cpsm, cbp, 1, x, y);
                store_entry(base_entry + y * w + x, v, cpsm);
            }
        }
    } else {
        /* CSM2. The palette is one line of `entries` texels starting at
         * (COU * 16, COV) in a buffer CBW wide, stored into the CLUT in
         * index order with no permutation at all. The manual gives CSM2 for
         * 16-bit CLUTs; a 32-bit one is loaded the same way and says so
         * once, rather than being dropped. */
        ++m_stats.csm2_loads;
        if (cpsm == GS_PSMCT32 && !m_said_csm2_32) {
            m_said_csm2_32 = true;
            rt_log_warn("gsr", "CSM2 with a 32-bit CLUT: the manual describes CSM2 for "
                               "16-bit CLUTs, and this load is treated as the same "
                               "linear read at 32 bits");
        }
        const Texclut tc = decode_texclut(texclut);
        /* 256 entries start at entry 0 here too; see the CSM1 arm above. */
        const uint32_t base_entry = (entries == 16) ? (csa * 16u) : 0u;
        for (uint32_t i = 0; i < entries; ++i) {
            const uint32_t v = m_mem->read_pixel(cpsm, cbp, tc.cbw, tc.cou * 16u + i,
                                                 tc.cov);
            store_entry(base_entry + i, v, cpsm);
        }
    }
    ++m_stats.loads;
    ++m_serial;
}

void ClutCache::source_word_range(uint64_t tex0, uint64_t texclut, uint32_t* first_word,
                                  uint32_t* last_word) const {
    const uint32_t lo = (uint32_t)tex0;
    const uint32_t hi = (uint32_t)(tex0 >> 32);
    const uint32_t entries = clut_entries(gs_tex0_psm(lo));
    const uint32_t cbp = gs_tex0_cbp(hi);
    const uint32_t csm = gs_tex0_csm(hi);
    /* 256 bytes to a block, four bytes to a word. */
    uint64_t first = (uint64_t)cbp * 64ull;
    uint64_t words;
    if (entries == 0) {
        first = 0;
        words = 0;
    } else if (csm == 0) {
        /* A 16 by 16 region of 32-bit texels is four blocks; the 8 by 2 and
         * the 16-bit cases are smaller and fit inside the same span. */
        words = 4ull * 64ull;
    } else {
        /* CSM2 reads one line of the buffer, at (COU * 16, COV). The range
         * has to start from COV's page row and not from CBP alone: derived
         * from the run's x extent by itself it named the first page or two
         * after CBP, which for a palette at COV 32 or more covers pages the
         * load never reads and misses the ones it does. The caller then sees
         * no overlap with what the rasteriser wrote, skips the readback, and
         * loads the palette from a stale host copy of local memory with no
         * log line and a picture in the previous frame's colours.
         *
         * gs_buffer_word_range is the same function the transfer and drawing
         * paths use. It covers whole page rows from CBP through the row
         * holding max_y, across the buffer's full CBW width, so the row
         * offset comes with it. A run wider than the buffer carries on into
         * the next page row, which is what the overflow term adds. */
        const Texclut tc = decode_texclut(texclut);
        const uint32_t cpsm = gs_tex0_cpsm(hi);
        const uint32_t cbw = tc.cbw ? tc.cbw : 1u;
        const uint32_t width = cbw * 64u;
        const uint32_t last_x = tc.cou * 16u + entries;
        const uint32_t page_h = gs_page_height(gs_psm_family(cpsm));
        const uint32_t max_y = tc.cov + (last_x / width) * (page_h ? page_h : 1u);
        gs_buffer_word_range(cpsm, cbp, cbw, max_y, first_word, last_word);
        return;
    }
    if (first >= GS_VRAM_WORDS || first + words > GS_VRAM_WORDS) {
        *first_word = 0;
        *last_word = words ? GS_VRAM_WORDS : 0;
        return;
    }
    *first_word = (uint32_t)first;
    *last_word = (uint32_t)(first + words);
}

} // namespace gsr
