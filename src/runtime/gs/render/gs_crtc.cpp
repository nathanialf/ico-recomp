/* gs/render/gs_crtc.cpp: CRTC geometry. See gs_crtc.h for the shape of it.
 *
 * Ours (MIT).
 *
 * Measured versus inferred, stated here rather than left to be discovered:
 *
 *   Measured in this project, from ICO (US) and recorded in
 *   gs/gs_parallel_scanout.cpp: NTSC's visible area is 2560 video clocks from
 *   clock 636 and 448 raster lines from line 50. Gameplay programs exactly
 *   that window (DW+1 2560, DH+1 448, DX 636, DY 50, MAGH 4), and MAGH is
 *   one less than the divider, so the divider is 5 and the window is 512
 *   pixels by 224 lines per field, which is the width of the buffer the game
 *   draws into. The attract movie programs
 *   DW+1 2880, DH+1 480, MAGH 3 from the same corner, which is 720 by 240.
 *   Its DISPFB2 carries DBX 36, DBY 12.
 *
 *   Contested, not settled: the FFMD row rule, that is, which buffer row a
 *   circuit reads for output field line L when SMODE2 FFMD is 1. Two rules
 *   are on the table:
 *
 *     stride 1  row = DBY + (L - c_y), the same read as FFMD 0. This is what
 *               paraLLEl-GS does. compute_circuit_rect sets phase_stride to
 *               2 only for alternative_sampling, which is INT && !FFMD
 *               (third_party/parallel-gs/gs/gs_renderer.cpp:4176-4178), so
 *               FFMD takes stride 1, and
 *               third_party/parallel-gs/gs/shaders/sample_circuit.frag:108-109
 *               computes
 *                 coord = single_sampled_coord * uvec2(1, phase_stride)
 *                       + uvec2(dbx, dby + phase),
 *               which never scales DBY. PCSX2 halves the frame rect for
 *               FFMD, which is the same statement.
 *     stride 2  row = DBY + (L - c_y) * 2 + field. This is what
 *               render/shaders/scanout.comp runs today.
 *
 *   The argument that used to stand here, that the movie's DBY 12 skips 24
 *   buffer rows because the decoded picture carries 8 and 17 row black
 *   borders, is an inference from a picture and not a measurement of the
 *   CRTC, so it is withdrawn as the deciding evidence. DBY is no longer
 *   doubled under either rule: scaling a register the game supplied has no
 *   support in any hardware description or reference implementation.
 *
 *   The measurement that settles it: replay one captured gameplay field
 *   through both renderers and compare the top output row against buffer
 *   row 0 and against buffer row 1.
 *
 *   Inferred, not measured: the PAL visible area and its origin. The mode
 *   itself is exercised now. The project retargeted to SCES_507.60 on
 *   2026-09-04, and a PAL run programs CMOD 3 through hw/gspriv.cpp's
 *   rt_gs_program_crt, which is the only writer of SMODE1; the body below
 *   records what that run showed. What is still carried over from the NTSC
 *   measurement, and unconfirmed, is the pair of constants the PAL arm uses:
 *   kModeLinesPal = 512 visible raster lines, at the NTSC origin
 *   (kOriginDx, kOriginDy) and the NTSC clock count. Neither the line count
 *   nor the origin has been checked against a PAL raster, so the placement
 *   inside the frame is unverified even though the mode runs. The warn below
 *   says exactly that, once.
 *
 *   Inferred: the merge blend unit. The GS treats 0x80 as one for alpha, and
 *   gs_swizzle.h's 16-bit expansion agrees, so the circuit merge here divides
 *   by 0x80 and clamps. A measured merge with a partial ALP would settle it;
 *   ICO enables one circuit, so no frame in this game exercises it.
 */
#include "gs_crtc.h"

#include "../../runtime.h"

namespace gsr {

namespace {

/* The visible area of the video mode, in DISPLAY register units rather than
 * in pixels: video clocks across and raster lines down, measured from the
 * origin below.
 *
 * Pixels only exist once a circuit's magnification is applied, which is why
 * the area is kept in clocks here. NTSC's 2560 clocks are 640 pixels at
 * MAGH+1 = 4 and 512 pixels at MAGH+1 = 5, and ICO's gameplay window uses
 * the second of those (DW+1 2560, MAGH 4, so 512 pixels). Measured; see the
 * file comment. */
constexpr uint32_t kModeClocksNtsc = 2560;
constexpr uint32_t kModeLinesNtsc = 448;
constexpr uint32_t kModeClocksPal = 2560;
constexpr uint32_t kModeLinesPal = 512;

/* Origin of the visible area, in DISPLAY register units. */
constexpr uint32_t kOriginDx = 636;
constexpr uint32_t kOriginDy = 50;

bool g_logged_pal = false;
bool g_logged_cmod = false;
bool g_logged_adaptive = false;
bool g_logged_dbxy_ignored = false;
bool g_logged_no_circuit = false;
bool g_logged_magnification = false;

struct ModeArea {
    uint32_t width;             /* pixels, at the circuit's own magnification */
    uint32_t lines_per_field;   /* lines */
};

/* The visible area expressed in the pixel grid the enabled circuits use.
 * magh and magv are the magnifications (MAGH+1, MAGV+1) the frame is built
 * at; see pick_magnification below. The line count is per field whatever
 * SMODE2.INT says: the mode's line total is an interlaced raster's, and a
 * non-interlaced raster scans half of it. The caller decides whether the
 * frame is one field or two. */
ModeArea mode_area(uint32_t cmod, uint32_t magh, uint32_t magv) {
    uint32_t clocks = kModeClocksNtsc;
    uint32_t lines = kModeLinesNtsc;
    if (cmod == 3) {
        clocks = kModeClocksPal;
        lines = kModeLinesPal;
        if (!g_logged_pal) {
            g_logged_pal = true;
            rt_log_warn("gsr", "SMODE1 CMOD 3 (PAL): the visible area used here is %u clocks "
                               "by %u lines at the NTSC origin (%u, %u). The mode runs, but "
                               "that line count and that origin are carried over from the "
                               "NTSC measurement and have not been checked against a PAL "
                               "raster, so the placement inside the frame is unverified",
                        clocks, lines, kOriginDx, kOriginDy);
        }
    } else if (cmod != 2 && !g_logged_cmod) {
        g_logged_cmod = true;
        rt_log_warn("gsr", "SMODE1 CMOD %u is neither NTSC nor PAL; the NTSC visible area "
                           "is used and the placement is not modelled for this mode", cmod);
    }
    ModeArea a{};
    a.width = clocks / (magh ? magh : 1u);
    /* kModeLinesNtsc and kModeLinesPal are the visible line count of the
     * interlaced raster, which is the unit DISPLAY DH and DY are in when INT
     * is 1, and a field is half of it. A non-interlaced raster has half as
     * many lines to begin with, so its visible area is that same half, and
     * the frame is one of them rather than two. The shift is therefore
     * unconditional; what interlace decides is only whether frame_h is
     * lines_per_field or twice it, which crtc_plan does below.
     *
     * This read `lines >> (interlaced ? 1 : 0)`, which gave a non-interlaced
     * raster the whole interlaced line count. Measured on 2026-09-04: ICO PAL
     * programs SMODE2 INT 0 with a DISPLAY2 of DH+1 256 and MAGV 0, and the
     * renderer logged "scanout frame is now 516x512" against that 512x256
     * circuit, so the circuit filled the top half of the frame and the bottom
     * half was BGCOLOR. The aspect derivation at the end of crtc_plan already
     * counts a non-interlaced DH twice against the same mode line total,
     * which is the same statement made in the other direction. */
    a.lines_per_field = (lines >> 1) / (magv ? magv : 1u);
    return a;
}

struct Circuit {
    bool enabled = false;
    uint32_t x = 0, y = 0;   /* position in the frame, pixels and field lines */
    uint32_t w = 0, h = 0;   /* size in pixels and field lines */
    Dispfb fb{};
};

/* One circuit's rectangle, in frame pixels and lines per field.
 *
 * DX and DY are in video clocks and raster lines from the raster origin, and
 * the visible area starts at (kOriginDx, kOriginDy), so the position inside
 * the frame is the difference divided down the same way the size is. A window
 * that starts left of or above the visible area is placed at 0 and its
 * overhang is lost, which is what a set does with it. */
Circuit circuit_rect(const Display& dp, const Dispfb& fb, bool enabled, bool interlaced) {
    Circuit c;
    c.enabled = enabled;
    c.fb = fb;
    const uint32_t magh = dp.magh + 1;
    const uint32_t magv = dp.magv + 1;
    c.w = (dp.dw + 1) / magh;
    c.h = ((dp.dh + 1) >> (interlaced ? 1 : 0)) / magv;
    c.x = dp.dx > kOriginDx ? (dp.dx - kOriginDx) / magh : 0;
    const uint32_t dy_rel = dp.dy > kOriginDy ? (dp.dy - kOriginDy) : 0;
    c.y = (dy_rel >> (interlaced ? 1 : 0)) / magv;
    return c;
}

/* The magnification the frame's pixel grid uses.
 *
 * A circuit's MAGH and MAGV say how many video clocks and raster lines one of
 * its pixels covers, so two circuits with different magnifications do not
 * share a pixel grid. The finer of the two is taken, so neither circuit has
 * to be downsampled to land in the frame. ICO enables one circuit, so the
 * case never arises on this disc and is logged once if it ever does. */
void pick_magnification(const Display& dp1, bool en1, const Display& dp2, bool en2,
                        uint32_t* magh, uint32_t* magv) {
    *magh = 0;
    *magv = 0;
    if (en1) {
        *magh = dp1.magh + 1;
        *magv = dp1.magv + 1;
    }
    if (en2) {
        const uint32_t h = dp2.magh + 1;
        const uint32_t v = dp2.magv + 1;
        if (*magh == 0 || h < *magh) *magh = h;
        if (*magv == 0 || v < *magv) *magv = v;
    }
    if (en1 && en2 && (dp1.magh != dp2.magh || dp1.magv != dp2.magv)
        && !g_logged_magnification) {
        g_logged_magnification = true;
        rt_log_warn("gsr", "the two display circuits have different magnifications "
                           "(MAGH %u/%u, MAGV %u/%u); the frame uses the finer grid",
                    dp1.magh, dp2.magh, dp1.magv, dp2.magv);
    }
    if (*magh == 0) *magh = 1;
    if (*magv == 0) *magv = 1;
}

} // namespace

ScanoutPlan crtc_plan(const RegisterFile& regs, uint32_t raster, uint32_t deinterlace,
                      uint32_t field) {
    ScanoutPlan plan;

    const Pmode pm = decode_pmode(regs.read_priv(GS_PRIV_PMODE));
    const Smode1 sm1 = decode_smode1(regs.read_priv(GS_PRIV_SMODE1));
    const Smode2 sm2 = decode_smode2(regs.read_priv(GS_PRIV_SMODE2));
    const Dispfb fb1 = decode_dispfb(regs.read_priv(GS_PRIV_DISPFB1));
    const Dispfb fb2 = decode_dispfb(regs.read_priv(GS_PRIV_DISPFB2));
    const Display dp1 = decode_display(regs.read_priv(GS_PRIV_DISPLAY1));
    const Display dp2 = decode_display(regs.read_priv(GS_PRIV_DISPLAY2));
    const uint64_t bg = regs.read_priv(GS_PRIV_BGCOLOR);

    plan.cmod = sm1.cmod;
    /* Host side only, for the field diagnostics: the registers this plan came
     * from, so one log line can show the window arithmetic in numbers instead
     * of only in its result. Nothing in the renderer reads them. */
    plan.pmode_raw = regs.read_priv(GS_PRIV_PMODE);
    plan.smode2_raw = regs.read_priv(GS_PRIV_SMODE2);
    plan.display1 = dp1;
    plan.display2 = dp2;
    const bool interlaced = sm2.interlaced != 0;
    uint32_t magh = 1, magv = 1;
    pick_magnification(dp1, pm.en1 != 0, dp2, pm.en2 != 0, &magh, &magv);
    const ModeArea area = mode_area(sm1.cmod, magh, magv);
    plan.mode_width = area.width;
    plan.mode_height = area.lines_per_field;

    Circuit c1 = circuit_rect(dp1, fb1, pm.en1 != 0, interlaced);
    Circuit c2 = circuit_rect(dp2, fb2, pm.en2 != 0, interlaced);

    if (!c1.enabled && !c2.enabled) {
        /* Before the game enables a circuit there is nothing to scan out.
         * Reported once, because a run that never gets past this is a run
         * with a black window and no other symptom. */
        if (!g_logged_no_circuit) {
            g_logged_no_circuit = true;
            rt_log_info("gsr", "PMODE enables no display circuit; nothing is scanned out "
                               "until the game enables one");
        }
        plan.have_picture = false;
        return plan;
    }

    /* The frame. */
    uint32_t frame_w = area.width;
    uint32_t frame_lines = area.lines_per_field;
    if (raster == GSR_RASTER_WINDOW) {
        if (c1.enabled) {
            if (c1.x + c1.w > frame_w) frame_w = c1.x + c1.w;
            if (c1.y + c1.h > frame_lines) frame_lines = c1.y + c1.h;
        }
        if (c2.enabled) {
            if (c2.x + c2.w > frame_w) frame_w = c2.x + c2.w;
            if (c2.y + c2.h > frame_lines) frame_lines = c2.y + c2.h;
        }
        /* The one game-supplied value any setting in this project overrides,
         * and it is a presentation register the game never reads back. See
         * docs/SETTINGS.md section 6. */
        const Dispfb& read = c2.enabled ? c2.fb : c1.fb;
        if ((read.dbx || read.dby) && !g_logged_dbxy_ignored) {
            g_logged_dbxy_ignored = true;
            rt_log_info("gsr", "raster window: circuit%u DISPFB DBX %u DBY %u ignored, "
                               "the buffer is read from its origin (display.raster)",
                        c2.enabled ? 2u : 1u, read.dbx, read.dby);
        }
        c1.fb.dbx = c1.fb.dby = 0;
        c2.fb.dbx = c2.fb.dby = 0;
    } else {
        /* crt: the frame is the mode area and a circuit that overruns it is
         * cropped on the right and the bottom. The crop happens in the
         * shader, which does not write outside the frame. */
    }

    /* All three modes reach the shader. Adaptive is this renderer's own
     * motion filter, named once so a run's log says which picture it is
     * looking at, and shaders/scanout.comp states what it does. */
    const uint32_t deint = deinterlace;
    if (deint == GSR_DEINTERLACE_ADAPTIVE && !g_logged_adaptive) {
        g_logged_adaptive = true;
        rt_log_info("gsr", "deinterlace adaptive: this renderer's own motion "
                           "filter, which will not match any other renderer's and is "
                           "excluded from the parity gate");
    }

    ScanoutPush& p = plan.push;
    p.frame_w = frame_w;
    p.frame_h = interlaced ? frame_lines * 2 : frame_lines;
    p.field = field & 1u;
    p.deinterlace = deint;
    p.interlaced = interlaced ? 1u : 0u;
    p.ffmd = sm2.ffmd;
    /* BGCOLOR is R in 0..7, G in 8..15, B in 16..23. */
    p.bgcolor = (uint32_t)(bg & 0x00FFFFFFull);
    p.merge = (uint32_t)(pm.en1 & 1u)
            | ((pm.en2 & 1u) << 1)
            | ((pm.mmod & 1u) << 2)
            | ((pm.amod & 1u) << 3)
            | ((pm.slbg & 1u) << 4)
            | ((pm.alp & 0xFFu) << 8);

    p.c1_enable = c1.enabled ? 1u : 0u;
    p.c1_base_block = c1.fb.fbp * 32u; /* DISPFB FBP counts pages, 32 blocks each */
    p.c1_fbw = c1.fb.fbw;
    p.c1_psm = c1.fb.psm;
    p.c1_dbx = c1.fb.dbx;
    p.c1_dby = c1.fb.dby;
    p.c1_x = c1.x;
    p.c1_y = c1.y;
    p.c1_w = c1.w;
    p.c1_h = c1.h;

    p.c2_enable = c2.enabled ? 1u : 0u;
    p.c2_base_block = c2.fb.fbp * 32u;
    p.c2_fbw = c2.fb.fbw;
    p.c2_psm = c2.fb.psm;
    p.c2_dbx = c2.fb.dbx;
    p.c2_dby = c2.fb.dby;
    p.c2_x = c2.x;
    p.c2_y = c2.y;
    p.c2_w = c2.w;
    p.c2_h = c2.h;

    plan.have_picture = true;

    /* Display aspect.
     *
     * The mode area is the active area of an analog set, shown 4:3, and PS2
     * pixels are not square, so the frame's own width:height is not its shape
     * on screen. In crt the frame is exactly the mode area, so the answer is
     * 4:3. In window the frame is the circuit's own window, and its shape
     * follows from the registers the game programmed: DW+1 clocks against the
     * mode's own clocks and DH+1 lines against the mode's own lines.
     *
     * Both references come from the same pair mode_area above uses, so PAL
     * derives the shape the way NTSC does instead of falling back to a flat
     * 4:3: 2560 clocks in both modes, 448 lines on NTSC and 512 on PAL. NTSC
     * gameplay (2560 x 448) gives 4:3 and the NTSC movie (2880 x 480) gives
     * 1.4, which keeps its pixels the same size as gameplay's; PAL gameplay
     * (2560 x 512) gives 4:3 and the PAL movie's own 2880 x 576 display env
     * gives 4:3 as well, a 720x576 picture filling its 4:3 area. A CMOD that
     * is neither reads the NTSC pair, which is what mode_area does with it
     * too and what it has already logged. The derivations are recorded in
     * docs/SETTINGS.md section 6, and gs_parallel_scanout.cpp derives the
     * same number the same way for the other backend. */
    const double kFourThree = 4.0 / 3.0;
    plan.display_aspect = kFourThree;
    if (raster == GSR_RASTER_WINDOW) {
        const Display& dp = c2.enabled ? dp2 : dp1;
        const double mode_clocks = double(sm1.cmod == 3 ? kModeClocksPal : kModeClocksNtsc);
        const double mode_lines = double(sm1.cmod == 3 ? kModeLinesPal : kModeLinesNtsc);
        const double clocks = double(dp.dw) + 1.0;
        const double lines = (double(dp.dh) + 1.0) * (interlaced ? 1.0 : 2.0);
        if (clocks > 0.0 && lines > 0.0) {
            plan.display_aspect = kFourThree * (clocks / mode_clocks) * (mode_lines / lines);
        }
    }
    return plan;
}

} // namespace gsr
