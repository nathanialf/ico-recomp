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
 *   ch8 fromSPR  normal + interleave (SADR wraps in the 16 KB scratchpad);
 *                chain is a loud stub (fromSPR is destination chain only)
 *   ch9 toSPR    normal + interleave + source chain (payload lands in the
 *                scratchpad at SADR, which wraps in the same 16 KB)
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
#include "../prof.h"

#include <cinttypes>
#include <cstdio>
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

/* Ring of the last 32 source-chain tags walked, per channel, kept across
 * kicks. A peripheral fatal raised from inside sink_payload happens while
 * the newest entry's payload is being fed, so this ring names the guest
 * bytes the failing stream came from. */
struct TagRec {
    uint32_t tadr, id, qwc, taddr;
    uint64_t kick;
};
TagRec g_tag_ring[10][32];
uint64_t g_tag_count[10];

/* Start-of-kick snapshot, per channel: the pointers the guest handed the
 * channel before the transfer ran, kept so the tag dump can say where the
 * walk that is now failing began. Written once per kick, read only from
 * rt_dmac_dump_recent_tags. */
struct KickRec {
    uint32_t tadr, madr, chcr;
    uint64_t kick;
    uint64_t tags_at_start;
    bool valid;
};
KickRec g_kick[10];

/* D_CTRL. DMAE starts set because that is the state the EE kernel leaves the
 * DMAC in before a game's ELF runs, and this runtime HLEs the kernel, so
 * nothing else would ever set it. Inferred, not measured: the supporting
 * evidence is that the game's own libdma read-modify-writes D_CTRL while
 * preserving bit 0 (the libdma helper at 0x00244748: "lw $10, 0x0($2)" at
 * 0x244758 with $2 = 0x1000E000,
 * "and $10, $10, 0xFFFFFFFD" at 0x244878, "sw $10, 0x0($4)" at 0x2448AC),
 * which only leaves the DMAC enabled if something before the game had
 * already enabled it. The first guest write that sets it explicitly is
 * StageOrientInit ("ori $2, $2, 0x3" at 0x19D324), well after the boot
 * DMA traffic starts. */
uint32_t g_dctrl = 1;
uint32_t g_dpcr = 0;
uint32_t g_dsqwc = 0;
uint32_t g_rbsr = 0, g_rbor = 0, g_stadr = 0;
uint32_t g_enable = 0; /* D_ENABLEW shadow, read back via D_ENABLER */

/* ---- suspend and the pending-kick queue ---------------------------------
 *
 * D_ENABLEW (0x1000F590) bit 16 suspends the whole DMAC: no channel
 * advances while it is set, and a CHCR write with STR=1 arms the channel
 * without starting it. Clearing the bit starts everything that is armed.
 * D_CTRL.DMAE=0 holds the DMAC the same way.
 *
 * The MPEG library brackets every one of its IPU channel starts and stops
 * with that bit. Measured across all eleven sites in this binary, the
 * window contains CHCR accesses and nothing else: one or more of them,
 * reads as well as writes, and never a MADR/TADR/QWC access. The common
 * shape is the shared helper at 0x00258690 (the MPEG library carries its
 * own inlined copies), described in prose
 * rather than transcribed: it disables interrupts, reads D_ENABLER
 * (0x1000F520), sets bit 16 and stores the result to D_ENABLEW
 * (0x1000F590), does one store to ch4 CHCR (0x1000B400), then reads
 * D_ENABLER again, masks bit 16 back off with 0xFFFEFFFF, stores that to
 * D_ENABLEW and tail-calls the interrupt re-enable.
 *
 * Both directions go through it: a stop writes CHCR = 5 or 0 (STR clear),
 * a start writes CHCR with bit 8 set (in the function at 0x00240218, once
 * for ch4 and once for ch3, each an "ori ..., 0x100" fed into that
 * store).
 *
 * Two things this measurement settles, against the guess that the library
 * holds the DMAC still while it samples the channel:
 *
 *   - MADR/TADR/QWC and IPU_BP are read back only after the suspend is
 *     cleared and interrupts are re-enabled (func_002586F8.s: the call to
 *     the helper at line 9, then the ch4 MADR load at line 15). What makes
 *     those values stable is the CHCR write that just cleared STR, not the
 *     suspend.
 *   - MADR/TADR/QWC are always written bare, outside any window
 *     (func_00240218.s lines 187-191 for ch3 and 248-252 for ch4).
 *
 * The teardown paths are the ones that put more than one access in the
 * window, and they read CHCR inside it: the teardown at 0x002517D0 sets
 * the suspend, then read-modify-writes ch3 CHCR and ch4 CHCR with the
 * 0xFFFFFEFF mask (STR clear) before releasing; the one at 0x00252488
 * stores to ch3, ch4 and ch9 (toSPR) CHCR in one window. Both then write
 * QWC = 0 after the release. So the shadow has to answer a CHCR read while
 * held with STR still set, which is what an armed-but-not-started channel
 * reads as on hardware, and a write that clears STR has to disarm it.
 *
 * So the suspend is an atomicity bracket around a single CHCR store, not a
 * long hold. Queueing the kick and running it at the release is therefore
 * the same transfer three instructions later. It is still what the hardware
 * does, and this model now does it, but it is not on its own an explanation
 * for the movie stalling: nothing observable happens between the two
 * D_ENABLEW writes.
 */
bool g_pending[10];      /* CHCR.STR seen while held, waiting for release */
bool g_pend_logged[10];  /* one log line per channel per hold window */

bool dma_held() { return (g_enable & 0x10000u) != 0 || (g_dctrl & 1) == 0; }

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
        /* The RAM path below refuses an unmapped page; this one used to
         * add the offset to whatever the page table held, which for a null
         * page is a small absolute address. Same fatal, same reason. */
        if (!page) {
            rt_fatal("dmac", rt_fault_ctx(),
                "DMA touches the scratchpad (address 0x%08x) but no scratchpad page is mapped",
                addr);
        }
        return page + off;
    }
    uint8_t* p = g_pages[addr >> 16];
    if (!p) {
        rt_fatal("dmac", rt_fault_ctx(), "DMA touches unmapped guest address 0x%08x", addr);
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

/* addr/spr name where the payload is read from; for ch9 the payload is
 * written to the scratchpad at c.sadr, so the sink needs the channel. */
void sink_payload(int ch, Channel& c, uint32_t addr, uint32_t qwc, bool spr, std::vector<uint8_t>& gif_accum) {
    if (qwc == 0) return;
    if (ch == 9) {
        /* toSPR: the destination is the scratchpad at SADR, which advances
         * by the quadwords transferred and wraps at the architectural 16 KB.
         * Source chain reaches here the same way normal mode does, only with
         * the source address coming from a tag instead of MADR (EE User's
         * Manual, DMAC chapter, toSPR channel; ps2tek "DMAC", D9_SADR). */
        for (uint32_t q = 0; q < qwc; ++q) {
            std::memcpy(dma_ptr(c.sadr & 0x3FFF, true), dma_ptr(addr + q * 16, spr), 16);
            c.sadr = (c.sadr + 16) & 0x3FFF;
        }
        return;
    }
    if (ch == 1) {
        static std::vector<uint8_t> buf;
        buf.clear();
        gather(buf, addr, qwc, spr);
        /* Hand VIF1 the address dma_ptr resolved, scratchpad bit included,
         * so a fatal in the command stream can point back at these bytes. */
        uint32_t gaddr = spr ? (addr | 0x80000000u) : addr;
        rt_vif1_feed(reinterpret_cast<const uint32_t*>(buf.data()), qwc * 4, gaddr);
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

    for (;;) {
        if (++tags > kTagCap) {
            rt_dmac_dump_recent_tags(ch);
            rt_fatal("dmac", rt_fault_ctx(), "ch%d (%s) source chain exceeded %u tags; runaway TADR=0x%08x STADR=0x%08x D_CTRL=0x%08x",
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
        g_tag_ring[ch][g_tag_count[ch] % 32] = {c.tadr, id, qwc, taddr, c.kicks};
        ++g_tag_count[ch];

        if (rt_trace()) {
            rt_log_debug("dmac", "ch%d tag #%u @0x%08x: %s qwc=%u addr=0x%08x%s irq=%d",
                ch, tags, c.tadr, tag_id_name(id), qwc, taddr, tspr ? " SPR" : "", irq ? 1 : 0);
        }

        if (tte && ch == 1) {
            /* Tag words 2-3 go to VIF1 as VIFcodes. */
            uint32_t w[2];
            std::memcpy(w, tagbuf + 8, 8);
            rt_vif1_feed(w, 2, c.tadr + 8);
        } else if (tte && ch != 9 && is_pow2(c.kicks)) {
            rt_log_warn("dmac", "ch%d (%s): TTE set on a non-VIF1 chain; tag words dropped", ch, kDesc[ch].name);
        }
        /* ch9 is left out of that log on purpose: toSPR has no peripheral
         * FIFO to receive tag words, so hardware ignores TTE there and
         * transfers no tag words anywhere (EE User's Manual, DMAC chapter,
         * CHCR.TTE; ps2tek "DMAC"). Dropping them is the hardware result,
         * not a gap in the model. */

        bool end = false;
        uint32_t next_tadr = c.tadr;
        switch (id) {
            case 0: /* REFE */
                c.madr = taddr | (tspr ? 0x80000000u : 0);
                sink_payload(ch, c, c.madr, qwc, tspr, gif_accum);
                next_tadr = c.tadr + 16;
                end = true;
                break;
            case 1: /* CNT */
                c.madr = c.tadr + 16;
                sink_payload(ch, c, c.madr, qwc, tadr_spr, gif_accum);
                next_tadr = c.madr + qwc * 16;
                break;
            case 2: /* NEXT */
                c.madr = c.tadr + 16;
                sink_payload(ch, c, c.madr, qwc, tadr_spr, gif_accum);
                next_tadr = taddr | (tspr ? 0x80000000u : 0);
                break;
            case 3: /* REF */
            case 4: /* REFS (stall control not modeled; loud below) */
                if (id == 4 && (g_dctrl & 0xC0u)) {
                    static uint64_t n = 0;
                    if (is_pow2(++n)) rt_log_warn("dmac", "REFS with D_CTRL.STS armed (0x%x); stall not modeled [#%" PRIu64 "]", g_dctrl, n);
                }
                c.madr = taddr | (tspr ? 0x80000000u : 0);
                sink_payload(ch, c, c.madr, qwc, tspr, gif_accum);
                next_tadr = c.tadr + 16;
                break;
            case 5: /* CALL */
                c.madr = c.tadr + 16;
                sink_payload(ch, c, c.madr, qwc, tadr_spr, gif_accum);
                if (asp == 0) { c.asr0 = c.madr + qwc * 16; asp = 1; }
                else if (asp == 1) { c.asr1 = c.madr + qwc * 16; asp = 2; }
                else {
                    rt_fatal("dmac", rt_fault_ctx(), "ch%d CALL with ASR stack full (asp=2)", ch);
                }
                next_tadr = taddr | (tspr ? 0x80000000u : 0);
                break;
            case 6: /* RET */
                c.madr = c.tadr + 16;
                sink_payload(ch, c, c.madr, qwc, tadr_spr, gif_accum);
                if (asp == 2) { next_tadr = c.asr1; asp = 1; }
                else if (asp == 1) { next_tadr = c.asr0; asp = 0; }
                else { next_tadr = c.tadr + 16; end = true; }
                break;
            default: /* 7: END */
                c.madr = c.tadr + 16;
                sink_payload(ch, c, c.madr, qwc, tadr_spr, gif_accum);
                next_tadr = c.madr + qwc * 16;
                end = true;
                break;
        }
        total_qw += qwc;
        c.tadr = next_tadr;
        /* MADR runs through the payload as it is read, so it ends past the
         * last quadword transferred. Only ch9 does this here: next_tadr is
         * computed from the pre-payload MADR just above, and ch1/ch2 have
         * always left MADR at the start of the last payload. Advancing it
         * after next_tadr is settled changes nothing about the walk. */
        if (ch == 9) c.madr += qwc * 16;
        if (irq && tie) end = true;
        if (end) break;
    }

    c.chcr = (c.chcr & ~0x30u) | (asp << 4);
    if (ch == 2 && !gif_accum.empty()) {
        rt_gif_submit(2, gif_accum.data(), (uint32_t)(gif_accum.size() / 16));
    }
    if (rt_trace() || is_pow2(c.kicks)) {
        rt_log_debug("dmac", "ch%d (%s) chain done: %u tags, %u qw [kick #%" PRIu64 "]",
            ch, kDesc[ch].name, tags, total_qw, c.kicks);
    }
}

void run_normal(int ch, Channel& c) {
    uint32_t qwc = c.qwc & 0xFFFF;
    bool spr = (c.madr & 0x80000000u) != 0;
    switch (ch) {
        case 2: {
            std::vector<uint8_t> gif_accum;
            sink_payload(ch, c, c.madr, qwc, spr, gif_accum);
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
        rt_log_debug("dmac", "ch%d (%s) normal done: %u qw [kick #%" PRIu64 "]", ch, kDesc[ch].name, qwc, c.kicks);
    }
}

void kick(int ch, Channel& c) {
    /* Tag walk and the gather into the scratch buffer are billed here;
     * the VIF1, GIF, IPU and GS work the payload triggers opens its own
     * zone and is subtracted back out. */
    RT_PROF_ZONE(RT_PROF_DMA);
    ++c.kicks;
    /* Snapshot before anything walks or advances the pointers, so the
     * fatal-path dump reports the kick as the guest programmed it. */
    g_kick[ch] = {c.tadr, c.madr, c.chcr, c.kicks, g_tag_count[ch], true};
    uint32_t dir = c.chcr & 1;
    uint32_t mode = (c.chcr >> 2) & 3;

    switch (ch) {
        case 1: /* VIF1 */
            if (dir == 0) {
                /* One line per kick if it ever became a per-field thing.
                 * Fold on this condition's own counter, first few kept. */
                static uint64_t from_kicks = 0;
                ++from_kicks;
                if (from_kicks <= 4 || (from_kicks & (from_kicks - 1)) == 0) {
                    rt_log_warn("dmac", "ch1 VIF1 kicked in FROM direction (GS readback); not "
                        "modeled, dropped [#%" PRIu64 "]", from_kicks);
                }
                break;
            }
            if (mode == 1) run_source_chain(ch, c);
            else if (mode == 0) {
                static std::vector<uint8_t> buf;
                buf.clear();
                gather(buf, c.madr, c.qwc & 0xFFFF, (c.madr & 0x80000000u) != 0);
                rt_vif1_feed(reinterpret_cast<const uint32_t*>(buf.data()), (c.qwc & 0xFFFF) * 4, c.madr);
                c.madr += (c.qwc & 0xFFFF) * 16;
                if (rt_trace() || is_pow2(c.kicks)) {
                    rt_log_debug("dmac", "ch1 (VIF1) normal done: %u qw [kick #%" PRIu64 "]", c.qwc & 0xFFFF, c.kicks);
                }
            } else {
                rt_fatal("dmac", rt_fault_ctx(), "ch1 VIF1 kicked in interleave mode");
            }
            break;
        case 2: /* GIF */
            if (mode == 1) run_source_chain(ch, c);
            else if (mode == 0) run_normal(ch, c);
            else rt_fatal("dmac", rt_fault_ctx(), "ch2 GIF kicked in interleave mode");
            break;
        case 8: /* fromSPR */
            if (mode == 1) {
                /* fromSPR is a destination-chain channel: the tags come from
                 * the data it reads out of the scratchpad, not from TADR
                 * (which the channel does not have). Nothing in this binary
                 * kicks it that way, so it stays a fatal rather than a guess.
                 * EE User's Manual, DMAC chapter; ps2tek "DMAC". */
                rt_fatal("dmac", rt_fault_ctx(), "ch8 (fromSPR) kicked in chain mode; destination chain is not modeled");
            }
            run_normal(ch, c);
            break;
        case 9: /* toSPR */
            /* Source chain: tags are read from TADR in main RAM (or the
             * scratchpad, when a tag address carries the SPR bit) with the
             * same REFE/CNT/NEXT/REF/REFS/CALL/RET/END semantics as VIF1,
             * and every payload is written to the scratchpad at SADR.
             * Measured use in this binary: the MPEG library's macroblock
             * copy builds a list of REF tags with qwc=0x30, terminated by a
             * REFE, and kicks CHCR=0x105 (the macroblock copy at
             * 0x00252F90: its tag loop at 0x002530D0 writes "0x30000030"
             * and "id << 28 | 0x30" pairs, then the SADR/TADR/QWC/CHCR
             * stores at 0x00253150). */
            if (mode == 1) run_source_chain(ch, c);
            else run_normal(ch, c);
            break;
        case 3: case 4: /* ---- IPU section: hw/ipu.cpp owns these ---- */
            /* The IPU model executes and completes ch3/ch4 itself (STR
             * clear + rt_dmac_raise), possibly deferred until a command
             * produces or consumes data, so the standard completion tail
             * below must not run. */
            rt_ipu_dma_kick(ch);
            return;
        case 0: /* VIF0: loud stub */
            rt_log_warn("dmac", "ch0 (VIF0) kicked (madr=0x%08x qwc=%u): STUB, transfer dropped [kick #%" PRIu64 "]",
                c.madr, c.qwc, c.kicks);
            break;
        default: /* SIF channels: HLE'd at the SifSetDma layer */
            rt_log_warn("dmac", "ch%d (%s) kicked via CHCR (madr=0x%08x qwc=%u): STUB (SIF DMA is HLE'd), dropped [kick #%" PRIu64 "]",
                ch, kDesc[ch].name, c.madr, c.qwc, c.kicks);
            break;
    }

    /* Completion: STR off, QWC drained, D_STAT channel bit up. The guest
     * handler runs from the deferred delivery path, not here. */
    c.chcr &= ~0x100u;
    c.qwc = 0;
    rt_dmac_raise(ch);
}

/* CHCR.STR written while the DMAC is held. The shadow keeps STR set, every
 * other register is left exactly as the guest wrote it, and nothing runs. */
void record_pending(int ch) {
    g_pending[ch] = true;
    if (!g_pend_logged[ch]) {
        g_pend_logged[ch] = true;
        rt_log_info("dmac", "ch%d (%s) started while the DMAC is held (D_ENABLE=0x%08x D_CTRL=0x%08x); "
                       "kick queued until release",
            ch, kDesc[ch].name, g_enable, g_dctrl);
    }
}

/* Release: run everything armed, in channel-number order, through the same
 * path a normal kick takes. */
void release_pending(const char* why) {
    int list[10];
    int n = 0;
    for (int ch = 0; ch < 10; ++ch) {
        g_pend_logged[ch] = false;
        if (g_pending[ch]) list[n++] = ch;
    }
    char names[80] = "";
    int at = 0;
    for (int i = 0; i < n; ++i) {
        at += std::snprintf(names + at, sizeof(names) - (size_t)at, "%sch%d (%s)",
            i ? ", " : "", list[i], kDesc[list[i]].name);
        if (at >= (int)sizeof(names)) { at = (int)sizeof(names) - 1; break; }
    }
    /* Logged before the transfers run so the line survives a fatal raised
     * from inside one of them. A release with nothing queued is the normal
     * case for the game's suspend bracket around a single CHCR store, so it
     * is not worth a line. */
    if (n > 0) {
        rt_log_info("dmac", "%s: running %d queued kick%s (%s)", why, n, n == 1 ? "" : "s", names);
    }
    bool ipu_kicked = false;
    for (int i = 0; i < n; ++i) {
        const int ch = list[i];
        g_pending[ch] = false;
        if (ch == 3 || ch == 4) ipu_kicked = true;
        kick(ch, g_ch[ch]);
    }
    /* The IPU pulls its ch4 source lazily and that walk is frozen while the
     * DMAC is held (rt_dmac_suspended in hw/ipu.cpp). If the release did not
     * itself kick ch3/ch4, a command left stalled for input has to be given
     * another chance to pull now that the channel can advance again. */
    if (!ipu_kicked) rt_ipu_dma_resume();
}

} // namespace

/* True while the DMAC is held and no channel may advance: D_ENABLE bit 16
 * (suspend) is set, or D_CTRL.DMAE is clear. hw/ipu.cpp asks before pulling
 * the next qword of its ch4 source. */
bool rt_dmac_suspended() { return dma_held(); }

/* ---- diagnostics -------------------------------------------------------- */

void rt_dmac_dump_recent_tags(int ch) {
    if (ch < 0 || ch >= 10) {
        rt_log_warn("dmac", "recent tags requested for channel %d, which does not exist", ch);
        return;
    }
    const KickRec& k = g_kick[ch];
    if (k.valid) {
        rt_log_info("dmac", "ch%d kick #%" PRIu64 " started at TADR=0x%08x MADR=0x%08x CHCR=0x%08x, %u tags walked so far",
            ch, k.kick, k.tadr, k.madr, k.chcr,
            (uint32_t)(g_tag_count[ch] - k.tags_at_start));
    } else {
        rt_log_info("dmac", "ch%d has not been kicked", ch);
    }
    const uint64_t n = g_tag_count[ch];
    if (n == 0) {
        rt_log_info("dmac", "ch%d has walked no source-chain tags", ch);
        return;
    }
    const uint32_t shown = (uint32_t)(n < 32 ? n : 32);
    for (uint32_t i = 0; i < shown; ++i) {
        const uint64_t idx = n - shown + i;
        const TagRec& t = g_tag_ring[ch][idx % 32];
        rt_log_info("dmac", "ch%d recent tag[%u]: @0x%08x %s qwc=%u addr=0x%08x kick=#%" PRIu64 "%s",
            ch, i, t.tadr, tag_id_name(t.id), t.qwc, t.taddr, t.kick,
            idx + 1 == n ? " <- current" : "");
    }
}

/* Appends one source-chain tag to a channel's ring. hw/ipu.cpp walks the
 * ch4 chain itself (the IPU owns that channel's execution), so it is the
 * only caller: without it the ch4 rows of rt_dmac_dump_recent_tags would
 * always be empty, which is exactly the channel a movie wedge is about.
 * The kick number comes from this file's own counter, so a ch4 row reads
 * the same way a ch1 or ch9 row does. */
void rt_dmac_record_tag(int ch, uint32_t tadr, uint32_t id, uint32_t qwc, uint32_t taddr) {
    if (ch < 0 || ch >= 10) return;
    g_tag_ring[ch][g_tag_count[ch] % 32] = {tadr, id, qwc, taddr, g_ch[ch].kicks};
    ++g_tag_count[ch];
}

/* Newest source-chain tag walked on a channel, the one marked "<- current"
 * by rt_dmac_dump_recent_tags. Reads the same ring rather than keeping a
 * second copy. False when the channel has walked no tags. */
bool rt_dmac_current_tag(int ch, uint32_t* id, uint32_t* addr, uint32_t* qwc) {
    if (ch < 0 || ch >= 10 || g_tag_count[ch] == 0) return false;
    const TagRec& t = g_tag_ring[ch][(g_tag_count[ch] - 1) % 32];
    if (id) *id = t.id;
    if (addr) *addr = t.taddr;
    if (qwc) *qwc = t.qwc;
    return true;
}

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
        case 0x1000F520: *out = g_enable; return true; /* D_ENABLER */
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
                if (v & 0x100u) {
                    if (dma_held()) record_pending(ch);
                    else kick(ch, c);
                } else {
                    if (g_pending[ch]) {
                        /* STR cleared while held: the channel is no longer
                         * started, so the queued kick goes away. The MPEG
                         * library's stop/restart sequence does this. */
                        g_pending[ch] = false;
                        rt_log_warn("dmac", "ch%d (%s) stopped (CHCR without STR) while held; queued kick dropped",
                            ch, kDesc[ch].name);
                    }
                    /* A transfer already running on an IPU channel stops
                     * here, at the store, not lazily at the next pull: the
                     * library reads MADR/QWC/TADR back in the next few
                     * instructions and its restart arithmetic is built on
                     * them standing still. */
                    if (ch == 3 || ch == 4) rt_ipu_dma_stop(ch);
                }
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
        case 0x1000E000: {
            const bool was_held = dma_held();
            g_dctrl = v;
            if (v & ~1u) {
                /* StageOrientInit writes 0x3, and anything that writes
                 * D_CTRL once tends to write it every field. Keep the first
                 * few and the distinct values, fold the rest: the value is
                 * in the line, so a new bit pattern still shows up as a
                 * fresh first line. */
                static uint64_t ctrl_writes = 0;
                static uint32_t last_value = 0;
                ++ctrl_writes;
                if (v != last_value || ctrl_writes <= 4
                    || (ctrl_writes & (ctrl_writes - 1)) == 0) {
                    last_value = v;
                    rt_log_warn("dmac", "D_CTRL = 0x%08x arms unmodeled features "
                        "(RELE/MFD/STS/STD); loud stub [#%" PRIu64 "]", v, ctrl_writes);
                }
            }
            if (was_held && !dma_held()) release_pending("D_CTRL.DMAE set");
            return true;
        }
        case 0x1000E020: g_dpcr = v; return true;
        case 0x1000E030: g_dsqwc = v; return true;
        case 0x1000E040:
            g_rbsr = v;
            if (v) rt_fatal("dmac", rt_fault_ctx(), "D_RBSR = 0x%08x arms MFIFO; not modeled (loud stub per plan)", v);
            return true;
        case 0x1000E050:
            g_rbor = v;
            if (v) rt_fatal("dmac", rt_fault_ctx(), "D_RBOR = 0x%08x arms MFIFO; not modeled (loud stub per plan)", v);
            return true;
        case 0x1000E060:
            g_stadr = v;
            if (v) rt_log_warn("dmac", "D_STADR = 0x%08x written; stall control is not modeled (loud stub)", v);
            return true;
        case 0x1000F590: { /* D_ENABLEW */
            const bool was_held = dma_held();
            g_enable = v;
            if (was_held && !dma_held()) release_pending("D_ENABLE suspend cleared");
            return true;
        }
        default: return false;
    }
}
