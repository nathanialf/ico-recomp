/* ee/intc.cpp: INTC and DMAC interrupt handler registries and delivery,
 * plus the GS CSR/IMR shadow.
 *
 * Delivery model (per the P2 plan and CLAUDE.md's HLE boundary): timeline
 * events raise I_STAT/D_STAT bits synchronously with the virtual clock, but
 * guest handlers only run from rt_intc_deliver(), which is called at
 * instruction boundaries (MMIO trap, syscall end, scheduler idle loop, EI).
 * Handlers run on a dedicated interrupt context with a dedicated guest stack
 * in kernel-reserved low RAM. After the outermost delivery pass the
 * scheduler preempts the interrupted thread if a handler readied a
 * higher-priority thread.
 *
 * Register behavior notes (public hardware facts, ps2tek EE INTC/DMAC):
 *   I_STAT (0x1000F000): write 1 to clear a bit. I_MASK (0x1000F010):
 *   write 1 to toggle a bit. D_STAT (0x1000E010): low bits write-1-clear
 *   channel interrupt status, bits 16+ write-1-toggle the channel interrupt
 *   mask. GS CSR (0x12001000): bits 0-4 are event flags (SIGNAL, FINISH,
 *   HSINT, VSINT, EDWINT), write 1 to clear; bit 13 is FIELD; bits 16-23
 *   revision, 24-31 id.
 */
#include "kernel.h"

#include "../hw/hw.h"
#include "../prof.h"

#include <cinttypes>
#include <cstdio>

namespace {

constexpr int kNumCauses = 15;
constexpr int kNumChannels = 10;
constexpr int kMaxHandlersPerLine = 8;
/* Guest stack for interrupt handlers: top of a 64 KB region in the
 * kernel-reserved first megabyte (the ELF loads at 0x100000). */
constexpr uint32_t kIntStackTop = 0x000E0000u;

struct Handler {
    bool used = false;
    uint32_t vram = 0;
    uint32_t gp = 0;   /* $gp at registration; the kernel restores it for the call */
    int next = 0;
};

struct Line {
    Handler h[kMaxHandlersPerLine];
    uint64_t dispatches = 0;
};

bool g_eie = true;          /* EE Status.EIE; ei/di instructions toggle it */
bool g_in_interrupt = false;
uint32_t g_istat = 0, g_imask = 0;
uint32_t g_dstat = 0, g_dmask = 0;
Line g_intc[kNumCauses];
Line g_dmac[kNumChannels];
R5900Context g_int_ctx;      /* dedicated interrupt-handler context */

const char* cause_name(int c) {
    static const char* names[kNumCauses] = {
        "GS", "SBUS", "VB_ON", "VB_OFF", "VIF0", "VIF1", "VU0", "VU1",
        "IPU", "TIMER0", "TIMER1", "TIMER2", "TIMER3", "SFIFO", "VU0WD"
    };
    return (c >= 0 && c < kNumCauses) ? names[c] : "?";
}

/* GS shadow. CSR: FINISH (bit 1) held set because the P2 runtime has no GS
 * backend and every submitted drawing "finishes" instantly; VSINT (bit 3)
 * sticky until acked; FIELD (bit 13) tracks the vblank timeline; REV/ID
 * bytes report a plausible retail GS (0x1B/0x55). */
uint32_t g_csr_flags = 0;    /* bits 0-4 sticky event flags */
unsigned g_gs_field = 0;
uint64_t g_gs_imr = 0x7F00;  /* all GS interrupt sources masked, per GS reset */

uint64_t csr_value() {
    return (uint64_t)(g_csr_flags | 0x2u /* FINISH */)
        | ((uint64_t)(g_gs_field & 1) << 13)
        | (0x1Bull << 16) | (0x55ull << 24);
}

uint32_t run_guest_handler(const Handler& h, uint32_t a0) {
    std::memset(&g_int_ctx, 0, sizeof(g_int_ctx));
    g_int_ctx.r[4].u64x[0] = a0;
    g_int_ctx.r[28].u64x[0] = h.gp;
    g_int_ctx.r[29].u64x[0] = kIntStackTop;
    g_int_ctx.r[31].u64x[0] = RT_CLEAN_EXIT_VRAM;
    if (h.vram < RECOMP_TEXT_BASE || h.vram >= RECOMP_TEXT_LIMIT || !g_functab[RECOMP_FUNC_IDX(h.vram)]) {
        rt_fatal("intc", nullptr, "interrupt handler vram 0x%08x has no translation", h.vram);
    }
    g_functab[RECOMP_FUNC_IDX(h.vram)](&g_int_ctx);
    return (uint32_t)g_int_ctx.r[2].u64x[0];
}

void dispatch_line(Line& line, int number, uint32_t a0, const char* kind, const char* name) {
    /* Guest handler code runs inside this zone, so "intc" measures time in
     * interrupt handlers, not dispatcher bookkeeping. The call count is the
     * number of lines dispatched. */
    RT_PROF_ZONE(RT_PROF_INTC);
    ++line.dispatches;
    bool any = false;
    for (const Handler& h : line.h) {
        if (!h.used) continue;
        any = true;
        uint32_t ret = run_guest_handler(h, a0);
        if (rt_trace()) {
            rt_log("intc", "%s %d (%s) handler 0x%08x returned %u", kind, number, name, h.vram, ret);
        }
    }
    if (!any && (line.dispatches & (line.dispatches - 1)) == 0) {
        rt_log("intc", "%s %d (%s) raised with no handler registered [#%" PRIu64 "]",
            kind, number, name, line.dispatches);
    }
}

int add_handler(Line* lines, int count, int number, uint32_t vram, int next, uint32_t gp) {
    if (number < 0 || number >= count) return -1;
    for (int i = 0; i < kMaxHandlersPerLine; ++i) {
        Handler& h = lines[number].h[i];
        if (!h.used) {
            h.used = true;
            h.vram = vram;
            h.gp = gp;
            h.next = next;
            return number * kMaxHandlersPerLine + i; /* handler id */
        }
    }
    return -1;
}

int remove_handler(Line* lines, int count, int number, int hid) {
    if (number < 0 || number >= count) return -1;
    int idx = hid - number * kMaxHandlersPerLine;
    if (idx < 0 || idx >= kMaxHandlersPerLine) return -1;
    lines[number].h[idx].used = false;
    return 0;
}

} // namespace

void rt_intc_init() {
    /* Everything already zero-initialized; EIE starts enabled: the kernel
     * enters the program with interrupts on and crt0 uses ei/di around
     * setup. */
}

void rt_intc_set_eie(bool on) {
    g_eie = on;
    if (on) rt_intc_deliver();
}

bool rt_intc_get_eie() { return g_eie; }
bool rt_in_interrupt() { return g_in_interrupt; }

/* COP0 Status for mfc0 $12: EIE is bit 16, IE is bit 0 (EE Core manual
 * layout; both must be set for interrupts). */
uint32_t rt_cop0_status_word() {
    return 0x10000001u & (g_eie ? 0xFFFFFFFFu : 0xFFFEFFFFu);
}

int rt_intc_add_handler(int cause, uint32_t vram, int next, uint32_t gp) {
    int id = add_handler(g_intc, kNumCauses, cause, vram, next, gp);
    rt_log("intc", "AddIntcHandler cause=%d (%s) handler=0x%08x next=%d gp=0x%08x -> id %d",
        cause, cause_name(cause), vram, next, gp, id);
    return id;
}

int rt_intc_remove_handler(int cause, int hid) {
    rt_log("intc", "RemoveIntcHandler cause=%d id=%d", cause, hid);
    return remove_handler(g_intc, kNumCauses, cause, hid);
}

int rt_intc_enable(int cause) {
    if (cause < 0 || cause >= kNumCauses) return 0;
    uint32_t bit = 1u << cause;
    int changed = (g_imask & bit) ? 0 : 1;
    g_imask |= bit;
    rt_log("intc", "_EnableIntc cause=%d (%s), imask=0x%04x", cause, cause_name(cause), g_imask);
    return changed;
}

int rt_intc_disable(int cause) {
    if (cause < 0 || cause >= kNumCauses) return 0;
    uint32_t bit = 1u << cause;
    int changed = (g_imask & bit) ? 1 : 0;
    g_imask &= ~bit;
    rt_log("intc", "_DisableIntc cause=%d (%s), imask=0x%04x", cause, cause_name(cause), g_imask);
    return changed;
}

int rt_dmac_add_handler(int ch, uint32_t vram, int next, uint32_t gp) {
    int id = add_handler(g_dmac, kNumChannels, ch, vram, next, gp);
    rt_log("intc", "AddDmacHandler channel=%d handler=0x%08x next=%d gp=0x%08x -> id %d",
        ch, vram, next, gp, id);
    return id;
}

int rt_dmac_remove_handler(int ch, int hid) {
    rt_log("intc", "RemoveDmacHandler channel=%d id=%d", ch, hid);
    return remove_handler(g_dmac, kNumChannels, ch, hid);
}

int rt_dmac_enable(int ch) {
    if (ch < 0 || ch >= kNumChannels) return 0;
    g_dmask |= 1u << ch;
    rt_log("intc", "_EnableDmac channel=%d, dmask=0x%04x", ch, g_dmask);
    return 1;
}

int rt_dmac_disable(int ch) {
    if (ch < 0 || ch >= kNumChannels) return 0;
    g_dmask &= ~(1u << ch);
    rt_log("intc", "_DisableDmac channel=%d, dmask=0x%04x", ch, g_dmask);
    return 1;
}

void rt_intc_raise(int cause) {
    if (cause >= 0 && cause < kNumCauses) g_istat |= 1u << cause;
}

void rt_dmac_raise(int ch) {
    if (ch >= 0 && ch < kNumChannels) g_dstat |= 1u << ch;
}

void rt_intc_deliver() {
    if (g_in_interrupt || !g_eie) return;
    if (!(g_istat & g_imask) && !(g_dstat & g_dmask)) return;
    g_in_interrupt = true;
    /* The kernel dispatcher clears the status bit, then calls the cause's
     * handlers. Loop until quiescent so a handler raising another line is
     * served in this pass. */
    for (int guard = 0; guard < 64; ++guard) {
        uint32_t ipend = g_istat & g_imask;
        uint32_t dpend = g_dstat & g_dmask;
        if (!ipend && !dpend) break;
        for (int c = 0; c < kNumCauses; ++c) {
            if (ipend & (1u << c)) {
                g_istat &= ~(1u << c);
                dispatch_line(g_intc[c], c, (uint32_t)c, "INTC", cause_name(c));
            }
        }
        for (int ch = 0; ch < kNumChannels; ++ch) {
            if (dpend & (1u << ch)) {
                g_dstat &= ~(1u << ch);
                char nm[8];
                std::snprintf(nm, sizeof(nm), "D%d", ch);
                dispatch_line(g_dmac[ch], ch, (uint32_t)ch, "DMAC", nm);
            }
        }
    }
    g_in_interrupt = false;
    rt_sched_maybe_preempt();
}

bool rt_intc_mmio_read(uint32_t addr, uint32_t* out) {
    switch (addr) {
        case 0x1000F000: *out = g_istat; return true;
        case 0x1000F010: *out = g_imask; return true;
        case 0x1000E010: *out = g_dstat | (g_dmask << 16); return true; /* D_STAT */
        /* D_CTRL/D_ENABLER and the rest of the DMAC block moved to
         * hw/dmac.cpp; only interrupt status/mask stays here. */
        default: return false;
    }
}

bool rt_intc_mmio_write(uint32_t addr, uint32_t v) {
    switch (addr) {
        case 0x1000F000: g_istat &= ~v; return true;        /* write 1 to clear */
        case 0x1000F010: g_imask ^= (v & 0x7FFF); return true; /* write 1 to toggle */
        case 0x1000E010:                                    /* D_STAT */
            g_dstat &= ~(v & 0xFFFF);
            g_dmask ^= (v >> 16) & 0x3FF;
            return true;
        default: return false;
    }
}

/* ---- GS shadow ----------------------------------------------------------- */

void rt_gs_vblank_start(unsigned field) {
    g_gs_field = field;
    g_csr_flags |= 0x8u; /* VSINT */
    /* Backend vsync: snapshots priv registers and emits the dump Vsync
     * packet (hw/gspriv.cpp). */
    rt_gs_vsync_hook(field);
}

void rt_gs_vblank_end() {
    /* HSINT and friends are not modeled; nothing to do at vblank end beyond
     * the INTC VB_OFF cause raised by the clock. */
}

bool rt_gs_mmio_read(uint32_t addr, uint64_t* out) {
    switch (addr) {
        case 0x12001000: *out = csr_value(); return true;   /* GS_CSR */
        case 0x12001010: *out = g_gs_imr; return true;      /* GS_IMR */
        default: return false;
    }
}

bool rt_gs_mmio_write(uint32_t addr, uint64_t v) {
    switch (addr) {
        case 0x12001000:
            if (v & 0x200) g_csr_flags = 0;                 /* RESET clears events */
            g_csr_flags &= ~(uint32_t)(v & 0x1F);           /* write 1 clears flag */
            return true;
        case 0x12001010:
            g_gs_imr = v;
            return true;
        default: return false;
    }
}

uint64_t rt_gs_get_imr() { return g_gs_imr; }
void rt_gs_put_imr(uint64_t v) { g_gs_imr = v; }
