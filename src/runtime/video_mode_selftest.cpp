/* video_mode_selftest.cpp: standalone exercise of the field timeline
 * (video_mode.cpp).
 *
 * What this target is for: the field period is not a compiled-in constant,
 * because the game's own display option selects 50 Hz or 60 Hz and programs
 * SetGsCrt for it, so both modes are reachable in one run. Every NTSC number
 * below is asserted against the literal this runtime carried when the period
 * was a constant (2460060 bus cycles per field, 206184 per vblank, 9371 per
 * H-blank, 59.94 Hz, 525 lines), so the 60 Hz option is timed exactly as it
 * was. The PAL numbers are asserted against their derivation from the analog
 * standard, and the switch is exercised in both directions.
 *
 * Links video_mode.cpp against a stub rt_log (below), which is the only
 * runtime.h extern it calls. No disc, no window, no GS.
 *
 * Exit code 0 = every check passed; 2 on the first failing CHECK.
 */
#include "video_mode.h"

#include "runtime.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

/* ---- runtime stubs ------------------------------------------------------- */

namespace {
int g_log_lines = 0;
}

void rt_log_line(const char* component, const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    std::printf("[%s] %s\n", component, buf);
    ++g_log_lines;
}

void rt_log_error(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_warn(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_info(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_debug(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}

/* ---- harness ------------------------------------------------------------- */

namespace {

int g_failures = 0;

void check(bool ok, const char* what, const char* file, int line) {
    if (ok) return;
    std::printf("FAIL %s (%s:%d)\n", what, file, line);
    ++g_failures;
}

#define CHECK(expr) check((expr), #expr, __FILE__, __LINE__)

bool near_eq(double a, double b) { return std::fabs(a - b) < 1e-9; }

/* Hook bookkeeping: what rt_video_set_mode reported, and how often. */
int g_hook_calls = 0;
RtVideoMode g_hook_prev = RT_VIDEO_PAL;
RtVideoMode g_hook_now = RT_VIDEO_PAL;

void hook(RtVideoMode prev, RtVideoMode now) {
    ++g_hook_calls;
    g_hook_prev = prev;
    g_hook_now = now;
}

/* The NTSC answers, spelled as the literals the runtime used before the
 * mode became a variable rather than as expressions, so a change to the
 * derivation cannot quietly agree with itself. */
void check_ntsc_now() {
    CHECK(rt_video_mode() == RT_VIDEO_NTSC);
    CHECK(rt_cycles_per_field() == 2460060ull);
    CHECK(rt_cycles_vblank() == 206184ull);
    CHECK(rt_cycles_per_hblank() == 9371ull);
    CHECK(near_eq(rt_field_rate_hz(), 59.94));
    CHECK(rt_lines_per_frame() == 525u);
}

void check_pal_now() {
    CHECK(rt_video_mode() == RT_VIDEO_PAL);
    /* 147456000 / 50, exactly, with no remainder: PAL is 50 fields per
     * second and the EE bus clock divides by it. */
    CHECK(rt_cycles_per_field() == 2949120ull);
    CHECK(RT_BUSCLK_HZ % 50ull == 0);
    CHECK(RT_BUSCLK_HZ / 50ull == rt_cycles_per_field());
    /* 24 blanking lines per field at 9437 cycles a line. */
    CHECK(rt_cycles_per_hblank() == 9437ull);
    CHECK(rt_cycles_vblank() == 24ull * 9437ull);
    CHECK(near_eq(rt_field_rate_hz(), 50.0));
    CHECK(rt_lines_per_frame() == 625u);
}

} // namespace

int main() {
    /* The two enum values are the SMODE1 CMOD field, which is what
     * hw/gspriv.cpp's rt_gs_program_crt maps SetGsCrt mode 0x02 and 0x03
     * onto and writes into SMODE1 bits 13-14. */
    CHECK(RT_VIDEO_NTSC == 2u);
    CHECK(RT_VIDEO_PAL == 3u);

    /* The boot mode: a console powers its GS up in its own region's mode,
     * so the timeline starts at PAL and nothing has programmed a mode yet. */
    check_pal_now();

    rt_video_add_mode_hook(hook);

    /* SetGsCrt mode 0x03, which is what the 50 Hz display option programs:
     * the mode is confirmed, not changed, so no deadline holder is asked to
     * re-derive anything. */
    rt_video_set_mode(RT_VIDEO_PAL, "selftest boot SetGsCrt");
    CHECK(g_hook_calls == 0);
    check_pal_now();

    /* A field is longer at 50 Hz than at 59.94, and both periods are the
     * bus clock over the field rate, which is the whole derivation. */
    CHECK(rt_cycles_per_field() > 2460060ull);
    CHECK(std::fabs((double)rt_cycles_per_field() * rt_field_rate_hz()
                    - (double)RT_BUSCLK_HZ) < 1.0);
    CHECK(std::fabs(2460060.0 * 59.94 - (double)RT_BUSCLK_HZ) < 5.0);

    /* SetGsCrt mode 0x02: the 60 Hz display option, and the proof that the
     * NTSC numbers come back to the cycle. */
    rt_video_set_mode(RT_VIDEO_NTSC, "selftest SetGsCrt NTSC");
    CHECK(g_hook_calls == 1);
    CHECK(g_hook_prev == RT_VIDEO_PAL);
    CHECK(g_hook_now == RT_VIDEO_NTSC);
    check_ntsc_now();

    /* Repeating the same mode is silent and moves nothing. */
    rt_video_set_mode(RT_VIDEO_NTSC, "selftest SetGsCrt NTSC again");
    CHECK(g_hook_calls == 1);
    check_ntsc_now();

    /* Back to 50 Hz, so the switch is exercised in both directions. */
    rt_video_set_mode(RT_VIDEO_PAL, "selftest SetGsCrt PAL");
    CHECK(g_hook_calls == 2);
    CHECK(g_hook_prev == RT_VIDEO_NTSC);
    CHECK(g_hook_now == RT_VIDEO_PAL);
    check_pal_now();

    /* One field of audio at 48 kHz, the quantity snd/engine.cpp steps its
     * 16.16 accumulator by: 960 exactly on PAL, 800.8 frames on NTSC. */
    CHECK(near_eq(48000.0 / rt_field_rate_hz(), 960.0));
    rt_video_set_mode(RT_VIDEO_NTSC, "selftest audio step");
    CHECK(std::fabs(48000.0 / rt_field_rate_hz() - 800.8) < 0.01);
    rt_video_set_mode(RT_VIDEO_PAL, "selftest done");

    /* Every mode change said so in the log. */
    CHECK(g_log_lines >= 4);

    if (g_failures) {
        std::printf("video-mode selftest: %d FAILURES\n", g_failures);
        return 2;
    }
    std::printf("video-mode selftest: all checks passed\n");
    return 0;
}
