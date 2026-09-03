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

#include <cstdio>

namespace {

/* 1, 2, 4, 8, ... : the log flood control this file shares with mmio.cpp. */
bool is_pow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

struct Timer {
    uint32_t mode = 0;
    uint32_t comp = 0;
    uint32_t hold = 0;
    uint64_t base_vclk = 0;   /* vclk when COUNT was last written / reset */
    uint32_t base_count = 0;
    bool equf = false, ovff = false;
    uint64_t last_check = 0;  /* vclk of the last flag evaluation */

    /* MODE-write logging (rt_timers_mmio_write, reg 0x10): the game rewrites
     * MODE with the same bits and the same COMP every field on some timers,
     * which used to log an identical line each time. logged tracks whether
     * logged_mode/logged_comp hold a real value yet; identical_rewrites
     * counts MODE writes suppressed since the last logged one, so the next
     * real change can say how many were skipped. A timer whose MODE never
     * changes again would otherwise never report that count at all, so the
     * count is also folded into a line on its 1st, 2nd, 4th, 8th, ...
     * suppression, the same flood control mmio.cpp uses. */
    bool logged = false;
    uint32_t logged_mode = 0;
    uint32_t logged_comp = 0;
    uint32_t identical_rewrites = 0;
};

Timer g_t[4];

/* Bus cycles per tick, indexed by Tn_MODE.CLKS. */
constexpr uint64_t kPrescale[4] = {1, 16, 256, RT_CYCLES_PER_HBLANK};

/* The same three power-of-two divisors as shifts, for raw_ticks. The
 * static_asserts are the tie: change a prescale without its shift and the
 * build stops. */
constexpr unsigned kPrescaleShift[3] = {0, 4, 8};
static_assert(kPrescale[0] == 1ull << kPrescaleShift[0], "CLKS 0 prescale and shift disagree");
static_assert(kPrescale[1] == 1ull << kPrescaleShift[1], "CLKS 1 prescale and shift disagree");
static_assert(kPrescale[2] == 1ull << kPrescaleShift[2], "CLKS 2 prescale and shift disagree");

uint64_t prescale(const Timer& t) { return kPrescale[t.mode & 3]; }

bool counting(const Timer& t) { return (t.mode & 0x80) != 0; } /* CUE */

/* Ticks elapsed since base (not wrapped).
 *
 * Three of the four prescales are powers of two, so those divide by a
 * shift. This is on the hot path by way of rt_timers_next_event, which
 * clock_next_event calls on every MMIO access: the movie makes about 33000
 * of those per field, and a 64-bit divide each was paying for nothing. Only
 * the H-blank prescale needs the real division. The result is still
 * elapsed / prescale(t) for every CLKS value; kPrescaleShift above is
 * asserted against kPrescale so the two cannot drift apart. */
uint64_t raw_ticks(const Timer& t) {
    if (!counting(t)) return 0;
    const uint64_t elapsed = rt_clock_now() - t.base_vclk;
    const unsigned clks = t.mode & 3;
    if (clks == 3) return elapsed / kPrescale[3];
    return elapsed >> kPrescaleShift[clks];
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

namespace {

/* Next compare/overflow interrupt across enabled timers, absolute vclk.
 *
 * Memoized, because clock_next_event calls this on every MMIO access and
 * the answer only moves when the timer state does. Every "when" below is an
 * absolute cycle computed from base_vclk, base_count, comp and mode, none
 * of which change as time passes, so the answer is good until something
 * writes a timer register or rt_timers_run_due retires an event. Those are
 * the only two places in this file that touch g_t, and both drop the cache;
 * nothing outside this file can reach that array, so the invalidation is
 * complete by construction rather than by convention. */
uint64_t g_next_event_cache = 0;
bool g_next_event_valid = false;

uint64_t timers_next_event_slow() {
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

} // namespace

uint64_t rt_timers_next_event() {
    if (!g_next_event_valid) {
        g_next_event_cache = timers_next_event_slow();
        g_next_event_valid = true;
    }
    return g_next_event_cache;
}

void rt_timers_run_due() {
    /* Unconditional: a compare that fired without resetting the count still
     * has to move the cached answer on to the next period, or the tick loop
     * would keep finding the same event due. */
    g_next_event_valid = false;
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
    g_next_event_valid = false; /* the only other writer of g_t; see the cache */
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
            /* Some timers get MODE rewritten with the same bits and the
             * same COMP every field; log only a real change, a power-of-two
             * suppression count, or every write under the "timer" verbose
             * channel. See the fields on Timer above. */
            {
                const bool changed = !t.logged || t.mode != t.logged_mode || t.comp != t.logged_comp;
                if (!changed) ++t.identical_rewrites;
                const bool milestone = !changed && is_pow2(t.identical_rewrites);
                if (changed || milestone || rt_verbose("timer")) {
                    char tail[64] = "";
                    if (changed && t.identical_rewrites > 0) {
                        std::snprintf(tail, sizeof(tail), " after %u identical rewrites",
                            t.identical_rewrites);
                    } else if (!changed) {
                        std::snprintf(tail, sizeof(tail), " [identical rewrite #%u]",
                            t.identical_rewrites);
                    }
                    rt_log("timer", "T%d_MODE = 0x%03x (clks=%u cue=%d cmpe=%d ovfe=%d zret=%d comp=0x%04x)%s",
                        i, t.mode, t.mode & 3, !!(t.mode & 0x80), !!(t.mode & 0x100), !!(t.mode & 0x200),
                        !!(t.mode & 0x40), t.comp, tail);
                    if (changed) {
                        t.logged = true;
                        t.logged_mode = t.mode;
                        t.logged_comp = t.comp;
                        t.identical_rewrites = 0;
                    }
                }
            }
            return true;
        case 0x20: t.comp = v & 0xFFFF; return true;
        case 0x30: t.hold = v & 0xFFFF; return true;
        default: return false;
    }
}
