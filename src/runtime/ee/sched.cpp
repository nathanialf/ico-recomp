/* ee/sched.cpp: guest thread scheduler, semaphores, and the virtual clock.
 *
 * Model: one guest thread = one minicoro stackful coroutine + one
 * R5900Context. PS2 EE kernel semantics (references: ps2sdk ee/kernel
 * headers, structural facts only):
 *   - 128 priorities (0 = highest), FIFO within a priority.
 *   - Strictly non-preemptive: the running thread keeps the CPU until it
 *     blocks (WaitSema, SleepThread, SuspendThread), yields
 *     (RotateThreadReadyQueue / ChangeThreadPriority on itself), or exits.
 *   - Interrupt handlers may iSignalSema/iWakeupThread; a reschedule happens
 *     after handler return and preempts the interrupted thread if a
 *     higher-priority thread became ready (preempted thread goes to the head
 *     of its priority's ready queue).
 *
 * Virtual clock: u64 EE bus cycles. Time advances only at runtime trap
 * points (syscalls, MMIO accesses) and in the scheduler idle loop, which
 * jumps straight to the next timeline event. The timeline is: vblank field
 * boundaries (the programmed video mode's field rate, alternating fields),
 * timer compare/overflow
 * interrupts (timers.cpp), kernel alarms (alarms.cpp) and deferred SIF
 * responses (sif/rpc.cpp, which owns the delivery queue; sif/sif.cpp owns
 * the register file).
 */
#include "kernel.h"

#include "prof.h"

#define MINICORO_IMPL
#include "minicoro.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <thread>
#include <vector>

EEKernelState g_kern;

/* ---- small shared helpers ----------------------------------------------- */

bool rt_trace() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("ICORECOMP_TRACE");
        v = (e && std::strcmp(e, "1") == 0) ? 1 : 0;
    }
    return v == 1;
}

uint32_t rt_gread32(uint32_t a) {
    uint8_t* p = rt_gptr(a);
    if (!p) rt_fatal("sched", rt_sched_current_ctx(), "guest read32 of unmapped address 0x%08x", a);
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

void rt_gwrite32(uint32_t a, uint32_t v) {
    uint8_t* p = rt_gptr(a);
    if (!p) rt_fatal("sched", rt_sched_current_ctx(), "guest write32 of unmapped address 0x%08x", a);
    std::memcpy(p, &v, 4);
}

/* ---- thread / semaphore state ------------------------------------------- */

namespace {

constexpr int kMaxThreads = 128;
constexpr int kMaxSemas = 256;
constexpr int kNumPrios = 128;
/* Host stack per guest-thread coroutine. Guest data lives in guest RAM; this
 * only holds the translated C frames, but interrupt-handler dispatch can
 * nest on top of a deep guest call chain, so be generous. */
constexpr size_t kCoroStackSize = 2u * 1024 * 1024;
/* Per-thread wake timestamps kept for the inventory's "wakes in the last
 * second". A thread woken by the vblank handler wakes 50 times a second on
 * PAL, so 128 covers that with room for a semaphore signalled harder, and
 * the inventory says "128+" rather than a wrong number if it is ever
 * exceeded. */
constexpr size_t kWakeRing = 128;

enum class TState : uint8_t { Free, Dormant, Ready, Run, Wait, Suspend, WaitSuspend };
enum class WaitKind : uint8_t { None, Sleep, Sema };

/* Sony ThreadParam.status values (ps2sdk kernel.h THS_*). */
constexpr uint32_t THS_RUN = 0x01, THS_READY = 0x02, THS_WAIT = 0x04,
                   THS_SUSPEND = 0x08, THS_DORMANT = 0x10;

struct EEThread {
    /* EE Status.EIE as this thread left it. The interrupt enable bit is
     * part of a thread's saved context on the real kernel: a thread that
     * runs di and then blocks keeps interrupts off for itself only, and
     * every other thread keeps taking them. Modelled as one global flag,
     * the PAL loader's read thread (func_00133250) did di and then slept in
     * sceCdSync's sceCdDelayThread loop, and no alarm or SIF0 interrupt was
     * ever delivered to anyone: the run idled forever after the first disc
     * read. The scheduler itself runs with interrupts enabled, which is
     * where pending interrupts are delivered between threads. */
    bool eie = true;
    int id = 0;
    TState state = TState::Free;
    WaitKind wait = WaitKind::None;
    int wait_sema = 0;
    int priority = 0;
    int init_priority = 0;
    uint32_t entry = 0, stack_base = 0, stack_size = 0, gp = 0;
    uint32_t attr = 0, option = 0;
    uint32_t start_arg = 0;
    int wakeup_count = 0;
    bool pending_delete = false;    /* ExitDeleteThread */
    int release_ret = 0;            /* value a released/awoken blocking call returns */
    uint64_t blocked_since = 0;
    uint64_t run_count = 0;
    /* Why this thread is interesting to an inventory, beyond its state.
     *
     * last_syscall_*: the syscall this thread made most recently, recorded
     * by syscalls.cpp. The run-state summary already keeps the last syscall
     * of the whole run; this is the per-thread version, which is what says
     * that thread 7 last called SleepThread while thread 3 last called
     * WaitSema.
     *
     * wake_at: the virtual-clock times of this thread's last kWakeRing
     * wakes. A count is not enough on its own: the failure this exists for
     * is a run where fields advanced at 50 Hz forever, and the question
     * that separates a thread the vblank handler keeps waking from a thread
     * nothing has touched in twenty seconds is "how many wakes in the last
     * second", not "how many since boot". A ring rather than a windowed
     * counter so the answer is exact rather than an artefact of when the
     * window last rolled; kWakeRing caps how many it can report. */
    int last_syscall_num = -1;
    const char* last_syscall_name = nullptr;
    uint64_t last_syscall_vclk = 0;
    uint64_t wakes = 0;
    uint64_t wake_at[kWakeRing] = {0};
    R5900Context ctx {};
    mco_coro* co = nullptr;
};

struct EESema {
    int id = 0;
    bool alive = false;
    int count = 0, max_count = 0, init_count = 0;
    uint32_t attr = 0, option = 0;
    int creator_tid = 0;
    uint32_t creator_ra = 0;        /* guest $ra at CreateSema, call-site hint */
    uint64_t signals = 0, waits = 0;
    uint64_t refused = 0;           /* SignalSema calls refused at max_count */
    bool refused_warned = false;    /* the overflow warn is one per semaphore */
    std::deque<int> waiters;        /* FIFO thread ids */
};

/* Set once by rt_sched_boot; read by rt_sched_on_ee_thread from any
 * thread. Written before any other thread can observe it and never again,
 * so a plain object is enough. */
std::thread::id g_ee_thread;
bool g_ee_thread_set = false;

EEThread g_threads[kMaxThreads];    /* index == id; 0 unused */
EESema g_semas[kMaxSemas];          /* index == id; 0 unused */
std::deque<int> g_ready[kNumPrios];
int g_current = 0;                  /* running thread id, 0 = scheduler */
uint64_t g_resumes = 0;             /* total thread resumes, for hang detection */

/* ---- virtual clock + vblank timeline ---- */

uint64_t g_vclk = 0;
/* Next vblank START. Seeded from the boot mode's field length (a constant,
 * because this is a static initializer) and moved on by the current field
 * length from then on; a mode change re-derives it in video_mode_changed
 * below. */
uint64_t g_next_field_edge = RT_CYCLES_PER_FIELD_BOOT;
uint64_t g_next_vblank_end = UINT64_MAX;
unsigned g_field = 0;
uint64_t g_vblank_count = 0;
uint64_t g_max_vblanks = 0;         /* ICORECOMP_MAX_VBLANKS, 0 = unlimited */

EEThread* tget(int id) {
    if (id <= 0 || id >= kMaxThreads) return nullptr;
    EEThread* t = &g_threads[id];
    return t->state == TState::Free ? nullptr : t;
}

EEThread* tresolve(int id) { return tget(id == 0 ? g_current : id); }

EESema* sget(int id) {
    if (id <= 0 || id >= kMaxSemas) return nullptr;
    EESema* s = &g_semas[id];
    return s->alive ? s : nullptr;
}

void ready_push_back(EEThread* t) {
    t->state = TState::Ready;
    g_ready[t->priority].push_back(t->id);
}

void ready_push_front(EEThread* t) {
    t->state = TState::Ready;
    g_ready[t->priority].push_front(t->id);
}

void ready_remove(EEThread* t) {
    auto& q = g_ready[t->priority];
    for (auto it = q.begin(); it != q.end(); ++it) {
        if (*it == t->id) { q.erase(it); return; }
    }
}

int best_ready_prio() {
    for (int p = 0; p < kNumPrios; ++p) {
        if (!g_ready[p].empty()) return p;
    }
    return -1;
}

const char* tstate_name(TState s) {
    switch (s) {
        case TState::Free: return "FREE";
        case TState::Dormant: return "DORMANT";
        case TState::Ready: return "READY";
        case TState::Run: return "RUN";
        case TState::Wait: return "WAIT";
        case TState::Suspend: return "SUSPEND";
        case TState::WaitSuspend: return "WAITSUSPEND";
    }
    return "?";
}

/* Yield the current thread's coroutine back to the scheduler loop. Caller
 * must have set the thread's state (and queued it if Ready). */
void yield_current() {
    mco_coro* co = mco_running();
    if (!co) rt_fatal("sched", nullptr, "yield_current called outside a thread coroutine");
    mco_yield(co);
}

/* Block the current thread. Returns when some agent readied+resumed it;
 * the return value is thread->release_ret (0 normally, error code when
 * released by ReleaseWaitThread / DeleteSema). */
int block_current(WaitKind k, int sema_id) {
    EEThread* t = tget(g_current);
    if (!t) rt_fatal("sched", nullptr, "block with no current thread");
    if (rt_in_interrupt()) {
        rt_fatal("sched", &t->ctx, "blocking call from interrupt context (wait kind %d, sema %d)", (int)k, sema_id);
    }
    t->state = TState::Wait;
    t->wait = k;
    t->wait_sema = sema_id;
    t->release_ret = 0;
    t->blocked_since = g_vclk;
    yield_current();
    t->wait = WaitKind::None;
    t->wait_sema = 0;
    return t->release_ret;
}

/* Make a blocked/suspended thread runnable again. The one choke point every
 * wake goes through (WakeupThread, SignalSema, ReleaseWaitThread, a deleted
 * semaphore), so it is also where the inventory's wake history is kept. */
void unblock(EEThread* t, int release_ret) {
    t->wake_at[t->wakes % kWakeRing] = g_vclk;
    ++t->wakes;
    t->release_ret = release_ret;
    if (t->state == TState::Wait) {
        t->wait = WaitKind::None;
        ready_push_back(t);
    } else if (t->state == TState::WaitSuspend) {
        t->wait = WaitKind::None;
        t->state = TState::Suspend; /* stays parked until ResumeThread */
    }
}

void coro_destroy(EEThread* t) {
    if (t->co) {
        mco_destroy(t->co); /* legal on dead or suspended coroutines */
        t->co = nullptr;
    }
}

void thread_trampoline(mco_coro* co) {
    EEThread* t = static_cast<EEThread*>(mco_get_user_data(co));
    uint32_t entry = t->entry;
    if (entry < RECOMP_TEXT_BASE || entry >= RECOMP_TEXT_LIMIT || !g_functab[RECOMP_FUNC_IDX(entry)]) {
        rt_fatal("sched", &t->ctx, "thread %d entry vram 0x%08x has no translation", t->id, entry);
    }
    g_functab[RECOMP_FUNC_IDX(entry)](&t->ctx);
    /* Guest root function returned: implicit ExitThread. */
    rt_log_info("sched", "thread %d root function returned; implicit ExitThread", t->id);
    t->state = TState::Dormant;
    /* Falling off the trampoline marks the coroutine dead; the scheduler
     * loop reaps it. */
}

void coro_start(EEThread* t, uint32_t arg) {
    std::memset(&t->ctx, 0, sizeof(t->ctx));
    t->ctx.r[4].u64x[0] = arg;                                   /* a0 */
    t->ctx.r[28].u64x[0] = t->gp;                                /* gp */
    t->ctx.r[29].u64x[0] = (uint64_t)((t->stack_base + t->stack_size) & ~0xFu); /* sp */
    t->ctx.r[31].u64x[0] = RT_CLEAN_EXIT_VRAM;                   /* ra sentinel */
    t->ctx.host = t;
    t->start_arg = arg;
    mco_desc desc = mco_desc_init(thread_trampoline, kCoroStackSize);
    desc.user_data = t;
    mco_result r = mco_create(&t->co, &desc);
    if (r != MCO_SUCCESS) rt_fatal("sched", nullptr, "mco_create failed for thread %d: %s", t->id, mco_result_description(r));
}

} // namespace

/* ---- clock -------------------------------------------------------------- */

uint64_t rt_clock_now() { return g_vclk; }

static uint64_t clock_next_event() {
    uint64_t nxt = g_next_field_edge;
    if (g_next_vblank_end < nxt) nxt = g_next_vblank_end;
    uint64_t t = rt_timers_next_event();
    if (t < nxt) nxt = t;
    t = rt_sif_next_event();
    if (t < nxt) nxt = t;
    t = rt_alarms_next_event();
    if (t < nxt) nxt = t;
    return nxt;
}

/* Where virtual time comes from. A field is rt_cycles_per_field() cycles;
 * whether those cycles are billed by a spinning guest (backedge/mmio) or
 * skipped over while every thread sleeps (idle) is the difference between
 * a wait that costs host time and one that costs none. */
uint64_t g_cyc_backedge = 0;
uint64_t g_cyc_mmio = 0;
uint64_t g_cyc_idle = 0;

extern "C" void rt_clock_sources(uint64_t* backedge, uint64_t* mmio, uint64_t* idle) {
    *backedge = g_cyc_backedge;
    *mmio = g_cyc_mmio;
    *idle = g_cyc_idle;
    g_cyc_backedge = g_cyc_mmio = g_cyc_idle = 0;
}

void rt_clock_tick(uint64_t cycles) {
    uint64_t target = g_vclk + cycles;
    /* Step event by event so ordering stays exact. */
    for (;;) {
        uint64_t nxt = clock_next_event();
        if (nxt > target) break;
        g_vclk = nxt;
        if (g_vclk >= g_next_field_edge) {
            ++g_vblank_count;
            g_field ^= 1;
            g_next_vblank_end = g_next_field_edge + rt_cycles_vblank();
            g_next_field_edge += rt_cycles_per_field();
            rt_gs_vblank_start(g_field);
            rt_intc_raise(RT_INTC_VB_ON);
            if (g_kern.vsync_flag_ptr) { /* SetVSyncFlag one-shot */
                rt_gwrite32(g_kern.vsync_flag_ptr, 1);
                if (g_kern.vsync_csr_ptr) rt_gwrite32(g_kern.vsync_csr_ptr, g_field);
                g_kern.vsync_flag_ptr = g_kern.vsync_csr_ptr = 0;
            }
            static uint64_t logged = 0;
            ++logged;
            if ((logged & (logged - 1)) == 0) {
                rt_log_debug("vblank", "field #%" PRIu64 " start (vclk=%" PRIu64 ")", g_vblank_count, g_vclk);
            }
            if (g_max_vblanks && g_vblank_count >= g_max_vblanks) {
                /* A bounded diagnostic run reaching its own bound is a
                 * deliberate end: it is what the run was asked to do. Named
                 * here rather than left to rt_sched_exit_game so the summary
                 * comes out at info; the first caller wins, so this is the
                 * reason that sticks. */
                rt_run_set_exit_reason(true, "ICORECOMP_MAX_VBLANKS=%" PRIu64 " reached",
                    g_max_vblanks);
                rt_sched_exit_game(0, "ICORECOMP_MAX_VBLANKS reached");
            }
        }
        if (g_vclk >= g_next_vblank_end) {
            g_next_vblank_end = UINT64_MAX;
            rt_gs_vblank_end();
            rt_intc_raise(RT_INTC_VB_OFF);
        }
        rt_timers_run_due();
        rt_sif_run_due();
        rt_alarms_run_due();
    }
    g_vclk = target;
}

/* Bus cycles billed for one EE hardware-register access.
 *
 * This used to be 512, chosen so that a tight guest poll loop would cross a
 * 16.7 ms field boundary in a few thousand iterations. That is a liveness
 * figure, not a hardware one, and it is 256 times what rt_backedge bills a
 * RAM-resident loop iteration, so any guest code that drives a peripheral
 * through its registers runs two orders of magnitude slower than the rest
 * of the game.
 *
 * The attract movie is exactly that code, and it measured the old value out
 * of existence. In the 22:22 Windows log (dist/windows/icorecomp.log, prof
 * window "fields 2881..3060") the movie spends the whole field on register
 * traffic and nothing else: 855361 MMIO accesses over 180 fields is 4752
 * per field, and 4752 * 512 = 2433024 of the 2460060 cycles a field holds.
 * The host is idle 68.7% of that field ("limit" bucket) and no thread is
 * ever blocked ("idle 0.0%"), so the movie's frame rate is set by this
 * constant and by nothing else.
 *
 * The retail game bounds it from the other side. The same window shows
 * roughly 50000 to 60000 register accesses per decoded picture, most of
 * them the MPEG library's byte-at-a-time FDEC scan across the stream's
 * zero stuffing. A retail PS2 plays that movie at 29.97 pictures a second,
 * two fields per picture, which is 4920120 bus cycles. Even if the EE did
 * nothing else at all in those two fields, the per-access cost cannot
 * exceed about 89 cycles, and the loop also runs motion compensation, the
 * CSC feed, the display list and the sound tick, so the real figure is well
 * under that.
 *
 * The exact EE uncached-access latency is not measured here and this is not
 * a claim about it. 32 bus cycles (64 EE cycles) is chosen as a value
 * comfortably inside the bound the retail workload proves, and 16 times a
 * loop iteration's 2 cycles, which is the right order for an uncached
 * hardware read against RAM-resident code. A poll loop still crosses a
 * field, in about 72000 iterations rather than 5000.
 *
 * Gameplay was checked on this value: a user run of the retail game on this
 * build held 59.9 fields per second with a healthy audio queue. The hazard
 * to watch when changing it is the one the g_backedge_cycles comment below
 * names, because this constant pulls the same lever from the other side:
 * bill too few cycles per access and the guest does more work per field
 * than a real EE could, so the game's sound tick runs too often per field
 * and over-refills the sndn2 stream ring.
 *
 * Tunable by ICORECOMP_MMIO_CYCLES for a sweep; the value in force is
 * logged at startup. */
uint64_t g_mmio_cycles = 32;

void rt_kernel_mmio_bill() {
    g_cyc_mmio += g_mmio_cycles;
    rt_clock_tick(g_mmio_cycles);
}

void rt_kernel_mmio_tick() {
    rt_kernel_mmio_bill();
    rt_intc_deliver();
}

/* rt_backedge: liveness for RAM-only spin loops. Generated code calls this
 * on every taken backward branch. A loop that touches neither MMIO nor a
 * syscall (e.g. the sndn2 library spinning on an IOP-updated counter in EE
 * RAM) would otherwise never reach a delivery point and livelock the whole
 * process. The fast path is one increment and one mask test; every
 * kBackedgeInterval-th call bills the elapsed iterations to the virtual
 * clock (kBackedgeCycles bus cycles per iteration, ~2 CPU cycles per loop
 * instruction pair) and runs the same delivery path MMIO reads use (due
 * timeline events, then INTC/DMAC handler dispatch + preemption check). */
namespace {
constexpr uint64_t kBackedgeInterval = 4096; /* power of two */
/* Bus cycles billed per taken backward branch. This is the only thing that
 * sets the emulated EE speed relative to the video and audio clocks: fields
 * are a fixed number of virtual cycles, so billing too few cycles per loop
 * iteration lets the guest do more work per field than a real EE could.
 * The game's sound tick then runs too often per field and over-refills the
 * stream ring, overwriting audio that has not been played yet.
 *
 * 2 assumes a loop iteration costs 2 bus cycles. A real spin-loop body is
 * 8-12 EE instructions, so ~5 bus cycles is closer. Tunable by ear:
 * ICORECOMP_EE_LOOP_CYCLES=n. */
uint64_t g_backedge_cycles = 2;
uint64_t g_backedges = 0;
} // namespace

extern "C" void rt_backedge(void) {
    if ((++g_backedges & (kBackedgeInterval - 1)) != 0) return;
    g_cyc_backedge += kBackedgeInterval * g_backedge_cycles;
    rt_clock_tick(kBackedgeInterval * g_backedge_cycles);
    rt_intc_deliver();
}

/* ---- scheduler core ------------------------------------------------------ */

namespace {

/* rt_video_add_mode_hook, registered in rt_sched_init.
 *
 * The GS CRTC restarts its vertical counter when SMODE1 changes, so the
 * field the game was in ends at the write and the next field edge is one
 * whole field of the new mode away. A vblank-end deadline that is still
 * pending belongs to the field that just ended and is left alone: it is
 * always nearer than the new field edge, because a vblank is a fraction of
 * a field. */
void video_mode_changed(RtVideoMode, RtVideoMode) {
    g_next_field_edge = g_vclk + rt_cycles_per_field();
    rt_log_info("sched", "video mode change at vclk %llu: next field edge at %llu"
        " (%llu cycles per field)",
        (unsigned long long)g_vclk, (unsigned long long)g_next_field_edge,
        (unsigned long long)rt_cycles_per_field());
}

} // namespace

void rt_sched_init() {
    const char* e = std::getenv("ICORECOMP_MAX_VBLANKS");
    if (e) g_max_vblanks = std::strtoull(e, nullptr, 10);
    if (const char* c = std::getenv("ICORECOMP_EE_LOOP_CYCLES")) {
        uint64_t v = std::strtoull(c, nullptr, 10);
        if (v >= 1 && v <= 64) g_backedge_cycles = v;
    }
    rt_log_info("sched", "EE loop billing: %llu bus cycles per backedge "
                    "(ICORECOMP_EE_LOOP_CYCLES; higher = slower emulated EE "
                    "relative to the field clock)",
        (unsigned long long)g_backedge_cycles);
    if (const char* m = std::getenv("ICORECOMP_MMIO_CYCLES")) {
        uint64_t v = std::strtoull(m, nullptr, 10);
        if (v >= 1 && v <= 4096) {
            g_mmio_cycles = v;
        } else {
            rt_log_warn("sched", "ICORECOMP_MMIO_CYCLES=%s is outside 1..4096; keeping %llu",
                m, (unsigned long long)g_mmio_cycles);
        }
    }
    rt_log_info("sched", "MMIO billing: %llu bus cycles per hardware-register access, so a field of "
                    "%llu cycles holds %llu of them (ICORECOMP_MMIO_CYCLES; this is what paces "
                    "register-driven guest code such as the MPEG player)",
        (unsigned long long)g_mmio_cycles, (unsigned long long)rt_cycles_per_field(),
        (unsigned long long)(rt_cycles_per_field() / g_mmio_cycles));
    rt_video_add_mode_hook(video_mode_changed);
    rt_intc_init();
    rt_timers_init();
    rt_sif_init();
}

R5900Context* rt_sched_current_ctx() {
    EEThread* t = tget(g_current);
    return t ? &t->ctx : nullptr;
}

int rt_thread_current_id() { return g_current; }

void rt_sched_maybe_preempt() {
    if (rt_in_interrupt() || g_current == 0) return;
    EEThread* c = tget(g_current);
    if (!c || c->state != TState::Run) return;
    int best = best_ready_prio();
    if (best >= 0 && best < c->priority) {
        /* Preempted thread goes to the HEAD of its priority queue so it runs
         * again before same-priority peers. */
        ready_push_front(c);
        yield_current();
    }
}

[[noreturn]] static void sched_loop() {
    uint64_t idle_streak_start = 0;
    bool idling = false;
    for (;;) {
        /* The field watchdog runs on its own thread and cannot walk these
         * tables while this one is editing them, so it leaves a request
         * instead and the dump happens here, between two thread resumes.
         * See rt_run_request_inventory in runtime.h. */
        if (const char* why = rt_run_take_inventory_request()) {
            rt_sched_dump_inventory(why);
        }
        int prio = best_ready_prio();
        if (prio >= 0) {
            idling = false;
            int id = g_ready[prio].front();
            g_ready[prio].pop_front();
            EEThread* t = &g_threads[id];
            t->state = TState::Run;
            g_current = id;
            ++t->run_count;
            ++g_resumes;
            mco_result r;
            /* Restore this thread's interrupt enable state (see EEThread::eie).
             * Enabling delivers whatever became pending while it was off the
             * CPU, before its first instruction, which is what a real
             * context switch into a thread with EIE set does. */
            rt_intc_set_eie(t->eie);
            {
                /* Everything the guest triggers (MMIO, syscalls, DMA,
                 * VIF1, VU1, GIF, GS) opens its own zone underneath
                 * this one, so the "ee" bucket ends up holding
                 * translated EE code and nothing else. */
                RT_PROF_ZONE(RT_PROF_EE);
                r = mco_resume(t->co);
            }
            t->eie = rt_intc_get_eie();
            g_current = 0;
            /* The scheduler context takes interrupts: a thread that blocked
             * with di must not hold them off the whole machine. */
            rt_intc_set_eie(true);
            if (r != MCO_SUCCESS) {
                rt_fatal("sched", &t->ctx, "mco_resume(thread %d) failed: %s", id, mco_result_description(r));
            }
            if (t->state == TState::Run) {
                /* Coroutine finished (root function returned) or yielded
                 * without setting a state; the former is normal exit. */
                if (mco_status(t->co) == MCO_DEAD) {
                    t->state = TState::Dormant;
                } else {
                    rt_fatal("sched", &t->ctx, "thread %d yielded without a scheduling state", id);
                }
            }
            if (t->state == TState::Dormant) {
                coro_destroy(t);
                if (t->pending_delete) {
                    rt_log_info("sched", "thread %d exited and deleted", id);
                    t->state = TState::Free;
                } else {
                    rt_log_info("sched", "thread %d is dormant", id);
                }
            }
            continue;
        }
        /* Idle: no runnable thread. Jump to the next timeline event. */
        if (!idling) {
            idling = true;
            idle_streak_start = g_vclk;
        }
        if (g_vclk - idle_streak_start > 5 * RT_BUSCLK_HZ) {
            rt_sched_dump_inventory("all threads blocked for 5 s of virtual time");
            rt_fatal("sched", nullptr, "deadlock: no thread became ready for 5 s of virtual time "
                     "(vclk=%" PRIu64 ", vblank #%" PRIu64 ")", g_vclk, g_vblank_count);
        }
        uint64_t nxt = clock_next_event();
        if (nxt == UINT64_MAX) {
            rt_sched_dump_inventory("no runnable threads and no pending timeline events");
            rt_fatal("sched", nullptr, "deadlock: nothing to run and nothing scheduled");
        }
        g_cyc_idle += nxt - g_vclk;
        rt_clock_tick(nxt - g_vclk);
        rt_intc_deliver();
    }
}

[[noreturn]] void rt_sched_boot(uint32_t entry_vram, uint32_t gp, uint32_t sp) {
    /* The thread that owns g_threads, g_semas and the waiter deques from
     * here on. Anything that wants to walk them from another thread (the
     * end-of-run summary raised by a fatal on the GS worker, for instance)
     * has to ask first: see rt_sched_on_ee_thread. */
    g_ee_thread = std::this_thread::get_id();
    g_ee_thread_set = true;
    EEThread* t = &g_threads[1];
    t->id = 1;
    t->state = TState::Dormant;
    t->entry = entry_vram;
    t->priority = t->init_priority = 0;
    t->gp = gp;
    /* Provisional stack until crt0's RFU060 declares the real one; matches
     * the P1 boot ($sp = top of RAM minus 64 KB). */
    t->stack_base = sp - 0x10000;
    t->stack_size = 0x10000;
    coro_start(t, 0);
    ready_push_back(t);
    rt_log_info("sched", "boot: thread 1 entry=0x%08x prio=0 sp=0x%08x gp=0x%08x", entry_vram, sp, gp);
    sched_loop();
}

/* How many of this thread's recorded wakes happened in the last second of
 * virtual time, and whether the ring could hold them all. */
size_t wakes_last_second(const EEThread* t, bool* capped) {
    const uint64_t cutoff = g_vclk > RT_BUSCLK_HZ ? g_vclk - RT_BUSCLK_HZ : 0;
    const size_t held = t->wakes < kWakeRing ? (size_t)t->wakes : kWakeRing;
    size_t n = 0;
    for (size_t i = 0; i < held; ++i) {
        if (t->wake_at[i] >= cutoff) ++n;
    }
    *capped = n == kWakeRing;
    return n;
}

bool rt_sched_on_ee_thread() {
    return g_ee_thread_set && std::this_thread::get_id() == g_ee_thread;
}

void rt_sched_dump_inventory(const char* why) {
    /* The tables below are the EE thread's, and nothing locks them. Walking
     * a std::deque another thread is editing can fault, and faulting inside
     * a crash report is worse than not printing one, so the inventory is
     * skipped rather than raced when the caller is on another thread (a
     * fatal raised on the GS worker reaches rt_run_summary the same way the
     * EE thread does). Before the scheduler boots there is no owner and
     * nothing to race with, which is why the unset case prints. */
    if (g_ee_thread_set && !rt_sched_on_ee_thread()) {
        rt_log_warn("sched", "thread/semaphore inventory (%s) SKIPPED: this is not the EE thread, "
            "and its tables are edited without a lock. Whatever raised this is on another thread",
            why);
        return;
    }
    rt_log_info("sched", "---- thread/semaphore inventory (%s) ----", why);
    rt_log_info("sched", "vclk=%" PRIu64 " cycles (%.3f s), vblank fields=%" PRIu64 ", thread resumes=%" PRIu64,
        g_vclk, (double)g_vclk / (double)RT_BUSCLK_HZ, g_vblank_count, g_resumes);
    for (int i = 1; i < kMaxThreads; ++i) {
        EEThread* t = &g_threads[i];
        if (t->state == TState::Free) continue;
        /* What it is waiting on, with enough of the other side of the wait
         * to say whether it can ever end: a semaphore's count and its
         * waiter list, or how long a sleeping thread has been asleep. A
         * bare "wait=sema 5" cannot distinguish a semaphore nobody signals
         * from one signalled every field. */
        char waitinfo[192] = "";
        if (t->wait == WaitKind::Sleep) {
            std::snprintf(waitinfo, sizeof(waitinfo),
                " wait=SleepThread (wakeups pending=%d, asleep %.3f s)",
                t->wakeup_count, (double)(g_vclk - t->blocked_since) / (double)RT_BUSCLK_HZ);
        } else if (t->wait == WaitKind::Sema) {
            EESema* s = sget(t->wait_sema);
            char wbuf[64] = "";
            size_t off = 0;
            if (s) {
                for (int w : s->waiters) {
                    off += (size_t)std::snprintf(wbuf + off, sizeof(wbuf) - off, "%s%d", off ? "," : "", w);
                    if (off >= sizeof(wbuf) - 8) break;
                }
            }
            std::snprintf(waitinfo, sizeof(waitinfo),
                " wait=sema %d (count=%d max=%d signals=%" PRIu64 " waiters=[%s], waiting %.3f s)",
                t->wait_sema, s ? s->count : -1, s ? s->max_count : -1,
                s ? s->signals : 0, wbuf,
                (double)(g_vclk - t->blocked_since) / (double)RT_BUSCLK_HZ);
        }
        /* The entry address as a decomp name. Every ios thread in this game
         * is created with the same entry (iosThreadMain), so the name alone
         * does not separate them; it is still the difference between
         * reading a thread list and reading twelve addresses. */
        uint32_t fn_entry = 0;
        const char* fn = rt_guest_func_name(t->entry, &fn_entry);
        char entryname[96] = "";
        if (fn) {
            if (fn_entry == t->entry) std::snprintf(entryname, sizeof(entryname), " (%s)", fn);
            else std::snprintf(entryname, sizeof(entryname), " (%s+0x%x)", fn, t->entry - fn_entry);
        }
        bool capped = false;
        const size_t win = wakes_last_second(t, &capped);
        rt_log_info("sched", "  thread %-3d %-11s prio=%-3d entry=0x%08x%s stack=0x%08x+0x%x gp=0x%08x "
            "runs=%" PRIu64 " wakes=%" PRIu64 " (%zu%s in the last second) last syscall=%s%s",
            t->id, tstate_name(t->state), t->priority, t->entry, entryname,
            t->stack_base, t->stack_size, t->gp, t->run_count, t->wakes, win, capped ? "+" : "",
            t->last_syscall_name ? t->last_syscall_name : "(none yet)", waitinfo);
    }
    for (int i = 1; i < kMaxSemas; ++i) {
        EESema* s = &g_semas[i];
        if (!s->alive) continue;
        char wbuf[128] = "";
        size_t off = 0;
        for (int w : s->waiters) {
            off += std::snprintf(wbuf + off, sizeof(wbuf) - off, "%s%d", off ? "," : "", w);
            if (off >= sizeof(wbuf) - 8) break;
        }
        rt_log_info("sched", "  sema %-3d count=%-3d max=%-3d init=%-3d signals=%" PRIu64 " waits=%" PRIu64
            " refused=%" PRIu64 " creator=thread %d (ra 0x%08x) waiters=[%s]",
            s->id, s->count, s->max_count, s->init_count, s->signals, s->waits, s->refused,
            s->creator_tid, s->creator_ra, wbuf);
    }
    rt_alarms_dump();
    rt_sif_dump_inventory();
    rt_log_info("sched", "---- end inventory ----");
}

/* Recorded per thread by syscalls.cpp, next to the run-state summary's
 * whole-run version. `name` is a dispatch-table entry with static
 * lifetime. */
void rt_sched_note_syscall(int num, const char* name) {
    EEThread* t = tget(g_current);
    if (!t) return;
    t->last_syscall_num = num;
    t->last_syscall_name = name;
    t->last_syscall_vclk = g_vclk;
}

[[noreturn]] void rt_sched_exit_game(int code, const char* why) {
    /* warn unless something nearer the cause already said it was
     * deliberate (a bounded diagnostic run reaching its bound). The guest
     * calling Exit is not the player quitting: the player quits through the
     * menu, the launcher or the window's close button, and every one of
     * those comes through rt_request_exit instead. Translated code reaching
     * Exit on its own is the game deciding to stop, which on this port is
     * more often a boot that went wrong than a feature, and it is one of the
     * ways a run can "just end" with nothing in the log to say why. */
    const bool expected = rt_run_exit_reason_known();
    if (expected) {
        rt_log_info("sched", "exiting: %s (exit status %d)", why, code);
    } else {
        rt_log_warn("sched", "the guest ended the run itself (Exit): %s (exit status %d)",
            why, code);
    }
    rt_sched_dump_inventory("exit");
    /* First caller wins, so this is only reached when nothing nearer the
     * cause named itself: an Exit the run did not ask for. */
    rt_run_set_exit_reason(false, "the guest called Exit: %s (exit status %d)", why, code);
    std::exit(code);
}

/* ---- thread syscall backends -------------------------------------------- */

int rt_thread_create(uint32_t entry, uint32_t stack, uint32_t stack_size,
                     uint32_t gp, int prio, uint32_t attr, uint32_t option) {
    /* Both failures below hand the guest a bare -1. Warn once per
     * condition, naming the limit and what it means, because the per-call
     * CreateThread line in syscalls.cpp stops after the first 32 and a
     * failure after boot would otherwise leave nothing in the log. */
    if (prio < 0 || prio >= kNumPrios) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            rt_log_warn("sched", "CreateThread(entry=0x%08x) asked for priority %d, outside the "
                "kernel's 0..%d; returning -1 the way the kernel does. The guest gets no thread",
                entry, prio, kNumPrios - 1);
        }
        return -1;
    }
    for (int i = 2; i < kMaxThreads; ++i) {
        if (g_threads[i].state == TState::Free) {
            EEThread* t = &g_threads[i];
            *t = EEThread{};
            t->id = i;
            t->state = TState::Dormant;
            t->entry = entry;
            t->stack_base = stack;
            t->stack_size = stack_size;
            t->gp = gp;
            t->priority = t->init_priority = prio;
            t->attr = attr;
            t->option = option;
            return i;
        }
    }
    {
        static bool warned = false;
        if (!warned) {
            warned = true;
            rt_log_warn("sched", "CreateThread(entry=0x%08x prio=%d) with all %d thread slots in "
                "use; returning -1 the way the kernel does when its table is full. Whatever the "
                "guest wanted this thread for will not run",
                entry, prio, kMaxThreads - 2);
        }
    }
    return -1;
}

int rt_thread_delete(int id) {
    EEThread* t = tget(id);
    if (!t || t->state != TState::Dormant || id == g_current) return -1;
    coro_destroy(t);
    t->state = TState::Free;
    return 0;
}

int rt_thread_start(int id, uint32_t arg) {
    EEThread* t = tget(id);
    if (!t || t->state != TState::Dormant) return -1;
    coro_start(t, arg);
    ready_push_back(t);
    rt_sched_maybe_preempt();
    return id;
}

void rt_thread_exit_current(bool and_delete) {
    EEThread* t = tget(g_current);
    if (!t) rt_fatal("sched", nullptr, "ExitThread with no current thread");
    t->state = TState::Dormant;
    t->pending_delete = and_delete;
    yield_current();
    rt_fatal("sched", &t->ctx, "dormant thread %d was resumed without StartThread", t->id);
}

int rt_thread_terminate(int id) {
    EEThread* t = tget(id);
    if (!t || id == g_current || t->state == TState::Dormant) return -1;
    if (t->state == TState::Ready) ready_remove(t);
    if (t->wait == WaitKind::Sema) {
        EESema* s = sget(t->wait_sema);
        if (s) {
            for (auto it = s->waiters.begin(); it != s->waiters.end(); ++it) {
                if (*it == id) { s->waiters.erase(it); break; }
            }
        }
    }
    t->wait = WaitKind::None;
    t->state = TState::Dormant;
    coro_destroy(t);
    return id;
}

int rt_thread_change_priority(int id, int prio, bool from_int) {
    EEThread* t = tresolve(id);
    if (!t || prio < 0 || prio >= kNumPrios) return -1;
    int old = t->priority;
    if (t->id == g_current && !from_int) {
        /* The running thread goes to the TAIL of the new priority's ready
         * queue, i.e. this doubles as a yield. INFERRED, not measured: no
         * disassembly of the retail kernel's ChangeThreadPriority has been
         * read for this port, and tail is the reading that matches the
         * kernel's other requeueing call, RotateThreadReadyQueue. If it is
         * head, a same-priority sibling that was already queued would run
         * one turn later than it does here. */
        t->priority = prio;
        ready_push_back(t);
        yield_current();
    } else if (t->state == TState::Ready) {
        ready_remove(t);
        t->priority = prio;
        ready_push_back(t);
    } else {
        t->priority = prio;
    }
    /* Raising another thread's priority above the running thread's makes it
     * the one that should be on the CPU, and the kernel switches to it at
     * once. Every other call that can make a higher-priority thread ready
     * (rt_thread_start, rt_thread_wakeup, rt_sema_signal, rt_thread_resume)
     * ends this way; this one used to be the exception, which left the
     * caller running until it blocked. Not from an interrupt handler: there
     * the reschedule happens on the way out of rt_intc_deliver. */
    if (t->id != g_current && !from_int) rt_sched_maybe_preempt();
    return old;
}

int rt_thread_rotate_ready_queue(int prio, bool from_int) {
    if (prio < 0 || prio >= kNumPrios) return -1;
    EEThread* c = tget(g_current);
    if (!from_int && c && c->priority == prio) {
        /* The running thread counts as the head of its priority: rotating it
         * yields to the next same-priority thread. */
        ready_push_back(c);
        yield_current();
    } else if (!g_ready[prio].empty()) {
        g_ready[prio].push_back(g_ready[prio].front());
        g_ready[prio].pop_front();
    }
    return prio;
}

int rt_thread_getid() { return g_current; }

int rt_thread_refer(int id, uint32_t out) {
    EEThread* t = tresolve(id);
    if (!t) return -1;
    uint32_t status;
    switch (t->state) {
        case TState::Run: status = THS_RUN; break;
        case TState::Ready: status = THS_READY; break;
        case TState::Wait: status = THS_WAIT; break;
        case TState::Suspend: status = THS_SUSPEND; break;
        case TState::WaitSuspend: status = THS_WAIT | THS_SUSPEND; break;
        default: status = THS_DORMANT; break;
    }
    /* Sony ThreadParam layout (ps2sdk kernel.h ee_thread_status_t):
     * status, func, stack, stack_size, gp_reg, initial_priority,
     * current_priority, attr, option, waitType, waitId, wakeupCount. */
    rt_gwrite32(out + 0, status);
    rt_gwrite32(out + 4, t->entry);
    rt_gwrite32(out + 8, t->stack_base);
    rt_gwrite32(out + 12, t->stack_size);
    rt_gwrite32(out + 16, t->gp);
    rt_gwrite32(out + 20, (uint32_t)t->init_priority);
    rt_gwrite32(out + 24, (uint32_t)t->priority);
    rt_gwrite32(out + 28, t->attr);
    rt_gwrite32(out + 32, t->option);
    rt_gwrite32(out + 36, t->wait == WaitKind::Sleep ? 1u : (t->wait == WaitKind::Sema ? 2u : 0u));
    rt_gwrite32(out + 40, t->wait == WaitKind::Sema ? (uint32_t)t->wait_sema : 0u);
    rt_gwrite32(out + 44, (uint32_t)t->wakeup_count);
    return t->id;
}

int rt_thread_sleep() {
    EEThread* t = tget(g_current);
    if (!t) return -1;
    if (t->wakeup_count > 0) {
        --t->wakeup_count;
        return 0;
    }
    return block_current(WaitKind::Sleep, 0);
}

int rt_thread_wakeup(int id, bool from_int) {
    EEThread* t = tresolve(id);
    if (!t) return -1;
    if ((t->state == TState::Wait || t->state == TState::WaitSuspend) && t->wait == WaitKind::Sleep) {
        unblock(t, 0);
        if (!from_int) rt_sched_maybe_preempt();
    } else {
        ++t->wakeup_count;
    }
    return id;
}

int rt_thread_cancel_wakeup(int id) {
    EEThread* t = tresolve(id);
    if (!t) return -1;
    int old = t->wakeup_count;
    t->wakeup_count = 0;
    return old;
}

int rt_thread_suspend(int id) {
    EEThread* t = tresolve(id);
    if (!t) return -1;
    switch (t->state) {
        case TState::Run:
            t->state = TState::Suspend;
            if (t->id == g_current) yield_current();
            return id;
        case TState::Ready:
            ready_remove(t);
            t->state = TState::Suspend;
            return id;
        case TState::Wait:
            t->state = TState::WaitSuspend;
            return id;
        default:
            return -1;
    }
}

int rt_thread_resume(int id) {
    EEThread* t = tresolve(id);
    if (!t) return -1;
    if (t->state == TState::Suspend) {
        ready_push_back(t);
        rt_sched_maybe_preempt();
        return id;
    }
    if (t->state == TState::WaitSuspend) {
        t->state = TState::Wait;
        return id;
    }
    return -1;
}

void rt_thread_setup_main(uint32_t gp, uint32_t stack_base, uint32_t stack_size) {
    EEThread* t = &g_threads[1];
    t->gp = gp;
    t->stack_base = stack_base;
    t->stack_size = stack_size;
}

int rt_thread_release_wait(int id, bool from_int) {
    EEThread* t = tget(id);
    if (!t || (t->state != TState::Wait && t->state != TState::WaitSuspend)) return -1;
    if (t->wait == WaitKind::Sema) {
        EESema* s = sget(t->wait_sema);
        if (s) {
            for (auto it = s->waiters.begin(); it != s->waiters.end(); ++it) {
                if (*it == id) { s->waiters.erase(it); break; }
            }
        }
    }
    unblock(t, -1 /* released: blocking call fails */);
    if (!from_int) rt_sched_maybe_preempt();
    return id;
}

/* ---- semaphore syscall backends ----------------------------------------- */

/* Kernel error codes for the two semaphore refusals.
 *
 * Source: ps2sdk iop/kernel/include/kerr.h, which lists
 *   #define KE_SEMA_ZERO -419   (PollSema/WaitSema on a zero count)
 *   #define KE_SEMA_OVF  -420   (SignalSema past max_count)
 * next to each other. That header is the IOP kernel's; the EE kernel using
 * the same numbering for the same two conditions is INFERRED, not measured
 * (ps2sdk's ee/kernel headers declare the syscalls but define no error
 * enum, and there is no ps2sdk tree on this machine to check an EE-side
 * one against). Both are negative, which is what every caller in this
 * binary tests, so the inference decides the exact value and nothing else.
 * KE_SEMA_ZERO was -420 here until 2026-09-05, which is KE_SEMA_OVF's
 * value in that header. */
constexpr int KE_SEMA_ZERO = -419;
constexpr int KE_SEMA_OVF = -420;

int rt_sema_create(int init_count, int max_count, uint32_t attr, uint32_t option) {
    for (int i = 1; i < kMaxSemas; ++i) {
        if (!g_semas[i].alive) {
            EESema* s = &g_semas[i];
            *s = EESema{};
            s->id = i;
            s->alive = true;
            s->count = init_count;
            s->init_count = init_count;
            s->max_count = max_count;
            s->attr = attr;
            s->option = option;
            s->creator_tid = g_current;
            R5900Context* c = rt_sched_current_ctx();
            s->creator_ra = c ? (uint32_t)c->r[31].u64x[0] : 0;
            return i;
        }
    }
    {
        static bool warned = false;
        if (!warned) {
            warned = true;
            rt_log_warn("sched", "CreateSema(init=%d max=%d) with all %d semaphore slots in use; "
                "returning -1 the way the kernel does when its table is full. Whatever was going "
                "to wait on this semaphore will not block, and whatever was going to signal it "
                "will fail",
                init_count, max_count, kMaxSemas - 1);
        }
    }
    return -1;
}

int rt_sema_delete(int id) {
    EESema* s = sget(id);
    if (!s) return -1;
    while (!s->waiters.empty()) {
        int tid = s->waiters.front();
        s->waiters.pop_front();
        EEThread* t = tget(tid);
        if (t) unblock(t, -1 /* wait failed: sema deleted */);
    }
    s->alive = false;
    rt_sched_maybe_preempt();
    return id;
}

int rt_sema_signal(int id, bool from_int) {
    EESema* s = sget(id);
    if (!s) return -1;
    /* max_count is a ceiling, not a comment: the kernel refuses a signal
     * that would push the count past it (KE_SEMA_OVF above) instead of
     * accumulating. Only the no-waiter arm can overflow; a signal handed
     * straight to a waiting thread never touches the count.
     *
     * What this changes, and what it does not. Measured on the PAL boot
     * (dist/logs/handoff-2026-09-04/icorecomp-latest.log, the "sema"
     * inventory): the main per-field semaphore the vblank handler signals
     * is created with max=8, and the rest of the table is max 1, 2, 8, 16
     * or 255. Nothing is created with max 0, so nothing is refused from
     * its first signal. With max=8 an EE thread that overruns a field
     * still banks the missed signals and still runs its catch-up fields
     * exactly as before; what is new is that the count cannot climb past
     * 8, which on hardware is where the kernel stops counting too. A
     * ceiling of 1 elsewhere in the table now drops a repeat signal the
     * way the kernel does. */
    if (s->waiters.empty() && s->count >= s->max_count) {
        ++s->refused;
        if (!s->refused_warned) {
            s->refused_warned = true;
            rt_log_warn("sched", "SignalSema(%d) refused: count is already %d and the semaphore "
                "was created with max_count %d, so the kernel answers KE_SEMA_OVF (%d) and the "
                "signal is dropped rather than banked (creator thread %d, ra 0x%08x). Further "
                "refusals on this semaphore are counted in the inventory, not logged",
                id, s->count, s->max_count, KE_SEMA_OVF, s->creator_tid, s->creator_ra);
        }
        return KE_SEMA_OVF;
    }
    ++s->signals;
    if (!s->waiters.empty()) {
        int tid = s->waiters.front();
        s->waiters.pop_front();
        EEThread* t = tget(tid);
        if (t) unblock(t, id /* WaitSema returns the sema id */);
        if (!from_int) rt_sched_maybe_preempt();
    } else {
        ++s->count;
    }
    return id;
}

int rt_sema_wait(int id) {
    EESema* s = sget(id);
    if (!s) return -1;
    ++s->waits;
    if (s->count > 0) {
        --s->count;
        return id;
    }
    s->waiters.push_back(g_current);
    return block_current(WaitKind::Sema, id);
}

int rt_sema_poll(int id) {
    EESema* s = sget(id);
    if (!s) return -1;
    if (s->count > 0) {
        --s->count;
        return id;
    }
    return KE_SEMA_ZERO;
}

int rt_sema_refer(int id, uint32_t out) {
    EESema* s = sget(id);
    if (!s) return -1;
    /* Sony SemaParam layout (ps2sdk kernel.h ee_sema_t):
     * count, max_count, init_count, wait_threads, attr, option. */
    rt_gwrite32(out + 0, (uint32_t)s->count);
    rt_gwrite32(out + 4, (uint32_t)s->max_count);
    rt_gwrite32(out + 8, (uint32_t)s->init_count);
    rt_gwrite32(out + 12, (uint32_t)s->waiters.size());
    rt_gwrite32(out + 16, s->attr);
    rt_gwrite32(out + 20, s->option);
    return id;
}
