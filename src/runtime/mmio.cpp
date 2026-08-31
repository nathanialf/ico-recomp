/* mmio.cpp: MMIO trap handlers for guest addresses whose page is NULL in
 * g_pages (0x10xxxxxx device registers, 0x12xxxxxx GS privileged registers).
 *
 * P1 policy: every access is logged (addr, size, value, running count per
 * address) and every read returns 0. Named-register lookup exists purely to
 * make logs legible; addresses are public PS2 hardware register addresses
 * (DMAC/GS/VIF/timer/INTC/SIF, as documented by e.g. psdevwiki/ps2tek), not
 * anything derived from the ICO decomp or ROM.
 *
 * Flood control: an access to the same address logs on its 1st, 2nd, 4th,
 * 8th, ... (power-of-two) occurrence, then the running count is folded into
 * every subsequent power-of-two log line so nothing is silently dropped from
 * view, but a tight poll loop does not spam the log once per instruction.
 */
#include "runtime.h"

#include "ee/kernel.h"
#include "hw/hw.h"

#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace {

struct NamedReg {
    uint32_t addr;
    const char* name;
};

/* Not exhaustive -- named only where useful for legibility, per plan scope. */
constexpr NamedReg kNamedRegs[] = {
    /* Timers (T0-T3) */
    {0x10000000, "T0_COUNT"}, {0x10000010, "T0_MODE"}, {0x10000020, "T0_COMP"}, {0x10000030, "T0_HOLD"},
    {0x10000800, "T1_COUNT"}, {0x10000810, "T1_MODE"}, {0x10000820, "T1_COMP"}, {0x10000830, "T1_HOLD"},
    {0x10001000, "T2_COUNT"}, {0x10001010, "T2_MODE"}, {0x10001020, "T2_COMP"},
    {0x10001800, "T3_COUNT"}, {0x10001810, "T3_MODE"}, {0x10001820, "T3_COMP"},
    /* IPU */
    {0x10002000, "IPU_CMD"}, {0x10002010, "IPU_CTRL"}, {0x10002020, "IPU_BP"}, {0x10002030, "IPU_TOP"},
    /* VIF0 / VIF1 */
    {0x10003800, "VIF0_STAT"}, {0x10003810, "VIF0_FBRST"}, {0x10003820, "VIF0_ERR"}, {0x10003830, "VIF0_MARK"},
    {0x10003C00, "VIF1_STAT"}, {0x10003C10, "VIF1_FBRST"}, {0x10003C20, "VIF1_ERR"}, {0x10003C30, "VIF1_MARK"},
    {0x10003C40, "VIF1_CYCLE"}, {0x10003C50, "VIF1_MODE"}, {0x10003C60, "VIF1_NUM"}, {0x10003C70, "VIF1_MASK"},
    {0x10003C80, "VIF1_CODE"}, {0x10003C90, "VIF1_ITOPS"}, {0x10003CA0, "VIF1_BASE"}, {0x10003CB0, "VIF1_OFST"},
    {0x10003CC0, "VIF1_TOPS"}, {0x10003CD0, "VIF1_ITOP"}, {0x10003CE0, "VIF1_TOP"}, {0x10003CF0, "VIF1_MSKPATH3"},
    /* GIF */
    {0x10003000, "GIF_CTRL"}, {0x10003010, "GIF_MODE"}, {0x10003020, "GIF_STAT"},
    {0x10003040, "GIF_TAG0"}, {0x10003050, "GIF_TAG1"}, {0x10003060, "GIF_TAG2"}, {0x10003070, "GIF_TAG3"},
    {0x10003080, "GIF_CNT"}, {0x10003090, "GIF_P3CNT"}, {0x100030A0, "GIF_P3TAG"},
    /* DMAC channels D0-D9 */
    {0x10008000, "D0_CHCR"}, {0x10008010, "D0_MADR"}, {0x10008020, "D0_QWC"}, {0x10008030, "D0_TADR"},
    {0x10009000, "D1_CHCR"}, {0x10009010, "D1_MADR"}, {0x10009020, "D1_QWC"}, {0x10009030, "D1_TADR"},
    {0x1000A000, "D2_CHCR"}, {0x1000A010, "D2_MADR"}, {0x1000A020, "D2_QWC"}, {0x1000A030, "D2_TADR"},
    {0x1000B000, "D3_CHCR"}, {0x1000B010, "D3_MADR"}, {0x1000B020, "D3_QWC"}, {0x1000B030, "D3_TADR"},
    {0x1000B400, "D4_CHCR"}, {0x1000B410, "D4_MADR"}, {0x1000B420, "D4_QWC"}, {0x1000B430, "D4_TADR"}, {0x1000B440, "D4_SADR"},
    {0x1000C000, "D5_CHCR"}, {0x1000C010, "D5_MADR"}, {0x1000C020, "D5_QWC"},
    {0x1000C400, "D6_CHCR"}, {0x1000C410, "D6_MADR"}, {0x1000C420, "D6_QWC"},
    {0x1000C800, "D7_CHCR"}, {0x1000C810, "D7_MADR"}, {0x1000C820, "D7_QWC"},
    {0x1000D000, "D8_CHCR"}, {0x1000D010, "D8_MADR"}, {0x1000D020, "D8_QWC"}, {0x1000D080, "D8_SADR"},
    {0x1000D400, "D9_CHCR"}, {0x1000D410, "D9_MADR"}, {0x1000D420, "D9_QWC"}, {0x1000D430, "D9_TADR"}, {0x1000D480, "D9_SADR"},
    /* DMAC global */
    {0x1000E000, "D_CTRL"}, {0x1000E010, "D_STAT"}, {0x1000E020, "D_PCR"},
    {0x1000E030, "D_SQWC"}, {0x1000E040, "D_RBSR"}, {0x1000E050, "D_RBOR"}, {0x1000E060, "D_STADR"},
    {0x1000E100, "D_ENABLEW"}, {0x1000E120, "D_ENABLER"},
    /* INTC */
    {0x1000F000, "I_STAT"}, {0x1000F010, "I_MASK"},
    /* SIF */
    {0x1000F200, "SIF_MSCOM"}, {0x1000F210, "SIF_SMCOM"}, {0x1000F220, "SIF_MSFLG"},
    {0x1000F230, "SIF_SMFLG"}, {0x1000F240, "SIF_CTRL"}, {0x1000F260, "SIF_BD6"},
    /* MCH (RDRAM config, used by early boot) */
    {0x1000F430, "MCH_RICM"}, {0x1000F440, "MCH_DRD"},
    /* GS privileged registers (0x12xxxxxx) */
    {0x12000000, "GS_PMODE"}, {0x12000010, "GS_SMODE1"}, {0x12000020, "GS_SMODE2"},
    {0x12000030, "GS_SRFSH"}, {0x12000040, "GS_SYNCH1"}, {0x12000050, "GS_SYNCH2"},
    {0x12000060, "GS_SYNCV"}, {0x12000070, "GS_DISPFB1"}, {0x12000080, "GS_DISPLAY1"},
    {0x12000090, "GS_DISPFB2"}, {0x120000A0, "GS_DISPLAY2"}, {0x120000B0, "GS_EXTBUF"},
    {0x120000C0, "GS_EXTDATA"}, {0x120000D0, "GS_EXTWRITE"}, {0x120000E0, "GS_BGCOLOR"},
    {0x12001000, "GS_CSR"}, {0x12001010, "GS_IMR"}, {0x12001040, "GS_BUSDIR"}, {0x12001080, "GS_SIGLBLID"},
};

const char* mmio_name(uint32_t addr) {
    for (const auto& r : kNamedRegs) {
        if (r.addr == addr) return r.name;
    }
    return nullptr;
}

struct AccessStat {
    uint64_t count = 0;
    uint64_t last_value = 0;
};

std::unordered_map<uint32_t, AccessStat> g_read_stats;
std::unordered_map<uint32_t, AccessStat> g_write_stats;

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

void log_access(const char* dir, uint32_t addr, int size_bits, uint64_t value, std::unordered_map<uint32_t, AccessStat>& stats) {
    AccessStat& st = stats[addr];
    ++st.count;
    st.last_value = value;
    if (!is_pow2(st.count)) return;

    const char* name = mmio_name(addr);
    if (name) {
        rt_log("mmio", "%s%-3d 0x%08x (%-14s) = 0x%llx  [access #%llu]",
            dir, size_bits, addr, name, (unsigned long long)value, (unsigned long long)st.count);
    } else {
        rt_log("mmio", "%s%-3d 0x%08x = 0x%llx  [access #%llu]",
            dir, size_bits, addr, (unsigned long long)value, (unsigned long long)st.count);
    }
}

/* Physical address for kseg0/kseg1 accesses to hardware registers. */
uint32_t hw_norm(uint32_t addr) {
    return addr >= 0x80000000u ? (addr & 0x1FFFFFFFu) : addr;
}

/* P2/P3 hardware model dispatch (hw/dmac.cpp, hw/vif1.cpp, hw/gif.cpp,
 * hw/gspriv.cpp, ee/intc.cpp, ee/timers.cpp, sif/sif.cpp). Returns true
 * when a module owns the register; unmatched addresses fall back to the P1
 * log-and-return-0 behavior. */
bool hw_read(uint32_t addr, uint64_t* out) {
    addr = hw_norm(addr);
    uint32_t v32;
    if (rt_dmac_mmio_read(addr, &v32)) { *out = v32; return true; }
    if (rt_vif_mmio_read(addr, &v32)) { *out = v32; return true; }
    if (rt_gif_mmio_read(addr, &v32)) { *out = v32; return true; }
    if (rt_timers_mmio_read(addr, &v32)) { *out = v32; return true; }
    if (rt_intc_mmio_read(addr, &v32)) { *out = v32; return true; }
    if (rt_sif_mmio_read(addr, &v32)) { *out = v32; return true; }
    if (rt_gspriv_mmio_read(addr, out)) return true;
    return false;
}

bool hw_write(uint32_t addr, uint64_t v) {
    addr = hw_norm(addr);
    if (rt_dmac_mmio_write(addr, (uint32_t)v)) return true;
    if (rt_vif_mmio_write(addr, (uint32_t)v)) return true;
    if (rt_gif_mmio_write(addr, (uint32_t)v)) return true;
    if (rt_timers_mmio_write(addr, (uint32_t)v)) return true;
    if (rt_intc_mmio_write(addr, (uint32_t)v)) return true;
    if (rt_sif_mmio_write(addr, (uint32_t)v)) return true;
    if (rt_gspriv_mmio_write(addr, v)) return true;
    return false;
}

uint64_t mmio_read_common(uint32_t addr, int bits) {
    /* Ordering matters for guest poll loops: advance the virtual clock and
     * raise due status bits FIRST, then sample the register, then deliver
     * pending interrupts. This models the real interrupt latency window in
     * which a polling load can observe a freshly raised I_STAT/D_STAT bit
     * before the kernel dispatcher acks it. The retail vsync wait (clear
     * VB_ON, spin on I_STAT bit 2) depends on that window; delivering
     * before the sample starves it forever. */
    rt_clock_tick(512);
    uint64_t v = 0;
    hw_read(addr, &v); /* v stays 0 for unmodeled registers */
    log_access("read", addr, bits, v, g_read_stats);
    rt_intc_deliver();
    return v;
}

void mmio_write_common(uint32_t addr, int bits, uint64_t v) {
    hw_write(addr, v);
    log_access("write", addr, bits, v, g_write_stats);
    /* CHCR-triggered DMA completions raised their D_STAT bits inside
     * hw_write; delivery happens here, after the store completed, via the
     * standard deferred path. */
    rt_kernel_mmio_tick();
}

} // namespace

uint8_t rt_mmio_read8(uint32_t addr) {
    return (uint8_t)mmio_read_common(addr, 8);
}
uint16_t rt_mmio_read16(uint32_t addr) {
    return (uint16_t)mmio_read_common(addr, 16);
}
uint32_t rt_mmio_read32(uint32_t addr) {
    return (uint32_t)mmio_read_common(addr, 32);
}
uint64_t rt_mmio_read64(uint32_t addr) {
    return mmio_read_common(addr, 64);
}
rc_u128 rt_mmio_read128(uint32_t addr) {
    rc_u128 v{};
    v.u64x[0] = mmio_read_common(addr, 128);
    return v;
}

void rt_mmio_write8(uint32_t addr, uint8_t v) {
    mmio_write_common(addr, 8, v);
}
void rt_mmio_write16(uint32_t addr, uint16_t v) {
    mmio_write_common(addr, 16, v);
}
void rt_mmio_write32(uint32_t addr, uint32_t v) {
    mmio_write_common(addr, 32, v);
}
void rt_mmio_write64(uint32_t addr, uint64_t v) {
    mmio_write_common(addr, 64, v);
}
void rt_mmio_write128(uint32_t addr, rc_u128 v) {
    /* FIFO windows (VIF0/VIF1/GIF) consume the full quadword; everything
     * else keeps the P1 behavior (low 64 bits, registers are at most 64
     * bits wide). */
    uint32_t naddr = hw_norm(addr);
    if (rt_hw_fifo_write128(naddr, &v)) {
        log_access("write", naddr, 128, v.u64x[0], g_write_stats);
        rt_kernel_mmio_tick();
        return;
    }
    mmio_write_common(addr, 128, v.u64x[0]);
}
