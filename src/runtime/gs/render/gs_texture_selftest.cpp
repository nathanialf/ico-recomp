/* gs/render/gs_texture_selftest.cpp: icorecomp-gs-texture-selftest.
 *
 * Ours (MIT). Milestone (c)'s share of the parts that can be wrong without
 * anyone seeing it in a picture. Needs no GPU, no disc and no ROM-derived
 * data: every expected value below is written out by hand from the GS User's
 * Manual's formula, not recorded from a run of this code, which would only
 * prove the code agrees with itself.
 *
 *   1. The four CLAMP modes, including REGION_REPEAT's mask and OR, on
 *      coordinates chosen to land inside, outside and negative.
 *   2. The CSM1 CLUT arrangement: the named slots, the fact that it is a
 *      permutation of all 256 indices, and that it is its own inverse.
 *   3. TEXA expansion for the 16-bit and 24-bit cases, both AEM values.
 *   4. The four TFX functions, with TCC 0 and 1, on values whose products
 *      are exact.
 *   5. The CLD table as a state machine, against a real load out of a
 *      LocalMemory holding a known palette, plus the CBP0/CBP1 comparison
 *      rules.
 *   6. The page tracker on synthetic buffers: a frame buffer and a texture
 *      that share a page, and two that do not.
 *   7. The mip base rules, explicit (MIPTBP) and automatic (MTBA).
 *
 * Exit status 0 when every case passed, 1 otherwise.
 */
#include "gs_clut.h"
#include "gs_draw.h"
#include "gs_texture.h"
#include "gs_vram.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void fail(const std::string& what, const std::string& got, const std::string& want) {
    std::printf("FAIL %s\n  got:  %s\n  want: %s\n", what.c_str(), got.c_str(), want.c_str());
    ++g_failures;
}

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", v);
    return buf;
}

void check_u(const char* what, uint32_t got, uint32_t want) {
    if (got != want) fail(what, hex(got), hex(want));
}

void check_i(const char* what, int got, int want) {
    if (got != want) fail(what, std::to_string(got), std::to_string(want));
}

/* ---- 1. the clamp modes ---------------------------------------------------- */

void test_clamp() {
    /* REPEAT on a 64-texel axis is the low six bits, and a negative
     * coordinate wraps to the top of the range rather than to zero. */
    check_i("REPEAT inside", gsr::gs_tex_wrap(20, GS_WM_REPEAT, 0, 0, 64), 20);
    check_i("REPEAT past the end", gsr::gs_tex_wrap(70, GS_WM_REPEAT, 0, 0, 64), 6);
    check_i("REPEAT negative", gsr::gs_tex_wrap(-1, GS_WM_REPEAT, 0, 0, 64), 63);

    /* CLAMP pins to the texture, both ends. */
    check_i("CLAMP inside", gsr::gs_tex_wrap(20, GS_WM_CLAMP, 0, 0, 64), 20);
    check_i("CLAMP past the end", gsr::gs_tex_wrap(70, GS_WM_CLAMP, 0, 0, 64), 63);
    check_i("CLAMP negative", gsr::gs_tex_wrap(-5, GS_WM_CLAMP, 0, 0, 64), 0);

    /* REGION_CLAMP pins to [MIN, MAX] instead, which can be a strict subset
     * of the texture. */
    check_i("REGION_CLAMP inside", gsr::gs_tex_wrap(20, GS_WM_REGION_CLAMP, 8, 40, 64), 20);
    check_i("REGION_CLAMP below", gsr::gs_tex_wrap(3, GS_WM_REGION_CLAMP, 8, 40, 64), 8);
    check_i("REGION_CLAMP above", gsr::gs_tex_wrap(55, GS_WM_REGION_CLAMP, 8, 40, 64), 40);

    /* REGION_REPEAT is (U & MIN) | MAX, so MIN is a mask and MAX a pattern.
     * MIN 7 and MAX 16 tile an 8-texel window starting at 16: 20 keeps its
     * low three bits (4) and lands at 20, 24 loses everything above three
     * bits and lands at 16, and 5 lands at 21. */
    check_i("REGION_REPEAT in the window",
            gsr::gs_tex_wrap(20, GS_WM_REGION_REPEAT, 7, 16, 64), 20);
    check_i("REGION_REPEAT wraps to the window start",
            gsr::gs_tex_wrap(24, GS_WM_REGION_REPEAT, 7, 16, 64), 16);
    check_i("REGION_REPEAT below the window",
            gsr::gs_tex_wrap(5, GS_WM_REGION_REPEAT, 7, 16, 64), 21);
}

/* ---- 2. the CSM1 CLUT arrangement ------------------------------------------ */

void test_clut_arrangement() {
    /* Bits 3 and 4 of the index exchange and nothing else moves. */
    check_u("CSM1 slot of 0", gsr::gs_clut_slot_csm1_8(0), 0);
    check_u("CSM1 slot of 7", gsr::gs_clut_slot_csm1_8(7), 7);
    check_u("CSM1 slot of 8", gsr::gs_clut_slot_csm1_8(8), 16);
    check_u("CSM1 slot of 16", gsr::gs_clut_slot_csm1_8(16), 8);
    check_u("CSM1 slot of 24", gsr::gs_clut_slot_csm1_8(24), 24);
    check_u("CSM1 slot of 0xFF", gsr::gs_clut_slot_csm1_8(0xFF), 0xFF);
    check_u("CSM1 slot of 0xE8", gsr::gs_clut_slot_csm1_8(0xE8), 0xF0);

    /* It has to be a permutation of the whole 256, or a palette would lose
     * or duplicate an entry, and it has to be its own inverse, or a load and
     * a lookup could not share one function. */
    bool seen[256] = {};
    for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t slot = gsr::gs_clut_slot_csm1_8(i);
        if (slot > 255 || seen[slot]) {
            fail("CSM1 arrangement is a permutation", "index " + std::to_string(i)
                 + " to slot " + std::to_string(slot), "each slot exactly once");
            return;
        }
        seen[slot] = true;
        if (gsr::gs_clut_slot_csm1_8(slot) != i) {
            fail("CSM1 arrangement is its own inverse", std::to_string(i), std::to_string(slot));
            return;
        }
    }

    /* A 4-bit palette has no bit 4 to exchange, so its sixteen entries are
     * consecutive and CSA picks the group. */
    check_u("4-bit entry, CSA 0", gsr::gs_clut_entry(5, 4, 0, 0), 5);
    check_u("4-bit entry, CSA 3", gsr::gs_clut_entry(5, 4, 0, 3), 53);
    check_u("8-bit entry, CSM1", gsr::gs_clut_entry(8, 8, 0, 0), 16);
    check_u("8-bit entry, CSM2 is linear", gsr::gs_clut_entry(8, 8, 1, 0), 8);
}

/* ---- 3. TEXA --------------------------------------------------------------- */

void test_texa() {
    /* TA0 0x20, TA1 0x70, AEM off. */
    const uint32_t texa = gsr::gs_texa_pack(0x20u | (0u << 15), 0x70u);
    /* R 31, G 0, B 0, A bit 0: red 248, alpha TA0. */
    check_u("TEXA 16-bit, A bit clear", gsr::gs_texa_expand16(0x001Fu, texa),
            0x200000F8u);
    /* Same with the A bit set: alpha TA1. */
    check_u("TEXA 16-bit, A bit set", gsr::gs_texa_expand16(0x801Fu, texa),
            0x700000F8u);
    /* An all-zero texel with AEM off keeps TA0. */
    check_u("TEXA 16-bit, zero texel, AEM off", gsr::gs_texa_expand16(0u, texa),
            0x20000000u);

    const uint32_t aem = gsr::gs_texa_pack(0x20u | (1u << 15), 0x70u);
    /* With AEM on, an all-zero texel becomes fully transparent instead. */
    check_u("TEXA 16-bit, zero texel, AEM on", gsr::gs_texa_expand16(0u, aem), 0u);
    /* AEM only applies to the zero texel: a black pixel with the A bit set
     * still takes TA1. */
    check_u("TEXA 16-bit, A bit set on black, AEM on",
            gsr::gs_texa_expand16(0x8000u, aem), 0x70000000u);

    /* 24-bit takes TA0 always, with the same AEM rule on a zero colour. */
    check_u("TEXA 24-bit", gsr::gs_texa_expand24(0x00336699u, texa), 0x20336699u);
    check_u("TEXA 24-bit, zero, AEM off", gsr::gs_texa_expand24(0u, texa), 0x20000000u);
    check_u("TEXA 24-bit, zero, AEM on", gsr::gs_texa_expand24(0u, aem), 0u);
}

/* ---- 4. the texture function ----------------------------------------------- */

void test_tfx() {
    /* Ct = (0x40, 0x80, 0xFF) with At 0x40; Cf = (0x80, 0x40, 0x80) with
     * Af 0x20. Alpha's one is 0x80, so a product shifts right by seven.
     *
     *   MODULATE  R (0x40*0x80)>>7 = 0x40   G (0x80*0x40)>>7 = 0x40
     *             B (0xFF*0x80)>>7 = 0xFF   A (0x40*0x20)>>7 = 0x10
     */
    const uint32_t ct = 0x40FF8040u;   /* 0xAABBGGRR */
    const uint32_t cf = 0x20804080u;
    check_u("TFX MODULATE, TCC 1",
            gsr::gs_tex_function(ct, cf, GS_TFX_MODULATE, 1), 0x10FF4040u);
    check_u("TFX MODULATE, TCC 0 keeps the fragment alpha",
            gsr::gs_tex_function(ct, cf, GS_TFX_MODULATE, 0), 0x20FF4040u);

    /* DECAL is the texel, and its alpha when TCC says so. */
    check_u("TFX DECAL, TCC 1", gsr::gs_tex_function(ct, cf, GS_TFX_DECAL, 1), ct);
    check_u("TFX DECAL, TCC 0", gsr::gs_tex_function(ct, cf, GS_TFX_DECAL, 0),
            0x20FF8040u);

    /* HIGHLIGHT adds Af to every colour channel and to the alpha:
     *   R 0x40 + 0x20 = 0x60   G 0x40 + 0x20 = 0x60   B 0xFF + 0x20 clamps
     *   A 0x40 + 0x20 = 0x60
     */
    check_u("TFX HIGHLIGHT, TCC 1",
            gsr::gs_tex_function(ct, cf, GS_TFX_HIGHLIGHT, 1), 0x60FF6060u);
    check_u("TFX HIGHLIGHT, TCC 0",
            gsr::gs_tex_function(ct, cf, GS_TFX_HIGHLIGHT, 0), 0x20FF6060u);

    /* HIGHLIGHT2 is the same colour and the texel's own alpha. */
    check_u("TFX HIGHLIGHT2, TCC 1",
            gsr::gs_tex_function(ct, cf, GS_TFX_HIGHLIGHT2, 1), 0x40FF6060u);
    check_u("TFX HIGHLIGHT2, TCC 0",
            gsr::gs_tex_function(ct, cf, GS_TFX_HIGHLIGHT2, 0), 0x20FF6060u);
}

/* ---- 5. the CLUT cache ----------------------------------------------------- */

uint64_t make_tex0(uint32_t psm, uint32_t cbp, uint32_t cpsm, uint32_t csm,
                   uint32_t csa, uint32_t cld) {
    return ((uint64_t)(psm & 0x3F) << 20)
         | ((uint64_t)(cbp & 0x3FFF) << 37)
         | ((uint64_t)(cpsm & 0xF) << 51)
         | ((uint64_t)(csm & 1) << 55)
         | ((uint64_t)(csa & 0x1F) << 56)
         | ((uint64_t)(cld & 7) << 61);
}

/* A palette written into local memory as the 16 by 16 region CSM1 reads:
 * entry value 0xFF000000 | (slot * 0x010101), so every slot is distinct and
 * a permuted read is visible in the value itself. */
void write_palette(gsr::LocalMemory& mem, uint32_t cbp) {
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            const uint32_t slot = y * 16 + x;
            mem.write_pixel(GS_PSMCT32, cbp, 1, x, y, 0xFF000000u | (slot * 0x010101u));
        }
    }
}

void test_clut_cache() {
    gsr::LocalMemory mem;
    gsr::ClutCache clut;
    clut.set_memory(&mem);
    const uint32_t cbp_a = 100, cbp_b = 200;
    write_palette(mem, cbp_a);
    write_palette(mem, cbp_b);
    /* Make the second palette tell itself apart from the first. */
    mem.write_pixel(GS_PSMCT32, cbp_b, 1, 0, 0, 0xDEADBEEFu);

    /* CLD 0 loads nothing at all. */
    clut.tex0_written(make_tex0(GS_PSMT8, cbp_a, GS_PSMCT32, 0, 0, 0), 0);
    check_u("CLD 0 loads nothing", (uint32_t)clut.stats().loads, 0);

    /* CLD 1 loads. The buffer is filled in the source's raster order, so
     * slot s holds the pixel at (s & 15, s >> 4), and a lookup of index i
     * goes through the arrangement. */
    clut.tex0_written(make_tex0(GS_PSMT8, cbp_a, GS_PSMCT32, 0, 0, 1), 0);
    check_u("CLD 1 loads", (uint32_t)clut.stats().loads, 1);
    check_u("CLUT slot 0", clut.words()[0], 0xFF000000u);
    check_u("CLUT slot 130", clut.words()[130], 0xFF828282u);
    const uint32_t entry_of_8 = gsr::gs_clut_entry(8, 8, 0, 0);
    check_u("index 8 reads slot 16", clut.words()[entry_of_8], 0xFF101010u);

    /* CLD 2 loads and remembers CBP in CBP0; CLD 4 then loads only when CBP
     * differs from CBP0, and stores it either way. */
    clut.tex0_written(make_tex0(GS_PSMT8, cbp_a, GS_PSMCT32, 0, 0, 2), 0);
    check_u("CLD 2 loads", (uint32_t)clut.stats().loads, 2);
    clut.tex0_written(make_tex0(GS_PSMT8, cbp_a, GS_PSMCT32, 0, 0, 4), 0);
    check_u("CLD 4 with the same CBP does not load", (uint32_t)clut.stats().loads, 2);
    clut.tex0_written(make_tex0(GS_PSMT8, cbp_b, GS_PSMCT32, 0, 0, 4), 0);
    check_u("CLD 4 with a new CBP loads", (uint32_t)clut.stats().loads, 3);
    check_u("the new palette is the one in the buffer", clut.words()[0], 0xDEADBEEFu);
    clut.tex0_written(make_tex0(GS_PSMT8, cbp_b, GS_PSMCT32, 0, 0, 4), 0);
    check_u("CLD 4 remembered the new CBP", (uint32_t)clut.stats().loads, 3);

    /* CBP1 is a second, independent register: CLD 5 against a CBP0 that
     * matches still loads, because it compares against CBP1. */
    clut.tex0_written(make_tex0(GS_PSMT8, cbp_b, GS_PSMCT32, 0, 0, 5), 0);
    check_u("CLD 5 compares against CBP1, not CBP0", (uint32_t)clut.stats().loads, 4);
    clut.tex0_written(make_tex0(GS_PSMT8, cbp_b, GS_PSMCT32, 0, 0, 5), 0);
    check_u("CLD 5 then remembers it", (uint32_t)clut.stats().loads, 4);
    clut.tex0_written(make_tex0(GS_PSMT8, cbp_a, GS_PSMCT32, 0, 0, 4), 0);
    check_u("CBP0 still holds the other base", (uint32_t)clut.stats().loads, 5);

    /* The serial only moves when the contents do, which is what lets a batch
     * hold one snapshot per distinct palette. */
    const uint32_t serial = clut.serial();
    clut.tex0_written(make_tex0(GS_PSMT8, cbp_a, GS_PSMCT32, 0, 0, 0), 0);
    check_u("a skipped load does not move the serial", clut.serial(), serial);

    /* A 4-bit palette lands in the group of sixteen CSA selects, and leaves
     * the rest of the buffer alone. */
    gsr::ClutCache clut4;
    clut4.set_memory(&mem);
    clut4.tex0_written(make_tex0(GS_PSMT4, cbp_a, GS_PSMCT32, 0, 2, 1), 0);
    check_u("4-bit CSA 2 fills slot 32", clut4.words()[32], 0xFF000000u);
    /* The 8 by 2 region's last texel is the palette's (7, 1), which the
     * writer above filled with 1 * 16 + 7. */
    check_u("4-bit CSA 2 fills slot 47", clut4.words()[47], 0xFF171717u);
    check_u("4-bit CSA 2 leaves slot 0", clut4.words()[0], 0u);
}

/* ---- 6. the page tracker --------------------------------------------------- */

void test_page_tracker() {
    /* A 512 by 448 PSMCT32 frame buffer at page 64 covers pages 64 to 175:
     * eight pages across and fourteen rows of 32 lines. */
    gsr::PageSet fb;
    gsr::gs_mark_pages(GS_PSMCT32, 64 * GS_PAGE_BLOCKS, 8, 0, 0, 511, 447, &fb);
    gsr::PageSet probe;
    probe.add(64);
    if (!fb.intersects(probe)) fail("frame buffer marks its first page", "no", "yes");
    probe.clear();
    probe.add(175);
    if (!fb.intersects(probe)) fail("frame buffer marks its last page", "no", "yes");
    probe.clear();
    probe.add(176);
    if (fb.intersects(probe)) fail("frame buffer stops at its last page", "yes", "no");

    /* A 256 by 256 PSMCT32 texture at block 2048, which is page 64: the
     * feedback case, and it has to be seen. */
    gsr::PageSet tex;
    gsr::gs_mark_pages(GS_PSMCT32, 2048, 4, 0, 0, 255, 255, &tex);
    if (!tex.intersects(fb)) {
        fail("a texture inside the frame buffer is feedback", "no overlap", "overlap");
    }

    /* The same texture moved to page 300 is not. */
    gsr::PageSet away;
    gsr::gs_mark_pages(GS_PSMCT32, 300 * GS_PAGE_BLOCKS, 4, 0, 0, 255, 255, &away);
    if (away.intersects(fb)) {
        fail("a texture clear of the frame buffer is not feedback", "overlap", "no overlap");
    }

    /* A small primitive marks only the pages it lands in, not the whole
     * buffer: pixels 0..15 of line 0 are one page. */
    gsr::PageSet one;
    gsr::gs_mark_pages(GS_PSMCT32, 64 * GS_PAGE_BLOCKS, 8, 0, 0, 15, 15, &one);
    probe.clear();
    probe.add(65);
    if (one.intersects(probe)) fail("a 16x16 primitive marks one page", "two", "one");

    /* A base that is not page aligned straddles two pages, and both are
     * marked; block 2049 is one block into page 64. */
    gsr::PageSet un;
    gsr::gs_mark_pages(GS_PSMCT32, 2049, 1, 0, 0, 63, 31, &un);
    probe.clear();
    probe.add(65);
    if (!un.intersects(probe)) {
        fail("an unaligned base straddles the next page", "one page", "two pages");
    }

    /* A word range covers the pages it spans: 2048 words to a page. */
    gsr::PageSet words;
    gsr::gs_mark_page_words(2047, 2049, &words);
    probe.clear();
    probe.add(0);
    if (!words.intersects(probe)) fail("word range marks page 0", "no", "yes");
    probe.clear();
    probe.add(1);
    if (!words.intersects(probe)) fail("word range marks page 1", "no", "yes");
    probe.clear();
    probe.add(2);
    if (words.intersects(probe)) fail("word range stops at page 1", "yes", "no");
}

/* ---- 7. the mip chain ------------------------------------------------------ */

void test_mip_bases() {
    /* TW 8, TH 8: a 256 by 256 PSMCT32 texture at block 100, MXL 2.
     * MIPTBP1 names levels 1 to 3 explicitly. */
    const uint64_t t0 = ((uint64_t)100)
                      | ((uint64_t)4 << 14)          /* TBW 4 */
                      | ((uint64_t)GS_PSMCT32 << 20)
                      | ((uint64_t)8 << 26)          /* TW */
                      | ((uint64_t)8 << 30);         /* TH */
    const uint32_t t0lo = (uint32_t)t0;
    const uint32_t t0hi = (uint32_t)(t0 >> 32);
    const uint64_t mip1 = (uint64_t)500 | ((uint64_t)2 << 14)
                        | ((uint64_t)600 << 20) | ((uint64_t)1 << 34)
                        | ((uint64_t)700 << 40) | ((uint64_t)1 << 54);
    const uint32_t m1lo = (uint32_t)mip1;
    const uint32_t m1hi = (uint32_t)(mip1 >> 32);

    const uint32_t tex1_explicit = (2u << 2);   /* MXL 2, MTBA 0 */
    check_u("level 0 base is TBP0",
            gsr::gs_mip_tbp(t0lo, t0hi, tex1_explicit, m1lo, m1hi, 0, 0, 0), 100);
    check_u("level 1 base from MIPTBP1",
            gsr::gs_mip_tbp(t0lo, t0hi, tex1_explicit, m1lo, m1hi, 0, 0, 1), 500);
    check_u("level 2 base from MIPTBP1",
            gsr::gs_mip_tbp(t0lo, t0hi, tex1_explicit, m1lo, m1hi, 0, 0, 2), 600);
    check_u("level 3 base from MIPTBP1",
            gsr::gs_mip_tbp(t0lo, t0hi, tex1_explicit, m1lo, m1hi, 0, 0, 3), 700);
    check_u("level 2 width from MIPTBP1",
            gsr::gs_mip_tbw(t0lo, tex1_explicit, m1lo, m1hi, 0, 0, 2), 1);

    /* MTBA 1 packs the levels one after another instead. A 256 by 256
     * PSMCT32 level is 256*256*4/256 = 1024 blocks, and the next is a
     * quarter of that. */
    const uint32_t tex1_auto = (2u << 2) | (1u << 9);
    check_u("automatic level 1 base",
            gsr::gs_mip_tbp(t0lo, t0hi, tex1_auto, 0, 0, 0, 0, 1), 100 + 1024);
    check_u("automatic level 2 base",
            gsr::gs_mip_tbp(t0lo, t0hi, tex1_auto, 0, 0, 0, 0, 2), 100 + 1024 + 256);
    check_u("automatic level 1 width",
            gsr::gs_mip_tbw(t0lo, tex1_auto, 0, 0, 0, 0, 1), 2);

    /* A level never shrinks below one texel, so a chain deeper than the
     * texture is wide still addresses something. */
    check_u("level size floor", gsr::gs_tex_level_size(2, 5), 1);
}

/* ---- the LOD formula ------------------------------------------------------- */

void test_lod() {
    /* LCM 1 takes K outright, in sixteenths. */
    check_i("LCM 1 uses K", gsr::gs_tex_lod(1, 0, -32, 0.25f, 3), -32);
    /* LCM 0 with L 0 and K 0: Q of 1/4 means the texture is minified by
     * four, so log2(4) = 2 and the LOD is 32 sixteenths. */
    check_i("LCM 0, Q a quarter", gsr::gs_tex_lod(0, 0, 0, 0.25f, 3), 32);
    /* L doubles the term, K shifts it. */
    check_i("LCM 0 with L 1", gsr::gs_tex_lod(0, 1, 0, 0.25f, 3), 64);
    check_i("LCM 0 with K", gsr::gs_tex_lod(0, 0, -16, 0.25f, 3), 16);
    /* Q of one is neither magnification nor minification. */
    check_i("LCM 0, Q one", gsr::gs_tex_lod(0, 0, 0, 1.0f, 3), 0);
    /* Magnification is a negative LOD, which is what selects MMAG. */
    if (gsr::gs_tex_lod(0, 0, 0, 4.0f, 3) >= 0) {
        fail("Q above one is magnification", "LOD >= 0", "LOD < 0");
    }
    /* Q of zero has no defined LOD. The substitution is MXL, the last level
     * this texture has, so the pin follows the texture rather than a literal.
     * docs/GS_RENDERER.md lists it as inferred. */
    check_i("Q of zero pins LOD at MXL", gsr::gs_tex_lod(0, 0, 0, 0.0f, 3), 48);
    check_i("and at MXL 0 that is level 0", gsr::gs_tex_lod(0, 0, 0, 0.0f, 0), 0);
}

/* ---- the bilinear blend ---------------------------------------------------- */

void test_bilerp() {
    /* A fraction of zero is the first texel exactly, which is the texel
     * centre rule's whole point. */
    check_u("bilerp at a texel centre",
            gsr::gs_tex_bilerp(0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u, 0, 0),
            0x11223344u);
    /* Halfway along x between 0 and 0x40 is 0x20. */
    check_u("bilerp halfway in x",
            gsr::gs_tex_bilerp(0u, 0x40404040u, 0u, 0x40404040u, 8, 0), 0x20202020u);
    /* Halfway in both axes over four corners of 0, 0x40, 0x40 and 0x80 is
     * 0x40. */
    check_u("bilerp halfway in both",
            gsr::gs_tex_bilerp(0u, 0x40404040u, 0x40404040u, 0x80808080u, 8, 8),
            0x40404040u);
}

} // namespace

int main() {
    test_clamp();
    test_clut_arrangement();
    test_texa();
    test_tfx();
    test_clut_cache();
    test_page_tracker();
    test_mip_bases();
    test_lod();
    test_bilerp();
    if (g_failures) {
        std::printf("gs-texture-selftest: %d failures\n", g_failures);
        return 1;
    }
    std::printf("gs-texture-selftest: pass.\n");
    std::printf("  Not settled here, and each one is on docs/GS_RENDERER.md's inferred\n"
                "  list. This selftest checks that the code does what the code says;\n"
                "  none of the three is a claim it can test.\n"
                "    - The CSM1 16 by 16 arrangement. Read off the manual's CLUT diagram\n"
                "      as an exchange of bits 3 and 4. tools/gs_gen_dump writes its\n"
                "      palettes through a second transcription of that same sentence and\n"
                "      cross-checks the two, so a change to one is caught, but both read\n"
                "      the same diagram. One captured 8-bit palette upload settles it.\n"
                "    - CSA on an 8-bit texture, ignored here. With a 16-bit CLUT a\n"
                "      256-entry palette fills half the buffer and CSA 16 would name the\n"
                "      other half. No case in the corpus uses a nonzero CSA.\n"
                "    - The LOD substituted for a Q of zero or a denormal, MXL * 16.\n"
                "      The hardware's behaviour there is not stated anywhere read here.\n");
    return 0;
}
