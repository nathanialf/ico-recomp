/* gs/render/gs_prim.h: the rasteriser's primitive record and the coverage
 * rules, shared verbatim by the C++ assembler and the GLSL fine pass.
 *
 * Ours (MIT). Written from the GS User's Manual's drawing chapters and this
 * repository's own register facts (src/runtime/hw/, gs_regs.h). No emulator
 * source was read for it.
 *
 * Written in the intersection of C++ and GLSL 450, the same discipline
 * gs_swizzle.h keeps: no templates, no references, no constexpr, no struct
 * methods. Define GS_PRIM_GLSL before including it from a shader. The one
 * place the two languages cannot share a body is 64-bit arithmetic, because
 * GLSL 450 has no 64-bit integer type without an optional extension; both
 * spellings are given below and both are exact, so they agree by
 * construction rather than by convention.
 *
 * ---- the coordinate system and the sample position ------------------------
 *
 * A vertex arrives as XYZ2/XYZF2 with X and Y as unsigned 12.4 fixed point.
 * The renderer subtracts XYOFFSET first, so everything in a record is a
 * signed 16.4 window coordinate: one unit is 1/16 of a pixel, and the whole
 * range a vertex can name is plus or minus 65535 of those units.
 *
 * A pixel is sampled once, at its centre. The centre of pixel (px, py) is
 * (px * 16 + 8, py * 16 + 8) in those units. Everything below follows from
 * that one choice:
 *
 *   - a sprite from x0 to x1 covers pixel px when x0 <= px*16+8 < x1, which
 *     is the manual's rule that the left and top edges are inclusive and the
 *     right and bottom edges are exclusive;
 *   - a triangle covers a pixel when the sample point is inside all three
 *     edges, with the top-left rule below deciding the ties;
 *   - a point covers the pixel that contains it, floor(x/16);
 *   - a line covers, for each pixel of its major axis, the one minor pixel
 *     the interpolated minor coordinate falls in.
 *
 * The +8 is a decision, not a measurement. It is the choice that makes the
 * sprite rule and the triangle rule agree at integer coordinates, which is
 * where this game's own geometry sits: a sprite from 0.0 to 16.0 and a
 * triangle with vertices at 0.0 and 16.0 both cover pixels 0 through 15. A
 * corner rule (sampling at px*16) would agree there too and would differ for
 * a primitive at a fractional position, so the two rules are separated only
 * by a sub-pixel case; docs/GS_RENDERER.md lists this among the things one
 * measured fractional-position sprite would settle.
 *
 * ---- the top-left rule ----------------------------------------------------
 *
 * The assembler orders the three vertices so the signed area is positive,
 * which makes the interior of every edge the positive side of that edge's
 * function
 *
 *   E(P) = (Bx - Ax) * (Py - Ay) - (By - Ay) * (Px - Ax)
 *
 * Window coordinates run down the screen, so with a positive area a left
 * edge runs upwards (dy < 0) and a top edge is horizontal with the interior
 * below it (dy == 0 and dx > 0). A sample point exactly on an edge is drawn
 * only when that edge is a left or a top edge, so two triangles sharing an
 * edge cover each shared pixel exactly once.
 *
 * That last sentence is this renderer's choice and not the manual's. The
 * manual gives the drawing kick and the sample position; it does not state a
 * fill rule in those words, and bottom-right would tile just as exactly.
 * docs/GS_RENDERER.md lists it as inferred beside the sample position, which
 * is the same capture: one measured primitive whose edge lands on a sample
 * point. gs_raster_selftest.cpp's shared-edge pair proves the tiling and
 * cannot tell the two rules apart.
 *
 * E is computed exactly. The products are up to 2^17 by 2^17, which does not
 * fit a 32-bit int, so both languages do it in 64 bits: int64_t in C++, and
 * imulExtended (core GLSL 450, OpSMulExtended, no extension and no optional
 * device feature) in the shader.
 */
#ifndef ICORECOMP_GS_PRIM_H
#define ICORECOMP_GS_PRIM_H

#ifdef GS_PRIM_GLSL
#ifndef GS_SWIZZLE_GLSL
#define GS_SWIZZLE_GLSL
#endif
#include "gs_swizzle.h"
#else
#include "gs_swizzle.h"
#include <cmath>
#include <cstdint>
namespace gsr {
#endif

/* ---- the record ------------------------------------------------------------
 *
 * One primitive is GSP_STRIDE consecutive 32-bit words in a storage buffer.
 * The words are all plain uints on the GLSL side; the ones that hold signed
 * quantities are bit-cast to int where they are read. A flat array rather
 * than a struct because std430 struct padding is one more thing that can
 * differ between the two compilers, and because the fine pass reads a
 * different subset per primitive kind.
 *
 * Coordinates in words 1 to 6 are signed 16.4 window coordinates. For a
 * triangle they are the three vertices, already wound so the area is
 * positive. For a sprite they are the two corners, already sorted so
 * x0 < x1 and y0 < y1, and words 5 and 6 are unused. For a line, words 1 to
 * 4 are the two endpoints and word 5 holds the minor coordinate at the
 * reference pixel in 16.16 pixels (see GSP_LINE_SLOPE). For a point only
 * words 1 and 2 are read.
 */
#define GSP_STRIDE       50u

#define GSP_FLAGS        0u   /* kind and the PRIM attribute bits, below */
#define GSP_X0           1u
#define GSP_Y0           2u
#define GSP_X1           3u
#define GSP_Y1           4u
#define GSP_X2           5u   /* line: minor coordinate at the reference pixel */
#define GSP_Y2           6u
#define GSP_REFX         7u   /* the clipped bounding box origin, in pixels */
#define GSP_REFY         8u
#define GSP_Z_I          9u   /* Z at the reference pixel centre, integer part */
#define GSP_Z_F         10u   /* and its 32-bit fraction */
#define GSP_ZDX_I       11u   /* dZ per pixel of x, integer part (two's complement) */
#define GSP_ZDX_F       12u
#define GSP_ZDY_I       13u
#define GSP_ZDY_F       14u
#define GSP_RGBA0       15u   /* 0xAABBGGRR, matching X0/Y0 after the winding swap */
#define GSP_RGBA1       16u
#define GSP_RGBA2       17u
#define GSP_RGBA_FLAT   18u   /* the provoking vertex's colour, swap or no swap */
#define GSP_FOG012      19u   /* f0 | f1<<8 | f2<<16 */
#define GSP_FOG_FLAT    20u
#define GSP_SCISSOR_X   21u   /* SCAX0 | SCAX1<<16, both inclusive */
#define GSP_SCISSOR_Y   22u   /* SCAY0 | SCAY1<<16 */
#define GSP_TEST        23u   /* the TEST register's low 19 bits, verbatim */
#define GSP_ALPHA       24u   /* see the GSP_A_* accessors */
#define GSP_LINE_SLOPE  25u   /* line: minor pixels per major pixel, 16.16 */

/* ---- the texture words -----------------------------------------------------
 *
 * Milestone (c). Everything the texture unit reads travels per primitive,
 * because TEX0, TEX1, CLAMP, TEXA and the mip bases can all change between
 * two primitives of one batch and none of them takes part in the batch key
 * (gs_draw.h says what that key is and why). Twenty-four words per record is
 * what that costs: 200 bytes a primitive, 6.5 MiB at the 32768-primitive
 * batch limit, against the 3.4 MiB milestone (b) used.
 *
 * The registers are carried as their raw 64-bit values split into halves,
 * and gs_texture.h holds the one set of field accessors both languages use,
 * so no field position is written twice.
 *
 * GSP_CLUT_BASE is a word index into the batch's CLUT table, not a CLUT
 * value: the CLUT is 1 KB of state that changes far less often than a
 * primitive, so the batch carries the distinct snapshots and each record
 * names the one it was assembled under. gs_draw.cpp builds the table.
 *
 * The coordinates are per vertex and follow the winding swap, so GSP_UV0
 * belongs to the vertex at GSP_X0. FST 1 uses the UV words, each holding
 * U in bits 0..15 and V in bits 16..31 as the register's own 12.4 fixed
 * point; FST 0 uses the S, T and Q words, which are float bit patterns.
 * Sprites use indices 0 and 1 for their two corners and share vertex 1's Q,
 * which is the same vertex the sprite takes its colour and Z from.
 */
#define GSP_TEX0_LO     26u
#define GSP_TEX0_HI     27u
#define GSP_TEX1_LO     28u
#define GSP_TEX1_HI     29u
#define GSP_CLAMP_LO    30u
#define GSP_CLAMP_HI    31u
#define GSP_TEXA        32u   /* packed by gs_texa_pack(): TA0, AEM, TA1 */
#define GSP_CLUT_BASE   33u   /* word index of this record's CLUT snapshot */
#define GSP_MIPTBP1_LO  34u
#define GSP_MIPTBP1_HI  35u
#define GSP_MIPTBP2_LO  36u
#define GSP_MIPTBP2_HI  37u
#define GSP_UV0         38u   /* U | V << 16, 12.4 texels, FST 1 */
#define GSP_UV1         39u
#define GSP_UV2         40u
#define GSP_S0          41u   /* float bit patterns, FST 0 */
#define GSP_T0          42u
#define GSP_Q0          43u
#define GSP_S1          44u
#define GSP_T1          45u
#define GSP_Q1          46u
#define GSP_S2          47u
#define GSP_T2          48u
#define GSP_Q2          49u

/* Word GSP_FLAGS. */
#define GSP_KIND_MASK    7u
#define GSP_KIND_POINT    0u
#define GSP_KIND_LINE     1u
#define GSP_KIND_SPRITE   2u
#define GSP_KIND_TRIANGLE 3u

#define GSP_F_MAJOR_X   (1u << 3)  /* line: the major axis is x */
#define GSP_F_IIP       (1u << 4)  /* Gouraud; flat when clear */
#define GSP_F_TME       (1u << 5)  /* textured */
#define GSP_F_FGE       (1u << 6)
#define GSP_F_ABE       (1u << 7)
#define GSP_F_AA1       (1u << 8)  /* edge coverage becomes the blend alpha */
#define GSP_F_FST       (1u << 9)  /* texture coordinates come from UV, not STQ */
/* A line whose two endpoints fell inside one pixel of its major axis. The
 * DDA emits a pixel before it steps, so the primitive covers the one pixel
 * the first vertex falls in and nothing else. Inferred; gs_covers_line_dot
 * below states it. */
#define GSP_F_LINE_DOT  (1u << 10)
/* SCANMSK MSK, the register's own two bits: 0 no mask, 1 reserved, 2 draws
 * only even raster lines, 3 draws only odd ones. It is a global register
 * rather than a context one and it is not part of the batch key, so it
 * travels per primitive like the rest of the pixel pipeline's switches. */
#define GSP_F_SCANMSK_SHIFT 11u
#define GSP_F_SCANMSK_MASK   3u

/* Word GSP_ALPHA. A, B, C and D are the ALPHA register's selectors, FIX its
 * fixed factor; the rest are the one-bit registers that belong to the pixel
 * pipeline and can change between primitives inside one batch. */
#define GSP_A_A_SHIFT     0u
#define GSP_A_B_SHIFT     2u
#define GSP_A_C_SHIFT     4u
#define GSP_A_D_SHIFT     6u
#define GSP_A_FIX_SHIFT   8u
#define GSP_A_COLCLAMP  (1u << 16)
#define GSP_A_PABE      (1u << 17)
#define GSP_A_FBA       (1u << 18)
#define GSP_A_DTHE      (1u << 19)

/* Word GSP_TEST, which is the TEST register's own layout. */
#define GSP_T_ATE       (1u << 0)
#define GSP_T_ATST_SHIFT  1u
#define GSP_T_AREF_SHIFT  4u
#define GSP_T_AFAIL_SHIFT 12u
#define GSP_T_DATE      (1u << 14)
#define GSP_T_DATM      (1u << 15)
#define GSP_T_ZTE       (1u << 16)
#define GSP_T_ZTST_SHIFT 17u

/* ATST, AFAIL and ZTST codes, the manual's. */
#define GSP_ATST_NEVER    0u
#define GSP_ATST_ALWAYS   1u
#define GSP_ATST_LESS     2u
#define GSP_ATST_LEQUAL   3u
#define GSP_ATST_EQUAL    4u
#define GSP_ATST_GEQUAL   5u
#define GSP_ATST_GREATER  6u
#define GSP_ATST_NOTEQUAL 7u

#define GSP_AFAIL_KEEP     0u
#define GSP_AFAIL_FB_ONLY  1u
#define GSP_AFAIL_ZB_ONLY  2u
#define GSP_AFAIL_RGB_ONLY 3u

#define GSP_ZTST_NEVER   0u
#define GSP_ZTST_ALWAYS  1u
#define GSP_ZTST_GEQUAL  2u
#define GSP_ZTST_GREATER 3u

/* ---- the binning grid ------------------------------------------------------
 *
 * The GS drawing area is 2048 by 2048 pixels, so a 64-pixel coarse bin grid
 * is 32 by 32 bins. The bin size is fixed rather than derived from the frame
 * buffer, so the bin range table is one allocation for the life of the
 * renderer.
 *
 * The fine tile is 16 pixels square at render scale 1 and smaller above it,
 * because a workgroup is 256 threads and one thread owns one sample
 * (gs_shadow.h's gs_scale_tile). Every tile size divides the bin, so a tile
 * always lies inside one bin and the fine pass finds its bin from the pixel
 * coordinate.
 */
#define GSP_BIN_PIXELS   64u
#define GSP_TILE_PIXELS  16u
#define GSP_BINS_X       32u
#define GSP_BINS_Y       32u
#define GSP_BIN_COUNT   1024u

/* ---- exact 64-bit edge arithmetic ------------------------------------------
 *
 * gs_cross64(a, b, c, d) is the exact value of a*b - c*d for 32-bit signed
 * inputs. The C++ spelling is int64_t; the GLSL spelling is a two-word value
 * built with imulExtended, which is core GLSL 450 and needs neither an
 * extension nor the shaderInt64 device feature the RHI deliberately does not
 * require (rhi/rhi.h). Both are exact, so they cannot disagree.
 */
#ifdef GS_PRIM_GLSL

#define gs_i64 ivec2   /* (low 32 bits as a bit pattern, high 32 bits) */

GS_FN gs_i64 gs_cross64(int a, int b, int c, int d) {
    int hi1, lo1, hi2, lo2;
    imulExtended(a, b, hi1, lo1);
    imulExtended(c, d, hi2, lo2);
    uint u1 = uint(lo1);
    uint u2 = uint(lo2);
    uint lo = u1 - u2;
    int borrow = (u1 < u2) ? 1 : 0;
    int hi = hi1 - hi2 - borrow;
    return ivec2(int(lo), hi);
}

GS_FN int gs_i64_sign(gs_i64 v) {
    if (v.y < 0) return -1;
    if (v.y > 0) return 1;
    return (uint(v.x) != 0u) ? 1 : 0;
}

/* The magnitude as a float, for the barycentric weights. float has 24 bits
 * of mantissa against the value's 35, so a weight carries a relative error
 * near 2^-24; on an 8-bit colour channel that is under 1e-4 of one step. Z
 * does not go through here for exactly that reason: it is 32 bits wide and
 * uses the fixed-point DDA below instead. */
GS_FN float gs_i64_float(gs_i64 v) {
    return float(v.y) * 4294967296.0 + float(uint(v.x));
}

#else

#define gs_i64 int64_t

GS_FN gs_i64 gs_cross64(int a, int b, int c, int d) {
    return (int64_t)a * (int64_t)b - (int64_t)c * (int64_t)d;
}

GS_FN int gs_i64_sign(gs_i64 v) {
    return v > 0 ? 1 : (v < 0 ? -1 : 0);
}

/* The same magnitude the GLSL half gives, for the callers below that want a
 * float: the AA1 coverage filter and the barycentric weights. */
GS_FN float gs_i64_float(gs_i64 v) {
    return (float)(double)v;
}

#endif

/* ---- sub-sample positions ---------------------------------------------------
 *
 * display.render_scale N puts N sample points inside one pixel instead of
 * one. The set of positions is the pixel's own regular subdivision, each
 * sample at the centre of its sub-cell:
 *
 *   N = 1    one sample at the pixel centre, (8, 8). Scale 1 is exactly the
 *            rule above and nothing below changes it.
 *   N = 4    a 2 by 2 grid, offsets 4 and 12 on each axis.
 *   N = 8    a 4 by 2 grid, offsets 2, 6, 10 and 14 across and 4 and 12 down.
 *   N = 16   a 4 by 4 grid, offsets 2, 6, 10 and 14 on each axis.
 *
 * Sample index s runs across first and then down, so s = row * cols + col.
 * An ordered grid rather than a rotated or jittered one because it is the
 * pattern a resolve can average with equal weights and the pattern the
 * high-resolution scanout can read as quadrants; a rotated grid gives better
 * edges on near-horizontal and near-vertical lines and would be a change to
 * these two functions and nothing else. Every axis gains samples at 4, 8 and
 * 16, which is what the high-resolution scanout needs and what keeps 2 out of
 * the allowed set (docs/SETTINGS.md section 6).
 */
#define GSP_MAX_SAMPLES 16u

GS_FN uint gs_sample_cols(uint samples) {
    if (samples >= 8u) return 4u;
    if (samples >= 4u) return 2u;
    return 1u;
}

GS_FN uint gs_sample_rows(uint samples) {
    return samples / gs_sample_cols(samples);
}

/* Sixteenths of a pixel, so these are added straight onto px * 16. */
GS_FN int gs_sample_x(uint samples, uint s) {
    uint cols = gs_sample_cols(samples);
    return int(((s % cols) * 2u + 1u) * 8u / cols);
}

GS_FN int gs_sample_y(uint samples, uint s) {
    uint cols = gs_sample_cols(samples);
    uint rows = samples / cols;
    return int((((s / cols) % rows) * 2u + 1u) * 8u / rows);
}

/* The sample nearest the pixel centre, with the lowest index winning a tie.
 * The resolve to native local memory takes depth from this one: a depth is
 * not a quantity an average has a meaning for, and the value the hardware
 * would have held is the one at the centre. Gives 0 for N = 1 and N = 4,
 * 1 for N = 8 and 5 for N = 16. */
GS_FN uint gs_center_sample(uint samples) {
    uint best = 0u;
    int best_d = 1 << 30;
    for (uint s = 0u; s < samples; ++s) {
        int dx = gs_sample_x(samples, s) - 8;
        int dy = gs_sample_y(samples, s) - 8;
        int d = dx * dx + dy * dy;
        if (d < best_d) {
            best_d = d;
            best = s;
        }
    }
    return best;
}

/* ---- AA1 edge coverage ------------------------------------------------------
 *
 * PRIM AA1 turns on the GS's antialiasing, which the manual describes for the
 * line and triangle families (not for points and not for sprites) as: the
 * coverage of a pixel on the edge of the primitive becomes that pixel's alpha
 * value, so that with alpha blending on, an edge pixel is mixed into what was
 * already there in proportion to how much of it the primitive covers. An
 * interior pixel has full coverage, which on the GS's alpha scale is 0x80,
 * and is therefore unchanged.
 *
 * The rule implemented here, stated exactly:
 *
 *   coverage(pixel) = min over the primitive's edges of the area of that
 *   pixel's unit square lying on the interior side of the edge.
 *
 * The per-edge area is the exact closed form for a half plane clipped to a
 * unit square, below. Its limits, and they are limits of this rule and not of
 * the code:
 *
 *   - it is exact for a pixel that one edge alone cuts, which is every pixel
 *     of an edge away from the primitive's corners;
 *   - at a corner, where two edges cut the same pixel, the true covered area
 *     is smaller than either half plane's, so the minimum over-estimates.
 *     Reproducing the intersection exactly means clipping a polygon per
 *     pixel, which is not what a rasteriser's coverage unit does;
 *   - it says nothing about how the hardware quantises coverage. This
 *     rounds to the nearest of the 129 values 0 to 0x80. One captured frame
 *     with an AA1 edge over a flat background settles both that and the
 *     corner question.
 *
 * The split between what coverage replaces and what it does not is also
 * inferred: here it replaces the source alpha as the blend unit's C selector
 * reads it, and leaves the alpha test, the destination alpha test, FBA and
 * the alpha actually written to the frame buffer on the primitive's own
 * alpha. docs/GS_RENDERER.md lists it.
 */
#ifdef GS_PRIM_GLSL
#define GS_ABSF  abs
#define GS_MAXF  max
#define GS_MINF  min
#define GS_SQRTF sqrt
#else
#define GS_ABSF(x)  ((x) < 0.0f ? -(x) : (x))
#define GS_MAXF(a, b) ((a) > (b) ? (a) : (b))
#define GS_MINF(a, b) ((a) < (b) ? (a) : (b))
#define GS_SQRTF(x) ((float)std::sqrt((double)(x)))
#endif

/* The area of the unit square around a sample point that lies on the positive
 * side of one edge. `e` is that edge's function value at the sample point (the
 * same value coverage already computed) and dx, dy the edge vector in
 * sixteenths of a pixel.
 *
 * E is twice the area of the triangle the edge makes with the sample point in
 * (1/16 pixel) squared units, so the signed distance from the sample point to
 * the edge, in whole pixels, is E / (16 * |edge|). With the edge's unit normal
 * written as (a, b) with a = |dy| / |edge| and b = |dx| / |edge|, the square's
 * half width along that normal is (a + b) / 2, and the covered fraction is
 * piecewise: linear while the edge crosses two opposite sides of the square,
 * and one minus a triangular corner once it crosses two adjacent ones. */
GS_FN float gs_aa_edge_cover(float e, int dx, int dy) {
    float fdx = float(dx);
    float fdy = float(dy);
    float len = GS_SQRTF(fdx * fdx + fdy * fdy);
    if (len == 0.0) return 1.0;   /* a zero-length edge cuts nothing */
    float d = e / (16.0 * len);
    float a = GS_ABSF(fdy) / len;
    float b = GS_ABSF(fdx) / len;
    float amax = GS_MAXF(a, b);
    float amin = GS_MINF(a, b);
    float reach = 0.5 * (amax + amin);   /* the square's half width along the normal */
    float slab = 0.5 * (amax - amin);    /* while the edge crosses opposite sides */
    float ad = GS_ABSF(d);
    if (ad >= reach) return d > 0.0 ? 1.0 : 0.0;
    if (ad <= slab) return 0.5 + d / amax;
    float t = reach - ad;
    float corner = t * t / (2.0 * amax * amin);
    return d > 0.0 ? (1.0 - corner) : corner;
}

/* The coverage of a pixel by a whole triangle: the smallest of its three
 * edges' half-plane areas, at the given sample point. Zero means the pixel is
 * outside the triangle altogether. */
GS_FN float gs_aa_triangle_cover(int x0, int y0, int x1, int y1, int x2, int y2,
                                 int sx, int sy) {
    float a = gs_aa_edge_cover(gs_i64_float(gs_cross64(x1 - x0, sy - y0, y1 - y0, sx - x0)),
                               x1 - x0, y1 - y0);
    float b = gs_aa_edge_cover(gs_i64_float(gs_cross64(x2 - x1, sy - y1, y2 - y1, sx - x1)),
                               x2 - x1, y2 - y1);
    float c = gs_aa_edge_cover(gs_i64_float(gs_cross64(x0 - x2, sy - y2, y0 - y2, sx - x2)),
                               x0 - x2, y0 - y2);
    return GS_MINF(GS_MINF(a, b), c);
}

/* A coverage fraction as the alpha the blend unit reads, where 0x80 is one.
 * Full coverage gives exactly 0x80, so an interior pixel blends exactly as it
 * would without AA1. */
GS_FN uint gs_aa_alpha(float cover) {
    int a = int(cover * 128.0 + 0.5);
    if (a < 0) a = 0;
    if (a > 128) a = 128;
    return uint(a);
}

/* ---- coverage --------------------------------------------------------------
 *
 * Two forms of each rule. The `_at` form takes the sample point in 16.4
 * window coordinates, which is what a sub-sample needs; the plain form takes
 * the pixel index and forms the centre sample itself, so the +8 is written
 * down in exactly one place and scale 1 goes through the same arithmetic it
 * always did.
 */

/* The tie rule, written once. With a positive area and window coordinates
 * running down the screen, a left edge runs upwards and a top edge is
 * horizontal with the interior below it. Inferred, not from the manual; see
 * the top-left rule note at the head of this file. */
GS_FN bool gs_edge_tie(int dx, int dy) {
    return (dy < 0) || (dy == 0 && dx > 0);
}

/* One edge of a positively wound triangle, from its already computed edge
 * value. The fine pass calls this form because it needs the magnitudes for
 * the barycentric weights and would otherwise compute each edge twice. */
GS_FN bool gs_edge_accept_value(gs_i64 e, int dx, int dy) {
    int s = gs_i64_sign(e);
    if (s > 0) return true;
    if (s < 0) return false;
    return gs_edge_tie(dx, dy);
}

/* True when the sample point is strictly inside the edge, or on it and the
 * edge is a left or a top edge. */
GS_FN bool gs_edge_accept(int ax, int ay, int bx, int by, int sx, int sy) {
    int dx = bx - ax;
    int dy = by - ay;
    return gs_edge_accept_value(gs_cross64(dx, sy - ay, dy, sx - ax), dx, dy);
}

GS_FN bool gs_covers_triangle_at(int x0, int y0, int x1, int y1, int x2, int y2,
                                 int sx, int sy) {
    return gs_edge_accept(x0, y0, x1, y1, sx, sy)
        && gs_edge_accept(x1, y1, x2, y2, sx, sy)
        && gs_edge_accept(x2, y2, x0, y0, sx, sy);
}

GS_FN bool gs_covers_triangle(int x0, int y0, int x1, int y1, int x2, int y2,
                              int px, int py) {
    return gs_covers_triangle_at(x0, y0, x1, y1, x2, y2, px * 16 + 8, py * 16 + 8);
}

/* x0 < x1 and y0 < y1, sorted by the assembler. */
GS_FN bool gs_covers_sprite_at(int x0, int y0, int x1, int y1, int sx, int sy) {
    return sx >= x0 && sx < x1 && sy >= y0 && sy < y1;
}

GS_FN bool gs_covers_sprite(int x0, int y0, int x1, int y1, int px, int py) {
    return gs_covers_sprite_at(x0, y0, x1, y1, px * 16 + 8, py * 16 + 8);
}

GS_FN bool gs_covers_point(int x0, int y0, int px, int py) {
    return (x0 >> 4) == px && (y0 >> 4) == py;
}

/* The line model, and the fact that it is inferred rather than measured.
 *
 * The manual states that a line is drawn by a DDA along its longer axis; it
 * does not state the rounding. What is implemented here is: the major axis
 * is covered by the same sample rule a sprite uses, so major pixel p is on
 * the line when p*16+8 lies in [min, max) of the two endpoints' major
 * coordinates; and for each such p the minor coordinate is the linear
 * interpolation evaluated at that sample point, and the pixel drawn is the
 * one that value falls in. `minor_ref` is that interpolation at the
 * reference pixel and `slope` its step per major pixel, both 16.16 in whole
 * pixels, both computed on the CPU where the division is available.
 *
 * The endpoints: the span test is [min, max), so the far endpoint is
 * exclusive when it falls exactly on a sample point and the near one is
 * inclusive, which is the same convention the sprite rule states. The manual
 * states it for a rectangle and not for a line, so the choice is inferred.
 *
 * A line whose two endpoints fall inside one pixel of the major axis has an
 * empty span. It is not dropped: the assembler marks it GSP_F_LINE_DOT and it
 * covers the pixel its first vertex falls in, which is what a DDA that emits
 * before it steps does. gs_covers_line_dot states that, and it is inferred
 * too. */
GS_FN bool gs_covers_line_span(int mj0, int mj1, int p) {
    int sp = p * 16 + 8;
    return sp >= mj0 && sp < mj1;
}

/* The minor coordinate at major pixel p, in 16.16 whole pixels. The DDA steps
 * one whole pixel of the major axis whatever the render scale is, because one
 * pixel per step is what the hardware's DDA does; super-sampling refines the
 * minor axis only, which is the axis the model had a choice about. */
GS_FN int gs_line_minor(int minor_ref, int slope, int refp, int p) {
    return minor_ref + slope * (p - refp);
}

/* The signed distance from the sample point to the line along the minor axis,
 * in 16.16 pixels. `smin` is the sample's minor coordinate in 16.4. */
GS_FN int gs_line_offset(int minor_ref, int slope, int refp, int p, int smin) {
    return int(uint(gs_line_minor(minor_ref, slope, refp, p)) - uint(smin) * 4096u);
}

/* Covered when the sample point is within half a pixel of the line along the
 * minor axis. At the pixel centre (smin = q * 16 + 8) that is exactly the
 * older rule, (m >> 16) == q, including its treatment of a value that lands
 * on the pixel's own boundary. */
GS_FN bool gs_covers_line_at(int mj0, int mj1, int minor_ref, int slope, int refp,
                             int p, int smin) {
    if (!gs_covers_line_span(mj0, mj1, p)) return false;
    int d = gs_line_offset(minor_ref, slope, refp, p, smin);
    return d >= -32768 && d < 32768;
}

GS_FN bool gs_covers_line(int mj0, int mj1, int minor_ref, int slope, int refp,
                          int p, int q) {
    return gs_covers_line_at(mj0, mj1, minor_ref, slope, refp, p, q * 16 + 8);
}

/* AA1 on a line. The coverage is the tent a one-pixel-wide DDA line leaves on
 * the minor axis: full where the line passes through the sample point, zero a
 * whole pixel away, so at most two minor pixels of each major step are drawn
 * and their coverages sum to one. Inferred, like the rest of the line model:
 * the manual gives neither the width the hardware antialiases a line at nor
 * the shape of the falloff. */
GS_FN float gs_aa_line_cover(int minor_ref, int slope, int refp, int p, int smin) {
    int d = gs_line_offset(minor_ref, slope, refp, p, smin);
    if (d < 0) d = -d;
    if (d >= 65536) return 0.0;
    return 1.0 - float(d) / 65536.0;
}

/* A line whose two endpoints fell inside one pixel of its major axis. The
 * assembler puts that pixel in X0/Y0 and sets GSP_F_LINE_DOT; the whole
 * primitive is that one pixel, at parameter zero along the line.
 *
 * Inferred. A DDA that emits its current pixel and then steps draws one pixel
 * for a segment that never leaves the pixel it started in, which is the model
 * here; a DDA that steps first draws nothing, which is what milestone (b)
 * did. One captured frame with a sub-pixel line settles it, and ICO draws
 * very few lines. */
GS_FN bool gs_covers_line_dot(int x0, int y0, int px, int py) {
    return (x0 >> 4) == px && (y0 >> 4) == py;
}

/* ---- SCANMSK ---------------------------------------------------------------
 *
 * The manual's table: 0 no mask, 2 draws only pixels whose Y address is even,
 * 3 only those whose Y address is odd. 1 is reserved and is treated as no
 * mask, with one log line from the assembler. The Y address is the frame
 * buffer row, which is the window coordinate this renderer carries. */
GS_FN bool gs_scanmsk_draws(uint msk, int py) {
    if (msk < 2u) return true;
    return (uint(py) & 1u) == (msk & 1u);
}

/* ---- FRAME and ZBUF in the same memory --------------------------------------
 *
 * Both buffers are written through the same local memory, so a FRAME and a
 * ZBUF that name the same bits of the same word are one storage cell that two
 * halves of the pixel pipeline write. Two pixels of a 16-bit format share a
 * word without sharing bits, which is not this case and is already correct
 * through the masked atomics.
 *
 * True when the colour and the depth of one pixel occupy exactly the same
 * bits: the same addressing width and the same address. A partial overlap
 * (different widths at the same word) is not modelled per pixel and is named
 * once by the renderer. */
GS_FN bool gs_alias_pixel(uint frame_psm, uint frame_addr, uint z_psm, uint z_addr) {
    return gs_addr_bits(frame_psm) == gs_addr_bits(z_psm) && frame_addr == z_addr;
}

/* The inverse of gs_swizzle.h's gs_expand16: an 8-bit-per-channel colour back
 * into the 5-5-5-1 the 16-bit formats store. Exact against gs_expand16, which
 * shifts five bits up by three, so a value that came out of local memory goes
 * back into it unchanged. */
GS_FN uint gs_pack16(uint rgba) {
    return ((rgba & 0xFFu) >> 3)
         | ((((rgba >> 8) & 0xFFu) >> 3) << 5)
         | ((((rgba >> 16) & 0xFFu) >> 3) << 10)
         | ((((rgba >> 24) & 0xFFu) >> 7) << 15);
}

/* A colour as the bits the frame buffer stores, and back. The 32-bit family
 * keeps all 32, including a 24-bit buffer's top byte, which belongs to
 * whatever wrote it last. */
GS_FN uint gs_frame_pack(uint psm, uint rgba) {
    if (psm == GS_PSMCT16 || psm == GS_PSMCT16S) return gs_pack16(rgba);
    return rgba;
}

GS_FN uint gs_frame_unpack(uint psm, uint bits) {
    if (psm == GS_PSMCT16 || psm == GS_PSMCT16S) return gs_expand16(bits & 0xFFFFu);
    return bits;
}

/* The Z buffer's precision. A comparison and a store both happen at the width
 * of the format, so a 24-bit buffer never sees the top byte of a 32-bit Z the
 * vertex carried. */
GS_FN uint gs_z_precision_mask(uint psm) {
    if (psm == GS_PSMZ24) return 0x00FFFFFFu;
    if (psm == GS_PSMZ16 || psm == GS_PSMZ16S) return 0x0000FFFFu;
    return 0xFFFFFFFFu;
}

/* What an aliased word holds after one pixel of the pipeline has run over it:
 * the colour, and then the depth on top of it where the depth was written,
 * because the colour is written first and the depth second. Both views of the
 * word are read back out of the result with gs_frame_unpack and the Z mask.
 *
 * The order is inferred. The manual has one pixel operation write both
 * buffers and does not say in which order the two writes reach memory;
 * docs/GS_RENDERER.md lists it, and one captured frame with a frame buffer
 * and a Z buffer at the same address settles it. */
GS_FN uint gs_alias_word(uint frame_psm, uint rgba, uint z_psm, uint z, bool wrote_z) {
    uint bits = gs_frame_pack(frame_psm, rgba);
    if (!wrote_z) return bits;
    uint zm = gs_z_precision_mask(z_psm);
    return (bits & ~zm) | (z & zm);
}

/* ---- the Z DDA -------------------------------------------------------------
 *
 * Z is 32 bits wide, so it cannot go through the float barycentric weights
 * the colour channels use. It is carried as 32.32 fixed point instead: an
 * integer part and a 32-bit fraction at the reference pixel, and the same
 * pair for the step in x and in y. The reference pixel is the primitive's
 * clipped bounding box origin, so the pixel deltas the shader multiplies by
 * are never negative and the whole accumulation is unsigned.
 *
 * The step's integer part is stored as a two's complement bit pattern and
 * multiplied with wrapping 32-bit arithmetic, which is the right arithmetic:
 * Z is a 32-bit value and the hardware's own Z register wraps.
 *
 * Accuracy: the plane is solved in double on the CPU (53 bits of mantissa
 * against Z's 32), and the fixed-point step quantises at 2^-32 per pixel,
 * so over the 2048 pixels of the widest possible drawing area the error is
 * under 2^-21 of one Z unit.
 */
#ifdef GS_PRIM_GLSL
GS_FN uint gs_z_at(uint z_i, uint z_f, uint zdx_i, uint zdx_f,
                   uint zdy_i, uint zdy_f, uint dxp, uint dyp) {
    uint hx, lx, hy, ly, c;
    umulExtended(zdx_f, dxp, hx, lx);
    umulExtended(zdy_f, dyp, hy, ly);
    uint frac = z_f;
    uint carry = 0u;
    frac = uaddCarry(frac, lx, c); carry += c;
    frac = uaddCarry(frac, ly, c); carry += c;
    return z_i + zdx_i * dxp + zdy_i * dyp + hx + hy + carry;
}
#else
GS_FN uint gs_z_at(uint z_i, uint z_f, uint zdx_i, uint zdx_f,
                   uint zdy_i, uint zdy_f, uint dxp, uint dyp) {
    uint64_t acc = ((uint64_t)z_i << 32) | (uint64_t)z_f;
    acc += (((uint64_t)zdx_i << 32) | (uint64_t)zdx_f) * (uint64_t)dxp;
    acc += (((uint64_t)zdy_i << 32) | (uint64_t)zdy_f) * (uint64_t)dyp;
    return (uint)(acc >> 32);
}
#endif

#ifndef GS_PRIM_GLSL
} /* namespace gsr */
#endif

#endif /* ICORECOMP_GS_PRIM_H */
