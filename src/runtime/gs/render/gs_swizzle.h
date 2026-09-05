/* gs/render/gs_swizzle.h: GS local memory addressing, shared verbatim by the
 * C++ renderer and the GLSL compute shaders.
 *
 * Ours (MIT). Derived from the GS User's Manual page/block/column layouts,
 * not from any emulator source. The file is written in the intersection of
 * C++ and GLSL 450: no templates, no references, no constexpr, no struct
 * methods, and every array literal goes through the GS_ARR macro because the
 * two languages spell array initialisers differently. Define GS_SWIZZLE_GLSL
 * before including it from a shader.
 *
 * The model, straight out of the manual:
 *
 *   local memory   4 MiB, addressed as 16384 blocks of 256 bytes.
 *   page           8192 bytes, always 32 blocks. Its size in pixels depends
 *                  on the pixel format: 64x32 for the 32-bit formats,
 *                  64x64 for the 16-bit formats, 128x64 for PSMT8 and
 *                  128x128 for PSMT4. Every one of those is 2048 words.
 *   block          256 bytes. 8x8, 16x8, 16x16 or 32x16 pixels by the same
 *                  four families. The 32 blocks of a page are not in raster
 *                  order; the per-format block tables below are the manual's
 *                  page diagrams transcribed.
 *   column         a horizontal slice of a block, 64 bytes: 8x2, 16x2, 16x4
 *                  or 32x4 pixels. Pixels inside a column are not in raster
 *                  order either.
 *
 * The column order is expressed once, as the 32-bit word order kColumn32,
 * and the narrower formats are placed inside those same words. That is the
 * physical truth: a 64-byte column is 16 words whatever the format, and a
 * 16-bit column packs two pixels per word, an 8-bit column four, a 4-bit
 * column eight. The rules that place them (which sub-position a row uses,
 * and the four-entry rotation the lower half of an 8- or 4-bit column takes)
 * are stated at each function.
 *
 * gs_swizzle_reference.cpp is a second, independent implementation written
 * from the manual's full column tables as literal arrays, and
 * gs_swizzle_selftest.cpp round-trips every (format, x, y) in a 128x128
 * region against it. The two share nothing but the format constants, so a
 * disagreement names a real error rather than a shared assumption.
 *
 * Addresses are returned in pixel units (words for the 32-bit formats,
 * half-words for the 16-bit ones, bytes for PSMT8, nibbles for PSMT4), which
 * is what both a CPU transfer and a shader fetch want. Callers convert to a
 * word index and a shift with gs_addr_bits().
 */
#ifndef ICORECOMP_GS_SWIZZLE_H
#define ICORECOMP_GS_SWIZZLE_H

/* Array initialisers are spelled differently in the two languages, and the
 * GLSL preprocessor has no variadic macros, so the brackets are macros and the
 * numbers between them are written once. */
#ifdef GS_SWIZZLE_GLSL
#define GS_ARR_BEGIN(n) uint[n](
#define GS_ARR_END )
/* GLSL has no linkage to control; C++ needs every function here to be inline
 * because this header is included by several translation units. */
#define GS_FN
#else
#include <cstdint>
namespace gsr {
using uint = uint32_t;
#define GS_ARR_BEGIN(n) {
#define GS_ARR_END }
#define GS_FN inline
#endif

/* PSM codes, GS User's Manual "pixel storage format". */
#define GS_PSMCT32  0u
#define GS_PSMCT24  1u
#define GS_PSMCT16  2u
#define GS_PSMCT16S 10u
#define GS_PSMT8    19u
#define GS_PSMT4    20u
#define GS_PSMT8H   27u
#define GS_PSMT4HL  36u
#define GS_PSMT4HH  44u
#define GS_PSMZ32   48u
#define GS_PSMZ24   49u
#define GS_PSMZ16   50u
#define GS_PSMZ16S  58u

/* Addressing families. Two formats in the same family have the same page,
 * block and column geometry and differ only in which bits of the addressed
 * unit they occupy. */
#define GS_FAM_32   0u  /* PSMCT32/24, PSMZ32/24, PSMT8H, PSMT4HL, PSMT4HH */
#define GS_FAM_16   1u  /* PSMCT16, PSMZ16 */
#define GS_FAM_16S  2u  /* PSMCT16S, PSMZ16S */
#define GS_FAM_8    3u  /* PSMT8 */
#define GS_FAM_4    4u  /* PSMT4 */
#define GS_FAM_32Z  5u  /* PSMZ32/24: 32-bit geometry, Z block order */
#define GS_FAM_16Z  6u
#define GS_FAM_16SZ 7u
#define GS_FAM_BAD  255u

/* One page of any format is 32 blocks of 256 bytes. */
#define GS_BLOCK_BYTES   256u
#define GS_PAGE_BLOCKS   32u
#define GS_VRAM_BYTES    4194304u
#define GS_VRAM_BLOCKS   16384u
#define GS_VRAM_WORDS    1048576u

/* ---- block tables ---------------------------------------------------------
 *
 * Block number for a (block column, block row) position inside a page,
 * transcribed from the manual's page diagrams. The 32-bit and PSMT8 pages
 * are 8 blocks wide by 4 high; the 16-bit and PSMT4 pages are 4 wide by 8
 * high. PSMT8 shares the 32-bit page diagram and PSMT4 shares the 16-bit
 * one, which is why there are four tables and not eight: only the Z formats
 * permute them further. */

const uint kBlock32[32] = GS_ARR_BEGIN(32)
     0u,  1u,  4u,  5u, 16u, 17u, 20u, 21u,
     2u,  3u,  6u,  7u, 18u, 19u, 22u, 23u,
     8u,  9u, 12u, 13u, 24u, 25u, 28u, 29u,
    10u, 11u, 14u, 15u, 26u, 27u, 30u, 31u
GS_ARR_END;

const uint kBlock32Z[32] = GS_ARR_BEGIN(32)
    24u, 25u, 28u, 29u,  8u,  9u, 12u, 13u,
    26u, 27u, 30u, 31u, 10u, 11u, 14u, 15u,
    16u, 17u, 20u, 21u,  0u,  1u,  4u,  5u,
    18u, 19u, 22u, 23u,  2u,  3u,  6u,  7u
GS_ARR_END;

const uint kBlock16[32] = GS_ARR_BEGIN(32)
     0u,  2u,  8u, 10u,
     1u,  3u,  9u, 11u,
     4u,  6u, 12u, 14u,
     5u,  7u, 13u, 15u,
    16u, 18u, 24u, 26u,
    17u, 19u, 25u, 27u,
    20u, 22u, 28u, 30u,
    21u, 23u, 29u, 31u
GS_ARR_END;

const uint kBlock16S[32] = GS_ARR_BEGIN(32)
     0u,  2u, 16u, 18u,
     1u,  3u, 17u, 19u,
     8u, 10u, 24u, 26u,
     9u, 11u, 25u, 27u,
     4u,  6u, 20u, 22u,
     5u,  7u, 21u, 23u,
    12u, 14u, 28u, 30u,
    13u, 15u, 29u, 31u
GS_ARR_END;

const uint kBlock16Z[32] = GS_ARR_BEGIN(32)
    24u, 26u, 16u, 18u,
    25u, 27u, 17u, 19u,
    28u, 30u, 20u, 22u,
    29u, 31u, 21u, 23u,
     8u, 10u,  0u,  2u,
     9u, 11u,  1u,  3u,
    12u, 14u,  4u,  6u,
    13u, 15u,  5u,  7u
GS_ARR_END;

const uint kBlock16SZ[32] = GS_ARR_BEGIN(32)
    24u, 26u,  8u, 10u,
    25u, 27u,  9u, 11u,
    16u, 18u,  0u,  2u,
    17u, 19u,  1u,  3u,
    28u, 30u, 12u, 14u,
    29u, 31u, 13u, 15u,
    20u, 22u,  4u,  6u,
    21u, 23u,  5u,  7u
GS_ARR_END;

/* ---- column word order ----------------------------------------------------
 *
 * The word offset inside a 64-byte column for a 32-bit pixel at (x & 7,
 * y & 1): kColumn32[(y & 1) * 8 + (x & 7)]. Every other format places its
 * pixels inside these same words. */
const uint kColumn32[16] = GS_ARR_BEGIN(16)
     0u,  1u,  4u,  5u,  8u,  9u, 12u, 13u,
     2u,  3u,  6u,  7u, 10u, 11u, 14u, 15u
GS_ARR_END;

/* ---- format metadata ------------------------------------------------------ */

GS_FN uint gs_psm_family(uint psm) {
    if (psm == GS_PSMCT32 || psm == GS_PSMCT24 || psm == GS_PSMT8H ||
        psm == GS_PSMT4HL || psm == GS_PSMT4HH) return GS_FAM_32;
    if (psm == GS_PSMZ32 || psm == GS_PSMZ24) return GS_FAM_32Z;
    if (psm == GS_PSMCT16) return GS_FAM_16;
    if (psm == GS_PSMCT16S) return GS_FAM_16S;
    if (psm == GS_PSMZ16) return GS_FAM_16Z;
    if (psm == GS_PSMZ16S) return GS_FAM_16SZ;
    if (psm == GS_PSMT8) return GS_FAM_8;
    if (psm == GS_PSMT4) return GS_FAM_4;
    return GS_FAM_BAD;
}

/* Bits per addressing unit: the width of the thing gs_pixel_addr() counts.
 * PSMCT24 and PSMZ24 address whole words and use the low 24 bits, and the
 * three H formats address whole words and use a field of the top byte, so
 * all of them answer 32. */
GS_FN uint gs_addr_bits(uint psm) {
    uint fam = gs_psm_family(psm);
    if (fam == GS_FAM_8) return 8u;
    if (fam == GS_FAM_4) return 4u;
    if (fam == GS_FAM_16 || fam == GS_FAM_16S || fam == GS_FAM_16Z || fam == GS_FAM_16SZ) return 16u;
    return 32u;
}

GS_FN uint gs_page_width(uint fam) {
    if (fam == GS_FAM_8 || fam == GS_FAM_4) return 128u;
    return 64u;
}

GS_FN uint gs_page_height(uint fam) {
    if (fam == GS_FAM_4) return 128u;
    if (fam == GS_FAM_8) return 64u;
    if (fam == GS_FAM_32 || fam == GS_FAM_32Z) return 32u;
    return 64u;
}

GS_FN uint gs_block_width(uint fam) {
    if (fam == GS_FAM_4) return 32u;
    if (fam == GS_FAM_8) return 16u;
    if (fam == GS_FAM_32 || fam == GS_FAM_32Z) return 8u;
    return 16u;
}

GS_FN uint gs_block_height(uint fam) {
    if (fam == GS_FAM_32 || fam == GS_FAM_32Z) return 8u;
    if (fam == GS_FAM_8 || fam == GS_FAM_4) return 16u;
    return 8u;
}

/* Blocks across a page, which is what the block table's row stride is. */
GS_FN uint gs_page_block_cols(uint fam) {
    return gs_page_width(fam) / gs_block_width(fam);
}

GS_FN uint gs_block_number(uint fam, uint bx, uint by) {
    uint i = by * gs_page_block_cols(fam) + bx;
    if (fam == GS_FAM_32 || fam == GS_FAM_8) return kBlock32[i];
    if (fam == GS_FAM_32Z) return kBlock32Z[i];
    if (fam == GS_FAM_16 || fam == GS_FAM_4) return kBlock16[i];
    if (fam == GS_FAM_16S) return kBlock16S[i];
    if (fam == GS_FAM_16Z) return kBlock16Z[i];
    if (fam == GS_FAM_16SZ) return kBlock16SZ[i];
    return 0u;
}

/* ---- position inside a block ----------------------------------------------
 *
 * Returned in pixel units from the start of the block, so a 32-bit block
 * answers 0..63, a 16-bit block 0..127, PSMT8 0..255 and PSMT4 0..511.
 *
 * 32-bit: the column is 8x2 and kColumn32 is the whole of it. Column index
 * is (y & 7) >> 1 and each column is 16 words.
 *
 * 16-bit: the column is 16x2, two pixels per word. The left half of the row
 * (x & 8 clear) takes the low half-word of each word and the right half the
 * high half-word, and the word order is kColumn32 for the row's own parity.
 *
 * 8-bit: the column is 16x4, four pixels per word. x picks the word the same
 * way (x & 7 selects within kColumn32's row, x & 8 selects the row), and the
 * four rows of the column pick the byte inside it in the order 0, 2, 1, 3.
 * The two lower rows of a column also rotate the word order by four entries.
 * Odd-numbered columns swap the byte order to 2, 0, 3, 1, which is the
 * alternation the manual's column diagram shows between successive columns.
 *
 * 4-bit: the column is 32x4, eight pixels per word, and it is the 8-bit rule
 * with one more x bit: x & 16 selects the high nibble pair, so the nibble is
 * ((x >> 4) & 1) * 4 plus the same rotated byte order. */

GS_FN uint gs_block_offset(uint fam, uint x, uint y) {
    uint bw = gs_block_width(fam);
    uint bh = gs_block_height(fam);
    uint lx = x % bw;
    uint ly = y % bh;

    if (fam == GS_FAM_32 || fam == GS_FAM_32Z) {
        uint col = ly >> 1;
        return col * 16u + kColumn32[(ly & 1u) * 8u + (lx & 7u)];
    }
    if (fam == GS_FAM_16 || fam == GS_FAM_16S || fam == GS_FAM_16Z || fam == GS_FAM_16SZ) {
        uint col = ly >> 1;
        uint w = kColumn32[(ly & 1u) * 8u + (lx & 7u)];
        /* Not named "half": that is a reserved word in GLSL. */
        uint hword = (lx >> 3) & 1u;
        return col * 32u + w * 2u + hword;
    }
    /* PSMT8 and PSMT4 share one rule; only the width of the sub-position and
     * the number of x bits that feed it differ. */
    uint col = ly >> 2;
    uint row = ly & 3u;             /* 0..3 inside the column */
    uint rot = (row >= 2u) ? 4u : 0u;
    uint w = kColumn32[((lx >> 3) & 1u) * 8u + (((lx & 7u) + rot) & 7u)];
    /* Byte order inside the word for rows 0..3, and its odd-column swap. */
    uint sub = (row == 0u) ? 0u : ((row == 1u) ? 2u : ((row == 2u) ? 1u : 3u));
    if ((col & 1u) != 0u) sub = sub ^ 2u;
    if (fam == GS_FAM_8) {
        return col * 64u + w * 4u + sub;
    }
    uint nib = ((lx >> 4) & 1u) * 4u + sub;
    return col * 128u + w * 8u + nib;
}

/* ---- full address ---------------------------------------------------------
 *
 * base_block is the buffer base in 256-byte blocks: BITBLTBUF SBP/DBP and
 * TEX0 TBP0 are already in that unit, FRAME FBP and DISPFB FBP are in pages
 * and become FBP * 32.
 *
 * fbw is the buffer width in units of 64 pixels, as every GS register that
 * carries one states it. The number of pages across is fbw * 64 / page
 * width, so an 8- or 4-bit buffer covers two 64-pixel units per page.
 *
 * The block index wraps at 4 MiB, which is what the hardware does with a
 * buffer that runs off the end of local memory. */
GS_FN uint gs_pixel_addr(uint psm, uint base_block, uint fbw, uint x, uint y) {
    uint fam = gs_psm_family(psm);
    uint pw = gs_page_width(fam);
    uint ph = gs_page_height(fam);
    uint pages_across = (fbw * 64u) / pw;
    if (pages_across == 0u) pages_across = 1u;
    uint page = (y / ph) * pages_across + (x / pw);
    uint bx = (x % pw) / gs_block_width(fam);
    uint by = (y % ph) / gs_block_height(fam);
    uint block = (base_block + page * GS_PAGE_BLOCKS + gs_block_number(fam, bx, by))
                 & (GS_VRAM_BLOCKS - 1u);
    uint units_per_block = (GS_BLOCK_BYTES * 8u) / gs_addr_bits(psm);
    return block * units_per_block + gs_block_offset(fam, x, y);
}

/* Word index in the 4 MiB store, and the bit offset of the pixel inside it.
 * gs_pixel_shift() is meaningless for the 32-bit formats and answers 0. */
GS_FN uint gs_pixel_word(uint psm, uint base_block, uint fbw, uint x, uint y) {
    uint bits = gs_addr_bits(psm);
    return (gs_pixel_addr(psm, base_block, fbw, x, y) * bits) >> 5;
}

GS_FN uint gs_pixel_shift(uint psm, uint base_block, uint fbw, uint x, uint y) {
    uint bits = gs_addr_bits(psm);
    return (gs_pixel_addr(psm, base_block, fbw, x, y) * bits) & 31u;
}

/* ---- word-level field selection -------------------------------------------
 *
 * The formats that address a whole word but occupy only part of it. Callers
 * that read or write those go through these rather than open-coding the
 * masks, so the C++ transfer path and the shaders agree. */

GS_FN uint gs_word_mask(uint psm) {
    if (psm == GS_PSMCT24 || psm == GS_PSMZ24) return 0x00FFFFFFu;
    if (psm == GS_PSMT8H) return 0xFF000000u;
    if (psm == GS_PSMT4HL) return 0x0F000000u;
    if (psm == GS_PSMT4HH) return 0xF0000000u;
    return 0xFFFFFFFFu;
}

GS_FN uint gs_word_shift(uint psm) {
    if (psm == GS_PSMT8H || psm == GS_PSMT4HL) return 24u;
    if (psm == GS_PSMT4HH) return 28u;
    return 0u;
}

/* ---- 16-bit colour conversion ---------------------------------------------
 *
 * PSMCT16/16S are A1B5G5R5 in the manual's bit order: R in 0..4, G in 5..9,
 * B in 10..14, A in 15. Expanded to 8 bits per channel by the hardware's own
 * rule, which is a left shift by 3 with no replication of the high bits, and
 * A expands to 0 or 128 rather than 0 or 255 (the manual's TEXA/alpha
 * expansion; 0x80 is the "one" the GS uses for alpha). */
GS_FN uint gs_expand16(uint v) {
    uint r = (v & 0x1Fu) << 3;
    uint g = ((v >> 5) & 0x1Fu) << 3;
    uint b = ((v >> 10) & 0x1Fu) << 3;
    uint a = ((v >> 15) & 1u) * 128u;
    return r | (g << 8) | (b << 16) | (a << 24);
}

#ifndef GS_SWIZZLE_GLSL
} /* namespace gsr */
#endif

#endif /* ICORECOMP_GS_SWIZZLE_H */
