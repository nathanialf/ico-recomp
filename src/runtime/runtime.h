/* runtime.h: internal declarations shared between the src/runtime .cpp files.
 *
 * This is NOT part of the ABI contract (that is include/recomp_*.h). It only
 * needs to be consistent within this runtime's own translation units.
 */
#ifndef ICORECOMP_RUNTIME_H
#define ICORECOMP_RUNTIME_H

#include <cstdarg>
#include <cstddef>
#include <cstdint>

#include "recomp_api.h"
#include "recomp_context.h"

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

/* Opens this run's log file and points file descriptor 2 at it, so the log
 * survives the console window dying with the process. Path comes from
 * ICORECOMP_LOG; unset it defaults to <dir>/icorecomp.log, falling back to
 * the per-user state directory and then the temp directory when <dir> is
 * not writable. ICORECOMP_LOG=- opts out. On POSIX the sink stays off
 * unless ICORECOMP_VERBOSE or ICORECOMP_LOG asks for it. file_allowed is
 * debug.log_file (peeked from settings.json before settings can log; see
 * rt_settings_peek_log_file): false behaves as ICORECOMP_LOG=- unless
 * ICORECOMP_LOG is set, in which case the env var decides and a line logs
 * that the settings value was ignored. Also parses ICORECOMP_VERBOSE, so
 * call it before anything else logs. */
void rt_log_init(const char* dir, bool file_allowed);

/* True when ICORECOMP_VERBOSE named this component (or "all"). Parsing is a
 * short list walk, so hot callers should cache the answer in a static. */
bool rt_verbose(const char* component);

/* Parses `spec` exactly like ICORECOMP_VERBOSE at startup ("-", "0" or
 * "none" clears every channel) and replaces the enabled-channel set with
 * it. Only adjusts what rt_verbose() answers; never touches the log file or
 * which sink a line goes to (that is fixed at rt_log_init). Called at most
 * once at startup, from main.cpp, and only when ICORECOMP_VERBOSE itself is
 * unset -- the environment variable always wins over debug.verbose, so
 * main never calls this when it is set. The settings UI (later milestones)
 * calls it again on a live edit. */
void rt_log_set_verbose(const char* spec);

/* A verbose diagnostic line. Goes to the log file only, never to the
 * console echo, so enabling a channel does not make the console unusable.
 * Callers gate on rt_verbose() first; this does not check again. */
void rt_logv(const char* component, const char* fmt, ...);

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

/* On Windows, when this process owns its console (a double-clicked run),
 * names the log file and waits for Enter so the failure stays readable.
 * No-op everywhere else, for runs launched from an existing shell, and on
 * any thread but the main one (a fatal on a worker must not block, since
 * nothing else can end the process while it does). */
void rt_log_hold_console();

/* True on the thread that called rt_log_init, which is main's first
 * statement: the process's main thread, and so the EE thread. */
bool rt_log_on_main_thread();

/* The status a fatal now in progress is exiting with, or -1 when none is.
 * For an atexit handler that cannot let std::exit finish and has to end the
 * process itself: without this it would have to guess, and guessing success
 * turns a crash into a clean run in every script that reads the status. */
int rt_fatal_exit_code();

void rt_log(const char* component, const char* fmt, ...);
void rt_vlog(const char* component, const char* fmt, va_list ap);

/* Register dump, shared by rt_break, rt_bad_indirect, the crash handler, and
 * unimplemented-strict-mode fatals. */
void rt_dump_registers(const R5900Context* ctx);

/* Prints a component-tagged fatal message (+ optional register dump when ctx
 * is non-null) and terminates the process with a nonzero exit code. Never
 * returns. */
[[noreturn]] void rt_fatal(const char* component, const R5900Context* ctx, const char* fmt, ...);

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

/* Base directory for config files and relative paths (config/recomp.toml,
 * config/local.toml, saves/). ICORECOMP_SOURCE_ROOT when its
 * config/recomp.toml exists (a dev checkout); otherwise "." so a packaged
 * binary resolves everything against the directory it is run from. */
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
 * file exists, otherwise from SCUS_971.13 on the mounted disc image (same
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

#endif /* ICORECOMP_RUNTIME_H */
