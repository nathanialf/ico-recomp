/* ee/timers.cpp: EE timers T0-T3 (0x10000000/0x10000800/0x10001000/
 * 0x10001800) derived from the virtual clock.
 *
 * Register model (public hardware facts, ps2tek EE timers):
 *   Tn_COUNT: 16-bit up-counter. Tn_MODE bits: 0-1 CLKS (0=BUSCLK,
 *   1=BUSCLK/16, 2=BUSCLK/256, 3=HBLNK), 6 ZRET (reset count on compare),
 *   7 CUE (count-up enable), 8 CMPE (compare interrupt enable), 9 OVFE
 *   (overflow interrupt enable), 10 EQUF (equal flag, write 1 to clear),
 *   11 OVFF (overflow flag, write 1 to clear). Tn_COMP: 16-bit compare.
 *   Timer n raises INTC cause 9+n.
 *
 * HBLNK is modeled as the NTSC line rate derived from the field timeline
 * (262.5 lines per 59.94 Hz field).
 */
#include "kernel.h"

namespace {

constexpr uint64_t kHblankCycles = RT_CYCLES_PER_FIELD * 2 / 525; /* per line */

struct Timer {
    uint32_t mode = 0;
    uint32_t comp = 0;
    uint32_t hold = 0;
    uint64_t base_vclk = 0;   /* vclk when COUNT was last written / reset */
    uint32_t base_count = 0;
    bool equf = false, ovff = false;
    uint64_t last_check = 0;  /* vclk of the last flag evaluation */
};

Timer g_t[4];

uint64_t prescale(const Timer& t) {
    switch (t.mode & 3) {
        case 0: return 1;
        case 1: return 16;
        case 2: return 256;
        default: return kHblankCycles;
    }
}

bool counting(const Timer& t) { return (t.mode & 0x80) != 0; } /* CUE */

/* Ticks elapsed since base (not wrapped). */
uint64_t raw_ticks(const Timer& t) {
    if (!counting(t)) return 0;
    return (rt_clock_now() - t.base_vclk) / prescale(t);
}

uint32_t count_now(const Timer& t) {
    return (uint32_t)((t.base_count + raw_ticks(t)) & 0xFFFF);
}

int timer_index(uint32_t addr, uint32_t* reg) {
    /* T0 0x100000x0, T1 0x100008x0, T2 0x100010x0, T3 0x100018x0 */
    if (addr < 0x10000000 || addr > 0x10001830) return -1;
    uint32_t off = addr - 0x10000000;
    int idx = (int)(off >> 11);
    *reg = off & 0x7FF;
    return idx;
}

} // namespace

void rt_timers_init() {}

/* Next compare/overflow interrupt across enabled timers, absolute vclk. */
uint64_t rt_timers_next_event() {
    uint64_t best = UINT64_MAX;
    for (const Timer& t : g_t) {
        if (!counting(t)) continue;
        if (!(t.mode & 0x300)) continue; /* neither CMPE nor OVFE */
        uint64_t ps = prescale(t);
        uint64_t ticks = raw_ticks(t);
        uint32_t cnt = (uint32_t)((t.base_count + ticks) & 0xFFFF);
        if (t.mode & 0x100) { /* CMPE */
            uint64_t dt = ((t.comp - cnt - 1) & 0xFFFF) + 1; /* ticks until compare */
            uint64_t when = t.base_vclk + (ticks + dt) * ps;
            if (when < best) best = when;
        }
        if (t.mode & 0x200) { /* OVFE */
            uint64_t dt = 0x10000 - (cnt & 0xFFFF);
            uint64_t when = t.base_vclk + (ticks + dt) * ps;
            if (when < best) best = when;
        }
    }
    return best;
}

void rt_timers_run_due() {
    for (int i = 0; i < 4; ++i) {
        Timer& t = g_t[i];
        if (!counting(t)) { t.last_check = rt_clock_now(); continue; }
        uint64_t ps = prescale(t);
        uint64_t prev_ticks = t.last_check > t.base_vclk ? (t.last_check - t.base_vclk) / ps : 0;
        uint64_t cur_ticks = raw_ticks(t);
        t.last_check = rt_clock_now();
        if (cur_ticks == prev_ticks) continue;
        uint64_t span = cur_ticks - prev_ticks;
        uint32_t prev_cnt = (uint32_t)((t.base_count + prev_ticks) & 0xFFFF);
        /* Compare hit within (prev, cur]? */
        uint64_t to_comp = ((t.comp - prev_cnt - 1) & 0xFFFF) + 1;
        if (to_comp <= span) {
            if (t.mode & 0x100) { /* CMPE */
                t.equf = true;
                rt_intc_raise(RT_INTC_TIMER0 + i);
            }
            if (t.mode & 0x40) { /* ZRET: count resets on compare */
                t.base_vclk = t.last_check - (span - to_comp) * ps;
                t.base_count = 0;
            }
        }
        uint64_t to_ovf = 0x10000 - prev_cnt;
        if ((t.mode & 0x200) && to_ovf <= span) { /* OVFE */
            t.ovff = true;
            rt_intc_raise(RT_INTC_TIMER0 + i);
        }
    }
}

bool rt_timers_mmio_read(uint32_t addr, uint32_t* out) {
    uint32_t reg;
    int i = timer_index(addr, &reg);
    if (i < 0) return false;
    Timer& t = g_t[i];
    switch (reg) {
        case 0x00: *out = count_now(t); return true;
        case 0x10:
            *out = (t.mode & 0x3FF) | (t.equf ? 0x400 : 0) | (t.ovff ? 0x800 : 0);
            return true;
        case 0x20: *out = t.comp; return true;
        case 0x30: *out = t.hold; return true;
        default: return false;
    }
}

bool rt_timers_mmio_write(uint32_t addr, uint32_t v) {
    uint32_t reg;
    int i = timer_index(addr, &reg);
    if (i < 0) return false;
    Timer& t = g_t[i];
    switch (reg) {
        case 0x00:
            t.base_count = v & 0xFFFF;
            t.base_vclk = rt_clock_now();
            return true;
        case 0x10:
            /* EQUF/OVFF are write-1-clear (bits 10/11). */
            if (v & 0x400) t.equf = false;
            if (v & 0x800) t.ovff = false;
            t.mode = v & 0x3FF;
            /* Writing MODE restarts the count base per hardware behavior of
             * enabling CUE; keep it simple and rebase. */
            t.base_count = count_now(t);
            t.base_vclk = rt_clock_now();
            t.last_check = t.base_vclk;
            rt_log("timer", "T%d_MODE = 0x%03x (clks=%u cue=%d cmpe=%d ovfe=%d zret=%d comp=0x%04x)",
                i, t.mode, t.mode & 3, !!(t.mode & 0x80), !!(t.mode & 0x100), !!(t.mode & 0x200),
                !!(t.mode & 0x40), t.comp);
            return true;
        case 0x20: t.comp = v & 0xFFFF; return true;
        case 0x30: t.hold = v & 0xFFFF; return true;
        default: return false;
    }
}
