/* gs/render/gs_render_log_stubs.cpp: the runtime services the native GS
 * renderer uses, for the tools that are not the runtime executable.
 *
 * Ours (MIT). The renderer and the RHI log through runtime.h's rt_log_* and
 * die through rt_fatal, which is what makes their failure messages read the
 * same in a game run as in a replay. log.cpp is the real implementation, but
 * it also owns the console, the log file, the user state directory and the
 * register dumper, so linking it into a small tool would drag most of the
 * runtime with it. icorecomp-gs-replay links this instead.
 *
 * The same arrangement the VIF1 and GS ring selftests already use; see their
 * blocks in CMakeLists.txt. Never linked into icorecomp-runtime, which has
 * the real log.cpp.
 */
#include "../../guest/widescreen.h"
#include "../../runtime.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void emit(const char* level, const char* component, const char* fmt, va_list ap) {
    std::fprintf(stderr, "[%s] %s: ", level, component ? component : "?");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

} // namespace

void rt_log_error(const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("error", component, fmt, ap);
    va_end(ap);
}

void rt_log_warn(const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("warn", component, fmt, ap);
    va_end(ap);
}

void rt_log_info(const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("info", component, fmt, ap);
    va_end(ap);
}

void rt_log_debug(const char* component, const char* fmt, ...) {
    /* Debug lines follow the same verbose gate the real sink uses, so a tool
     * run is as quiet or as loud as a game run with the same setting. */
    if (!rt_verbose(component)) return;
    va_list ap;
    va_start(ap, fmt);
    emit("debug", component, fmt, ap);
    va_end(ap);
}

/* host/run_state.cpp's push side, which gs_native.cpp calls on the present
 * path so a stalled run's log can say where the renderer was. Empty here:
 * there is no run to summarise in a standalone tool. The replay tool reads a
 * dump and exits with a status, the selftests assert and exit, and neither
 * has a phase machine, a watchdog or an end-of-run block for these to feed.
 * Linking the real module would pull in the settings layer, the window
 * service and the crash handlers for values nothing in this process reads. */
void rt_run_note_present() {}
void rt_run_note_rhi(const char* /*state*/) {}
void rt_run_phase(RtRunPhase /*reached*/) {}

/* The same, for host/window_service.cpp: rt_window_notify_quit records who
 * asked for the close so the end-of-run block can name it. A tool that never
 * opens a window still links the service for its stub half, so the symbol
 * has to exist. */
void rt_run_set_exit_reason(bool /*user_quit*/, const char* /*fmt*/, ...) {}

/* guest/widescreen.cpp's presentation helper, which gs_native.cpp calls once
 * per field. The real one reads the factor the settings layer pushed; a tool
 * run has no settings layer and so no widescreen, and "off" is exactly
 * "present at the aspect the CRTC registers derived". Linking the real
 * module here would pull in guest memory (rt_gptr) and the loader for a
 * value that can only be 0 in this process. */
double rt_widescreen_present_aspect(double derived) { return derived; }

/* ICORECOMP_VERBOSE is a comma or space separated list of component names, as
 * the real implementation reads it. This copy has no settings.json to fall
 * back to, which is the one difference and the reason it is stated here. */
bool rt_verbose(const char* component) {
    if (!component) return false;
    const char* spec = std::getenv("ICORECOMP_VERBOSE");
    if (!spec || !*spec) return false;
    if (std::strcmp(spec, "all") == 0) return true;
    const size_t len = std::strlen(component);
    for (const char* p = spec; *p;) {
        while (*p == ',' || *p == ' ') ++p;
        const char* start = p;
        while (*p && *p != ',' && *p != ' ') ++p;
        if ((size_t)(p - start) == len && std::strncmp(start, component, len) == 0) return true;
    }
    return false;
}

void rt_fatal(const char* component, const R5900Context* /*ctx*/, const char* fmt, ...) {
    /* No guest context to dump here: nothing in a tool run has one. The
     * message is the whole report, which is why the renderer puts the device
     * name and the missing requirement into it. */
    std::fprintf(stderr, "[fatal] %s: ", component ? component : "?");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::exit(1);
}
