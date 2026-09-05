/* gs/render/gs_draw.h: primitive assembly, the CPU half of milestone (b).
 *
 * Ours (MIT). Turns the register stream the GIF decoder produces into the
 * packed primitive records gs_prim.h defines, bins them into the coarse
 * 64x64 grid, and hands the whole batch to the backend to dispatch.
 *
 * Why this is its own translation unit rather than sitting inside
 * gs_native.cpp, which is where the milestone brief put it: assembly is the
 * part with the retention rules and the fixed-point setup, which is exactly
 * the part that has to be exercised without a GPU. gs_native.cpp links the
 * Vulkan RHI; this file links nothing but the register file, so
 * gs_raster_selftest.cpp can drive it directly. gs_native.cpp owns the
 * wiring, the dispatch and the memory synchronisation.
 *
 * ---- what a batch is --------------------------------------------------------
 *
 * The fine pass loads a tile of the frame buffer and of the Z buffer into
 * threadgroup memory, walks that tile's primitives in submission order, and
 * stores the tile back. That only works while every primitive in the walk
 * addresses the same two buffers, so a batch is a run of primitives that
 * share FRAME and ZBUF (of whichever context each one selects), FOGCOL and
 * DIMX. Everything else the pixel pipeline reads is carried per primitive in
 * the record: the scissor, TEST, ALPHA, COLCLAMP, PABE, FBA, DTHE and the
 * PRIM attribute bits. When a primitive arrives whose batch key differs, the
 * open batch is flushed first, so a context switch costs one dispatch and
 * changes nothing about the result.
 *
 * The other flush points, which the backend owns: a vsync, a transfer, and
 * any host read of local memory.
 *
 * ---- the page tracker, and why a batch also breaks on feedback --------------
 *
 * A texture is read out of local memory in place: there is no upload and no
 * cache, so a primitive that samples a page an earlier primitive drew into
 * reads whatever is in that page at the moment its workgroup runs.
 *
 * Inside one 16x16 tile that is already ordered: the fine pass walks a
 * tile's primitives serially and keeps that tile's colour and depth in
 * threadgroup memory, so a later primitive sees the earlier one's pixels.
 * But it sees them in that threadgroup copy, not in local memory, and a
 * texture fetch reads local memory. And across tiles there is no ordering at
 * all: two workgroups run at once and neither waits for the other, so a
 * primitive in one tile can read a page another tile has not written yet.
 * Both of those make the same rule necessary.
 *
 * So the engine tracks two sets of 8 KiB pages per batch: the pages the
 * batch's primitives write, from FRAME and ZBUF over each primitive's
 * clipped bounding box, and the pages a primitive is about to read, from
 * TEX0's base and size, its mip levels, and the CLUT source. When a read
 * meets a write already in the batch, the batch is flushed first, which is
 * exactly the dependency the dispatch boundary expresses. That is the rule
 * the bloom pass needs, where the game draws the frame buffer back onto
 * itself.
 *
 * ---- what travels per primitive, and what does not --------------------------
 *
 * AA1 and SCANMSK both landed in milestone (d) and both travel in the
 * record's flags word: AA1 because it is a PRIM attribute, SCANMSK because
 * it is a global register that is not part of the batch key and can change
 * between two primitives of one batch. The rules they select are written in
 * gs_prim.h and used by the fine pass. What is still named once rather than
 * implemented is SCANMSK 1, which the manual reserves.
 */
#ifndef ICORECOMP_GS_DRAW_H
#define ICORECOMP_GS_DRAW_H

#include "gif_decode.h"
#include "gs_clut.h"
#include "gs_prim.h"
#include "gs_regs.h"
#include "gs_texture.h"

#include <cstddef>   /* offsetof, for the push block layout assertions */
#include <cstdint>
#include <vector>

namespace gsr {

/* The fine pass's push constants. Plain scalars only, so std140 and std430
 * lay the block out identically and the C++ struct can be memcpy'd into the
 * command list; 16 words, 64 bytes, inside the RHI's 128-byte budget. The
 * field order has to stay in step with shaders/raster.comp.
 *
 * The size was stated as 48 bytes here while the struct was 64: the comment
 * had not followed the four render-scale words in. Nothing checked either
 * number, so the only thing standing between this block and the shader's was
 * a comment. The static_assert below is the check; there is one for every
 * push block (gs_crtc.h, gs_shadow.h, gs_native.cpp). A block that grows past
 * the budget is a fatal at the push otherwise, one field at a time, with no
 * indication of which struct grew. */
struct RasterPush {
    uint32_t frame_base_block; /* FRAME FBP converted to 256-byte blocks */
    uint32_t frame_bw;         /* FRAME FBW, in 64-pixel units */
    uint32_t frame_psm;
    uint32_t frame_mask;       /* FBMSK: set bits are not written */
    uint32_t z_base_block;     /* ZBUF ZBP converted to 256-byte blocks */
    uint32_t z_psm;            /* the full PSM code, 0x30 plus ZBUF's field */
    uint32_t z_write;          /* 1 when ZMSK is clear */
    uint32_t tile_x0;          /* dispatch origin, in tiles of tile_w by tile_h */
    uint32_t tile_y0;
    uint32_t fogcol;           /* 0x00BBGGRR */
    uint32_t dimx0;            /* DIMX, low and high halves */
    uint32_t dimx1;
    /* Render scale. The assembler leaves these zero: they are a host setting
     * and not a property of the geometry, so gs_native.cpp fills them in on
     * the copy it pushes. gs_shadow.h's gs_scale_tile picks the tile. */
    uint32_t samples;          /* 1, 4, 8 or 16 */
    uint32_t tile_w;           /* tile_w * tile_h * samples == 256 */
    uint32_t tile_h;
    uint32_t shadow;           /* 1 when colour and depth live in the shadow */
};

/* One word per field, in the order shaders/raster.comp declares them, and
 * inside the RHI's push constant budget (rhi.h kPushConstantBytes, 128). Not
 * included here to keep this header free of the RHI; gs_native.cpp asserts
 * the budget where both headers are in scope. */
static_assert(sizeof(RasterPush) == 16 * sizeof(uint32_t),
              "RasterPush must stay 16 words, in step with shaders/raster.comp");
static_assert(offsetof(RasterPush, samples) == 12 * sizeof(uint32_t),
              "the render scale words come after dimx1, as raster.comp declares them");
static_assert(offsetof(RasterPush, shadow) == 15 * sizeof(uint32_t),
              "shadow is the last word of the fine pass's push block");

/* A conservative range of local memory words a buffer occupies, as
 * [first, last): whole pages, from `base_block` (256-byte blocks, which is
 * what BITBLTBUF SBP/DBP carry and what FRAME FBP times 32 becomes), across
 * `bw` 64-pixel units, down to line `max_y`. A buffer that runs off the end
 * of the 4 MiB wraps in the hardware, and this answers the whole store for
 * that case rather than a truncated range. Used by the renderer to decide
 * whether a host access to local memory has to read the device buffer back
 * first. */
void gs_buffer_word_range(uint32_t psm, uint32_t base_block, uint32_t bw, uint32_t max_y,
                          uint32_t* first_word, uint32_t* last_word);

/* One vertex as the queue holds it. */
struct DrawVertex {
    int32_t x = 0, y = 0;   /* window coordinates, 1/16 pixel, XYOFFSET already off */
    uint32_t z = 0;
    uint32_t rgba = 0;      /* 0xAABBGGRR */
    uint32_t fog = 0;       /* 0..255 */
    uint32_t s = 0, t = 0;  /* ST as raw float bits */
    uint32_t q = 0;
    uint32_t u = 0, v = 0;  /* UV, 1/16 texel */
};

/* The set of 8 KiB local memory pages a buffer or a primitive touches. Local
 * memory is 4 MiB, which is 512 pages, so one set is 64 bytes and a test is
 * sixteen ANDs. Page granularity rather than block granularity because a
 * page is the unit the swizzle's own geometry is expressed in, and because a
 * conservative overlap costs one extra dispatch while a missed one costs a
 * wrong picture. */
#define GSP_PAGE_COUNT 512u
#define GSP_PAGE_WORDS 16u

struct PageSet {
    uint32_t bits[GSP_PAGE_WORDS] = {};

    void clear() {
        for (uint32_t i = 0; i < GSP_PAGE_WORDS; ++i) bits[i] = 0;
    }
    void add(uint32_t page) {
        page &= (GSP_PAGE_COUNT - 1u);
        bits[page >> 5] |= 1u << (page & 31u);
    }
    bool intersects(const PageSet& o) const {
        for (uint32_t i = 0; i < GSP_PAGE_WORDS; ++i) {
            if (bits[i] & o.bits[i]) return true;
        }
        return false;
    }
};

/* Marks every page a rectangle of a buffer occupies. `base_block` is in
 * 256-byte blocks, which is what TEX0 TBP0 carries and what FRAME FBP times
 * 32 becomes; a base that is not page aligned has its buffer straddle one
 * more page than its size suggests, so both are marked. The rectangle is
 * inclusive on both ends and is clamped to nothing when it is empty. */
void gs_mark_pages(uint32_t psm, uint32_t base_block, uint32_t bw, int32_t x0,
                   int32_t y0, int32_t x1, int32_t y1, PageSet* out);

/* Marks the pages a word range covers, for the CLUT, whose source is
 * described as a range rather than as a rectangle. */
void gs_mark_page_words(uint32_t first_word, uint32_t last_word, PageSet* out);

/* The backend's side of a flush. Called from inside vertex() when the batch
 * key changes; the implementation reads the batch out of the engine,
 * dispatches it, and calls clear(). */
class DrawFlusher {
public:
    virtual ~DrawFlusher() = default;
    virtual void gsr_flush_draws() = 0;
};

class DrawEngine {
public:
    explicit DrawEngine(const RegisterFile& regs) : m_regs(regs) {}

    void set_flusher(DrawFlusher* f) { m_flusher = f; }

    /* The CLUT the texture unit reads. Not owned; gs_native.cpp keeps it
     * beside local memory because a load is a CPU event. A null cache means
     * every record's CLUT snapshot is zeroed, which is what the selftests
     * that draw no palettised texture want. */
    void set_clut(const ClutCache* c) { m_clut = c; }

    /* PRIM was written. The manual states that a PRIM write starts a new
     * primitive, so the vertex queue is emptied. */
    void prim_written();

    /* One of XYZ2, XYZF2, XYZ3 or XYZF3. The first two kick, the second two
     * queue the vertex without drawing, which is also what the PACKED ADC
     * bit produces (gif_decode.h redirects the address). */
    void vertex(uint32_t addr, uint64_t value);

    /* ---- what a flush reads ---- */

    bool empty() const { return m_prims.empty(); }
    uint32_t prim_count() const { return (uint32_t)(m_prims.size() / GSP_STRIDE); }
    const std::vector<uint32_t>& prims() const { return m_prims; }
    const RasterPush& push() const { return m_push; }

    /* The batch's CLUT table: the distinct 1 KB snapshots the batch's
     * primitives were assembled under, concatenated. GSP_CLUT_BASE is a word
     * index into this. Empty when no primitive in the batch was textured. */
    const std::vector<uint32_t>& cluts() const { return m_cluts; }

    /* The pages this batch has written so far, for the caller that wants to
     * make the same feedback decision about something that is not a
     * primitive (gs_native.cpp does it for a CLUT load). */
    const PageSet& written_pages() const { return m_written_pages; }

    /* Fills bin_index() and bin_range() from the primitives collected so
     * far. Called once per flush, before either is read. */
    void build_bins();
    const std::vector<uint32_t>& bin_index() const { return m_bin_index; }
    const std::vector<uint32_t>& bin_range() const { return m_bin_range; }

    /* The tile rectangle the dispatch has to cover, as an origin and a
     * count, in tiles of `tile_w` by `tile_h` pixels. Zero width means the
     * batch drew nothing anywhere. */
    void tile_grid(uint32_t tile_w, uint32_t tile_h, uint32_t* tx, uint32_t* ty,
                   uint32_t* tw, uint32_t* th) const;

    /* The clipped pixel rectangle the batch drew into, inclusive on both
     * ends. False when the batch drew nothing. The resolve pass at render
     * scale above 1 runs over exactly this. */
    bool pixel_grid(uint32_t* x0, uint32_t* y0, uint32_t* x1, uint32_t* y1) const;

    /* The lowest raster line this batch reached, after the scissor. The
     * conservative buffer ranges are computed from it rather than from the
     * whole 2048-line drawing area. */
    uint32_t max_row() const { return m_max_py; }

    /* The words of local memory this batch may write, as [first, last).
     * gs_native.cpp uses it to decide whether a host access has to read the
     * device buffer back first. Conservative: whole pages, both buffers. */
    void write_range(uint32_t* first_word, uint32_t* last_word) const;

    void clear();

    /* ---- counters, for the end of run report ---- */
    struct Stats {
        uint64_t kicks = 0;         /* drawing kicks seen */
        uint64_t prims = 0;         /* records emitted */
        uint64_t degenerate = 0;    /* zero-area triangles, dropped */
        uint64_t offscreen = 0;     /* nothing left after the scissor */
        uint64_t short_lines = 0;   /* lines drawn as one pixel: see GSP_F_LINE_DOT */
        uint64_t scanmsk = 0;       /* primitives assembled under a SCANMSK */
        uint64_t textured = 0;      /* TME set */
        uint64_t feedback_flushes = 0; /* batches broken by the page tracker */
        uint64_t mipmapped = 0;     /* MXL > 0 */
        uint64_t aa1 = 0;           /* AA1 set on a line or a triangle */
        uint64_t aa1_ignored = 0;   /* AA1 set on a point or a sprite */
        uint64_t reserved_prim = 0; /* PRIM 7 */
        uint64_t batches = 0;
    };
    const Stats& stats() const { return m_stats; }

private:
    void push_vertex(const DrawVertex& v);
    void kick();
    void emit_triangle(const DrawVertex& a, const DrawVertex& b, const DrawVertex& c);
    void emit_sprite(const DrawVertex& a, const DrawVertex& b);
    void emit_line(const DrawVertex& a, const DrawVertex& b);
    void emit_point(const DrawVertex& a);

    /* Shared record head: the batch key check and the flush it may force,
     * then kind, the PRIM attribute flags, the scissor, TEST and ALPHA. A
     * flush does not change any register, so the caller's state stays
     * valid across it. */
    void begin_record(uint32_t kind);
    /* Clips the record's bounding box, fills refx/refy and the bin rectangle,
     * and commits or drops the record. */
    bool finish_record(int32_t minx, int32_t miny, int32_t maxx, int32_t maxy);
    /* AA1 draws pixels the sample rule rejects, up to one pixel outside the
     * primitive's own box, so the box grows before anything is derived from
     * it. Called by the two emitters AA1 applies to, before they compute the
     * reference pixel, so the reference stays the one finish_record stores. */
    void expand_for_aa1(int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) const;
    void abandon_record();

    void note_once(bool& flag, const char* what);

    /* Fills the record's texture words from the registers of the context the
     * primitive selected, and returns the pages it will read. */
    void capture_texture(PageSet* reads);
    /* Appends the CLUT standing now to the batch's table, or reuses the
     * snapshot already appended for it, and returns its word index. */
    uint32_t clut_snapshot();

    const RegisterFile& m_regs;
    DrawFlusher* m_flusher = nullptr;

    DrawVertex m_queue[3];
    uint32_t m_queued = 0;

    /* The batch key, and whether one is open. */
    bool m_have_key = false;
    bool m_said_no_flusher = false;
    uint64_t m_key_frame = 0, m_key_zbuf = 0, m_key_fogcol = 0, m_key_dimx = 0;

    /* The primitive being built, appended to m_prims by finish_record. */
    uint32_t m_rec[GSP_STRIDE];
    /* The attribute state the current primitive was assembled with. */
    Prim m_attr{};
    uint32_t m_ctxt = 0;
    Scissor m_scissor{};

    /* The batch's page sets and its CLUT table. */
    PageSet m_written_pages;
    const ClutCache* m_clut = nullptr;
    std::vector<uint32_t> m_cluts;
    uint32_t m_clut_serial_cached = 0;
    uint32_t m_clut_base_cached = 0;

    std::vector<uint32_t> m_prims;
    /* Bin rectangle per primitive, packed as x0 | y0<<8 | x1<<16 | y1<<24
     * over the 32 by 32 coarse grid. */
    std::vector<uint32_t> m_prim_bins;
    std::vector<uint32_t> m_bin_index;
    std::vector<uint32_t> m_bin_range;
    std::vector<uint32_t> m_bin_count;

    RasterPush m_push{};
    /* The clipped pixel rectangle of the whole batch. */
    uint32_t m_px0 = 0, m_py0 = 0, m_px1 = 0, m_py1 = 0;
    uint32_t m_max_py = 0;

    Stats m_stats;
    bool m_said_tex_psm = false;
    bool m_said_mtba = false;
    bool m_said_reserved = false;
    bool m_said_short_line = false;
    bool m_said_line_model = false;
    bool m_said_scanmsk_reserved = false;
    bool m_said_aa1_ignored = false;
};

} // namespace gsr

#endif /* ICORECOMP_GS_DRAW_H */
