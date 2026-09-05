/* gs/render/gs_draw.cpp: primitive assembly. See gs_draw.h for the shape.
 *
 * Ours (MIT). The vertex queue and its retention rules, the coordinate
 * conversion, the fixed-point setup and the coarse binning.
 *
 * ---- binning happens here, on the CPU, and why ------------------------------
 *
 * The alternative was a compute pass that bins on the GPU. This does it on
 * the CPU for three reasons, in order of weight:
 *
 *   1. Submission order. The fine pass must walk a bin's primitives in the
 *      order the GIF delivered them, or a same-pixel overlap resolves the
 *      wrong way. Appending on the CPU while assembling gives that ordering
 *      for nothing. A GPU bin pass would need either an atomic append plus a
 *      sort by primitive index afterwards, or a count pass, a prefix sum and
 *      a fill pass: three dispatches and a barrier chain to recover an
 *      ordering the CPU never lost.
 *   2. The bounding box is already in hand. Assembly computes the clipped
 *      bounding box anyway, to decide whether the primitive survives the
 *      scissor at all, so binning is an integer loop over the bins that box
 *      touches and costs no second pass over the geometry.
 *   3. The transfer has to happen regardless. The records travel to the
 *      device over PCIe either way; the bin lists add a few kilobytes to a
 *      copy that is already being made.
 *
 * What would change the answer is primitive counts far above what this game
 * submits. A frame of ICO is thousands of primitives, and the counting sort
 * below is linear in primitives times bins touched. If a later milestone
 * finds a frame where this shows up in a profile, the GPU path is a drop-in
 * replacement for build_bins() alone.
 */
#include "gs_draw.h"

#include "../../runtime.h"

#include <cmath>
#include <cstring>

namespace gsr {

namespace {

/* The batch is flushed at this many primitives whatever else happens, so one
 * dispatch's buffers stay a bounded allocation. 32768 records is 3.4 MiB. */
constexpr uint32_t kMaxBatchPrims = 32768;

/* Floor division by 16, which is what an arithmetic right shift is for a
 * signed value in C++20. Named so the intent is not mistaken for a truncation
 * towards zero, which would be wrong on the left and top of the screen. */
inline int32_t floor16(int32_t v) { return v >> 4; }

inline int32_t imin(int32_t a, int32_t b) { return a < b ? a : b; }
inline int32_t imax(int32_t a, int32_t b) { return a > b ? a : b; }

/* A double into the 32.32 pair gs_prim.h's Z DDA reads. The integer part is
 * taken modulo 2^32 because Z is a 32-bit value and the hardware's own Z
 * register wraps; the fraction is the part below it, always non-negative
 * because the split is at the floor. A non-finite value can only come from a
 * plane solve on a triangle whose area underflowed, and answers zero rather
 * than a trap. */
void store_fixed(double v, uint32_t* ip, uint32_t* fp) {
    if (!std::isfinite(v)) {
        *ip = 0;
        *fp = 0;
        return;
    }
    const double fl = std::floor(v);
    double m = std::fmod(fl, 4294967296.0);
    if (m < 0.0) m += 4294967296.0;
    if (!std::isfinite(m)) m = 0.0;
    *ip = (uint32_t)(uint64_t)m;
    double fr = v - fl;
    if (!(fr >= 0.0)) fr = 0.0;
    if (fr > 0.9999999997) fr = 0.9999999997;
    *fp = (uint32_t)(uint64_t)(fr * 4294967296.0);
}

/* A double into 16.16, saturating rather than wrapping: the values that go
 * through here are a line's minor coordinate and its slope, both of which
 * are bounded by the drawing area for any line that draws anything, so a
 * value outside the range belongs to a line that covers no pixel. */
int32_t store_16_16(double v) {
    if (!std::isfinite(v)) return 0;
    const double scaled = v * 65536.0;
    if (scaled >= 2147483647.0) return 2147483647;
    if (scaled <= -2147483648.0) return -2147483647 - 1;
    return (int32_t)std::lround(scaled);
}

} // namespace

void gs_buffer_word_range(uint32_t psm, uint32_t base_block, uint32_t bw, uint32_t max_y,
                          uint32_t* first_word, uint32_t* last_word) {
    const uint32_t fam = gs_psm_family(psm);
    const uint32_t pw = gs_page_width(fam);
    const uint32_t ph = gs_page_height(fam);
    if (pw == 0 || ph == 0) {
        *first_word = 0;
        *last_word = GS_VRAM_WORDS;
        return;
    }
    uint32_t across = (bw * 64u) / pw;
    if (across == 0) across = 1u;
    const uint64_t rows = (uint64_t)(max_y / ph) + 1u;
    const uint64_t words = (uint64_t)across * rows * 2048ull;
    /* 256 bytes to a block, four bytes to a word. */
    const uint64_t f = (uint64_t)base_block * 64ull;
    if (f >= GS_VRAM_WORDS || f + words > GS_VRAM_WORDS) {
        *first_word = 0;
        *last_word = GS_VRAM_WORDS;
        return;
    }
    *first_word = (uint32_t)f;
    *last_word = (uint32_t)(f + words);
}

void gs_mark_pages(uint32_t psm, uint32_t base_block, uint32_t bw, int32_t x0,
                   int32_t y0, int32_t x1, int32_t y1, PageSet* out) {
    if (x1 < x0 || y1 < y0) return;
    const uint32_t fam = gs_psm_family(psm);
    const uint32_t pw = gs_page_width(fam);
    const uint32_t ph = gs_page_height(fam);
    if (fam == GS_FAM_BAD || pw == 0 || ph == 0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    uint32_t across = (bw * 64u) / pw;
    if (across == 0) across = 1u;
    /* A base that is not a whole number of pages puts every page of the
     * buffer across two hardware pages, so both are marked. TEX0 TBP0 is in
     * blocks and is free to be unaligned; FRAME and ZBUF are in pages and
     * never are. */
    const bool straddles = (base_block & (GS_PAGE_BLOCKS - 1u)) != 0;
    const uint32_t page0 = (uint32_t)y0 / ph;
    const uint32_t page1 = (uint32_t)y1 / ph;
    const uint32_t col0 = (uint32_t)x0 / pw;
    const uint32_t col1 = (uint32_t)x1 / pw;
    for (uint32_t py = page0; py <= page1; ++py) {
        for (uint32_t px = col0; px <= col1; ++px) {
            const uint32_t block = (base_block + (py * across + px) * GS_PAGE_BLOCKS)
                                   & (GS_VRAM_BLOCKS - 1u);
            out->add(block >> 5);
            if (straddles) out->add((block >> 5) + 1u);
        }
    }
}

void gs_mark_page_words(uint32_t first_word, uint32_t last_word, PageSet* out) {
    if (last_word <= first_word) return;
    /* 2048 words to a page. */
    const uint32_t first_page = first_word / 2048u;
    const uint32_t last_page = (last_word - 1u) / 2048u;
    for (uint32_t p = first_page; p <= last_page; ++p) out->add(p);
}

void DrawEngine::note_once(bool& flag, const char* what) {
    if (flag) return;
    flag = true;
    rt_log_info("gsr", "%s", what);
}

void DrawEngine::prim_written() {
    /* A PRIM write starts a new primitive: the queue is emptied, which is
     * how a strip is restarted without an ADC vertex. */
    m_queued = 0;
}

void DrawEngine::push_vertex(const DrawVertex& v) {
    if (m_queued < 3) {
        m_queue[m_queued++] = v;
    } else {
        m_queue[0] = m_queue[1];
        m_queue[1] = m_queue[2];
        m_queue[2] = v;
    }
}

void DrawEngine::vertex(uint32_t addr, uint64_t value) {
    const bool kicks = (addr == GS_REG_XYZ2 || addr == GS_REG_XYZF2);
    const bool with_fog = (addr == GS_REG_XYZF2 || addr == GS_REG_XYZF3);

    /* The attribute source: PRMODECONT AC 1 takes the attribute bits from
     * PRIM, AC 0 takes them from PRMODE. The primitive type itself always
     * comes from the last PRIM write. */
    const uint64_t prim_reg = m_regs.read(GS_REG_PRIM);
    const uint64_t prmode = m_regs.read(GS_REG_PRMODE);
    const bool use_prim = (m_regs.read(GS_REG_PRMODECONT) & 1ull) != 0;
    m_attr = decode_prim(use_prim ? prim_reg : prmode);
    m_attr.prim = (uint32_t)(prim_reg & 7ull);
    m_ctxt = m_attr.ctxt;

    const Xyoffset off = decode_xyoffset(
        m_regs.read(m_ctxt ? GS_REG_XYOFFSET_2 : GS_REG_XYOFFSET_1));

    DrawVertex v;
    v.x = (int32_t)(uint32_t)(value & 0xFFFFull) - off.ofx;
    v.y = (int32_t)(uint32_t)((value >> 16) & 0xFFFFull) - off.ofy;
    if (with_fog) {
        /* XYZF2 carries Z in 24 bits and F in the top byte. */
        v.z = (uint32_t)((value >> 32) & 0x00FFFFFFull);
        v.fog = (uint32_t)((value >> 56) & 0xFFull);
    } else {
        v.z = (uint32_t)((value >> 32) & 0xFFFFFFFFull);
        v.fog = (uint32_t)((m_regs.read(GS_REG_FOG) >> 56) & 0xFFull);
    }

    const uint64_t rgbaq = m_regs.read(GS_REG_RGBAQ);
    v.rgba = (uint32_t)(rgbaq & 0xFFFFFFFFull);
    v.q = (uint32_t)((rgbaq >> 32) & 0xFFFFFFFFull);

    const uint64_t st = m_regs.read(GS_REG_ST);
    v.s = (uint32_t)(st & 0xFFFFFFFFull);
    v.t = (uint32_t)((st >> 32) & 0xFFFFFFFFull);

    const uint64_t uv = m_regs.read(GS_REG_UV);
    v.u = (uint32_t)(uv & 0x3FFFull);
    v.v = (uint32_t)((uv >> 32) & 0x3FFFull);

    push_vertex(v);
    if (!kicks) return;

    ++m_stats.kicks;
    kick();
}

void DrawEngine::kick() {
    /* The retention rules. Each case draws from the queue and then leaves
     * behind exactly what the next primitive of that kind needs:
     *
     *   point, line, triangle, sprite   nothing
     *   line strip                      the last vertex
     *   triangle strip                  the last two
     *   triangle fan                    the first and the last
     *
     * A queue with fewer vertices than the kind needs draws nothing and
     * keeps what it has, which is how the first vertices of a strip behave. */
    switch (m_attr.prim) {
        case 0: /* point */
            if (m_queued >= 1) {
                emit_point(m_queue[m_queued - 1]);
                m_queued = 0;
            }
            break;
        case 1: /* line */
            if (m_queued >= 2) {
                emit_line(m_queue[m_queued - 2], m_queue[m_queued - 1]);
                m_queued = 0;
            }
            break;
        case 2: /* line strip */
            if (m_queued >= 2) {
                emit_line(m_queue[m_queued - 2], m_queue[m_queued - 1]);
                m_queue[0] = m_queue[m_queued - 1];
                m_queued = 1;
            }
            break;
        case 3: /* triangle */
            if (m_queued >= 3) {
                emit_triangle(m_queue[0], m_queue[1], m_queue[2]);
                m_queued = 0;
            }
            break;
        case 4: /* triangle strip */
            if (m_queued >= 3) {
                emit_triangle(m_queue[0], m_queue[1], m_queue[2]);
                m_queue[0] = m_queue[1];
                m_queue[1] = m_queue[2];
                m_queued = 2;
            }
            break;
        case 5: /* triangle fan: the first vertex stays put */
            if (m_queued >= 3) {
                emit_triangle(m_queue[0], m_queue[1], m_queue[2]);
                m_queue[1] = m_queue[2];
                m_queued = 2;
            }
            break;
        case 6: /* sprite */
            if (m_queued >= 2) {
                emit_sprite(m_queue[m_queued - 2], m_queue[m_queued - 1]);
                m_queued = 0;
            }
            break;
        default: /* 7 is reserved and draws nothing */
            ++m_stats.reserved_prim;
            note_once(m_said_reserved,
                      "PRIM type 7 is reserved and draws nothing; the kick was dropped");
            m_queued = 0;
            break;
    }
}

/* ---- record construction --------------------------------------------------- */

void DrawEngine::begin_record(uint32_t kind) {
    const uint64_t frame = m_regs.read(m_ctxt ? GS_REG_FRAME_2 : GS_REG_FRAME_1);
    const uint64_t zbuf = m_regs.read(m_ctxt ? GS_REG_ZBUF_2 : GS_REG_ZBUF_1);
    const uint64_t fogcol = m_regs.read(GS_REG_FOGCOL);
    const uint64_t dimx = m_regs.read(GS_REG_DIMX);

    const bool key_differs = m_have_key
        && (frame != m_key_frame || zbuf != m_key_zbuf
            || fogcol != m_key_fogcol || dimx != m_key_dimx);
    if ((key_differs || prim_count() >= kMaxBatchPrims) && !m_prims.empty()) {
        if (m_flusher) {
            m_flusher->gsr_flush_draws();
        } else if (!m_said_no_flusher) {
            /* No flusher is the selftests' configuration: they read the
             * records this engine built and never draw them. Anything else
             * reaching here means records with different FRAME or ZBUF are
             * landing in one batch, and the primitive vector grows past
             * kMaxBatchPrims without bound. Loud once rather than a silent
             * change of behaviour. */
            m_said_no_flusher = true;
            rt_log_warn("gsr", "a batch boundary was reached with no flusher attached "
                               "(%zu primitives, key %s); the records stay in one batch and "
                               "the batch keeps growing",
                        m_prims.size(), key_differs ? "changed" : "full");
        }
    }
    if (!m_have_key || key_differs) {
        m_key_frame = frame;
        m_key_zbuf = zbuf;
        m_key_fogcol = fogcol;
        m_key_dimx = dimx;
        m_have_key = true;

        const Frame f = decode_frame(frame);
        const Zbuf z = decode_zbuf(zbuf);
        m_push.frame_base_block = f.fbp * GS_PAGE_BLOCKS;
        m_push.frame_bw = f.fbw;
        m_push.frame_psm = f.psm;
        m_push.frame_mask = f.fbmsk;
        m_push.z_base_block = z.zbp * GS_PAGE_BLOCKS;
        m_push.z_psm = z.psm;
        m_push.z_write = z.zmsk ? 0u : 1u;
        m_push.fogcol = (uint32_t)(fogcol & 0x00FFFFFFull);
        m_push.dimx0 = (uint32_t)(dimx & 0xFFFFFFFFull);
        m_push.dimx1 = (uint32_t)((dimx >> 32) & 0xFFFFFFFFull);
    }

    m_scissor = decode_scissor(m_regs.read(m_ctxt ? GS_REG_SCISSOR_2 : GS_REG_SCISSOR_1));

    /* SCANMSK is a global register and is not part of the batch key, so it
     * travels in the record like the rest of the pixel pipeline's switches.
     * The manual reserves value 1; it is treated as no mask and named once,
     * rather than guessed at. */
    uint32_t scanmsk = (uint32_t)(m_regs.read(GS_REG_SCANMSK) & 3ull);
    if (scanmsk == 1) {
        note_once(m_said_scanmsk_reserved,
                  "SCANMSK MSK 1 is reserved in the manual; it is treated as no mask and "
                  "every raster line is drawn");
        scanmsk = 0;
    }
    if (scanmsk) ++m_stats.scanmsk;

    std::memset(m_rec, 0, sizeof(m_rec));
    uint32_t flags = kind & GSP_KIND_MASK;
    if (m_attr.iip) flags |= GSP_F_IIP;
    if (m_attr.tme) flags |= GSP_F_TME;
    if (m_attr.fst) flags |= GSP_F_FST;
    if (m_attr.fge) flags |= GSP_F_FGE;
    if (m_attr.abe) flags |= GSP_F_ABE;
    flags |= (scanmsk & GSP_F_SCANMSK_MASK) << GSP_F_SCANMSK_SHIFT;

    /* AA1 applies to the line and triangle families and not to points or
     * sprites, which is what the manual gives it for. A sprite with AA1 set
     * is drawn with hard edges and counted, because ignoring a bit the game
     * set is worth a line in the report even when the manual says it does
     * nothing. */
    if (m_attr.aa1) {
        if (kind == GSP_KIND_TRIANGLE || kind == GSP_KIND_LINE) {
            flags |= GSP_F_AA1;
            ++m_stats.aa1;
        } else {
            ++m_stats.aa1_ignored;
            note_once(m_said_aa1_ignored,
                      "AA1 is set on a point or a sprite, which the manual gives no edge "
                      "coverage for; the primitive is drawn with hard edges");
        }
    }
    m_rec[GSP_FLAGS] = flags;

    m_rec[GSP_SCISSOR_X] = m_scissor.x0 | (m_scissor.x1 << 16);
    m_rec[GSP_SCISSOR_Y] = m_scissor.y0 | (m_scissor.y1 << 16);
    m_rec[GSP_TEST] = (uint32_t)(m_regs.read(m_ctxt ? GS_REG_TEST_2 : GS_REG_TEST_1)
                                 & 0x7FFFFull);

    const uint64_t alpha = m_regs.read(m_ctxt ? GS_REG_ALPHA_2 : GS_REG_ALPHA_1);
    uint32_t a = (uint32_t)(alpha & 0xFFull)
               | ((uint32_t)((alpha >> 32) & 0xFFull) << GSP_A_FIX_SHIFT);
    if (m_regs.read(GS_REG_COLCLAMP) & 1ull) a |= GSP_A_COLCLAMP;
    if (m_regs.read(GS_REG_PABE) & 1ull) a |= GSP_A_PABE;
    if (m_regs.read(m_ctxt ? GS_REG_FBA_2 : GS_REG_FBA_1) & 1ull) a |= GSP_A_FBA;
    if (m_regs.read(GS_REG_DTHE) & 1ull) a |= GSP_A_DTHE;
    m_rec[GSP_ALPHA] = a;

    if (!m_attr.tme) return;

    ++m_stats.textured;
    PageSet reads;
    capture_texture(&reads);
    /* The feedback rule. If this primitive reads a page an earlier primitive
     * of the open batch writes, the two cannot share a dispatch: the fine
     * pass orders primitives inside a tile but not across tiles, and a
     * texture fetch reads local memory rather than the tile's threadgroup
     * copy. Breaking the batch here is the dependency, and it costs one
     * dispatch. gs_draw.h states the whole rule. */
    if (!m_prims.empty() && m_flusher && reads.intersects(m_written_pages)) {
        ++m_stats.feedback_flushes;
        m_flusher->gsr_flush_draws();
    }
    /* After the flush, because a flush empties the batch's CLUT table. */
    m_rec[GSP_CLUT_BASE] = clut_snapshot();
}

/* ---- the texture words ------------------------------------------------------ */

void DrawEngine::capture_texture(PageSet* reads) {
    const uint64_t tex0 = m_regs.read(m_ctxt ? GS_REG_TEX0_2 : GS_REG_TEX0_1);
    const uint64_t tex1 = m_regs.read(m_ctxt ? GS_REG_TEX1_2 : GS_REG_TEX1_1);
    const uint64_t clmp = m_regs.read(m_ctxt ? GS_REG_CLAMP_2 : GS_REG_CLAMP_1);
    const uint64_t mip1 = m_regs.read(m_ctxt ? GS_REG_MIPTBP1_2 : GS_REG_MIPTBP1_1);
    const uint64_t mip2 = m_regs.read(m_ctxt ? GS_REG_MIPTBP2_2 : GS_REG_MIPTBP2_1);
    const uint64_t texa = m_regs.read(GS_REG_TEXA);

    m_rec[GSP_TEX0_LO] = (uint32_t)(tex0 & 0xFFFFFFFFull);
    m_rec[GSP_TEX0_HI] = (uint32_t)((tex0 >> 32) & 0xFFFFFFFFull);
    m_rec[GSP_TEX1_LO] = (uint32_t)(tex1 & 0xFFFFFFFFull);
    m_rec[GSP_TEX1_HI] = (uint32_t)((tex1 >> 32) & 0xFFFFFFFFull);
    m_rec[GSP_CLAMP_LO] = (uint32_t)(clmp & 0xFFFFFFFFull);
    m_rec[GSP_CLAMP_HI] = (uint32_t)((clmp >> 32) & 0xFFFFFFFFull);
    m_rec[GSP_MIPTBP1_LO] = (uint32_t)(mip1 & 0xFFFFFFFFull);
    m_rec[GSP_MIPTBP1_HI] = (uint32_t)((mip1 >> 32) & 0xFFFFFFFFull);
    m_rec[GSP_MIPTBP2_LO] = (uint32_t)(mip2 & 0xFFFFFFFFull);
    m_rec[GSP_MIPTBP2_HI] = (uint32_t)((mip2 >> 32) & 0xFFFFFFFFull);
    m_rec[GSP_TEXA] = gs_texa_pack((uint32_t)(texa & 0xFFFFFFFFull),
                                   (uint32_t)((texa >> 32) & 0xFFFFFFFFull));

    /* The pages this primitive will read: every mip level the filter can
     * reach, plus the CLUT's source. */
    const uint32_t lo = m_rec[GSP_TEX0_LO];
    const uint32_t hi = m_rec[GSP_TEX0_HI];
    const uint32_t psm = gs_tex0_psm(lo);
    if (gs_psm_family(psm) == GS_FAM_BAD) {
        note_once(m_said_tex_psm,
                  "TEX0 names a pixel format the swizzle has no addressing for; the "
                  "primitive is drawn and its texels read as zero");
        return;
    }
    const uint32_t tw = gs_tex0_tw(lo);
    const uint32_t th = gs_tex0_th(lo, hi);
    const uint32_t mxl = gs_tex1_mxl(m_rec[GSP_TEX1_LO]);
    if (mxl) ++m_stats.mipmapped;
    if (mxl && gs_tex1_mtba(m_rec[GSP_TEX1_LO])) {
        note_once(m_said_mtba,
                  "TEX1 MTBA is set: the mip bases are computed from TBP0 by this "
                  "renderer's own packing rule, which the manual states only for a "
                  "texture whose levels follow one another in memory");
    }
    for (uint32_t level = 0; level <= mxl && level <= 6u; ++level) {
        const uint32_t base = gs_mip_tbp(lo, hi, m_rec[GSP_TEX1_LO], m_rec[GSP_MIPTBP1_LO],
                                         m_rec[GSP_MIPTBP1_HI], m_rec[GSP_MIPTBP2_LO],
                                         m_rec[GSP_MIPTBP2_HI], level);
        const uint32_t bw = gs_mip_tbw(lo, m_rec[GSP_TEX1_LO], m_rec[GSP_MIPTBP1_LO],
                                       m_rec[GSP_MIPTBP1_HI], m_rec[GSP_MIPTBP2_LO],
                                       m_rec[GSP_MIPTBP2_HI], level);
        const uint32_t w = gs_tex_level_size(tw, level);
        const uint32_t h = gs_tex_level_size(th, level);
        gs_mark_pages(psm, base, bw, 0, 0, (int32_t)w - 1, (int32_t)h - 1, reads);
    }
    if (gs_tex_index_bits(psm) != 0 && m_clut) {
        /* The CLUT's own source pages. The snapshot itself was taken when
         * TEX0 was written, and gs_native.cpp is what makes local memory
         * current at that moment; marking the pages here keeps a primitive
         * whose palette lives in a page this batch is writing out of the
         * same dispatch as that write, which is the same treatment the
         * texture pages get. */
        uint32_t f = 0, l = 0;
        m_clut->source_word_range(tex0, m_regs.read(GS_REG_TEXCLUT), &f, &l);
        gs_mark_page_words(f, l, reads);
    }
}

uint32_t DrawEngine::clut_snapshot() {
    if (!m_clut) return 0;
    const uint32_t serial = m_clut->serial();
    if (!m_cluts.empty() && serial == m_clut_serial_cached) return m_clut_base_cached;
    m_clut_base_cached = (uint32_t)m_cluts.size();
    m_clut_serial_cached = serial;
    m_cluts.insert(m_cluts.end(), m_clut->words(), m_clut->words() + GS_CLUT_WORDS);
    return m_clut_base_cached;
}

void DrawEngine::abandon_record() {
    ++m_stats.offscreen;
}

void DrawEngine::expand_for_aa1(int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) const {
    if ((m_rec[GSP_FLAGS] & GSP_F_AA1) == 0) return;
    /* One pixel on every side. A pixel whose centre is outside the primitive
     * can still be partly covered by it, and gs_prim.h's coverage rule
     * reaches at most half a pixel past an edge in the direction of the
     * edge's normal, which is inside one whole pixel of the box either way.
     * The scissor still clips: finish_record applies it after this. */
    *x0 -= 1;
    *y0 -= 1;
    *x1 += 1;
    *y1 += 1;
}

bool DrawEngine::finish_record(int32_t minx, int32_t miny, int32_t maxx, int32_t maxy) {
    /* The drawing area is 2048 by 2048 whatever the frame buffer is, and the
     * scissor is inclusive on both ends. */
    int32_t x0 = imax(minx, (int32_t)m_scissor.x0);
    int32_t y0 = imax(miny, (int32_t)m_scissor.y0);
    int32_t x1 = imin(maxx, (int32_t)m_scissor.x1);
    int32_t y1 = imin(maxy, (int32_t)m_scissor.y1);
    x0 = imax(x0, 0);
    y0 = imax(y0, 0);
    x1 = imin(x1, 2047);
    y1 = imin(y1, 2047);
    if (x0 > x1 || y0 > y1) {
        abandon_record();
        return false;
    }

    m_rec[GSP_REFX] = (uint32_t)x0;
    m_rec[GSP_REFY] = (uint32_t)y0;

    /* The pages this primitive writes, for the feedback rule in
     * begin_record. The clipped box rather than the whole buffer, so a small
     * primitive marks the one or two pages it actually lands in. */
    gs_mark_pages(m_push.frame_psm, m_push.frame_base_block, m_push.frame_bw,
                  x0, y0, x1, y1, &m_written_pages);
    if (m_push.z_write) {
        gs_mark_pages(m_push.z_psm, m_push.z_base_block, m_push.frame_bw,
                      x0, y0, x1, y1, &m_written_pages);
    }

    const uint32_t bx0 = (uint32_t)x0 / GSP_BIN_PIXELS;
    const uint32_t by0 = (uint32_t)y0 / GSP_BIN_PIXELS;
    const uint32_t bx1 = (uint32_t)x1 / GSP_BIN_PIXELS;
    const uint32_t by1 = (uint32_t)y1 / GSP_BIN_PIXELS;

    m_prims.insert(m_prims.end(), m_rec, m_rec + GSP_STRIDE);
    m_prim_bins.push_back(bx0 | (by0 << 8) | (bx1 << 16) | (by1 << 24));

    /* The batch's own pixel rectangle. The dispatch grid is derived from it
     * at whatever tile size the render scale asks for, and so is the
     * rectangle the resolve pass covers, so it is kept in pixels rather than
     * in tiles of one fixed size. */
    if (m_prims.size() == GSP_STRIDE) {
        m_px0 = (uint32_t)x0;
        m_py0 = (uint32_t)y0;
        m_px1 = (uint32_t)x1;
        m_py1 = (uint32_t)y1;
        m_max_py = (uint32_t)y1;
    } else {
        if ((uint32_t)x0 < m_px0) m_px0 = (uint32_t)x0;
        if ((uint32_t)y0 < m_py0) m_py0 = (uint32_t)y0;
        if ((uint32_t)x1 > m_px1) m_px1 = (uint32_t)x1;
        if ((uint32_t)y1 > m_py1) m_py1 = (uint32_t)y1;
        if ((uint32_t)y1 > m_max_py) m_max_py = (uint32_t)y1;
    }
    ++m_stats.prims;
    return true;
}

/* ---- the four emitters ------------------------------------------------------ */

void DrawEngine::emit_point(const DrawVertex& a) {
    begin_record(GSP_KIND_POINT);
    m_rec[GSP_X0] = (uint32_t)a.x;
    m_rec[GSP_Y0] = (uint32_t)a.y;
    m_rec[GSP_RGBA0] = a.rgba;
    m_rec[GSP_RGBA1] = a.rgba;
    m_rec[GSP_RGBA2] = a.rgba;
    m_rec[GSP_RGBA_FLAT] = a.rgba;
    m_rec[GSP_FOG_FLAT] = a.fog;
    m_rec[GSP_Z_I] = a.z;
    m_rec[GSP_UV0] = a.u | (a.v << 16);
    m_rec[GSP_S0] = a.s;
    m_rec[GSP_T0] = a.t;
    m_rec[GSP_Q0] = a.q;
    const int32_t px = floor16(a.x);
    const int32_t py = floor16(a.y);
    finish_record(px, py, px, py);
}

void DrawEngine::emit_sprite(const DrawVertex& a, const DrawVertex& b) {
    begin_record(GSP_KIND_SPRITE);
    /* A sprite is an axis-aligned rectangle between the two vertices, in
     * either order, so the corners are sorted here and the coverage rule in
     * gs_prim.h can assume x0 < x1 and y0 < y1. The manual gives the second
     * vertex as the one whose colour and Z the whole rectangle takes, which
     * is why the sort never moves them. */
    const bool swap_x = b.x < a.x;
    const bool swap_y = b.y < a.y;
    const int32_t x0 = swap_x ? b.x : a.x;
    const int32_t x1 = swap_x ? a.x : b.x;
    const int32_t y0 = swap_y ? b.y : a.y;
    const int32_t y1 = swap_y ? a.y : b.y;
    /* The texture coordinates follow their own corner through that sort. U
     * and S vary with x and V and T with y, so each axis is swapped
     * independently, and a sprite given right to left keeps the mapping the
     * game asked for instead of mirroring it. Q is the second vertex's for
     * the whole rectangle, the same vertex the colour and Z come from; a
     * sprite is axis aligned and its Q is constant, so there is nothing to
     * interpolate. */
    const DrawVertex& ux0 = swap_x ? b : a;
    const DrawVertex& ux1 = swap_x ? a : b;
    const DrawVertex& vy0 = swap_y ? b : a;
    const DrawVertex& vy1 = swap_y ? a : b;
    m_rec[GSP_UV0] = ux0.u | (vy0.v << 16);
    m_rec[GSP_UV1] = ux1.u | (vy1.v << 16);
    m_rec[GSP_S0] = ux0.s;
    m_rec[GSP_T0] = vy0.t;
    m_rec[GSP_S1] = ux1.s;
    m_rec[GSP_T1] = vy1.t;
    m_rec[GSP_Q0] = b.q;
    m_rec[GSP_Q1] = b.q;
    m_rec[GSP_X0] = (uint32_t)x0;
    m_rec[GSP_Y0] = (uint32_t)y0;
    m_rec[GSP_X1] = (uint32_t)x1;
    m_rec[GSP_Y1] = (uint32_t)y1;
    m_rec[GSP_RGBA0] = b.rgba;
    m_rec[GSP_RGBA1] = b.rgba;
    m_rec[GSP_RGBA2] = b.rgba;
    m_rec[GSP_RGBA_FLAT] = b.rgba;
    m_rec[GSP_FOG_FLAT] = b.fog;
    /* No GSP_FOG012: a sprite has no per-vertex fog. raster.comp reads that
     * slot only for a triangle or a line under IIP, and reads GSP_FOG_FLAT
     * for everything else, so writing it here was a value nothing loaded. */
    m_rec[GSP_Z_I] = b.z;
    finish_record(floor16(x0), floor16(y0), floor16(x1), floor16(y1));
}

void DrawEngine::emit_line(const DrawVertex& a, const DrawVertex& b) {
    begin_record(GSP_KIND_LINE);
    const int32_t dx = b.x - a.x;
    const int32_t dy = b.y - a.y;
    const int32_t adx = dx < 0 ? -dx : dx;
    const int32_t ady = dy < 0 ? -dy : dy;
    const bool major_x = adx >= ady;
    if (major_x) m_rec[GSP_FLAGS] |= GSP_F_MAJOR_X;

    m_rec[GSP_X0] = (uint32_t)a.x;
    m_rec[GSP_Y0] = (uint32_t)a.y;
    m_rec[GSP_X1] = (uint32_t)b.x;
    m_rec[GSP_Y1] = (uint32_t)b.y;
    m_rec[GSP_RGBA0] = a.rgba;
    m_rec[GSP_RGBA1] = b.rgba;
    m_rec[GSP_RGBA2] = b.rgba;
    m_rec[GSP_RGBA_FLAT] = b.rgba;
    m_rec[GSP_FOG012] = a.fog | (b.fog << 8) | (b.fog << 16);
    m_rec[GSP_FOG_FLAT] = b.fog;
    m_rec[GSP_UV0] = a.u | (a.v << 16);
    m_rec[GSP_UV1] = b.u | (b.v << 16);
    m_rec[GSP_S0] = a.s;
    m_rec[GSP_T0] = a.t;
    m_rec[GSP_Q0] = a.q;
    m_rec[GSP_S1] = b.s;
    m_rec[GSP_T1] = b.t;
    m_rec[GSP_Q1] = b.q;

    const int32_t maj0 = major_x ? a.x : a.y;
    const int32_t maj1 = major_x ? b.x : b.y;
    const int32_t min0 = major_x ? a.y : a.x;
    const int32_t min1 = major_x ? b.y : b.x;
    const int32_t dmaj = maj1 - maj0;

    /* First and last major pixel under the same sample rule a sprite uses:
     * major pixel p is on the line when p*16+8 lies in [lo, hi). */
    const int32_t lo = imin(maj0, maj1);
    const int32_t hi = imax(maj0, maj1);
    const int32_t p_first = floor16(lo + 7);   /* ceil((lo - 8) / 16) */
    const int32_t p_last = floor16(hi - 9);    /* ceil((hi - 8) / 16) - 1 */
    if (dmaj == 0 || p_first > p_last) {
        /* Both endpoints inside one pixel of the major axis, so the span
         * rule selects no pixel at all. The DDA emits a pixel before it
         * steps, so the primitive is the one pixel the first vertex falls
         * in, at parameter zero along the line: vertex a's colour under IIP,
         * vertex a's Z, and no gradient. Inferred, and gs_prim.h's
         * gs_covers_line_dot says what would settle it. */
        ++m_stats.short_lines;
        note_once(m_said_short_line,
                  "a line's two endpoints fell inside one pixel of its major axis; it is "
                  "drawn as the one pixel the first vertex falls in, which is what a DDA "
                  "that emits before it steps does and is not measured");
        m_rec[GSP_FLAGS] |= GSP_F_LINE_DOT;
        store_fixed((double)a.z, &m_rec[GSP_Z_I], &m_rec[GSP_Z_F]);
        int32_t dx0 = floor16(a.x);
        int32_t dy0 = floor16(a.y);
        int32_t dx1 = dx0;
        int32_t dy1 = dy0;
        expand_for_aa1(&dx0, &dy0, &dx1, &dy1);
        finish_record(dx0, dy0, dx1, dy1);
        return;
    }
    note_once(m_said_line_model,
              "the line DDA is this renderer's own model (major axis sampled like a "
              "sprite, minor coordinate interpolated to the pixel it lands in) and has "
              "not been measured against the hardware");

    /* refx/refy are set by finish_record from the clipped box, so the
     * reference the shader steps from has to be the same value. Compute the
     * clipped box first, then the DDA from its origin. */
    int32_t bx0, by0, bx1, by1;
    if (major_x) {
        bx0 = p_first;
        bx1 = p_last;
        by0 = floor16(imin(min0, min1));
        by1 = floor16(imax(min0, min1));
    } else {
        by0 = p_first;
        by1 = p_last;
        bx0 = floor16(imin(min0, min1));
        bx1 = floor16(imax(min0, min1));
    }

    expand_for_aa1(&bx0, &by0, &bx1, &by1);

    /* The clipped origin, computed the same way finish_record will. */
    int32_t rx = imax(imax(bx0, (int32_t)m_scissor.x0), 0);
    int32_t ry = imax(imax(by0, (int32_t)m_scissor.y0), 0);
    const int32_t refp = major_x ? rx : ry;

    const double slope = (double)(min1 - min0) / (double)dmaj; /* minor per major, 1/16 both */
    const double minor_ref =
        ((double)min0 + ((double)(refp * 16 + 8) - (double)maj0) * slope) / 16.0;
    m_rec[GSP_X2] = (uint32_t)store_16_16(minor_ref);
    m_rec[GSP_LINE_SLOPE] = (uint32_t)store_16_16(slope);

    /* Z, and the colour weight, run along the major axis only. */
    const double dz = (double)b.z - (double)a.z;
    const double dzdmaj = 16.0 * dz / (double)dmaj; /* per major pixel */
    const double zref = (double)a.z
        + dzdmaj * (((double)(refp * 16 + 8) - (double)maj0) / 16.0);
    store_fixed(zref, &m_rec[GSP_Z_I], &m_rec[GSP_Z_F]);
    if (major_x) {
        store_fixed(dzdmaj, &m_rec[GSP_ZDX_I], &m_rec[GSP_ZDX_F]);
    } else {
        store_fixed(dzdmaj, &m_rec[GSP_ZDY_I], &m_rec[GSP_ZDY_F]);
    }

    finish_record(bx0, by0, bx1, by1);
}

void DrawEngine::emit_triangle(const DrawVertex& v0, const DrawVertex& v1,
                               const DrawVertex& v2) {
    begin_record(GSP_KIND_TRIANGLE);

    /* The flat colour is the last vertex's, before any reordering. */
    m_rec[GSP_RGBA_FLAT] = v2.rgba;
    m_rec[GSP_FOG_FLAT] = v2.fog;

    /* Wind positively, so gs_prim.h's edge functions have the interior on
     * their positive side and the top-left rule is stated once. Swapping two
     * vertices reverses the sign and changes nothing else about the shape. */
    const DrawVertex* a = &v0;
    const DrawVertex* b = &v1;
    const DrawVertex* c = &v2;
    const gs_i64 area = gs_cross64(b->x - a->x, c->y - a->y, b->y - a->y, c->x - a->x);
    const int sign = gs_i64_sign(area);
    if (sign == 0) {
        /* A zero-area triangle covers nothing under the top-left rule: every
         * sample point lies on all three edges at once, and the tie rules of
         * an edge and its reverse disagree. Dropped here so the plane solve
         * below never divides by zero. */
        ++m_stats.degenerate;
        return;
    }
    if (sign < 0) {
        const DrawVertex* t = b;
        b = c;
        c = t;
    }

    m_rec[GSP_X0] = (uint32_t)a->x;
    m_rec[GSP_Y0] = (uint32_t)a->y;
    m_rec[GSP_X1] = (uint32_t)b->x;
    m_rec[GSP_Y1] = (uint32_t)b->y;
    m_rec[GSP_X2] = (uint32_t)c->x;
    m_rec[GSP_Y2] = (uint32_t)c->y;
    m_rec[GSP_RGBA0] = a->rgba;
    m_rec[GSP_RGBA1] = b->rgba;
    m_rec[GSP_RGBA2] = c->rgba;
    m_rec[GSP_FOG012] = a->fog | (b->fog << 8) | (c->fog << 16);
    /* The texture coordinates follow the winding swap with their vertex, so
     * GSP_UV0 and GSP_S0 belong to the vertex at GSP_X0. */
    m_rec[GSP_UV0] = a->u | (a->v << 16);
    m_rec[GSP_UV1] = b->u | (b->v << 16);
    m_rec[GSP_UV2] = c->u | (c->v << 16);
    m_rec[GSP_S0] = a->s;
    m_rec[GSP_T0] = a->t;
    m_rec[GSP_Q0] = a->q;
    m_rec[GSP_S1] = b->s;
    m_rec[GSP_T1] = b->t;
    m_rec[GSP_Q1] = b->q;
    m_rec[GSP_S2] = c->s;
    m_rec[GSP_T2] = c->t;
    m_rec[GSP_Q2] = c->q;

    int32_t bx0 = floor16(imin(a->x, imin(b->x, c->x)));
    int32_t by0 = floor16(imin(a->y, imin(b->y, c->y)));
    int32_t bx1 = floor16(imax(a->x, imax(b->x, c->x)));
    int32_t by1 = floor16(imax(a->y, imax(b->y, c->y)));
    expand_for_aa1(&bx0, &by0, &bx1, &by1);

    /* The reference pixel the Z DDA steps from is the clipped box origin,
     * the same value finish_record will store, so the deltas the shader
     * multiplies by are never negative. */
    const int32_t rx = imax(imax(bx0, (int32_t)m_scissor.x0), 0);
    const int32_t ry = imax(imax(by0, (int32_t)m_scissor.y0), 0);

    /* The plane solve, in double. Z is 32 bits wide and double carries 53
     * bits of mantissa, so the solve itself is exact to well under one Z
     * unit; store_fixed then quantises the step at 2^-32 per pixel, which
     * over the 2048 pixels of the widest drawing area is under 2^-21 of a Z
     * unit. Colour does not go through here: it is 8 bits wide and the fine
     * pass interpolates it from the same exact edge values it already
     * computes for coverage. */
    const double e1x = (double)(b->x - a->x);
    const double e1y = (double)(b->y - a->y);
    const double e2x = (double)(c->x - a->x);
    const double e2y = (double)(c->y - a->y);
    const double det = e1x * e2y - e1y * e2x;
    const double dz1 = (double)b->z - (double)a->z;
    const double dz2 = (double)c->z - (double)a->z;
    const double dzdx = 16.0 * (dz1 * e2y - dz2 * e1y) / det;
    const double dzdy = 16.0 * (dz2 * e1x - dz1 * e2x) / det;
    const double zref = (double)a->z
        + dzdx * (((double)(rx * 16 + 8) - (double)a->x) / 16.0)
        + dzdy * (((double)(ry * 16 + 8) - (double)a->y) / 16.0);
    store_fixed(zref, &m_rec[GSP_Z_I], &m_rec[GSP_Z_F]);
    store_fixed(dzdx, &m_rec[GSP_ZDX_I], &m_rec[GSP_ZDX_F]);
    store_fixed(dzdy, &m_rec[GSP_ZDY_I], &m_rec[GSP_ZDY_F]);

    finish_record(bx0, by0, bx1, by1);
}

/* ---- binning and the batch --------------------------------------------------- */

void DrawEngine::build_bins() {
    ++m_stats.batches;
    m_bin_count.assign(GSP_BIN_COUNT, 0u);
    for (size_t i = 0; i < m_prim_bins.size(); ++i) {
        const uint32_t r = m_prim_bins[i];
        const uint32_t x0 = r & 0xFFu, y0 = (r >> 8) & 0xFFu;
        const uint32_t x1 = (r >> 16) & 0xFFu, y1 = (r >> 24) & 0xFFu;
        for (uint32_t by = y0; by <= y1; ++by) {
            for (uint32_t bx = x0; bx <= x1; ++bx) ++m_bin_count[by * GSP_BINS_X + bx];
        }
    }
    m_bin_range.assign(GSP_BIN_COUNT * 2u, 0u);
    uint32_t total = 0;
    for (uint32_t b = 0; b < GSP_BIN_COUNT; ++b) {
        m_bin_range[b * 2u] = total;
        m_bin_range[b * 2u + 1u] = m_bin_count[b];
        total += m_bin_count[b];
        /* Reused as the fill cursor below. */
        m_bin_count[b] = m_bin_range[b * 2u];
    }
    m_bin_index.assign(total ? total : 1u, 0u);
    for (size_t i = 0; i < m_prim_bins.size(); ++i) {
        const uint32_t r = m_prim_bins[i];
        const uint32_t x0 = r & 0xFFu, y0 = (r >> 8) & 0xFFu;
        const uint32_t x1 = (r >> 16) & 0xFFu, y1 = (r >> 24) & 0xFFu;
        for (uint32_t by = y0; by <= y1; ++by) {
            for (uint32_t bx = x0; bx <= x1; ++bx) {
                m_bin_index[m_bin_count[by * GSP_BINS_X + bx]++] = (uint32_t)i;
            }
        }
    }
}

void DrawEngine::tile_grid(uint32_t tile_w, uint32_t tile_h, uint32_t* tx, uint32_t* ty,
                           uint32_t* tw, uint32_t* th) const {
    if (m_prims.empty() || tile_w == 0 || tile_h == 0) {
        *tx = 0;
        *ty = 0;
        *tw = 0;
        *th = 0;
        return;
    }
    const uint32_t x0 = m_px0 / tile_w;
    const uint32_t y0 = m_py0 / tile_h;
    *tx = x0;
    *ty = y0;
    *tw = m_px1 / tile_w - x0 + 1u;
    *th = m_py1 / tile_h - y0 + 1u;
}

bool DrawEngine::pixel_grid(uint32_t* x0, uint32_t* y0, uint32_t* x1, uint32_t* y1) const {
    if (m_prims.empty()) return false;
    *x0 = m_px0;
    *y0 = m_py0;
    *x1 = m_px1;
    *y1 = m_py1;
    return true;
}

void DrawEngine::write_range(uint32_t* first_word, uint32_t* last_word) const {
    if (m_prims.empty()) {
        *first_word = 0;
        *last_word = 0;
        return;
    }
    uint32_t f0, l0, f1, l1;
    gs_buffer_word_range(m_push.frame_psm, m_push.frame_base_block, m_push.frame_bw,
                         m_max_py, &f0, &l0);
    if (m_push.z_write) {
        /* The Z buffer has no width register of its own: it is the same
         * width as the frame buffer, which is what the manual states. */
        gs_buffer_word_range(m_push.z_psm, m_push.z_base_block, m_push.frame_bw,
                             m_max_py, &f1, &l1);
    } else {
        f1 = f0;
        l1 = l0;
    }
    *first_word = f0 < f1 ? f0 : f1;
    *last_word = l0 > l1 ? l0 : l1;
}

void DrawEngine::clear() {
    m_prims.clear();
    m_prim_bins.clear();
    m_bin_index.clear();
    /* The page sets and the CLUT table belong to the batch that just went
     * out, so the next batch starts with no dependency on it: the dispatch
     * boundary is what orders the two. */
    m_written_pages.clear();
    m_cluts.clear();
    m_clut_serial_cached = 0;
    m_clut_base_cached = 0;
    m_max_py = 0;
    m_px0 = m_py0 = m_px1 = m_py1 = 0;
}

} // namespace gsr
