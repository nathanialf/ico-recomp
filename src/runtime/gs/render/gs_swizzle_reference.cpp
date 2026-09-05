/* gs/render/gs_swizzle_reference.cpp: a second, independent implementation of
 * GS local memory addressing, used only by gs_swizzle_selftest.cpp.
 *
 * Ours (MIT). Written from the GS User's Manual's column diagrams as literal
 * tables and from the page diagrams as literal block tables, and composed by
 * an explicit page -> block -> column -> pixel walk. gs_swizzle.h computes the
 * same answer a different way: it keeps one word-order table and derives the
 * narrower formats' sub-positions from stated rules. The two files share
 * nothing but the PSM constants, so where they agree the composition is
 * confirmed by two separate transcriptions.
 *
 * Scope of the check, stated rather than implied:
 *
 *   PSMCT32/24, PSMZ32/24, PSMT8H, PSMT4HL, PSMT4HH, PSMCT16, PSMCT16S,
 *   PSMZ16, PSMZ16S and PSMT8 are checked against literal column tables here.
 *
 *   PSMT4 is not. Its column diagram is 32x4 pixels, 512 nibbles, and it is
 *   not transcribed into a literal table in this file. Writing one from
 *   anything other than the manual page in front of the author would be a
 *   plausible value substituted for a known one, which this project does not
 *   do. The selftest checks PSMT4 for everything that does not depend on that
 *   diagram: block and column placement against the page table above,
 *   bijectivity over a whole page, and, for x below 16, the word inside the
 *   block against the literal PSMT8 column table, which the two formats share
 *   because a 4-bit and an 8-bit column are the same 64 bytes. What is left
 *   unchecked is the 3-bit nibble position inside that word. Settling it needs
 *   either the manual page or a measured PSMT4 upload from the running game;
 *   until then the header's rule for it is inferred, not measured.
 *
 * Returns byte addresses inside the 4 MiB store plus the bit offset of the
 * pixel inside the byte, which is a different unit from gs_swizzle.h's pixel
 * addresses on purpose: the selftest has to convert one to the other, so a
 * shared off-by-one in the unit cannot pass unnoticed.
 */
#include "gs_swizzle_reference.h"

#include "gs_swizzle.h"

namespace gsref {

namespace {

/* ---- column tables, transcribed from the manual --------------------------
 *
 * kCol32[y & 7][x & 7]: word offset inside an 8x8 block of 32-bit pixels. */
const uint32_t kCol32[8][8] = {
    {  0,  1,  4,  5,  8,  9, 12, 13 },
    {  2,  3,  6,  7, 10, 11, 14, 15 },
    { 16, 17, 20, 21, 24, 25, 28, 29 },
    { 18, 19, 22, 23, 26, 27, 30, 31 },
    { 32, 33, 36, 37, 40, 41, 44, 45 },
    { 34, 35, 38, 39, 42, 43, 46, 47 },
    { 48, 49, 52, 53, 56, 57, 60, 61 },
    { 50, 51, 54, 55, 58, 59, 62, 63 },
};

/* kCol16[y & 7][x & 15]: half-word offset inside a 16x8 block of 16-bit
 * pixels. */
const uint32_t kCol16[8][16] = {
    {   0,   2,   8,  10,  16,  18,  24,  26,   1,   3,   9,  11,  17,  19,  25,  27 },
    {   4,   6,  12,  14,  20,  22,  28,  30,   5,   7,  13,  15,  21,  23,  29,  31 },
    {  32,  34,  40,  42,  48,  50,  56,  58,  33,  35,  41,  43,  49,  51,  57,  59 },
    {  36,  38,  44,  46,  52,  54,  60,  62,  37,  39,  45,  47,  53,  55,  61,  63 },
    {  64,  66,  72,  74,  80,  82,  88,  90,  65,  67,  73,  75,  81,  83,  89,  91 },
    {  68,  70,  76,  78,  84,  86,  92,  94,  69,  71,  77,  79,  85,  87,  93,  95 },
    {  96,  98, 104, 106, 112, 114, 120, 122,  97,  99, 105, 107, 113, 115, 121, 123 },
    { 100, 102, 108, 110, 116, 118, 124, 126, 101, 103, 109, 111, 117, 119, 125, 127 },
};

/* kCol8[y & 15][x & 15]: byte offset inside a 16x16 block of 8-bit pixels. */
const uint32_t kCol8[16][16] = {
    {   0,   4,  16,  20,  32,  36,  48,  52,   8,  12,  24,  28,  40,  44,  56,  60 },
    {   2,   6,  18,  22,  34,  38,  50,  54,  10,  14,  26,  30,  42,  46,  58,  62 },
    {  33,  37,  49,  53,   1,   5,  17,  21,  41,  45,  57,  61,   9,  13,  25,  29 },
    {  35,  39,  51,  55,   3,   7,  19,  23,  43,  47,  59,  63,  11,  15,  27,  31 },
    {  66,  70,  82,  86,  98, 102, 114, 118,  74,  78,  90,  94, 106, 110, 122, 126 },
    {  64,  68,  80,  84,  96, 100, 112, 116,  72,  76,  88,  92, 104, 108, 120, 124 },
    {  99, 103, 115, 119,  67,  71,  83,  87, 107, 111, 123, 127,  75,  79,  91,  95 },
    {  97, 101, 113, 117,  65,  69,  81,  85, 105, 109, 121, 125,  73,  77,  89,  93 },
    { 128, 132, 144, 148, 160, 164, 176, 180, 136, 140, 152, 156, 168, 172, 184, 188 },
    { 130, 134, 146, 150, 162, 166, 178, 182, 138, 142, 154, 158, 170, 174, 186, 190 },
    { 161, 165, 177, 181, 129, 133, 145, 149, 169, 173, 185, 189, 137, 141, 153, 157 },
    { 163, 167, 179, 183, 131, 135, 147, 151, 171, 175, 187, 191, 139, 143, 155, 159 },
    { 194, 198, 210, 214, 226, 230, 242, 246, 202, 206, 218, 222, 234, 238, 250, 254 },
    { 192, 196, 208, 212, 224, 228, 240, 244, 200, 204, 216, 220, 232, 236, 248, 252 },
    { 227, 231, 243, 247, 195, 199, 211, 215, 235, 239, 251, 255, 203, 207, 219, 223 },
    { 225, 229, 241, 245, 193, 197, 209, 213, 233, 237, 249, 253, 201, 205, 217, 221 },
};

/* ---- page diagrams, transcribed as [block row][block column] -------------- */
const uint32_t kPage32[4][8] = {
    {  0,  1,  4,  5, 16, 17, 20, 21 },
    {  2,  3,  6,  7, 18, 19, 22, 23 },
    {  8,  9, 12, 13, 24, 25, 28, 29 },
    { 10, 11, 14, 15, 26, 27, 30, 31 },
};
const uint32_t kPage32Z[4][8] = {
    { 24, 25, 28, 29,  8,  9, 12, 13 },
    { 26, 27, 30, 31, 10, 11, 14, 15 },
    { 16, 17, 20, 21,  0,  1,  4,  5 },
    { 18, 19, 22, 23,  2,  3,  6,  7 },
};
const uint32_t kPage16[8][4] = {
    {  0,  2,  8, 10 }, {  1,  3,  9, 11 }, {  4,  6, 12, 14 }, {  5,  7, 13, 15 },
    { 16, 18, 24, 26 }, { 17, 19, 25, 27 }, { 20, 22, 28, 30 }, { 21, 23, 29, 31 },
};
const uint32_t kPage16S[8][4] = {
    {  0,  2, 16, 18 }, {  1,  3, 17, 19 }, {  8, 10, 24, 26 }, {  9, 11, 25, 27 },
    {  4,  6, 20, 22 }, {  5,  7, 21, 23 }, { 12, 14, 28, 30 }, { 13, 15, 29, 31 },
};
const uint32_t kPage16Z[8][4] = {
    { 24, 26, 16, 18 }, { 25, 27, 17, 19 }, { 28, 30, 20, 22 }, { 29, 31, 21, 23 },
    {  8, 10,  0,  2 }, {  9, 11,  1,  3 }, { 12, 14,  4,  6 }, { 13, 15,  5,  7 },
};
const uint32_t kPage16SZ[8][4] = {
    { 24, 26,  8, 10 }, { 25, 27,  9, 11 }, { 16, 18,  0,  2 }, { 17, 19,  1,  3 },
    { 28, 30, 12, 14 }, { 29, 31, 13, 15 }, { 20, 22,  4,  6 }, { 21, 23,  5,  7 },
};

struct Geometry {
    uint32_t page_w, page_h;   /* pixels */
    uint32_t block_w, block_h; /* pixels */
    uint32_t bits;             /* bits per pixel in the addressed unit */
};

Geometry geometry_of(uint32_t psm) {
    switch (psm) {
        case GS_PSMCT32: case GS_PSMCT24: case GS_PSMT8H:
        case GS_PSMT4HL: case GS_PSMT4HH:
        case GS_PSMZ32: case GS_PSMZ24:
            return Geometry{ 64, 32, 8, 8, 32 };
        case GS_PSMCT16: case GS_PSMCT16S: case GS_PSMZ16: case GS_PSMZ16S:
            return Geometry{ 64, 64, 16, 8, 16 };
        case GS_PSMT8:
            return Geometry{ 128, 64, 16, 16, 8 };
        case GS_PSMT4:
            return Geometry{ 128, 128, 32, 16, 4 };
        default:
            return Geometry{ 0, 0, 0, 0, 0 };
    }
}

uint32_t page_block(uint32_t psm, uint32_t bx, uint32_t by) {
    switch (psm) {
        case GS_PSMCT32: case GS_PSMCT24: case GS_PSMT8H:
        case GS_PSMT4HL: case GS_PSMT4HH:
        case GS_PSMT8:
            return kPage32[by][bx];
        case GS_PSMZ32: case GS_PSMZ24:
            return kPage32Z[by][bx];
        case GS_PSMCT16: case GS_PSMT4:
            return kPage16[by][bx];
        case GS_PSMCT16S:
            return kPage16S[by][bx];
        case GS_PSMZ16:
            return kPage16Z[by][bx];
        case GS_PSMZ16S:
            return kPage16SZ[by][bx];
        default:
            return 0;
    }
}

} // namespace

bool has_column_table(uint32_t psm) {
    return psm != GS_PSMT4 && geometry_of(psm).bits != 0;
}

Address address(uint32_t psm, uint32_t base_block, uint32_t fbw, uint32_t x, uint32_t y) {
    Address a{};
    const Geometry g = geometry_of(psm);
    if (g.bits == 0) return a;

    /* Page, in the buffer's own page raster. FBW counts 64-pixel units, so a
     * 128-pixel-wide page covers two of them. */
    uint32_t pages_across = (fbw * 64) / g.page_w;
    if (pages_across == 0) pages_across = 1;
    const uint32_t page = (y / g.page_h) * pages_across + (x / g.page_w);

    /* Block inside the page, from the page diagram. */
    const uint32_t bx = (x % g.page_w) / g.block_w;
    const uint32_t by = (y % g.page_h) / g.block_h;
    const uint32_t block = (base_block + page * 32 + page_block(psm, bx, by)) & (GS_VRAM_BLOCKS - 1);

    /* Pixel inside the block, from the column diagram. Each table is indexed
     * by the pixel's position inside the block and answers in that format's
     * own storage unit. */
    const uint32_t lx = x % g.block_w;
    const uint32_t ly = y % g.block_h;
    uint32_t bit_in_block = 0;
    switch (g.bits) {
        case 32: bit_in_block = kCol32[ly][lx] * 32; break;
        case 16: bit_in_block = kCol16[ly][lx] * 16; break;
        case 8:  bit_in_block = kCol8[ly][lx] * 8; break;
        default: /* PSMT4: no literal table here, see the file comment. */
            a.valid = false;
            a.byte_addr = block * GS_BLOCK_BYTES;
            a.bit_in_byte = 0;
            a.block = block;
            a.column = ly / 4;
            return a;
    }
    a.valid = true;
    a.byte_addr = block * GS_BLOCK_BYTES + (bit_in_block >> 3);
    a.bit_in_byte = bit_in_block & 7;
    a.block = block;
    a.column = (bit_in_block >> 3) / 64;
    return a;
}

} // namespace gsref
