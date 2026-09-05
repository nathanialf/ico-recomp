/* gs/render/gs_swizzle_selftest.cpp: icorecomp-gs-swizzle-selftest.
 *
 * Round-trips GS local memory addressing (gs_swizzle.h, the header the C++
 * renderer and the GLSL shaders share) against gs_swizzle_reference.cpp, a
 * separately written implementation built from the manual's literal column
 * and page tables. Needs no GPU, no disc and no runtime services, so it runs
 * on any build host and in CI.
 *
 * Three checks, and the third exists because the second cannot cover PSMT4:
 *
 *   1. Address equality. For every format that the reference holds a column
 *      table for, every pixel of a 128x128 region at several buffer bases and
 *      widths must land on the same byte of local memory in both
 *      implementations.
 *
 *   2. Bijectivity. Over one page of each format, the addresses of the page's
 *      own pixels must be a permutation of that page's storage units. A
 *      swizzle that collides loses pixels; one that leaves gaps addresses
 *      memory the page does not own. This catches errors the equality check
 *      cannot, because it does not depend on the reference at all.
 *
 *   3. Placement. For PSMT4, whose column diagram this project has not
 *      transcribed (see gs_swizzle_reference.cpp), the block and the 64-byte
 *      column each pixel lands in must still match the reference, and check 2
 *      still applies inside the block. That leaves exactly one thing
 *      unverified: the order of the 512 nibbles inside a PSMT4 column. The
 *      run says so on its last line rather than reporting a clean pass over
 *      something it did not test.
 *
 * Exit status 0 when every check passed, 1 otherwise. Failures print the
 * format, the coordinate and both answers.
 */
#include "gs_swizzle.h"
#include "gs_swizzle_reference.h"
#include "gs_vram.h"

#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

struct Format {
    uint32_t psm;
    const char* name;
};

const Format kFormats[] = {
    { GS_PSMCT32,  "PSMCT32"  },
    { GS_PSMCT24,  "PSMCT24"  },
    { GS_PSMCT16,  "PSMCT16"  },
    { GS_PSMCT16S, "PSMCT16S" },
    { GS_PSMT8,    "PSMT8"    },
    { GS_PSMT4,    "PSMT4"    },
    { GS_PSMT8H,   "PSMT8H"   },
    { GS_PSMT4HL,  "PSMT4HL"  },
    { GS_PSMT4HH,  "PSMT4HH"  },
    { GS_PSMZ32,   "PSMZ32"   },
    { GS_PSMZ24,   "PSMZ24"   },
    { GS_PSMZ16,   "PSMZ16"   },
    { GS_PSMZ16S,  "PSMZ16S"  },
};

/* Buffer bases and widths the checks sweep. The bases are block numbers, so
 * 0 is the start of memory, 32 is one page in, and 8191 is a base whose page
 * arithmetic wraps a 4 MiB store on a wide buffer. FBW values are in 64-pixel
 * units: 8 is the 512-pixel gameplay buffer, 12 the 768-pixel one the movie's
 * 720-wide buffer needs, and 1 is the narrowest legal width. */
const uint32_t kBases[] = { 0, 32, 96, 8191 };
const uint32_t kWidths[] = { 1, 8, 12 };

int g_failures = 0;

void fail(const char* what, const char* fmt_name, uint32_t base, uint32_t fbw,
          uint32_t x, uint32_t y, unsigned long long ours, unsigned long long theirs) {
    if (g_failures < 20) {
        std::printf("FAIL %s: %s base=%u fbw=%u (%u,%u) header=%llu reference=%llu\n",
                    what, fmt_name, base, fbw, x, y, ours, theirs);
    }
    ++g_failures;
}

/* Check 1: the shared header and the reference agree on the byte. */
void check_equality(const Format& f) {
    if (!gsref::has_column_table(f.psm)) return;
    const uint32_t bits = gsr::gs_addr_bits(f.psm);
    for (uint32_t base : kBases) {
        for (uint32_t fbw : kWidths) {
            for (uint32_t y = 0; y < 128; ++y) {
                for (uint32_t x = 0; x < 128; ++x) {
                    const uint64_t addr = gsr::gs_pixel_addr(f.psm, base, fbw, x, y);
                    const uint64_t byte_addr = (addr * bits) / 8;
                    const uint32_t bit_in_byte = (uint32_t)((addr * bits) % 8);
                    const gsref::Address ref = gsref::address(f.psm, base, fbw, x, y);
                    if (byte_addr != ref.byte_addr) {
                        fail("byte", f.name, base, fbw, x, y, byte_addr, ref.byte_addr);
                    } else if (bit_in_byte != ref.bit_in_byte) {
                        fail("bit", f.name, base, fbw, x, y, bit_in_byte, ref.bit_in_byte);
                    }
                }
            }
        }
    }
}

/* Check 2: one page of pixels covers that page's storage exactly once. */
void check_bijective(const Format& f) {
    const uint32_t fam = gsr::gs_psm_family(f.psm);
    const uint32_t pw = gsr::gs_page_width(fam);
    const uint32_t ph = gsr::gs_page_height(fam);
    const uint32_t bits = gsr::gs_addr_bits(f.psm);
    const uint32_t units_per_page = (8192u * 8u) / bits;

    /* One page wide, so the page index is always 0 and every address lands in
     * the first page of the buffer. */
    const uint32_t fbw = pw / 64u ? pw / 64u : 1u;
    std::vector<uint8_t> seen(units_per_page, 0);
    for (uint32_t y = 0; y < ph; ++y) {
        for (uint32_t x = 0; x < pw; ++x) {
            const uint32_t addr = gsr::gs_pixel_addr(f.psm, 0, fbw, x, y);
            if (addr >= units_per_page) {
                fail("page range", f.name, 0, fbw, x, y, addr, units_per_page);
                continue;
            }
            if (seen[addr]) {
                fail("collision", f.name, 0, fbw, x, y, addr, addr);
                continue;
            }
            seen[addr] = 1;
        }
    }
    for (uint32_t i = 0; i < units_per_page; ++i) {
        if (!seen[i]) {
            fail("gap", f.name, 0, fbw, 0, 0, i, i);
            break;
        }
    }
}

/* Check 3: block and column placement, which holds for every format
 * including the one with no literal column table. */
void check_placement(const Format& f) {
    const uint32_t bits = gsr::gs_addr_bits(f.psm);
    for (uint32_t base : kBases) {
        for (uint32_t fbw : kWidths) {
            for (uint32_t y = 0; y < 128; ++y) {
                for (uint32_t x = 0; x < 128; ++x) {
                    const uint64_t addr = gsr::gs_pixel_addr(f.psm, base, fbw, x, y);
                    const uint64_t byte_addr = (addr * bits) / 8;
                    const gsref::Address ref = gsref::address(f.psm, base, fbw, x, y);
                    const uint32_t block = (uint32_t)(byte_addr / GS_BLOCK_BYTES);
                    const uint32_t column = (uint32_t)((byte_addr % GS_BLOCK_BYTES) / 64);
                    if (block != ref.block) {
                        fail("block", f.name, base, fbw, x, y, block, ref.block);
                    } else if (column != ref.column) {
                        fail("column", f.name, base, fbw, x, y, column, ref.column);
                    }
                }
            }
        }
    }
}

/* Check 4, PSMT4 only: the word a 4-bit pixel lands in must be the word the
 * literal 8-bit column table puts the same block-local pixel in.
 *
 * A PSMT4 column and a PSMT8 column are the same 64 bytes, and for x below 16
 * the two formats pick that word from the same table entry, so this compares
 * the header's PSMT4 rule against a transcribed table rather than against
 * itself. It leaves exactly one thing unverified: which of the eight nibbles
 * inside that word the pixel occupies. Both formats have block-local (0,0) at
 * page 0 block 0 for x < 16 and y < 16, which is why the coordinates can be
 * compared directly.
 *
 * Verified for x < 16. For x in 16..31 the header adds 4 to the nibble and
 * keeps the word, which is the part of the manual's diagram this project has
 * not transcribed; check 2 still proves it collides with nothing. */
void check_psmt4_word_against_psmt8() {
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            const uint32_t nib = gsr::gs_pixel_addr(GS_PSMT4, 0, 2, x, y);
            const uint32_t word4 = (nib % 512) / 8;
            const gsref::Address ref8 = gsref::address(GS_PSMT8, 0, 2, x, y);
            const uint32_t word8 = (ref8.byte_addr % 256) / 4;
            if (word4 != word8) {
                fail("PSMT4 word vs PSMT8 table", "PSMT4", 0, 2, x, y, word4, word8);
            }
        }
    }
}

/* The 16-bit expansion the CRTC and the texture path both use. Checked here
 * because it is in the same shared header and a shader cannot be unit
 * tested on this host. */
void check_expand16() {
    struct Case { uint32_t v; uint32_t rgba; };
    const Case cases[] = {
        { 0x0000u, 0x00000000u },              /* all zero */
        { 0x001Fu, 0x000000F8u },              /* R = 31 -> 0xF8 */
        { 0x03E0u, 0x0000F800u },              /* G = 31 */
        { 0x7C00u, 0x00F80000u },              /* B = 31 */
        { 0x8000u, 0x80000000u },              /* A = 1 -> 0x80, the GS "one" */
        { 0xFFFFu, 0x80F8F8F8u },
    };
    for (const Case& c : cases) {
        const uint32_t got = gsr::gs_expand16(c.v);
        if (got != c.rgba) {
            std::printf("FAIL expand16: 0x%04x -> 0x%08x, expected 0x%08x\n",
                        c.v, got, c.rgba);
            ++g_failures;
        }
    }
}

} // namespace

/* Check 5: the store and load path, not the address.
 *
 * The three checks above settle where a pixel lives. They say nothing about
 * whether LocalMemory::write_pixel puts the value there and read_pixel gets it
 * back, and that is a different piece of arithmetic: a shift and a mask for
 * the sub-word formats, and gs_word_mask/gs_word_shift for the three H
 * formats whose field is a byte or a nibble of a 32-bit word. A fault there
 * loses an upload while every address check stays green, which is the shape
 * of the PSMT4 uploads that landed nothing on 2026-09-04.
 *
 * Every texel of a region gets a distinct value, and every texel is read back.
 * Distinct values are what makes a store that lands on a neighbour's bits
 * visible: two texels sharing storage would each read the other's value. */
void check_store_load(const Format& f) {
    const uint32_t bits = gsr::gs_addr_bits(f.psm);
    if (bits == 0) return;
    /* The width of the field the format actually stores, which for the H
     * formats is narrower than the 32 bits they address. */
    const uint32_t field = gsr::gs_transfer_bits(f.psm);
    if (field == 0) return;

    static gsr::LocalMemory mem;   /* 4 MiB; reused across formats */
    const uint32_t w = 64, h = 64;
    const uint32_t bw = 2;
    const uint32_t base = 32;      /* one page in, so base_block is exercised */
    const uint32_t mask = field >= 32 ? 0xFFFFFFFFu : ((1u << field) - 1u);

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t v = ((y * w + x) * 2654435761u) & mask;
            mem.write_pixel(f.psm, base, bw, x, y, v);
        }
    }
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t want = ((y * w + x) * 2654435761u) & mask;
            const uint32_t got = mem.read_pixel(f.psm, base, bw, x, y);
            if (got != want) {
                ++g_failures;
                if (g_failures < 20) {
                    std::printf("  FAIL %s store/load at %u,%u: wrote 0x%08x read 0x%08x\n",
                                f.name, x, y, want, got);
                }
                return;
            }
        }
    }
}

/* Check 6: the block ownership tracker's rules and its run coalescing.
 *
 * No device is needed for any of it: the bitmaps, the spans that reject a
 * query cheaply, and the run walk are all host logic, and they are what the
 * five rules in gs_native.cpp's drawing comment are built on. A fault here is
 * a lost upload or a lost draw, which is what the span-based predecessor
 * produced on 2026-09-05.
 */
void check_ownership() {
    using LM = gsr::LocalMemory;
    const int before = g_failures;
    auto fail_if = [&](bool bad, const char* what) {
        if (!bad) return;
        ++g_failures;
        std::printf("  FAIL ownership: %s\n", what);
    };

    {
        /* A host write marks whole blocks, and only the blocks it touches. */
        LM m;
        m.write_pixel(GS_PSMCT32, 0, 8, 3, 0, 0x1234u);  /* word 3, block 0 */
        fail_if(!m.host_block(0), "a host write did not mark its own block");
        fail_if(m.host_block(1), "a host write marked a block it did not touch");
        fail_if(m.any_device(), "a host write marked a device block");
        fail_if(m.host_lo() != 0 || m.host_hi() != 1, "the host span is not the one block");
    }

    {
        /* Rule 4, and the invariant that a block is in at most one bitmap
         * once the ordering is respected: upload, then draw. */
        LM m;
        m.note_host_words(0, 64);
        m.clear_host_blocks(0, 1);                     /* the upload */
        m.note_device_words(0, 64);                    /* then the draw */
        fail_if(m.host_block(0), "a drawn block stayed host-dirty after the upload");
        fail_if(!m.device_block(0), "a drawn block was not marked device-written");
        fail_if(m.both_owners() != 0, "a correct order was counted as two owners");
    }

    {
        /* And that the counter fires when the order is not respected. */
        LM m;
        m.note_device_words(0, 64);
        m.note_host_words(0, 64);                      /* without reconciling */
        fail_if(m.both_owners() == 0, "two owners on one block was not counted");
    }

    {
        /* Run coalescing: adjacent blocks are one run, a gap splits them, and
         * the walk stops at the end it was given. */
        LM m;
        m.note_host_words(LM::block_word(4), LM::block_word(7));    /* 4,5,6 */
        m.note_host_words(LM::block_word(9), LM::block_word(10));   /* 9 */
        uint32_t b0 = 0, b1 = 0;
        bool ok = LM::next_run(m.host_bits(), 0, LM::kBlocks, &b0, &b1);
        fail_if(!ok || b0 != 4 || b1 != 7, "the first run is not blocks 4..7");
        ok = LM::next_run(m.host_bits(), b1, LM::kBlocks, &b0, &b1);
        fail_if(!ok || b0 != 9 || b1 != 10, "the second run is not block 9");
        ok = LM::next_run(m.host_bits(), b1, LM::kBlocks, &b0, &b1);
        fail_if(ok, "a third run was found where there are two");
        ok = LM::next_run(m.host_bits(), 0, 5, &b0, &b1);
        fail_if(!ok || b0 != 4 || b1 != 5, "the walk did not stop at the end it was given");
    }

    {
        /* A partial word at either end still marks the whole block, because a
         * block is the unit both copies move in. */
        LM m;
        m.note_host_words(63, 65);
        fail_if(!m.host_block(0) || !m.host_block(1),
                "a range straddling a block boundary marked only one of them");
        fail_if(m.host_block(2), "a range straddling a boundary marked a third block");
    }

    {
        /* mark_all_dirty is the whole store, host side only. */
        LM m;
        m.note_device_words(0, GS_VRAM_WORDS);
        m.mark_all_dirty();
        fail_if(!m.host_block(0) || !m.host_block(LM::kBlocks - 1),
                "mark_all_dirty left blocks unmarked");
        fail_if(m.any_device(), "mark_all_dirty left device blocks marked");
    }

    std::printf("  %-9s %-8s block ownership: marks, order, runs, boundaries\n",
                "tracker", g_failures == before ? "ok" : "FAILED");
}

int main() {
    std::printf("gs-swizzle-selftest: %zu formats, 128x128 region, %zu bases, %zu widths\n",
                sizeof(kFormats) / sizeof(kFormats[0]),
                sizeof(kBases) / sizeof(kBases[0]),
                sizeof(kWidths) / sizeof(kWidths[0]));

    for (const Format& f : kFormats) {
        const int before = g_failures;
        check_equality(f);
        check_bijective(f);
        check_placement(f);
        check_store_load(f);
        const char* how = gsref::has_column_table(f.psm)
            ? "address + placement + bijective + store/load"
            : "placement + bijective + word + store/load (no literal column table)";
        std::printf("  %-9s %-8s %s\n", f.name,
                    g_failures == before ? "ok" : "FAILED", how);
    }
    check_psmt4_word_against_psmt8();
    check_expand16();
    check_ownership();

    if (g_failures) {
        std::printf("gs-swizzle-selftest: %d failures\n", g_failures);
        return 1;
    }
    std::printf("gs-swizzle-selftest: pass.\n");
    std::printf("  Not covered: which of the eight nibbles inside a word a PSMT4 pixel\n"
                "  occupies. Its block, its column and its word are all checked against\n"
                "  transcribed tables; the nibble order is inferred from the 8-bit column\n"
                "  rule, which check 1 does verify against a transcribed table for PSMT8.\n"
                "  See gs_swizzle_reference.cpp and docs/GS_RENDERER.md.\n");
    return 0;
}
