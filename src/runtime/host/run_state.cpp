/* host/run_state.cpp: the phase state machine, the end-of-run summary and
 * the field watchdog.
 *
 * Why this file exists
 * --------------------
 * A run on display.backend = d3d12 ended with an error on screen and a log
 * whose last line was an ordinary HLE warning. Nothing in the file said what
 * ended the run, at the default level or any other. The rule that follows is
 * absolute: the log always says how a run ended and why, whichever of the
 * many exits the process took.
 *
 * There are more exits from this process than there are functions that know
 * about all of them: main returning, rt_fatal, the SEH filter, std::terminate,
 * the POSIX signal handlers, abort, the window-closed path in the vsync hook,
 * the GS ring's own std::exit calls, the restart path, and the atexit chain.
 * So the summary is written by whichever of them gets here first and by an
 * atexit handler that catches the rest, and it is idempotent, so no caller
 * has to know whether another one already ran.
 *
 * Push, not pull
 * --------------
 * Everything the summary reports is stored here by its producer as a plain
 * relaxed atomic. The alternative, asking each subsystem for its state when
 * the summary runs, calls into subsystems on a path where one of them is
 * very likely to be the thing that is stuck, and takes their locks on a
 * thread that may hold a lock they want. The cost of pushing is one relaxed
 * store per event at sites that already do far more work than that.
 *
 * The strings stored are pointers, not copies: every caller passes a string
 * literal or a table entry with static lifetime. That is what makes a store
 * from the GS worker and a load from the watchdog thread safe with no lock.
 *
 * The watchdog
 * ------------
 * Its own thread, because the thread it watches is the one that can be
 * stuck. It never ends the process: a run that is only very slow must not
 * be killed by its own instrument, and a hang that is reported is already
 * diagnosable.
 *
 * Linkage: this file calls nothing but runtime.h's log entry points, so the
 * log selftest can link it beside log.cpp with no stubs.
 */
#include "runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

namespace {

using RunClock = std::chrono::steady_clock;

/* ---- the phase ---------------------------------------------------------- */

std::atomic<int> g_phase{(int)RT_PHASE_START};

const char* const kPhaseNames[RT_PHASE_COUNT] = {
    "start",
    "log init",
    "settings",
    "backend created",
    "window created",
    "launcher shown",
    "guest booted",
    "first field",
    "first present",
    "gameplay",
};

/* ---- counters ----------------------------------------------------------- */

std::atomic<uint64_t> g_fields{0};
std::atomic<uint64_t> g_presents{0};
std::atomic<uint64_t> g_gif_packets{0};

std::atomic<const char*> g_last_syscall{nullptr};
std::atomic<int> g_last_syscall_num{0};
std::atomic<const char*> g_last_rpc{nullptr};
std::atomic<uint32_t> g_last_rpc_fno{0};

std::atomic<const char*> g_gs_record{nullptr};
std::atomic<const char*> g_gs_worker{nullptr};
std::atomic<uint64_t> g_gs_queued{0};
std::atomic<uint64_t> g_gs_replayed{0};
std::atomic<const char*> g_rhi_state{nullptr};
/* -1 until the window service has sampled the window once. A run with no
 * window never moves it, and the summary says "no window" rather than
 * claiming the window was not minimised. */
std::atomic<int> g_minimized{-1};

/* When the process started, as close to it as this file can get: the first
 * call into any entry point here, which main makes right after rt_log_init.
 * A run that never calls one reports its wall time from the first phase
 * transition instead, which is still bounded by the run. */
std::once_flag g_start_once;
RunClock::time_point g_start;
std::atomic<bool> g_started{false};

/* ---- the exit reason ---------------------------------------------------- */

std::mutex g_reason_mu;
char g_reason[512] = {0};
bool g_reason_set = false;
bool g_user_quit = false;

std::atomic<bool> g_summary_done{false};

/* The pending inventory request (a string literal, so a pointer is the whole
 * value) and the summary-path hook. Both are plain atomics for the same
 * reason as everything else in this file: the producer is the watchdog
 * thread and the consumer is the EE thread, and neither may take a lock the
 * other might be holding. */
std::atomic<const char*> g_inventory_why{nullptr};
std::atomic<void (*)(const char*)> g_inventory_hook{nullptr};

const char* text_or(const std::atomic<const char*>& a, const char* fallback) {
    const char* p = a.load(std::memory_order_relaxed);
    return p ? p : fallback;
}

/* ---- the watchdog ------------------------------------------------------- */

std::atomic<uint64_t> g_last_field_ms{0};   /* ms since g_start of the last field */
std::thread g_watchdog;
std::mutex g_watchdog_mu;
std::condition_variable g_watchdog_cv;
bool g_watchdog_stop = false;
std::atomic<bool> g_watchdog_running{false};

/* The first stall report comes after five seconds without a field and the
 * next after thirty, which is the difference between "say it once, loudly"
 * and "fill the file". Both are wall-clock times, not field counts: a hang
 * is exactly the case where no field arrives to count. */
constexpr uint64_t kStallFirstMs = 5000;
constexpr uint64_t kStallRepeatMs = 30000;
/* The poll interval. Short enough that the five-second threshold is
 * reported within half a second of being crossed, long enough that a
 * sleeping thread costs nothing. */
constexpr auto kWatchdogStep = std::chrono::milliseconds(500);

/* The guest-loop check: fields keep arriving but nothing draws. Ten seconds
 * rather than five, because a legitimately blank stretch (a load, a fade)
 * lasts seconds and a false alarm here would train a reader to ignore the
 * line. */
constexpr uint64_t kNoGifMs = 10000;

uint64_t now_ms() {
    if (!g_started.load(std::memory_order_acquire)) return 0;
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        RunClock::now() - g_start).count();
}

void start_clock() {
    std::call_once(g_start_once, [] {
        g_start = RunClock::now();
        g_started.store(true, std::memory_order_release);
        /* Registered on the first call into this file, which main makes
         * straight after rt_log_init. Handlers run in reverse registration
         * order, so this one runs after everything registered later: the
         * audio device's closing counters, the GS backend teardown and its
         * statistics, the achievement store. The summary is therefore the
         * last block in the file, with every subsystem's closing lines
         * above it, which is what it is for.
         *
         * It may or may not run before log.cpp's own handler, and it does
         * not need to. log.cpp registers its handler from inside the first
         * enqueue that starts the writer thread, and rt_log_init writes its
         * prologue with the writer deliberately parked, so in practice the
         * first line that starts the writer is the phase line this very
         * function is about to log, and log.cpp's handler ends up
         * registered second and therefore runs first. That is harmless:
         * once it has joined the writer, logging is back on the calling
         * thread, and every line the summary emits after that is written
         * and flushed before its own call returns. Either order puts the
         * whole block in the file. */
        std::atexit([] {
            rt_run_summary();
        });
    });
}

void watchdog_main();

/* One summary line at a level chosen at run time. A plain function and not
 * a lambda: va_start's named argument has to be an ordinary parameter for
 * every compiler this builds under. */
RT_PRINTF_FORMAT(2, 3)
void summary_line(RtLogLevel level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlog(level, "run", fmt, ap);
    va_end(ap);
}

/* One line naming everything a stalled or crashed run needs placing. Shared
 * by the watchdog and the summary so the two never drift apart. */
void format_state(char* buf, size_t len) {
    const int minimized = g_minimized.load(std::memory_order_relaxed);
    std::snprintf(buf, len,
        "phase=%s fields=%llu presents=%llu gif packets=%llu; GS worker %s, last record %s, "
        "%llu bytes queued, %llu records replayed; RHI %s; last syscall %s(%d); last RPC %s "
        "fno=0x%x; window %s",
        rt_run_phase_name(rt_run_phase_now()),
        (unsigned long long)g_fields.load(std::memory_order_relaxed),
        (unsigned long long)g_presents.load(std::memory_order_relaxed),
        (unsigned long long)g_gif_packets.load(std::memory_order_relaxed),
        text_or(g_gs_worker, "no worker thread"),
        text_or(g_gs_record, "none"),
        (unsigned long long)g_gs_queued.load(std::memory_order_relaxed),
        (unsigned long long)g_gs_replayed.load(std::memory_order_relaxed),
        text_or(g_rhi_state, "nothing submitted"),
        text_or(g_last_syscall, "none"), g_last_syscall_num.load(std::memory_order_relaxed),
        text_or(g_last_rpc, "none"), g_last_rpc_fno.load(std::memory_order_relaxed),
        minimized < 0 ? "not sampled (no window, or none opened yet)"
                      : (minimized ? "minimised" : "on screen"));
}

} // namespace

/* ---- thread names -------------------------------------------------------- */

namespace {
thread_local const char* t_thread_name = nullptr;
}

void rt_thread_set_name(const char* name) { t_thread_name = name; }

const char* rt_thread_name() {
    if (t_thread_name) return t_thread_name;
    /* The thread that opened the log is the process's main thread and the EE
     * thread both. Naming it from here rather than requiring a call keeps
     * the common case correct without a registration nobody would remember
     * to make. */
    return rt_log_on_main_thread() ? "EE (main)" : "unnamed";
}

/* ---- the phase ----------------------------------------------------------- */

const char* rt_run_phase_name(RtRunPhase p) {
    if ((int)p < 0 || (int)p >= RT_PHASE_COUNT) return "unknown";
    return kPhaseNames[(int)p];
}

RtRunPhase rt_run_phase_now() {
    return (RtRunPhase)g_phase.load(std::memory_order_relaxed);
}

void rt_run_phase(RtRunPhase reached) {
    start_clock();
    if ((int)reached <= (int)RT_PHASE_START || (int)reached >= RT_PHASE_COUNT) return;
    /* Monotonic, and settled with a compare-exchange rather than a load and
     * a store: two threads can reach a phase at once (the GS worker's first
     * present against the EE thread's first field) and a lost update would
     * put the run back a step. */
    int cur = g_phase.load(std::memory_order_relaxed);
    while (cur < (int)reached) {
        if (g_phase.compare_exchange_weak(cur, (int)reached, std::memory_order_relaxed)) {
            rt_log_info("run", "phase: %s (%llu ms into the run)",
                rt_run_phase_name(reached), (unsigned long long)now_ms());
            return;
        }
    }
}

/* ---- the exit reason ------------------------------------------------------ */

void rt_run_set_exit_reason(bool user_quit, const char* fmt, ...) {
    start_clock();
    std::lock_guard<std::mutex> lk(g_reason_mu);
    /* First caller wins. The reason nearest the cause is the one worth
     * keeping: everything downstream of it (the atexit fallback, the
     * window-closed path a fatal passes through on its way out) is a
     * generalisation that would only blur it. */
    if (g_reason_set) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(g_reason, sizeof(g_reason), fmt, ap);
    va_end(ap);
    g_reason_set = true;
    g_user_quit = user_quit;
}

bool rt_run_exit_reason_known() {
    std::lock_guard<std::mutex> lk(g_reason_mu);
    return g_reason_set;
}

void rt_run_reason_text(char* buf, size_t buf_len) {
    if (!buf || buf_len == 0) return;
    std::lock_guard<std::mutex> lk(g_reason_mu);
    std::snprintf(buf, buf_len, "%s", g_reason_set ? g_reason
        : "the process ended without naming a reason");
}

/* ---- counters ------------------------------------------------------------- */

void rt_run_note_field() {
    g_fields.fetch_add(1, std::memory_order_relaxed);
    g_last_field_ms.store(now_ms(), std::memory_order_relaxed);
}

void rt_run_note_present() { g_presents.fetch_add(1, std::memory_order_relaxed); }
void rt_run_note_gif() { g_gif_packets.fetch_add(1, std::memory_order_relaxed); }

void rt_run_note_syscall(int num, const char* name) {
    g_last_syscall.store(name, std::memory_order_relaxed);
    g_last_syscall_num.store(num, std::memory_order_relaxed);
}

/* ---- the guest inventory, on demand --------------------------------------- */

void rt_run_request_inventory(const char* why) {
    /* Last request wins. Two stalls cannot both be interesting at once, and
     * the scheduler picks this up within one pass of its loop. */
    g_inventory_why.store(why, std::memory_order_relaxed);
}

const char* rt_run_take_inventory_request() {
    return g_inventory_why.exchange(nullptr, std::memory_order_relaxed);
}

void rt_run_set_inventory_hook(void (*fn)(const char* why)) {
    g_inventory_hook.store(fn, std::memory_order_relaxed);
}

void rt_run_note_rpc(const char* what, uint32_t fno) {
    g_last_rpc.store(what, std::memory_order_relaxed);
    g_last_rpc_fno.store(fno, std::memory_order_relaxed);
}

void rt_run_note_gs_record(const char* kind) {
    g_gs_record.store(kind, std::memory_order_relaxed);
}

void rt_run_note_gs_worker(const char* state) {
    g_gs_worker.store(state, std::memory_order_relaxed);
}

void rt_run_note_gs_queued(uint64_t bytes, uint64_t records_replayed) {
    g_gs_queued.store(bytes, std::memory_order_relaxed);
    g_gs_replayed.store(records_replayed, std::memory_order_relaxed);
}

void rt_run_note_rhi(const char* state) {
    g_rhi_state.store(state, std::memory_order_relaxed);
}

void rt_run_note_window_minimized(bool minimized) {
    g_minimized.store(minimized ? 1 : 0, std::memory_order_relaxed);
}

/* ---- the summary ---------------------------------------------------------- */

void rt_run_summary() {
    start_clock();
    /* Exactly once, whoever gets here. Exchange rather than a test and a
     * set: two threads can be on their way out at the same moment (a fatal
     * on the GS worker while the EE thread is in the window-closed path),
     * and two copies of this block would be read as two runs. */
    if (g_summary_done.exchange(true, std::memory_order_acq_rel)) return;

    /* The watchdog first, so it cannot put a stall line through the middle
     * of the block below. It is joined, not merely told to stop.
     *
     * Then synchronous from here. The run is over, so there is no frame path
     * left to keep the log writer off, and a summary still sitting in the
     * writer's ring when the process leaves through _Exit is a summary that
     * was never written. rt_log_drain flushes what is queued and puts every
     * later line on this thread, written and flushed before its call
     * returns, which is what the crash handlers and rt_fatal need: three
     * runs on Windows ended with the failure box on screen and nothing at
     * all in the log, which is the failure mode this ordering removes. */
    rt_run_watchdog_stop();
    rt_log_drain();

    char reason[512];
    bool user_quit;
    {
        std::lock_guard<std::mutex> lk(g_reason_mu);
        std::snprintf(reason, sizeof(reason), "%s", g_reason_set ? g_reason
            : "the process exited without naming a reason: main returned, or something"
              " called std::exit from a path that does not report itself");
        user_quit = g_reason_set && g_user_quit;
    }

    const RtLogLevel level = user_quit ? RT_LOG_INFO : RT_LOG_WARN;
    const uint64_t ms = now_ms();

    char state[768];
    format_state(state, sizeof(state));

    /* The inventory first, so the "---- end of run ----" block stays the
     * last thing in the file. Direct rather than deferred: on the ordinary
     * exit path the watchdog is joined and the GS worker is joined by now,
     * so there is no other thread left to be halfway through the
     * scheduler's tables. That is not true of every caller: rt_fatal
     * (log.cpp) and the crash handlers reach this summary on whatever
     * thread faulted. The hook itself makes that check and prints one line
     * instead of walking the tables when it is on the wrong thread
     * (ee/sched.cpp rt_sched_dump_inventory, rt_sched_on_ee_thread), which
     * is where it belongs: this file must not depend on the scheduler.
     * Null in a build that links this file without the scheduler. */
    if (void (*hook)(const char*) = g_inventory_hook.load(std::memory_order_relaxed)) {
        hook("end of run");
    }

    /* Emitted through rt_vlog rather than the four named entry points,
     * because the level is decided at run time here. The "---- " bracketing
     * is what makes the block greppable as one thing in a file a crash may
     * have interleaved with a driver's own output. */
    summary_line(level, "---- end of run ----");
    summary_line(level, "reason: %s", reason);
    summary_line(level, "ended in phase: %s (of %d; the run got this far and no further)",
        rt_run_phase_name(rt_run_phase_now()), (int)RT_PHASE_COUNT - 1);
    summary_line(level, "state: %s", state);
    summary_line(level, "wall time: %llu.%03llu s; ending thread: %s",
        (unsigned long long)(ms / 1000), (unsigned long long)(ms % 1000), rt_thread_name());
    if (const char* path = rt_log_path()) {
        summary_line(level, "log: %s", path);
    } else {
        summary_line(level, "log: this run kept no log file (debug.log_file is off, or no"
             " writable location was found)");
    }
    summary_line(level, "---- end of run ----");

    /* The message box a run with no console shows is built from the failure
     * block, and a warn-level summary does not reach it on its own. The
     * reason and the log path are exactly what that box has to carry, so
     * they are put there by hand. */
    {
        char box[1024];
        std::snprintf(box, sizeof(box), "\nHow this run ended: %s\nIt reached the %s phase.\n",
            reason, rt_run_phase_name(rt_run_phase_now()));
        rt_log_record_failure_text(box);
    }
}

/* ---- the watchdog ---------------------------------------------------------- */

namespace {

void watchdog_main() {
    rt_thread_set_name("field watchdog");
    uint64_t reported_at_ms = 0;   /* 0 = no report outstanding */
    uint64_t gif_at_last_report = 0;
    bool gif_stall_logged = false;
    uint64_t last_gif_change_ms = now_ms();
    uint64_t last_gif_count = g_gif_packets.load(std::memory_order_relaxed);

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_watchdog_mu);
            g_watchdog_cv.wait_for(lk, kWatchdogStep, [] { return g_watchdog_stop; });
            if (g_watchdog_stop) break;
        }

        const uint64_t now = now_ms();
        const uint64_t last_field = g_last_field_ms.load(std::memory_order_relaxed);
        const uint64_t since_field = now > last_field ? now - last_field : 0;

        char state[768];
        if (since_field >= kStallFirstMs) {
            const bool first = reported_at_ms == 0;
            if (first || now - reported_at_ms >= kStallRepeatMs) {
                reported_at_ms = now;
                format_state(state, sizeof(state));
                rt_log_warn("run", "no field has completed for %llu.%llu s. %s",
                    (unsigned long long)(since_field / 1000),
                    (unsigned long long)((since_field % 1000) / 100), state);
                if (first) {
                    rt_log_warn("run", "the run is not being ended: a very slow field and a hung"
                        " one look the same from here, and killing the first would be its own"
                        " bug. This repeats every %llu s while the stall lasts.",
                        (unsigned long long)(kStallRepeatMs / 1000));
                }
                rt_run_request_inventory("no field completed for 5 s");
            }
        } else if (reported_at_ms != 0) {
            format_state(state, sizeof(state));
            rt_log_warn("run", "fields are completing again after the stall above. %s", state);
            reported_at_ms = 0;
        }

        /* The guest-loop check. Fields keep arriving, so the EE thread and
         * the pacer are both alive, but the guest has submitted no GIF
         * traffic at all: it is spinning somewhere with nothing to draw.
         * Suppressed while the overlay menu is up, because the menu pauses
         * nothing and a paused-looking guest there is the expected state. */
        const uint64_t gif = g_gif_packets.load(std::memory_order_relaxed);
        if (gif != last_gif_count) {
            last_gif_count = gif;
            last_gif_change_ms = now;
            gif_stall_logged = false;
        } else if (!gif_stall_logged && since_field < kStallFirstMs &&
                   rt_run_phase_now() >= RT_PHASE_FIRST_FIELD &&
                   now - last_gif_change_ms >= kNoGifMs) {
            gif_stall_logged = true;
            gif_at_last_report = gif;
            format_state(state, sizeof(state));
            rt_log_warn("run", "fields are still advancing but the guest has submitted no GIF"
                " traffic for %llu s (%llu packets total): it is looping with nothing to draw."
                " %s", (unsigned long long)((now - last_gif_change_ms) / 1000),
                (unsigned long long)gif_at_last_report, state);
            /* The line above says the guest is looping; the inventory says
             * what it is looping on. Requested rather than taken here: the
             * scheduler's thread and semaphore tables belong to the EE
             * thread, which is running. */
            rt_run_request_inventory("no GIF traffic while fields advance");
        }
    }
}

} // namespace

void rt_run_watchdog_start() {
    start_clock();
    if (g_watchdog_running.exchange(true)) return;
    {
        std::lock_guard<std::mutex> lk(g_watchdog_mu);
        g_watchdog_stop = false;
    }
    /* Seeded to now rather than to zero: the watchdog would otherwise report
     * the whole of the boot before the first field as a stall. */
    g_last_field_ms.store(now_ms(), std::memory_order_relaxed);
    g_watchdog = std::thread(watchdog_main);
    rt_log_info("run", "field watchdog started: a warn line if no field completes for %llu s,"
        " repeated every %llu s while the stall lasts. It never ends the run.",
        (unsigned long long)(kStallFirstMs / 1000),
        (unsigned long long)(kStallRepeatMs / 1000));
}

void rt_run_watchdog_stop() {
    if (!g_watchdog_running.exchange(false)) return;
    {
        std::lock_guard<std::mutex> lk(g_watchdog_mu);
        g_watchdog_stop = true;
    }
    g_watchdog_cv.notify_all();
    /* Joined, not detached. This runs from the summary, which runs from an
     * atexit handler on the way to the CRT tearing the process down: a
     * detached thread still inside rt_log_warn while log.cpp's own handler
     * joins the writer would be writing into a sink that is closing. The
     * join is bounded by the poll interval above.
     *
     * Except on the one path where the join cannot work: a fault taken on
     * the watchdog thread itself reaches the crash handler, which writes
     * the summary, which lands here. Joining this thread from this thread
     * is a deadlock, and a crash dump that never finishes is the failure
     * this whole file exists to remove. Detach instead and let the process
     * end. */
    if (!g_watchdog.joinable()) return;
    if (g_watchdog.get_id() == std::this_thread::get_id()) {
        g_watchdog.detach();
        return;
    }
    g_watchdog.join();
}
