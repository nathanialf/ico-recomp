/* runtime.h: internal declarations shared between the src/runtime .cpp files.
 *
 * This is NOT part of the ABI contract (that is include/recomp_*.h). It only
 * needs to be consistent within this runtime's own translation units.
 */
#ifndef ICORECOMP_RUNTIME_H
#define ICORECOMP_RUNTIME_H

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "recomp_api.h"
#include "recomp_context.h"

/* The programmed CRT video mode and the field timeline derived from it.
 * Included here because the field period is read from the EE kernel, the
 * pad, the sound engine, the profiler and the frame pacer alike. */
#include "video_mode.h"

/* ---- memory (mem.cpp) --------------------------------------------------- */

/* 32 MB, matching the retail EE. Shared with main.cpp for initial $sp setup. */
constexpr uint32_t RT_RAM_SIZE = 32u * 1024 * 1024;

/* Allocates EE RAM, scratchpad and VU memory windows and populates g_pages.
 * Must run before the loader or any guest code executes. */
void rt_mem_init();

/* Sentinel written into $ra before the entry call so a top-level `jr $ra`
 * (translated as rt_call_indirect with this target) is recognized as a clean
 * program exit instead of a bad-indirect fault. Chosen outside every mapped
 * range (RAM/aliases/scratchpad/VU window/MMIO) and outside the function
 * table's valid vram range, so it can never collide with a real target. */
constexpr uint32_t RT_CLEAN_EXIT_VRAM = 0xE0000000u;

/* ---- logging (log.cpp) -------------------------------------------------- */

/* Format-string checking for printf-style helpers. gnu_printf rather than
 * printf: on mingw-w64 the printf archetype means the Microsoft CRT's
 * format set, which has no %zu, and the warning it raises for size_t
 * conversions is spurious because mingw-w64's C++ mode routes vsnprintf
 * through its own ANSI implementation. MSVC has no equivalent attribute. */
#if defined(__GNUC__)
#define RT_PRINTF_FORMAT(fmt_idx, first_arg) __attribute__((format(gnu_printf, fmt_idx, first_arg)))
#else
#define RT_PRINTF_FORMAT(fmt_idx, first_arg)
#endif

/* ---- levels -------------------------------------------------------------
 *
 * Four levels, lowest to highest. A line is emitted when its level is at or
 * above the level the run is configured for (debug.log_level, default
 * warn, environment twin ICORECOMP_LOG_LEVEL), so an error survives every
 * setting and a debug line has to be asked for.
 *
 * Which level a site gets:
 *   error  the operation did not happen at all (a file that failed to
 *          write, a fatal about to end the run)
 *   warn   it happened differently from what was asked: a refusal, a
 *          fallback, an out-of-range or unknown value, a missing file, an
 *          expired fact, anything that changes what the user should do
 *   info   startup facts, applied settings, device and backend identity,
 *          one-time summaries, profile reports and counters
 *   debug  per-field, per-packet, flood-controlled and trace lines
 *
 * ICORECOMP_VERBOSE keeps its own meaning on top of this: a channel named
 * there passes its debug lines whatever the level is (rt_verbose below).
 *
 * Destination: error, warn and info go to the log file and to the console
 * echo. Debug lines go to the log file only, as the verbose channels
 * always have, so turning the level down to debug does not make the
 * console unusable. */
enum RtLogLevel {
    RT_LOG_DEBUG = 0,
    RT_LOG_INFO  = 1,
    RT_LOG_WARN  = 2,
    RT_LOG_ERROR = 3,
};

/* The level in force. Read directly by rt_log_level_enabled below so a hot
 * path pays a relaxed load and a compare rather than a call; written by
 * rt_log_set_level from the main thread while the GS worker may be
 * reading, which is why it is an atomic and not a plain int. Not part of
 * the API: call the functions. */
extern std::atomic<int> g_rt_log_level;

/* True when a line at `level` would be emitted. Cheap enough to sit in
 * front of an argument list that costs something to build. */
inline bool rt_log_level_enabled(RtLogLevel level) {
    return (int)level >= g_rt_log_level.load(std::memory_order_relaxed);
}

/* Same, plus the ICORECOMP_VERBOSE channel spec: a debug line whose component
 * is a named channel passes even when the level is warn or error. */
bool rt_log_level_enabled_for(RtLogLevel level, const char* component);

/* Sets the level for the rest of the run. Hot: debug.log_level applies
 * from the settings menu without a restart. */
void rt_log_set_level(RtLogLevel level);
RtLogLevel rt_log_get_level();

/* "error", "warn", "info" or "debug". */
const char* rt_log_level_name(RtLogLevel level);
/* Parses one of those four names. Returns false and leaves *out alone for
 * anything else, so a caller keeps its default and logs the bad value. */
bool rt_log_level_parse(const char* name, RtLogLevel* out);

/* Opens this run's log file and points file descriptor 2 at it, so the log
 * survives the console window dying with the process. Path comes from
 * ICORECOMP_LOG; unset it defaults to <dir>/icorecomp.log, falling back to
 * the per-user state directory and then the temp directory when <dir> is
 * not writable. ICORECOMP_LOG=- opts out. On POSIX the sink stays off
 * unless ICORECOMP_VERBOSE or ICORECOMP_LOG asks for it. file_allowed is
 * debug.log_file (peeked from settings.json before settings can log; see
 * rt_settings_peek_log_file): false behaves as ICORECOMP_LOG=- unless
 * ICORECOMP_LOG is set, in which case the env var decides and a line logs
 * that the settings value was ignored. console_wanted is debug.console,
 * peeked the same way; see rt_console_init, which this calls first so the
 * prologue below has somewhere to land. Also parses ICORECOMP_VERBOSE and
 * ICORECOMP_LOG_LEVEL, so call it before anything else logs.
 *
 * The prologue it writes (build stamp and exe identity, log path, base
 * directory, the level and channels in force) is unconditional: it is what
 * says which build produced a log, and a level that hid it would make
 * every other line unattributable. */
void rt_log_init(const char* dir, bool file_allowed, bool console_wanted);

/* Sets the level rt_log_init starts from, and the level a program that
 * never calls rt_log_init runs at. The standalone tools and the selftests
 * call this with RT_LOG_INFO before anything else: their progress lines
 * are the output, and the runtime's warn default would hide them.
 * ICORECOMP_LOG_LEVEL still wins over it inside rt_log_init. */
void rt_log_set_initial_level(RtLogLevel level);

/* Attaches to the console this process was launched from, or allocates one
 * when `want_alloc` (debug.console) asks for it, and points stdout and
 * stderr at whichever it got. On Windows only, and only meaningful now
 * that ico.exe is a GUI-subsystem binary: a double-clicked run has no
 * console at all unless this allocates one, and a run started from cmd or
 * PowerShell keeps writing to that shell's console. A no-op everywhere
 * else, where stderr is already the echo. Called by rt_log_init. */
void rt_console_init(bool want_alloc);

/* True when this process has a console to echo to (attached or allocated).
 * False on a double-clicked Windows run with debug.console off, which is
 * why the fatal paths fall back to a message box. */
bool rt_console_present();

/* True when ICORECOMP_VERBOSE named this component (or
 * "all"), or when the level is debug, which enables every channel. Parsing
 * is a short list walk, so hot callers may cache the answer in a static.
 * Defined as "a debug line from this component would be emitted", i.e.
 * rt_log_level_enabled_for(RT_LOG_DEBUG, component).
 *
 * Caching makes that channel restart-only: rt_log_set_verbose below
 * replaces the set on a live edit, and a cached answer will not see it.
 * Five callers cache today, and their channels are documented as
 * restart-only in docs/SETTINGS.md: "present" (gs/gs_parallel.cpp),
 * "widescreen" (hw/gif.cpp) and "geom" (hw/gif.cpp, hw/vu1rt.cpp,
 * hw/gspriv.cpp). A new caching site has to be added there too. */
bool rt_verbose(const char* component);

/* Parses `spec` exactly like ICORECOMP_VERBOSE at startup ("-", "0" or
 * "none" clears every channel) and replaces the enabled-channel set with
 * it. Only adjusts what rt_verbose() answers; never touches the log file or
 * which sink a line goes to (that is fixed at rt_log_init). Called at most
 * once at startup, from main.cpp, and only when ICORECOMP_VERBOSE itself is
 * unset, because the environment variable is the only source of the channel set.
 * The settings UI calls it again on a live edit, from the main thread,
 * while other threads are reading the set; log.cpp publishes it as an
 * immutable snapshot for that reason. A channel whose callers cache the
 * answer (see rt_verbose) does not change until the next start. */
void rt_log_set_verbose(const char* spec);

/* Nudges the log writer thread so a field's lines reach the file promptly.
 * Does no I/O and never waits, so it costs the frame path nothing; the
 * writer flushes on its own whenever it drains the queue empty. Called
 * once per field. */
void rt_log_flush();

/* Drains everything queued to the log file and then puts logging back on
 * the calling thread for the rest of the process. For fatal paths and
 * state dumps: after this returns, every later log line is written and
 * flushed before its call returns, so a handler that leaves through
 * std::_Exit (which runs no atexit) still produces a complete log.
 * Idempotent, and bounded: it gives up waiting rather than hanging a
 * process that is already on its way down. */
void rt_log_drain();

/* Path of the open log file, or null when logging is console only. */
const char* rt_log_path();

/* On Windows, when this process owns its console (a double-clicked run
 * with debug.console on), names the log file and waits for Enter so the
 * failure stays readable. With no console at all, which is what a
 * double-clicked run is by default, puts the failure and the log path in
 * a message box instead, because otherwise it would reach nobody. No-op
 * for runs launched from an existing shell (their console outlives the
 * process), on POSIX, and on any thread but the main one (a fatal on a
 * worker must not block, since nothing else can end the process while it
 * does). */
void rt_log_hold_console();

/* True on the thread that called rt_log_init, which is main's first
 * statement: the process's main thread, and so the EE thread. */
bool rt_log_on_main_thread();

/* Writes one already-composed line to the log file on the calling thread,
 * bypassing the writer thread and its ring entirely, and flushes the file
 * before returning. For a crash handler, which cannot rely on another
 * thread ever running again: the fault may have happened inside the writer,
 * or on a thread holding the lock the writer needs. It never blocks on the
 * I/O mutex (it try-locks and writes regardless), because a dump that
 * deadlocks is worse than one whose lines interleave.
 *
 * Not a replacement for rt_log_drain(). Lines queued before the fault are
 * still in the ring afterwards, so a handler writes its synchronous block
 * first and drains after it, and the file reads "the crash, then whatever
 * the run had queued behind it". That order is deliberate: the crash lines
 * have to survive even when the drain never finishes.
 *
 * `text` is written as given, newline included; rt_log_sync formats and
 * adds the "[icorecomp][component]" prefix and the newline. */
void rt_log_write_sync(const char* text);
void rt_log_sync(const char* component, const char* fmt, ...) RT_PRINTF_FORMAT(2, 3);

/* Appends text to the failure block a run with no console shows in its
 * message box (rt_log_hold_console). Error-level lines and rt_fatal's own
 * message land there on their own; this is for the end-of-run summary,
 * whose reason and log path have to be in the box the user is actually
 * looking at. Capped like every other writer of that block. */
void rt_log_record_failure_text(const char* text);

/* Test hook for the synchronous crash-write path. Runs exactly what a
 * crash handler runs (rt_log_write_sync, then a drain) with a caller-
 * supplied reason, and returns instead of ending the process, so the
 * selftest can read the file back. Not called by the runtime itself. */
void rt_log_crash_write_selftest(const char* reason);

/* The status a fatal now in progress is exiting with, or -1 when none is.
 * For an atexit handler that cannot let std::exit finish and has to end the
 * process itself: without this it would have to guess, and guessing success
 * turns a crash into a clean run in every script that reads the status. */
int rt_fatal_exit_code();

/* The four line-emitting entry points. Each checks its own level first, so
 * a caller only needs rt_log_level_enabled when building the arguments is
 * itself expensive. rt_log_debug also passes when ICORECOMP_VERBOSE names the
 * component, which is what rt_verbose reports. */
void rt_log_error(const char* component, const char* fmt, ...) RT_PRINTF_FORMAT(2, 3);
void rt_log_warn(const char* component, const char* fmt, ...) RT_PRINTF_FORMAT(2, 3);
void rt_log_info(const char* component, const char* fmt, ...) RT_PRINTF_FORMAT(2, 3);
void rt_log_debug(const char* component, const char* fmt, ...) RT_PRINTF_FORMAT(2, 3);
void rt_vlog(RtLogLevel level, const char* component, const char* fmt, va_list ap);

/* Register dump, shared by rt_break, rt_bad_indirect, the crash handler, and
 * unimplemented-strict-mode fatals. */
void rt_dump_registers(const R5900Context* ctx);

/* The guest function whose body covers `vram`, from the translator's
 * generated function-name table (hooks.cpp), or null when nothing does:
 * outside the translated range, or a stub build with no table linked. The
 * names in that table are the disc listing's own, plus a provisional
 * func_XXXXXXXX where the translator placed no donor function. `entry_out`,
 * when given, receives that function's entry address so a caller can print
 * the offset into it. */
const char* rt_guest_func_name(uint32_t vram, uint32_t* entry_out);

/* Prints a component-tagged fatal message (+ optional register dump when ctx
 * is non-null) and terminates the process with a nonzero exit code. Never
 * returns. */
[[noreturn]] void rt_fatal(const char* component, const R5900Context* ctx, const char* fmt, ...);

/* ---- run state and the end-of-run summary (host/run_state.cpp) ----------
 *
 * Why this exists: a run on display.backend = d3d12 ended with an error on
 * screen and a log whose last line was an ordinary HLE warning. Nothing in
 * the file said what ended the run. The rule that follows from that is
 * simple and absolute: the log at the default level always says how a run
 * ended and why.
 *
 * Two halves. The phase is a monotonic state machine every subsystem
 * advances with one call as it passes its own milestone, so a run that dies
 * early is placed by the last phase it reached rather than by guesswork
 * over which lines are missing. The summary is one block, written exactly
 * once, by whichever exit path gets there first: rt_fatal, the SEH filter,
 * std::terminate, the POSIX signal handlers, the window-closed path, the
 * restart path, and an atexit handler that catches every path that named no
 * reason at all.
 *
 * Level: warn when the end was not a user quit, info when it was. A
 * deliberate end is not news; anything else is.
 */

enum RtRunPhase {
    RT_PHASE_START = 0,        /* nothing has happened yet */
    RT_PHASE_LOG_INIT,         /* the log sink is open */
    RT_PHASE_SETTINGS,         /* settings.json is loaded and applied */
    RT_PHASE_BACKEND_CREATED,  /* a GS backend object exists */
    RT_PHASE_WINDOW_CREATED,   /* the executable's one window is open */
    RT_PHASE_LAUNCHER_SHOWN,   /* the launcher is drawing */
    RT_PHASE_GUEST_BOOTED,     /* the scheduler is running translated code */
    RT_PHASE_FIRST_FIELD,      /* the first field boundary was reached */
    RT_PHASE_FIRST_PRESENT,    /* something reached the swapchain */
    RT_PHASE_GAMEPLAY,         /* fields have been running for a while */
    RT_PHASE_COUNT,
};

/* Advances the phase. Monotonic: a lower phase than the one already reached
 * is ignored, so the order of the calls decides nothing and a subsystem that
 * runs twice costs nothing. Logs the transition at info. Safe from any
 * thread; safe before rt_log_init. */
void rt_run_phase(RtRunPhase reached);
RtRunPhase rt_run_phase_now();
const char* rt_run_phase_name(RtRunPhase p);

/* Records why the run is ending. The first caller wins: the reason nearest
 * the cause is the one worth keeping, and every later path (the atexit
 * fallback in particular) is a generalisation of it. `user_quit` is true
 * only for a deliberate end (Quit from the menu or the launcher, the window
 * closed, a restart to apply a cold key, the guest's own Exit syscall, a
 * bounded diagnostic run reaching its field limit); it selects info rather
 * than warn for the summary. */
void rt_run_set_exit_reason(bool user_quit, const char* fmt, ...) RT_PRINTF_FORMAT(2, 3);
bool rt_run_exit_reason_known();

/* Writes the end-of-run block. Idempotent: the first call wins and every
 * later one returns without a word, so the crash handlers, the window-closed
 * path and the atexit fallback can all call it unconditionally. Never
 * fatal, never blocks on anything but the log. */
void rt_run_summary();

/* The reason line the summary printed (or would print), for the fatal
 * message box. Never null; "unknown" before a reason is set. Copies into
 * `buf`. */
void rt_run_reason_text(char* buf, size_t buf_len);

/* ---- what the summary reports -------------------------------------------
 *
 * Push, not pull: every producer stores a word, so the summary and the
 * watchdog read plain atomics and never call into a subsystem that may be
 * the one that is stuck. Each of these is a relaxed store of one value and
 * costs nothing measurable where it is called. The string arguments must
 * have static lifetime; the pointer is what is stored. */
void rt_run_note_field();                 /* one field boundary completed */
void rt_run_note_present();               /* one present reached the swapchain */
void rt_run_note_gif();                   /* one GIF packet was submitted */
void rt_run_note_syscall(int num, const char* name);
void rt_run_note_rpc(const char* what, uint32_t fno);
void rt_run_note_gs_record(const char* kind);      /* last ring record replayed */
void rt_run_note_gs_worker(const char* state);     /* "parked", "in a present", ... */
void rt_run_note_gs_queued(uint64_t bytes, uint64_t records_replayed);
void rt_run_note_rhi(const char* state);           /* last submitted command list */
void rt_run_note_window_minimized(bool minimized);

/* ---- the field watchdog -------------------------------------------------
 *
 * Started once the guest has booted, stopped by the summary. A separate
 * thread, because the thread it is watching is the one that can be stuck:
 * if no field completes for five seconds it says so at warn, with the phase,
 * the GS worker's state, the last submitted command list and whether the
 * window is minimised, and repeats every thirty seconds while the stall
 * lasts. It never kills the process: a run that is only very slow must not
 * be ended by its own instrument. */
void rt_run_watchdog_start();
void rt_run_watchdog_stop();

/* ---- the guest inventory, on demand -------------------------------------
 *
 * The thread/semaphore/RPC inventory (ee/sched.cpp rt_sched_dump_inventory)
 * is the only thing that names a wait, and the run that needed it most was
 * one where nothing was wrong enough to fire the deadlock fatal: fields kept
 * advancing at 50 Hz because the vblank handler woke one thread each field,
 * while every other thread sat in a wait nothing was ever going to end. The
 * watchdog is the instrument that notices that, and the inventory is what
 * turns "no GIF traffic for 10 s" into a named wait.
 *
 * Two entry points because the watchdog runs on its own thread and the
 * scheduler's tables are the EE thread's:
 *
 *   rt_run_request_inventory  stores the reason and returns at once. Safe
 *                             from any thread. The EE scheduler loop picks
 *                             it up at its next pass and does the dump on
 *                             its own thread, so nothing walks a deque that
 *                             another thread is editing.
 *   rt_run_take_inventory_request  the scheduler side of that: returns the
 *                             pending reason (and clears it) or null.
 *
 * The summary path is different: by then the watchdog is joined and the GS
 * worker is joined, so the dump can be direct. main.cpp registers
 * rt_sched_dump_inventory as the hook after rt_sched_init; a build that
 * links run_state.cpp without the scheduler (the log selftest) leaves it
 * null and the summary simply has no inventory in it. */
void rt_run_request_inventory(const char* why);
const char* rt_run_take_inventory_request();
void rt_run_set_inventory_hook(void (*fn)(const char* why));

/* ---- thread names (host/run_state.cpp) ----------------------------------
 *
 * A name for the calling thread, for the crash report and the summary. Not
 * an OS thread name: this process has three threads that matter (the EE
 * thread, the GS command ring's worker, the log writer) and a crash dump
 * that says which one faulted is the difference between reading the right
 * file and reading all of them. Set once per thread, at the top of its
 * entry function. */
void rt_thread_set_name(const char* name);
const char* rt_thread_name();

/* ---- crash report (host/crash_report.cpp) -------------------------------
 *
 * Everything a crash handler writes before it does anything else, written
 * synchronously through rt_log_write_sync so it is on disk even when the
 * writer thread is the one that died: the fault, its address, the module
 * and offset the fault address falls in, a backtrace with module and offset
 * per frame, the faulting CPU registers when the platform hands them over,
 * the current guest pc_hint and the calling thread's name.
 *
 * Windows resolves modules with GetModuleHandleExW on the address itself
 * rather than walking EnumProcessModules, and captures frames with
 * CaptureStackBackTrace: both are kernel32, so a crash dump never depends
 * on DbgHelp being present or on symbols being installed. POSIX uses
 * dladdr and backtrace().
 *
 * `code` is the SEH exception code or the signal number, `fault_pc` the
 * faulting instruction address (null when the platform does not say), and
 * `access_addr` the address a memory fault touched (null otherwise).
 * `platform_ctx` is the EXCEPTION_POINTERS on Windows and the ucontext_t on
 * POSIX, or null; it is only read for register values. `detail` is free
 * text (an exception's what(), say) or null.
 *
 * Never returns a failure and never throws: every step degrades to a line
 * saying it could not be taken. */
void rt_crash_report(const char* what, uint64_t code, const void* fault_pc,
                     const void* access_addr, const void* platform_ctx,
                     const char* detail);

/* Reserves stack for the crash report on the calling thread, and must be
 * called near the top of every thread that can fault.
 *
 * A stack overflow is the one fault where the handler has to run on a stack
 * that has just been declared exhausted. Windows unwinds the guard page and
 * hands the filter whatever is left, which by default is not enough for the
 * formatting buffers a crash report needs: the filter faults again and the
 * process dies with nothing written, which reads exactly like the process
 * "just ending". SetThreadStackGuarantee reserves the room in advance.
 *
 * A no-op on POSIX, where the equivalent is an alternate signal stack this
 * runtime does not install; that gap is stated rather than papered over. */
void rt_crash_reserve_stack();

/* ---- sha1 (sha1.cpp) ----------------------------------------------------- */

struct Sha1Digest {
    uint8_t bytes[20];
};

Sha1Digest rt_sha1_file(const char* path, bool* ok);
Sha1Digest rt_sha1_buffer(const uint8_t* data, size_t len);
/* Lowercase hex, no separators; buf must hold at least 41 bytes. */
void rt_sha1_to_hex(const Sha1Digest& d, char* buf);
/* Case-insensitive compare against a 40-hex-char string. */
bool rt_sha1_equals_hex(const Sha1Digest& d, const char* hex40);

/* ---- loader (loader.cpp) ------------------------------------------------- */

struct LoaderConfig {
    /* [decomp] */
    char decomp_root[512] = {0};
    char decomp_elf[512] = {0};
    /* [pins] */
    char elf_sha1[64] = {0};
    /* [target] */
    uint32_t entry = 0;
    uint32_t vram_base = 0;
    uint32_t gp = 0;
};

/* Base directory for config files and relative paths (the target's config
 * file, config/local.toml, saves/). ICORECOMP_SOURCE_ROOT when that config
 * file exists there (a dev checkout); otherwise the executable's own
 * directory, so a packaged binary resolves everything against itself. */
const char* rt_base_dir();

/* Reads <base>/config/recomp.toml (see rt_base_dir). When the file does not
 * exist (packaged runtime, no dev tree) the committed [pins]/[target]
 * values are filled in as compiled-in defaults and the boot ELF comes from
 * the disc instead (see rt_load_elf); that path still returns true. Returns
 * false and logs a fatal-quality message (does not exit) only when a
 * present config is missing required keys, so callers can decide how to
 * fail. */
bool rt_load_config(LoaderConfig* out);

/* Resolves decomp_root/decomp_elf into an absolute path. buf must hold at
 * least 1024 bytes. */
void rt_resolve_elf_path(const LoaderConfig& cfg, char* buf, size_t buf_size);

/* Verifies the SHA-1 pin, then loads the single PT_LOAD segment into guest
 * RAM (via g_pages) at its vaddr and zeroes bss (memsz - filesz). The ELF
 * bytes come from the decomp checkout when the config names one and the
 * file exists, otherwise from SCES_507.60 on the mounted disc image (same
 * bytes, same pin). Fatal on any failure.
 *
 * After a successful rt_boot_precheck this reuses the ELF image the
 * precheck already read (matched on the same source string), so the file or
 * disc read happens once per run. */
void rt_load_elf(const LoaderConfig& cfg);

/* Everything main does between rt_load_config and the entry lookup, run for
 * its failures rather than its effects: config load, disc mount (the same
 * probe list rt_iso_mount walks, non-fatal), boot ELF acquisition, the
 * SHA-1 pin, ELF header sanity, and the entry-function lookup. Returns
 * false with one human-readable line in `err` where those paths would
 * rt_fatal, so the launcher can report the problem in its window instead of
 * dying before it can draw. err may be null.
 *
 * On success the disc is left mounted and the ELF image is cached for the
 * rt_load_elf that follows. Nothing is written into guest memory: the
 * PT_LOAD segments are still loaded by rt_load_elf.
 *
 * Defined only in the full runtime executable: it reads g_functab, which
 * lives in mem.cpp, and the standalone selftests that link loader.cpp do
 * not link mem.cpp (see the gate in loader.cpp). */
bool rt_boot_precheck(char* err, size_t err_len);

/* ---- main (main.cpp) ---------------------------------------------------- */

/* True when the run was started with --no-launcher. */
bool rt_no_launcher_flag();

/* Puts one block of text where the user will see it: the console when there
 * is one, and a message box when there is not. ico.exe is a GUI-subsystem
 * binary on Windows, so a double-clicked run has no console unless
 * debug.console asked for one, and a printf there reaches nobody. Used for
 * --help, for a bad argument, and for a restart that could not spawn the
 * new process. No-op on POSIX when there is a console to print to, which
 * there always is. */
void rt_show_message(const char* title, const char* text);

/* Restarts this executable to apply a cold settings key (host/settings.h).
 *
 * Two steps, and the split is the point. This call only prepares the new
 * process: on Windows it CreateProcessW's the running executable with the
 * same command line and working directory in CREATE_SUSPENDED, on POSIX it
 * resolves the executable's own path and checks it is still executable.
 * Nothing is torn down yet, so a failure here returns false with a reason
 * in `err` and the caller keeps running, with the new value saved for the
 * next launch and the old one still in force. On success it asks for the
 * ordinary exit (rt_request_exit, naming `why`), so the run ends through
 * the same shutdown every quit takes: the GS backend's teardown writes its
 * pipeline cache, the window is destroyed, the log is drained. The new
 * process is only resumed (Windows) or exec'd (POSIX) from an atexit
 * handler main registers before every other one, so it runs after all of
 * them and nothing is still being flushed when the successor starts.
 *
 * Returns true when the restart is under way, in which case the caller must
 * not assume it returns at all: with no window (dump or headless runs)
 * rt_request_exit exits from inside this call. `err` may be null. */
bool rt_restart_now(const char* why, char* err, size_t err_len);

#endif /* ICORECOMP_RUNTIME_H */
