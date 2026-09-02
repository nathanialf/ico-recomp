/* ee/alarms.cpp: EE kernel alarms (SetAlarm/ReleaseAlarm and the i-variants,
 * syscalls 0x18/0x19, 0x1E/0x1F and 0xFC-0xFF).
 *
 * Contract (ps2sdk ee/kernel/include/kernel.h, used as a clean-room
 * structural reference; the prototypes and syscall numbers are public SDK
 * facts):
 *
 *   int SetAlarm(u16 time, void (*handler)(int id, u16 time, void *arg),
 *                void *arg);
 *   int ReleaseAlarm(int id);
 *
 * SetAlarm arms a one-shot alarm that calls handler(id, time, arg) in
 * interrupt context once `time` ticks of the kernel's alarm clock have
 * elapsed, and returns the alarm id, or -1 when the kernel's fixed alarm
 * table has no free entry. ReleaseAlarm cancels an armed alarm and returns
 * its id, or -1 when that id is not armed. The i-variants are the same
 * calls made from handler context.
 *
 * Tick unit: the alarm clock counts H-BLANKs, about 15.7 kHz on NTSC and so
 * roughly 63.5 us per tick, which is RT_CYCLES_PER_HBLANK bus cycles (see
 * the derivation on that constant in kernel.h). The retail game's two users
 * both treat a tick as much shorter than a field: libcdvd's blocking read
 * retries every 0x3C ticks (about 3.8 ms) and its stream path sleeps 8
 * ticks (about 0.5 ms) between RPC retries.
 *
 * Delivery model, matching timers.cpp and intc.cpp: rt_alarms_run_due()
 * runs from rt_clock_tick alongside rt_timers_run_due and only moves a due
 * alarm to the pending state, the same way a due timer only raises an INTC
 * bit. The guest handler runs from rt_intc_deliver(), on the shared
 * interrupt-handler context and stack, at an instruction boundary (MMIO
 * trap, syscall end, backedge, scheduler idle loop, ei). That is what lets
 * an alarm fire while every thread is asleep: the scheduler's idle path
 * jumps the clock to the next timeline event and rt_alarms_next_event()
 * puts an armed alarm on that timeline.
 *
 * Both users in the retail binary park a thread on the alarm and let the
 * handler release it: the libcdvd sleep helper at 0x0024BFD0 creates a
 * semaphore, arms the handler at 0x0024BFA8 with the semaphore id as arg,
 * and blocks in WaitSema while the handler does iSignalSema(arg); the
 * second user (0x0024FBF8) arms the handler at 0x0024FBD0 with its own
 * thread id and calls SleepThread, and the handler wakes that thread.
 * Neither handler goes through ExitHandler: each ends with `ei` and a plain
 * `jr $31` back to the kernel, which is what rt_intc_run_handler's
 * clean-exit ra sentinel already models for INTC and DMAC handlers.
 * Neither user calls ReleaseAlarm, and neither reads the returned id.
 */
#include "kernel.h"

#include "../prof.h"

#include <cinttypes>

namespace {

/* The retail kernel's alarm table size and its id numbering are not
 * documented facts we have measured; all that is relied on here is that an
 * id is non-negative and that exhaustion returns -1. 64 entries is a
 * runtime choice, and running out is logged loudly rather than papered
 * over: the game never arms more than one alarm at a time. */
constexpr int kMaxAlarms = 64;

struct Alarm {
    bool armed = false;    /* counting down towards `due` */
    bool pending = false;  /* due reached, handler not dispatched yet */
    uint16_t time = 0;
    uint32_t handler = 0;
    uint32_t arg = 0;
    uint32_t gp = 0;       /* $gp at SetAlarm; restored for the handler call */
    uint32_t entry = 0;    /* translated function to enter (see resolve_entry) */
    int32_t sp_delta = 0;  /* stack adjustment skipped ahead of `entry` */
    uint64_t due = 0;      /* absolute vclk */
};

Alarm g_alarms[kMaxAlarms];
uint64_t g_arms = 0;
uint64_t g_fires = 0;
bool g_first_fire_logged = false;

double ticks_ms(uint16_t t) {
    return (double)t * (double)RT_CYCLES_PER_HBLANK * 1000.0 / (double)RT_BUSCLK_HZ;
}

bool translated(uint32_t vram) {
    return vram >= RECOMP_TEXT_BASE && vram < RECOMP_TEXT_LIMIT && g_functab[RECOMP_FUNC_IDX(vram)];
}

/* A handler address is not always the start of a translated function. The
 * translator emits one C function per symbol, and the retail binary points
 * SetAlarm at an address inside one: the handler entry is the delay slot of
 * a two-instruction function, a single stack adjustment that falls through
 * into the next function.
 *
 * Resolve that from guest memory at arm time instead of hard-coding the
 * address, so no ROM-derived constant lives in this source (the same
 * discipline sif.cpp uses for the sifcmd sub-entries): step forward to the
 * first address that does have a translation, and accept only
 * `addiu $29, $29, simm16` on the way, folding it into the stack pointer
 * the handler is entered with. Anything else is fatal. Entering at the
 * wrong instruction, or skipping one that does more than move the stack,
 * would be silent wrongness. */
constexpr int kMaxSubEntryInsns = 4;

/* Resolutions are cached and logged once per distinct handler address: the
 * game re-arms the same alarm in a retry loop. */
struct SubEntry {
    uint32_t vram = 0;
    uint32_t entry = 0;
    int32_t sp_delta = 0;
};
SubEntry g_resolved[8];
int g_resolved_n = 0;

void resolve_uncached(uint32_t vram, uint32_t* entry, int32_t* sp_delta) {
    *entry = vram;
    *sp_delta = 0;
    for (int i = 0; i <= kMaxSubEntryInsns; ++i) {
        if (translated(*entry)) {
            if (*entry != vram) {
                rt_log("alarm", "handler 0x%08x is not a translated function entry; entering "
                    "0x%08x with sp %+d after %d skipped stack adjustment(s)",
                    vram, *entry, *sp_delta, i);
            }
            return;
        }
        if (i == kMaxSubEntryInsns) break;
        const uint8_t* p = rt_gptr(*entry);
        if (!p) {
            rt_fatal("alarm", nullptr, "SetAlarm handler 0x%08x has no translation and its code "
                "is not mapped in guest RAM", vram);
        }
        uint32_t w;
        std::memcpy(&w, p, 4);
        /* addiu $29, $29, simm16: opcode 0x09, rs = rt = 29. */
        if ((w >> 26) != 0x09u || ((w >> 21) & 0x1Fu) != 29u || ((w >> 16) & 0x1Fu) != 29u) {
            rt_fatal("alarm", nullptr, "SetAlarm handler 0x%08x has no translation, and the "
                "instruction at 0x%08x (word 0x%08x) is not a stack adjustment this runtime can "
                "step over to reach the next translated entry", vram, *entry, w);
        }
        *sp_delta += (int32_t)(int16_t)(w & 0xFFFFu);
        *entry += 4;
    }
    rt_fatal("alarm", nullptr, "SetAlarm handler 0x%08x has no translated function entry within "
        "%d instructions", vram, kMaxSubEntryInsns);
}

void resolve_entry(uint32_t vram, uint32_t* entry, int32_t* sp_delta) {
    for (int i = 0; i < g_resolved_n; ++i) {
        if (g_resolved[i].vram == vram) {
            *entry = g_resolved[i].entry;
            *sp_delta = g_resolved[i].sp_delta;
            return;
        }
    }
    resolve_uncached(vram, entry, sp_delta);
    if (g_resolved_n < (int)(sizeof(g_resolved) / sizeof(g_resolved[0]))) {
        g_resolved[g_resolved_n++] = SubEntry{vram, *entry, *sp_delta};
    }
}

} // namespace

int rt_alarm_set(uint32_t time, uint32_t handler, uint32_t arg, uint32_t gp) {
    /* The kernel's time argument is u16; the syscall stub passes a full
     * register, so take the low half exactly as the kernel does. */
    const uint16_t t = (uint16_t)time;
    for (int i = 0; i < kMaxAlarms; ++i) {
        Alarm& al = g_alarms[i];
        if (al.armed || al.pending) continue;
        al = Alarm{};
        al.armed = true;
        al.time = t;
        al.handler = handler;
        al.arg = arg;
        al.gp = gp;
        resolve_entry(handler, &al.entry, &al.sp_delta);
        /* time == 0 is not a documented case. It lands due immediately here,
         * so the handler runs at the next delivery point. */
        al.due = rt_clock_now() + (uint64_t)t * RT_CYCLES_PER_HBLANK;
        ++g_arms;
        if ((g_arms & (g_arms - 1)) == 0) {
            rt_log("alarm", "armed id=%d time=%u ticks (%.3f ms) handler=0x%08x arg=0x%08x "
                "gp=0x%08x thread=%d due=%" PRIu64 " [#%" PRIu64 "]",
                i, t, ticks_ms(t), handler, arg, gp, rt_thread_current_id(), al.due, g_arms);
        }
        return i;
    }
    rt_log("alarm", "WARNING: SetAlarm(time=%u, handler=0x%08x, arg=0x%08x) with all %d alarm "
        "slots armed; returning -1 the way the kernel does when its table is full. Whatever "
        "was waiting on this alarm will not be woken.", (uint16_t)time, handler, arg, kMaxAlarms);
    return -1;
}

int rt_alarm_release(int id) {
    if (id < 0 || id >= kMaxAlarms) return -1;
    Alarm& al = g_alarms[id];
    if (!al.armed && !al.pending) return -1;
    /* Cancelling an alarm that is already due but not yet dispatched: on
     * hardware the interrupt would have been taken at the due tick, so this
     * window only exists here while interrupts are disabled. What the retail
     * kernel does in it is not known; cancelling is the reading that matches
     * "not yet delivered". No game code takes this path. */
    const bool was_pending = al.pending;
    al = Alarm{};
    rt_log("alarm", "released id=%d (%s)", id, was_pending ? "was due, handler not yet run" : "was armed");
    return id;
}

uint64_t rt_alarms_next_event() {
    uint64_t best = UINT64_MAX;
    for (const Alarm& al : g_alarms) {
        if (al.armed && al.due < best) best = al.due;
    }
    return best;
}

void rt_alarms_run_due() {
    const uint64_t now = rt_clock_now();
    for (Alarm& al : g_alarms) {
        if (!al.armed || al.due > now) continue;
        al.armed = false;
        al.pending = true;
    }
}

bool rt_alarms_pending() {
    for (const Alarm& al : g_alarms) {
        if (al.pending) return true;
    }
    return false;
}

void rt_alarms_dispatch_pending() {
    RT_PROF_ZONE(RT_PROF_INTC);
    for (int i = 0; i < kMaxAlarms; ++i) {
        if (!g_alarms[i].pending) continue;
        /* One-shot: the slot is free before the handler runs, so a handler
         * that re-arms an alarm can reuse this entry. */
        const Alarm fired = g_alarms[i];
        g_alarms[i] = Alarm{};
        ++g_fires;
        if (!g_first_fire_logged) {
            g_first_fire_logged = true;
            rt_log("alarm", "first alarm fired: handler 0x%08x runs on the interrupt context and "
                "stack, like an INTC handler, %u H-blank ticks (%.3f ms) after it was armed",
                fired.handler, fired.time, ticks_ms(fired.time));
        }
        if ((g_fires & (g_fires - 1)) == 0) {
            rt_log("alarm", "fired id=%d time=%u handler=0x%08x arg=0x%08x late=%" PRIu64
                " cycles [#%" PRIu64 "]", i, fired.time, fired.handler, fired.arg,
                rt_clock_now() - fired.due, g_fires);
        }
        /* handler(id, time, arg), per the ps2sdk prototype. */
        rt_intc_run_handler(fired.entry, (uint32_t)i, fired.time, fired.arg, fired.gp,
            fired.sp_delta);
    }
}
