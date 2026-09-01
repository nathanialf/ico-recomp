/* hw/ipu.cpp: EE IPU (image processor) model, sized to this one binary.
 *
 * The retail ELF drives the IPU from Sony's libmpeg/libipu (translated
 * vendor code) to play the single FMV (attract movie). The measured command
 * census of that driver is: BCLR, BDEC, VDEC, FDEC, SETIQ, SETVQ, CSC,
 * SETTH. IDEC and PACK are never issued and are loud-fatal stubs here, per
 * the coverage policy (no speculative ops).
 *
 * Model:
 *   - Bitstream input is pulled on demand from the toIPU DMA (ch4, normal
 *     and source chain) into an internal byte queue; an 8-qword prefill
 *     mimics the hardware input FIFO so IPU_CTRL.IFC / IPU_BP / D4_MADR
 *     readbacks stay consistent with what libmpeg's ring-buffer position
 *     arithmetic expects. The ch4 transfer completes (STR clear, D_STAT
 *     bit 4) as soon as its source is fully pulled.
 *   - Commands execute synchronously at the IPU_CMD write. A command that
 *     runs out of bitstream rewinds to its start and stays pending (BUSY
 *     reads 1); it is retried after the next ch4 kick supplies data. This
 *     mirrors hardware stalling on an empty FIFO without partial-decode
 *     bookkeeping.
 *   - Output is an unbounded byte queue drained by the fromIPU DMA (ch3,
 *     normal mode). A ch3 kick that cannot be satisfied yet stays pending
 *     (STR stays set) and completes when a command produces enough data.
 *   - Command completion raises INTC cause 8 (IPU), matching hardware.
 *
 * Sources: EE User's Manual IPU chapter and ps2tek for the register layout,
 * command encodings and the documented integer CSC method. ISO/IEC 13818-2
 * tables B-1..B-15 for the VLC code assignments (transcribed below in this
 * file's own table format; the code/value assignments are facts fixed by
 * the standard). PCSX2's IPU was read as a behavioral reference for the
 * result-register encodings that the manual leaves vague (VDEC result =
 * value | length << 16, the CMD-read bitstream peek, the BDEC trailing
 * start-code scan, CTRL write masking); no code was copied from it, per the
 * license rules in CLAUDE.md. The IDCT is this file's own implementation of
 * the standard's definition (Annex A) in double precision.
 */
#include "hw.h"

#include "../ee/kernel.h"
#include "../prof.h"

#include <cinttypes>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

constexpr int RT_INTC_IPU = 8; /* ee/kernel.h cause numbering */

/* ---- guest memory access ------------------------------------------------- */

uint8_t* ipu_dma_ptr(uint32_t addr) {
    if (addr & 0x80000000u) {
        uint8_t* page = g_pages[0x70000000u >> 16];
        return page + (addr & 0x3FFF);
    }
    uint8_t* p = g_pages[addr >> 16];
    if (!p) {
        rt_fatal("ipu", nullptr, "IPU DMA touches unmapped guest address 0x%08x", addr);
    }
    return p + (addr & 0xFFFF);
}

/* ---- toIPU DMA (ch4) pull source ----------------------------------------- */

struct ToIpuState {
    bool active = false;
    bool chain = false;
    bool tag_end = false; /* current payload is the last (REFE/END or IRQ+TIE) */
    uint64_t kicks = 0;
    uint64_t tags = 0;
};
ToIpuState g_to;

struct FromIpuState {
    bool active = false;
    bool drain_all = false; /* QWC=0 kick: complete when the output queue is empty */
    uint64_t kicks = 0;
};
FromIpuState g_from;

/* dmac.cpp register file pointers (0 CHCR, 1 MADR, 2 QWC, 3 TADR). */
uint32_t* g_ch3_reg[4];
uint32_t* g_ch4_reg[4];
bool g_reg_bound = false;

void bind_regs() {
    if (g_reg_bound) return;
    for (int i = 0; i < 4; ++i) {
        g_ch3_reg[i] = rt_dmac_ipu_reg(3, i);
        g_ch4_reg[i] = rt_dmac_ipu_reg(4, i);
    }
    g_reg_bound = true;
}

/* ---- bitstream input ----------------------------------------------------- */

std::vector<uint8_t> g_in;   /* pulled input bytes */
size_t g_in_pos = 0;         /* consumed bits within g_in */
uint64_t g_bp_bits = 0;      /* absolute bit position since BCLR (for IPU_BP) */
bool g_underflow = false;    /* set when a decode ran out of input */

void complete_ch4() {
    g_to.active = false;
    *g_ch4_reg[0] &= ~0x100u; /* STR */
    rt_dmac_raise(4);
    if (rt_trace() || is_pow2(g_to.kicks)) {
        rt_log("ipu", "toIPU ch4 transfer complete (madr=0x%08x tadr=0x%08x) [kick #%" PRIu64 "]",
            *g_ch4_reg[1], *g_ch4_reg[3], g_to.kicks);
    }
}

/* Walks the ch4 source (normal or source chain) one qword at a time. */
bool pull_qword() {
    if (!g_to.active) return false;
    if (!(*g_ch4_reg[0] & 0x100u)) {
        /* The driver stopped the channel manually (libmpeg's stop/restart
         * sequence writes CHCR without STR under D_ENABLE suspend). */
        g_to.active = false;
        return false;
    }
    for (;;) {
        if (*g_ch4_reg[2] > 0) {
            uint32_t madr = *g_ch4_reg[1];
            size_t base = g_in.size();
            g_in.resize(base + 16);
            std::memcpy(g_in.data() + base, ipu_dma_ptr(madr), 16);
            *g_ch4_reg[1] = madr + 16;
            *g_ch4_reg[2] -= 1;
            if (*g_ch4_reg[2] == 0 && (!g_to.chain || g_to.tag_end)) complete_ch4();
            return true;
        }
        if (!g_to.chain || g_to.tag_end) {
            complete_ch4();
            return false;
        }
        /* Read the next source-chain tag. */
        if (++g_to.tags > 65536) {
            rt_fatal("ipu", nullptr, "toIPU source chain exceeded 65536 tags; runaway TADR=0x%08x", *g_ch4_reg[3]);
        }
        uint32_t tadr = *g_ch4_reg[3];
        uint64_t lo;
        std::memcpy(&lo, ipu_dma_ptr(tadr), 8);
        uint32_t qwc = (uint32_t)(lo & 0xFFFF);
        uint32_t id = (uint32_t)((lo >> 28) & 7);
        bool irq = (lo >> 31) & 1;
        uint32_t taddr = (uint32_t)((lo >> 32) & 0x7FFFFFF0u);
        bool tspr = (lo >> 63) & 1;
        bool tie = (*g_ch4_reg[0] >> 7) & 1;
        /* CHCR.TAG mirrors tag bits 16-31. */
        *g_ch4_reg[0] = (*g_ch4_reg[0] & 0xFFFFu) | ((uint32_t)(lo >> 16) & 0xFFFF0000u);
        if ((*g_ch4_reg[0] >> 6) & 1) {
            static uint64_t n = 0;
            if (is_pow2(++n)) rt_log("ipu", "toIPU chain tag with TTE set; tag words 2-3 dropped [#%" PRIu64 "]", n);
        }
        switch (id) {
            case 0: /* REFE */
                *g_ch4_reg[1] = taddr | (tspr ? 0x80000000u : 0);
                *g_ch4_reg[2] = qwc;
                *g_ch4_reg[3] = tadr + 16;
                g_to.tag_end = true;
                break;
            case 1: /* CNT */
                *g_ch4_reg[1] = tadr + 16;
                *g_ch4_reg[2] = qwc;
                *g_ch4_reg[3] = tadr + 16 + qwc * 16;
                break;
            case 2: /* NEXT */
                *g_ch4_reg[1] = tadr + 16;
                *g_ch4_reg[2] = qwc;
                *g_ch4_reg[3] = taddr | (tspr ? 0x80000000u : 0);
                break;
            case 3: /* REF */
            case 4: /* REFS */
                *g_ch4_reg[1] = taddr | (tspr ? 0x80000000u : 0);
                *g_ch4_reg[2] = qwc;
                *g_ch4_reg[3] = tadr + 16;
                break;
            case 7: /* END */
                *g_ch4_reg[1] = tadr + 16;
                *g_ch4_reg[2] = qwc;
                *g_ch4_reg[3] = tadr + 16 + qwc * 16;
                g_to.tag_end = true;
                break;
            default: /* CALL/RET have no ASR on ch4 */
                rt_fatal("ipu", nullptr, "toIPU chain tag id %u (CALL/RET) not supported on ch4", id);
        }
        if (irq && tie) g_to.tag_end = true;
        if (*g_ch4_reg[2] == 0 && g_to.tag_end) {
            complete_ch4();
            return false;
        }
    }
}

size_t avail_bits() { return g_in.size() * 8 - g_in_pos; }

bool ensure_bits(size_t n) {
    while (avail_bits() < n) {
        if (!pull_qword()) {
            g_underflow = true;
            return false;
        }
    }
    return true;
}

/* MSB-first peek of up to 32 bits at the current position. Caller must have
 * ensured availability. */
uint32_t peek_bits(unsigned n) {
    uint64_t acc = 0;
    size_t byte = g_in_pos >> 3;
    unsigned sh = (unsigned)(g_in_pos & 7);
    for (int i = 0; i < 6; ++i) {
        acc = (acc << 8) | (byte + i < g_in.size() ? g_in[byte + i] : 0);
    }
    /* acc holds 48 bits starting at byte; drop the sub-byte offset. */
    acc = (acc >> (48 - sh - n)) & ((n == 32) ? 0xFFFFFFFFull : ((1ull << n) - 1));
    return (uint32_t)acc;
}

void advance_bits(size_t n) {
    g_in_pos += n;
    g_bp_bits += n;
}

uint32_t get_bits(unsigned n) {
    uint32_t v = peek_bits(n);
    advance_bits(n);
    return v;
}

int32_t get_signed(unsigned n) {
    int32_t v = (int32_t)peek_bits(n);
    advance_bits(n);
    v <<= (32 - n);
    v >>= (32 - n);
    return v;
}

/* Drop consumed whole bytes; only when no command is mid-flight. */
void compact_in() {
    size_t drop = g_in_pos >> 3;
    if (drop == 0) return;
    g_in.erase(g_in.begin(), g_in.begin() + drop);
    g_in_pos &= 7;
}

/* Mimic the hardware 8-qword input FIFO. */
void prefill() {
    while (g_to.active && avail_bits() < 8 * 128) {
        if (!pull_qword()) break;
    }
}

/* ---- output queue and fromIPU DMA (ch3) ---------------------------------- */

std::vector<uint8_t> g_out;
size_t g_out_head = 0;

size_t out_avail() { return g_out.size() - g_out_head; }

void out_push(const void* data, size_t len) {
    size_t base = g_out.size();
    g_out.resize(base + len);
    std::memcpy(g_out.data() + base, data, len);
}

void complete_ch3() {
    g_from.active = false;
    g_from.drain_all = false;
    *g_ch3_reg[0] &= ~0x100u;
    rt_dmac_raise(3);
    if (rt_trace() || is_pow2(g_from.kicks)) {
        rt_log("ipu", "fromIPU ch3 transfer complete (madr=0x%08x) [kick #%" PRIu64 "]",
            *g_ch3_reg[1], g_from.kicks);
    }
}

void drain_ch3() {
    if (!g_from.active) return;
    if (!(*g_ch3_reg[0] & 0x100u)) {
        g_from.active = false; /* stopped manually by the driver */
        g_from.drain_all = false;
        return;
    }
    while ((*g_ch3_reg[2] > 0 || g_from.drain_all) && out_avail() >= 16) {
        std::memcpy(ipu_dma_ptr(*g_ch3_reg[1]), g_out.data() + g_out_head, 16);
        g_out_head += 16;
        *g_ch3_reg[1] += 16;
        if (*g_ch3_reg[2] > 0) *g_ch3_reg[2] -= 1;
    }
    if (g_out_head == g_out.size()) {
        g_out.clear();
        g_out_head = 0;
    }
    if (g_from.drain_all ? out_avail() == 0 : *g_ch3_reg[2] == 0) complete_ch3();
}

/* ---- register / decoder state -------------------------------------------- */

uint32_t g_ctrl_bits = 0; /* stored IDP/AS/IVF/QST/MP1/PCT (bits 16-26) */
bool g_ecd = false, g_scd = false;
uint32_t g_cbp_reg = 0;
uint32_t g_cmd_data = 0;  /* IPU_CMD read result (VDEC/FDEC) */
uint32_t g_top = 0;
bool g_busy = false;      /* command pending on input */
uint32_t g_cur_cmd = 0;
uint32_t g_last_cmd_code = 0xF; /* command nibble of the last accepted command */

uint8_t g_iq[64];  /* intra quantizer matrix, transmission (zigzag) order */
uint8_t g_niq[64]; /* non-intra */
uint8_t g_vq[32];  /* VQCLUT, stored only (PACK is not used) */
uint16_t g_th[2] = {0, 0};

int g_dcpred[3];

/* Retry snapshot: taken at command acceptance, restored on input underflow. */
struct Snapshot {
    size_t in_pos;
    uint64_t bp_bits;
    size_t out_size;
    int dcpred[3];
    uint32_t cbp;
};
Snapshot g_snap;

uint64_t g_cmd_census[16];

int ctrl_idp() { return (g_ctrl_bits >> 16) & 3; }
int ctrl_as() { return (g_ctrl_bits >> 20) & 1; }
int ctrl_ivf() { return (g_ctrl_bits >> 21) & 1; }
int ctrl_qst() { return (g_ctrl_bits >> 22) & 1; }
int ctrl_mp1() { return (g_ctrl_bits >> 23) & 1; }
int ctrl_pct() { return (g_ctrl_bits >> 24) & 7; }

/* ---- VLC tables ----------------------------------------------------------
 * Code assignments are ISO/IEC 13818-2 facts: B-1 (macroblock address
 * increment), B-2/3/4 (macroblock type), B-9 (coded block pattern), B-10
 * (motion code), B-11 (dmvector), B-12/13 (DC size), B-14/15 (DCT
 * coefficients). Entries are {code, length, value...}; a code is matched
 * when the next `length` bits equal `code`. Sign bits, escape payloads and
 * the first-coefficient rule are handled by the decoders below. */

struct Vlc {
    uint16_t code;
    uint8_t len;
    int16_t val;
};

/* B-1. Values 1..33; 0x22 = stuffing (MPEG1 only), 0x23 = escape; the
 * escape/stuffing VDEC result encoding (0xb0022/0xb0023) matches what the
 * libmpeg driver in this binary tests for. */
constexpr Vlc kMbai[] = {
    {0x0001, 1, 1}, {0x0002, 3, 3}, {0x0003, 3, 2}, {0x0002, 4, 5},
    {0x0003, 4, 4}, {0x0002, 5, 7}, {0x0003, 5, 6}, {0x0006, 7, 9},
    {0x0007, 7, 8}, {0x0006, 8, 15}, {0x0007, 8, 14}, {0x0008, 8, 13},
    {0x0009, 8, 12}, {0x000A, 8, 11}, {0x000B, 8, 10}, {0x0012, 10, 21},
    {0x0013, 10, 20}, {0x0014, 10, 19}, {0x0015, 10, 18}, {0x0016, 10, 17},
    {0x0017, 10, 16}, {0x0008, 11, 0x23}, {0x000F, 11, 0x22}, {0x0018, 11, 33},
    {0x0019, 11, 32}, {0x001A, 11, 31}, {0x001B, 11, 30}, {0x001C, 11, 29},
    {0x001D, 11, 28}, {0x001E, 11, 27}, {0x001F, 11, 26}, {0x0020, 11, 25},
    {0x0021, 11, 24}, {0x0022, 11, 23}, {0x0023, 11, 22},
};

/* Macroblock type flag values in the VDEC result (hardware encoding,
 * confirmed by what this binary's driver masks): */
constexpr int MB_INTRA = 1, MB_PATTERN = 2, MB_BACKWARD = 4, MB_FORWARD = 8, MB_QUANT = 16;
constexpr int MB_MC_FRAME = 128; /* motion_type frame, bits 6-7 = 2 */

constexpr Vlc kMbtI[] = {
    {0x0001, 1, MB_INTRA}, {0x0001, 2, MB_INTRA | MB_QUANT},
};
constexpr Vlc kMbtP[] = {
    {0x0001, 1, MB_FORWARD | MB_PATTERN}, {0x0001, 2, MB_PATTERN},
    {0x0001, 3, MB_FORWARD}, {0x0001, 5, MB_QUANT | MB_PATTERN},
    {0x0002, 5, MB_QUANT | MB_FORWARD | MB_PATTERN}, {0x0003, 5, MB_INTRA},
    {0x0001, 6, MB_QUANT | MB_INTRA},
};
constexpr Vlc kMbtB[] = {
    {0x0002, 2, MB_FORWARD | MB_BACKWARD},
    {0x0003, 2, MB_FORWARD | MB_BACKWARD | MB_PATTERN},
    {0x0002, 3, MB_BACKWARD}, {0x0003, 3, MB_BACKWARD | MB_PATTERN},
    {0x0002, 4, MB_FORWARD}, {0x0003, 4, MB_FORWARD | MB_PATTERN},
    {0x0002, 5, MB_QUANT | MB_FORWARD | MB_BACKWARD | MB_PATTERN},
    {0x0003, 5, MB_INTRA},
    {0x0001, 6, MB_QUANT | MB_INTRA},
    {0x0002, 6, MB_QUANT | MB_BACKWARD | MB_PATTERN},
    {0x0003, 6, MB_QUANT | MB_FORWARD | MB_PATTERN},
};

/* B-10 motion_code magnitude (the sign bit follows the code). */
constexpr Vlc kMotion[] = {
    {0x0001, 1, 0}, {0x0001, 2, 1}, {0x0001, 3, 2}, {0x0001, 4, 3},
    {0x0003, 6, 4}, {0x0003, 7, 7}, {0x0004, 7, 6}, {0x0005, 7, 5},
    {0x0009, 9, 10}, {0x000A, 9, 9}, {0x000B, 9, 8}, {0x000C, 10, 16},
    {0x000D, 10, 15}, {0x000E, 10, 14}, {0x000F, 10, 13}, {0x0010, 10, 12},
    {0x0011, 10, 11},
};

constexpr Vlc kDmv[] = {
    {0x0000, 1, 0}, {0x0002, 2, 1}, {0x0003, 2, -1},
};

constexpr Vlc kCbp[] = {
    {0x0007, 3, 0x3C}, {0x000A, 4, 0x20}, {0x000B, 4, 0x10}, {0x000C, 4, 0x08},
    {0x000D, 4, 0x04}, {0x0008, 5, 0x3E}, {0x0009, 5, 0x02}, {0x000A, 5, 0x3D},
    {0x000B, 5, 0x01}, {0x000C, 5, 0x38}, {0x000D, 5, 0x34}, {0x000E, 5, 0x2C},
    {0x000F, 5, 0x1C}, {0x0010, 5, 0x28}, {0x0011, 5, 0x14}, {0x0012, 5, 0x30},
    {0x0013, 5, 0x0C}, {0x000C, 6, 0x3F}, {0x000D, 6, 0x03}, {0x000E, 6, 0x24},
    {0x000F, 6, 0x18}, {0x0010, 7, 0x22}, {0x0011, 7, 0x12}, {0x0012, 7, 0x0A},
    {0x0013, 7, 0x06}, {0x0014, 7, 0x21}, {0x0015, 7, 0x11}, {0x0016, 7, 0x09},
    {0x0017, 7, 0x05}, {0x0004, 8, 0x3A}, {0x0005, 8, 0x36}, {0x0006, 8, 0x2E},
    {0x0007, 8, 0x1E}, {0x0008, 8, 0x39}, {0x0009, 8, 0x35}, {0x000A, 8, 0x2D},
    {0x000B, 8, 0x1D}, {0x000C, 8, 0x26}, {0x000D, 8, 0x1A}, {0x000E, 8, 0x25},
    {0x000F, 8, 0x19}, {0x0010, 8, 0x2B}, {0x0011, 8, 0x17}, {0x0012, 8, 0x33},
    {0x0013, 8, 0x0F}, {0x0014, 8, 0x2A}, {0x0015, 8, 0x16}, {0x0016, 8, 0x32},
    {0x0017, 8, 0x0E}, {0x0018, 8, 0x29}, {0x0019, 8, 0x15}, {0x001A, 8, 0x31},
    {0x001B, 8, 0x0D}, {0x001C, 8, 0x23}, {0x001D, 8, 0x13}, {0x001E, 8, 0x0B},
    {0x001F, 8, 0x07}, {0x0001, 9, 0x00}, {0x0002, 9, 0x27}, {0x0003, 9, 0x1B},
    {0x0004, 9, 0x3B}, {0x0005, 9, 0x37}, {0x0006, 9, 0x2F}, {0x0007, 9, 0x1F},
};

constexpr Vlc kDcLuma[] = {
    {0x0000, 2, 1}, {0x0001, 2, 2}, {0x0004, 3, 0}, {0x0005, 3, 3},
    {0x0006, 3, 4}, {0x000E, 4, 5}, {0x001E, 5, 6}, {0x003E, 6, 7},
    {0x007E, 7, 8}, {0x00FE, 8, 9}, {0x01FE, 9, 10}, {0x01FF, 9, 11},
};
constexpr Vlc kDcChroma[] = {
    {0x0000, 2, 0}, {0x0001, 2, 1}, {0x0002, 2, 2}, {0x0006, 3, 3},
    {0x000E, 4, 4}, {0x001E, 5, 5}, {0x003E, 6, 6}, {0x007E, 7, 7},
    {0x00FE, 8, 8}, {0x01FE, 9, 9}, {0x03FE, 10, 10}, {0x03FF, 10, 11},
};

/* B-14/B-15 DCT coefficient codes. run 64 = end of block, 65 = escape. */
struct DctVlc {
    uint16_t code;
    uint8_t len;
    uint8_t run;
    uint8_t level;
};

/* B-14; the first-coefficient rule ('1s' = (0,1) as the first coefficient
 * of a non-intra block) is handled in dct_vlc_decode. */
constexpr DctVlc kDct14[] = {
    {0x0002, 2, 64, 0}, {0x0003, 2, 0, 1}, {0x0003, 3, 1, 1}, {0x0004, 4, 0, 2},
    {0x0005, 4, 2, 1}, {0x0005, 5, 0, 3}, {0x0006, 5, 4, 1}, {0x0007, 5, 3, 1},
    {0x0001, 6, 65, 0}, {0x0004, 6, 7, 1}, {0x0005, 6, 6, 1}, {0x0006, 6, 1, 2},
    {0x0007, 6, 5, 1}, {0x0004, 7, 2, 2}, {0x0005, 7, 9, 1}, {0x0006, 7, 0, 4},
    {0x0007, 7, 8, 1}, {0x0020, 8, 13, 1}, {0x0021, 8, 0, 6}, {0x0022, 8, 12, 1},
    {0x0023, 8, 11, 1}, {0x0024, 8, 3, 2}, {0x0025, 8, 1, 3}, {0x0026, 8, 0, 5},
    {0x0027, 8, 10, 1}, {0x0008, 10, 16, 1}, {0x0009, 10, 5, 2}, {0x000A, 10, 0, 7},
    {0x000B, 10, 2, 3}, {0x000C, 10, 1, 4}, {0x000D, 10, 15, 1}, {0x000E, 10, 14, 1},
    {0x000F, 10, 4, 2}, {0x0010, 12, 0, 11}, {0x0011, 12, 8, 2}, {0x0012, 12, 4, 3},
    {0x0013, 12, 0, 10}, {0x0014, 12, 2, 4}, {0x0015, 12, 7, 2}, {0x0016, 12, 21, 1},
    {0x0017, 12, 20, 1}, {0x0018, 12, 0, 9}, {0x0019, 12, 19, 1}, {0x001A, 12, 18, 1},
    {0x001B, 12, 1, 5}, {0x001C, 12, 3, 3}, {0x001D, 12, 0, 8}, {0x001E, 12, 6, 2},
    {0x001F, 12, 17, 1}, {0x0010, 13, 10, 2}, {0x0011, 13, 9, 2}, {0x0012, 13, 5, 3},
    {0x0013, 13, 3, 4}, {0x0014, 13, 2, 5}, {0x0015, 13, 1, 7}, {0x0016, 13, 1, 6},
    {0x0017, 13, 0, 15}, {0x0018, 13, 0, 14}, {0x0019, 13, 0, 13}, {0x001A, 13, 0, 12},
    {0x001B, 13, 26, 1}, {0x001C, 13, 25, 1}, {0x001D, 13, 24, 1}, {0x001E, 13, 23, 1},
    {0x001F, 13, 22, 1}, {0x0010, 14, 0, 31}, {0x0011, 14, 0, 30}, {0x0012, 14, 0, 29},
    {0x0013, 14, 0, 28}, {0x0014, 14, 0, 27}, {0x0015, 14, 0, 26}, {0x0016, 14, 0, 25},
    {0x0017, 14, 0, 24}, {0x0018, 14, 0, 23}, {0x0019, 14, 0, 22}, {0x001A, 14, 0, 21},
    {0x001B, 14, 0, 20}, {0x001C, 14, 0, 19}, {0x001D, 14, 0, 18}, {0x001E, 14, 0, 17},
    {0x001F, 14, 0, 16}, {0x0010, 15, 0, 40}, {0x0011, 15, 0, 39}, {0x0012, 15, 0, 38},
    {0x0013, 15, 0, 37}, {0x0014, 15, 0, 36}, {0x0015, 15, 0, 35}, {0x0016, 15, 0, 34},
    {0x0017, 15, 0, 33}, {0x0018, 15, 0, 32}, {0x0019, 15, 1, 14}, {0x001A, 15, 1, 13},
    {0x001B, 15, 1, 12}, {0x001C, 15, 1, 11}, {0x001D, 15, 1, 10}, {0x001E, 15, 1, 9},
    {0x001F, 15, 1, 8}, {0x0010, 16, 1, 18}, {0x0011, 16, 1, 17}, {0x0012, 16, 1, 16},
    {0x0013, 16, 1, 15}, {0x0014, 16, 6, 3}, {0x0015, 16, 16, 2}, {0x0016, 16, 15, 2},
    {0x0017, 16, 14, 2}, {0x0018, 16, 13, 2}, {0x0019, 16, 12, 2}, {0x001A, 16, 11, 2},
    {0x001B, 16, 31, 1}, {0x001C, 16, 30, 1}, {0x001D, 16, 29, 1}, {0x001E, 16, 28, 1},
    {0x001F, 16, 27, 1},
};

constexpr DctVlc kDct15[] = {
    {0x0002, 2, 0, 1}, {0x0002, 3, 1, 1}, {0x0006, 3, 0, 2}, {0x0006, 4, 64, 0},
    {0x0007, 4, 0, 3}, {0x0005, 5, 2, 1}, {0x0006, 5, 1, 2}, {0x0007, 5, 3, 1},
    {0x001C, 5, 0, 4}, {0x001D, 5, 0, 5}, {0x0001, 6, 65, 0}, {0x0004, 6, 0, 7},
    {0x0005, 6, 0, 6}, {0x0006, 6, 4, 1}, {0x0007, 6, 5, 1}, {0x0004, 7, 7, 1},
    {0x0005, 7, 8, 1}, {0x0006, 7, 6, 1}, {0x0007, 7, 2, 2}, {0x0078, 7, 9, 1},
    {0x0079, 7, 1, 3}, {0x007A, 7, 10, 1}, {0x007B, 7, 0, 8}, {0x007C, 7, 0, 9},
    {0x0020, 8, 1, 5}, {0x0021, 8, 11, 1}, {0x0022, 8, 0, 11}, {0x0023, 8, 0, 10},
    {0x0024, 8, 13, 1}, {0x0025, 8, 12, 1}, {0x0026, 8, 3, 2}, {0x0027, 8, 1, 4},
    {0x00FA, 8, 0, 12}, {0x00FB, 8, 0, 13}, {0x00FC, 8, 2, 3}, {0x00FD, 8, 4, 2},
    {0x00FE, 8, 0, 14}, {0x00FF, 8, 0, 15}, {0x0004, 9, 5, 2}, {0x0005, 9, 14, 1},
    {0x0007, 9, 15, 1}, {0x000C, 10, 2, 4}, {0x000D, 10, 16, 1}, {0x0010, 12, 0, 11},
    {0x0011, 12, 8, 2}, {0x0012, 12, 4, 3}, {0x0013, 12, 0, 10}, {0x0014, 12, 2, 4},
    {0x0015, 12, 7, 2}, {0x0016, 12, 21, 1}, {0x0017, 12, 20, 1}, {0x0018, 12, 0, 9},
    {0x0019, 12, 19, 1}, {0x001A, 12, 18, 1}, {0x001B, 12, 1, 5}, {0x001C, 12, 3, 3},
    {0x001D, 12, 0, 8}, {0x001E, 12, 6, 2}, {0x001F, 12, 17, 1}, {0x0010, 13, 10, 2},
    {0x0011, 13, 9, 2}, {0x0012, 13, 5, 3}, {0x0013, 13, 3, 4}, {0x0014, 13, 2, 5},
    {0x0015, 13, 1, 7}, {0x0016, 13, 1, 6}, {0x0017, 13, 0, 15}, {0x0018, 13, 0, 14},
    {0x0019, 13, 0, 13}, {0x001A, 13, 0, 12}, {0x001B, 13, 26, 1}, {0x001C, 13, 25, 1},
    {0x001D, 13, 24, 1}, {0x001E, 13, 23, 1}, {0x001F, 13, 22, 1}, {0x0010, 14, 0, 31},
    {0x0011, 14, 0, 30}, {0x0012, 14, 0, 29}, {0x0013, 14, 0, 28}, {0x0014, 14, 0, 27},
    {0x0015, 14, 0, 26}, {0x0016, 14, 0, 25}, {0x0017, 14, 0, 24}, {0x0018, 14, 0, 23},
    {0x0019, 14, 0, 22}, {0x001A, 14, 0, 21}, {0x001B, 14, 0, 20}, {0x001C, 14, 0, 19},
    {0x001D, 14, 0, 18}, {0x001E, 14, 0, 17}, {0x001F, 14, 0, 16}, {0x0010, 15, 0, 40},
    {0x0011, 15, 0, 39}, {0x0012, 15, 0, 38}, {0x0013, 15, 0, 37}, {0x0014, 15, 0, 36},
    {0x0015, 15, 0, 35}, {0x0016, 15, 0, 34}, {0x0017, 15, 0, 33}, {0x0018, 15, 0, 32},
    {0x0019, 15, 1, 14}, {0x001A, 15, 1, 13}, {0x001B, 15, 1, 12}, {0x001C, 15, 1, 11},
    {0x001D, 15, 1, 10}, {0x001E, 15, 1, 9}, {0x001F, 15, 1, 8}, {0x0010, 16, 1, 18},
    {0x0011, 16, 1, 17}, {0x0012, 16, 1, 16}, {0x0013, 16, 1, 15}, {0x0014, 16, 6, 3},
    {0x0015, 16, 16, 2}, {0x0016, 16, 15, 2}, {0x0017, 16, 14, 2}, {0x0018, 16, 13, 2},
    {0x0019, 16, 12, 2}, {0x001A, 16, 11, 2}, {0x001B, 16, 31, 1}, {0x001C, 16, 30, 1},
    {0x001D, 16, 29, 1}, {0x001E, 16, 28, 1}, {0x001F, 16, 27, 1},
};

/* Non-linear quantiser scale, ISO 13818-2 7.4.2.2 (q_scale_type = 1). */
constexpr int kNonLinearQ[32] = {
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 10, 12, 14, 16, 18, 20, 22,
    24, 28, 32, 36, 40, 44, 48, 52,
    56, 64, 72, 80, 88, 96, 104, 112,
};

/* Inverse scan patterns, ISO 13818-2 figures 7-2 / 7-3. */
constexpr uint8_t kScanZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
constexpr uint8_t kScanAlt[64] = {
    0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
    41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
    53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63,
};

/* Generic VLC match: peeks up to 16 bits and scans the table. Returns the
 * index, or -1 for no match (caller raises ECD). Consumes the code. */
template <size_t N>
int vlc_decode(const Vlc (&tab)[N], int* len_out) {
    if (!ensure_bits(16)) {
        /* Near end of stream fewer than 16 bits can still hold a valid
         * short code; retry with what is available. */
        if (avail_bits() == 0) return -2;
        g_underflow = false;
    }
    size_t have = avail_bits();
    uint32_t window = 0;
    unsigned wbits = (unsigned)(have < 16 ? have : 16);
    window = peek_bits(wbits);
    for (size_t i = 0; i < N; ++i) {
        if (tab[i].len > wbits) {
            if (tab[i].len <= 16 && have < tab[i].len) {
                /* Cannot rule the longer codes out yet. */
                g_underflow = true;
                return -2;
            }
            continue;
        }
        if ((window >> (wbits - tab[i].len)) == tab[i].code) {
            advance_bits(tab[i].len);
            if (len_out) *len_out = tab[i].len;
            return (int)i;
        }
    }
    return -1;
}

/* DCT coefficient decode (B-14/B-15). Returns 0 ok (run/level/sign filled),
 * 1 end-of-block, -1 invalid code, -2 underflow. */
int dct_vlc_decode(bool intra_vlc, bool first, int* run, int* level_mag, bool* negative) {
    if (!ensure_bits(17)) {
        if (avail_bits() < 17) {
            /* A short code plus sign may still fit; simplest correct
             * behavior is to require the full window and retry when more
             * data arrives. Real streams end with start codes, never mid
             * block. */
            return -2;
        }
    }
    g_underflow = false;
    uint32_t window = peek_bits(17);
    if (!intra_vlc && first && (window >> 16) == 1) {
        /* First coefficient of a non-intra block: '1s'. */
        advance_bits(1);
        *run = 0;
        *level_mag = 1;
        *negative = get_bits(1) != 0;
        return 0;
    }
    const DctVlc* tab = intra_vlc ? kDct15 : kDct14;
    size_t n = intra_vlc ? sizeof(kDct15) / sizeof(kDct15[0]) : sizeof(kDct14) / sizeof(kDct14[0]);
    for (size_t i = 0; i < n; ++i) {
        if ((window >> (17 - tab[i].len)) != tab[i].code) continue;
        advance_bits(tab[i].len);
        if (tab[i].run == 64) return 1; /* EOB */
        if (tab[i].run == 65) {         /* escape: 6-bit run + 12-bit signed level */
            if (!ensure_bits(18)) return -2;
            *run = (int)get_bits(6);
            int32_t lv = get_signed(12);
            if (lv == 0 || lv == -2048) return -1; /* forbidden codes */
            *negative = lv < 0;
            *level_mag = lv < 0 ? -lv : lv;
            return 0;
        }
        *run = tab[i].run;
        *level_mag = tab[i].level;
        if (!ensure_bits(1)) return -2;
        *negative = get_bits(1) != 0;
        return 0;
    }
    return -1;
}

/* ---- IDCT ----------------------------------------------------------------
 * Straight double-precision implementation of the 8x8 inverse DCT as
 * defined by ISO/IEC 13818-2 Annex A. Own code, no external source. */

double g_idct_mat[8][8]; /* [x][u] = C(u)/2 * cos((2x+1)u pi / 16) */
bool g_idct_init = false;

void idct_init() {
    if (g_idct_init) return;
    for (int x = 0; x < 8; ++x) {
        for (int u = 0; u < 8; ++u) {
            double cu = (u == 0) ? (1.0 / std::sqrt(2.0)) : 1.0;
            g_idct_mat[x][u] = 0.5 * cu * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
        }
    }
    g_idct_init = true;
}

void idct8x8(const int16_t in[64], double out[64]) {
    double tmp[64];
    for (int y = 0; y < 8; ++y) { /* rows: over u */
        for (int x = 0; x < 8; ++x) {
            double s = 0;
            for (int u = 0; u < 8; ++u) s += g_idct_mat[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    }
    for (int x = 0; x < 8; ++x) { /* columns: over v */
        for (int y = 0; y < 8; ++y) {
            double s = 0;
            for (int v = 0; v < 8; ++v) s += g_idct_mat[y][v] * tmp[v * 8 + x];
            out[y * 8 + x] = s;
        }
    }
}

/* ---- BDEC ---------------------------------------------------------------- */

int16_t g_dct[64]; /* coefficient staging block */

void saturate_coeff(int32_t* v) {
    if (*v > 2047) *v = 2047;
    if (*v < -2048) *v = -2048;
}

/* Decodes the AC (and for non-intra, all) coefficients of one block into
 * g_dct. Returns false on underflow (retry). Invalid codes end the block
 * (reference-hardware behavior; the trailing start-code scan raises ECD on
 * genuinely broken streams). *any_ac reports whether any coefficient other
 * than g_dct[0] was written (DC-only blocks skip the full IDCT). */
bool decode_block_coeffs(bool intra, int qscale, bool* any_ac) {
    const uint8_t* scan = ctrl_as() ? kScanAlt : kScanZigzag;
    const uint8_t* w = intra ? g_iq : g_niq;
    int i = intra ? 0 : -1; /* coefficient index in scan order */
    for (;;) {
        int run, mag;
        bool neg;
        int r = dct_vlc_decode(intra && ctrl_ivf(), !intra && i < 0, &run, &mag, &neg);
        if (r == -2) { g_underflow = true; return false; }
        if (r == 1) return true; /* EOB */
        if (r == -1) {
            static uint64_t n = 0;
            if (is_pow2(++n)) rt_log("ipu", "invalid DCT coefficient code; block ended [#%" PRIu64 "]", n);
            return true;
        }
        i = (i < 0 ? 0 : i + 1) + run;
        if (i >= 64) return true;
        int j = scan[i];
        int32_t val;
        if (intra) {
            val = ((int32_t)mag * qscale * w[i]) >> 4;
        } else {
            val = ((2 * (int32_t)mag + 1) * qscale * w[i]) >> 5;
        }
        if (neg) val = -val;
        saturate_coeff(&val);
        g_dct[j] = (int16_t)val;
        if (j != 0) *any_ac = true;
    }
}

/* Output macroblock staging: Y 16x16, Cb 8x8, Cr 8x8, 16-bit, 768 bytes.
 * This is the BDEC output layout (RAW16). */
struct MacroBlock16 {
    int16_t y[16][16];
    int16_t cb[8][8];
    int16_t cr[8][8];
};
MacroBlock16 g_mb16;

/* CSC input layout (RAW8), 384 bytes. */
struct MacroBlock8 {
    uint8_t y[16][16];
    uint8_t cb[8][8];
    uint8_t cr[8][8];
};

void store_luma_block(int b, bool field_dct, const double* px, bool intra) {
    for (int i = 0; i < 8; ++i) {
        int row = field_dct ? ((b >> 1) + 2 * i) : ((b >> 1) * 8 + i);
        int col = (b & 1) * 8;
        for (int x = 0; x < 8; ++x) {
            long v = std::lround(px[i * 8 + x]);
            if (intra) {
                if (v < 0) v = 0;
                if (v > 255) v = 255;
            } else {
                if (v < -256) v = -256;
                if (v > 255) v = 255;
            }
            g_mb16.y[row][col + x] = (int16_t)v;
        }
    }
}

void store_chroma_block(int16_t (*plane)[8], const double* px, bool intra) {
    for (int i = 0; i < 8; ++i) {
        for (int x = 0; x < 8; ++x) {
            long v = std::lround(px[i * 8 + x]);
            if (intra) {
                if (v < 0) v = 0;
                if (v > 255) v = 255;
            } else {
                if (v < -256) v = -256;
                if (v > 255) v = 255;
            }
            plane[i][x] = (int16_t)v;
        }
    }
}

/* Trailing start-code scan after a BDEC: if the next 8 bits are zero, the
 * stream aligns to the byte boundary and zero bytes are skipped; a
 * following 000001 sets SCD, other nonzero data sets ECD. (Behavior per
 * hardware as documented by the reference emulator; libmpeg relies on SCD
 * to find the end of each slice.) */
bool bdec_tail_scan() {
    if (!ensure_bits(8)) return false;
    if (peek_bits(8) == 0) {
        /* Align to the byte boundary. */
        size_t misalign = g_in_pos & 7;
        if (misalign) advance_bits(8 - misalign);
        for (;;) {
            if (!ensure_bits(24)) return false;
            uint32_t sc = peek_bits(24);
            if (sc != 0) {
                if (sc == 1) g_scd = true;
                else g_ecd = true;
                break;
            }
            advance_bits(8);
        }
    }
    if (!ensure_bits(32)) return false;
    g_top = peek_bits(32);
    return true;
}

bool exec_bdec(uint32_t val) {
    uint32_t fb = val & 0x3F;
    uint32_t qsc = (val >> 16) & 0x1F;
    bool dt = (val >> 25) & 1;
    bool dcr = (val >> 26) & 1;
    bool mbi = (val >> 27) & 1;

    if (ctrl_mp1()) {
        rt_fatal("ipu", nullptr, "BDEC with IPU_CTRL.MP1 (MPEG1) set; not modeled (this stream is MPEG2)");
    }
    if (!ensure_bits(fb)) return false;
    advance_bits(fb);

    if (dcr) {
        g_dcpred[0] = g_dcpred[1] = g_dcpred[2] = 128 << ctrl_idp();
    }
    int qscale = ctrl_qst() ? kNonLinearQ[qsc] : (int)(qsc << 1);
    std::memset(&g_mb16, 0, sizeof(g_mb16));

    uint32_t cbp = 0x3F;
    if (!mbi) {
        int len;
        int idx = vlc_decode(kCbp, &len);
        if (idx == -2) { g_underflow = true; return false; }
        if (idx < 0) {
            static uint64_t n = 0;
            if (is_pow2(++n)) rt_log("ipu", "invalid coded_block_pattern code; cbp=0 [#%" PRIu64 "]", n);
            cbp = 0;
        } else {
            cbp = (uint32_t)kCbp[idx].val;
        }
    }

    double px[64];
    for (int b = 0; b < 6; ++b) {
        if (!(cbp & (0x20u >> b))) continue;
        std::memset(g_dct, 0, sizeof(g_dct));
        if (mbi) {
            /* DC coefficient: size VLC + differential. */
            int len;
            int idx;
            if (b < 4) idx = vlc_decode(kDcLuma, &len);
            else idx = vlc_decode(kDcChroma, &len);
            if (idx == -2) { g_underflow = true; return false; }
            int size = idx < 0 ? 0 : (b < 4 ? kDcLuma[idx].val : kDcChroma[idx].val);
            int diff = 0;
            if (size) {
                if (!ensure_bits((unsigned)size)) return false;
                diff = (int)get_bits((unsigned)size);
                if (!(diff & (1 << (size - 1)))) diff -= (1 << size) - 1;
            }
            int cc = b < 4 ? 0 : (b == 4 ? 1 : 2);
            g_dcpred[cc] += diff;
            g_dct[0] = (int16_t)(g_dcpred[cc] << (3 - ctrl_idp()));
        }
        bool any_ac = false;
        if (!decode_block_coeffs(mbi, qscale, &any_ac)) return false;
        if (any_ac) {
            idct8x8(g_dct, px);
        } else {
            /* DC-only block: the IDCT is a constant dc/8. */
            double c = g_dct[0] / 8.0;
            for (int i = 0; i < 64; ++i) px[i] = c;
        }
        if (b < 4) store_luma_block(b, dt, px, mbi);
        else if (b == 4) store_chroma_block(g_mb16.cb, px, mbi);
        else store_chroma_block(g_mb16.cr, px, mbi);
    }

    g_cbp_reg = cbp;
    out_push(&g_mb16, sizeof(g_mb16)); /* 48 qwords */
    if (!bdec_tail_scan()) return false;
    return true;
}

/* ---- VDEC ---------------------------------------------------------------- */

bool exec_vdec(uint32_t val) {
    uint32_t fb = val & 0x3F;
    uint32_t tbl = (val >> 26) & 3;
    if (!ensure_bits(fb)) return false;
    advance_bits(fb);

    uint32_t result = 0;
    switch (tbl) {
        case 0: { /* macroblock address increment */
            int len;
            int idx = vlc_decode(kMbai, &len);
            if (idx == -2) return false;
            if (idx >= 0) {
                if (kMbai[idx].val == 0x22 && !ctrl_mp1()) {
                    result = 0; /* stuffing is MPEG1-only */
                } else {
                    result = (uint32_t)kMbai[idx].val | ((uint32_t)len << 16);
                }
            }
            break;
        }
        case 1: { /* macroblock type, table selected by CTRL.PCT */
            int pct = ctrl_pct() ? ctrl_pct() : 1;
            int len;
            if (pct == 1) {
                int idx = vlc_decode(kMbtI, &len);
                if (idx == -2) return false;
                if (idx >= 0) result = (uint32_t)kMbtI[idx].val;
            } else if (pct == 2) {
                int idx = vlc_decode(kMbtP, &len);
                if (idx == -2) return false;
                if (idx >= 0) {
                    uint32_t modes = (uint32_t)kMbtP[idx].val;
                    /* frame picture, frame_pred_frame_dct: motion type is
                     * implicitly frame MC. */
                    if (modes & MB_FORWARD) modes |= MB_MC_FRAME;
                    result = modes;
                }
            } else if (pct == 3) {
                int idx = vlc_decode(kMbtB, &len);
                if (idx == -2) return false;
                if (idx >= 0) {
                    uint32_t modes = (uint32_t)kMbtB[idx].val | MB_MC_FRAME;
                    result = modes | ((uint32_t)len << 16);
                }
            } else {
                static uint64_t n = 0;
                if (is_pow2(++n)) rt_log("ipu", "VDEC MBT with PCT=%d (D picture?); returning error [#%" PRIu64 "]", pct, n);
            }
            break;
        }
        case 2: { /* motion code */
            if (!ensure_bits(1)) return false;
            if (peek_bits(1) == 1) {
                advance_bits(1);
                result = 0x00010000; /* value 0, length 1 */
            } else {
                int len;
                int idx = vlc_decode(kMotion, &len);
                if (idx == -2) return false;
                if (idx > 0) { /* idx 0 is the '1' code, handled above */
                    int32_t mag = kMotion[idx].val;
                    if (!ensure_bits(1)) return false;
                    int32_t sign = get_bits(1) ? -1 : 0;
                    int32_t v = (mag ^ sign) - sign;
                    result = (uint32_t)v | ((uint32_t)len << 16);
                }
            }
            break;
        }
        default: { /* dmvector */
            int len;
            int idx = vlc_decode(kDmv, &len);
            if (idx == -2) return false;
            if (idx >= 0) {
                result = (uint32_t)(int32_t)kDmv[idx].val | ((uint32_t)len << 16);
            }
            break;
        }
    }

    g_cmd_data = result;
    g_ecd = (result == 0);
    if (!ensure_bits(32)) return false;
    g_top = peek_bits(32);
    return true;
}

/* ---- CSC ----------------------------------------------------------------- */

/* Documented integer BT.601 conversion of the IPU (EE User's Manual):
 * Y coefficient 0x95, R/Cr 0xCC, G/Cr -0x68, G/Cb -0x32, B/Cb 0x102,
 * biases 16 (Y) and 128 (C), >>6 then rounded >>1. Alpha is 0x80 before
 * thresholding. */
void csc_one_mb(const MacroBlock8* mb, uint8_t* rgb_out /* 1024 bytes */) {
    for (int yy = 0; yy < 16; ++yy) {
        for (int xx = 0; xx < 16; ++xx) {
            int ylev = mb->y[yy][xx] - 16;
            if (ylev < 0) ylev = 0;
            int lum = (0x95 * ylev) >> 6;
            int cr = mb->cr[yy >> 1][xx >> 1] - 128;
            int cb = mb->cb[yy >> 1][xx >> 1] - 128;
            int r = (lum + ((0xCC * cr) >> 6) + 1) >> 1;
            int g = (lum + ((-0x68 * cr) >> 6) + ((-0x32 * cb) >> 6) + 1) >> 1;
            int b = (lum + ((0x102 * cb) >> 6) + 1) >> 1;
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            int a = 0x80;
            if (g_th[0] > 0 && r < g_th[0] && g < g_th[0] && b < g_th[0]) {
                r = g = b = a = 0;
            } else if (g_th[1] > 0 && r < g_th[1] && g < g_th[1] && b < g_th[1]) {
                a = 0x40;
            }
            uint8_t* p = rgb_out + (yy * 16 + xx) * 4;
            p[0] = (uint8_t)r;
            p[1] = (uint8_t)g;
            p[2] = (uint8_t)b;
            p[3] = (uint8_t)a;
        }
    }
}

bool exec_csc(uint32_t val) {
    uint32_t mbc = val & 0x7FF;
    bool ofm = (val >> 27) & 1;
    if (ofm) {
        rt_fatal("ipu", nullptr, "CSC with OFM=1 (RGB16 output); not in this binary's census");
    }
    MacroBlock8 mb;
    uint8_t rgb[1024];
    for (uint32_t m = 0; m < mbc; ++m) {
        if (!ensure_bits(sizeof(mb) * 8)) return false;
        /* Input follows the bitstream position (normally byte aligned). */
        for (size_t i = 0; i < sizeof(mb); ++i) {
            ((uint8_t*)&mb)[i] = (uint8_t)get_bits(8);
        }
        csc_one_mb(&mb, rgb);
        out_push(rgb, sizeof(rgb));
    }
    return true;
}

/* ---- SETIQ / SETVQ / SETTH / BCLR ---------------------------------------- */

bool exec_setiq(uint32_t val) {
    uint32_t fb = val & 0x3F;
    bool niq = (val >> 27) & 1;
    if (!ensure_bits(fb)) return false;
    advance_bits(fb);
    if (!ensure_bits(64 * 8)) return false;
    uint8_t* dst = niq ? g_niq : g_iq;
    for (int i = 0; i < 64; ++i) dst[i] = (uint8_t)get_bits(8);
    if (rt_trace()) rt_log("ipu", "SETIQ %s matrix loaded", niq ? "non-intra" : "intra");
    return true;
}

bool exec_setvq() {
    if (!ensure_bits(32 * 8)) return false;
    for (int i = 0; i < 32; ++i) g_vq[i] = (uint8_t)get_bits(8);
    return true;
}

void exec_bclr(uint32_t val) {
    g_in.clear();
    g_in_pos = val & 0x7F;
    g_bp_bits = val & 0x7F;
    g_underflow = false;
    if (rt_trace()) rt_log("ipu", "BCLR bp=%u", val & 0x7F);
}

void soft_reset() {
    g_in.clear();
    g_in_pos = 0;
    g_bp_bits = 0;
    g_out.clear();
    g_out_head = 0;
    g_cbp_reg = 0;
    g_th[0] = g_th[1] = 0;
    g_ecd = g_scd = false;
    g_busy = false;
    g_top = 0;
    g_cmd_data = 0;
    g_underflow = false;
    /* The driver stops ch3/ch4 around a reset; drop any pending transfer
     * state so a stale completion cannot fire later. */
    g_to.active = false;
    g_from.active = false;
    g_from.drain_all = false;
    /* Keeps the IQ/VQ matrices and the picture-parameter CTRL bits, and
     * (a hardware quirk mirrored from the reference behavior) reinitializes
     * the DC predictors only at 8-bit precision. */
    if (ctrl_idp() == 0) {
        g_dcpred[0] = g_dcpred[1] = g_dcpred[2] = 128;
    }
    rt_intc_raise(RT_INTC_IPU);
    rt_log("ipu", "soft reset (IPU_CTRL.RST)");
}

/* ---- command dispatch / retry -------------------------------------------- */

void take_snapshot() {
    g_snap.in_pos = g_in_pos;
    g_snap.bp_bits = g_bp_bits;
    g_snap.out_size = g_out.size();
    std::memcpy(g_snap.dcpred, g_dcpred, sizeof(g_dcpred));
    g_snap.cbp = g_cbp_reg;
}

void restore_snapshot() {
    g_in_pos = g_snap.in_pos;
    g_bp_bits = g_snap.bp_bits;
    g_out.resize(g_snap.out_size);
    std::memcpy(g_dcpred, g_snap.dcpred, sizeof(g_dcpred));
    g_cbp_reg = g_snap.cbp;
}

void run_pending() {
    if (!g_busy) return;
    idct_init();
    restore_snapshot();
    g_underflow = false;
    g_ecd = false;
    g_scd = false;

    uint32_t val = g_cur_cmd;
    bool done = false;
    switch (val >> 28) {
        case 2: done = exec_bdec(val); break;
        case 3: done = exec_vdec(val); break;
        case 4: { /* FDEC: skip FB, then the next 32 bits (no advance) */
            uint32_t fb = val & 0x3F;
            if (!ensure_bits(fb)) break;
            advance_bits(fb);
            if (!ensure_bits(32)) break;
            g_cmd_data = peek_bits(32);
            g_top = g_cmd_data;
            done = true;
            break;
        }
        case 5: done = exec_setiq(val); break;
        case 6: done = exec_setvq(); break;
        case 7: done = exec_csc(val); break;
        default:
            rt_fatal("ipu", nullptr, "unreachable pending command 0x%08x", val);
    }

    if (!done) {
        /* Input underflow: rewind and wait for the next toIPU kick. */
        restore_snapshot();
        static uint64_t n = 0;
        if (rt_trace() || is_pow2(++n)) {
            rt_log("ipu", "command 0x%08x stalls on input (have %zu bits); pending [#%" PRIu64 "]",
                val, avail_bits(), n);
        }
        return;
    }

    g_busy = false;
    compact_in();
    prefill();
    rt_intc_raise(RT_INTC_IPU);
    drain_ch3();
}

void cmd_write(uint32_t val) {
    uint32_t code = val >> 28;
    ++g_cmd_census[code];
    if (is_pow2(g_cmd_census[code]) || rt_trace()) {
        static const char* names[16] = {"BCLR", "IDEC", "BDEC", "VDEC", "FDEC", "SETIQ", "SETVQ", "CSC",
                                        "PACK", "SETTH", "?", "?", "?", "?", "?", "?"};
        rt_log("ipu", "%s 0x%08x [#%" PRIu64 "]", names[code], val, g_cmd_census[code]);
    }
    if (g_busy) {
        rt_log("ipu", "command 0x%08x written while 0x%08x is still pending; previous dropped", val, g_cur_cmd);
        g_busy = false;
    }
    g_ecd = false;
    g_scd = false;
    g_last_cmd_code = code;

    switch (code) {
        case 0: /* BCLR: immediate */
            exec_bclr(val);
            rt_intc_raise(RT_INTC_IPU);
            return;
        case 9: /* SETTH: immediate */
            g_th[0] = (uint16_t)(val & 0x1FF);
            g_th[1] = (uint16_t)((val >> 16) & 0x1FF);
            rt_intc_raise(RT_INTC_IPU);
            return;
        case 1:
            rt_fatal("ipu", nullptr, "IDEC issued (0x%08x); not in this binary's measured census", val);
        case 8:
            rt_fatal("ipu", nullptr, "PACK issued (0x%08x); not in this binary's measured census", val);
        case 2: case 3: case 4: case 5: case 6: case 7:
            g_cur_cmd = val;
            g_busy = true;
            take_snapshot();
            run_pending();
            return;
        default:
            rt_fatal("ipu", nullptr, "unknown IPU command 0x%08x", val);
    }
}

uint64_t g_busy_polls = 0;

} // namespace

/* ---- DMA bridge (called from hw/dmac.cpp) -------------------------------- */

void rt_ipu_dma_kick(int ch) {
    RT_PROF_ZONE(RT_PROF_IPU);
    bind_regs();
    idct_init();
    if (ch == 4) {
        ++g_to.kicks;
        if (g_to.active) {
            rt_log("ipu", "toIPU ch4 kicked while a transfer is active; continuing with new registers");
        }
        uint32_t chcr = *g_ch4_reg[0];
        uint32_t mode = (chcr >> 2) & 3;
        if (mode == 2) {
            rt_fatal("ipu", nullptr, "toIPU ch4 kicked in interleave mode");
        }
        g_to.active = true;
        g_to.chain = (mode == 1);
        /* Chain kicks with QWC!=0 resume a partially transferred tag. */
        g_to.tag_end = false;
        if (g_to.chain && *g_ch4_reg[2] != 0) {
            uint32_t tag_id = (chcr >> 28) & 7;
            bool tag_irq = (chcr >> 31) & 1;
            bool tie = (chcr >> 7) & 1;
            if (tag_id == 0 || tag_id == 7 || (tag_irq && tie)) g_to.tag_end = true;
        }
        if (!g_to.chain && *g_ch4_reg[2] == 0) {
            /* Normal-mode kick with nothing to move completes at once. */
            complete_ch4();
            return;
        }
        if (rt_trace() || is_pow2(g_to.kicks)) {
            rt_log("ipu", "toIPU ch4 kick: %s madr=0x%08x qwc=%u tadr=0x%08x [#%" PRIu64 "]",
                g_to.chain ? "chain" : "normal", *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3], g_to.kicks);
        }
        prefill();
        run_pending();
        drain_ch3();
    } else {
        ++g_from.kicks;
        if (g_from.active) {
            rt_log("ipu", "fromIPU ch3 kicked while a transfer is pending; continuing with new registers");
        }
        g_from.active = true;
        g_from.drain_all = (*g_ch3_reg[2] == 0);
        if (g_from.drain_all) {
            static uint64_t n = 0;
            if (is_pow2(++n)) {
                rt_log("ipu", "fromIPU ch3 kicked with QWC=0; draining the whole output queue (%zu qw) [#%" PRIu64 "]",
                    out_avail() / 16, n);
            }
        }
        if (rt_trace() || is_pow2(g_from.kicks)) {
            rt_log("ipu", "fromIPU ch3 kick: madr=0x%08x qwc=%u (out has %zu qw) [#%" PRIu64 "]",
                *g_ch3_reg[1], *g_ch3_reg[2], out_avail() / 16, g_from.kicks);
        }
        drain_ch3();
    }
}

/* ---- MMIO ---------------------------------------------------------------- */

bool rt_ipu_mmio_read(uint32_t addr, uint64_t* out) {
    RT_PROF_ZONE(RT_PROF_IPU);
    switch (addr) {
        case 0x10002000: { /* IPU_CMD */
            uint32_t data = g_cmd_data;
            if (g_last_cmd_code != 3 && g_last_cmd_code != 4) {
                /* Live 32-bit bitstream peek (hardware behavior after
                 * non-VDEC/FDEC commands). */
                bool saved = g_underflow;
                if (ensure_bits(32)) data = peek_bits(32);
                g_underflow = saved;
            }
            *out = (uint64_t)data | (g_busy ? (1ull << 63) : 0);
            if (g_busy && is_pow2(++g_busy_polls)) {
                rt_log("ipu", "IPU_CMD busy poll while 0x%08x waits for input [#%" PRIu64 "]",
                    g_cur_cmd, g_busy_polls);
            }
            return true;
        }
        case 0x10002010: { /* IPU_CTRL */
            uint32_t ifc = (uint32_t)(avail_bits() / 128);
            if (ifc > 8) ifc = 8;
            uint32_t ofc = (uint32_t)(out_avail() / 16);
            if (ofc > 8) ofc = 8;
            uint32_t v = ifc | (ofc << 4) | ((g_cbp_reg & 0x3F) << 8) |
                (g_ecd ? 1u << 14 : 0) | (g_scd ? 1u << 15 : 0) |
                (g_ctrl_bits & 0x07F30000u) | (g_busy ? 1u << 31 : 0);
            *out = v;
            return true;
        }
        case 0x10002020: { /* IPU_BP */
            uint32_t ifc = (uint32_t)(avail_bits() / 128);
            if (ifc > 8) ifc = 8;
            *out = (uint32_t)(g_bp_bits & 0x7F) | (ifc << 8); /* FP reported 0 */
            return true;
        }
        case 0x10002030: { /* IPU_TOP */
            *out = (uint64_t)g_top | (g_busy ? (1ull << 63) : 0);
            return true;
        }
        case 0x10007000: case 0x10007010: {
            static uint64_t n = 0;
            if (is_pow2(++n)) rt_log("ipu", "IPU FIFO window read at 0x%08x; returns 0 [#%" PRIu64 "]", addr, n);
            *out = 0;
            return true;
        }
        default:
            return false;
    }
}

bool rt_ipu_mmio_write(uint32_t addr, uint64_t v) {
    RT_PROF_ZONE(RT_PROF_IPU);
    switch (addr) {
        case 0x10002000: /* IPU_CMD */
            bind_regs();
            idct_init();
            cmd_write((uint32_t)v);
            return true;
        case 0x10002010: { /* IPU_CTRL: bits 16-26 stored (18-19 reserved), RST acts */
            uint32_t nv = (uint32_t)v;
            g_ctrl_bits = nv & 0x07F30000u;
            if (((g_ctrl_bits >> 16) & 3) == 3) {
                rt_log("ipu", "IPU_CTRL.IDP=3 is reserved; treating as 9-bit precision");
                g_ctrl_bits = (g_ctrl_bits & ~0x30000u) | 0x10000u;
            }
            if (nv & 0x40000000u) soft_reset();
            return true;
        }
        case 0x10002020: case 0x10002030:
            rt_log("ipu", "write to read-only IPU register 0x%08x = 0x%llx ignored", addr, (unsigned long long)v);
            return true;
        case 0x10007010:
            /* Only 128-bit stores are in this binary's census; those are
             * routed through rt_ipu_fifo_feed by mmio.cpp before this
             * dispatcher runs. A narrower store reaching here is new. */
            rt_fatal("ipu", nullptr, "sub-qword write to the toIPU FIFO window (0x%08x = 0x%llx); "
                "not in this binary's census (feeds are DMA ch4 and 128-bit stores)",
                addr, (unsigned long long)v);
        case 0x10007000:
            rt_fatal("ipu", nullptr, "write to the fromIPU FIFO window 0x10007000");
        default:
            return false;
    }
}

void rt_ipu_fifo_feed(const uint8_t* qw16) {
    RT_PROF_ZONE(RT_PROF_IPU);
    static uint64_t n = 0;
    size_t base = g_in.size();
    g_in.resize(base + 16);
    std::memcpy(g_in.data() + base, qw16, 16);
    if (rt_trace() || is_pow2(++n)) {
        rt_log("ipu", "toIPU FIFO qword store (input now %zu bits) [#%" PRIu64 "]", avail_bits(), n);
    }
    run_pending();
}

/* ---- selftest hooks (used by hw/ipu_selftest.cpp only) ------------------- */

void rt_ipu_test_feed(const uint8_t* data, size_t len) {
    size_t base = g_in.size();
    g_in.resize(base + len);
    std::memcpy(g_in.data() + base, data, len);
}

size_t rt_ipu_test_out_avail() { return out_avail(); }

size_t rt_ipu_test_read_out(uint8_t* dst, size_t maxlen) {
    size_t n = out_avail();
    if (n > maxlen) n = maxlen;
    std::memcpy(dst, g_out.data() + g_out_head, n);
    g_out_head += n;
    if (g_out_head == g_out.size()) {
        g_out.clear();
        g_out_head = 0;
    }
    return n;
}
