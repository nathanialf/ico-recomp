/* video_mode.h: the programmed CRT video mode and everything the field
 * timeline hangs off it.
 *
 * One authority for "how long is a field". The EE kernel's vblank timeline,
 * the H-blank the timers and the kernel alarms count, the pad tick, the
 * audio mixer's frames per field and the host frame pacer all read the
 * numbers below rather than carrying an NTSC constant of their own.
 *
 * The mode is not a setting. It is what the game programmed through
 * SetGsCrt: hw/gspriv.cpp's rt_gs_program_crt is the only writer of SMODE1
 * and the only caller of rt_video_set_mode, so the field rate follows the
 * CMOD the game asked for. The US disc asks for NTSC once at boot and never
 * changes it. The PAL disc asks for NTSC or PAL depending on the display
 * option the player picked: its gsb_Init reads one config word and takes
 * either a 512x448 buffer with SetGsCrt mode 2 or a 512x512 buffer with
 * mode 3. That was read off the objdump listing the PAL disc ships
 * (SRCFILE.TXT), which is a different link of the same source than the
 * retail ELF, so it is a reading of the game's behaviour and not of an
 * address. Either way the mode can change at run time on that disc, which
 * is what the rest of this file exists for.
 *
 * Hardware facts, all public (ps2tek "GS privileged registers" for CMOD and
 * the analog video clock; the CCIR/ITU-R analog broadcast standards for the
 * two line counts and the two line rates):
 *
 *   EE bus clock            147.456 MHz, both modes
 *   NTSC   525 lines/frame, 15734.26 Hz line rate, 59.94 Hz field rate
 *   PAL    625 lines/frame, 15625 Hz line rate,    50 Hz field rate
 *
 * so a field is 147456000 / 59.94 = 2460060 bus cycles on NTSC and
 * 147456000 / 50 = 2949120 on PAL, and a line is one 262.5th of an
 * NTSC field and one 312.5th of a PAL field, which is 9371 and 9437
 * cycles; see RT_CYCLES_PER_HBLANK_NTSC below for why the line comes from
 * the field rather than from the line rate directly.
 *
 * The NTSC numbers here are the ones this runtime has always used, written
 * out unchanged so a US run is byte-identical to what it was before the
 * mode became a variable.
 */
#ifndef ICORECOMP_VIDEO_MODE_H
#define ICORECOMP_VIDEO_MODE_H

#include <cstdint>

/* Unit of the virtual clock: EE bus cycles (BUSCLK; the CPU core runs at
 * 2x). */
constexpr uint64_t RT_BUSCLK_HZ = 147456000ull;

/* The two modes, valued as the SMODE1 CMOD field they program: 2 = NTSC,
 * 3 = PAL. Nothing else is modeled; rt_gs_program_crt is fatal on any other
 * SetGsCrt mode, so a DTV or VESA mode never reaches here. */
enum RtVideoMode : uint32_t {
    RT_VIDEO_NTSC = 2,
    RT_VIDEO_PAL  = 3,
};

/* Field rates. 59.94 is written as the literal this runtime has always
 * used rather than as 60000/1001, so every double derived from it (the
 * audio step, the achievement clock, the profile summary) keeps the value
 * it had. */
constexpr double RT_FIELD_HZ_NTSC = 59.94;
constexpr double RT_FIELD_HZ_PAL  = 50.0;

/* Bus cycles per field: RT_BUSCLK_HZ / the field rate above, truncated. */
constexpr uint64_t RT_CYCLES_PER_FIELD_NTSC = 2460060ull; /* 147456000 / 59.94 */
constexpr uint64_t RT_CYCLES_PER_FIELD_PAL  = 2949120ull; /* 147456000 / 50 */

/* Lines per frame and active lines per frame, the analog standards' own
 * numbers. The active counts are what the blanking below is derived from. */
constexpr uint32_t RT_LINES_PER_FRAME_NTSC = 525;
constexpr uint32_t RT_LINES_PER_FRAME_PAL  = 625;
constexpr uint32_t RT_ACTIVE_LINES_NTSC = 480;
constexpr uint32_t RT_ACTIVE_LINES_PAL  = 576;

/* One H-blank in bus cycles, derived from the field timeline rather than
 * from the line rate directly, so the alarm clock, the H-blank timers and
 * the vblank timeline all count the same line: 262.5 lines per NTSC field
 * and 312.5 per PAL field.
 *
 *   NTSC  2460060 * 2 / 525 = 9371, so 147456000 / 9371 = 15735.7 Hz
 *         against the standard's 15734.26 Hz
 *   PAL   2949120 * 2 / 625 = 9437, so 147456000 / 9437 = 15625.3 Hz
 *         against the standard's 15625 Hz
 *
 * The difference in both cases is the rounding of the field length. */
constexpr uint64_t RT_CYCLES_PER_HBLANK_NTSC =
    RT_CYCLES_PER_FIELD_NTSC * 2 / RT_LINES_PER_FRAME_NTSC;
constexpr uint64_t RT_CYCLES_PER_HBLANK_PAL =
    RT_CYCLES_PER_FIELD_PAL * 2 / RT_LINES_PER_FRAME_PAL;
static_assert(RT_CYCLES_PER_HBLANK_NTSC == 9371ull, "the NTSC H-blank must not move");
static_assert(RT_CYCLES_PER_HBLANK_PAL == 9437ull, "the PAL H-blank is 147456000 / 15625");

/* Vertical blank, in lines per field and then in bus cycles. The line count
 * is the frame's blanking lines split between its two fields:
 * NTSC (525 - 480) / 2 = 22, PAL (625 - 576) / 2 = 24.
 *
 * The NTSC length is the constant this runtime has always used, 206184,
 * kept to the cycle so a US run does not move. It is 22 lines to within 22
 * cycles (22 * 9371 = 206162); the tree carries the rounded-up form and
 * this file does not silently re-derive it. The PAL length is 24 lines at
 * the PAL H-blank above, which is the same construction. */
constexpr uint32_t RT_VBLANK_LINES_NTSC =
    (RT_LINES_PER_FRAME_NTSC - RT_ACTIVE_LINES_NTSC) / 2;
constexpr uint32_t RT_VBLANK_LINES_PAL =
    (RT_LINES_PER_FRAME_PAL - RT_ACTIVE_LINES_PAL) / 2;
static_assert(RT_VBLANK_LINES_NTSC == 22 && RT_VBLANK_LINES_PAL == 24,
    "the blanking line counts are (total - active) / 2");
constexpr uint64_t RT_CYCLES_VBLANK_NTSC = 206184ull;
constexpr uint64_t RT_CYCLES_VBLANK_PAL  = RT_VBLANK_LINES_PAL * RT_CYCLES_PER_HBLANK_PAL;

/* The mode the timeline runs at before the game's first SetGsCrt.
 *
 * Inferred, not measured: a console powers its GS up in the mode of its own
 * region, and this is the European disc. It is only the starting mode: the
 * game's own display option selects 50 Hz or 60 Hz and programs SetGsCrt
 * accordingly, so NTSC timing is a mode this runtime switches into, not a
 * build it is excluded from. Every retail path reaches SetGsCrt in the first
 * few fields of boot (gsb_Init), so this only decides the length of those
 * fields.
 *
 * constexpr because the vblank timeline and the pad tick seed their first
 * deadline from it at static initialization time, before any code runs. */
constexpr RtVideoMode RT_VIDEO_MODE_BOOT = RT_VIDEO_PAL;
constexpr uint64_t RT_CYCLES_PER_FIELD_BOOT = RT_CYCLES_PER_FIELD_PAL;

/* ---- the live mode ------------------------------------------------------ */

RtVideoMode rt_video_mode();
const char* rt_video_mode_name();       /* "NTSC" or "PAL" */

uint64_t rt_cycles_per_field();
uint64_t rt_cycles_vblank();
uint64_t rt_cycles_per_hblank();
double   rt_field_rate_hz();
uint32_t rt_lines_per_frame();

/* Called by rt_gs_program_crt with the CMOD the game programmed, and by
 * nothing else. Logs every change; a repeat of the current mode is silent
 * past the first call. `why` names the caller for the log line. */
void rt_video_set_mode(RtVideoMode mode, const char* why);

/* Registered once by the modules that hold a deadline in cycles, so a mode
 * change can re-derive it instead of letting a stale period run. Called in
 * registration order, after the new mode is in place. Registration is
 * process-lifetime; there is no unregister. */
using RtVideoModeHook = void (*)(RtVideoMode prev, RtVideoMode now);
void rt_video_add_mode_hook(RtVideoModeHook hook);

#endif /* ICORECOMP_VIDEO_MODE_H */
