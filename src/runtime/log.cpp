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
 * Verbosity: ICORECOMP_VERBOSE names the components whose high-volume
 * diagnostic channel is on ("vu1,geom", or "all"). Those lines go to the
 * log file and not to the console echo, so the file can carry a full
 * trace while the console stays readable. Setting it also turns the file
 * sink on everywhere, since a trace with nowhere to go is worse than
 * none.
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
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

/* ICORECOMP_VERBOSE, parsed once by rt_log_init. */
bool g_verbose_all = false;
std::vector<std::string> g_verbose_tags;

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
 *                    is in place (stderr is the log file then), g_logfile
 *                    when it is not, and stderr again when there is no log
 *                    file at all, in which case stderr is still the
 *                    console and is the only sink there is.
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

std::mutex g_mu;                 /* ring, counters, flags. Never held across I/O. */
std::condition_variable g_cv;    /* producer -> writer */
std::condition_variable g_idle;  /* writer -> rt_log_drain */
std::mutex g_io_mu;              /* serializes the two FILE* sinks */
std::thread g_writer;
std::once_flag g_writer_once;

/* False until the writer thread is up, and false again once a fatal path
 * has drained it. Both states mean "write on the calling thread". */
std::atomic<bool> g_async{false};

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
        start_writer();
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

void parse_verbose(const char* spec) {
    std::string cur;
    for (const char* p = spec;; ++p) {
        if (*p == ',' || *p == ' ' || *p == '\0') {
            if (!cur.empty()) {
                if (cur == "all" || cur == "1") g_verbose_all = true;
                else g_verbose_tags.push_back(cur);
                cur.clear();
            }
            if (*p == '\0') break;
        } else {
            cur.push_back(*p);
        }
    }
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

void rt_log_init(const char* dir) {
    /* This is main's first statement, so in practice nothing has logged
     * yet and no writer thread exists. It is still cheap to be certain:
     * park the writer for the whole of this function, not just up to the
     * first store. g_console_sink, g_logfile, stderr's buffering and fd 2
     * all move below, and they are plain globals rather than atomics, so a
     * writer waking mid-swap would read them torn. Parking has to cover
     * every exit, and this function has several early returns, so it is a
     * guard rather than a call. */
    struct AsyncPause {
        bool resume;
        AsyncPause() : resume(g_async.exchange(false, std::memory_order_acq_rel)) {
            wait_writer_idle();
        }
        ~AsyncPause() {
            if (resume) g_async.store(true, std::memory_order_release);
        }
    } pause_writer;

    const char* vspec = std::getenv("ICORECOMP_VERBOSE");
    /* "-", "0" or "none" turns the channels off, including the defaults. */
    const bool verbose_off = vspec && (std::strcmp(vspec, "-") == 0
        || std::strcmp(vspec, "0") == 0 || std::strcmp(vspec, "none") == 0);
    if (vspec && !verbose_off) parse_verbose(vspec);
    const bool verbose = g_verbose_all || !g_verbose_tags.empty();

    const char* env = std::getenv("ICORECOMP_LOG");
    const std::string base = (dir && *dir) ? dir : ".";

    /* Candidates, in the order they are tried. An explicit ICORECOMP_LOG is
     * the user's choice and gets no fallbacks: silently writing somewhere
     * else would be worse than saying it failed. */
    std::vector<std::string> candidates;
    if (env && *env) {
        /* "-" or "0" opts out: console only, the pre-sink behavior. */
        if (std::strcmp(env, "-") == 0 || std::strcmp(env, "0") == 0) {
            if (verbose) {
                std::fprintf(stderr, "[icorecomp][log] ICORECOMP_LOG=%s disables the log file;"
                    " ICORECOMP_VERBOSE output goes to the console\n", env);
            }
            return;
        }
        candidates.push_back(env);
    } else {
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
    for (const std::string& c : candidates) {
        std::string why;
        f = try_open(c, &why);
        if (f) {
            path = c;
            break;
        }
        /* stdout, not stderr: stderr is about to become the log file, and
         * on the failure path there is no log file to read this in. The
         * console is the only place this can land. */
        std::fprintf(stdout, "[icorecomp][log] could not open '%s' for writing (%s)\n",
            c.c_str(), why.c_str());
        std::fflush(stdout);
    }
    if (!f) {
        std::fprintf(stdout, "[icorecomp][log] no writable log location; this run logs to the"
            " console only, and the console does not survive the window closing\n");
        std::fflush(stdout);
        return;
    }

    int saved = rt_dup(2);
    std::fflush(stderr);
    if (rt_dup2(rt_fileno(f), 2) < 0) {
        /* No redirect, so output from the GS library, SDL and the Vulkan
         * loader stays on the console. The runtime's own log is the part
         * that matters and it keeps its file: writing through the FILE*
         * directly needs nothing from fd 2. */
        std::fprintf(stdout, "[icorecomp][log] could not point stderr at '%s'; the runtime log still"
            " goes there, but output from the renderer and the Vulkan driver stays on the console\n",
            path.c_str());
        std::fflush(stdout);
        g_logfile = f;
        std::setvbuf(g_logfile, nullptr, _IOFBF, 1 << 16);
        g_console_sink = stderr;
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
    if (!vspec) parse_verbose(kDefaultVerbose);

    /* Announced on stdout as well as into the log. Someone looking for
     * the file needs the answer on the console, where it is visible
     * without already having found the file.
     *
     * The build stamp is here for a specific reason: this package is run
     * off a network share, where a stale client-side copy of the exe is
     * indistinguishable from a fix that did not work. Naming the build in
     * the first line of every run settles that without guesswork. */
    std::fprintf(stdout, "[icorecomp] build " __DATE__ " " __TIME__ " (exe: %s)\n",
        rt_exe_identity().c_str());
    std::fprintf(stdout, "[icorecomp][log] this run's log: %s\n", path.c_str());
    std::fflush(stdout);

    {
        std::string line = "[icorecomp] build " __DATE__ " " __TIME__ " (exe: "
            + rt_exe_identity() + ")\n";
        emit("%s", line.c_str());
    }
    emit("[icorecomp][log] writing this run's log to %s\n", path.c_str());
    emit("[icorecomp][log] base directory for config/, saves/ and the disc probe: %s\n", base.c_str());
    if (g_verbose_all || !g_verbose_tags.empty()) {
        std::string tags = g_verbose_all ? "all" : std::string();
        for (const std::string& t : g_verbose_tags) {
            if (!tags.empty()) tags += ",";
            tags += t;
        }
        emit("[icorecomp][log] verbose channels enabled: %s (file only, not echoed to the console;"
            " set ICORECOMP_VERBOSE to change, or ICORECOMP_VERBOSE=none to turn off)\n",
            tags.c_str());
    }
    if (rt_verbose("geom")) {
        emit("[icorecomp][log] the geometry checker is on: every GIF packet is re-parsed and"
            " counted, per microprogram and per MSCAL entry, in the profiler summary."
            " This costs per field, so frame times from this run read high.\n");
    }
}

bool rt_verbose(const char* component) {
    if (g_verbose_all) return true;
    for (const std::string& t : g_verbose_tags) {
        if (t == component) return true;
    }
    return false;
}

void rt_log_set_verbose(const char* spec) {
    g_verbose_all = false;
    g_verbose_tags.clear();
    if (!spec) return;
    const bool off = std::strcmp(spec, "-") == 0 || std::strcmp(spec, "0") == 0
        || std::strcmp(spec, "none") == 0;
    if (!off) parse_verbose(spec);
}

void rt_logv(const char* component, const char* fmt, ...) {
    char line[3072];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    emit_file("[icorecomp][%s] %s\n", component, line);
}

const char* rt_log_path() {
    return g_log_path.empty() ? nullptr : g_log_path.c_str();
}

void rt_vlog(const char* component, const char* fmt, va_list ap) {
    char line[3072];
    std::vsnprintf(line, sizeof(line), fmt, ap);
    emit("[icorecomp][%s] %s\n", component, line);
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

void rt_log(const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlog(component, fmt, ap);
    va_end(ap);
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
#ifdef _WIN32
    /* Only when this process owns the console, i.e. it was double-clicked
     * from Explorer rather than launched from an existing shell: closing
     * would otherwise erase the failure before it can be read. A run
     * launched from cmd or a CI job has other processes on the console
     * list and must not block. */
    DWORD pids[4];
    DWORD n = GetConsoleProcessList(pids, 4);
    if (n != 1) return;
    if (const char* path = rt_log_path()) {
        std::fprintf(stdout, "\nThe full log for this run is in %s\n", path);
    }
    std::fprintf(stdout, "Press Enter to close this window.\n");
    std::fflush(stdout);
    (void)std::getchar();
#endif
}

[[noreturn]] void rt_fatal(const char* component, const R5900Context* ctx, const char* fmt, ...) {
    char line[3072];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    /* Synchronous from here: the FATAL line and the dump must reach the
     * file whether this exits through atexit or not. */
    rt_log_drain();
    emit("[icorecomp][%s] FATAL: %s\n", component, line);
    if (ctx) rt_dump_registers(ctx);
    rt_log_hold_console();
    std::exit(1);
}
