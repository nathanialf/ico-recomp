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
 * boundaries (59.94 Hz NTSC, alternating fields), timer compare/overflow
 * interrupts (timers.cpp) and deferred SIF responses (sif/sif.cpp).
 */
#include "kernel.h"

#define MINICORO_IMPL
#include "minicoro.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <deque>
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

enum class TState : uint8_t { Free, Dormant, Ready, Run, Wait, Suspend, WaitSuspend };
enum class WaitKind : uint8_t { None, Sleep, Sema };

/* Sony ThreadParam.status values (ps2sdk kernel.h THS_*). */
constexpr uint32_t THS_RUN = 0x01, THS_READY = 0x02, THS_WAIT = 0x04,
                   THS_SUSPEND = 0x08, THS_DORMANT = 0x10;

struct EEThread {
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
    std::deque<int> waiters;        /* FIFO thread ids */
};

EEThread g_threads[kMaxThreads];    /* index == id; 0 unused */
EESema g_semas[kMaxSemas];          /* index == id; 0 unused */
std::deque<int> g_ready[kNumPrios];
int g_current = 0;                  /* running thread id, 0 = scheduler */
uint64_t g_resumes = 0;             /* total thread resumes, for hang detection */

/* ---- virtual clock + vblank timeline ---- */

uint64_t g_vclk = 0;
uint64_t g_next_field_edge = RT_CYCLES_PER_FIELD; /* next vblank START */
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

/* Make a blocked/suspended thread runnable again. */
void unblock(EEThread* t, int release_ret) {
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
    rt_log("sched", "thread %d root function returned; implicit ExitThread", t->id);
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
    return nxt;
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
            g_next_vblank_end = g_next_field_edge + RT_CYCLES_VBLANK;
            g_next_field_edge += RT_CYCLES_PER_FIELD;
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
                rt_log("vblank", "field #%" PRIu64 " start (vclk=%" PRIu64 ")", g_vblank_count, g_vclk);
            }
            if (g_max_vblanks && g_vblank_count >= g_max_vblanks) {
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
    }
    g_vclk = target;
}

void rt_kernel_mmio_tick() {
    /* ~3.5 us per polled MMIO access: a tight guest poll loop crosses a
     * 16.7 ms field boundary in a few thousand iterations. */
    rt_clock_tick(512);
    rt_intc_deliver();
}

/* ---- scheduler core ------------------------------------------------------ */

void rt_sched_init() {
    const char* e = std::getenv("ICORECOMP_MAX_VBLANKS");
    if (e) g_max_vblanks = std::strtoull(e, nullptr, 10);
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
            mco_result r = mco_resume(t->co);
            g_current = 0;
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
                    rt_log("sched", "thread %d exited and deleted", id);
                    t->state = TState::Free;
                } else {
                    rt_log("sched", "thread %d is dormant", id);
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
        rt_clock_tick(nxt - g_vclk);
        rt_intc_deliver();
    }
}

[[noreturn]] void rt_sched_boot(uint32_t entry_vram, uint32_t gp, uint32_t sp) {
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
    rt_log("sched", "boot: thread 1 entry=0x%08x prio=0 sp=0x%08x gp=0x%08x", entry_vram, sp, gp);
    sched_loop();
}

void rt_sched_dump_inventory(const char* why) {
    rt_log("sched", "---- thread/semaphore inventory (%s) ----", why);
    rt_log("sched", "vclk=%" PRIu64 " cycles (%.3f s), vblank fields=%" PRIu64 ", thread resumes=%" PRIu64,
        g_vclk, (double)g_vclk / (double)RT_BUSCLK_HZ, g_vblank_count, g_resumes);
    for (int i = 1; i < kMaxThreads; ++i) {
        EEThread* t = &g_threads[i];
        if (t->state == TState::Free) continue;
        char waitinfo[64] = "";
        if (t->wait == WaitKind::Sleep) std::snprintf(waitinfo, sizeof(waitinfo), " wait=SleepThread");
        if (t->wait == WaitKind::Sema) std::snprintf(waitinfo, sizeof(waitinfo), " wait=sema %d", t->wait_sema);
        rt_log("sched", "  thread %-3d %-11s prio=%-3d entry=0x%08x stack=0x%08x+0x%x gp=0x%08x runs=%" PRIu64 "%s",
            t->id, tstate_name(t->state), t->priority, t->entry, t->stack_base, t->stack_size,
            t->gp, t->run_count, waitinfo);
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
        rt_log("sched", "  sema %-3d count=%-3d max=%-3d init=%-3d signals=%" PRIu64 " waits=%" PRIu64
            " creator=thread %d (ra 0x%08x) waiters=[%s]",
            s->id, s->count, s->max_count, s->init_count, s->signals, s->waits,
            s->creator_tid, s->creator_ra, wbuf);
    }
    rt_sif_dump_inventory();
    rt_log("sched", "---- end inventory ----");
}

[[noreturn]] void rt_sched_exit_game(int code, const char* why) {
    rt_log("sched", "exiting: %s", why);
    rt_sched_dump_inventory("exit");
    std::exit(code);
}

/* ---- thread syscall backends -------------------------------------------- */

int rt_thread_create(uint32_t entry, uint32_t stack, uint32_t stack_size,
                     uint32_t gp, int prio, uint32_t attr, uint32_t option) {
    if (prio < 0 || prio >= kNumPrios) return -1;
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
        /* Kernel semantics: the running thread goes to the TAIL of the new
         * priority's ready queue, i.e. this doubles as a yield. */
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

/* Kernel error code for PollSema on a zero-count semaphore (Sony KE_SEMA_ZERO). */
constexpr int KE_SEMA_ZERO = -420;

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
