/* sif/sif.cpp: SIF register file, SifSetDma recording, and the transfer
 * entry point into the virtual IOP's RPC layer (sif/rpc.cpp).
 *
 * The IOP is HLE'd at the SIF RPC layer (CLAUDE.md). This module provides
 * the layers underneath:
 *
 *  1. Register file. Kernel registers 1-4 (MAINADDR/SUBADDR/MSFLAG/SMFLAG,
 *     mirrored at MMIO 0x1000F200/F210/F220/F230) plus 32 user registers
 *     addressed as 0x80000000|n (ps2sdk ee/kernel/include/sifdma.h:
 *     SIF_REG_ID_SYSTEM). SMFLAG boots with SIF_STAT_SIFINIT |
 *     SIF_STAT_CMDINIT | SIF_STAT_BOOTEND (0x70000) already set: the virtual
 *     IOP is ready before the EE asks.
 *  2. SifSetDma: every transfer is logged and recorded (ring buffer), then
 *     handed to rt_rpc_on_dma_entry, which stages the payload in virtual
 *     IOP RAM and parses sifcmd packets (INIT handshake, RPC BIND/CALL).
 *     Responses come back deferred through the virtual-clock timeline as
 *     sifcmd packets + a DMAC SIF0 interrupt (see rpc.cpp), never inside
 *     the SifSetDma syscall itself, per CLAUDE.md's DMA rule.
 *  3. The runtime-discovered HLE hook for the sifcmd SET_SREG system
 *     handler (a data-referenced sub-entry inside the handwritten libkernel
 *     blob the translator cannot cover).
 */
#include "rpc.h"

#include <cinttypes>

namespace {

/* ps2sdk ee/kernel/include/sifdma.h */
constexpr uint32_t SIF_REG_MAINADDR = 1;
constexpr uint32_t SIF_REG_SUBADDR = 2;
constexpr uint32_t SIF_REG_MSFLAG = 3;
constexpr uint32_t SIF_REG_SMFLAG = 4;
constexpr uint32_t SIF_STAT_SIFINIT = 0x10000;
constexpr uint32_t SIF_STAT_CMDINIT = 0x20000;
constexpr uint32_t SIF_STAT_BOOTEND = 0x40000;

constexpr uint32_t SIF_CMD_SET_SREG = 0x80000001u;

uint32_t g_reg[32];        /* kernel registers, index 1..31 */
uint32_t g_ureg[32];       /* user registers, 0x80000000|n */

struct DmaRecord {
    uint32_t src, dest, size, attr;
    uint32_t id;
    uint64_t vclk;
    uint32_t cid;          /* sifcmd id if the payload looks like a packet */
};

constexpr int kDmaRingSize = 64;
DmaRecord g_dma_ring[kDmaRingSize];
uint32_t g_dma_count = 0;
uint32_t g_dma_id = 0;

void record_dma(uint32_t src, uint32_t dest, uint32_t size, uint32_t attr, uint32_t id, uint32_t cid) {
    DmaRecord& r = g_dma_ring[g_dma_count % kDmaRingSize];
    r = DmaRecord{src, dest, size, attr, id, rt_clock_now(), cid};
    ++g_dma_count;
}

} // namespace

void rt_sif_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    std::memset(g_ureg, 0, sizeof(g_ureg));
    g_reg[SIF_REG_SUBADDR] = RT_SIF_IOP_CMDBUF;
    g_reg[SIF_REG_SMFLAG] = SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND;
    rt_log("sif", "init: virtual IOP ready (SMFLAG=0x%05x, IOP cmd buffer=0x%06x)",
        g_reg[SIF_REG_SMFLAG], RT_SIF_IOP_CMDBUF);
    rt_rpc_init();
}

uint32_t rt_sif_get_reg(uint32_t idx) {
    if (idx & 0x80000000u) return g_ureg[idx & 31];
    return (idx < 32) ? g_reg[idx] : 0;
}

void rt_sif_set_reg(uint32_t idx, uint32_t v) {
    if (idx & 0x80000000u) {
        g_ureg[idx & 31] = v;
        return;
    }
    if (idx < 32) {
        /* MSFLAG/SMFLAG writes from the EE OR bits in (the hardware register
         * write sets bits; clearing goes through a dedicated path we have
         * not seen this game use). Address registers are plain writes. */
        if (idx == SIF_REG_MSFLAG || idx == SIF_REG_SMFLAG) g_reg[idx] |= v;
        else g_reg[idx] = v;
    }
}

uint32_t rt_sif_set_dma(uint32_t tx_addr, uint32_t count) {
    uint32_t id = ++g_dma_id;
    for (uint32_t i = 0; i < count && i < 32; ++i) {
        /* SifDmaTransfer_t: {void* src; void* dest; int size; int attr}
         * (ps2sdk ee/kernel/include/sifdma.h). */
        uint32_t base = tx_addr + i * 16;
        uint32_t src = rt_gread32(base + 0);
        uint32_t dest = rt_gread32(base + 4);
        uint32_t size = rt_gread32(base + 8);
        uint32_t attr = rt_gread32(base + 12);
        uint32_t cid = 0;
        if ((dest & 0x1FFFFFFFu) == RT_SIF_IOP_CMDBUF && size >= 16 && rt_gptr(src)) {
            cid = rt_gread32(src + 8);
        }
        if (rt_trace() || cid || (g_dma_count & (g_dma_count + 1)) == 0) {
            rt_log("sif", "SifSetDma id=%u entry %u/%u: src=0x%08x dest(IOP)=0x%08x size=%u attr=0x%x cid=0x%08x",
                id, i + 1, count, src, dest, size, attr, cid);
        }
        record_dma(src, dest, size, attr, id, cid);
        rt_rpc_on_dma_entry(src, dest & 0x1FFFFFFFu, size);
    }
    return id;
}

int rt_sif_dma_stat(uint32_t id) {
    (void)id;
    /* Negative = transfer completed (kernel sceSifDmaStat convention);
     * the virtual SIF completes everything instantly. */
    return -1;
}

uint64_t rt_sif_next_event() { return rt_rpc_next_event(); }

void rt_sif_run_due() { rt_rpc_run_due(); }

bool rt_sif_mmio_read(uint32_t addr, uint32_t* out) {
    switch (addr) {
        case 0x1000F200: *out = g_reg[SIF_REG_MAINADDR]; return true; /* MSCOM */
        case 0x1000F210: *out = g_reg[SIF_REG_SUBADDR]; return true;  /* SMCOM */
        case 0x1000F220: *out = g_reg[SIF_REG_MSFLAG]; return true;   /* MSFLG */
        case 0x1000F230: *out = g_reg[SIF_REG_SMFLAG]; return true;   /* SMFLG */
        case 0x1000F240: *out = 0xF0000102u; return true;             /* SIF_CTRL: idle-ish */
        case 0x1000F260: *out = 0; return true;                       /* SIF_BD6 */
        default: return false;
    }
}

bool rt_sif_mmio_write(uint32_t addr, uint32_t v) {
    switch (addr) {
        case 0x1000F200: g_reg[SIF_REG_MAINADDR] = v; return true;
        case 0x1000F210: g_reg[SIF_REG_SUBADDR] = v; return true;
        case 0x1000F220: g_reg[SIF_REG_MSFLAG] |= v; return true;
        case 0x1000F230: g_reg[SIF_REG_SMFLAG] |= v; return true;
        case 0x1000F240: return true; /* SIF_CTRL pokes: accept */
        default: return false;
    }
}

namespace {

/* HLE for the sifcmd SET_SREG system handler, installed via rt_override at
 * the runtime-discovered address (see rt_sif_try_resolve_indirect).
 * Semantics per ps2sdk ee/kernel/src/sifcmd.c set_sreg():
 *   cmd_data->sregs[pkt->sreg] = pkt->val;
 * Call contract observed at the dispatch site: a0 = packet copy, a1 = the
 * handler arg registered at init (the library's cmd data block, whose sregs
 * array pointer sits at +0x1C in this SDK build). Returns &sregs[sreg]. */
void hle_sif_set_sreg(R5900Context* ctx) {
    uint32_t pkt = (uint32_t)ctx->r[4].u64x[0];
    uint32_t cmd_data = (uint32_t)ctx->r[5].u64x[0];
    uint32_t sreg = rt_gread32(pkt + 0x10);
    uint32_t val = rt_gread32(pkt + 0x14);
    uint32_t sregs = rt_gread32(cmd_data + 0x1C);
    uint32_t slot = sregs + sreg * 4;
    rt_gwrite32(slot, val);
    ctx->r[2].s64x[0] = (int64_t)(int32_t)slot; /* v0, matching the original leaf */
    rt_log("sif", "HLE set_sreg: sregs[%u] = %u (sregs array at 0x%08x)", sreg, val, sregs);
}

} // namespace

bool rt_sif_try_resolve_indirect(R5900Context* ctx, uint32_t target, uint32_t caller_vram) {
    /* Only claim the call if the dispatched packet (a0) carries the
     * SET_SREG system cid; that constant is public wire format (ps2sdk
     * sifcmd). The target address itself is discovered here at runtime, so
     * no ROM-derived address constant lives in this source. */
    uint32_t pkt = (uint32_t)ctx->r[4].u64x[0];
    if (!rt_gptr(pkt) || rt_gread32(pkt + 8) != SIF_CMD_SET_SREG) return false;
    rt_log("sif", "resolving untranslated sifcmd system handler at 0x%08x (caller 0x%08x) as SET_SREG HLE",
        target, caller_vram);
    rt_override(target, hle_sif_set_sreg);
    hle_sif_set_sreg(ctx);
    return true;
}

void rt_sif_dump_inventory() {
    rt_log("sif", "regs: MAINADDR=0x%08x SUBADDR=0x%08x MSFLAG=0x%08x SMFLAG=0x%08x",
        g_reg[SIF_REG_MAINADDR], g_reg[SIF_REG_SUBADDR], g_reg[SIF_REG_MSFLAG], g_reg[SIF_REG_SMFLAG]);
    for (int i = 0; i < 32; ++i) {
        if (g_ureg[i]) rt_log("sif", "ureg[0x80000000|%d] = 0x%08x", i, g_ureg[i]);
    }
    uint32_t n = g_dma_count < kDmaRingSize ? g_dma_count : kDmaRingSize;
    rt_log("sif", "SifSetDma transfers recorded: %u total, last %u:", g_dma_count, n);
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t idx = (g_dma_count - n + k) % kDmaRingSize;
        const DmaRecord& r = g_dma_ring[idx];
        rt_log("sif", "  id=%-4u src=0x%08x dest=0x%08x size=%-5u attr=0x%x cid=0x%08x vclk=%" PRIu64,
            r.id, r.src, r.dest, r.size, r.attr, r.cid, r.vclk);
    }
    rt_rpc_dump_inventory();
}
