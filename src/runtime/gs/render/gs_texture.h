/* gs/render/gs_texture.h: the texture unit's registers and its arithmetic,
 * shared verbatim by the C++ side and the GLSL fine pass.
 *
 * Ours (MIT). Field positions and formulas are the GS User's Manual's
 * texture chapters (TEX0/TEX1/TEX2, TEXCLUT, TEXA, CLAMP, MIPTBP1/2) and
 * nothing else. No emulator source was read for it.
 *
 * Same discipline as gs_prim.h and gs_swizzle.h: written in the intersection
 * of C++ and GLSL 450, no templates, no references, no constexpr, no struct
 * methods, every function scalar-returning, no 64-bit integers. Define
 * GS_TEXTURE_GLSL before including it from a shader.
 *
 * What is here and what is not. Everything in this file is a pure function
 * of register bits and texel values: the field decoders, the four CLAMP
 * modes, the CLUT slot arrangement, the TEXA expansion, the four TFX
 * functions, the LOD formula and the bilinear blend. The fetch itself is not
 * here, because it needs local memory: raster.comp does the fetch through
 * gs_swizzle.h and hands the values to these functions, and
 * gs_texture_selftest.cpp calls the same functions from the CPU with values
 * written out by hand from the manual.
 *
 * ---- the one texture coordinate representation ----------------------------
 *
 * Both coordinate paths reach the sampler as a signed integer in sixteenths
 * of a texel:
 *
 *   FST 1: the UV register is 14 bits per axis with 4 fractional bits, so it
 *          is already in this unit and is interpolated in it.
 *   FST 0: S, T and Q are interpolated per pixel and U = (S / Q) * 2^TW,
 *          V = (T / Q) * 2^TH, computed in 32-bit float and then rounded to
 *          the same sixteenth of a texel.
 *
 * One representation rather than two because every rule below (the clamp
 * modes' AND and OR, the bilinear neighbourhood, the mip shift) is an
 * integer rule on a texel coordinate, and having two widths would mean
 * writing each of them twice. The 4 fractional bits are what the UV register
 * carries; whether the hardware's STQ path carries more of them is not
 * stated by the manual and is not measured here, so it is listed as inferred
 * in docs/GS_RENDERER.md.
 */
#ifndef ICORECOMP_GS_TEXTURE_H
#define ICORECOMP_GS_TEXTURE_H

#ifdef GS_TEXTURE_GLSL
#ifndef GS_SWIZZLE_GLSL
#define GS_SWIZZLE_GLSL
#endif
#include "gs_swizzle.h"
#define GS_LOG2(x)  log2(x)
#define GS_FLOOR(x) floor(x)
#define GS_ABS(x)   abs(x)
#else
#include "gs_swizzle.h"
#include <cmath>
#include <cstdint>
namespace gsr {
#define GS_LOG2(x)  std::log2(x)
#define GS_FLOOR(x) std::floor(x)
#define GS_ABS(x)   std::fabs(x)
#endif

/* Written out rather than using each language's own, because C++'s
 * std::clamp needs a header and a namespace the GLSL side does not have. */
GS_FN int gs_iclamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- TEX0 ------------------------------------------------------------------
 *
 * The register is 64 bits and both languages hold it as two 32-bit words, so
 * every field that crosses bit 32 is assembled from both halves here rather
 * than at each use.
 *
 *   TBP0 0..13   TBW 14..19   PSM 20..25   TW 26..29   TH 30..33
 *   TCC 34       TFX 35..36   CBP 37..50   CPSM 51..54 CSM 55
 *   CSA 56..60   CLD 61..63
 */
/* Each accessor takes only the half or halves its field lives in, so no
 * parameter of any of them is unused in either language. */
GS_FN uint gs_tex0_tbp0(uint lo) { return lo & 0x3FFFu; }
GS_FN uint gs_tex0_tbw(uint lo)  { return (lo >> 14) & 0x3Fu; }
GS_FN uint gs_tex0_psm(uint lo)  { return (lo >> 20) & 0x3Fu; }
GS_FN uint gs_tex0_tw(uint lo)   { return (lo >> 26) & 0xFu; }
GS_FN uint gs_tex0_th(uint lo, uint hi) { return ((lo >> 30) & 3u) | ((hi & 3u) << 2); }
GS_FN uint gs_tex0_tcc(uint hi)  { return (hi >> 2) & 1u; }
GS_FN uint gs_tex0_tfx(uint hi)  { return (hi >> 3) & 3u; }
GS_FN uint gs_tex0_cbp(uint hi)  { return (hi >> 5) & 0x3FFFu; }
GS_FN uint gs_tex0_cpsm(uint hi) { return (hi >> 19) & 0xFu; }
GS_FN uint gs_tex0_csm(uint hi)  { return (hi >> 23) & 1u; }
GS_FN uint gs_tex0_csa(uint hi)  { return (hi >> 24) & 0x1Fu; }
GS_FN uint gs_tex0_cld(uint hi)  { return (hi >> 29) & 7u; }

/* TFX codes, the manual's. */
#define GS_TFX_MODULATE   0u
#define GS_TFX_DECAL      1u
#define GS_TFX_HIGHLIGHT  2u
#define GS_TFX_HIGHLIGHT2 3u

/* TEX2 writes only these fields of TEX0 and leaves the rest standing: PSM,
 * CBP, CPSM, CSM, CSA and CLD. The mask is written once here so the CPU side
 * and any later reader agree on which bits move. */
#define GS_TEX2_MASK_LO 0x03F00000u   /* PSM, bits 20..25 */
#define GS_TEX2_MASK_HI 0xFFFFFFE0u   /* CBP..CLD, bits 37..63 */

/* ---- TEX1 ------------------------------------------------------------------
 *
 *   LCM 0   MXL 2..4   MMAG 5   MMIN 6..8   MTBA 9   L 19..20   K 32..43
 *
 * K is 12 bits, signed, with 4 fractional bits, which is the same unit the
 * LOD below is carried in. */
GS_FN uint gs_tex1_lcm(uint lo)  { return lo & 1u; }
GS_FN uint gs_tex1_mxl(uint lo)  { return (lo >> 2) & 7u; }
GS_FN uint gs_tex1_mmag(uint lo) { return (lo >> 5) & 1u; }
GS_FN uint gs_tex1_mmin(uint lo) { return (lo >> 6) & 7u; }
GS_FN uint gs_tex1_mtba(uint lo) { return (lo >> 9) & 1u; }
GS_FN uint gs_tex1_l(uint lo)    { return (lo >> 19) & 3u; }
GS_FN int gs_tex1_k(uint hi) {
    int k = int(hi & 0xFFFu);
    return (k >= 2048) ? (k - 4096) : k;
}

/* MMIN codes. MMAG is the first two of them and nothing else. */
#define GS_FILT_NEAREST                0u
#define GS_FILT_LINEAR                 1u
#define GS_FILT_NEAREST_MIPMAP_NEAREST 2u
#define GS_FILT_NEAREST_MIPMAP_LINEAR  3u
#define GS_FILT_LINEAR_MIPMAP_NEAREST  4u
#define GS_FILT_LINEAR_MIPMAP_LINEAR   5u

/* ---- CLAMP -----------------------------------------------------------------
 *
 *   WMS 0..1  WMT 2..3  MINU 4..13  MAXU 14..23  MINV 24..33  MAXV 34..43
 */
GS_FN uint gs_clamp_wms(uint lo)  { return lo & 3u; }
GS_FN uint gs_clamp_wmt(uint lo)  { return (lo >> 2) & 3u; }
GS_FN uint gs_clamp_minu(uint lo) { return (lo >> 4) & 0x3FFu; }
GS_FN uint gs_clamp_maxu(uint lo) { return (lo >> 14) & 0x3FFu; }
GS_FN uint gs_clamp_minv(uint lo, uint hi) {
    return ((lo >> 24) & 0xFFu) | ((hi & 3u) << 8);
}
GS_FN uint gs_clamp_maxv(uint hi) { return (hi >> 2) & 0x3FFu; }

#define GS_WM_REPEAT        0u
#define GS_WM_CLAMP         1u
#define GS_WM_REGION_CLAMP  2u
#define GS_WM_REGION_REPEAT 3u

/* One axis of the wrap unit, on a whole texel coordinate, exactly the
 * manual's four formulas:
 *
 *   REPEAT         C = U & (size - 1)
 *   CLAMP          C = clamp(U, 0, size - 1)
 *   REGION_CLAMP   C = clamp(U, MIN, MAX)
 *   REGION_REPEAT  C = (U & MIN) | MAX
 *
 * REGION_REPEAT is the one that is not an interval: MIN is a bit mask and
 * MAX is a bit pattern OR'd in afterwards, which is how the hardware tiles a
 * sub-rectangle of a larger page without a divide. The AND is done on the
 * two's complement value so a negative coordinate wraps the same way REPEAT
 * does, and the result of every mode is then used as an unsigned texel
 * index.
 *
 * `size` is the texture's size on this axis at the mip level being sampled,
 * always a power of two. MIN and MAX are taken from the register unscaled:
 * whether the region bounds follow a mip level's halving is not stated by
 * the manual and is listed as inferred in docs/GS_RENDERER.md. */
GS_FN int gs_tex_wrap(int c, uint mode, uint minv, uint maxv, uint size) {
    if (mode == GS_WM_REPEAT) return c & int(size - 1u);
    if (mode == GS_WM_CLAMP) return gs_iclamp(c, 0, int(size) - 1);
    if (mode == GS_WM_REGION_CLAMP) return gs_iclamp(c, int(minv), int(maxv));
    return (c & int(minv)) | int(maxv);
}

/* ---- TEXA ------------------------------------------------------------------
 *
 * TEXA is carried through the primitive record as one packed word rather
 * than the register's two, because only three fields of it are read:
 *
 *   bits 0..7   TA0        bit 8   AEM        bits 16..23   TA1
 */
GS_FN uint gs_texa_pack(uint lo, uint hi) {
    return (lo & 0xFFu) | (((lo >> 15) & 1u) << 8) | ((hi & 0xFFu) << 16);
}
GS_FN uint gs_texa_ta0(uint texa) { return texa & 0xFFu; }
GS_FN uint gs_texa_aem(uint texa) { return (texa >> 8) & 1u; }
GS_FN uint gs_texa_ta1(uint texa) { return (texa >> 16) & 0xFFu; }

/* A 16-bit texel (A1B5G5R5) expanded to 8 bits per channel with TEXA's
 * alpha. The colour expansion is the same left shift by 3 gs_swizzle.h's
 * gs_expand16 uses; only the alpha differs, because a texture's alpha comes
 * from TEXA and not from the 0/128 rule a frame buffer read uses:
 *
 *   A = (the texel's A bit is 0) ? TA0 : TA1
 *   and when AEM is set and the texel is entirely zero, A = 0 instead.
 *
 * The AEM case is the manual's "alpha expansion" for a texture whose black
 * is meant to be transparent. */
GS_FN uint gs_texa_expand16(uint v16, uint texa) {
    uint r = (v16 & 0x1Fu) << 3;
    uint g = ((v16 >> 5) & 0x1Fu) << 3;
    uint b = ((v16 >> 10) & 0x1Fu) << 3;
    uint abit = (v16 >> 15) & 1u;
    uint a = (abit != 0u) ? gs_texa_ta1(texa) : gs_texa_ta0(texa);
    if (gs_texa_aem(texa) != 0u && abit == 0u && (v16 & 0x7FFFu) == 0u) a = 0u;
    return r | (g << 8) | (b << 16) | (a << 24);
}

/* A 24-bit texel: the colour is the low three bytes as they stand and the
 * alpha is TA0, with the same AEM rule on an all-zero colour. TA1 has no
 * part in a 24-bit expansion because there is no A bit to select it. */
GS_FN uint gs_texa_expand24(uint v24, uint texa) {
    uint rgb = v24 & 0x00FFFFFFu;
    uint a = gs_texa_ta0(texa);
    if (gs_texa_aem(texa) != 0u && rgb == 0u) a = 0u;
    return rgb | (a << 24);
}

/* ---- the CLUT --------------------------------------------------------------
 *
 * The CLUT buffer is 1 KB. This renderer holds it as 256 32-bit words and
 * addresses it in entries of the CLUT's own format: 256 entries when CPSM is
 * 32-bit, 512 half-word entries when it is 16-bit.
 *
 * CSM1 stores a 256-entry palette as a 16 by 16 arrangement, so the entry an
 * index names is not the slot at that index. Reading the manual's CLUT
 * diagram, the 16x16 grid is held as two 8-wide halves, which exchanges bits
 * 3 and 4 of the index: index 8 is at slot 16, index 16 at slot 8, and the
 * bits above and below are unmoved. That permutation is its own inverse, so
 * the same function serves a load and a lookup.
 *
 * This is the one rule in the file that is read off a diagram rather than
 * off a formula, and docs/GS_RENDERER.md lists it as inferred. One captured
 * 8-bit palette upload settles it: with the wrong rule a palette's colours
 * come out permuted in blocks of eight, which is loud rather than subtle.
 *
 * A 16-entry CSM1 palette (a 4-bit texture) has no bit 4 to exchange, so its
 * entries are consecutive and CSA picks which group of 16 they start at.
 * CSM2 is linear by definition and uses no permutation at all. */
#define GS_CLUT_WORDS 256u

GS_FN uint gs_clut_slot_csm1_8(uint index) {
    uint bit3 = (index >> 3) & 1u;
    uint bit4 = (index >> 4) & 1u;
    return (index & 0xE7u) | (bit3 << 4) | (bit4 << 3);
}

/* The CLUT entry a texel index names, in entries of the CLUT's own format.
 *
 *   4-bit texture   CSA selects the group of 16, so entry = CSA * 16 + index
 *   8-bit texture   the whole palette from entry 0; CSM1 permutes as above
 *
 * CSA is 5 bits, which is 32 groups of 16 entries: exactly the 512 half-word
 * entries of a 16-bit CLUT, and twice the 256 word entries of a 32-bit one.
 * A 32-bit CLUT therefore only reaches CSA 0..15, which is the manual's own
 * range for it.
 *
 * Inferred, not measured: that an 8-bit texture ignores CSA. Its 256 entries
 * fill a 32-bit CLUT exactly, leaving CSA nowhere to point, but they fill only
 * half of a 16-bit CLUT's 512 half-word entries, so with a 16-bit CPSM there
 * is a second place a palette could sit and CSA 16 would name it. The manual's
 * wording on that case is not settled here, so the choice is to start at entry
 * 0 and say so: docs/GS_RENDERER.md lists it, and gs_clut.cpp's load makes the
 * same choice at the writing end. One captured 8-bit texture with a 16-bit
 * CLUT at a nonzero CSA settles it, and gets loudly wrong colours if this is
 * the wrong rule. */
GS_FN uint gs_clut_entry(uint index, uint bits_per_index, uint csm, uint csa) {
    if (bits_per_index == 4u) return csa * 16u + (index & 0xFu);
    if (csm == 0u) return gs_clut_slot_csm1_8(index & 0xFFu);
    return index & 0xFFu;
}

/* ---- the texture function --------------------------------------------------
 *
 * TFX, from the manual's table, with Ct the texel, Cf the fragment's own
 * colour (the interpolated vertex colour) and every channel 8 bits. Alpha's
 * "one" on the GS is 0x80, which is why the products shift by 7 and not by
 * 8, and every result is clamped to 255.
 *
 *   MODULATE    Cv = (Ct * Cf) >> 7      Av = TCC ? (At * Af) >> 7 : Af
 *   DECAL       Cv = Ct                  Av = TCC ? At : Af
 *   HIGHLIGHT   Cv = (Ct * Cf) >> 7 + Af Av = TCC ? At + Af : Af
 *   HIGHLIGHT2  Cv = (Ct * Cf) >> 7 + Af Av = TCC ? At : Af
 *
 * The two HIGHLIGHT modes differ only in the alpha they produce, which is
 * the whole reason both exist: HIGHLIGHT2 keeps the texture's own alpha
 * while still adding the fragment's alpha into the colour.
 *
 * With TCC 0 the texel's alpha takes no part at all and the fragment's alpha
 * passes through, which is what "RGB only" means. */
GS_FN uint gs_tex_function(uint texel, uint frag, uint tfx, uint tcc) {
    uint af = (frag >> 24) & 0xFFu;
    uint at = (texel >> 24) & 0xFFu;
    uint out_rgb = 0u;
    uint i;
    for (i = 0u; i < 3u; ++i) {
        uint ct = (texel >> (i * 8u)) & 0xFFu;
        uint cf = (frag >> (i * 8u)) & 0xFFu;
        int v;
        if (tfx == GS_TFX_DECAL) {
            v = int(ct);
        } else {
            v = int((ct * cf) >> 7);
            if (tfx == GS_TFX_HIGHLIGHT || tfx == GS_TFX_HIGHLIGHT2) v += int(af);
        }
        out_rgb |= uint(gs_iclamp(v, 0, 255)) << (i * 8u);
    }
    uint out_a;
    if (tcc == 0u) {
        out_a = af;
    } else if (tfx == GS_TFX_MODULATE) {
        out_a = uint(gs_iclamp(int((at * af) >> 7), 0, 255));
    } else if (tfx == GS_TFX_HIGHLIGHT) {
        out_a = uint(gs_iclamp(int(at + af), 0, 255));
    } else {
        out_a = at; /* DECAL and HIGHLIGHT2 both keep the texel's alpha */
    }
    return out_rgb | (out_a << 24);
}

/* ---- LOD and the mip chain -------------------------------------------------
 *
 * The manual's formula, carried in sixteenths so it is the same unit as K:
 *
 *   LCM 0   LOD = (log2(1 / |Q|) << L) + K
 *   LCM 1   LOD = K
 *
 * The level a filter samples is LOD's integer part and the blend between two
 * levels is its fraction, which is what a fixed-point LOD field means; the
 * manual gives the formula and not the split, so docs/GS_RENDERER.md lists
 * the split as inferred. A negative LOD is magnification and selects MMAG.
 *
 * Q of zero or a denormal would send log2 to infinity. What the GS does there
 * is not stated, so this pins LOD at MXL, the last level this texture has,
 * and the picture stays on the smallest mip level rather than on a NaN. MXL
 * rather than a literal 6 because the caller clamps the level to MXL anyway
 * and a pin written twice can only drift; docs/GS_RENDERER.md lists the
 * substitution as inferred, because a shader cannot log it. */
GS_FN int gs_tex_lod(uint lcm, uint l, int k, float q, uint mxl) {
    if (lcm != 0u) return k;
    float aq = GS_ABS(q);
    if (!(aq > 1.0e-30)) return int(mxl) * 16;
    float lod = GS_LOG2(1.0 / aq) * float(1u << l);
    return int(GS_FLOOR(lod * 16.0)) + k;
}

/* Texture size at a mip level: the base size halved per level, never below
 * one texel, which is where a mip chain stops shrinking. */
GS_FN uint gs_tex_level_size(uint log2_size, uint level) {
    uint s = 1u << log2_size;
    s = s >> level;
    return (s == 0u) ? 1u : s;
}

/* MIPTBP1 holds levels 1..3 and MIPTBP2 levels 4..6, each as a 14-bit base
 * and a 6-bit width, at 20-bit spacing:
 *
 *   TBPn 0..13   TBWn 14..19   TBPn+1 20..33   TBWn+1 34..39
 *   TBPn+2 40..53  TBWn+2 54..59
 *
 * `which` is 0, 1 or 2 inside one register. The middle pair crosses bit 32,
 * which is the reason these are functions and not a pair of shifts at each
 * use. */
GS_FN uint gs_miptbp_tbp(uint lo, uint hi, uint which) {
    if (which == 0u) return lo & 0x3FFFu;
    if (which == 1u) return ((lo >> 20) & 0xFFFu) | ((hi & 3u) << 12);
    return (hi >> 8) & 0x3FFFu;
}
GS_FN uint gs_miptbp_tbw(uint lo, uint hi, uint which) {
    if (which == 0u) return (lo >> 14) & 0x3Fu;
    if (which == 1u) return (hi >> 2) & 0x3Fu;
    return (hi >> 22) & 0x3Fu;
}

/* Blocks one mip level occupies, which the automatic base computation below
 * needs. A level is `w` by `h` texels at the format's own bits per addressed
 * unit (gs_swizzle.h's gs_addr_bits: 32 for the 24-bit and the H formats,
 * because they occupy a whole word), and a block is 256 bytes. Never zero: a
 * level smaller than one block still starts the next one somewhere. */
GS_FN uint gs_tex_level_blocks(uint psm, uint log2w, uint log2h, uint level) {
    uint w = gs_tex_level_size(log2w, level);
    uint h = gs_tex_level_size(log2h, level);
    uint bytes = (w * h * gs_addr_bits(psm)) / 8u;
    uint blocks = bytes / GS_BLOCK_BYTES;
    return (blocks == 0u) ? 1u : blocks;
}

/* The base and the width of one mip level.
 *
 * MTBA 0 is the explicit case: levels 1 to 3 come from MIPTBP1 and levels 4
 * to 6 from MIPTBP2, each as its own TBP and TBW.
 *
 * MTBA 1 is the automatic case. The manual states that the bases are
 * computed from TBP0 for a texture whose levels follow one another in
 * memory, and does not write the arithmetic out. What is implemented is the
 * packing that description names: each level starts where the previous one
 * ended, and the width halves per level and never falls below one 64-texel
 * unit. docs/GS_RENDERER.md lists it as inferred. */
GS_FN uint gs_mip_tbp(uint tex0_lo, uint tex0_hi, uint tex1_lo,
                      uint m1lo, uint m1hi, uint m2lo, uint m2hi, uint level) {
    if (level == 0u) return gs_tex0_tbp0(tex0_lo);
    if (gs_tex1_mtba(tex1_lo) == 0u) {
        if (level <= 3u) return gs_miptbp_tbp(m1lo, m1hi, level - 1u);
        return gs_miptbp_tbp(m2lo, m2hi, level - 4u);
    }
    uint psm = gs_tex0_psm(tex0_lo);
    uint log2w = gs_tex0_tw(tex0_lo);
    uint log2h = gs_tex0_th(tex0_lo, tex0_hi);
    uint base = gs_tex0_tbp0(tex0_lo);
    uint n;
    for (n = 0u; n < level; ++n) base += gs_tex_level_blocks(psm, log2w, log2h, n);
    return base;
}

GS_FN uint gs_mip_tbw(uint tex0_lo, uint tex1_lo, uint m1lo, uint m1hi,
                      uint m2lo, uint m2hi, uint level) {
    if (level == 0u) return gs_tex0_tbw(tex0_lo);
    if (gs_tex1_mtba(tex1_lo) == 0u) {
        if (level <= 3u) return gs_miptbp_tbw(m1lo, m1hi, level - 1u);
        return gs_miptbp_tbw(m2lo, m2hi, level - 4u);
    }
    uint bw = gs_tex0_tbw(tex0_lo) >> level;
    return (bw == 0u) ? 1u : bw;
}

/* ---- the bilinear blend ----------------------------------------------------
 *
 * The four texels of the neighbourhood, weighted by the coordinate's four
 * fractional bits. The weights are sixteenths, so the blend is
 *
 *   C = (C00*(16-fu)*(16-fv) + C10*fu*(16-fv)
 *      + C01*(16-fu)*fv      + C11*fu*fv + 128) >> 8
 *
 * with the +128 rounding to nearest. The manual gives the four-texel blend
 * and not its rounding; round to nearest is the same choice the Gouraud DDA
 * made in gs_prim.h, and it is what makes a blend at a texel centre
 * reproduce that texel exactly.
 *
 * Alpha goes through the same blend as the colour channels. A texture whose
 * alpha is a mask therefore gets intermediate alphas at its edges, which is
 * what a filter does and what the alpha test then decides on. */
GS_FN uint gs_tex_bilerp(uint c00, uint c10, uint c01, uint c11, uint fu, uint fv) {
    uint out_v = 0u;
    uint i;
    uint w00 = (16u - fu) * (16u - fv);
    uint w10 = fu * (16u - fv);
    uint w01 = (16u - fu) * fv;
    uint w11 = fu * fv;
    for (i = 0u; i < 4u; ++i) {
        uint v = ((c00 >> (i * 8u)) & 0xFFu) * w00
               + ((c10 >> (i * 8u)) & 0xFFu) * w10
               + ((c01 >> (i * 8u)) & 0xFFu) * w01
               + ((c11 >> (i * 8u)) & 0xFFu) * w11;
        out_v |= (((v + 128u) >> 8) & 0xFFu) << (i * 8u);
    }
    return out_v;
}

/* A blend between two mip levels, by LOD's fractional sixteenths. Same
 * rounding rule as the bilinear blend above. */
GS_FN uint gs_tex_mix16(uint a, uint b, uint f) {
    uint out_v = 0u;
    uint i;
    for (i = 0u; i < 4u; ++i) {
        uint va = (a >> (i * 8u)) & 0xFFu;
        uint vb = (b >> (i * 8u)) & 0xFFu;
        out_v |= (((va * (16u - f) + vb * f + 8u) >> 4) & 0xFFu) << (i * 8u);
    }
    return out_v;
}

/* ---- format questions the sampler asks -------------------------------------
 *
 * How many bits of an index a palette format uses, and zero for a format
 * that is not palettised. PSMT8H is an 8-bit index in the top byte of a
 * 32-bit word and PSMT4HL/4HH are 4-bit indices in two nibbles of it;
 * gs_swizzle.h's gs_word_mask/gs_word_shift already extract the field, so
 * the only thing left to know here is the index width. */
GS_FN uint gs_tex_index_bits(uint psm) {
    if (psm == GS_PSMT8 || psm == GS_PSMT8H) return 8u;
    if (psm == GS_PSMT4 || psm == GS_PSMT4HL || psm == GS_PSMT4HH) return 4u;
    return 0u;
}

/* A non-palettised texel, in its stored form, to RGBA. The Z formats are
 * included because TEX0's PSM field is six bits wide and can name them: a
 * texture bound to a Z buffer reads the same bits the colour format of the
 * same width would, which is what the addressing gives and what
 * docs/GS_RENDERER.md records as inferred, the manual not listing the Z
 * formats as texture formats at all. */
GS_FN uint gs_tex_direct_rgba(uint psm, uint raw, uint texa) {
    if (psm == GS_PSMCT24 || psm == GS_PSMZ24) return gs_texa_expand24(raw, texa);
    if (psm == GS_PSMCT16 || psm == GS_PSMCT16S
        || psm == GS_PSMZ16 || psm == GS_PSMZ16S) {
        return gs_texa_expand16(raw & 0xFFFFu, texa);
    }
    return raw;
}

#ifndef GS_TEXTURE_GLSL
} /* namespace gsr */
#endif

#endif /* ICORECOMP_GS_TEXTURE_H */
