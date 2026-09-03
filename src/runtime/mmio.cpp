/* mmio.cpp: MMIO trap handlers for guest addresses whose page is NULL in
 * g_pages (0x10xxxxxx device registers, 0x12xxxxxx GS privileged registers,
 * and every other hole in the EE physical map: kseg2/kseg3, the gap above
 * 32 MB of RAM, the IOP RAM and EE ROM windows, and everything else no page
 * is mapped for).
 *
 * The log-everything, read-returns-0 policy described below now holds only
 * inside the two hardware windows this title's binary actually touches:
 * 0x10000000-0x1000FFFF (EE bus registers: DMAC channel slots, VIF0/VIF1
 * FIFO windows, GIF, timers, INTC, SIF) and 0x12000000-0x12001FFF (GS
 * privileged registers). An access outside those windows is a wild guest
 * pointer, not an unmodeled register, and CLAUDE.md's "unmapped MMIO is
 * fatal with a state dump" applies: unmapped_fatal below stops the run with
 * a register and scheduler dump instead of returning 0 and letting the
 * guest spin forever on a value that will never change. A misaligned
 * 16/32/64-bit access inside the two modeled windows is likewise fatal
 * (unaligned_fatal): lh/lw/ld and sh/sw/sd raise AdEL/AdES for that on the
 * EE, they do not silently truncate. Two qualifications, both recorded
 * here rather than guessed at:
 *   - lq/sq do not fault. They force 16-byte alignment by masking the
 *     effective address (recomp_ops.h's rc_read128/rc_write128 do the same
 *     for RAM), so rt_mmio_read128/rt_mmio_write128 mask before dispatch
 *     and 128-bit accesses are never checked here.
 *   - lwl/lwr/ldl/ldr and swl/swr/sdl/sdr are the R5900's unaligned-access
 *     instructions and do not fault either, but recomp_ops.h implements
 *     them by handing the unaligned address to rc_read32/rc_read64 and
 *     letting those mask it, so on a NULL page they arrive here looking
 *     exactly like a misaligned lw. No such access to a hardware register
 *     is in the measured census of this binary, so the fatal stands; if
 *     one ever fires with an lwl-family return address, this is why.
 *
 * Within the two modeled windows, the P1 policy stands:
 *
 * P1 policy: every access is logged (addr, size, value, running count per
 * address) and every read returns 0 unless a P2/P3 hardware model (see
 * hw_read/hw_write below) claims the register. Named-register lookup exists
 * purely to make logs legible; addresses are public PS2 hardware register
 * addresses (DMAC/GS/VIF/timer/INTC/SIF, as documented by e.g.
 * psdevwiki/ps2tek), not anything derived from the ICO decomp or ROM.
 *
 * Flood control: an access to the same address logs on its 1st, 2nd, 4th,
 * 8th, ... (power-of-two) occurrence, then the running count is folded into
 * every subsequent power-of-two log line so nothing is silently dropped from
 * view, but a tight poll loop does not spam the log once per instruction.
 *
 * Unaligned loads/stores that land on a mapped RAM page never reach this
 * file: recomp_ops.h's rc_read../rc_write.. mask the offset to the access
 * size before touching guest memory, because that header is the ABI shared
 * by generated code and the interpreter. A hardware-accurate AdEL/AdES
 * check for those belongs there, gated behind a measurement of whether the
 * game ever mis-aligns a RAM access (not done here: see RC_CHECK_ALIGN in
 * a future change), not in this file, which only ever sees NULL-page
 * addresses.
 */
#include "runtime.h"

#include "ee/kernel.h"
#include "hw/hw.h"
#include "prof.h"

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
    {0x1000F590, "D_ENABLEW"}, {0x1000F520, "D_ENABLER"},
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

/* Direct-mapped front for the two maps above.
 *
 * The counter itself is the point of the map, so it cannot be skipped, but
 * the hash probe on the way to it can: the movie touches five registers in
 * rotation and makes about 33000 accesses a field, and the probe measured
 * 7.1 ns each (hw/ipu_selftest.cpp's register access benchmark). Hardware
 * registers are 16-byte aligned, so the low bits of addr >> 4 spread them
 * without collisions in practice; a collision just costs the map lookup it
 * would have cost anyway. std::unordered_map is node based, so a stat's
 * address is stable once inserted and safe to hold here. */
constexpr size_t kStatCacheSize = 128; /* power of two */
struct StatCache {
    uint32_t addr[kStatCacheSize];
    AccessStat* st[kStatCacheSize];
    bool valid[kStatCacheSize];
};
StatCache g_read_cache{}, g_write_cache{};

AccessStat& stat_for(uint32_t addr, std::unordered_map<uint32_t, AccessStat>& stats, StatCache& cache) {
    const size_t i = (addr >> 4) & (kStatCacheSize - 1);
    if (cache.valid[i] && cache.addr[i] == addr) return *cache.st[i];
    AccessStat& st = stats[addr];
    cache.addr[i] = addr;
    cache.st[i] = &st;
    cache.valid[i] = true;
    return st;
}

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

void log_access(const char* dir, uint32_t addr, int size_bits, uint64_t value,
                std::unordered_map<uint32_t, AccessStat>& stats, StatCache& cache) {
    AccessStat& st = stat_for(addr, stats, cache);
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

/* The IPU register block (IPU_CMD/CTRL/BP/TOP) and its two FIFO windows.
 *
 * Used for two things, both about the cost of an access rather than its
 * meaning. It picks the profile bucket, so an IPU access opens one zone
 * here instead of one here and a second inside hw/ipu.cpp: the movie makes
 * about 33000 register accesses per field and each zone costs two clock
 * reads, which measured 64 ns per access for the nested pair against 19 ns
 * for the IPU handler's own work (hw/ipu_selftest.cpp's register access
 * benchmark). And it lets hw_read/hw_write try the IPU first instead of
 * walking the DMAC, VIF, GIF, timer, INTC and SIF handlers to reach it. */
bool is_ipu_addr(uint32_t norm) {
    return (norm & ~0x3Fu) == 0x10002000u || (norm & ~0x1Fu) == 0x10007000u;
}

/* P2/P3 hardware model dispatch (hw/dmac.cpp, hw/vif1.cpp, hw/gif.cpp,
 * hw/gspriv.cpp, ee/intc.cpp, ee/timers.cpp, sif/sif.cpp). Returns true
 * when a module owns the register; unmatched addresses fall back to the P1
 * log-and-return-0 behavior.
 *
 * `norm` is the physical address (hw_norm applied) and `ipu` is
 * is_ipu_addr(norm), both computed once by the caller, which needs the
 * flag anyway to pick the profile bucket. */
bool hw_read(uint32_t norm, bool ipu, uint64_t* out) {
    if (ipu) return rt_ipu_mmio_read(norm, out); /* IPU regs are 64-bit reads */
    uint32_t v32;
    if (rt_dmac_mmio_read(norm, &v32)) { *out = v32; return true; }
    if (rt_vif_mmio_read(norm, &v32)) { *out = v32; return true; }
    if (rt_gif_mmio_read(norm, &v32)) { *out = v32; return true; }
    if (rt_timers_mmio_read(norm, &v32)) { *out = v32; return true; }
    if (rt_intc_mmio_read(norm, &v32)) { *out = v32; return true; }
    if (rt_sif_mmio_read(norm, &v32)) { *out = v32; return true; }
    if (rt_gspriv_mmio_read(norm, out)) return true;
    return false;
}

bool hw_write(uint32_t norm, bool ipu, uint64_t v) {
    if (ipu) return rt_ipu_mmio_write(norm, v);
    if (rt_dmac_mmio_write(norm, (uint32_t)v)) return true;
    if (rt_vif_mmio_write(norm, (uint32_t)v)) return true;
    if (rt_gif_mmio_write(norm, (uint32_t)v)) return true;
    if (rt_timers_mmio_write(norm, (uint32_t)v)) return true;
    if (rt_intc_mmio_write(norm, (uint32_t)v)) return true;
    if (rt_sif_mmio_write(norm, (uint32_t)v)) return true;
    if (rt_gspriv_mmio_write(norm, v)) return true;
    return false;
}

/* Which of the two hardware windows (if either) this title's binary
 * touches. Measured basis: every unnamed MMIO access seen in a healthy run
 * (before the wild-pointer read that motivated this file) falls inside
 * 0x1000xxxx (DMAC channel slots, VIF0/VIF1 FIFO windows at
 * 0x10004000/0x10005000, GIF, timers, INTC, SIF) or 0x1200xxxx (GS
 * privileged registers). Nothing else on the EE bus is ever addressed by
 * this game. */
enum class MmioClass { EeRegs, GsPriv, Wild };

MmioClass mmio_class(uint32_t addr, uint32_t norm) {
    /* hw_norm's mask is the kseg0/kseg1 physical translation and does not
     * apply above them: kseg2/kseg3 are TLB mapped and this title maps
     * nothing there (mem.cpp maps RAM aliases at the 0x8 and 0xA prefixes
     * only). An address there whose masked form happens to land in a
     * window is still a wild pointer, so classify it before the mask. */
    if (addr >= 0xC0000000u) return MmioClass::Wild;
    if (norm >= 0x10000000u && norm < 0x10010000u) return MmioClass::EeRegs;
    if (norm >= 0x12000000u && norm < 0x12002000u) return MmioClass::GsPriv;
    return MmioClass::Wild;
}

/* An access this file must fault on. 128-bit is absent by design: lq/sq
 * mask the low 4 bits of the effective address instead of raising AdEL or
 * AdES, and rt_mmio_read128/rt_mmio_write128 have already applied that
 * mask, so a 128-bit access is aligned by the time it gets here. */
bool misaligned(uint32_t addr, int bits) {
    return bits > 8 && bits < 128 && (addr & (uint32_t)(bits / 8 - 1)) != 0;
}

/* Names the hole a wild access landed in, for the fatal log line. addr is
 * the raw guest address (pre hw_norm) so the kseg2/kseg3 check sees the
 * segment bits hw_norm's masking would otherwise erase; norm is the
 * physical address for everything else. */
const char* mmio_region_name(uint32_t addr, uint32_t norm) {
    if (addr >= 0xC0000000u) return "kseg2/kseg3 (TLB mapped, nothing mapped there by this title)";
    if (norm < 0x10000000u) return "beyond the 32 MB of EE RAM";
    if (norm >= 0x1C000000u && norm < 0x1C200000u) return "IOP RAM through the EE window (not modeled)";
    if (norm >= 0x1E000000u && norm < 0x20000000u) return "EE ROM/IOP register window (not modeled)";
    return "nothing on the EE bus";
}

/* A wild guest access: not inside either hardware window. Returning 0 and
 * carrying on, the P1 behavior this replaces, let the guest read garbage
 * off the bus and loop forever on a value that would never change on real
 * hardware either (the addresses seen doing this are not backed by
 * anything). CLAUDE.md: unmapped MMIO is a fatal with a state dump. */
[[noreturn]] void unmapped_fatal(const char* dir, uint32_t addr, uint32_t norm, int bits, uint64_t value) {
    rt_log_drain();
    R5900Context* ctx = rt_fault_ctx();
    uint32_t ra = ctx ? (uint32_t)ctx->r[31].u64x[0] : 0;
    const char* suffix = misaligned(addr, bits)
        ? (dir[0] == 'r' ? ", unaligned (AdEL on the EE)" : ", unaligned (AdES on the EE)")
        : "";
    rt_log("mmio", "FATAL: unmapped guest %s%d at 0x%08x (physical 0x%08x, %s)%s value=0x%llx thread=%d ra=0x%08x",
        dir, bits, addr, norm, mmio_region_name(addr, norm), suffix, (unsigned long long)value,
        rt_thread_current_id(), ra);
    rt_dump_registers(ctx);
    rt_sched_dump_inventory("unmapped guest access");
    rt_fatal("mmio", nullptr, "unmapped guest %s%d at 0x%08x; the state dump is above", dir, bits, addr);
}

/* A misaligned 16/32/64-bit access that DOES land inside a modeled window
 * (see misaligned() for why 128-bit is not one of these). lh/lw/ld and
 * sh/sw/sd raise AdEL/AdES for this on hardware; recomp_ops.h masks the
 * offset instead on the RAM path, a divergence the file header records,
 * but nothing masks it here. */
[[noreturn]] void unaligned_fatal(const char* dir, uint32_t addr, uint32_t norm, int bits, uint64_t value) {
    rt_log_drain();
    R5900Context* ctx = rt_fault_ctx();
    uint32_t ra = ctx ? (uint32_t)ctx->r[31].u64x[0] : 0;
    rt_log("mmio", "FATAL: unaligned guest %s%d of hardware register 0x%08x (physical 0x%08x) value=0x%llx "
        "thread=%d ra=0x%08x", dir, bits, addr, norm, (unsigned long long)value, rt_thread_current_id(), ra);
    rt_dump_registers(ctx);
    rt_sched_dump_inventory("unaligned guest access");
    rt_fatal("mmio", nullptr, "unaligned guest %s%d of hardware register 0x%08x: the EE raises AdEL/AdES here",
        dir, bits, addr);
}

uint64_t mmio_read_common(uint32_t addr, int bits) {
    const uint32_t norm = hw_norm(addr);
    if (mmio_class(addr, norm) == MmioClass::Wild) unmapped_fatal("read", addr, norm, bits, 0);
    if (misaligned(addr, bits)) unaligned_fatal("read", addr, norm, bits, 0);
    const bool ipu = is_ipu_addr(norm);
    RT_PROF_ZONE(ipu ? RT_PROF_IPU : RT_PROF_MMIO);
    /* Ordering matters for guest poll loops: advance the virtual clock and
     * raise due status bits FIRST, then sample the register, then deliver
     * pending interrupts. This models the real interrupt latency window in
     * which a polling load can observe a freshly raised I_STAT/D_STAT bit
     * before the kernel dispatcher acks it. The retail vsync wait (clear
     * VB_ON, spin on I_STAT bit 2) depends on that window; delivering
     * before the sample starves it forever. */
    rt_kernel_mmio_bill();
    uint64_t v = 0;
    hw_read(norm, ipu, &v); /* v stays 0 for unmodeled registers */
    log_access("read", addr, bits, v, g_read_stats, g_read_cache);
    rt_intc_deliver();
    return v;
}

void mmio_write_common(uint32_t addr, int bits, uint64_t v) {
    const uint32_t norm = hw_norm(addr);
    if (mmio_class(addr, norm) == MmioClass::Wild) unmapped_fatal("write", addr, norm, bits, v);
    if (misaligned(addr, bits)) unaligned_fatal("write", addr, norm, bits, v);
    const bool ipu = is_ipu_addr(norm);
    RT_PROF_ZONE(ipu ? RT_PROF_IPU : RT_PROF_MMIO);
    hw_write(norm, ipu, v);
    log_access("write", addr, bits, v, g_write_stats, g_write_cache);
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
    /* lq forces 16-byte alignment by masking, exactly as recomp_ops.h's
     * rc_read128 does on the RAM path; it does not raise AdEL. */
    v.u64x[0] = mmio_read_common(addr & ~0xFu, 128);
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
    /* sq forces 16-byte alignment by masking, exactly as recomp_ops.h's
     * rc_write128 does on the RAM path; it does not raise AdES. */
    addr &= ~0xFu;
    uint32_t naddr = hw_norm(addr);
    /* The FIFO windows (0x10004000/0x10005000/0x10006000) are inside
     * EeRegs, so a wild 128-bit store never matches rt_hw_fifo_write128
     * and falls through to mmio_write_common below, where mmio_class
     * catches it. */
    {
        RT_PROF_ZONE(is_ipu_addr(naddr) ? RT_PROF_IPU : RT_PROF_MMIO);
        if (rt_hw_fifo_write128(naddr, &v)) {
            log_access("write", naddr, 128, v.u64x[0], g_write_stats, g_write_cache);
            rt_kernel_mmio_tick();
            return;
        }
    }
    mmio_write_common(addr, 128, v.u64x[0]);
}
