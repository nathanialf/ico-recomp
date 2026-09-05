/* log.cpp: shared logging, the log-file sink, verbosity gating, and
 * register-dump helpers.
 *
 * Log sink: on Windows a double-clicked run owns its console, and the
 * console dies with the process, so a crash takes the whole log with it.
 * rt_log_init points file descriptor 2 at a log file instead. Because the
 * redirect is at the fd level rather than on the C stderr FILE*, it catches
 * every module in the process, including the GS shared library, SDL and the
 * Vulkan loader, each of which may carry its own CRT copy. The original
 * stderr is kept as a duplicate so the runtime's own messages still echo to
 * the console while it is alive.
 *
 * The sink tries several locations in turn rather than giving up on the
 * first failure, because the one that matters most (next to the
 * executable) is also the one a user is most likely to have made
 * read-only: an install under Program Files, a read-only network share, a
 * folder still open in an archiver. Each attempt that fails names itself
 * and the reason on the console, and the location finally chosen is
 * logged as the first line of the run, so a bug report says where its own
 * log came from.
 *
 * Levels: every line carries one of error, warn, info, debug, and is
 * emitted only when its level is at or above the level in force
 * (debug.log_level, default warn, environment twin ICORECOMP_LOG_LEVEL).
 * Both sinks obey it. Debug lines additionally go to the log file only,
 * never to the console echo, which is what the verbose channels have
 * always done and is why turning the level down does not make the console
 * unusable. See runtime.h for what each level is for.
 *
 * Verbosity: ICORECOMP_VERBOSE names the components whose high-volume
 * diagnostic channel is on ("vu1,geom", or "all"). A channel named there
 * passes its debug lines whatever the level is, so a warn-level run can
 * still carry one full trace. Setting it also turns the file sink on
 * everywhere, since a trace with nowhere to go is worse than none.
 *
 * Console: on Windows ico.exe is a GUI-subsystem binary, so a
 * double-clicked run has no console at all. rt_console_init attaches to
 * the console the process was launched from when there is one, and
 * allocates one when debug.console asks for it. With neither, the console
 * echo is off and rt_log_hold_console shows the failure in a message box
 * instead, because a fatal that reaches nobody is the failure mode this
 * codebase does not accept.
 *
 * Threading: no log call from guest-executing code touches a file. A
 * caller formats its line into a stack buffer, copies it into a bounded
 * ring under a mutex that is never held across I/O, and returns. One
 * writer thread owns both FILE* sinks and does every fwrite and fflush.
 * That takes the cost of a write-through file handle and of a Windows
 * console round trip off the frame path entirely, whatever it turns out
 * to be, rather than relying on a per-field flush to keep it small.
 *
 * The ring is bounded, so a sink slower than the producers drops lines
 * rather than growing without limit or stalling the caller. Dropped lines
 * are counted and the writer names the count in the log, because silent
 * loss is the failure mode this codebase does not accept.
 *
 * Fatal paths do not use any of that. rt_log_drain stops the asynchronous
 * mode, waits for what is queued to reach the file, and puts every later
 * line back on the calling thread, so a crash dump is complete even when
 * the process leaves through _Exit and runs no atexit handler.
 */
#include "runtime.h"

#include "host/portable.h"


#include <atomic>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/* Duplicate of the pre-redirect stderr; null when no redirect happened, in
 * which case stderr is still the console and needs no second write. */
std::FILE* g_console = nullptr;
std::string g_log_path;

/* The log file, held explicitly, for the case where the fd-2 redirect
 * could not be installed. When the redirect IS in place, stderr already
 * is this file and writing through both would interleave two buffers
 * onto one file, so this stays null and stderr does the work. */
std::FILE* g_logfile = nullptr;

/* The verbose channel set: ICORECOMP_VERBOSE at startup, for CI and
 * scripted runs; rt_log_set_verbose can replace it later.
 *
 * Published as an immutable snapshot rather than mutated in place. The
 * writer is the main thread, which rebuilds the set when the menu commits
 * at startup); the readers are every thread that logs, including the GS
 * command ring's worker (gs/gs_threaded.cpp). Clearing a shared vector
 * would free the strings a reader is part-way through comparing, so a new
 * set is built off to the side and swapped in whole: a reader sees either
 * the old set or the new one, and holds its own reference to whichever it
 * got for as long as it walks it. This is the hazard g_rt_log_level is an
 * atomic for (runtime.h), one level up.
 *
 * g_verbose_any is the fast path, and only a hint, so it needs no ordering
 * against the snapshot: a stale false costs one debug line at the moment
 * of an edit, a stale true costs one shared_ptr load that finds an empty
 * set. */
struct VerboseSet {
    bool all = false;
    std::vector<std::string> tags;
};
std::shared_ptr<const VerboseSet> g_verbose;   /* atomic_load/atomic_store only */
std::atomic<bool> g_verbose_any{false};

std::shared_ptr<const VerboseSet> verbose_set() {
    return std::atomic_load_explicit(&g_verbose, std::memory_order_acquire);
}

void publish_verbose(const VerboseSet& set) {
    std::shared_ptr<const VerboseSet> snapshot = std::make_shared<const VerboseSet>(set);
    std::atomic_store_explicit(&g_verbose, snapshot, std::memory_order_release);
    g_verbose_any.store(set.all || !set.tags.empty(), std::memory_order_relaxed);
}

/* Set by rt_console_init: whether this process has a console to echo to at
 * all, and whether it owns that console (allocated it) rather than having
 * attached to the one it was launched from. Owning it is what makes
 * waiting for Enter on a fatal correct: an attached console belongs to the
 * shell and outlives the process, so blocking it would only hang a script.
 */
bool g_console_present = false;
bool g_console_owned = false;

/* The first lines of whatever went wrong, kept so a run with no console
 * can still put the failure in front of the user. Only error-level lines
 * and rt_fatal's own message land here; capped so a fault that logs in a
 * loop cannot grow it without bound.
 *
 * Its own mutex, not g_mu: every thread that can log an error can append
 * here, and the GS worker is allowed to raise a fatal (see
 * rt_log_hold_console), so the EE thread's error line and the worker's
 * FATAL can arrive together. Two unsynchronised appends would corrupt the
 * one buffer the message box is about to show, which is the last thing
 * the process does. Error-level lines only, so the lock is never on a hot
 * path, and it is never held across I/O. */
std::string g_failure_text;
std::mutex g_failure_mu;
constexpr size_t kFailureTextMax = 1400;

void record_failure(const char* text, size_t len) {
    std::lock_guard<std::mutex> lk(g_failure_mu);
    if (g_failure_text.size() >= kFailureTextMax) return;
    g_failure_text.append(text, std::min(len, kFailureTextMax - g_failure_text.size()));
}

/* Only rt_log_hold_console's message box reads it, and that is Windows
 * only, so this is unused elsewhere by design rather than by accident. */
[[maybe_unused]] std::string failure_text_copy() {
    std::lock_guard<std::mutex> lk(g_failure_mu);
    return g_failure_text;
}

/* Channels a run gets without being asked, once it has a log file to put
 * them in. Deliberately empty: "geom" re-parses every GIF packet the
 * runtime submits, around 86,000 quadwords per field on this game, which
 * is a second full pass over all graphics traffic, and a diagnostic must
 * not cost frame time in a run nobody asked to diagnose.
 *
 * It was default-on for the opening-cutscene geometry investigation, on
 * the grounds that a run made on one machine and read on another comes
 * back useless without it. That was the right call while the bug was open
 * and the wrong default to keep afterwards. Set ICORECOMP_VERBOSE=geom to
 * turn it back on. */
const char* const kDefaultVerbose = "";

/* ---- asynchronous sink ---------------------------------------------------
 *
 * Two sinks, resolved once by rt_log_init and read only by the writer
 * thread afterwards:
 *
 *   file_sink()      where every line goes. stderr when the fd-2 redirect
 *                    is in place (stderr is the log file then), stderr
 *                    again on the Windows GUI-with-no-console path where
 *                    stderr was reopened onto the log file, g_logfile when
 *                    neither happened, and stderr once more when there is
 *                    no log file at all, in which case stderr is still the
 *                    console and is the only sink there is. It is never
 *                    two FILE objects on one file: two would have
 *                    independent offsets and would overwrite each other.
 *   g_console_sink   the console echo, or null when there is nothing to
 *                    echo to. Verbose lines skip it.
 */
std::FILE* g_console_sink = nullptr;

std::FILE* file_sink() { return g_logfile ? g_logfile : stderr; }

enum Dest : uint8_t {
    kBoth = 0,      /* log file and console echo */
    kFileOnly = 1,  /* verbose channels: file only */
};

/* One queued line. Fixed slots rather than a packed byte ring: the ring is
 * 2 MB of demand-paged BSS, which costs nothing in a process that already
 * maps 32 MB of guest RAM, and it keeps the enqueue path to a bounds check
 * and one memcpy of the formatted length. */
constexpr size_t kLineMax = 4096;
constexpr size_t kSlots = 512;

struct Slot {
    uint32_t len;
    uint8_t dest;
    char text[kLineMax];
};

Slot g_ring[kSlots];
size_t g_head = 0;      /* next slot to fill */
size_t g_tail = 0;      /* next slot to write out */
uint64_t g_dropped = 0; /* lines lost to a full ring since the last batch */
bool g_stop = false;
bool g_busy = false;    /* writer holds a drained batch and is doing I/O */

/* The thread that ran rt_log_init: main's first statement, so this is the
 * process's main thread, which is also the EE thread (guest threads are
 * coroutines on it). Two fatal-path decisions depend on knowing whether the
 * caller is that thread, and rt_log is the one service every thread in the
 * process already shares. */
std::thread::id g_main_thread;
bool g_main_thread_known = false;

/* Set by rt_fatal just before it calls std::exit, and read back by
 * rt_fatal_exit_code(). An atexit handler that has to end the process itself
 * (gs/gs_threaded.cpp's abandon_worker, when a fatal was raised on the GS
 * worker thread and the join would deadlock) needs the status the fatal was
 * going to exit with; without it a fatal would report success. -1 means no
 * fatal is in progress. */
std::atomic<int> g_fatal_exit_code{-1};

std::mutex g_mu;                 /* ring, counters, flags. Never held across I/O. */
std::condition_variable g_cv;    /* producer -> writer */
std::condition_variable g_idle;  /* writer -> rt_log_drain */
std::mutex g_io_mu;              /* serializes the two FILE* sinks */
std::thread g_writer;
std::once_flag g_writer_once;

/* False until the writer thread is up, and false again once a fatal path
 * has drained it. Both states mean "write on the calling thread". */
std::atomic<bool> g_async{false};

/* False until rt_log_init has finished moving the sink globals
 * (g_console_sink, g_logfile, stderr's buffering, fd 2). While it is
 * false, enqueue writes on the calling thread rather than starting the
 * writer, so nothing can bring the writer up behind rt_log_init's back
 * while its AsyncPause guard has it nominally parked. Set from that
 * guard's destructor, which is what covers rt_log_init's several early
 * exits. A process that never calls rt_log_init (the standalone tools)
 * stays synchronous, which is correct and only slower. */
std::atomic<bool> g_sink_open{false};

/* Set while this thread is inside the enqueue critical section. A crash
 * handler that finds it set must not wait on the writer: the thread it
 * would be waiting for may need the mutex this thread is holding. */
thread_local bool t_enqueuing = false;

void write_now(uint8_t dest, const char* text, size_t len) {
    std::fwrite(text, 1, len, file_sink());
    if (dest == kBoth && g_console_sink) std::fwrite(text, 1, len, g_console_sink);
}

void flush_now() {
    std::fflush(file_sink());
    if (g_console_sink) std::fflush(g_console_sink);
}

/* Waits until the writer has written and flushed everything queued.
 * Bounded: a writer stuck in a blocking console write on a hung console
 * host must not take a crash dump, or process exit, down with it. */
void wait_writer_idle() {
    if (t_enqueuing) {
        /* Called from a fault taken inside the enqueue critical section.
         * The writer may be blocked on the mutex this thread holds, so
         * waiting for it would hang. Give up the queued lines; a dump
         * missing its tail beats a process that never dies. */
        return;
    }
    std::unique_lock<std::mutex> lk(g_mu);
    g_cv.notify_all();
    g_idle.wait_for(lk, std::chrono::milliseconds(500),
        [] { return g_head == g_tail && !g_busy; });
}

void writer_main() {
    std::string batch;   /* everything in this drain, in order */
    std::string echo;    /* the kBoth subset, same order */
    for (;;) {
        uint64_t dropped = 0;
        bool stop = false;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return g_head != g_tail || g_stop; });
            batch.clear();
            echo.clear();
            while (g_tail != g_head) {
                const Slot& s = g_ring[g_tail];
                batch.append(s.text, s.len);
                if (s.dest == kBoth) echo.append(s.text, s.len);
                g_tail = (g_tail + 1) % kSlots;
            }
            dropped = g_dropped;
            g_dropped = 0;
            stop = g_stop;
            g_busy = true;
        }
        {
            std::lock_guard<std::mutex> io(g_io_mu);
            if (dropped) {
                /* Loud, and placed where the gap is: everything after this
                 * line was queued after the lines that were lost. */
                char note[160];
                int n = std::snprintf(note, sizeof(note),
                    "[icorecomp][log] %llu log lines dropped before this point:"
                    " the log queue filled faster than the writer thread drained it\n",
                    (unsigned long long)dropped);
                if (n > 0) write_now(kBoth, note, (size_t)n);
            }
            if (!batch.empty()) {
                std::fwrite(batch.data(), 1, batch.size(), file_sink());
                if (!echo.empty() && g_console_sink) {
                    std::fwrite(echo.data(), 1, echo.size(), g_console_sink);
                }
            }
            /* One flush per batch, and a batch is everything that had
             * accumulated when this pass started. Under load the batches
             * grow and the flush rate falls on its own, so this does not
             * need a timer to stay off the writer's back. A kill loses at
             * most what was queued while the last batch was being
             * written. */
            flush_now();
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_busy = false;
        }
        g_idle.notify_all();
        if (stop) return;
    }
}

void start_writer() {
    std::call_once(g_writer_once, [] {
        g_writer = std::thread(writer_main);
        g_async.store(true, std::memory_order_release);
        /* Runs before the CRT closes stdio and, being registered here,
         * after any atexit the caller registered earlier, so handlers that
         * log on the way out (the GS backend stats report) are drained. */
        std::atexit([] {
            rt_log_drain();
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_stop = true;
            }
            g_cv.notify_all();
            if (g_writer.joinable()) g_writer.join();
            /* A producer that passed the g_async check just as the drain
             * cleared it can have left a line behind. Sweep it out here. */
            std::lock_guard<std::mutex> io(g_io_mu);
            std::lock_guard<std::mutex> lk(g_mu);
            while (g_tail != g_head) {
                const Slot& s = g_ring[g_tail];
                write_now(s.dest, s.text, s.len);
                g_tail = (g_tail + 1) % kSlots;
            }
            flush_now();
        });
    });
}

/* The one path a log line takes. Formatting already happened; this only
 * copies. */
void enqueue(uint8_t dest, const char* text, size_t len) {
    if (len == 0) return;
    if (len > kLineMax) len = kLineMax;
    if (!g_async.load(std::memory_order_acquire)) {
        if (g_sink_open.load(std::memory_order_acquire)) start_writer();
        if (!g_async.load(std::memory_order_acquire)) {
            /* Before the thread exists, and after a fatal path took it
             * away: the calling thread does the write. */
            std::lock_guard<std::mutex> io(g_io_mu);
            write_now(dest, text, len);
            flush_now();
            return;
        }
    }
    t_enqueuing = true;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        size_t next = (g_head + 1) % kSlots;
        if (next == g_tail) {
            ++g_dropped;
            t_enqueuing = false;
            return;
        }
        Slot& s = g_ring[g_head];
        s.dest = dest;
        s.len = (uint32_t)len;
        std::memcpy(s.text, text, len);
        g_head = next;
    }
    t_enqueuing = false;
    g_cv.notify_one();
}

/* Formats once, then queues for the log file and the console duplicate.
 * Overlong lines are truncated rather than split; nothing the runtime logs
 * comes close to the buffer size. */
void emit(const char* fmt, ...) {
    char buf[kLineMax];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    enqueue(kBoth, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

/* Verbose lines go to the log file only. With no log file there is one
 * sink and it is the console; rt_log_init says so once rather than
 * dropping the trace. */
void emit_file(const char* fmt, ...) {
    char buf[kLineMax];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    enqueue(kFileOnly, buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

/* Formats one already-gated line and puts it on its sink.
 *
 * The prefix keeps the shape every existing line and every grep in
 * docs/SETTINGS.md already has, "[icorecomp][component] ", and marks the
 * level after the component for everything but info. Info is the unmarked
 * case because it is what the format has always meant; marking it would
 * break those greps and say nothing new. */
void write_line(RtLogLevel level, const char* component, const char* line) {
    if (level == RT_LOG_DEBUG) {
        emit_file("[icorecomp][%s][debug] %s\n", component, line);
        return;
    }
    if (level == RT_LOG_INFO) {
        emit("[icorecomp][%s] %s\n", component, line);
        return;
    }
    const char* tag = (level == RT_LOG_ERROR) ? "error" : "warn";
    if (level == RT_LOG_ERROR) {
        char buf[kLineMax];
        int n = std::snprintf(buf, sizeof(buf), "[%s] %s\n", component, line);
        if (n > 0) record_failure(buf, (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    }
    emit("[icorecomp][%s][%s] %s\n", component, tag, line);
}

/* The console half of rt_log_init's own reporting, which has to happen
 * before the log file exists (or when it never will). Guarded rather than
 * a bare fprintf: on a GUI-subsystem Windows run with no console there is
 * no handle behind stdout, so a line written there reaches nobody. A macro
 * rather than a function so the compiler keeps checking the format string
 * against its arguments. */
#define CONSOLE_PRINTF(...) do { \
    if (g_console_present) { \
        std::fprintf(stdout, __VA_ARGS__); \
        std::fflush(stdout); \
    } \
} while (0)

/* Adds the channels `spec` names to `out`. Builds into a caller-owned set
 * rather than into the published one, so the snapshot readers walk is only
 * ever replaced whole. */
void parse_verbose(const char* spec, VerboseSet* out) {
    std::string cur;
    for (const char* p = spec;; ++p) {
        if (*p == ',' || *p == ' ' || *p == '\0') {
            if (!cur.empty()) {
                if (cur == "all" || cur == "1") out->all = true;
                else out->tags.push_back(cur);
                cur.clear();
            }
            if (*p == '\0') break;
        } else {
            cur.push_back(*p);
        }
    }
}

/* Inserts ".prev" before the last '.' in the filename portion of `path`
 * ("foo/icorecomp.log" -> "foo/icorecomp.prev.log"). Works on the string
 * directly rather than through std::filesystem::path, so it never disturbs
 * whichever separator style the caller's path already uses. */
std::string with_prev_suffix(const std::string& path) {
    size_t sep = path.find_last_of("/\\");
    size_t name_start = (sep == std::string::npos) ? 0 : sep + 1;
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < name_start) return path + ".prev";
    return path.substr(0, dot) + ".prev" + path.substr(dot);
}

/* Outcome of rotate_prev_log, carried forward so the result can be logged
 * once the log file is open (it is not open yet at rotation time). */
struct RotateOutcome {
    bool attempted = false;
    bool ok = false;
    std::string prev_path;
    std::string error;
};

/* If a file already exists at `path`, renames it to the same path with
 * ".prev" inserted before the extension, replacing any older ".prev" file,
 * so the log about to be opened at `path` does not overwrite a crash log
 * from the previous run. Never fatal: a failed rename falls through
 * to the normal open-and-truncate behavior. */
RotateOutcome rotate_prev_log(const std::string& path) {
    RotateOutcome out;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return out;
    out.attempted = true;
    out.prev_path = with_prev_suffix(path);
    std::filesystem::rename(path, out.prev_path, ec);
    out.ok = !ec;
    if (!out.ok) out.error = ec.message();
    return out;
}

/* One candidate location for the log. Returns the open file, or null with
 * `why` filled in. */
std::FILE* try_open(const std::string& path, std::string* why) {
    if (path.empty()) {
        *why = "no candidate path";
        return nullptr;
    }
    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    unsigned long win_err = 0;
    std::FILE* f = rt_fopen_log(path.c_str(), &win_err);
    if (!f) {
        *why = std::strerror(errno);
        if (win_err != 0) {
            char extra[64];
            std::snprintf(extra, sizeof(extra), " (Windows error %lu)", win_err);
            *why += extra;
        }
    }
    return f;
}

std::string temp_dir() {
#ifdef _WIN32
    if (const char* p = std::getenv("TEMP")) return p;
    if (const char* p = std::getenv("TMP")) return p;
    return std::string();
#else
    if (const char* p = std::getenv("TMPDIR")) return p;
    return std::string("/tmp");
#endif
}

} // namespace

/* Declared in runtime.h so rt_log_level_enabled can be an inline load.
 * Warn is the shipped default; rt_log_set_initial_level moves it before
 * rt_log_init for the standalone tools, ICORECOMP_LOG_LEVEL moves it
 * inside rt_log_init, and debug.log_level moves it once settings load. */
std::atomic<int> g_rt_log_level{(int)RT_LOG_WARN};

bool rt_log_on_main_thread() {
    /* Before rt_log_init nothing else has started, so the caller is the main
     * thread by construction. */
    return !g_main_thread_known || std::this_thread::get_id() == g_main_thread;
}

int rt_fatal_exit_code() {
    return g_fatal_exit_code.load(std::memory_order_acquire);
}

void rt_console_init(bool want_alloc) {
#ifdef _WIN32
    /* Order matters. Attaching to the launching shell's console comes
     * first so a run started from cmd, PowerShell or a CI job keeps
     * printing where the user is already looking; only a run with no
     * console behind it (a double-click from Explorer) can be given one of
     * its own, and only when debug.console asks. */
    bool have = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
    bool owned = false;
    if (!have && GetConsoleWindow() != nullptr) {
        /* Already has one: a console-subsystem build, or a host that
         * handed us one. Nothing to attach or allocate. */
        have = true;
    }
    if (!have && want_alloc) {
        have = owned = AllocConsole() != 0;
    }
    if (!have) return;

    /* The CRT's stdout/stderr/stdin are not wired to a console this
     * process only just acquired, so point them at it. freopen replaces
     * the underlying fd as well as the FILE*, which is what lets the
     * fd-2 redirect in rt_log_init dup a real handle afterwards. */
    std::FILE* f = nullptr;
    (void)freopen_s(&f, "CONOUT$", "w", stdout);
    (void)freopen_s(&f, "CONOUT$", "w", stderr);
    (void)freopen_s(&f, "CONIN$", "r", stdin);
    g_console_present = true;
    g_console_owned = owned;
#else
    /* stderr is the console and always has been. debug.console names a
     * Windows-only behaviour and changes nothing here. */
    (void)want_alloc;
    g_console_present = true;
    g_console_owned = false;
#endif
}

bool rt_console_present() { return g_console_present; }

void rt_log_init(const char* dir, bool file_allowed, bool console_wanted) {
    g_main_thread = std::this_thread::get_id();
    g_main_thread_known = true;
    /* This is main's first statement, so in practice nothing has logged
     * yet and no writer thread exists. It is still cheap to be certain:
     * park the writer for the whole of this function, not only up to the
     * first store. g_console_sink, g_logfile, stderr's buffering and fd 2
     * all move below, and they are plain globals rather than atomics, so a
     * writer waking mid-swap would read them torn. Parking has to cover
     * every exit, and this function has several early returns, so it is a
     * guard rather than a call.
     *
     * Parking an already-running writer is only half of it: on a cold
     * start there is no writer yet, and the first emit below would have
     * started one from inside this function. g_sink_open is what stops
     * that, and the destructor sets it, because the destructor is the one
     * thing every exit from here runs. */
    struct AsyncPause {
        bool resume;
        AsyncPause() : resume(g_async.exchange(false, std::memory_order_acq_rel)) {
            wait_writer_idle();
        }
        ~AsyncPause() {
            g_sink_open.store(true, std::memory_order_release);
            if (resume) g_async.store(true, std::memory_order_release);
        }
    } pause_writer;

    rt_console_init(console_wanted);

    /* ICORECOMP_LOG_LEVEL wins over debug.log_level for the whole run, as
     * every other environment twin does. A name that is not one of the
     * four keeps the level the caller left in place and says so; settings
     * handling is never fatal and neither is this. The line goes to stdout
     * because the log file does not exist yet. */
    const char* lspec = std::getenv("ICORECOMP_LOG_LEVEL");
    bool level_from_env = false;
    if (lspec) {
        RtLogLevel parsed = rt_log_get_level();
        if (rt_log_level_parse(lspec, &parsed)) {
            rt_log_set_level(parsed);
            level_from_env = true;
        } else {
            CONSOLE_PRINTF("[icorecomp][log] ICORECOMP_LOG_LEVEL=%s is not one of"
                " error/warn/info/debug; keeping %s\n", lspec, rt_log_level_name(rt_log_get_level()));
        }
    }

    const char* vspec = std::getenv("ICORECOMP_VERBOSE");
    /* "-", "0" or "none" turns the channels off, including the defaults. */
    const bool verbose_off = vspec && (std::strcmp(vspec, "-") == 0
        || std::strcmp(vspec, "0") == 0 || std::strcmp(vspec, "none") == 0);
    VerboseSet vset;
    if (vspec && !verbose_off) parse_verbose(vspec, &vset);
    const bool verbose = vset.all || !vset.tags.empty();
    publish_verbose(vset);

    const char* env = std::getenv("ICORECOMP_LOG");
    /* The environment always wins over debug.log_file (see settings.cpp's
     * kEnvTwins table). When it disagrees with a false setting, say so once
     * so a run started with the old env var doesn't look like it silently
     * ignored a settings.json edit. */
    if (env && *env && !file_allowed) {
        CONSOLE_PRINTF("[icorecomp][log] debug.log_file: using ICORECOMP_LOG=%s,"
            " settings.json value ignored\n", env);
    }
    const std::string base = (dir && *dir) ? dir : ".";

    /* Candidates, in the order they are tried. An explicit ICORECOMP_LOG is
     * the user's choice and gets no fallbacks: silently writing somewhere
     * else would be worse than saying it failed. */
    std::vector<std::string> candidates;
    if (env && *env) {
        /* "-" or "0" opts out: console only, the pre-sink behavior. */
        if (std::strcmp(env, "-") == 0 || std::strcmp(env, "0") == 0) {
            if (verbose) {
                CONSOLE_PRINTF("[icorecomp][log] ICORECOMP_LOG=%s disables the log file;"
                    " ICORECOMP_VERBOSE output goes to the console\n", env);
            }
            return;
        }
        candidates.push_back(env);
    } else {
        /* debug.log_file=false, no ICORECOMP_LOG override: same opt-out as
         * ICORECOMP_LOG=- above, just from settings.json instead of the
         * environment. */
        if (!file_allowed) {
            if (verbose) {
                CONSOLE_PRINTF("[icorecomp][log] debug.log_file=false disables the log file\n");
            }
            return;
        }
#ifdef _WIN32
        candidates.push_back(base + "/icorecomp.log");
#else
        /* POSIX runs are launched from a shell that keeps the output, so
         * the sink is opt-in there, except under ICORECOMP_VERBOSE: that
         * trace is far too large for a terminal to hold. */
        if (!verbose) return;
        candidates.push_back(base + "/icorecomp.log");
#endif
        if (std::string d = rt_user_state_dir(); !d.empty()) candidates.push_back(d + "/icorecomp.log");
        if (std::string d = temp_dir(); !d.empty()) candidates.push_back(d + "/icorecomp.log");
    }

    std::FILE* f = nullptr;
    std::string path;
    RotateOutcome rotated;
    for (const std::string& c : candidates) {
        RotateOutcome rot = rotate_prev_log(c);
        std::string why;
        f = try_open(c, &why);
        if (f) {
            path = c;
            rotated = rot;
            break;
        }
        /* stdout, not stderr: stderr is about to become the log file, and
         * on the failure path there is no log file to read this in. The
         * console is the only place this can land. */
        CONSOLE_PRINTF("[icorecomp][log] could not open '%s' for writing (%s)\n",
            c.c_str(), why.c_str());
    }
    if (!f) {
        CONSOLE_PRINTF("[icorecomp][log] no writable log location; this run logs to the"
            " console only, and the console does not survive the window closing\n");
        return;
    }

    /* A GUI-subsystem run with no console has no handle behind fd 2 at
     * all (GetStdHandle returns null), and msvcrt's stderr FILE object is
     * dead with it: _dup(2) is an invalid parameter, _dup2 onto fd 2
     * succeeds but leaves the FILE object closed, so every fwrite(stderr)
     * fails without a word. That happened to the first PAL packages, which
     * rotated their log and then wrote nothing. So without a handle behind
     * fd 2 stderr is reopened onto the log file (append) and becomes the
     * one stream the run writes through, so that the shared library's and
     * the driver's own stderr lines land in the same file as the runtime's
     * own, interleaved rather than overwriting one another. */
    bool have_stderr = true;
#ifdef _WIN32
    {
        HANDLE h2 = GetStdHandle(STD_ERROR_HANDLE);
        have_stderr = h2 != nullptr && h2 != INVALID_HANDLE_VALUE;
    }
#endif
    int saved = have_stderr ? rt_dup(2) : -1;
    bool reopened_stderr = false;
    (void)reopened_stderr;
    std::fflush(stderr);
    if (!have_stderr || rt_dup2(rt_fileno(f), 2) < 0) {
        /* No redirect, so output from the GS library, SDL and the Vulkan
         * loader stays on the console. The runtime's own log is the part
         * that matters and it keeps its file: writing through the FILE*
         * directly needs nothing from fd 2. */
        if (have_stderr) {
            CONSOLE_PRINTF("[icorecomp][log] could not point stderr at '%s'; the runtime log still"
                " goes there, but output from the renderer and the Vulkan driver stays on the console\n",
                path.c_str());
        }
#ifdef _WIN32
        else {
            /* See above: revive the CRT's stderr onto the log file, and
             * give fd 2 the same file for anything that writes the
             * descriptor directly.
             *
             * One stream, not two. Two FILE objects on one file have
             * independent offsets: the runtime's buffered writes at its own
             * offset would overwrite whatever a library had appended, which
             * is exactly the output this revival exists to keep. So on
             * success the first handle is closed and g_logfile is left null,
             * which makes file_sink() return stderr and puts the runtime's
             * own lines and the library's on the same offset.
             *
             * The path is normalized to backslashes first, for the reason
             * rt_fopen_log gives: the runtime joins with '/', which a UNC
             * path does not tolerate mixed.
             *
             * What is lost by going through _wfreopen_s is
             * FILE_FLAG_WRITE_THROUGH, which rt_fopen_log obtains from
             * CreateFileW and which no CRT reopen can ask for. The writer
             * thread already flushes once per batch, so a kill loses at most
             * what was queued while the last batch was being written; on a
             * network share the difference is that a flushed line may sit in
             * the client's cache rather than on the server. That is the
             * accepted cost of keeping the library's lines. */
            std::string wpath_utf8 = path;
            for (char& c : wpath_utf8) {
                if (c == '/') c = '\\';
            }
            int wn = MultiByteToWideChar(CP_UTF8, 0, wpath_utf8.c_str(), -1, nullptr, 0);
            std::wstring wpath(size_t(wn > 0 ? wn : 1), L'\0');
            if (wn > 0) {
                MultiByteToWideChar(CP_UTF8, 0, wpath_utf8.c_str(), -1, wpath.data(), wn);
            }
            std::FILE* nf = nullptr;
            if (wn > 0 && _wfreopen_s(&nf, wpath.c_str(), L"a", stderr) == 0 && nf) {
                /* _IOFBF and not _IOLBF: the Microsoft CRT treats _IOLBF as
                 * _IOFBF anyway, and the other branch buffers the same way
                 * and relies on the per-batch flush. */
                std::setvbuf(stderr, nullptr, _IOFBF, 1 << 16);
                (void)rt_dup2(rt_fileno(stderr), 2);
                /* The rotate already ran and the file was created empty by
                 * rt_fopen_log, so "a" starts at offset 0. */
                std::fclose(f);
                f = nullptr;
                reopened_stderr = true;
            }
        }
#endif
        if (f) {
            g_logfile = f;
            std::setvbuf(g_logfile, nullptr, _IOFBF, 1 << 16);
        }
        /* stderr only when there is a console behind it: on a GUI-subsystem
         * Windows run with no console, stderr has no handle and the echo
         * has nowhere to go. It is also the file sink there, and echoing a
         * line into the sink it already went to would double it. */
        g_console_sink = (g_console_present && !reopened_stderr) ? stderr : nullptr;
        if (saved >= 0) {
#ifdef _WIN32
            _close(saved);
#else
            close(saved);
#endif
        }
    } else {
        /* fd 2 now holds its own handle on the file; f's is redundant. */
        std::fclose(f);
        if (saved >= 0) {
            g_console = rt_fdopen(saved, "w");
            /* A console write on Windows is a synchronous round trip to
             * the console host. Flushing one per line cost more frame
             * time than everything else the runtime logs put together;
             * buffer it and let the once-per-field rt_log_flush push it
             * out. */
            if (g_console) std::setvbuf(g_console, nullptr, _IOFBF, 1 << 16);
        }
        g_console_sink = g_console;
        /* stderr is unbuffered by default, so without this every log line
         * is its own write syscall, and with FILE_FLAG_WRITE_THROUGH its
         * own trip to the disk or the file server. At sixty lines a field
         * that costs real frame time. Buffer instead and let the
         * once-per-field rt_log_flush bound what a kill can lose. */
        std::setvbuf(stderr, nullptr, _IOFBF, 1 << 16);
    }
    g_log_path = path;

    /* The file exists now, so the default channels have somewhere to go.
     * An explicit ICORECOMP_VERBOSE, including one that turns everything
     * off, always wins. */
    if (!vspec) {
        parse_verbose(kDefaultVerbose, &vset);
        publish_verbose(vset);
    }

    /* Announced on stdout as well as into the log. Someone looking for
     * the file needs the answer on the console, where it is visible
     * without already having found the file.
     *
     * The build stamp is here for a specific reason: this package is run
     * off a network share, where a stale client-side copy of the exe is
     * indistinguishable from a fix that did not work. Naming the build in
     * the first line of every run settles that without guesswork. */
    CONSOLE_PRINTF("[icorecomp] build " __DATE__ " " __TIME__ " (exe: %s)\n",
        rt_exe_identity().c_str());
    CONSOLE_PRINTF("[icorecomp][log] this run's log: %s\n", path.c_str());

    {
        std::string line = "[icorecomp] build " __DATE__ " " __TIME__ " (exe: "
            + rt_exe_identity() + ")\n";
        emit("%s", line.c_str());
    }
    emit("[icorecomp][log] writing this run's log to %s\n", path.c_str());
    if (rotated.attempted) {
        if (rotated.ok) {
            emit("[icorecomp][log] previous run's log kept as %s\n", rotated.prev_path.c_str());
        } else {
            emit("[icorecomp][log] could not keep the previous run's log as %s (%s); it was overwritten\n",
                rotated.prev_path.c_str(), rotated.error.c_str());
        }
    }
    emit("[icorecomp][log] base directory for config/, saves/ and the disc probe: %s\n", base.c_str());
    /* Unconditional, like the rest of this prologue: a reader who cannot
     * find a line they expected needs to know which level swallowed it. */
    emit("[icorecomp][log] log level %s%s (error > warn > info > debug; a line shows when its"
        " level is at or above this one)\n",
        rt_log_level_name(rt_log_get_level()),
        level_from_env ? " from ICORECOMP_LOG_LEVEL" : " (debug.log_level applies once settings load)");
    emit("[icorecomp][log] console: %s\n",
        g_console_owned ? "allocated for this run (debug.console)"
                        : (g_console_present ? "the one this process was launched from"
                                             : "none; failures are shown in a message box"));
    if (vset.all || !vset.tags.empty()) {
        std::string tags = vset.all ? "all" : std::string();
        for (const std::string& t : vset.tags) {
            if (!tags.empty()) tags += ",";
            tags += t;
        }
        emit("[icorecomp][log] verbose channels enabled: %s (file only, not echoed to the console;"
            " set ICORECOMP_VERBOSE to change, or ICORECOMP_VERBOSE=none to turn off)\n",
            tags.c_str());
    }
    if (rt_log_get_level() == RT_LOG_DEBUG) {
        emit("[icorecomp][log] the log level is debug, so every verbose channel this build"
            " defines is on. Set debug.log_level to info or warn and name single channels in"
            " ICORECOMP_VERBOSE to trace one subsystem at field-rate cost.\n");
    }
#ifdef ICORECOMP_GEOM_CHECK
    /* Not a channel any more: the geometry checker is a build define
     * (docs/GS_RENDERER.md), so a build that has it says so unconditionally
     * rather than being asked. */
    emit("[icorecomp][log] the geometry checker is compiled in: every GIF packet is re-parsed"
        " and counted, per microprogram and per MSCAL entry, in the profiler summary."
        " This costs per field, so frame times from this run read high.\n");
#endif
}

bool rt_verbose(const char* component) {
    return rt_log_level_enabled_for(RT_LOG_DEBUG, component);
}

bool rt_log_level_enabled_for(RtLogLevel level, const char* component) {
    if (rt_log_level_enabled(level)) return true;
    /* Below the level. Only a debug line can still get through, and only
     * because ICORECOMP_VERBOSE named its channel. */
    if (level != RT_LOG_DEBUG) return false;
    if (!g_verbose_any.load(std::memory_order_relaxed)) return false;
    std::shared_ptr<const VerboseSet> set = verbose_set();
    if (!set) return false;
    if (set->all) return true;
    for (const std::string& t : set->tags) {
        if (t == component) return true;
    }
    return false;
}

void rt_log_set_level(RtLogLevel level) {
    g_rt_log_level.store((int)level, std::memory_order_relaxed);
}

RtLogLevel rt_log_get_level() {
    return (RtLogLevel)g_rt_log_level.load(std::memory_order_relaxed);
}

void rt_log_set_initial_level(RtLogLevel level) {
    rt_log_set_level(level);
}

const char* rt_log_level_name(RtLogLevel level) {
    switch (level) {
    case RT_LOG_DEBUG: return "debug";
    case RT_LOG_INFO:  return "info";
    case RT_LOG_WARN:  return "warn";
    case RT_LOG_ERROR: return "error";
    }
    return "warn";
}

bool rt_log_level_parse(const char* name, RtLogLevel* out) {
    if (!name || !out) return false;
    if (std::strcmp(name, "debug") == 0) { *out = RT_LOG_DEBUG; return true; }
    if (std::strcmp(name, "info") == 0)  { *out = RT_LOG_INFO;  return true; }
    if (std::strcmp(name, "warn") == 0)  { *out = RT_LOG_WARN;  return true; }
    if (std::strcmp(name, "error") == 0) { *out = RT_LOG_ERROR; return true; }
    return false;
}

void rt_log_set_verbose(const char* spec) {
    VerboseSet vset;
    if (spec) {
        const bool off = std::strcmp(spec, "-") == 0 || std::strcmp(spec, "0") == 0
            || std::strcmp(spec, "none") == 0;
        if (!off) parse_verbose(spec, &vset);
    }
    publish_verbose(vset);
}

const char* rt_log_path() {
    return g_log_path.empty() ? nullptr : g_log_path.c_str();
}

void rt_vlog(RtLogLevel level, const char* component, const char* fmt, va_list ap) {
    /* Gated here, not only in the four named entry points above: a caller
     * that picks its level at run time (host/run_state.cpp's summary, which
     * is info for a user quit and warn for anything else) reaches the sink
     * through this function alone, and without the gate its info lines were
     * written into a file the level had ruled out. Same rule as the named
     * entry points, debug's verbose channel included. */
    if (!rt_log_level_enabled_for(level, component)) return;
    char line[3072];
    std::vsnprintf(line, sizeof(line), fmt, ap);
    write_line(level, component, line);
}

void rt_log_error(const char* component, const char* fmt, ...) {
    if (!rt_log_level_enabled(RT_LOG_ERROR)) return;
    va_list ap;
    va_start(ap, fmt);
    rt_vlog(RT_LOG_ERROR, component, fmt, ap);
    va_end(ap);
}

void rt_log_warn(const char* component, const char* fmt, ...) {
    if (!rt_log_level_enabled(RT_LOG_WARN)) return;
    va_list ap;
    va_start(ap, fmt);
    rt_vlog(RT_LOG_WARN, component, fmt, ap);
    va_end(ap);
}

void rt_log_info(const char* component, const char* fmt, ...) {
    if (!rt_log_level_enabled(RT_LOG_INFO)) return;
    va_list ap;
    va_start(ap, fmt);
    rt_vlog(RT_LOG_INFO, component, fmt, ap);
    va_end(ap);
}

void rt_log_debug(const char* component, const char* fmt, ...) {
    if (!rt_log_level_enabled_for(RT_LOG_DEBUG, component)) return;
    va_list ap;
    va_start(ap, fmt);
    rt_vlog(RT_LOG_DEBUG, component, fmt, ap);
    va_end(ap);
}

void rt_log_flush() {
    /* A wake, nothing more. The writer flushes both sinks every time it
     * drains the ring empty, so there is no buffered output for this to
     * push out and no reason for a frame to wait on one. */
    g_cv.notify_one();
}

void rt_log_drain() {
    if (!g_async.exchange(false, std::memory_order_acq_rel)) return;
    wait_writer_idle();
}

void rt_log_write_sync(const char* text) {
    if (!text) return;
    const size_t len = std::strlen(text);
    if (len == 0) return;
    /* try_lock, never lock. This is the crash path: the thread that faulted
     * may be the one holding g_io_mu, in which case waiting for it here is
     * a deadlock and the dump never appears. Two writers interleaving one
     * line is a cosmetic problem; a crash handler that hangs is the failure
     * this whole path exists to remove. */
    std::unique_lock<std::mutex> io(g_io_mu, std::try_to_lock);
    write_now(kBoth, text, len);
    flush_now();
}

void rt_log_sync(const char* component, const char* fmt, ...) {
    /* The body is bounded by what is left of a line once the prefix and the
     * component name have had their share, so the two buffers cannot add up
     * to more than one line. snprintf truncates rather than overruns either
     * way; sizing them apart is what lets the compiler see that, instead of
     * assuming a full body behind a full prefix. */
    constexpr size_t kSyncPrefix = 128;
    char body[kLineMax - kSyncPrefix];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    char line[kLineMax];
    std::snprintf(line, sizeof(line), "[icorecomp][%s][error] %s\n", component, body);
    rt_log_write_sync(line);
}

void rt_log_record_failure_text(const char* text) {
    if (!text) return;
    record_failure(text, std::strlen(text));
}

void rt_log_crash_write_selftest(const char* reason) {
    /* Exactly the sequence a crash handler runs, minus the ending: the
     * synchronous write first, then the drain that lets the queued lines
     * out behind it. Kept here rather than in the selftest so the two
     * cannot drift: what the test exercises is what the handler does. */
    char line[kLineMax];
    std::snprintf(line, sizeof(line),
        "[icorecomp][crash][error] ---- crash ---- (selftest: %s)\n",
        reason ? reason : "no reason given");
    rt_log_write_sync(line);
    rt_log_drain();
}

static const char* const kGprNames[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

void rt_dump_registers(const R5900Context* ctx) {
    /* Every caller is a fatal path: rt_fatal, rt_break, rt_bad_indirect and
     * the four crash handlers, three of which leave through std::_Exit and
     * run no atexit. Go synchronous here so the dump, and everything queued
     * ahead of it, is on disk by the time this returns. */
    rt_log_drain();
    if (!ctx) {
        emit("[icorecomp][regs] (null context)\n");
        return;
    }
    emit("[icorecomp][regs] pc_hint=0x%08x\n", ctx->pc_hint);
    for (int i = 0; i < 32; i += 2) {
        const rc_u128& a = ctx->r[i];
        const rc_u128& b = ctx->r[i + 1];
        emit("  %2d %-4s = %016llx:%016llx   %2d %-4s = %016llx:%016llx\n",
            i, kGprNames[i], (unsigned long long)a.u64x[1], (unsigned long long)a.u64x[0],
            i + 1, kGprNames[i + 1], (unsigned long long)b.u64x[1], (unsigned long long)b.u64x[0]);
    }
    emit("  lo = %016llx:%016llx   hi = %016llx:%016llx\n",
        (unsigned long long)ctx->lo.u64x[1], (unsigned long long)ctx->lo.u64x[0],
        (unsigned long long)ctx->hi.u64x[1], (unsigned long long)ctx->hi.u64x[0]);
    emit("  sa = 0x%08x  fcr31 = 0x%08x\n", ctx->sa, ctx->fcr31);
    /* Guest stack scan: words in [sp, sp+0x800) that look like text
     * addresses. Not a real unwind, but with ra it usually names the
     * caller chain well enough to locate a fault. */
    uint32_t sp = (uint32_t)ctx->r[29].u64x[0];
    if (sp >= 0x1000 && sp < 0x02000000) {
        emit("[icorecomp][regs] stack scan from sp=0x%08x:\n", sp);
        int shown = 0;
        for (uint32_t a = sp & ~3u; a < sp + 0x800 && shown < 24; a += 4) {
            uint8_t* page = g_pages[a >> 16];
            if (!page) break;
            uint32_t v;
            std::memcpy(&v, page + (a & 0xFFFF), 4);
            if (v >= 0x00100000 && v < 0x00300000 && (v & 3) == 0) {
                emit("    [sp+0x%03x] 0x%08x\n", a - sp, v);
                ++shown;
            }
        }
    }
}

void rt_log_hold_console() {
    /* Reached from rt_fatal and from the crash handlers. Whatever else
     * happens next, the log is complete from here on. */
    rt_log_drain();
    /* Never wait for a keypress on a thread that is not the main one. The GS
     * command ring's worker (gs/gs_threaded.cpp) can raise a fatal, and
     * blocking it here would leave the process alive with a window that
     * still looks live: the EE thread would keep waiting for a field the
     * worker is never going to finish. The main thread reaches its own exit
     * path and holds the console there instead. */
    if (!rt_log_on_main_thread()) {
        emit("[icorecomp][log] fatal on a non-main thread; not holding the console\n");
        return;
    }
#ifdef _WIN32
    if (g_console_present) {
        /* Only when this process owns the console, i.e. debug.console
         * allocated one for a double-clicked run: closing would otherwise
         * erase the failure before it can be read. A run launched from cmd
         * or a CI job has other processes on the console list and must not
         * block. */
        DWORD pids[4];
        DWORD n = GetConsoleProcessList(pids, 4);
        if (!g_console_owned && n != 1) return;
        if (const char* path = rt_log_path()) {
            CONSOLE_PRINTF("\nThe full log for this run is in %s\n", path);
        }
        CONSOLE_PRINTF("Press Enter to close this window.\n");
        (void)std::getchar();
        return;
    }

    /* No console: the default for a double-clicked ico.exe. Nothing this
     * process has written is visible anywhere but the log file, so the
     * failure goes in a message box. The launcher window reports the boot
     * failures it can (rt_boot_precheck, see main.cpp); this is for
     * everything that gets past it, and for the failures that happen
     * before there is a window at all. */
    std::string body = failure_text_copy();
    if (body.empty()) body = "ICO stopped. No failure message was recorded.\n";
    if (const char* path = rt_log_path()) {
        body += "\nThe full log for this run is in:\n";
        body += path;
        body += "\n";
    } else {
        body += "\nThis run kept no log file (debug.log_file is off, or no writable"
                " location was found).\n";
    }
    /* MessageBoxW, not MessageBoxA: a log path can hold characters the
     * active code page cannot spell, and a box that mangles the path a
     * user is being sent to is worse than no box. */
    int wide = MultiByteToWideChar(CP_UTF8, 0, body.c_str(), (int)body.size(), nullptr, 0);
    if (wide > 0) {
        std::vector<wchar_t> wbuf((size_t)wide + 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, body.c_str(), (int)body.size(), wbuf.data(), wide);
        MessageBoxW(nullptr, wbuf.data(), L"ICO stopped", MB_OK | MB_ICONERROR);
    }
#endif
}

[[noreturn]] void rt_fatal(const char* component, const R5900Context* ctx, const char* fmt, ...) {
    char line[3072];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    /* The FATAL line goes to the file first, written and flushed by this
     * thread, before the drain, before the message box and before the exit.
     *
     * It used to be emitted after rt_log_drain() instead, on the reasoning
     * that the drain puts logging back on the calling thread and everything
     * after it is therefore synchronous. That reasoning has a hole, and
     * three runs on Windows fell through it: every one ended with the
     * failure box on screen and no FATAL line in the log at all, whichever
     * backend was in use. rt_log_drain is bounded and gives up rather than
     * hang, so a writer that is slow or wedged leaves it returning with the
     * ring still full; and its own g_async exchange races a producer on
     * another thread that has already passed the check, which can restart
     * the asynchronous path underneath the very line that must not be
     * queued. Neither is a theory worth carrying on the one path where
     * losing the line costs the whole diagnosis.
     *
     * rt_log_write_sync has no such hole: it does not touch g_async, it
     * try-locks rather than waits, and it flushes before returning. Whatever
     * thread raises the fatal, and whatever the writer is doing, the line is
     * on disk by the end of this statement. The drain that follows is then
     * only about the lines queued BEFORE the fatal, which are a bonus rather
     * than the point. */
    {
        char full[3400];
        int n = std::snprintf(full, sizeof(full), "[icorecomp][%s][error] FATAL: %s\n",
                              component, line);
        if (n > 0) rt_log_write_sync(full);
    }
    /* Recorded as well as written, so a run with no console can show it in
     * rt_log_hold_console's message box. Its own buffer, its own mutex. */
    {
        char full[3400];
        int n = std::snprintf(full, sizeof(full), "[%s] FATAL: %s\n", component, line);
        if (n > 0) record_failure(full, (size_t)n < sizeof(full) ? (size_t)n : sizeof(full) - 1);
    }
    /* Now the backlog: everything the run had queued before the fatal, put
     * behind it in the file, and logging on the calling thread from here so
     * the register dump and the end-of-run summary are synchronous too. */
    rt_log_drain();
    if (ctx) rt_dump_registers(ctx);
    /* Published before the exit so an atexit handler that has to end the
     * process on its own still reports a failure; see rt_fatal_exit_code. */
    g_fatal_exit_code.store(1, std::memory_order_release);
    /* The end-of-run block, written here rather than left to the atexit
     * chain: a fatal is the case where the reason is known exactly, and
     * rt_log_hold_console below is about to show the user a message box
     * built from the failure text the summary contributes to. std::exit
     * would reach the atexit fallback too, but only after the box has
     * already been shown, which is too late for the one reader who needs
     * it. rt_run_summary is idempotent, so the later atexit call is a
     * no-op. */
    rt_run_set_exit_reason(false, "fatal in %s: %s", component, line);
    rt_run_summary();
    rt_log_hold_console();
    std::exit(1);
}
