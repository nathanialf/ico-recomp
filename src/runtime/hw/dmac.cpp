/* hw/dmac.cpp: EE DMAC channel register file and synchronous transfer
 * execution.
 *
 * Model (per the plan and CLAUDE.md): a write to CHCR with STR set executes
 * the whole transfer synchronously inside the MMIO trap. Completion clears
 * STR/QWC, updates MADR/TADR, and raises the channel's D_STAT bit via
 * rt_dmac_raise; the registered guest DMAC handler then runs from the
 * deferred delivery path (rt_intc_deliver at the next instruction
 * boundary), never inside the CHCR write itself.
 *
 * Implemented channels:
 *   ch0 VIF0     loud stub (the game clears its registers but never kicks)
 *   ch1 VIF1     normal + source chain (CNT/NEXT/REF/REFS/CALL/RET/END/
 *                REFE), 2-deep ASR stack, TTE tag words 2-3 to VIF1
 *   ch2 GIF      normal + source chain, payload to GIF PATH3
 *   ch3/ch4 IPU  forwarded to hw/ipu.cpp (rt_ipu_dma_kick), which owns
 *                execution and completion for both channels: ch4 (toIPU)
 *                is a pull source for the IPU bitstream (normal + source
 *                chain), ch3 (fromIPU) drains the IPU output queue and can
 *                stay pending (STR set) until a command produces data
 *   ch5/6/7 SIF  register file only; kicks are loud stubs (SIF DMA is
 *                HLE'd at the SifSetDma syscall layer, sif/sif.cpp)
 *   ch8 fromSPR  normal + interleave (SADR wraps in the 16 KB scratchpad)
 *   ch9 toSPR    normal + interleave + dest chain treated as loud stub
 *
 * Global registers: D_CTRL (DMAE honored, RELE/MFD/STS/STD loud stubs),
 * D_PCR/D_SQWC stored (SQWC drives interleave), D_RBSR/D_RBOR/D_STADR are
 * loud stubs per the plan: the boot trap log shows the game writing only
 * zeros to them; a nonzero arm is a fatal model error. D_STAT lives in
 * ee/intc.cpp next to the delivery machinery.
 *
 * Register layouts are public PS2 hardware documentation (ps2tek, EE DMAC).
 */
#include "hw.h"

#include "../ee/kernel.h"

#include <cinttypes>
#include <cstring>
#include <vector>

namespace {

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

struct Channel {
    uint32_t chcr = 0;
    uint32_t madr = 0;
    uint32_t qwc = 0;
    uint32_t tadr = 0;
    uint32_t asr0 = 0, asr1 = 0;
    uint32_t sadr = 0;
    uint64_t kicks = 0;
};

struct ChannelDesc {
    uint32_t base;
    const char* name;
    bool has_tadr;
    bool has_asr;
    bool has_sadr;
};

constexpr ChannelDesc kDesc[10] = {
    {0x10008000, "VIF0",    true,  true,  false},
    {0x10009000, "VIF1",    true,  true,  false},
    {0x1000A000, "GIF",     true,  true,  false},
    {0x1000B000, "fromIPU", false, false, false},
    {0x1000B400, "toIPU",   true,  false, false},
    {0x1000C000, "SIF0",    false, false, false},
    {0x1000C400, "SIF1",    true,  false, false},
    {0x1000C800, "SIF2",    false, false, false},
    {0x1000D000, "fromSPR", false, false, true},
    {0x1000D400, "toSPR",   true,  false, true},
};

Channel g_ch[10];
uint32_t g_dctrl = 0;
uint32_t g_dpcr = 0;
uint32_t g_dsqwc = 0;
uint32_t g_rbsr = 0, g_rbor = 0, g_stadr = 0;
uint32_t g_enable = 0; /* D_ENABLEW shadow, read back via D_ENABLER */

int channel_at(uint32_t addr, uint32_t* reg_off) {
    for (int i = 0; i < 10; ++i) {
        if (addr >= kDesc[i].base && addr < kDesc[i].base + 0x100) {
            *reg_off = addr - kDesc[i].base;
            return i;
        }
    }
    return -1;
}

/* Resolve a guest DMA address to host memory. spr forces the scratchpad;
 * bit 31 of the address also selects it (MADR/TADR SPR bit). The
 * scratchpad wraps at its architectural 16 KB. */
uint8_t* dma_ptr(uint32_t addr, bool spr) {
    if (spr || (addr & 0x80000000u)) {
        uint32_t off = addr & 0x3FFF;
        uint8_t* page = g_pages[0x70000000u >> 16];
        return page + off;
    }
    uint8_t* p = g_pages[addr >> 16];
    if (!p) {
        rt_fatal("dmac", nullptr, "DMA touches unmapped guest address 0x%08x", addr);
    }
    return p + (addr & 0xFFFF);
}

/* Copy qwc qwords from guest memory into a contiguous scratch buffer
 * (crossing 64 KB page boundaries safely). */
void gather(std::vector<uint8_t>& out, uint32_t addr, uint32_t qwc, bool spr) {
    size_t base = out.size();
    out.resize(base + (size_t)qwc * 16);
    for (uint32_t q = 0; q < qwc; ++q) {
        std::memcpy(out.data() + base + (size_t)q * 16, dma_ptr(addr + q * 16, spr), 16);
    }
}

/* ---- peripheral sinks ---------------------------------------------------- */

void sink_payload(int ch, uint32_t addr, uint32_t qwc, bool spr, std::vector<uint8_t>& gif_accum) {
    if (qwc == 0) return;
    if (ch == 1) {
        static std::vector<uint8_t> buf;
        buf.clear();
        gather(buf, addr, qwc, spr);
        rt_vif1_feed(reinterpret_cast<const uint32_t*>(buf.data()), qwc * 4);
    } else if (ch == 2) {
        gather(gif_accum, addr, qwc, spr);
    }
}

/* ---- transfer execution -------------------------------------------------- */

const char* tag_id_name(uint32_t id) {
    static const char* names[8] = {"REFE", "CNT", "NEXT", "REF", "REFS", "CALL", "RET", "END"};
    return names[id & 7];
}

void run_source_chain(int ch, Channel& c) {
    const bool tte = (c.chcr >> 6) & 1;
    const bool tie = (c.chcr >> 7) & 1;
    std::vector<uint8_t> gif_accum;
    uint32_t tags = 0, total_qw = 0;
    /* ASP field of CHCR (bits 4-5) is the live stack depth. */
    uint32_t asp = (c.chcr >> 4) & 3;

    /* Runaway guard. A full in-game display list is a long balanced
     * CALL/CNT/RET + REF walk (the START menu transition walks >4096 tags
     * legitimately, main list monotonically increasing, shared subroutine
     * packets re-CALLed many times), so the cap must be far above any real
     * frame. 1M tags at 16 bytes each is 16 MB, half of EE RAM: past that
     * the chain cannot be real data. On trip, dump the last 32 tags walked
     * so a genuine loop is visible in the log. */
    constexpr uint32_t kTagCap = 1u << 20;
    struct TagRec { uint32_t tadr, id, qwc, taddr; };
    static TagRec last_tags[32];

    for (;;) {
        if (++tags > kTagCap) {
            for (uint32_t i = 0; i < 32; ++i) {
                const TagRec& t = last_tags[(tags - 1 - 32 + i) % 32];
                rt_log("dmac", "runaway last[%u]: @0x%08x %s qwc=%u addr=0x%08x",
                    i, t.tadr, tag_id_name(t.id), t.qwc, t.taddr);
            }
            rt_fatal("dmac", nullptr, "ch%d (%s) source chain exceeded %u tags; runaway TADR=0x%08x STADR=0x%08x D_CTRL=0x%08x",
                ch, kDesc[ch].name, kTagCap, c.tadr, g_stadr, g_dctrl);
        }
        bool tadr_spr = (c.tadr & 0x80000000u) != 0;
        uint8_t tagbuf[16];
        std::memcpy(tagbuf, dma_ptr(c.tadr, tadr_spr), 16);
        uint64_t lo;
        std::memcpy(&lo, tagbuf, 8);
        uint32_t qwc = (uint32_t)(lo & 0xFFFF);
        uint32_t id = (uint32_t)((lo >> 28) & 7);
        bool irq = (lo >> 31) & 1;
        uint32_t taddr = (uint32_t)((lo >> 32) & 0x7FFFFFF0u);
        bool tspr = (lo >> 63) & 1;
        c.chcr = (c.chcr & 0xFFFFu) | ((uint32_t)(lo >> 16) & 0xFFFF0000u); /* CHCR.TAG mirrors tag bits 16-31 */
        last_tags[(tags - 1) % 32] = {c.tadr, id, qwc, taddr};

        if (rt_trace()) {
            rt_log("dmac", "ch%d tag #%u @0x%08x: %s qwc=%u addr=0x%08x%s irq=%d",
                ch, tags, c.tadr, tag_id_name(id), qwc, taddr, tspr ? " SPR" : "", irq ? 1 : 0);
        }

        if (tte && ch == 1) {
            /* Tag words 2-3 go to VIF1 as VIFcodes. */
            uint32_t w[2];
            std::memcpy(w, tagbuf + 8, 8);
            rt_vif1_feed(w, 2);
        } else if (tte && is_pow2(c.kicks)) {
            rt_log("dmac", "ch%d (%s): TTE set on a non-VIF1 chain; tag words dropped", ch, kDesc[ch].name);
        }

        bool end = false;
        uint32_t next_tadr = c.tadr;
        switch (id) {
            case 0: /* REFE */
                c.madr = taddr | (tspr ? 0x80000000u : 0);
                sink_payload(ch, c.madr, qwc, tspr, gif_accum);
                next_tadr = c.tadr + 16;
                end = true;
                break;
            case 1: /* CNT */
                c.madr = c.tadr + 16;
                sink_payload(ch, c.madr, qwc, tadr_spr, gif_accum);
                next_tadr = c.madr + qwc * 16;
                break;
            case 2: /* NEXT */
                c.madr = c.tadr + 16;
                sink_payload(ch, c.madr, qwc, tadr_spr, gif_accum);
                next_tadr = taddr | (tspr ? 0x80000000u : 0);
                break;
            case 3: /* REF */
            case 4: /* REFS (stall control not modeled; loud below) */
                if (id == 4 && (g_dctrl & 0xC0u)) {
                    static uint64_t n = 0;
                    if (is_pow2(++n)) rt_log("dmac", "REFS with D_CTRL.STS armed (0x%x); stall not modeled [#%" PRIu64 "]", g_dctrl, n);
                }
                c.madr = taddr | (tspr ? 0x80000000u : 0);
                sink_payload(ch, c.madr, qwc, tspr, gif_accum);
                next_tadr = c.tadr + 16;
                break;
            case 5: /* CALL */
                c.madr = c.tadr + 16;
                sink_payload(ch, c.madr, qwc, tadr_spr, gif_accum);
                if (asp == 0) { c.asr0 = c.madr + qwc * 16; asp = 1; }
                else if (asp == 1) { c.asr1 = c.madr + qwc * 16; asp = 2; }
                else {
                    rt_fatal("dmac", nullptr, "ch%d CALL with ASR stack full (asp=2)", ch);
                }
                next_tadr = taddr | (tspr ? 0x80000000u : 0);
                break;
            case 6: /* RET */
                c.madr = c.tadr + 16;
                sink_payload(ch, c.madr, qwc, tadr_spr, gif_accum);
                if (asp == 2) { next_tadr = c.asr1; asp = 1; }
                else if (asp == 1) { next_tadr = c.asr0; asp = 0; }
                else { next_tadr = c.tadr + 16; end = true; }
                break;
            default: /* 7: END */
                c.madr = c.tadr + 16;
                sink_payload(ch, c.madr, qwc, tadr_spr, gif_accum);
                next_tadr = c.madr + qwc * 16;
                end = true;
                break;
        }
        total_qw += qwc;
        c.tadr = next_tadr;
        if (irq && tie) end = true;
        if (end) break;
    }

    c.chcr = (c.chcr & ~0x30u) | (asp << 4);
    if (ch == 2 && !gif_accum.empty()) {
        rt_gif_submit(2, gif_accum.data(), (uint32_t)(gif_accum.size() / 16));
    }
    if (rt_trace() || is_pow2(c.kicks)) {
        rt_log("dmac", "ch%d (%s) chain done: %u tags, %u qw [kick #%" PRIu64 "]",
            ch, kDesc[ch].name, tags, total_qw, c.kicks);
    }
}

void run_normal(int ch, Channel& c) {
    uint32_t qwc = c.qwc & 0xFFFF;
    bool spr = (c.madr & 0x80000000u) != 0;
    switch (ch) {
        case 2: {
            std::vector<uint8_t> gif_accum;
            sink_payload(ch, c.madr, qwc, spr, gif_accum);
            if (!gif_accum.empty()) rt_gif_submit(2, gif_accum.data(), (uint32_t)(gif_accum.size() / 16));
            break;
        }
        case 8: { /* fromSPR: SADR (scratchpad) -> MADR (RAM), optional interleave */
            uint32_t mode = (c.chcr >> 2) & 3;
            uint32_t tqwc = (g_dsqwc >> 16) & 0xFF;
            uint32_t sqwc = g_dsqwc & 0xFF;
            uint32_t left = qwc, m = c.madr, s = c.sadr;
            while (left) {
                uint32_t burst = (mode == 2 && tqwc) ? (left < tqwc ? left : tqwc) : left;
                for (uint32_t q = 0; q < burst; ++q) {
                    std::memcpy(dma_ptr(m, false), dma_ptr(s & 0x3FFF, true), 16);
                    m += 16;
                    s = (s + 16) & 0x3FFF;
                }
                left -= burst;
                if (mode == 2 && left) m += sqwc * 16;
            }
            c.madr = m;
            c.sadr = s;
            break;
        }
        case 9: { /* toSPR: MADR (RAM) -> SADR (scratchpad), optional interleave */
            uint32_t mode = (c.chcr >> 2) & 3;
            uint32_t tqwc = (g_dsqwc >> 16) & 0xFF;
            uint32_t sqwc = g_dsqwc & 0xFF;
            uint32_t left = qwc, m = c.madr, s = c.sadr;
            while (left) {
                uint32_t burst = (mode == 2 && tqwc) ? (left < tqwc ? left : tqwc) : left;
                for (uint32_t q = 0; q < burst; ++q) {
                    std::memcpy(dma_ptr(s & 0x3FFF, true), dma_ptr(m, false), 16);
                    m += 16;
                    s = (s + 16) & 0x3FFF;
                }
                left -= burst;
                if (mode == 2 && left) m += sqwc * 16;
            }
            c.madr = m;
            c.sadr = s;
            break;
        }
        default:
            break; /* callers ensure this is not reached */
    }
    if (ch == 2) c.madr += qwc * 16;
    if (rt_trace() || is_pow2(c.kicks)) {
        rt_log("dmac", "ch%d (%s) normal done: %u qw [kick #%" PRIu64 "]", ch, kDesc[ch].name, qwc, c.kicks);
    }
}

void kick(int ch, Channel& c) {
    ++c.kicks;
    uint32_t dir = c.chcr & 1;
    uint32_t mode = (c.chcr >> 2) & 3;

    if (!(g_dctrl & 1)) {
        rt_log("dmac", "ch%d (%s) kicked with D_CTRL.DMAE=0; executing anyway (model has no pending queue)",
            ch, kDesc[ch].name);
    }
    if (g_enable & 0x10000u) {
        rt_log("dmac", "ch%d (%s) kicked while D_ENABLE suspend is set; executing anyway", ch, kDesc[ch].name);
    }

    switch (ch) {
        case 1: /* VIF1 */
            if (dir == 0) {
                rt_log("dmac", "ch1 VIF1 kicked in FROM direction (GS readback); not modeled, dropped");
                break;
            }
            if (mode == 1) run_source_chain(ch, c);
            else if (mode == 0) {
                static std::vector<uint8_t> buf;
                buf.clear();
                gather(buf, c.madr, c.qwc & 0xFFFF, (c.madr & 0x80000000u) != 0);
                rt_vif1_feed(reinterpret_cast<const uint32_t*>(buf.data()), (c.qwc & 0xFFFF) * 4);
                c.madr += (c.qwc & 0xFFFF) * 16;
                if (rt_trace() || is_pow2(c.kicks)) {
                    rt_log("dmac", "ch1 (VIF1) normal done: %u qw [kick #%" PRIu64 "]", c.qwc & 0xFFFF, c.kicks);
                }
            } else {
                rt_fatal("dmac", nullptr, "ch1 VIF1 kicked in interleave mode");
            }
            break;
        case 2: /* GIF */
            if (mode == 1) run_source_chain(ch, c);
            else if (mode == 0) run_normal(ch, c);
            else rt_fatal("dmac", nullptr, "ch2 GIF kicked in interleave mode");
            break;
        case 8: case 9: /* SPR */
            if (mode == 1) {
                rt_fatal("dmac", nullptr, "ch%d (%s) kicked in chain mode; only normal/interleave modeled",
                    ch, kDesc[ch].name);
            }
            run_normal(ch, c);
            break;
        case 3: case 4: /* ---- IPU section: hw/ipu.cpp owns these ---- */
            /* The IPU model executes and completes ch3/ch4 itself (STR
             * clear + rt_dmac_raise), possibly deferred until a command
             * produces or consumes data, so the standard completion tail
             * below must not run. */
            rt_ipu_dma_kick(ch);
            return;
        case 0: /* VIF0: loud stub */
            rt_log("dmac", "ch0 (VIF0) kicked (madr=0x%08x qwc=%u): STUB, transfer dropped [kick #%" PRIu64 "]",
                c.madr, c.qwc, c.kicks);
            break;
        default: /* SIF channels: HLE'd at the SifSetDma layer */
            rt_log("dmac", "ch%d (%s) kicked via CHCR (madr=0x%08x qwc=%u): STUB (SIF DMA is HLE'd), dropped [kick #%" PRIu64 "]",
                ch, kDesc[ch].name, c.madr, c.qwc, c.kicks);
            break;
    }

    /* Completion: STR off, QWC drained, D_STAT channel bit up. The guest
     * handler runs from the deferred delivery path, not here. */
    c.chcr &= ~0x100u;
    c.qwc = 0;
    rt_dmac_raise(ch);
}

} // namespace

/* ---- IPU section: register-file access for hw/ipu.cpp ------------------- */

uint32_t* rt_dmac_ipu_reg(int ch, int which) {
    Channel& c = g_ch[ch];
    switch (which) {
        case 0: return &c.chcr;
        case 1: return &c.madr;
        case 2: return &c.qwc;
        default: return &c.tadr;
    }
}

bool rt_dmac_mmio_read(uint32_t addr, uint32_t* out) {
    uint32_t off;
    int ch = channel_at(addr, &off);
    if (ch >= 0) {
        Channel& c = g_ch[ch];
        switch (off) {
            case 0x00: *out = c.chcr; return true;
            case 0x10: *out = c.madr; return true;
            case 0x20: *out = c.qwc; return true;
            case 0x30: *out = c.tadr; return true;
            case 0x40: *out = c.asr0; return true;
            case 0x50: *out = c.asr1; return true;
            case 0x80: *out = c.sadr; return true;
            default: return false;
        }
    }
    switch (addr) {
        case 0x1000E000: *out = g_dctrl; return true;
        case 0x1000E020: *out = g_dpcr; return true;
        case 0x1000E030: *out = g_dsqwc; return true;
        case 0x1000E040: *out = g_rbsr; return true;
        case 0x1000E050: *out = g_rbor; return true;
        case 0x1000E060: *out = g_stadr; return true;
        case 0x1000E120: *out = g_enable; return true; /* D_ENABLER */
        default: return false;
    }
}

bool rt_dmac_mmio_write(uint32_t addr, uint32_t v) {
    uint32_t off;
    int ch = channel_at(addr, &off);
    if (ch >= 0) {
        Channel& c = g_ch[ch];
        switch (off) {
            case 0x00:
                c.chcr = v;
                if (v & 0x100u) kick(ch, c);
                return true;
            case 0x10: c.madr = v; return true;
            case 0x20: c.qwc = v & 0xFFFF; return true;
            case 0x30: c.tadr = v; return true;
            case 0x40: c.asr0 = v; return true;
            case 0x50: c.asr1 = v; return true;
            case 0x80: c.sadr = v; return true;
            default: return false;
        }
    }
    switch (addr) {
        case 0x1000E000:
            g_dctrl = v;
            if (v & ~1u) {
                rt_log("dmac", "D_CTRL = 0x%08x arms unmodeled features (RELE/MFD/STS/STD); loud stub", v);
            }
            return true;
        case 0x1000E020: g_dpcr = v; return true;
        case 0x1000E030: g_dsqwc = v; return true;
        case 0x1000E040:
            g_rbsr = v;
            if (v) rt_fatal("dmac", nullptr, "D_RBSR = 0x%08x arms MFIFO; not modeled (loud stub per plan)", v);
            return true;
        case 0x1000E050:
            g_rbor = v;
            if (v) rt_fatal("dmac", nullptr, "D_RBOR = 0x%08x arms MFIFO; not modeled (loud stub per plan)", v);
            return true;
        case 0x1000E060:
            g_stadr = v;
            if (v) rt_log("dmac", "D_STADR = 0x%08x written; stall control is not modeled (loud stub)", v);
            return true;
        case 0x1000E100: g_enable = v; return true; /* D_ENABLEW */
        default: return false;
    }
}
