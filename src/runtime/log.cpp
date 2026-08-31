/* log.cpp: shared logging, the log-file sink, and register-dump helpers.
 *
 * Log sink: on Windows a double-clicked run owns its console, and the
 * console dies with the process, so a crash takes the whole log with it.
 * rt_log_init points file descriptor 2 at a log file instead. Because the
 * redirect is at the fd level rather than on the C stderr FILE*, it catches
 * every module in the process, including the GS shared library, SDL and the
 * Vulkan loader, each of which may carry its own CRT copy. The original
 * stderr is kept as a duplicate so the runtime's own messages still echo to
 * the console while it is alive.
 */
#include "runtime.h"

#include "host/portable.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/* Duplicate of the pre-redirect stderr; null when no redirect happened, in
 * which case stderr is still the console and needs no second write. */
std::FILE* g_console = nullptr;
std::string g_log_path;

/* Formats once, then writes to the log file (via stderr, redirected) and to
 * the console duplicate. Overlong lines are truncated rather than split;
 * nothing the runtime logs comes close to the buffer size. */
void emit(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    std::fputs(buf, stderr);
    if (g_console) {
        std::fputs(buf, g_console);
        std::fflush(g_console);
    }
}

} // namespace

void rt_log_init(const char* dir) {
    const char* env = std::getenv("ICORECOMP_LOG");
    std::string path;
    if (env && *env) {
        /* "-" or "0" opts out: console only, the pre-sink behavior. */
        if (std::strcmp(env, "-") == 0 || std::strcmp(env, "0") == 0) return;
        path = env;
    } else {
#ifdef _WIN32
        path = std::string(dir && *dir ? dir : ".") + "/icorecomp.log";
#else
        /* POSIX runs are launched from a shell that keeps the output, so
         * the sink is opt-in there. */
        (void)dir;
        return;
#endif
    }

    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) {
        std::fprintf(stderr, "[icorecomp][log] could not open '%s' for writing; console only\n",
            path.c_str());
        return;
    }

    int saved = rt_dup(2);
    std::fflush(stderr);
    if (rt_dup2(rt_fileno(f), 2) < 0) {
        std::fprintf(stderr, "[icorecomp][log] could not redirect stderr to '%s'; console only\n",
            path.c_str());
        std::fclose(f);
        if (saved >= 0) {
#ifdef _WIN32
            _close(saved);
#else
            close(saved);
#endif
        }
        return;
    }
    /* fd 2 now holds its own handle on the file; f's is redundant. */
    std::fclose(f);
    if (saved >= 0) g_console = rt_fdopen(saved, "w");
    g_log_path = path;

    emit("[icorecomp][log] writing this run's log to %s\n", path.c_str());
}

const char* rt_log_path() {
    return g_log_path.empty() ? nullptr : g_log_path.c_str();
}

void rt_vlog(const char* component, const char* fmt, va_list ap) {
    char line[3072];
    std::vsnprintf(line, sizeof(line), fmt, ap);
    emit("[icorecomp][%s] %s\n", component, line);
    std::fflush(stderr);
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
    if (!ctx) {
        emit("[icorecomp][regs] (null context)\n");
        std::fflush(stderr);
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
    std::fflush(stderr);
}

void rt_log_hold_console() {
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
    emit("[icorecomp][%s] FATAL: %s\n", component, line);
    if (ctx) rt_dump_registers(ctx);
    std::fflush(stderr);
    rt_log_hold_console();
    std::exit(1);
}
