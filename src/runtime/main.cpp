/* main.cpp: boot entry point.
 *
 * Sequence: open the log sink and install the crash handlers -> set FPU
 * FTZ/DAZ -> allocate guest memory + page table -> read
 * config/recomp.toml -> SHA-1-check and load the boot ELF (mounting the
 * disc image if that is where it comes from) -> wire up generated code (if
 * linked) -> bring up the GS backend and its window -> initialize the P2
 * kernel HLE (scheduler, INTC, timers, SIF) -> create guest thread 1
 * running the translated entry point and enter the scheduler loop, under a
 * crash handler that dumps the current guest context.
 *
 * Two orderings, chosen by the launcher gate below: without the launcher
 * the window comes up late on purpose, and with it the window comes up
 * first. See the gate and the rt_hw_init() call for why both are right.
 *
 * Builds in two modes, selected at CMake configure time by whether
 * generated/ee exists (see CMakeLists.txt, -DICORECOMP_HAVE_GENERATED):
 *   - stub mode: no generated/ee, g_functab is entirely empty. The entry
 *     lookup below fails gracefully with a clear message instead of
 *     crashing, so this mode is a useful smoke test of memory/loader/build
 *     wiring on its own.
 *   - full mode: generated/ee's funcs_table.c is linked in and its
 *     g_functab_init() populates g_functab; we then actually run crt0 until
 *     it traps into rt_syscall / rt_mmio_.. / rt_bad_indirect / etc.
 *
 * Presentation loop (ICORECOMP_GS=parallel|both): the EE scheduler already
 * owns the only loop in the process (ee/sched.cpp sched_loop; rt_sched_boot
 * never returns), and its virtual-clock timeline calls the GS backend's
 * vsync() at every field boundary via rt_gs_vsync_hook. The live backend
 * (gs/gs_parallel.cpp) renders and presents to the window swapchain inside
 * that vsync() call, so the per-field cadence is exactly "run guest to the
 * field boundary, then render + present", just with the loop inverted:
 * presentation runs inside the scheduler rather than the scheduler inside a
 * main-owned frame loop. Everything stays on the main OS thread (guest
 * threads are minicoro coroutines), which keeps SDL/WSI happy. main's part
 * of the contract is creating the backend on the main stack, before any
 * guest code runs, via rt_hw_init() below: Vulkan device and window setup
 * must not first happen lazily under a 2 MB coroutine stack mid-frame.
 * With ICORECOMP_GS=dump or unset the behavior is byte-identical to the
 * dump-only runtime: same calls, same order, no window, no Vulkan.
 * (Wish, if main ever needs to own the loop, e.g. for a GUI event pump
 * that must run between fields: a resumable scheduler entry point like
 * "rt_sched_run_until_field_boundary()" returning control to main once per
 * field. Not needed for the current inversion.)
 */
#include "runtime.h"
#include "target.h"

#include "ee/kernel.h"
#include "gs/gs_backend.h"
#include "guest/achievements.h"
#include "host/settings.h"
#include "host/window.h"
#include "hw/hw.h"
#include "iso/iso9660.h"
#include "prof.h"
#include "sif/rpc.h"
#include "ui/ui.h"

#ifdef ICORECOMP_HAVE_PARALLEL_GS
#include "gs/gs_parallel_api.h"
#endif

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#ifdef _WIN32
#include <cwchar>
#include <windows.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

#include "host/portable.h"

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#define ICORECOMP_X86 1
#include <pmmintrin.h>
#include <xmmintrin.h>
#endif

/* AArch64 under gcc or clang. MSVC's _M_ARM64 is deliberately not here: it
 * has neither the builtins nor inline asm, so it keeps the unknown-
 * architecture warning below rather than a silently wrong FPCR. */
#if defined(__aarch64__)
#define ICORECOMP_ARM64 1
/* GCC exposes the FPCR builtins; clang does not, and takes the mrs/msr
 * path. __has_builtin is itself only guaranteed on clang and gcc 10+, hence
 * the outer guard. */
#if defined(__has_builtin)
#if __has_builtin(__builtin_aarch64_get_fpcr)
#define ICORECOMP_HAVE_FPCR_BUILTIN 1
#endif
#endif
#endif

#ifdef ICORECOMP_HAVE_GENERATED
extern "C" void g_functab_init(void);
#endif

namespace {

void set_fpu_ftz_daz() {
#ifdef ICORECOMP_X86
    /* recomp_ops.h requires FTZ+DAZ on every guest-executing thread so the
     * host FPU never produces a denormal the PS2 tier-0 FPU model doesn't
     * expect. FTZ = MXCSR bit 15, DAZ = bit 6. */
    _mm_setcsr(_mm_getcsr() | 0x8000u | 0x0040u);
    rt_log_info("main", "MXCSR FTZ+DAZ set (0x%08x)", _mm_getcsr());
#elif defined(ICORECOMP_ARM64)
    /* AArch64 has one flush-to-zero control where x86 has two: FPCR.FZ
     * (bit 24) flushes denormal inputs and denormal results for single and
     * double precision alike, so it covers what MXCSR FTZ and DAZ cover
     * together. FPCR.FZ16 (bit 19) does the same for half precision, which
     * the guest never produces but which costs nothing to pin down, so a
     * host default cannot make one arm64 machine differ from another.
     *
     * Not verified. Nobody on this project has an arm64 host, and the check
     * that would decide it is the per-op three-way test suite (interpreter
     * against compiled emit) run on arm64. Until that has run, the denormal
     * behavior of the tier-0 FPU model on this architecture is unmeasured. */
    uint64_t fpcr = 0;
#ifdef ICORECOMP_HAVE_FPCR_BUILTIN
    fpcr = (uint64_t)__builtin_aarch64_get_fpcr();
#else
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
#endif
    fpcr |= (1ull << 24) | (1ull << 19);
#ifdef ICORECOMP_HAVE_FPCR_BUILTIN
    __builtin_aarch64_set_fpcr((uint32_t)fpcr);
#else
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
    rt_log_info("main", "FPCR FZ+FZ16 set (0x%016llx); denormal handling here is unverified,"
                   " the arm64 three-way op tests are what decide it",
        (unsigned long long)fpcr);
#else
    rt_log_warn("main", "warning: no known FTZ/DAZ setup for this architecture");
#endif
}

const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGABRT: return "SIGABRT";
#ifdef SIGBUS
        case SIGBUS:  return "SIGBUS";
#endif
        default: return "signal";
    }
}

/* Not async-signal-safe (fprintf, unordered_map lookups in rt_dump_registers
 * are reachable transitively via rt_log elsewhere, though not from here).
 * Acceptable for a debug boot skeleton: goal is a readable dump on the way
 * down, not signal-handler purity.
 *
 * The order is the order that survives the most: the synchronous crash
 * block first, written and flushed by this thread rather than handed to the
 * log writer, which may itself be the thread that died; then the guest
 * register dump; then the end-of-run summary, which is what a reader
 * greps for; then the console hold, which on a windowed run puts the
 * failure in a message box. Every step after the first is one a broken
 * process might not reach.
 *
 * On POSIX this is installed through sigaction with SA_SIGINFO, so the
 * handler is handed the address the faulting access touched and the machine
 * context the fault was taken in. Neither reaches a plain std::signal
 * handler, and the two of them together are the difference between "it
 * crashed" and "it read this address from this instruction". */
[[noreturn]] void crash_exit(int sig, const void* access_addr, const void* ucontext) {
    const char* name = signal_name(sig);
    rt_crash_report(name, (uint64_t)sig, nullptr, access_addr, ucontext, nullptr);
    rt_log_error("crash", "FATAL: caught %s while running guest code (thread %d)",
        name, rt_thread_current_id());
    if (rt_sched_current_ctx()) rt_dump_registers(rt_sched_current_ctx());
    rt_run_set_exit_reason(false, "%s on thread \"%s\"; the crash block above has the"
        " faulting address, the module it falls in and the backtrace",
        name, rt_thread_name());
    rt_run_summary();
    std::fflush(stderr);
    rt_log_hold_console();
    std::_Exit(1);
}

#ifdef _WIN32
/* Windows has no sigaction, and faults arrive through the SEH filter below
 * with far more detail than a signal carries, so this is only the fallback
 * for a CRT-raised signal. */
void crash_handler(int sig) { crash_exit(sig, nullptr, nullptr); }
#else
void crash_handler(int sig, siginfo_t* info, void* ucontext) {
    crash_exit(sig, info ? info->si_addr : nullptr, ucontext);
}
#endif

/* Last resort for a failure that never reaches the signal handlers: an
 * uncaught C++ exception (the Vulkan and swapchain paths in the GS library
 * throw) unwinds through std::terminate instead. Without this the process
 * dies through the CRT's own abort message, which on a double-clicked
 * Windows run is exactly the output that disappears with the console. */
[[noreturn]] void terminate_handler() {
    const char* what = "unknown";
    if (std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            what = ex.what();
        } catch (...) {
            what = "non-std exception";
        }
    }
    rt_crash_report("std::terminate", 0, nullptr, nullptr, nullptr, what);
    rt_log_error("crash", "FATAL: std::terminate: %s", what);
    if (rt_sched_current_ctx()) rt_dump_registers(rt_sched_current_ctx());
    rt_run_set_exit_reason(false, "std::terminate on thread \"%s\": %s",
        rt_thread_name(), what);
    rt_run_summary();
    std::fflush(stderr);
    rt_log_hold_console();
    std::_Exit(1);
}

#ifdef _WIN32
/* Structured exceptions raised on a minicoro stack, or inside the GS DLL,
 * do not reliably reach the CRT's SIGSEGV mapping. This filter runs for any
 * unhandled one in the process, including a stack overflow. */
LONG WINAPI seh_filter(EXCEPTION_POINTERS* info) {
    DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    const void* addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
    const char* name = "exception";
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      name = "ACCESS_VIOLATION"; break;
        case EXCEPTION_ILLEGAL_INSTRUCTION:   name = "ILLEGAL_INSTRUCTION"; break;
        case EXCEPTION_STACK_OVERFLOW:        name = "STACK_OVERFLOW"; break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    name = "INT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    name = "FLT_DIVIDE_BY_ZERO"; break;
        case EXCEPTION_IN_PAGE_ERROR:         name = "IN_PAGE_ERROR"; break;
        case EXCEPTION_PRIV_INSTRUCTION:      name = "PRIV_INSTRUCTION"; break;
        default: break;
    }
    /* An access violation carries the address it touched and whether it was
     * a read, a write or an execute in ExceptionInformation, which is the
     * one piece of detail that turns "it faulted" into "it wrote through a
     * null pointer here". Only meaningful for that code and IN_PAGE_ERROR;
     * for anything else the array holds nothing to report. */
    const void* touched = nullptr;
    const char* op = nullptr;
    if (info && info->ExceptionRecord &&
        (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
        info->ExceptionRecord->NumberParameters >= 2) {
        touched = (const void*)info->ExceptionRecord->ExceptionInformation[1];
        switch (info->ExceptionRecord->ExceptionInformation[0]) {
            case 0: op = "reading"; break;
            case 1: op = "writing"; break;
            case 8: op = "executing"; break;
            default: op = "accessing"; break;
        }
    }
    char detail[96] = {0};
    if (op) std::snprintf(detail, sizeof(detail), "the faulting access was %s", op);
    rt_crash_report(name, (uint64_t)code, addr, touched, info, op ? detail : nullptr);
    rt_log_error("crash", "FATAL: unhandled %s (code 0x%08lx) at host address %p (thread %d)",
        name, (unsigned long)code, addr, rt_thread_current_id());
    if (rt_sched_current_ctx()) rt_dump_registers(rt_sched_current_ctx());
    rt_run_set_exit_reason(false, "unhandled %s (code 0x%08lx) at %p on thread \"%s\"%s%s;"
        " the crash block above has the module, offset and backtrace",
        name, (unsigned long)code, addr, rt_thread_name(),
        op ? ", " : "", op ? op : "");
    rt_run_summary();
    std::fflush(stderr);
    rt_log_hold_console();
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

/* SIGINT: dump the thread/semaphore inventory before dying so an
 * interactive interrupt of a parked or spinning run is diagnosable. */
void sigint_handler(int) {
    /* warn, not info: the dump below goes out whatever the level, so at
     * the shipped default this marker is the only thing that would say
     * what asked for it. */
    rt_log_warn("main", "SIGINT");
    if (rt_sched_current_ctx()) rt_dump_registers(rt_sched_current_ctx());
    rt_sched_dump_inventory("SIGINT");
    /* A user interrupt is a deliberate end, so the summary goes out at info
     * with the rest of the block. It still goes out: "the run stopped
     * because someone pressed Ctrl+C" is exactly the kind of ending a log
     * that ends mid-line leaves a reader guessing at. */
    rt_run_set_exit_reason(true, "SIGINT (the run was interrupted from the terminal)");
    rt_run_summary();
    std::fflush(stderr);
    /* Logging is asynchronous and _Exit runs no atexit, so drain before
     * leaving or the inventory dump above never reaches the file. */
    rt_log_drain();
    std::_Exit(130);
}

void install_crash_handler() {
    /* Before the handlers, not after: a stack overflow taken between the two
     * would find the filter installed and no room to run it in. */
    rt_crash_reserve_stack();
    std::set_terminate(terminate_handler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(seh_filter);
    /* Windows has no sigaction. Faults arrive through the filter above with
     * the exception record and the machine context; these are the fallback
     * for a signal the CRT raises itself, abort() in particular. */
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGFPE, crash_handler);
    std::signal(SIGILL, crash_handler);
    std::signal(SIGABRT, crash_handler);
#else
    /* sigaction with SA_SIGINFO, not std::signal: the handler is then given
     * the address the faulting access touched and the machine context the
     * fault was taken in, and neither reaches a signal() handler. SA_ONSTACK
     * is deliberately not set: this runtime installs no alternate signal
     * stack, so asking for one that does not exist would lose the handler
     * on the fault that needs it most, a stack overflow.
     *
     * SIGABRT is here because abort() is a real exit path in this process:
     * a failed assertion in a driver or in the GS library ends the run
     * through it, and without a handler that leaves nothing in the log at
     * all. */
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    const int fault_signals[] = {
        SIGSEGV, SIGFPE, SIGILL, SIGABRT,
#ifdef SIGBUS
        SIGBUS,
#endif
    };
    for (int s : fault_signals) {
        if (sigaction(s, &sa, nullptr) != 0) {
            rt_log_warn("main", "sigaction for %s failed (%s); a fault of that kind will end"
                " this run with no crash block in the log", signal_name(s), std::strerror(errno));
        }
    }
#endif
    std::signal(SIGINT, sigint_handler);
}

/* --no-launcher: skip the launcher window and boot straight into the game
 * (see rt_no_launcher_flag). */
bool g_no_launcher = false;

/* The launcher's last gate condition: rt_hw_init() built a live backend and
 * a window actually opened for it. A dump-only run, a headless live run and
 * a build with no SDL at all have nothing to draw a launcher into, and take
 * the boot-first ordering. The window is the executable's
 * (host/window_service.h), so this is one question asked of one owner rather
 * than of whichever renderer happens to be live. */
bool live_windowed_backend() {
    return rt_window_exists();
}


std::string usage_text(const char* argv0) {
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "usage: %s [--disc <path>] [--no-launcher]\n"
        "\n"
        "  --disc <path>   ICO %s disc image to mount (.iso, or the .bin of a\n"
        "                  bin/cue rip). Without it the runtime looks for a disc\n"
        "                  per the order in src/runtime/iso/iso9660.h (config,\n"
        "                  decomp checkout, then ico.iso next to the exe).\n"
        "  --no-launcher   skip the launcher window and boot the disc directly\n"
        "\n"
        "Behavior toggles are environment variables (ICORECOMP_GS, ICORECOMP_MAX_VBLANKS,\n"
        "ICORECOMP_INPUT_SCRIPT, ICORECOMP_SAVES_DIR, ICORECOMP_LOG_LEVEL, ...); see the\n"
        "runtime sources or the packaged README.txt.\n",
        argv0, RT_TARGET_REGION);
    return buf;
}

/* ---- restart to apply a cold settings key --------------------------------
 *
 * See rt_restart_now in runtime.h for the contract and host/settings.h for
 * which keys are cold. What lives here is the process work, because this is
 * the file that owns argc/argv and the one whose atexit registration can be
 * made to run last.
 */

/* main's own argv, kept for execv: POSIX hands the successor the arguments
 * this run was given, exactly as it got them (argv[argc] is null, which is
 * the terminator execv wants). Windows does not need this, it re-uses
 * GetCommandLineW. */
int g_argc = 0;
char** g_argv = nullptr;

/* Set by rt_restart_now once the successor is prepared, read by
 * restart_atexit below. */
bool g_restart_armed = false;

#ifdef _WIN32
PROCESS_INFORMATION g_child{};
#else
std::string g_restart_exe;

/* The running executable's own path. /proc/self/exe on Linux, and on macOS
 * the _NSGetExecutablePath answer host/portable.h already resolves for
 * rt_exe_dir. argv[0] is deliberately not the fallback: it is whatever the
 * caller passed, which need not be a path at all. */
std::string self_exe_path() {
#ifdef __APPLE__
    return rt_apple_exe_path();
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = 0;
    return std::string(buf);
#endif
}
#endif /* _WIN32 */

/* Registered by main before anything else registers an atexit handler, so
 * it runs after all of them: handlers run in reverse registration order, and
 * the ones that matter here (the GS backend's teardown and its pipeline
 * cache write, the window destroy, the log drain and writer join) are all
 * registered later than this. By the time this runs the process has flushed
 * everything it flushes on a quit, which is what the successor has to be
 * able to read.
 *
 * Nothing here logs: the sink is closed by now. stderr is what is left, and
 * on a GUI-subsystem Windows run with no console even that goes nowhere,
 * which is why the failures worth reporting are reported by rt_restart_now
 * while the run is still alive. */
void restart_atexit() {
    if (!g_restart_armed) return;
#ifdef _WIN32
    /* A fatal that happened after the successor was prepared: the new
     * process must not come up as if the run had ended cleanly, and a
     * suspended process nobody resumes would sit in the task list for
     * ever. */
    if (rt_fatal_exit_code() >= 0) {
        TerminateProcess(g_child.hProcess, 1);
        CloseHandle(g_child.hThread);
        CloseHandle(g_child.hProcess);
        return;
    }
    if (ResumeThread(g_child.hThread) == (DWORD)-1) {
        std::fprintf(stderr, "[icorecomp] restart: ResumeThread failed (error %lu)\n",
            (unsigned long)GetLastError());
        TerminateProcess(g_child.hProcess, 1);
    }
    CloseHandle(g_child.hThread);
    CloseHandle(g_child.hProcess);
#else
    if (rt_fatal_exit_code() >= 0) return;
    /* exec replaces this image without running the CRT's own stdio flush,
     * so anything still buffered would be lost. The log sink flushed itself
     * in its own atexit handler above; this covers everything else. */
    std::fflush(nullptr);
    /* argv as this run received it. The fallback is for the theoretical
     * exec that handed this process no arguments at all, including no
     * argv[0]: the successor still has to be given a program name. */
    char* only_name[2] = {const_cast<char*>(g_restart_exe.c_str()), nullptr};
    execv(g_restart_exe.c_str(), (g_argc > 0 && g_argv) ? g_argv : only_name);
    std::fprintf(stderr, "[icorecomp] restart: execv(%s) failed: %s\n",
        g_restart_exe.c_str(), std::strerror(errno));
#endif
}

} // namespace

void rt_show_message(const char* title, const char* text) {
    if (rt_console_present()) {
        std::fputs(text, stdout);
        std::fflush(stdout);
        return;
    }
#ifdef _WIN32
    int len = (int)std::strlen(text);
    int wide = MultiByteToWideChar(CP_UTF8, 0, text, len, nullptr, 0);
    int wtitle = MultiByteToWideChar(CP_UTF8, 0, title, -1, nullptr, 0);
    if (wide > 0 && wtitle > 0) {
        std::vector<wchar_t> wbuf((size_t)wide + 1, L'\0');
        std::vector<wchar_t> wtbuf((size_t)wtitle, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, len, wbuf.data(), wide);
        MultiByteToWideChar(CP_UTF8, 0, title, -1, wtbuf.data(), wtitle);
        MessageBoxW(nullptr, wbuf.data(), wtbuf.data(), MB_OK | MB_ICONINFORMATION);
    }
#else
    (void)title;
#endif
}

bool rt_restart_now(const char* why, char* err, size_t err_len) {
    if (err && err_len) err[0] = 0;

#ifdef _WIN32
    constexpr DWORD kMax = 4096;
    wchar_t exe[kMax];
    DWORD n = GetModuleFileNameW(nullptr, exe, kMax);
    if (n == 0 || n >= kMax) {
        if (err && err_len) {
            std::snprintf(err, err_len, "GetModuleFileNameW failed (error %lu)",
                (unsigned long)GetLastError());
        }
        return false;
    }

    /* The same command line this run was given, argv[0] included:
     * GetCommandLineW is the string Windows itself parsed, so the successor
     * sees exactly what this process saw. CreateProcessW may write into the
     * buffer it is handed, and the one GetCommandLineW returns belongs to
     * the process, so it is copied first. The working directory is null,
     * which means "the one this process has": a --disc path relative to it
     * has to keep resolving. */
    const wchar_t* cmd = GetCommandLineW();
    std::vector<wchar_t> cmdbuf(cmd, cmd + std::wcslen(cmd) + 1);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    /* CREATE_SUSPENDED so the successor does not touch the log file, the
     * settings file, the memory card or the pipeline cache until this
     * process has finished writing all four; restart_atexit resumes it.
     * No handle inheritance: the successor opens its own log.
     *
     * Creation flags carry no console flag either way, so the successor
     * inherits this process's console state: none for a double-clicked run,
     * and the shell's console for a run started from one. That is what
     * makes debug.console take effect on the restart, since the new process
     * decides for itself from rt_settings_peek_console. */
    if (!CreateProcessW(exe, cmdbuf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
            nullptr, nullptr, &si, &pi)) {
        if (err && err_len) {
            std::snprintf(err, err_len, "CreateProcessW failed (error %lu)",
                (unsigned long)GetLastError());
        }
        return false;
    }
    g_child = pi;
#else
    std::string exe = self_exe_path();
    if (exe.empty()) {
        if (err && err_len) std::snprintf(err, err_len, "could not resolve this executable's path");
        return false;
    }
    /* The one check that can be made before the shutdown starts. execv
     * itself cannot be tried early: it either replaces this image or fails
     * with the run already half torn down, so what is testable is tested
     * here, while continuing to run is still an option. */
    if (access(exe.c_str(), X_OK) != 0) {
        if (err && err_len) {
            std::snprintf(err, err_len, "%s is not executable: %s", exe.c_str(), std::strerror(errno));
        }
        return false;
    }
    g_restart_exe = exe;
#endif

    g_restart_armed = true;
    /* The ordinary quit path from here: the window's quit flag, the
     * launcher loop's own WINDOW_CLOSED branch, main returning, then every
     * atexit handler in turn and restart_atexit last of all. */
    rt_request_exit(why);
    return true;
}

bool rt_no_launcher_flag() { return g_no_launcher; }

int main(int argc, char** argv) {
    /* Before the first atexit registration anywhere in the process, which
     * is what makes this handler run last (see restart_atexit): a restart
     * must resume or exec its successor only after every other handler has
     * flushed what it owns. A run that never asks for a restart leaves it a
     * no-op. argv is kept for the same path, which hands it to execv. */
    g_argc = argc;
    g_argv = argv;
    std::atexit(restart_atexit);

    /* The log file has to exist before anything can log to it, including
     * settings.json loading itself, so this peeks debug.log_file and
     * debug.console directly off disk instead of waiting for
     * rt_settings_init() below. One call, so the file is read and parsed
     * once rather than once per key. */
#ifdef _WIN32
    /* Belt and braces, not the fix. Some Microsoft C runtimes (the UCRT and
     * the msvcr* pair) answer an invalid CRT argument by calling the
     * invalid-parameter handler, whose default terminates the process with
     * no message; installing an empty one makes such a call return the way
     * the C standard says it does, and every such call site here checks its
     * return value. The classic msvcrt.dll this mingw-w64 cross build links
     * (cmake/mingw-w64.cmake says so) is not believed to consult a handler
     * at all, and which runtime the first GUI-subsystem package died under
     * was never settled: that can only be measured on Windows. What
     * actually fixed that death is the have_stderr test in log.cpp, which
     * stops _dup(2) being called when fd 2 has no handle behind it. */
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*,
                                      unsigned int, uintptr_t) {});
#endif
    bool want_log_file = true;
    bool want_console = false;
    rt_settings_peek_boot(&want_log_file, &want_console);
    rt_log_init(rt_base_dir(), want_log_file, want_console);
    /* The first phase, and the first call into host/run_state.cpp, which is
     * what registers the atexit handler that writes the end-of-run summary
     * for every path that names no reason of its own. It has to be here,
     * right after the log sink exists and before anything that can fail:
     * a run that dies in rt_prof_init must still say so. */
    rt_run_phase(RT_PHASE_LOG_INIT);
    rt_thread_set_name("EE (main)");
    rt_prof_init();
    /* Installed before anything that can fault, which now includes the
     * Vulkan device and window setup in rt_hw_init: a crash there used to
     * die with no dump at all. */
    install_crash_handler();

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--disc") == 0 && i + 1 < argc) {
            rt_iso_set_path(argv[++i]);
        } else if (std::strncmp(arg, "--disc=", 7) == 0) {
            rt_iso_set_path(arg + 7);
        } else if (std::strcmp(arg, "--no-launcher") == 0) {
            g_no_launcher = true;
        } else if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            rt_show_message("ICO", usage_text(argv[0]).c_str());
            rt_run_set_exit_reason(true, "--help was asked for; nothing was run");
            return 0;
        } else {
            /* Error level, so it lands in the failure text a run with no
             * console shows in its message box. */
            rt_log_error("main", "unknown argument: %s", arg);
            rt_show_message("ICO",
                ("unknown argument: " + std::string(arg) + "\n\n" + usage_text(argv[0])).c_str());
            rt_run_set_exit_reason(false, "unknown command line argument: %s", arg);
            rt_run_summary();
            rt_log_hold_console();
            return 2;
        }
    }

    set_fpu_ftz_daz();

    /* After the log sink, profiler and crash handler are up, and after
     * argument parsing so --disc has already latched, but before anything
     * that reads a setting (mem init has none today, but the GS backend and
     * audio init later in this sequence do). The verbose channel set is not
     * a setting any more (debug.verbose is retired in settings.cpp):
     * ICORECOMP_VERBOSE, read by rt_log_init, is the one way to name a
     * channel, and it exists for CI and scripted runs only. */
    rt_settings_init();
    /* The level: ICORECOMP_LOG_LEVEL was already applied by rt_log_init and
     * always wins, so only reach for debug.log_level when the variable is
     * unset. */
    if (!std::getenv("ICORECOMP_LOG_LEVEL")) {
        rt_log_set_level(rt_settings().debug.log_level);
    }
    /* The rest of the hot settings whose subsystem keeps its own copy. A
     * load runs no applier (settings.h), so without this a value that only
     * ever appears in settings.json would never reach them. */
    rt_settings_apply_loaded();
    rt_run_phase(RT_PHASE_SETTINGS);

    /* ---- launcher gate ---------------------------------------------------
     *
     * Which of the two orderings below this run uses. Every condition is a
     * reason the launcher would be wrong, not merely unnecessary:
     *   - no UI in this build: there is nothing to show.
     *   - ICORECOMP_INPUT_SCRIPT: a scripted run drives the pad from a file
     *     and must stay reproducible; a window waiting for a click is not.
     *   - ICORECOMP_MAX_VBLANKS: a bounded diagnostic run, expected to boot
     *     and exit on its own.
     *   - --no-launcher, and launcher.show_at_startup: the user said so.
     *   - ICORECOMP_GS selects the dump backend: a dump run has nothing to
     *     draw the launcher into. Asked here, before rt_mem_init and
     *     rt_hw_init, because those two open the ICORECOMP_GS_DUMP file
     *     (truncating it) and would do so before the cheap failures below
     *     -- no disc, bad pin -- get their chance to stop the run.
     *   - no live windowed backend: a live backend that opened no window
     *     (headless), or a build with no paraLLEl-GS at all. That one can
     *     only be answered after rt_hw_init(), so the two init calls it
     *     needs happen first and the rest of the sequence below skips them.
     */
    bool launcher = false;
    const char* launcher_reason = "UI built in, live window, no bypass, launcher.show_at_startup";
#ifdef ICORECOMP_UI
    if (std::getenv("ICORECOMP_INPUT_SCRIPT")) {
        launcher_reason = "ICORECOMP_INPUT_SCRIPT is set";
    } else if (std::getenv("ICORECOMP_MAX_VBLANKS")) {
        launcher_reason = "ICORECOMP_MAX_VBLANKS is set";
    } else if (rt_no_launcher_flag()) {
        launcher_reason = "--no-launcher";
    } else if (!rt_settings().launcher.show_at_startup) {
        launcher_reason = "launcher.show_at_startup is false";
    } else if (!rt_gs_backend_selects_live()) {
        launcher_reason = "ICORECOMP_GS selects the dump backend";
    } else {
        launcher = true;
    }
#else
    launcher_reason = "this build has no UI (ICORECOMP_UI=OFF)";
#endif

    bool mem_ready = false, hw_ready = false, ui_ready = false;
    if (launcher) {
        rt_mem_init();
        mem_ready = true;
        rt_hw_init();
        hw_ready = true;
        if (!live_windowed_backend()) {
            launcher = false;
            launcher_reason = "no live windowed backend in this run";
        }
    }
    rt_log_info("main", "launcher gate: %s (%s)",
        launcher ? "launcher first" : "boot straight into the game", launcher_reason);

    if (launcher) {
        rt_ui_init();
        ui_ready = true;
        /* Returns only when the boot precheck passed and Start was pressed:
         * the disc is mounted and the boot ELF is already read and
         * pin-checked, so the rt_load_elf below reuses it. Quit and window
         * close leave the process here, with nothing loaded. */
        if (!rt_launcher_run()) {
            /* A deliberate Quit or window close, not an error: no console
             * hold, the process just ends. */
            rt_log_info("main", "launcher exited without starting the game");
            rt_run_set_exit_reason(true, "the launcher was closed without starting the game");
            return 0;
        }
    }

    if (!mem_ready) rt_mem_init();

    LoaderConfig cfg;
    if (!rt_load_config(&cfg)) {
        rt_fatal("main", nullptr, "could not load config/%s", RT_TARGET_CONFIG_TOML);
    }
    /* Which disc this build is for, said once, up front: it decides the boot
     * ELF the disc check looks for and the SHA-1 pin. See docs/TARGET.md. */
    rt_log_info("main", "target: boot ELF %s (%s)",
        RT_TARGET_BOOT_ELF, RT_TARGET_REGION);
    rt_load_elf(cfg);

#ifdef ICORECOMP_HAVE_GENERATED
    g_functab_init();
    rt_log_info("main", "generated code linked: g_functab_init() called");
#else
    rt_log_info("main", "stub build: no generated code linked; g_functab is empty");
#endif

    if (cfg.entry < RECOMP_TEXT_BASE || cfg.entry >= RECOMP_TEXT_LIMIT) {
        rt_fatal("main", nullptr, "config [target].entry 0x%08x is outside the function table range [0x%08x, 0x%08x)",
            cfg.entry, RECOMP_TEXT_BASE, RECOMP_TEXT_LIMIT);
    }
    uint32_t entry_idx = RECOMP_FUNC_IDX(cfg.entry);
    recomp_fn_t entry_fn = g_functab[entry_idx];
    if (!entry_fn) {
#ifdef ICORECOMP_HAVE_GENERATED
        rt_log_error("main", "FATAL: entry function at vram 0x%08x not found in g_functab after g_functab_init()"
                        " -- generated code does not cover this address", cfg.entry);
#else
        rt_log_error("main", "FATAL: no generated code linked (stub build); entry function at vram 0x%08x cannot be"
                        " resolved. Configure with generated/ee present (or -DGENERATED_DIR=...) to run the game.", cfg.entry);
#endif
        rt_run_set_exit_reason(false, "the entry function at vram 0x%08x is not in the generated"
            " function table, so there is nothing to run", cfg.entry);
        rt_run_summary();
        rt_log_hold_console();
        return 1;
    }

    (void)entry_fn; /* thread 1's trampoline looks it up again via g_functab */

    /* Creates the GS backend (ICORECOMP_GS selects dump / parallel / both;
     * see gs/gs_select.cpp) on the main thread stack. For the live backend
     * this brings up the Vulkan device and, when a display is available,
     * the window + swapchain, before any guest code exists.
     *
     * Deliberately last on this path: everything above can fail cheaply and
     * often does on a fresh install (no disc image, wrong disc, SHA-1 pin
     * mismatch, no generated code). Opening the window first turned each of
     * those into a black window that vanished, which reads as a crash
     * rather than the specific error it is. That is the failure ordering
     * for a run with no launcher, and it is why the calls above come first
     * here.
     *
     * The launcher path inverts it deliberately: it opens the window before
     * any of them, because rt_boot_precheck (loader.cpp) runs exactly those
     * same steps and reports each failure inside the window, by name,
     * instead of on a console the user may never see. Nothing is lost, the
     * failures just arrive somewhere the user is already looking. Both
     * calls are skipped here when the launcher path already made them. */
    if (!hw_ready) rt_hw_init();

    /* The UI needs the window (and its surface size) to exist, so it comes
     * up right after the backend. A scripted run bypasses the UI entirely
     * (settings plan): ICORECOMP_INPUT_SCRIPT drives the pad from a file and
     * must stay reproducible, which a menu that eats input would break. */
    if (!ui_ready && !std::getenv("ICORECOMP_INPUT_SCRIPT")) rt_ui_init();

    /* Hands the GS command ring to its worker thread (gs/gs_threaded.cpp).
     * Here, and not at rt_hw_init: everything above this line runs the ring
     * inline on this thread, which is what the launcher's own present loop
     * and the UI's first texture uploads expect, and from here on every
     * field of the game is produced by the EE thread and consumed by the
     * worker. A no-op when ICORECOMP_GS_THREAD=0 bypassed the ring. */
    rt_gs_backend_start_worker();

    /* The achievement store lives beside the virtual memory card, so this
     * runs after that directory is resolved and before the first guest
     * field, which is what ticks the observer (sif/pad.cpp). The flush at
     * exit goes through atexit, which is how host/audio.cpp and
     * gs/gs_select.cpp already end their runs: every exit path in this
     * runtime except the crash handlers leaves through std::exit. */
    rt_achievements_init(rt_mc_saves_dir());
    std::atexit(rt_achievements_shutdown);

    rt_sched_init();

    /* The end-of-run summary prints the thread/semaphore/RPC inventory
     * through this hook. Registered here rather than called from
     * host/run_state.cpp directly, because that file deliberately links
     * against nothing but the log entry points (the log selftest builds it
     * on its own), and because before this point there is no scheduler to
     * take an inventory of. */
    rt_run_set_inventory_hook(rt_sched_dump_inventory);

    /* From here the guest owns the run, and the cold settings keys stop
     * being changeable: a change to one of those is applied by restarting
     * the program, which is only offered while the launcher still owns the
     * window. The menu disables both controls on this, and
     * commit_validate() refuses the change whoever asks (host/settings.h). */
    rt_settings_set_gameplay_active(true);

    /* The guest owns the run from here, so this is where the field watchdog
     * belongs: before it there are no fields to watch and a stall report
     * would be meaningless. rt_sched_boot never returns, so the stop half
     * runs from the summary. */
    rt_run_phase(RT_PHASE_GUEST_BOOTED);
    rt_run_watchdog_start();

    rt_log_info("main", "booting scheduler: thread 1 entry vram=0x%08x sp=0x%08x gp=0x%08x",
        cfg.entry, uint32_t(RT_RAM_SIZE - 0x10000), cfg.gp);

    /* Creates guest thread 1 at priority 0 running the translated entry and
     * never returns: the process ends via the Exit syscall, a fatal, or the
     * clean-exit sentinel. Initial $sp matches P1 (top of RAM minus 64 KB;
     * crt0's RFU060 declares the real stack). */
    rt_sched_boot(cfg.entry, cfg.gp, RT_RAM_SIZE - 0x10000);
}
