/* gs/render/gs_clut.h: the CLUT buffer and its load rules.
 *
 * Ours (MIT). The GS User's Manual's CLUT chapter: the 1 KB buffer, CSM1's
 * 16 by 16 arrangement and CSM2's linear one, CBP0 and CBP1, and the CLD
 * table that decides whether a TEX0 write loads the buffer at all.
 *
 * Why this is CPU state and not a GPU pass. Every event that can load the
 * CLUT is a register write, which this renderer sees on the CPU, and the
 * source is local memory, of which the host holds a copy. Loading here costs
 * one pass over at most 256 texels and produces 1 KB of constant data that
 * travels with the primitive batch; loading on the GPU would need the load
 * ordered against the drawing that precedes it, which is a dependency the
 * batch boundary already expresses.
 *
 * One buffer, not one per context, and why. The manual gives the CLUT one
 * buffer and two comparison registers, CBP0 and CBP1, which CLD 2 to 5 store
 * to and compare against. Two comparison registers for one buffer is the
 * arrangement that makes CLD 4 and 5 useful: a program alternating between
 * two palettes keeps both base pointers remembered and reloads only when the
 * one it is about to use is not the one standing. A buffer per context would
 * make the second comparison register redundant. If a capture ever shows the
 * two contexts holding different palettes at once, this struct becomes an
 * array of two and every caller passes its context index; nothing else about
 * the model changes.
 *
 * The whole of the load path reads the host copy of local memory. The caller
 * is responsible for having that copy current: gs_native.cpp flushes the
 * open batch and reads the device buffer back when the CLUT's source pages
 * are ones the rasteriser has written.
 */
#ifndef ICORECOMP_GS_CLUT_H
#define ICORECOMP_GS_CLUT_H

#include "gs_texture.h"
#include "gs_vram.h"

#include <cstdint>

namespace gsr {

/* The CLUT buffer is 1 KB, which is GS_CLUT_WORDS words (gs_texture.h, where
 * the shader reads the same constant). It is addressed in entries of the
 * CLUT's own format: 256 word entries when CPSM is 32-bit, 512 half-word
 * entries when it is 16-bit. gs_texture.h's gs_clut_entry() gives the entry
 * a texel index names; this file only fills the buffer. */

class ClutCache {
public:
    /* The source of every load. Not owned. */
    void set_memory(const LocalMemory* mem) { m_mem = mem; }

    /* Whether the CLD field of this TEX0 value would load the buffer, given
     * the CBP0/CBP1 standing now. Pure: the caller uses it to decide whether
     * local memory has to be made current before tex0_written() runs. */
    bool would_load(uint64_t tex0) const;

    /* A TEX0_1, TEX0_2 or TEX2 write has landed. Applies the CLD rule, loads
     * the buffer if the rule says to, and updates CBP0/CBP1. `texclut` is
     * the TEXCLUT register, read only when CSM is 2. */
    void tex0_written(uint64_t tex0, uint64_t texclut);

    /* The word range of local memory a load of this TEX0 would read, as
     * [first, last). Conservative: whole pages. */
    void source_word_range(uint64_t tex0, uint64_t texclut, uint32_t* first_word,
                           uint32_t* last_word) const;

    const uint32_t* words() const { return m_words; }

    /* Bumped every time the buffer's contents change, so a batch can tell
     * whether it already holds a snapshot of this palette without comparing
     * 1 KB. Starts at 1, so zero is "no snapshot yet". */
    uint32_t serial() const { return m_serial; }

    struct Stats {
        uint64_t writes = 0;        /* TEX0/TEX2 writes seen */
        uint64_t loads = 0;         /* loads actually performed */
        uint64_t skipped = 0;       /* CLD 0, and CLD 4/5 whose CBP matched */
        uint64_t csm2_loads = 0;
        uint64_t unknown_cld = 0;   /* CLD 6 and 7, which the manual leaves out */
    };
    const Stats& stats() const { return m_stats; }

private:
    void load(uint64_t tex0, uint64_t texclut);
    void store_entry(uint32_t entry, uint32_t value, uint32_t cpsm);

    const LocalMemory* m_mem = nullptr;
    uint32_t m_words[GS_CLUT_WORDS] = {};
    uint32_t m_cbp0 = 0;
    uint32_t m_cbp1 = 0;
    uint32_t m_serial = 1;
    Stats m_stats;
    bool m_said_unknown_cld = false;
    bool m_said_csm2_32 = false;
};

} // namespace gsr

#endif /* ICORECOMP_GS_CLUT_H */
