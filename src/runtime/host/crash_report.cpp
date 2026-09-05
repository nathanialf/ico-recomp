/* host/crash_report.cpp: what a crash handler writes before it does
 * anything else.
 *
 * The requirement this file answers: a crash log that says only "caught
 * SIGSEGV" places nothing. A crash log that names the module and offset the
 * fault address falls in, and the same for every frame on the stack, places
 * the fault in a source file even when the crash happened on a user's
 * machine with no debugger and no symbols.
 *
 * Written synchronously, through rt_log_write_sync (log.cpp), and not
 * through the ordinary log path. The ordinary path hands the line to
 * another thread, and the whole premise of a crash handler is that no other
 * thread is guaranteed to run again: the writer may be the thread that
 * faulted, or it may be waiting on a lock this thread holds. So every line
 * here is written and flushed by the faulting thread itself, and the queued
 * lines are drained afterwards by the caller. The resulting file reads
 * "the crash, then whatever the run had queued behind it", which is the
 * order that survives the most.
 *
 * Dependencies, deliberately none:
 *
 *   Windows: GetModuleHandleExW on the address itself resolves the module,
 *   and CaptureStackBackTrace walks the stack. Both are kernel32. DbgHelp
 *   is not used, so this works on a machine with no debugging tools
 *   installed and cannot fail because a DLL is missing. The cost is that
 *   frames are "module+offset" rather than function names, which is what a
 *   map file or a symbol server turns back into a function anyway.
 *
 *   POSIX: dladdr for the module (and the nearest exported symbol when
 *   there is one) and backtrace() for the frames.
 *
 * Nothing here allocates, throws or fails: every step degrades to a line
 * saying it could not be taken.
 */
#include "runtime.h"

#include "../ee/kernel.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#if defined(__linux__) || defined(__APPLE__)
#include <dlfcn.h>
#include <execinfo.h>
#endif
#ifdef __linux__
#include <sys/ucontext.h>
#endif
#ifdef __APPLE__
#include <sys/ucontext.h>
#endif
#endif

namespace {

constexpr unsigned kMaxFrames = 48;

void sync_line(const char* fmt, ...) RT_PRINTF_FORMAT(1, 2);

void sync_line(const char* fmt, ...) {
    char body[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    char line[1152];
    std::snprintf(line, sizeof(line), "[icorecomp][crash][error] %s\n", body);
    rt_log_write_sync(line);
}

#ifdef _WIN32

/* Module and offset for one address. GetModuleHandleExW with
 * FROM_ADDRESS is the whole lookup: it hands back the module the address
 * belongs to, and the module handle is its load base, so the offset is a
 * subtraction. UNCHANGED_REFCOUNT so a crash handler does not change any
 * module's reference count on its way out.
 *
 * `out` gets "name+0xoffset", or "unknown module" when the address is not
 * in any loaded image, which is itself worth saying: it means a jump
 * through a wild pointer rather than a fault inside known code. */
void module_for(const void* addr, char* out, size_t out_len) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)addr, &mod) || !mod) {
        std::snprintf(out, out_len, "not in any loaded module");
        return;
    }
    wchar_t wpath[MAX_PATH];
    DWORD n = GetModuleFileNameW(mod, wpath, MAX_PATH);
    /* Four bytes per UTF-16 code unit plus the terminator is the most UTF-8
     * can need, so the conversion cannot be the truncating kind. That matters
     * here and not elsewhere: on a short buffer WideCharToMultiByte writes as
     * much as fits and does not terminate it, and the leaf scan below walks
     * to the first NUL. A crash handler reading off the end of its own buffer
     * would lose the report it exists to write. */
    char path[MAX_PATH * 4 + 1] = "?";
    if (n > 0 && n < MAX_PATH) {
        if (WideCharToMultiByte(CP_UTF8, 0, wpath, -1, path, (int)sizeof(path),
                                nullptr, nullptr) == 0) {
            /* The call wrote nothing it promises to have terminated. */
            std::snprintf(path, sizeof(path), "?");
        }
    }
    /* The leaf name only: a crash line is read on one screen and the
     * directory is the same for every frame. */
    const char* leaf = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') leaf = p + 1;
    }
    const uintptr_t base = (uintptr_t)mod;
    std::snprintf(out, out_len, "%s+0x%llx", leaf,
        (unsigned long long)((uintptr_t)addr - base));
}

void write_backtrace() {
    void* frames[kMaxFrames];
    const USHORT n = CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
    if (n == 0) {
        sync_line("backtrace: CaptureStackBackTrace returned no frames");
        return;
    }
    sync_line("backtrace (%u frames; the first few are this handler):", (unsigned)n);
    for (USHORT i = 0; i < n; ++i) {
        char where[512];
        module_for(frames[i], where, sizeof(where));
        sync_line("  #%02u %p  %s", (unsigned)i, frames[i], where);
    }
}

void write_registers(const void* platform_ctx) {
    const EXCEPTION_POINTERS* ep = (const EXCEPTION_POINTERS*)platform_ctx;
    if (!ep || !ep->ContextRecord) {
        sync_line("registers: the platform handed no context record over");
        return;
    }
    const CONTEXT* c = ep->ContextRecord;
#if defined(_M_X64) || defined(__x86_64__)
    sync_line("registers: rip=%016llx rsp=%016llx rbp=%016llx",
        (unsigned long long)c->Rip, (unsigned long long)c->Rsp, (unsigned long long)c->Rbp);
    sync_line("           rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx",
        (unsigned long long)c->Rax, (unsigned long long)c->Rbx,
        (unsigned long long)c->Rcx, (unsigned long long)c->Rdx);
    sync_line("           rsi=%016llx rdi=%016llx r8 =%016llx r9 =%016llx",
        (unsigned long long)c->Rsi, (unsigned long long)c->Rdi,
        (unsigned long long)c->R8, (unsigned long long)c->R9);
#elif defined(_M_ARM64) || defined(__aarch64__)
    sync_line("registers: pc=%016llx sp=%016llx fp=%016llx lr=%016llx",
        (unsigned long long)c->Pc, (unsigned long long)c->Sp,
        (unsigned long long)c->Fp, (unsigned long long)c->Lr);
#else
    (void)c;
    sync_line("registers: no register layout is known for this architecture");
#endif
}

#else /* !_WIN32 */

void module_for(const void* addr, char* out, size_t out_len) {
#if defined(__linux__) || defined(__APPLE__)
    Dl_info info;
    std::memset(&info, 0, sizeof(info));
    if (dladdr(addr, &info) == 0 || !info.dli_fname) {
        std::snprintf(out, out_len, "not in any loaded object");
        return;
    }
    const char* leaf = info.dli_fname;
    for (const char* p = info.dli_fname; *p; ++p) {
        if (*p == '/') leaf = p + 1;
    }
    const uintptr_t base = (uintptr_t)info.dli_fbase;
    if (info.dli_sname) {
        std::snprintf(out, out_len, "%s+0x%llx (%s+0x%llx)", leaf,
            (unsigned long long)((uintptr_t)addr - base), info.dli_sname,
            (unsigned long long)((uintptr_t)addr - (uintptr_t)info.dli_saddr));
    } else {
        std::snprintf(out, out_len, "%s+0x%llx", leaf,
            (unsigned long long)((uintptr_t)addr - base));
    }
#else
    (void)addr;
    std::snprintf(out, out_len, "no module lookup on this platform");
#endif
}

void write_backtrace() {
#if defined(__linux__) || defined(__APPLE__)
    void* frames[kMaxFrames];
    const int n = backtrace(frames, (int)kMaxFrames);
    if (n <= 0) {
        sync_line("backtrace: backtrace() returned no frames");
        return;
    }
    sync_line("backtrace (%d frames; the first few are this handler):", n);
    for (int i = 0; i < n; ++i) {
        char where[512];
        module_for(frames[i], where, sizeof(where));
        sync_line("  #%02d %p  %s", i, frames[i], where);
    }
    /* backtrace_symbols_fd as well as the per-frame lines above, and not
     * instead of them: it writes whatever the dynamic symbol table can
     * resolve straight to the descriptor with no allocation, which is the
     * one form that still works when the heap is what was corrupted. fd 2
     * is the log file (log.cpp points it there), so this lands in the same
     * file as everything else. */
    backtrace_symbols_fd(frames, n, 2);
#else
    sync_line("backtrace: not available on this platform");
#endif
}

void write_registers(const void* platform_ctx) {
/* REG_RIP and its neighbours are glibc's own names for the general
     * register slots and only exist when __USE_GNU is on, which is why the
     * x86-64 branch tests for the macro rather than for the architecture.
     * Without it there is no portable way to index the array, and a guessed
     * index would print the wrong register, which is worse than printing
     * none. */
#if defined(__linux__) && defined(__x86_64__) && defined(REG_RIP)
    const ucontext_t* uc = (const ucontext_t*)platform_ctx;
    if (!uc) {
        sync_line("registers: the signal handler was given no ucontext");
        return;
    }
    const greg_t* g = uc->uc_mcontext.gregs;
    sync_line("registers: rip=%016llx rsp=%016llx rbp=%016llx",
        (unsigned long long)g[REG_RIP], (unsigned long long)g[REG_RSP],
        (unsigned long long)g[REG_RBP]);
    sync_line("           rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx",
        (unsigned long long)g[REG_RAX], (unsigned long long)g[REG_RBX],
        (unsigned long long)g[REG_RCX], (unsigned long long)g[REG_RDX]);
#elif defined(__linux__) && defined(__aarch64__)
    const ucontext_t* uc = (const ucontext_t*)platform_ctx;
    if (!uc) {
        sync_line("registers: the signal handler was given no ucontext");
        return;
    }
    sync_line("registers: pc=%016llx sp=%016llx",
        (unsigned long long)uc->uc_mcontext.pc, (unsigned long long)uc->uc_mcontext.sp);
#else
    (void)platform_ctx;
    sync_line("registers: no register layout is known for this platform");
#endif
}

#endif /* _WIN32 */

} // namespace

void rt_crash_reserve_stack() {
#ifdef _WIN32
    /* 64 KB. The report's own frames come to about 3 KB, and the rest is
     * headroom for the CRT and the kernel call that gets the filter running
     * at all. Failure is logged and not fatal: the process runs perfectly
     * well without it, it just loses the dump on the one fault that needs
     * this most. */
    ULONG bytes = 64 * 1024;
    if (!SetThreadStackGuarantee(&bytes)) {
        rt_log_warn("crash", "SetThreadStackGuarantee(64 KB) failed (Windows error %lu) on"
            " thread \"%s\"; a stack overflow on this thread may end the run with no crash"
            " block in the log", (unsigned long)GetLastError(), rt_thread_name());
    }
#endif
}

void rt_crash_report(const char* what, uint64_t code, const void* fault_pc,
                     const void* access_addr, const void* platform_ctx,
                     const char* detail) {
    /* The first line, and the one that has to survive on its own: what
     * happened, where, and on which thread. Everything after it is
     * additional detail that a broken platform call could cost. */
    sync_line("---- crash ----");
    sync_line("%s (code 0x%llx) on thread \"%s\"", what ? what : "unknown fault",
        (unsigned long long)code, rt_thread_name());
    if (detail && *detail) sync_line("detail: %s", detail);

    if (fault_pc) {
        char where[512];
        module_for(fault_pc, where, sizeof(where));
        sync_line("faulting instruction: %p in %s", fault_pc, where);
    } else {
        sync_line("faulting instruction: the platform did not report one");
    }
    if (access_addr) {
        char where[512];
        module_for(access_addr, where, sizeof(where));
        sync_line("address touched: %p (%s)", access_addr, where);
    }

    /* The guest side. pc_hint is the translated function the EE thread was
     * last known to be inside.
     *
     * rt_sched_current_ctx answers from the scheduler's own state whatever
     * thread asks, so on a GS-worker fault it used to print the EE thread's
     * pc_hint under a heading that reads as this thread's context. Ask
     * whose thread this is first: the EE thread's own fault is the only one
     * the guest context describes. */
    if (!rt_sched_on_ee_thread()) {
        sync_line("guest pc_hint: not the EE thread, so this fault has no guest context "
                  "(the EE thread's own is in the end-of-run summary)");
    } else if (const R5900Context* ctx = rt_sched_current_ctx()) {
        sync_line("guest pc_hint: 0x%08x (guest thread %d)", ctx->pc_hint,
            rt_thread_current_id());
    } else {
        sync_line("guest pc_hint: no guest context on this thread");
    }

    write_registers(platform_ctx);
    write_backtrace();
    sync_line("---- crash ----");

    /* The synchronous block is on disk. Now let everything the run had
     * queued behind it out, so the lines leading up to the fault are in the
     * same file. This is bounded and gives up rather than hanging (log.cpp),
     * which is why it is safe here at all. */
    rt_log_drain();
}
