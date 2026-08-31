/* sif/sif.cpp: SIF register file, SifSetDma recording, and a minimal
 * instantly-ready IOP for the sifcmd handshake.
 *
 * The IOP is HLE'd at the SIF RPC layer (CLAUDE.md). P2 does not implement
 * any RPC service; this module provides just enough of the layers underneath
 * for the EE-side Sony libraries to finish their local initialization:
 *
 *  1. Register file. Kernel registers 1-4 (MAINADDR/SUBADDR/MSFLAG/SMFLAG,
 *     mirrored at MMIO 0x1000F200/F210/F220/F230) plus 32 user registers
 *     addressed as 0x80000000|n (ps2sdk ee/kernel/include/sifdma.h:
 *     SIF_REG_ID_SYSTEM). SMFLAG boots with SIF_STAT_SIFINIT |
 *     SIF_STAT_CMDINIT | SIF_STAT_BOOTEND (0x70000) already set: the virtual
 *     IOP is ready before the EE asks.
 *  2. SifSetDma: every transfer is logged and recorded (ring buffer) for the
 *     P3+ RPC HLE to mount on; returns a nonzero rising dma id.
 *  3. sifcmd INIT handshake: when a SifSetDma payload is a sifcmd packet
 *     with cid SIF_CMD_INIT_CMD (0x80000002), the virtual IOP schedules a
 *     deferred response: a SIF_CMD_SET_SREG (0x80000001) packet {sreg=0,
 *     val=1} written into the EE's declared receive buffer, followed by a
 *     DMAC SIF0 (channel 5) interrupt. The EE library's own translated
 *     interrupt handler then parses the packet and marks RPC init done.
 *     Wire format per ps2sdk common/ee sifcmd headers: 16-byte header
 *     {u32 psize:8|dsize:24, u32 dest, s32 cid, u32 opt}, payload follows.
 *     The response is delivered deferred (never inside the SifSetDma
 *     syscall's register writeback), per CLAUDE.md's DMA rule.
 */
#include "../runtime/ee/kernel.h"

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
constexpr uint32_t SIF_CMD_INIT_CMD = 0x80000002u;
/* ps2sdk ee/kernel/include/sifcmd.h system cids for the RPC layer. */
constexpr uint32_t SIF_CMD_RPC_END = 0x80000008u;
constexpr uint32_t SIF_CMD_RPC_BIND = 0x80000009u;
constexpr uint32_t SIF_CMD_RPC_CALL = 0x8000000Au;
constexpr uint32_t SIF_CMD_RPC_RDATA = 0x8000000Cu;

/* Well-known IOP RPC server ids (public SDK facts, ps2sdk iop module
 * sources), for log legibility only. */
const char* rpc_server_name(uint32_t sid) {
    switch (sid) {
        case 0x80000001: return "fileio";
        case 0x80000003: return "iopheap";
        case 0x80000006: return "loadfile";
        case 0x80000100: return "padman";
        case 0x80000101: return "padman(ext)";
        case 0x80000400: return "mcserv";
        case 0x80000592: return "cdvd:init";
        case 0x80000593: return "cdvd:scmd";
        case 0x80000595: return "cdvd:ncmd";
        case 0x80000596: return "cdvd:search";
        case 0x80000597: return "cdvd:diskready";
        case 0x80000701: return "libsd/sdrpc";
        case 0x80000901: return "mcman?";
        default: return nullptr;
    }
}

/* Fictional IOP-side sifcmd packet buffer address handed to the EE via
 * SUBADDR/SMCOM. The EE only echoes it back inside packets; any nonzero
 * IOP-RAM-plausible value works. */
constexpr uint32_t kIopCmdBuffer = 0x000BD000u;

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

/* Deferred virtual-IOP response. */
bool g_resp_pending = false;
uint64_t g_resp_due = 0;
uint32_t g_resp_ee_buf = 0;

uint64_t g_ee_pktbuf_logged = 0;

void record_dma(uint32_t src, uint32_t dest, uint32_t size, uint32_t attr, uint32_t id, uint32_t cid) {
    DmaRecord& r = g_dma_ring[g_dma_count % kDmaRingSize];
    r = DmaRecord{src, dest, size, attr, id, rt_clock_now(), cid};
    ++g_dma_count;
}

} // namespace

void rt_sif_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    std::memset(g_ureg, 0, sizeof(g_ureg));
    g_reg[SIF_REG_SUBADDR] = kIopCmdBuffer;
    g_reg[SIF_REG_SMFLAG] = SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND;
    rt_log("sif", "init: virtual IOP ready (SMFLAG=0x%05x, IOP cmd buffer=0x%06x)",
        g_reg[SIF_REG_SMFLAG], kIopCmdBuffer);
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
        if (size >= 16 && rt_gptr(src)) {
            /* Peek the sifcmd header: {psize:8|dsize:24, dest, cid, opt}. */
            uint32_t w0 = rt_gread32(src + 0);
            uint32_t maybe_cid = rt_gread32(src + 8);
            if ((w0 & 0xFF) >= 16 && (maybe_cid & 0x80000000u)) cid = maybe_cid;
        }
        rt_log("sif", "SifSetDma id=%u entry %u/%u: src=0x%08x dest(IOP)=0x%08x size=%u attr=0x%x%s%s0x%08x",
            id, i + 1, count, src, dest, size, attr,
            cid ? " cid=" : "", cid ? "" : " opt=", cid ? cid : (size >= 16 && rt_gptr(src) ? rt_gread32(src + 12) : 0));
        record_dma(src, dest, size, attr, id, cid);
        if (cid == SIF_CMD_RPC_BIND && size >= 36) {
            /* SifRpcBindPkt_t: sifcmd header(16), rec_id(+16), pkt_addr(+20),
             * rpc_id(+24), client(+28), sid(+32). (ps2sdk sifrpc.h) */
            uint32_t sid = rt_gread32(src + 32);
            const char* nm = rpc_server_name(sid);
            rt_log("sif", "RPC BIND request: server id 0x%08x%s%s (client=0x%08x). "
                "P2 has no RPC services; the caller's wait will never complete.",
                sid, nm ? " = " : "", nm ? nm : "", rt_gread32(src + 28));
        } else if (cid == SIF_CMD_RPC_CALL && size >= 40) {
            /* SifRpcCallPkt_t: header(16), rec_id, pkt_addr, rpc_id,
             * client(+28), rpc_number(+32), send_size(+36). */
            uint32_t fno = rt_gread32(src + 32);
            rt_log("sif", "RPC CALL request: client=0x%08x fno=0x%x send_size=%u. "
                "P2 has no RPC services; the caller's wait will never complete.",
                rt_gread32(src + 28), fno, rt_gread32(src + 36));
        }
        if (cid == SIF_CMD_INIT_CMD) {
            /* INIT packet payload word 0 (offset 16) is the EE receive
             * buffer address (ps2sdk ee/kernel/src/sifcmd.c init packet). */
            uint32_t opt = rt_gread32(src + 12);
            if (size >= 20 && opt == 0) {
                g_resp_ee_buf = rt_gread32(src + 16) & 0x1FFFFFFFu;
                g_resp_pending = true;
                g_resp_due = rt_clock_now() + 4096; /* ~28 us virtual SIF latency */
                rt_log("sif", "sifcmd INIT_CMD seen: EE pktbuf=0x%08x; scheduling SET_SREG(0,1) response",
                    g_resp_ee_buf);
            } else {
                rt_log("sif", "sifcmd INIT_CMD with opt=%u (size=%u): no response modeled", opt, size);
            }
        }
    }
    return id;
}

int rt_sif_dma_stat(uint32_t id) {
    (void)id;
    /* Negative = transfer completed (kernel sceSifDmaStat convention);
     * the virtual SIF completes everything instantly. */
    return -1;
}

uint64_t rt_sif_next_event() {
    return g_resp_pending ? g_resp_due : UINT64_MAX;
}

void rt_sif_run_due() {
    if (!g_resp_pending || rt_clock_now() < g_resp_due) return;
    g_resp_pending = false;
    uint32_t buf = g_resp_ee_buf;
    if (!rt_gptr(buf)) {
        rt_log("sif", "SET_SREG response dropped: EE pktbuf 0x%08x unmapped", buf);
        return;
    }
    /* SIF_CMD_SET_SREG packet: header {psize=24, dsize=0, dest=0,
     * cid=0x80000001, opt=0}, payload {u32 sreg=0, u32 val=1}. */
    rt_gwrite32(buf + 0, 24);              /* psize=24, dsize=0 */
    rt_gwrite32(buf + 4, 0);
    rt_gwrite32(buf + 8, SIF_CMD_SET_SREG);
    rt_gwrite32(buf + 12, 0);
    rt_gwrite32(buf + 16, 0);              /* sreg index 0 (RPC init flag) */
    rt_gwrite32(buf + 20, 1);              /* value 1 */
    rt_dmac_raise(RT_DMAC_SIF0);
    rt_log("sif", "virtual IOP: SET_SREG(0,1) packet written to 0x%08x, DMAC SIF0 raised", buf);
    ++g_ee_pktbuf_logged;
}

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
}
