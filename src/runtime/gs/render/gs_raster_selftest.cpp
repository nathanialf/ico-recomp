/* gs/render/gs_raster_selftest.cpp: icorecomp-gs-raster-selftest.
 *
 * Ours (MIT). Exercises the parts of milestone (b) that can be wrong without
 * anyone seeing it in a picture, and needs no GPU, no disc and no ROM-derived
 * data:
 *
 *   1. Coverage. A reference rasteriser built on gs_prim.h's own rules walks
 *      a 4x4 pixel grid for two triangles that share an edge, two sprites and
 *      a point, and the expected masks are written out by hand from the rule
 *      in the comment above each one. They are not recorded from a run of
 *      this code, which would only prove the code agrees with itself. The
 *      pair of triangles is the top-left rule's own test: together they must
 *      cover every pixel of the square exactly once, no pixel twice and none
 *      missed.
 *   2. Retention. A strip, a fan, a list and a sprite run through the vertex
 *      queue, and each emitted primitive's vertex set is checked against the
 *      set the manual's retention rule gives. Also the ADC form, which
 *      queues a vertex without drawing, and a PRIM write, which empties the
 *      queue.
 *   3. Assembly equivalence. The same geometry submitted as a PACKED loop and
 *      as a REGLIST loop must produce byte-identical primitive records.
 *   4. The Z plane solve. A triangle whose plane has exact integer gradients
 *      is emitted and gs_prim.h's fixed-point DDA is evaluated at several
 *      pixels against the closed form.
 *   5. Milestone (d): AA1's coverage on edges whose covered area can be
 *      written down by hand, the line DDA on segments whose pixels can be,
 *      SCANMSK's row rule, and the aliased FRAME/ZBUF word.
 *   6. Milestone (f): the sub-sample grid for every allowed render scale and
 *      the shadow's page state machine.
 *   7. The shader interface and the dispatch grid: the offset of every field
 *      of every push block against the order the GLSL declares it, the record
 *      stride against the last word named in it, and the workgroup counts a
 *      batch can ask for, including the empty batch and the inverted scissor
 *      that produce none. All of it is what a first run on hardware fails on
 *      with no log line: a reordered push block is a wrong picture, and a
 *      dispatch grid past the API's 65535 groups per axis is undefined.
 *
 * Exit status 0 when every case passed, 1 otherwise.
 */
#include "gif_decode.h"
#include "gs_crtc.h"   /* ScanoutPush, for the shader interface case; header only */
#include "gs_draw.h"
#include "gs_prim.h"
#include "gs_regs.h"
#include "gs_shadow.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void fail(const std::string& what, const std::string& got, const std::string& want) {
    std::printf("FAIL %s\n  got:  %s\n  want: %s\n", what.c_str(), got.c_str(), want.c_str());
    ++g_failures;
}

/* ---- 1. coverage ----------------------------------------------------------- */

/* The reference rasteriser: the whole of it is the loop, because the rule
 * itself lives in gs_prim.h and is the thing under test. One row of the mask
 * per pixel row, bit 0 the leftmost pixel. */
const int kGrid = 4;

std::string show(const unsigned* rows) {
    std::string s;
    for (int y = 0; y < kGrid; ++y) {
        for (int x = 0; x < kGrid; ++x) s += ((rows[y] >> x) & 1u) ? '#' : '.';
        s += y + 1 < kGrid ? '/' : ' ';
    }
    return s;
}

void check_mask(const char* what, const unsigned* got, const unsigned* want) {
    if (std::memcmp(got, want, sizeof(unsigned) * kGrid) == 0) return;
    fail(what, show(got), show(want));
}

void raster_triangle(int x0, int y0, int x1, int y1, int x2, int y2, unsigned* rows) {
    /* The same winding normalisation the assembler does, so a triangle given
     * either way round produces the same coverage. */
    if (gsr::gs_i64_sign(gsr::gs_cross64(x1 - x0, y2 - y0, y1 - y0, x2 - x0)) < 0) {
        int tx = x1, ty = y1;
        x1 = x2; y1 = y2;
        x2 = tx; y2 = ty;
    }
    for (int y = 0; y < kGrid; ++y) {
        rows[y] = 0;
        for (int x = 0; x < kGrid; ++x) {
            if (gsr::gs_covers_triangle(x0, y0, x1, y1, x2, y2, x, y)) rows[y] |= 1u << x;
        }
    }
}

void raster_sprite(int x0, int y0, int x1, int y1, unsigned* rows) {
    for (int y = 0; y < kGrid; ++y) {
        rows[y] = 0;
        for (int x = 0; x < kGrid; ++x) {
            if (gsr::gs_covers_sprite(x0, y0, x1, y1, x, y)) rows[y] |= 1u << x;
        }
    }
}

void raster_point(int x0, int y0, unsigned* rows) {
    for (int y = 0; y < kGrid; ++y) {
        rows[y] = 0;
        for (int x = 0; x < kGrid; ++x) {
            if (gsr::gs_covers_point(x0, y0, x, y)) rows[y] |= 1u << x;
        }
    }
}

void test_coverage() {
    unsigned lower[kGrid], upper[kGrid], got[kGrid];

    /* The lower-left half of a 4x4 pixel square: pixel corners (0,0), (4,0)
     * and (0,4), which in 16.4 are (0,0), (64,0) and (0,64).
     *
     * By hand. The two axis-aligned edges pass every sample in the square.
     * The hypotenuse runs from (64,0) to (0,64), so its edge function is
     * 64 * (64 - sx - sy) and it passes when sx + sy < 64; it is neither a
     * left edge (dy is +64) nor a top edge (dy is not zero), so a sample
     * exactly on it is rejected. With sx = 16px + 8 and sy = 16py + 8 that
     * is 16(px + py) + 16 < 64, so px + py <= 2.
     *
     *   ###.
     *   ##..
     *   #...
     *   ....                                                              */
    const unsigned want_lower[kGrid] = { 0x7u, 0x3u, 0x1u, 0x0u };
    raster_triangle(0, 0, 64, 0, 0, 64, got);
    check_mask("triangle, lower-left half of a 4x4 square", got, want_lower);
    std::memcpy(lower, got, sizeof(got));

    /* The upper-right half, corners (4,0), (4,4) and (0,4). Its two
     * axis-aligned edges are the right edge (rejects a sample at sx == 64,
     * which no sample reaches) and the bottom edge (rejects sy == 64, which
     * no sample reaches either). Its hypotenuse runs from (0,64) to (64,0),
     * so dy is -64: it is a left edge, and a sample exactly on it is
     * accepted. That gives sx + sy >= 64, so px + py >= 3.
     *
     *   ...#
     *   ..##
     *   .###
     *   ####                                                              */
    const unsigned want_upper[kGrid] = { 0x8u, 0xCu, 0xEu, 0xFu };
    raster_triangle(64, 0, 64, 64, 0, 64, got);
    check_mask("triangle, upper-right half of a 4x4 square", got, want_upper);
    std::memcpy(upper, got, sizeof(got));

    /* The rule's whole point: the shared edge belongs to exactly one of
     * them, so the pair tiles the square once. */
    for (int y = 0; y < kGrid; ++y) {
        if ((lower[y] & upper[y]) != 0u) {
            fail("shared edge drawn twice on row " + std::to_string(y),
                 show(lower), show(upper));
            break;
        }
        if ((lower[y] | upper[y]) != 0xFu) {
            fail("shared edge left a gap on row " + std::to_string(y),
                 show(lower), show(upper));
            break;
        }
    }

    /* The same triangle with its second and third vertices exchanged. The
     * assembler winds it back, so the coverage must not move. */
    raster_triangle(0, 0, 0, 64, 64, 0, got);
    check_mask("triangle, reversed winding", got, want_lower);

    /* A sprite from (0.5, 0.5) to (2.5, 2.5), which is (8,8) to (40,40).
     * A pixel is covered when its centre is in [8, 40): centres are 8, 24,
     * 40 and 56, so pixels 0 and 1 on each axis.
     *
     *   ##..
     *   ##..
     *   ....
     *   ....                                                              */
    const unsigned want_half[kGrid] = { 0x3u, 0x3u, 0x0u, 0x0u };
    raster_sprite(8, 8, 40, 40, got);
    check_mask("sprite at half-pixel positions", got, want_half);

    /* A sprite on integer corners, (1,1) to (3,3), which is (16,16) to
     * (48,48). Centres in [16, 48) are 24 and 40, so pixels 1 and 2. This is
     * the case the exclusive right and bottom edges are stated for: the
     * sprite is two pixels wide, not three.
     *
     *   ....
     *   .##.
     *   .##.
     *   ....                                                              */
    const unsigned want_int[kGrid] = { 0x0u, 0x6u, 0x6u, 0x0u };
    raster_sprite(16, 16, 48, 48, got);
    check_mask("sprite on integer corners", got, want_int);

    /* A sprite one sixteenth of a pixel wide, from (24,24) to (25,40).
     * Pixel 1's centre is at 24 exactly, and the left edge is inclusive, so
     * that one column is covered; in y the range [24,40) holds only the
     * centre at 24, which is pixel row 1. One pixel, not none.
     *
     *   ....
     *   .#..
     *   ....
     *   ....                                                              */
    const unsigned want_thin_hit[kGrid] = { 0x0u, 0x2u, 0x0u, 0x0u };
    raster_sprite(24, 24, 25, 40, got);
    check_mask("sprite one sixteenth wide, over a sample point", got, want_thin_hit);

    /* The same width shifted one sixteenth right, to (25,24) to (26,40).
     * Now no centre lies in [25, 26): pixel 1's is at 24 and pixel 2's at
     * 40, so the sprite covers nothing at all. That is what a sample rule
     * does, and it is the case a renderer that rounded corners to whole
     * pixels would get wrong in the other direction. */
    const unsigned want_thin_miss[kGrid] = { 0x0u, 0x0u, 0x0u, 0x0u };
    raster_sprite(25, 24, 26, 40, got);
    check_mask("sprite one sixteenth wide, between sample points", got, want_thin_miss);

    /* A point at (2.25, 1.6875), which is (36, 27). It lands in pixel
     * (36 >> 4, 27 >> 4) = (2, 1). */
    const unsigned want_point[kGrid] = { 0x0u, 0x4u, 0x0u, 0x0u };
    raster_point(36, 27, got);
    check_mask("point", got, want_point);
}

/* ---- a harness for the assembler ------------------------------------------- */

struct Harness : gsr::DrawFlusher {
    gsr::RegisterFile regs;
    gsr::DrawEngine draw{regs};
    int flushes = 0;

    Harness() {
        draw.set_flusher(this);
        /* One batch key for the whole of a test, so nothing flushes under
         * the test's feet. A scissor over the whole drawing area, no
         * XYOFFSET, and a plain 32-bit frame and Z buffer. */
        /* SCAX0 at bit 0, SCAX1 at 16, SCAY0 at 32, SCAY1 at 48. */
        regs.write(gsr::GS_REG_SCISSOR_1, (2047ull << 16) | (2047ull << 48));
        regs.write(gsr::GS_REG_XYOFFSET_1, 0);
        regs.write(gsr::GS_REG_FRAME_1, 64ull | (8ull << 16));
        regs.write(gsr::GS_REG_ZBUF_1, 128ull | (1ull << 24));
        regs.write(gsr::GS_REG_PRMODECONT, 1);
        regs.write(gsr::GS_REG_RGBAQ, 0x80402010ull | (0x3F800000ull << 32));
    }
    void gsr_flush_draws() override { ++flushes; }

    void prim(uint32_t value) {
        regs.write(gsr::GS_REG_PRIM, value);
        draw.prim_written();
    }
    void rgba(uint32_t c) {
        regs.write(gsr::GS_REG_RGBAQ, (uint64_t)c | (0x3F800000ull << 32));
    }
    /* A vertex in whole pixels, kicking or not. */
    void xyz(int px, int py, uint32_t z, bool kick) {
        xyz16(px * 16, py * 16, z, kick);
    }
    /* The same in sixteenths of a pixel, which is the register's own unit and
     * what the sub-pixel cases need. */
    void xyz16(int x16, int y16, uint32_t z, bool kick) {
        const uint64_t v = (uint64_t)(uint32_t)x16 | ((uint64_t)(uint32_t)y16 << 16)
                         | ((uint64_t)z << 32);
        const uint32_t addr = kick ? gsr::GS_REG_XYZ2 : gsr::GS_REG_XYZ3;
        regs.write(addr, v);
        draw.vertex(addr, v);
    }

    uint32_t count() const { return draw.prim_count(); }
    uint32_t word(uint32_t prim, uint32_t index) const {
        return draw.prims()[(size_t)prim * GSP_STRIDE + index];
    }
    /* The three vertices of a record, as a sorted, printable set, so a
     * comparison does not depend on the winding normalisation's order. */
    std::string vertex_set(uint32_t prim, int n) const {
        std::vector<std::string> v;
        static const uint32_t xs[3] = { GSP_X0, GSP_X1, GSP_X2 };
        static const uint32_t ys[3] = { GSP_Y0, GSP_Y1, GSP_Y2 };
        for (int i = 0; i < n; ++i) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "(%d,%d)", (int)word(prim, xs[i]) / 16,
                          (int)word(prim, ys[i]) / 16);
            v.push_back(buf);
        }
        for (size_t i = 0; i + 1 < v.size(); ++i) {
            for (size_t j = i + 1; j < v.size(); ++j) {
                if (v[j] < v[i]) std::swap(v[i], v[j]);
            }
        }
        std::string s;
        for (const std::string& e : v) s += e;
        return s;
    }
};

void check_prims(const char* what, const Harness& h, int per_prim,
                 const std::vector<std::string>& want) {
    if (h.count() != want.size()) {
        fail(what, std::to_string(h.count()) + " primitives",
             std::to_string(want.size()) + " primitives");
        return;
    }
    for (uint32_t i = 0; i < h.count(); ++i) {
        const std::string got = h.vertex_set(i, per_prim);
        if (got != want[i]) fail(std::string(what) + " #" + std::to_string(i), got, want[i]);
    }
}

/* ---- 2. retention ---------------------------------------------------------- */

void test_retention() {
    {   /* Triangle list: two independent triangles from six vertices. The
         * zigzag keeps every set of three off a straight line, which a
         * zero-area triangle would be, and those are dropped. */
        Harness h;
        h.prim(3);
        for (int i = 0; i < 6; ++i) h.xyz(i * 10, (i % 2) * 10, 0, true);
        check_prims("triangle list", h, 3,
                    { "(0,0)(10,10)(20,0)", "(30,10)(40,0)(50,10)" });
    }
    {   /* Triangle strip: the last two vertices are kept, so five vertices
         * give three triangles sharing edges. */
        Harness h;
        h.prim(4);
        for (int i = 0; i < 5; ++i) h.xyz(i * 10, (i % 2) * 10, 0, true);
        check_prims("triangle strip", h, 3,
                    { "(0,0)(10,10)(20,0)", "(10,10)(20,0)(30,10)",
                      "(20,0)(30,10)(40,0)" });
    }
    {   /* Triangle fan: the first vertex is kept, so every triangle shares
         * it and the newest vertex replaces the middle one. */
        Harness h;
        h.prim(5);
        for (int i = 0; i < 5; ++i) h.xyz(i * 10, (i % 2) * 10, 0, true);
        check_prims("triangle fan", h, 3,
                    { "(0,0)(10,10)(20,0)", "(0,0)(20,0)(30,10)",
                      "(0,0)(30,10)(40,0)" });
    }
    {   /* Line strip: one vertex is kept, so four vertices give three
         * lines. Lines are not wound, so the pair is in submission order. */
        Harness h;
        h.prim(2);
        for (int i = 0; i < 4; ++i) h.xyz(i * 10, 0, 0, true);
        check_prims("line strip", h, 2,
                    { "(0,0)(10,0)", "(10,0)(20,0)", "(20,0)(30,0)" });
    }
    {   /* Sprites take two vertices each and keep nothing. */
        Harness h;
        h.prim(6);
        h.xyz(1, 2, 0, true);
        h.xyz(5, 8, 0, true);
        h.xyz(20, 20, 0, true);
        h.xyz(24, 26, 0, true);
        check_prims("sprite pairs", h, 2,
                    { "(1,2)(5,8)", "(20,20)(24,26)" });
    }
    {   /* ADC: the third vertex is queued without drawing, so the first
         * triangle a strip produces is made of vertices 1, 2 and 3. */
        Harness h;
        h.prim(4);
        h.xyz(0, 0, 0, true);
        h.xyz(10, 10, 0, true);
        h.xyz(20, 0, 0, false);
        h.xyz(30, 10, 0, true);
        check_prims("triangle strip with an ADC vertex", h, 3,
                    { "(10,10)(20,0)(30,10)" });
    }
    {   /* A PRIM write empties the queue, which is how a strip restarts
         * without an ADC vertex. Two vertices, a PRIM write, then three
         * more: only one triangle, from the three after the write. */
        Harness h;
        h.prim(4);
        h.xyz(0, 0, 0, true);
        h.xyz(10, 10, 0, true);
        h.prim(4);
        h.xyz(20, 0, 0, true);
        h.xyz(30, 10, 0, true);
        h.xyz(40, 0, 0, true);
        check_prims("PRIM write restarts the strip", h, 3,
                    { "(20,0)(30,10)(40,0)" });
    }
    {   /* Flat shading takes the last vertex's colour, and the winding
         * normalisation must not move it. The triangle below is submitted
         * clockwise in window coordinates, so the assembler swaps its second
         * and third vertices; the flat colour still has to be the third
         * vertex's. */
        Harness h;
        h.prim(3); /* IIP clear: flat */
        h.rgba(0x11111111u);
        h.xyz(0, 0, 0, true);
        h.rgba(0x22222222u);
        h.xyz(0, 10, 0, true);
        h.rgba(0x33333333u);
        h.xyz(10, 0, 0, true);
        if (h.count() != 1) {
            fail("flat colour: primitive count", std::to_string(h.count()), "1");
        } else if (h.word(0, GSP_RGBA_FLAT) != 0x33333333u) {
            char got[32];
            std::snprintf(got, sizeof(got), "%08x", h.word(0, GSP_RGBA_FLAT));
            fail("flat colour is the last vertex's", got, "33333333");
        }
    }
}

/* ---- 3. PACKED against REGLIST --------------------------------------------- */

struct Packet {
    std::vector<uint8_t> bytes;
    void qword(uint64_t lo, uint64_t hi) {
        const size_t at = bytes.size();
        bytes.resize(at + 16);
        std::memcpy(bytes.data() + at, &lo, 8);
        std::memcpy(bytes.data() + at + 8, &hi, 8);
    }
    void tag(uint32_t nloop, bool eop, uint32_t flg, uint32_t nreg, uint64_t regs) {
        qword((uint64_t)(nloop & 0x7FFF) | ((uint64_t)(eop ? 1 : 0) << 15)
              | ((uint64_t)(flg & 3) << 58) | ((uint64_t)(nreg & 15) << 60), regs);
    }
    uint32_t qwords() const { return (uint32_t)(bytes.size() / 16); }
};

/* The sink the backend uses, in miniature: registers into the file, PRIM and
 * the vertex addresses into the assembler. */
struct Sink {
    Harness& h;
    explicit Sink(Harness& hh) : h(hh) {}
    void reg(uint32_t addr, uint64_t value) {
        h.regs.write(addr, value);
        if (addr == gsr::GS_REG_PRIM) h.draw.prim_written();
        else if (addr == gsr::GS_REG_XYZ2 || addr == gsr::GS_REG_XYZF2
                 || addr == gsr::GS_REG_XYZ3 || addr == gsr::GS_REG_XYZF3) {
            h.draw.vertex(addr, value);
        }
    }
    void image(const uint8_t*, uint32_t) {}
    void note(const char*) {}
};

struct Vert {
    uint32_t colour;
    int px, py;
    uint32_t z;
};

const Vert kVerts[3] = {
    { 0x807F3010u, 3, 5, 0x00010000u },
    { 0x40203010u, 40, 9, 0x00020000u },
    { 0xFF00FF7Fu, 21, 33, 0x00030000u },
};

std::vector<uint32_t> assemble(bool reglist) {
    Harness h;
    Sink sink(h);
    gsr::GifDecodeState st;

    /* State first, as A+D, identically for both. PRIM 3 with IIP set so the
     * per-vertex colours all reach the record. */
    Packet state;
    state.tag(1, false, 0, 1, 0xEull);
    state.qword((uint64_t)(3u | (1u << 3)), gsr::GS_REG_PRIM);
    gsr::gif_decode(st, state.bytes.data(), state.qwords(), sink);

    Packet geom;
    if (reglist) {
        /* REGLIST carries register values already in register layout, two to
         * a quadword. RGBAQ's Q is the whole high half, so it is written out
         * as the 1.0 the PACKED path's latch holds after reset. */
        geom.tag(3, true, 1, 2, (uint64_t)gsr::GS_REG_RGBAQ
                                | ((uint64_t)gsr::GS_REG_XYZ2 << 4));
        for (const Vert& v : kVerts) {
            const uint64_t rgbaq = (uint64_t)v.colour | (0x3F800000ull << 32);
            const uint64_t xyz = (uint64_t)(uint32_t)(v.px * 16)
                               | ((uint64_t)(uint32_t)(v.py * 16) << 16)
                               | ((uint64_t)v.z << 32);
            geom.qword(rgbaq, xyz);
        }
    } else {
        /* PACKED spreads one register value over a whole quadword: RGBAQ
         * takes one byte out of each 32-bit lane and its Q from the latch,
         * and XYZ2 takes X and Y out of the low half and Z out of the high
         * half. */
        geom.tag(3, true, 0, 2, (uint64_t)gsr::GS_REG_RGBAQ
                                | ((uint64_t)gsr::GS_REG_XYZ2 << 4));
        for (const Vert& v : kVerts) {
            geom.qword((uint64_t)(v.colour & 0xFFu)
                       | ((uint64_t)((v.colour >> 8) & 0xFFu) << 32),
                       (uint64_t)((v.colour >> 16) & 0xFFu)
                       | ((uint64_t)((v.colour >> 24) & 0xFFu) << 32));
            geom.qword((uint64_t)(uint32_t)(v.px * 16)
                       | ((uint64_t)(uint32_t)(v.py * 16) << 32),
                       (uint64_t)v.z);
        }
    }
    gsr::gif_decode(st, geom.bytes.data(), geom.qwords(), sink);
    return h.draw.prims();
}

void test_packed_reglist_equivalence() {
    const std::vector<uint32_t> packed = assemble(false);
    const std::vector<uint32_t> reglist = assemble(true);
    if (packed.size() != GSP_STRIDE) {
        fail("PACKED assembly produced one triangle",
             std::to_string(packed.size() / GSP_STRIDE), "1");
        return;
    }
    if (packed != reglist) {
        std::string a, b;
        for (size_t i = 0; i < packed.size(); ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08x ", packed[i]);
            a += buf;
            std::snprintf(buf, sizeof(buf), "%08x ",
                          i < reglist.size() ? reglist[i] : 0u);
            b += buf;
        }
        fail("PACKED and REGLIST assemble the same record", a, b);
    }
}

/* ---- 4. the Z plane solve --------------------------------------------------- */

void test_z_plane() {
    /* Corners (0,0), (10,0) and (0,10) in pixels, with Z 0, 1000 and 2000.
     * The gradients are exactly 100 per pixel of x and 200 per pixel of y,
     * and the value at a pixel centre is 100 px + 200 py + 150. */
    Harness h;
    h.prim(3);
    h.xyz(0, 0, 0, true);
    h.xyz(10, 0, 1000, true);
    h.xyz(0, 10, 2000, true);
    if (h.count() != 1) {
        fail("Z plane: primitive count", std::to_string(h.count()), "1");
        return;
    }
    if (h.word(0, GSP_REFX) != 0 || h.word(0, GSP_REFY) != 0) {
        fail("Z plane: reference pixel", std::to_string(h.word(0, GSP_REFX)) + ","
             + std::to_string(h.word(0, GSP_REFY)), "0,0");
        return;
    }
    const int probes[][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 2, 3 }, { 4, 4 } };
    for (const auto& p : probes) {
        const uint32_t got = gsr::gs_z_at(h.word(0, GSP_Z_I), h.word(0, GSP_Z_F),
                                          h.word(0, GSP_ZDX_I), h.word(0, GSP_ZDX_F),
                                          h.word(0, GSP_ZDY_I), h.word(0, GSP_ZDY_F),
                                          (uint32_t)p[0], (uint32_t)p[1]);
        const uint32_t want = (uint32_t)(100 * p[0] + 200 * p[1] + 150);
        if (got != want) {
            fail("Z at pixel (" + std::to_string(p[0]) + "," + std::to_string(p[1]) + ")",
                 std::to_string(got), std::to_string(want));
        }
    }
}

/* ---- 5. AA1 coverage -------------------------------------------------------
 *
 * Every expected value here is worked out from the rule in gs_prim.h, not
 * recorded from a run. The rule is: the covered fraction of a pixel is the
 * area of its unit square on the interior side of the nearest edge, and the
 * edge function's value is 16 * |edge| times the signed distance in pixels,
 * so a distance is turned into an edge value by multiplying by 16 * |edge|.
 *
 * The comparison is to a tolerance of one part in a thousand of full
 * coverage, which is well under one step of the 129-value alpha the rule
 * quantises to: the shader evaluates this in float and the C++ side promotes
 * some intermediates to double, so the two agree to within a rounding rather
 * than exactly. Nothing about coverage decides which pixels are drawn, so
 * that difference cannot move a pixel; it can only move an edge pixel's alpha
 * by one at worst. */
void check_near(const char* what, float got, float want) {
    const float d = got > want ? got - want : want - got;
    if (d <= 0.001f) return;
    char g[32], w[32];
    std::snprintf(g, sizeof(g), "%.5f", (double)got);
    std::snprintf(w, sizeof(w), "%.5f", (double)want);
    fail(what, g, w);
}

void test_aa1_coverage() {
    /* A vertical edge, running down the screen: dx 0, dy 16 (one pixel). The
     * square's half width along the normal is 0.5, so coverage is 0.5 + d
     * until the edge leaves the pixel. One pixel of distance is 16 * |edge| =
     * 256 units of edge value. */
    check_near("vertical edge through the sample point",
               gsr::gs_aa_edge_cover(0.0f, 0, 16), 0.5f);
    check_near("vertical edge a quarter pixel inside",
               gsr::gs_aa_edge_cover(64.0f, 0, 16), 0.75f);
    check_near("vertical edge a quarter pixel outside",
               gsr::gs_aa_edge_cover(-64.0f, 0, 16), 0.25f);
    check_near("vertical edge half a pixel inside, the whole square",
               gsr::gs_aa_edge_cover(128.0f, 0, 16), 1.0f);
    check_near("vertical edge half a pixel outside, nothing",
               gsr::gs_aa_edge_cover(-128.0f, 0, 16), 0.0f);
    check_near("vertical edge two pixels inside stays at one",
               gsr::gs_aa_edge_cover(1024.0f, 0, 16), 1.0f);

    /* A horizontal edge is the same rule with the axes exchanged. */
    check_near("horizontal edge through the sample point",
               gsr::gs_aa_edge_cover(0.0f, 16, 0), 0.5f);
    check_near("horizontal edge a quarter pixel inside",
               gsr::gs_aa_edge_cover(64.0f, 16, 0), 0.75f);

    /* A 45 degree edge. Its normal is (1, 1) / sqrt(2), so the square's half
     * width along it is sqrt(2) / 2 and the whole of the coverage is the
     * corner branch. Through the centre it is half the square, by symmetry.
     * At half that reach, the uncovered corner is a triangle of area
     * t * t / (2 * amax * amin) with t = sqrt(2) / 4 and amax = amin =
     * sqrt(2) / 2, which is 0.125, so coverage is 0.875. The edge value for a
     * distance d is 16 * 16 * sqrt(2) * d, which is 128 at d = sqrt(2) / 4
     * and 256 at d = sqrt(2) / 2. */
    check_near("45 degree edge through the sample point",
               gsr::gs_aa_edge_cover(0.0f, 16, 16), 0.5f);
    check_near("45 degree edge, one corner uncovered",
               gsr::gs_aa_edge_cover(128.0f, 16, 16), 0.875f);
    check_near("45 degree edge, one corner covered",
               gsr::gs_aa_edge_cover(-128.0f, 16, 16), 0.125f);
    check_near("45 degree edge at the square's corner",
               gsr::gs_aa_edge_cover(256.0f, 16, 16), 1.0f);

    /* The alpha the blend unit reads: full coverage is the GS's own one, so
     * an interior pixel blends exactly as it would without AA1. */
    if (gsr::gs_aa_alpha(1.0f) != 128u) {
        fail("full coverage is alpha 0x80", std::to_string(gsr::gs_aa_alpha(1.0f)), "128");
    }
    if (gsr::gs_aa_alpha(0.5f) != 64u) {
        fail("half coverage is alpha 0x40", std::to_string(gsr::gs_aa_alpha(0.5f)), "64");
    }
    if (gsr::gs_aa_alpha(0.0f) != 0u) {
        fail("no coverage is alpha 0", std::to_string(gsr::gs_aa_alpha(0.0f)), "0");
    }

    /* And the whole-triangle rule, on the lower-left half of a 4 by 4 pixel
     * square: corners (0,0), (64,0) and (0,64) in 16.4.
     *
     * Pixel (0,0), centre (8,8). The two axis-aligned edges are half a pixel
     * away, which is the whole square, and the hypotenuse is further, so the
     * pixel is fully covered and AA1 changes nothing about it.
     *
     * Pixel (2,1), centre (40,24), lies exactly on the hypotenuse: its edge
     * value is 64 * (64 - 40 - 24) = 0, and a 45 degree edge through the
     * sample point covers half the square. The other two edges are 1.5 and
     * 2.5 pixels away. So the coverage is a half. */
    check_near("interior pixel is fully covered",
               gsr::gs_aa_triangle_cover(0, 0, 64, 0, 0, 64, 8, 8), 1.0f);
    check_near("pixel on the hypotenuse is half covered",
               gsr::gs_aa_triangle_cover(0, 0, 64, 0, 0, 64, 40, 24), 0.5f);
    /* A pixel well outside every edge covers nothing. */
    check_near("pixel outside the triangle covers nothing",
               gsr::gs_aa_triangle_cover(0, 0, 64, 0, 0, 64, 56, 56), 0.0f);
}

/* ---- 6. the line DDA -------------------------------------------------------
 *
 * The record the assembler built is walked with the same coverage functions
 * the fine pass calls, so what is under test is the rule and the fixed-point
 * setup together. Expected pixels are written out from the rule: the major
 * axis is covered where p * 16 + 8 lies in [min, max) of the endpoints, and
 * the minor pixel is the one the interpolation at that sample point falls in.
 */
void raster_line(const Harness& h, uint32_t prim, unsigned* rows) {
    const uint32_t flags = h.word(prim, GSP_FLAGS);
    const int x0 = (int)h.word(prim, GSP_X0);
    const int y0 = (int)h.word(prim, GSP_Y0);
    const int x1 = (int)h.word(prim, GSP_X1);
    const int y1 = (int)h.word(prim, GSP_Y1);
    const bool major_x = (flags & GSP_F_MAJOR_X) != 0;
    const bool dot = (flags & GSP_F_LINE_DOT) != 0;
    const int minor_ref = (int)h.word(prim, GSP_X2);
    const int slope = (int)h.word(prim, GSP_LINE_SLOPE);
    const int refp = major_x ? (int)h.word(prim, GSP_REFX) : (int)h.word(prim, GSP_REFY);
    const int lo = major_x ? (x0 < x1 ? x0 : x1) : (y0 < y1 ? y0 : y1);
    const int hi = major_x ? (x0 < x1 ? x1 : x0) : (y0 < y1 ? y1 : y0);
    for (int y = 0; y < kGrid; ++y) {
        rows[y] = 0;
        for (int x = 0; x < kGrid; ++x) {
            bool on;
            if (dot) {
                on = gsr::gs_covers_line_dot(x0, y0, x, y);
            } else {
                const int p = major_x ? x : y;
                const int smin = (major_x ? y : x) * 16 + 8;
                on = gsr::gs_covers_line_at(lo, hi, minor_ref, slope, refp, p, smin);
            }
            if (on) rows[y] |= 1u << x;
        }
    }
}

void test_line_dda() {
    unsigned got[kGrid];
    {   /* Horizontal, from (0,1) to (4,1). Major x. The span is [0, 64), so
         * the sample points at 8, 24, 40 and 56 are in and the one at 72 is
         * not: pixels 0 to 3. The far endpoint at x = 4 is exclusive, which
         * is the same convention the sprite rule states.
         *
         *   ....
         *   ####
         *   ....
         *   ....                                                          */
        Harness h;
        h.prim(1);
        h.xyz(0, 1, 0, true);
        h.xyz(4, 1, 0, true);
        const unsigned want[kGrid] = { 0x0u, 0xFu, 0x0u, 0x0u };
        if (h.count() != 1) {
            fail("horizontal line: primitive count", std::to_string(h.count()), "1");
        } else {
            raster_line(h, 0, got);
            check_mask("horizontal line, far endpoint exclusive", got, want);
        }
    }
    {   /* A 45 degree line from (0,0) to (4,4). The major axis is x, because
         * the two deltas are equal and the rule takes x. At major sample
         * x = p + 0.5 the minor coordinate is p + 0.5, which falls in pixel
         * p, so the line is the diagonal.
         *
         *   #...
         *   .#..
         *   ..#.
         *   ...#                                                          */
        Harness h;
        h.prim(1);
        h.xyz(0, 0, 0, true);
        h.xyz(4, 4, 0, true);
        const unsigned want[kGrid] = { 0x1u, 0x2u, 0x4u, 0x8u };
        raster_line(h, 0, got);
        check_mask("45 degree line", got, want);
    }
    {   /* Steeper than 45 degrees, from (0,0) to (2,4): the major axis is y.
         * At major sample y = p + 0.5 the minor coordinate is (p + 0.5) / 2,
         * which is 0.25, 0.75, 1.25 and 1.75, so pixels 0, 0, 1, 1.
         *
         *   #...
         *   #...
         *   .#..
         *   .#..                                                          */
        Harness h;
        h.prim(1);
        h.xyz(0, 0, 0, true);
        h.xyz(2, 4, 0, true);
        const unsigned want[kGrid] = { 0x1u, 0x1u, 0x2u, 0x2u };
        raster_line(h, 0, got);
        check_mask("steep line, major axis y", got, want);
    }
    {   /* Backwards: (4,1) to (0,1) has to cover the same pixels as the
         * forward case, because the span is taken as [min, max). */
        Harness h;
        h.prim(1);
        h.xyz(4, 1, 0, true);
        h.xyz(0, 1, 0, true);
        const unsigned want[kGrid] = { 0x0u, 0xFu, 0x0u, 0x0u };
        raster_line(h, 0, got);
        check_mask("horizontal line submitted right to left", got, want);
    }
    {   /* A line whose two endpoints fall inside one pixel of the major
         * axis: from (1 + 2/16, 2 + 3/16) to (1 + 5/16, 2 + 5/16). The major
         * axis is x and the span is [18, 21) in sixteenths, which holds no
         * sample point at all: the nearest are 8 and 24. So the DDA emits its
         * first pixel and steps out of the line: pixel (1,2), and nothing
         * else.
         *
         *   ....
         *   ....
         *   .#..
         *   ....                                                          */
        Harness h;
        h.prim(1);
        h.xyz16(16 + 2, 32 + 3, 0, true);
        h.xyz16(16 + 5, 32 + 5, 0, true);
        const unsigned want[kGrid] = { 0x0u, 0x0u, 0x2u, 0x0u };
        if (h.count() != 1) {
            fail("sub-pixel line: primitive count", std::to_string(h.count()), "1");
        } else if ((h.word(0, GSP_FLAGS) & GSP_F_LINE_DOT) == 0) {
            fail("sub-pixel line is marked as a dot", "no flag", "GSP_F_LINE_DOT");
        } else {
            raster_line(h, 0, got);
            check_mask("sub-pixel line draws its first pixel", got, want);
        }
        if (h.draw.stats().short_lines != 1) {
            fail("sub-pixel line is counted",
                 std::to_string(h.draw.stats().short_lines), "1");
        }
    }
}

/* ---- 7. SCANMSK ------------------------------------------------------------ */

void test_scanmsk() {
    /* The manual's table, one row at a time. */
    for (int y = 0; y < 4; ++y) {
        if (!gsr::gs_scanmsk_draws(0u, y)) {
            fail("SCANMSK 0 draws row " + std::to_string(y), "masked", "drawn");
        }
        if (!gsr::gs_scanmsk_draws(1u, y)) {
            fail("SCANMSK 1 is reserved and draws row " + std::to_string(y),
                 "masked", "drawn");
        }
        const bool even = (y % 2) == 0;
        if (gsr::gs_scanmsk_draws(2u, y) != even) {
            fail("SCANMSK 2 draws even rows only, row " + std::to_string(y),
                 even ? "masked" : "drawn", even ? "drawn" : "masked");
        }
        if (gsr::gs_scanmsk_draws(3u, y) == even) {
            fail("SCANMSK 3 draws odd rows only, row " + std::to_string(y),
                 even ? "drawn" : "masked", even ? "masked" : "drawn");
        }
    }

    /* And that the assembler carries the register into the record, where the
     * fine pass reads it. */
    Harness h;
    h.regs.write(gsr::GS_REG_SCANMSK, 3);
    h.prim(6);
    h.xyz(0, 0, 0, true);
    h.xyz(8, 8, 0, true);
    if (h.count() != 1) {
        fail("SCANMSK: primitive count", std::to_string(h.count()), "1");
        return;
    }
    const uint32_t msk = (h.word(0, GSP_FLAGS) >> GSP_F_SCANMSK_SHIFT) & GSP_F_SCANMSK_MASK;
    if (msk != 3) fail("SCANMSK reaches the record", std::to_string(msk), "3");
    if (h.draw.stats().scanmsk != 1) {
        fail("SCANMSK is counted", std::to_string(h.draw.stats().scanmsk), "1");
    }
}

/* ---- 8. FRAME and ZBUF over the same bits ---------------------------------- */

void check_word(const char* what, uint32_t got, uint32_t want) {
    if (got == want) return;
    char g[16], w[16];
    std::snprintf(g, sizeof(g), "%08x", got);
    std::snprintf(w, sizeof(w), "%08x", want);
    fail(what, g, w);
}

void test_alias() {
    /* The test itself: same width and same address is one storage cell. */
    if (!gsr::gs_alias_pixel(GS_PSMCT32, 1234u, GS_PSMZ32, 1234u)) {
        fail("PSMCT32 and PSMZ32 at one address alias", "no", "yes");
    }
    if (gsr::gs_alias_pixel(GS_PSMCT32, 1234u, GS_PSMZ32, 1235u)) {
        fail("two addresses do not alias", "yes", "no");
    }
    if (gsr::gs_alias_pixel(GS_PSMCT16, 1234u, GS_PSMZ32, 1234u)) {
        fail("two widths at one address are not modelled as an alias", "yes", "no");
    }

    /* The order. The colour is written first and the depth second, so the
     * word ends up holding the depth wherever the depth's format owns bits.
     * A 32-bit pair: the depth is the whole word, and the colour read back
     * out of it is the depth's bits. */
    check_word("32-bit alias takes the depth",
               gsr::gs_alias_word(GS_PSMCT32, 0x11223344u, GS_PSMZ32, 0xAABBCCDDu, true),
               0xAABBCCDDu);
    /* With no depth write the colour stands, and the depth a later primitive
     * reads is the colour's own bits. That is the case that makes an aliased
     * Z buffer visibly wrong on hardware too. */
    check_word("32-bit alias with ZMSK keeps the colour",
               gsr::gs_alias_word(GS_PSMCT32, 0x11223344u, GS_PSMZ32, 0xAABBCCDDu, false),
               0x11223344u);
    /* A 24-bit depth owns only the low three bytes, so the colour's top byte
     * survives. */
    check_word("24-bit depth leaves the top byte alone",
               gsr::gs_alias_word(GS_PSMCT32, 0x11223344u, GS_PSMZ24, 0xAABBCCDDu, true),
               0x11BBCCDDu);
    /* A 16-bit pair: the colour is packed to 5-5-5-1 first, then the depth
     * covers all sixteen bits of it. */
    check_word("16-bit alias takes the depth",
               gsr::gs_alias_word(GS_PSMCT16, 0x80F8F8F8u, GS_PSMZ16, 0x1234u, true),
               0x1234u);
    /* And the two views of the finished word. */
    check_word("the colour read back out of an aliased word",
               gsr::gs_frame_unpack(GS_PSMCT32, 0xAABBCCDDu), 0xAABBCCDDu);
    check_word("the depth read back out of an aliased word",
               0xAABBCCDDu & gsr::gs_z_precision_mask(GS_PSMZ24), 0x00BBCCDDu);
    /* The 16-bit pack is exact against the expansion, which is what lets the
     * colour survive a round trip through the word. */
    check_word("gs_pack16 inverts gs_expand16",
               gsr::gs_pack16(gsr::gs_expand16(0x1234u)), 0x1234u);
}

/* ---- 9. the sub-sample grid ------------------------------------------------- */

void test_samples() {
    /* Scale 1 is the pixel centre and nothing else, which is what keeps it
     * byte identical to the path before render scale existed. */
    if (gsr::gs_sample_x(1, 0) != 8 || gsr::gs_sample_y(1, 0) != 8) {
        fail("scale 1 samples the pixel centre",
             std::to_string(gsr::gs_sample_x(1, 0)) + ","
             + std::to_string(gsr::gs_sample_y(1, 0)), "8,8");
    }

    /* The three grids, written out from the rule: the centre of each
     * sub-cell, across first and then down. */
    static const int k4[4][2] = { { 4, 4 }, { 12, 4 }, { 4, 12 }, { 12, 12 } };
    static const int k8[8][2] = {
        { 2, 4 }, { 6, 4 }, { 10, 4 }, { 14, 4 },
        { 2, 12 }, { 6, 12 }, { 10, 12 }, { 14, 12 },
    };
    static const int k16[16][2] = {
        { 2, 2 }, { 6, 2 }, { 10, 2 }, { 14, 2 },
        { 2, 6 }, { 6, 6 }, { 10, 6 }, { 14, 6 },
        { 2, 10 }, { 6, 10 }, { 10, 10 }, { 14, 10 },
        { 2, 14 }, { 6, 14 }, { 10, 14 }, { 14, 14 },
    };
    struct Grid { uint32_t n; const int (*pos)[2]; };
    const Grid grids[3] = { { 4, k4 }, { 8, k8 }, { 16, k16 } };
    for (const Grid& g : grids) {
        for (uint32_t s = 0; s < g.n; ++s) {
            const int x = gsr::gs_sample_x(g.n, s);
            const int y = gsr::gs_sample_y(g.n, s);
            if (x == g.pos[s][0] && y == g.pos[s][1]) continue;
            fail("sample " + std::to_string(s) + " of " + std::to_string(g.n),
                 std::to_string(x) + "," + std::to_string(y),
                 std::to_string(g.pos[s][0]) + "," + std::to_string(g.pos[s][1]));
        }
        /* Every sample is inside the pixel and no two share a position. */
        for (uint32_t a = 0; a < g.n; ++a) {
            if (gsr::gs_sample_x(g.n, a) < 0 || gsr::gs_sample_x(g.n, a) > 15
                || gsr::gs_sample_y(g.n, a) < 0 || gsr::gs_sample_y(g.n, a) > 15) {
                fail("sample inside the pixel, " + std::to_string(g.n), "outside", "inside");
            }
            for (uint32_t b = a + 1; b < g.n; ++b) {
                if (gsr::gs_sample_x(g.n, a) == gsr::gs_sample_x(g.n, b)
                    && gsr::gs_sample_y(g.n, a) == gsr::gs_sample_y(g.n, b)) {
                    fail("samples " + std::to_string(a) + " and " + std::to_string(b)
                         + " of " + std::to_string(g.n), "the same position", "distinct");
                }
            }
        }
        /* One workgroup is 256 threads, one per sample of the tile. */
        uint32_t tw = 0, th = 0;
        gsr::gs_scale_tile(g.n, &tw, &th);
        if (tw * th * g.n != 256) {
            fail("the tile fills a workgroup at " + std::to_string(g.n),
                 std::to_string(tw * th * g.n), "256");
        }
    }

    /* The sample the resolve takes a depth from: the one nearest the pixel
     * centre, lowest index on a tie. At 4 every sample is the same distance
     * from the centre, so it is sample 0; at 8 the nearest are the four at
     * x 6 and 10, and the first of those is index 1; at 16 they are the four
     * around the centre and the first is index 5. */
    const uint32_t want_center[4][2] = { { 1, 0 }, { 4, 0 }, { 8, 1 }, { 16, 5 } };
    for (const auto& c : want_center) {
        if (gsr::gs_center_sample(c[0]) == c[1]) continue;
        fail("centre sample of " + std::to_string(c[0]),
             std::to_string(gsr::gs_center_sample(c[0])), std::to_string(c[1]));
    }

    /* And the sizes the setting costs, which is what the log line reports. */
    if (gsr::gs_shadow_bytes(1) != 0 || gsr::gs_shadow_bytes(4) != 16u * 1024 * 1024
        || gsr::gs_shadow_bytes(8) != 32u * 1024 * 1024
        || gsr::gs_shadow_bytes(16) != 64u * 1024 * 1024) {
        fail("shadow size", std::to_string(gsr::gs_shadow_bytes(4)), "16 MiB at scale 4");
    }
    if (gsr::gs_scale_allowed(2) || !gsr::gs_scale_allowed(16)) {
        fail("the allowed scales are 1, 4, 8 and 16", "2 accepted", "2 rejected");
    }
}

/* ---- 10. the shadow's page state machine ----------------------------------- */

void test_shadow_pages() {
    gsr::ShadowPages sh;
    if (sh.active()) fail("a fresh shadow is inactive", "active", "inactive");
    sh.set_samples(4);
    if (!sh.active()) fail("scale 4 makes the shadow active", "inactive", "active");

    gsr::PageSet writes;
    writes.add(3);
    writes.add(7);
    std::vector<uint32_t> seed;
    sh.take_seed_list(writes, seed);
    if (seed.size() != 2 || seed[0] != 3 || seed[1] != 7) {
        fail("the first batch seeds both pages", std::to_string(seed.size()) + " pages",
             "3 and 7");
    }
    /* Already seeded: the second batch over the same pages seeds nothing. */
    sh.take_seed_list(writes, seed);
    if (!seed.empty()) {
        fail("a seeded page is not seeded again", std::to_string(seed.size()) + " pages",
             "none");
    }
    if (!sh.all_valid(writes)) fail("both pages are valid", "no", "yes");

    /* A native write over page 7 drops page 7 and nothing else. 2048 words to
     * a page, so page 7 is words 14336 to 16383. */
    sh.invalidate_words(7u * 2048u + 10u, 7u * 2048u + 20u);
    if (sh.all_valid(writes)) fail("a transfer drops the page it wrote", "still valid",
                                   "dropped");
    if (!sh.valid(3)) fail("a transfer drops only the pages it wrote", "3 dropped",
                           "3 valid");
    sh.take_seed_list(writes, seed);
    if (seed.size() != 1 || seed[0] != 7) {
        fail("the dropped page is seeded again", std::to_string(seed.size()) + " pages",
             "7");
    }

    /* A scale change drops the whole shadow: the plane count the addresses
     * are built from changed. */
    if (!sh.set_samples(8)) fail("a scale change drops the shadow", "kept", "dropped");
    if (sh.valid(3) || sh.valid(7)) {
        fail("nothing survives a scale change", "a page survived", "none");
    }
    /* The same scale twice is not a change. */
    if (sh.set_samples(8)) fail("setting the same scale drops nothing", "dropped", "kept");
}

/* ---- 7. the shader interface, and the dispatch grid ------------------------
 *
 * None of this needs a GPU, and all of it is what the renderer's first run on
 * hardware failed on if it were wrong: a push block that does not match the
 * shader reads the neighbouring field's value with no symptom but a wrong
 * picture, and a dispatch grid derived from a guest register is bounded by
 * nothing on the host side.
 */

/* The push blocks, against the field order shaders/raster.comp,
 * shaders/scanout.comp and shaders/shadow.comp declare. The sizes are
 * static_asserts in the headers, so a mismatch there does not build at all;
 * what is here is the offset of every field, which is the part a reordering
 * would break while the size stayed right. */
void test_push_layout() {
    struct Field { const char* name; size_t offset; size_t want; };

    const Field raster[] = {
        { "frame_base_block", offsetof(gsr::RasterPush, frame_base_block), 0 },
        { "frame_bw",         offsetof(gsr::RasterPush, frame_bw),         4 },
        { "frame_psm",        offsetof(gsr::RasterPush, frame_psm),        8 },
        { "frame_mask",       offsetof(gsr::RasterPush, frame_mask),      12 },
        { "z_base_block",     offsetof(gsr::RasterPush, z_base_block),    16 },
        { "z_psm",            offsetof(gsr::RasterPush, z_psm),           20 },
        { "z_write",          offsetof(gsr::RasterPush, z_write),         24 },
        { "tile_x0",          offsetof(gsr::RasterPush, tile_x0),         28 },
        { "tile_y0",          offsetof(gsr::RasterPush, tile_y0),         32 },
        { "fogcol",           offsetof(gsr::RasterPush, fogcol),          36 },
        { "dimx0",            offsetof(gsr::RasterPush, dimx0),           40 },
        { "dimx1",            offsetof(gsr::RasterPush, dimx1),           44 },
        { "samples",          offsetof(gsr::RasterPush, samples),         48 },
        { "tile_w",           offsetof(gsr::RasterPush, tile_w),          52 },
        { "tile_h",           offsetof(gsr::RasterPush, tile_h),          56 },
        { "shadow",           offsetof(gsr::RasterPush, shadow),          60 },
    };
    for (const Field& f : raster) {
        if (f.offset != f.want) {
            fail(std::string("RasterPush.") + f.name + " is where raster.comp declares it",
                 std::to_string(f.offset), std::to_string(f.want));
        }
    }

    const Field shadow[] = {
        { "mode",             offsetof(gsr::ShadowPush, mode),             0 },
        { "samples",          offsetof(gsr::ShadowPush, samples),          4 },
        { "frame_base_block", offsetof(gsr::ShadowPush, frame_base_block), 8 },
        { "frame_bw",         offsetof(gsr::ShadowPush, frame_bw),        12 },
        { "frame_psm",        offsetof(gsr::ShadowPush, frame_psm),       16 },
        { "frame_mask",       offsetof(gsr::ShadowPush, frame_mask),      20 },
        { "z_base_block",     offsetof(gsr::ShadowPush, z_base_block),    24 },
        { "z_psm",            offsetof(gsr::ShadowPush, z_psm),           28 },
        { "z_write",          offsetof(gsr::ShadowPush, z_write),         32 },
        { "x0",               offsetof(gsr::ShadowPush, x0),              36 },
        { "y0",               offsetof(gsr::ShadowPush, y0),              40 },
        { "x1",               offsetof(gsr::ShadowPush, x1),              44 },
        { "y1",               offsetof(gsr::ShadowPush, y1),              48 },
    };
    for (const Field& f : shadow) {
        if (f.offset != f.want) {
            fail(std::string("ShadowPush.") + f.name + " is where shadow.comp declares it",
                 std::to_string(f.offset), std::to_string(f.want));
        }
    }

    /* One record is GSP_STRIDE words and every named word has to be inside
     * it. The shader indexes the record by these names against a storage
     * buffer bound to exactly prim_count * GSP_STRIDE * 4 bytes, so a name
     * past the stride reads outside the binding. */
    if (GSP_Q2 >= GSP_STRIDE) {
        fail("the last record word is inside the stride", std::to_string(GSP_Q2),
             std::string("< ") + std::to_string(GSP_STRIDE));
    }
    /* 128 bytes is rhi.h's push constant budget. Named here as a number
     * rather than included, so this file keeps linking nothing but the
     * assembler; gs_native.cpp holds the static_assert against the RHI's own
     * constant. */
    const size_t kBudget = 128;
    if (sizeof(gsr::RasterPush) > kBudget || sizeof(gsr::ScanoutPush) > kBudget
        || sizeof(gsr::ShadowPush) > kBudget) {
        fail("every push block is inside the RHI's budget",
             std::to_string(sizeof(gsr::RasterPush)) + "/"
                 + std::to_string(sizeof(gsr::ScanoutPush)) + "/"
                 + std::to_string(sizeof(gsr::ShadowPush)),
             std::string("all <= ") + std::to_string(kBudget));
    }
}

/* The dispatch grid: what tile_grid() answers for a batch that drew nothing,
 * for an inverted scissor, and for the largest rectangle the drawing area
 * allows at every render scale. The last one is the number that has to stay
 * under the 65535 groups per axis both APIs guarantee. */
void test_dispatch_grid() {
    {   /* An empty batch has no tile grid at all, at every scale. Zero is
         * what gs_native.cpp's flush tests for before it dispatches; a stray
         * 1 here would dispatch a workgroup over a batch with no records and
         * read a bin table that build_bins never filled. */
        Harness h;
        for (uint32_t samples : { 1u, 4u, 8u, 16u }) {
            uint32_t tw_px = 0, th_px = 0;
            gsr::gs_scale_tile(samples, &tw_px, &th_px);
            uint32_t tx = 1, ty = 1, tw = 1, th = 1;
            h.draw.tile_grid(tw_px, th_px, &tx, &ty, &tw, &th);
            if (tw != 0 || th != 0) {
                fail("an empty batch has no tiles at scale " + std::to_string(samples),
                     std::to_string(tw) + "x" + std::to_string(th), "0x0");
            }
        }
    }
    {   /* An inverted scissor (SCAX0 > SCAX1) leaves nothing after the clip,
         * so the record is dropped and the batch stays empty. The division
         * that derives the grid is never reached with a negative width. */
        Harness h;
        h.regs.write(gsr::GS_REG_SCISSOR_1, 100ull | (10ull << 16) | (100ull << 32)
                                            | (10ull << 48));
        h.prim(6);
        h.xyz(20, 20, 0, true);
        h.xyz(60, 60, 0, true);
        if (h.count() != 0) {
            fail("an inverted scissor drops the primitive", std::to_string(h.count()),
                 "0");
        }
        uint32_t tx = 1, ty = 1, tw = 1, th = 1;
        h.draw.tile_grid(16, 16, &tx, &ty, &tw, &th);
        if (tw != 0 || th != 0) {
            fail("an inverted scissor leaves no tile grid",
                 std::to_string(tw) + "x" + std::to_string(th), "0x0");
        }
    }
    {   /* The whole drawing area, which is the largest grid any batch can
         * ask for: 2048 pixels over the smallest tile is 512 groups, and the
         * limit is 65535. A tile size that stopped dividing 2048 would show
         * up here as a count that does not match the closed form. */
        Harness h;
        h.prim(6);
        h.xyz(0, 0, 0, true);
        h.xyz(2047, 2047, 0, true);
        if (h.count() != 1) {
            fail("the full-area sprite is one record", std::to_string(h.count()), "1");
        }
        for (uint32_t samples : { 1u, 4u, 8u, 16u }) {
            uint32_t tw_px = 0, th_px = 0;
            gsr::gs_scale_tile(samples, &tw_px, &th_px);
            if (tw_px * th_px * samples != 256u) {
                fail("the tile fills one 256-thread workgroup at scale "
                         + std::to_string(samples),
                     std::to_string(tw_px * th_px * samples), "256");
            }
            uint32_t tx = 0, ty = 0, tw = 0, th = 0;
            h.draw.tile_grid(tw_px, th_px, &tx, &ty, &tw, &th);
            const uint32_t want_w = 2047u / tw_px + 1u;
            const uint32_t want_h = 2047u / th_px + 1u;
            if (tx != 0 || ty != 0 || tw != want_w || th != want_h) {
                fail("the full-area grid at scale " + std::to_string(samples),
                     std::to_string(tw) + "x" + std::to_string(th),
                     std::to_string(want_w) + "x" + std::to_string(want_h));
            }
            if (tw > 65535u || th > 65535u) {
                fail("the full-area grid is inside the API limit at scale "
                         + std::to_string(samples),
                     std::to_string(tw) + "x" + std::to_string(th), "<= 65535");
            }
        }
        /* The resolve pass's own grid, which gs_native.cpp computes as
         * (x1 - x0) / 8 + 1 over the same rectangle. 2048 pixels in blocks
         * of 8 is 256 groups, and one more for the inclusive end. */
        uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (!h.draw.pixel_grid(&x0, &y0, &x1, &y1)) {
            fail("the full-area batch has a pixel grid", "none", "0,0..2047,2047");
        } else {
            const uint32_t gx = (x1 - x0) / 8u + 1u;
            const uint32_t gy = (y1 - y0) / 8u + 1u;
            if (gx != 256u || gy != 256u) {
                fail("the resolve grid over the whole drawing area",
                     std::to_string(gx) + "x" + std::to_string(gy), "256x256");
            }
        }
    }
}

} // namespace

int main() {
    test_coverage();
    test_retention();
    test_packed_reglist_equivalence();
    test_z_plane();
    test_aa1_coverage();
    test_line_dda();
    test_scanmsk();
    test_alias();
    test_samples();
    test_shadow_pages();
    test_push_layout();
    test_dispatch_grid();
    if (g_failures) {
        std::printf("gs-raster-selftest: %d failures\n", g_failures);
        return 1;
    }
    std::printf("gs-raster-selftest: pass.\n");
    return 0;
}
