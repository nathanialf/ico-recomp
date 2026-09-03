/* gs/gs_parallel_scanout.cpp: the vsync path of the paraLLEl-GS shim.
 *
 * Part of libicorecomp-parallel-gs; see gs_parallel_lib.cpp for the library
 * overview and gs_parallel_impl.h for the RtPgs type. Holds the display
 * aspect derivation, the display copy phase snoop that feeds VSyncInfo::phase,
 * RtPgs::vsync itself with its scanout geometry and CRTC logging, and the
 * screenshot readback of the scanout image.
 */
#include "gs_parallel_impl.h"

#include "gs_interface.hpp"
#include "gs_readback.h"

#include <cstring>

namespace {

/* Scanout aspect ratio.
 *
 * paraLLEl-GS never reports a display aspect directly. It reports the mode
 * area (mode_width x mode_height) as "reference for the target output aspect
 * ratio" and the part of it the CRTC actually scanned out (internal_width x
 * internal_height); see ScanoutResult in gs_renderer.hpp. Every NTSC and PAL
 * mode area the renderer models is the active area of an analog TV, which is
 * displayed 4:3. PS2 pixels are not square, so the scanout image's own
 * width:height is not the ratio to present at:
 *
 *   display aspect = 4/3 * (internal_w / mode_w) / (internal_h / mode_h)
 *
 * Measured for ICO (US), from the GS_DISPLAY2/GS_SMODE2 values the game
 * programs: NTSC, INT=1, FFMD=1, DISPLAY2 DW+1=2560 with MAGH+1=5 (512
 * pixels) and DH+1=448. FFMD with INT forces a deinterlaced scanout, so
 * gs_renderer.cpp does not double mode_height and the mode lands at 512x224
 * after adapt_to_internal_horizontal_resolution rescales mode_width by
 * clock_divider/(MAGH+1). internal then equals mode, the fraction terms
 * cancel, and the target is a plain 4:3. Presenting at 512:224 (2.29:1),
 * which is what this file used to do, stretched the picture 1.71x
 * horizontally and letterboxed the result inside the window.
 *
 * The image's own size is no better a source: force_deinterlace runs
 * fastmad_deinterlace, so result.image comes back 512x448 here while
 * internal/mode stay 512x224. Vertical sampling doubled, screen area did not.
 *
 * double_strike (240p) needs no correction here. The note in gs_renderer.cpp
 * about doubling the height for aspect purposes applies when the aspect is
 * derived from raw pixel counts; in the mode-fraction form a 240p picture
 * already covers the whole mode height, so internal_h / mode_h is 1 either
 * way.
 *
 * The fraction terms are written out but cannot currently move: the merged
 * scanout path sets internal equal to mode (gs_renderer.cpp:4790-4791,
 * "result.internal_width = mode_width"), and the one path where they differ is
 * the raw_circuit_scanout early return, which RtPgs::vsync never asks for.
 * High-resolution scanout, which RtPgs::vsync does ask for at render scale 4
 * and up, does not move them either: it doubles the returned image
 * (gs_renderer.cpp:4792-4793 shifts image_info by the flag) and leaves
 * internal and mode alone, so the fractions still cancel. So for every option
 * set this shim passes today the answer is exactly 4:3. The terms stay because
 * they are what makes that a derivation rather than a constant someone has to
 * re-derive if raw_circuit_scanout is ever enabled.
 *
 * Returns 0 when the renderer reported no usable mode. */
constexpr bool kScanoutOverscan = false;
constexpr double kModeDisplayAspect = 4.0 / 3.0;
/* An earlier revision derived this from the active area and the circuit's
 * MAGH instead of from the mode area, because a mode area grown to hold an
 * oversized window is no longer the 4:3 rectangle. It reported 0.3333 at boot:
 * before the game enables a circuit, PMODE EN1 and EN2 are both 0, the
 * expression fell back to DISPLAY1, which ICO never writes, and MAGH read 0,
 * so the clock/MAGH scale came out 4 instead of 0.8 and divided the answer by
 * 4. The mode fraction below has no such dependency, and it is correct again
 * now that the scanout keeps the fixed active area as the mode area. Any
 * future move back to a MAGH form has to answer "which circuit, and what if
 * none is enabled" first. */

double scanout_display_aspect(const ParallelGS::ScanoutResult& s) {
    /* Two mode families this constant does NOT describe, neither reachable
     * from here:
     *
     *  - Overscan. Those mode areas are 712x240 (NTSC) and 712x288 (PAL)
     *    against 640x224 / 640x256 active areas. kScanoutOverscan is the
     *    single place info.overscan is set, and this assert is the gate.
     *  - LC_HDTV (SMODE1 CMOD progressive + HDTV clock): gs_renderer.cpp
     *    reports 1920x540 and 1280x720 mode areas, which are 16:9, not 4:3.
     *    ScanoutResult carries no CMOD/LC field, so this function cannot tell
     *    them apart and would silently squeeze such a picture by 25%. It never
     *    sees one: rt_gs_program_crt (hw/gspriv.cpp) is the only writer of
     *    SMODE1 and calls rt_fatal on any SetGsCrt mode that is not NTSC or
     *    PAL. Anyone lifting that fatal has to derive the aspect here first. */
    static_assert(!kScanoutOverscan, "kModeDisplayAspect assumes the non-overscan mode area");
    if (!s.mode_width || !s.mode_height || !s.internal_width || !s.internal_height) return 0.0;
    return kModeDisplayAspect
         * (double(s.internal_width) / double(s.mode_width))
         * (double(s.mode_height) / double(s.internal_height));
}

/* info.force_progressive as RtPgs::vsync always sets it. Named so the CRTC
 * mirrors below cannot drift from the value the renderer is handed. */
constexpr bool kForceProgressive = true;

/* GSRenderer::scanout_is_interlaced (gs_renderer.cpp:4204-4212), mirrored
 * exactly: SMODE2 INT, except that INT && !FFMD under force_progressive
 * scans out progressive. ICO runs INT=1 FFMD=1 so the exception is not taken
 * here, but the mirror is written out rather than assumed, because a caller
 * that reports a height the renderer did not use is worse than no line. */
bool crtc_is_interlaced(const ParallelGS::PrivRegisterState& priv) {
    bool is_interlaced = priv.smode2.INT != 0;
    if (is_interlaced && !priv.smode2.FFMD && kForceProgressive) is_interlaced = false;
    return is_interlaced;
}

/* The crtc_rects height in gs_renderer.cpp:4605-4608, mirrored exactly,
 * including the round up to an even count on the progressive arm. The unit is
 * lines per field, which is the unit mode_height and the reported circuit
 * rect are in, so the placement line and the crop line below compare
 * directly. */
uint32_t crtc_lines_per_field(const ParallelGS::PrivRegisterState& priv,
                              const ParallelGS::DISPLAYBits& dp) {
    const bool interlaced = crtc_is_interlaced(priv);
    uint32_t height = (uint32_t(dp.DH) + 1u + (interlaced ? 1u : 0u)) >> (interlaced ? 1 : 0);
    if (!interlaced) height = (height + 1u) & ~1u;
    return height;
}

} // namespace

void RtPgs::note_xyoffset(uint32_t reg, uint32_t ofy) {
    const uint32_t frac = ofy & 0xFu;
    if (frac == 8u) {
        /* The half pixel form. It names the register and the base value. */
        if (m_copy_ofy_base < 0) {
            m_copy_ofy_reg = reg;
            m_copy_ofy_base = int32_t(ofy & ~0xFu);
        }
        if (reg == m_copy_ofy_reg && int32_t(ofy & ~0xFu) == m_copy_ofy_base)
            m_copy_parity = 1;
    } else if (frac == 0u && m_copy_parity != 1 && m_copy_ofy_base >= 0 &&
               reg == m_copy_ofy_reg && int32_t(ofy) == m_copy_ofy_base) {
        /* The +8 form is unambiguous, so it wins for the field once seen: a
         * later write of the same base with a zero fraction is some other
         * user of the register, not a second display copy. */
        m_copy_parity = 0;
    }
    /* "A draw programmed the copy's XYOFFSET during this field", which is the
     * question info.high_resolution_scanout asks: which producer wrote the
     * buffer, not which field the copy was drawn for. It is deliberately
     * independent of the parity latch above, because that latch stays -1 on
     * every copy field until the first half pixel write is seen, and a
     * DISPFB write in one of those boot fields would otherwise latch
     * m_hires_from_copy false and hold it there. Once the base is latched
     * this is the copy's own register and base; before that there is nothing
     * to match against, so any XYOFFSET write counts, which is a wider test
     * for the handful of fields between boot and the first half pixel
     * field. */
    if (m_copy_ofy_base < 0 ||
        (reg == m_copy_ofy_reg && int32_t(ofy & ~0xFu) == m_copy_ofy_base))
        m_copy_seen = true;
}

void RtPgs::snoop_display_copy_phase(const uint8_t* data, uint32_t qwords) {
    /* GIF tag framing as in hw/gif.cpp's track_framing and the A+D decode as
     * in hw/geomcheck.cpp's rt_geom_scan; both are public hardware facts
     * (ps2tek). Only the canonical register packet is walked: PACKED, one
     * register per loop, REGS[0] = A+D, which is the shape gsb_setNormalReg
     * and the scratchpad templates use. Every other tag is stepped over
     * whole, so this costs one iteration per tag rather than one per vertex
     * and can stay on for every packet of every field. */
    uint32_t i = 0;
    while (i < qwords) {
        uint64_t lo, hi;
        std::memcpy(&lo, data + size_t(i) * 16, 8);
        std::memcpy(&hi, data + size_t(i) * 16 + 8, 8);
        ++i;
        const uint32_t nloop = uint32_t(lo & 0x7FFFu);
        const uint32_t flg = uint32_t((lo >> 58) & 3u);
        uint32_t nreg = uint32_t((lo >> 60) & 15u);
        if (nreg == 0) nreg = 16;
        uint64_t payload;
        switch (flg) {
            case 0: payload = uint64_t(nloop) * nreg; break;
            case 1: payload = (uint64_t(nloop) * nreg + 1) / 2; break;
            default: payload = nloop; break;
        }

        /* A tag whose payload runs past this submission is either a PATH3
         * split (hw/gif.cpp keeps that framing, this walker does not) or a
         * misparse of a continuation qword. Either way, stop rather than read
         * a register out of data that is not one. */
        if (payload > uint64_t(qwords - i)) return;

        if (flg == 0 && nreg == 1 && (hi & 15u) == 0x0Eu) {
            const uint32_t n = uint32_t(payload);
            for (uint32_t l = 0; l < n; ++l) {
                uint64_t d0, d1;
                std::memcpy(&d0, data + size_t(i + l) * 16, 8);
                std::memcpy(&d1, data + size_t(i + l) * 16 + 8, 8);
                const uint32_t addr = uint32_t(d1 & 0xFFu);
                if (addr == 0x18u || addr == 0x19u)
                    note_xyoffset(addr, uint32_t((d0 >> 32) & 0xFFFFu));
            }
        }

        i += uint32_t(payload);
    }
}

uint32_t RtPgs::vsync(unsigned field) {
    ++m_vsyncs;
    const bool presented = m_transfer_since_vsync;
    m_transfer_since_vsync = false;

    m_iface->flush();

    ParallelGS::VSyncInfo info = {};
    /* The phase is the vertical position the field in VRAM was drawn for,
     * not a property of the clock, so it is taken from the copy itself when
     * the copy can be seen, and from the field counter otherwise.
     *
     * What the game does. ICO renders the frame at 512x448 into VRAM 0x80000
     * and once per field draws two sprites into the 512x224 PSMCT32 buffer at
     * VRAM 0 that the CRTC reads: a black sprite over the whole buffer, then
     * one textured sprite mapping the 448 rows onto the 224 rows.
     * gsb_setNormalReg builds it (XYOFFSET_1 OFY = (0x800 - height/4) * 16,
     * sprite y in [-0.25, 223.75), V in [0.5, 448.5)), and the SDK helper at
     * 0x00243640 (../ico src/cod/vendor_2418A0.c:203-212) rewrites OFY as
     * either t or t + 8 (half a pixel in the 4-bit fraction) depending on its
     * fourth argument. That argument is CSR bit 13 itself, not its
     * complement: ../ico ios/pad.c:12 keeps D_00631950 = CSR.FIELD ^ 1, and
     * gsb_StageSetting.s (0x00114008-0x00114010) passes sltiu of it against
     * 1, which is D_00631950 == 0, which is CSR.FIELD.
     *
     * What that does to the picture. XYOFFSET is subtracted from the vertex,
     * so the +8 moves the sprite half a pixel up. v(y) = 2y + 1, a
     * destination pixel Y's two vertical sub-samples sit at y = Y -/+ 0.25
     * (evaluate_barycentric_ij at fb_pixel << (3 - 1), less the average
     * sampling offset triangle_setup.comp:800-815 folds into error_i/j), so
     * they read source rows 2Y and 2Y+1 without the +8 and 2Y+1 and 2Y+2 with
     * it. Sub-sample s of destination row Y is stored as row 2Y+s of the
     * super-sampled 512x224 buffer and read back as that row by
     * sample_circuit.frag:134, so buffer row n holds source row n on the
     * argument-0 field and source row n+1 on the argument-1 field.
     *
     * What the renderer does. high_resolution_scanout on an FFMD game turns
     * on field aware rendering, whose only per-field effect is
     * gs_renderer.cpp:4870 / 4903, `if (field_aware_rendering && !info.phase)
     * vp.y -= 1.0f`: phase 0 puts buffer row m+1 on merged row m, phase 1
     * puts buffer row m there. Nothing else in the scanout is per-field. The
     * CRTC y offset is normalised away by the crtc_shift subtraction
     * (gs_renderer.cpp:4680-4693, reached because overscan and crtc_offsets
     * are both off and the circuit height equals the mode height), and
     * compute_circuit_rect's own phase stays 0 for FFMD
     * (gs_renderer.cpp:4177 only assigns it when alternative_sampling, which
     * is INT && !FFMD).
     *
     * (Renderer line numbers in this file are the submodule with
     * third_party/patches/parallel-gs-0001-full-pixel-raster-snap.patch
     * applied and nothing else. 0001 inserts 17 lines at gs_renderer.cpp:2609,
     * so a citation below that point is the pristine number plus 17. The
     * configure step also applies
     * parallel-gs-0002-report-scanout-placement.patch, which inserts 9 further
     * lines at 4790, so a configured tree reads 9 higher than these numbers
     * from there down. The gs_interface.hpp citations are pristine: neither
     * patch touches that header. 0002 does add to gs_renderer.hpp at line 50,
     * which nothing here cites by number yet.)
     *
     * The two therefore agree, both landing on merged row m = source row m+1,
     * exactly when phase equals the helper's argument: argument 0 needs the
     * lift, argument 1 does not.
     *
     * The field counter can only guess at that. The buffer scanned out at
     * vsync F was drawn during field F-1 (sched.cpp calls the backend from
     * rt_gs_vblank_start, before the guest's handler for that field runs), so
     * argument = CSR.FIELD during F-1 = (F & 1) ^ 1, which is what the
     * counter branch below hands over, and it is what the 4x measurement
     * picked: the raw parity wobbled by a full native line, the flipped one
     * much less. But "drawn during field F-1" is a timing assumption. When a
     * field is repeated because the guest did not finish a new copy in time,
     * the same buffer is scanned out under both parities and moves by one
     * merged row, half a line of the game's 224 line raster, which is the
     * residual wobble that was left.
     *
     * snoop_display_copy_phase removes the assumption: it reads the OFY the
     * copy was actually drawn with out of the GIF stream since the previous
     * vsync, which is by construction the traffic that produced the buffer
     * being scanned out now (submissions are synchronous). A field with no
     * copy in it repeats the previous phase, so a repeated picture stays put
     * instead of moving half a line. Until the half pixel form has been seen
     * once there is nothing to calibrate against and the field counter is
     * used, which is the previous behaviour exactly.
     *
     * The attract movie is the case where "no copy" does not mean "no new
     * picture", and holding through it is wrong. It never draws the display
     * copy: the decoded picture is colour converted into two field buffers in
     * VRAM, and the movie's own vblank handler (../ico
     * asm/nonmatchings/ito/mpeg/mv_sub/func_0023EE28.s, 0x0023EE44 reads CSR
     * bit 13 into D_00633B90) calls func_0023EDF0 (../ico
     * ito/mpeg/mv_sub.c:115-135), which rewrites the low 9 bits of the
     * DISPFB2 word, the FBP field, from one of two stored values selected by
     * that CSR.FIELD bit, and then programs the display environment through
     * func_00241F20 (../ico
     * asm/matchings/src/cod/vendor_2418A0/func_00241F20.s, the .L00241F84 arm
     * writes PMODE, SMODE2, DISPFB2, DISPLAY2 and BGCOLOR). DBY and DISPLAY2
     * stay put, so the whole per-field difference is which buffer the CRTC
     * reads, and the field that buffer belongs to is the CSR.FIELD the
     * handler read, which is what the display copy's fourth argument is too.
     *
     * The two values are set once, by func_0023E578 (../ico
     * asm/nonmatchings/ito/mpeg/mv_sub/func_0023E578.s): at 0023E5EC it
     * stores 0 to self+0x28, and at 0023E5F4 it stores
     * ceil(width / 64) * ceil((height / 2) / 32) to self+0x2C, which is the
     * page count of a width by height/2 PSMCT32 buffer (0023E578-0023E5C0
     * builds it, and FBP counts 8192 byte pages of 64 by 32 pixels). So the
     * pair is one buffer at page 0 and a second immediately after it, each
     * half the picture's lines. Two adjacent half-height buffers displayed on
     * alternate fields under FFMD = 1 is a field pair: the CRTC reads each as
     * a packed field and lays buffer A's row n on display line 2n and buffer
     * B's row n on 2n+1, so the only content assignment that is a coherent
     * picture on hardware is the even lines in A and the odd lines in B.
     * Holding pinned every field of the movie on one parity, so the two
     * buffers landed on the same merged row instead of one row apart: the
     * same half-line wobble the copy snoop exists to remove, now at the full
     * one-merged-row size because the two buffers hold different lines of the
     * picture rather than the same lines resampled.
     *
     * m_dispfb_flip is the discriminator. A field with no copy but with a
     * DISPFB write that changed value is a new picture the guest chose during
     * that field, so the field counter, which is CSR.FIELD during that field,
     * is the answer. A field with neither is a genuine repeat and still
     * holds. The counter is not a weaker measurement here than the write
     * time would be: writes seen between vsync F-1 and vsync F happened
     * during field F-1, whose CSR.FIELD is (F & 1) ^ 1, which is the counter.
     *
     * At 1x the same value drives the weave instead (high_resolution_scanout
     * is off, force_deinterlace is on, gs_renderer.cpp:4246-4247 and
     * 5045-5062; weave.frag puts the current field on rows where
     * (y & 1) == phase). The argument-0 field reads v = 2Y+1 against the
     * argument-1 field's 2Y+2, so it is the earlier of the two and belongs on
     * the even rows, which is phase 0: the same assignment, so one derivation
     * serves both paths. */
    const unsigned counter_phase = (field & 1) ^ 1u;
    unsigned phase = counter_phase;
    const int copy_parity = m_copy_parity;
    const bool copy_seen = m_copy_seen;
    const bool dispfb_flip = m_dispfb_flip;
    /* Not cleared here: a field the renderer produced no image for cannot
     * carry the line, so the change stays pending until one can. */
    const bool display_geom_changed = m_display_geom_changed;
    bool phase_from_copy = false;
    bool phase_from_flip = false;
    bool phase_held = false;
    if (m_copy_ofy_base >= 0) {
        if (copy_parity >= 0) {
            phase = unsigned(copy_parity);
            phase_from_copy = true;
        } else if (dispfb_flip) {
            /* phase stays counter_phase */
            phase_from_flip = true;
        } else if (m_last_phase >= 0) {
            phase = unsigned(m_last_phase);
            phase_held = true;
        }
    }
    info.phase = phase;
    m_last_phase = int(phase);
    m_copy_parity = -1;
    m_copy_seen = false;
    m_dispfb_flip = false;
    if (phase_held) ++m_phase_held;
    if (phase_from_flip) ++m_phase_from_flip;
    /* Which producer wrote the buffer being scanned out. Independent of the
     * calibration latch above, because the question is not which phase to use
     * but whether the buffer's sub-samples carry distinct source rows. It
     * keys on m_copy_seen and not on the parity, so it is right from the
     * first copy field rather than from the first half pixel field; see
     * note_xyoffset. Sticky: a field with neither signal scans out the same
     * buffer the last decided field did, so it keeps that field's answer. */
    if (copy_seen) m_hires_from_copy = true;
    else if (dispfb_flip) m_hires_from_copy = false;
    if (phase_from_copy && phase != counter_phase) ++m_phase_disagreed;

    /* Two bounded lines, both about the one value that decides the vertical
     * placement. The first fires once, when the half pixel XYOFFSET is first
     * recognised, and names the register and base so a run can be checked
     * against gsb_setNormalReg's own numbers. The second fires when the copy
     * and the field counter disagree, or when a field carried no copy at all,
     * on powers of two so a persistent disagreement is visible without
     * running for the whole session. */
    if (m_copy_ofy_base >= 0 && !m_copy_ofy_logged) {
        m_copy_ofy_logged = true;
        logf("paraLLEl-GS: display copy XYOFFSET_%u OFY base %d (%.4f px),"
             " half-pixel form seen; scanout phase now follows the copy",
             m_copy_ofy_reg == 0x19u ? 2u : 1u, m_copy_ofy_base,
             double(m_copy_ofy_base) / 16.0);
    } else if (m_copy_ofy_base < 0 && m_copy_ofy_search_left) {
        if (--m_copy_ofy_search_left == 0) {
            logf("paraLLEl-GS: no fractional XYOFFSET OFY in the first %llu fields;"
                 " scanout phase stays on the field counter",
                 (unsigned long long)m_vsyncs);
        }
    }
    if (phase_held || phase_from_flip || (phase_from_copy && phase != counter_phase)) {
        const uint64_t n = phase_held      ? m_phase_held
                         : phase_from_flip ? m_phase_from_flip
                                           : m_phase_disagreed;
        if ((n & (n - 1)) == 0) {
            logf("paraLLEl-GS: field %llu phase %u from %s (counter would say %u);"
                 " held=%llu disagreed=%llu dispfb=%llu",
                 (unsigned long long)m_vsyncs, phase,
                 phase_held      ? "the previous field, no display copy this field"
                 : phase_from_flip ? "the field counter, no display copy but DISPFB changed this field"
                                   : "the display copy",
                 counter_phase,
                 (unsigned long long)m_phase_held,
                 (unsigned long long)m_phase_disagreed,
                 (unsigned long long)m_phase_from_flip);
        }
        /* First field that declined the hold because the guest re-pointed the
         * CRTC: re-arm the CRTC lines below so a log carries a few
         * consecutive fields from inside that section, where the FBP
         * alternation is visible. Once per run, like the geometry line's own
         * re-arm. */
        if (phase_from_flip && m_phase_from_flip == 1) m_crtc_log_left = 6;
    }
    info.force_progressive = kForceProgressive;
    info.anti_blur = true;
    info.adapt_to_internal_horizontal_resolution = true;
    /* Paired with kModeDisplayAspect: that constant is the aspect of the
     * non-overscan mode area only. Flipping this to true without deriving a
     * new constant distorts geometry. */
    info.overscan = kScanoutOverscan;
    /* Placement rule.
     *
     * The output frame is the area the renderer models as visible for the
     * video mode: for non-overscan NTSC that is 640 raster pixels by 224 lines
     * per field starting at raster (159, 25), which in DISPLAY units is 2560
     * clocks from clock 636 and 448 lines from line 50 (clock divider 4,
     * gs_registers.hpp:723). Measured, ICO's gameplay window is exactly that:
     * DW+1 2560, DH+1 448, DX 636, DY 50. The attract movie programs DW+1 2880
     * and DH+1 480 from the same DX 636 and DY 50, so it shares the top left
     * corner and runs 320 clocks further right and 32 lines further down.
     *
     * What a TV does with that excess is not established here, and this file
     * does not claim it is hidden. A real NTSC active line is nearer 710
     * raster pixels than 640, so the 80 columns the frame drops are not all
     * outside what a set shows; 640x224 is the renderer's safe area, not the
     * analog active area. So the position taken, in one place and shared with
     * the crop lines below, is: the frame is the renderer's non-overscan mode
     * area, which is what this shim asks for by asking for nothing; the movie
     * is cropped against it, which is a known divergence and is logged as one;
     * and whether to present the overscan area instead (info.overscan, a
     * 712x240 mode area, which needs kModeDisplayAspect re-derived first) is
     * not established, so nothing is changed on its own.
     *
     * Fitting the movie to the frame is not the answer either: it moves the
     * movie's origin away from gameplay's, which is the one thing the raster
     * fixes. The placement lines below report where each window actually
     * landed, so all of this stays checkable from a log. */
    /* Asked for at 4x and up, which is the minimum the renderer documents
     * (gs_interface.hpp:186-192) and the minimum its scanout path enforces:
     * gs_renderer.cpp:4256-4262 drops the request when either sampling axis
     * has no extra samples, and only X4 and above set both
     * (gs_interface.cpp:70-105).
     *
     * The reason this works for ICO at all is super-sampled textures, which
     * the constructor and set_render_scale turn on with the same 4x test.
     * ICO renders the frame at 512x448 into VRAM 0x80000 and then, once per
     * field, draws two sprites into the 512x224 PSMCT32 buffer at VRAM 0
     * that the CRTC reads: a black sprite over the whole buffer, then one
     * textured sprite that maps the full 448 rows onto the full 224 rows.
     * With single-sampled textures every sub-sample of a destination pixel
     * reads the same texel, the display buffer's sub-samples are all equal,
     * and high-resolution scanout has nothing to reconstruct: that is the
     * blit case gs_interface.hpp:188-191 names.
     *
     * With super-sampled textures the source render target is bound as a
     * texture array of its own sub-samples (gs_interface.cpp:1595-1603 for
     * the promotion, page_tracker.cpp:147-153 and :176 for the "this page
     * was a framebuffer" test that gates it), triangle_setup.comp:683-799
     * keeps the copy on the per-sample path (its UV delta is 1:1 in x and
     * 2:1 in y, so it is neither the downsample nor the upsample case and
     * lands in the blit-to-FB zone that keeps TEX_PER_SAMPLE_BIT), and
     * ubershader.comp:1155-1201 then gives each destination sub-sample its
     * own source sub-sample. The two vertical sub-samples of a destination
     * row therefore carry the two distinct source rows 2Y and 2Y+1, so the
     * 512x224 buffer's sub-samples describe the whole 448-row render, and
     * sample_circuit.frag:110-141 reads them back at double resolution.
     *
     * The renderer can still decline: gs_renderer.cpp:4256-4262 and :4377
     * turn it off for EXTWRITE, non-progressive requests and the HDTV modes.
     * scanout.high_resolution_scanout below reports what actually happened,
     * as hires= on the geometry line.
     *
     * All of which holds only for a display buffer the display copy wrote.
     * The attract movie's buffers do not come from a draw at all: the picture
     * is uploaded into VRAM by the movie's own GIF chains, so every
     * sub-sample of a destination pixel holds the same uploaded texel and the
     * buffer carries 240 rows, not 480 rows spread over its sub-samples. Two
     * such buffers, one per field, are exactly the interlaced pair a CRT
     * weaves: field 0's rows on the even display lines, field 1's on the odd.
     * field_aware_rendering cannot reconstruct that. All it does is shift a
     * whole field by one merged row (gs_renderer.cpp:4870 / 4903), so each
     * field is scanned out on its own, line doubled, and the disjoint row
     * sets alternate at the field rate. That is the movie's jitter, and it
     * survived giving the phase its correct per-field value because the phase
     * was never the missing part: the composition was.
     *
     * Declining high-resolution scanout for those fields hands them to the
     * renderer's own interlaced path instead: force_deinterlace becomes true
     * (gs_renderer.cpp:4246-4247), should_deinterlace with it
     * (gs_renderer.cpp:5045), and fastmad_deinterlace runs
     * shaders/weave.frag over the last four fields at double height.
     *
     * That filter is not a pure weave, and the difference decides how much of
     * the movie is actually reconstructed. Rows where (y & 1) == phase are the
     * current field taken as-is. Row 0 and the last row are the previous field
     * taken as-is. Every other row is
     * mix(previous field, 0.5 * (the current field's two neighbouring rows),
     * bob_factor) with bob_factor = smoothstep(0.04, 0.06, diff), where diff
     * is a luma difference between fields of the same parity. So still content
     * weaves and gives the whole picture back, and moving content is bobbed
     * from the current field alone: line doubled, at the field's vertical
     * detail rather than the frame's.
     *
     * Declining hires still follows. The alternative is field_aware_rendering
     * on buffers whose sub-samples are all equal, which reconstructs nothing
     * at all and only shifts alternate fields by a merged row. FastMAD weaves
     * the still parts and holds the moving parts steady, which is strictly
     * more of the decoded picture. The same info.phase drives it, so the
     * derivation above serves both paths unchanged.
     *
     * m_hires_from_copy is that test, carried from the phase derivation:
     * the copy wrote this buffer, or a DISPFB flip pointed at it. It is not a
     * setting and not a heuristic on content; it is which producer the field
     * that is being scanned out came from. */
    info.high_resolution_scanout = m_opts.render_scale >= 4 && m_hires_from_copy;
    /* Both consumers of the scanout image here (swapchain blit, screenshot
     * readback) want a transfer source. */
    info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dst_stage = VK_PIPELINE_STAGE_2_BLIT_BIT;
    info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;

    ParallelGS::ScanoutResult scanout = m_iface->vsync(info);

    /* Logged here rather than in present() so headless runs report it too,
     * and once per geometry change so a mode switch is visible without
     * spamming every field. */
    const double aspect = scanout_display_aspect(scanout);
    /* A non-null image always carries non-zero mode and internal dimensions,
     * so an all-zero m_aspect_log_geom already means "nothing logged yet". */
    /* The super-sampling rate and the renderer's own high-resolution answer
     * are part of the key so a live render-scale change re-logs the line
     * once. scanout.high_resolution_scanout is the result flag: the renderer
     * declines the request on its own for some scanout configurations, and
     * this is the only place that shows whether it actually engaged. */
    const uint32_t geom[7] = {
        scanout.internal_width, scanout.internal_height,
        scanout.mode_width, scanout.mode_height,
        scanout.interlaced ? 1u : 0u,
        m_opts.render_scale,
        scanout.high_resolution_scanout ? 1u : 0u,
    };
    /* The derived geometry is not the whole story: a display setup can change
     * DX, DW, MAGH, DBY or the window height without moving internal or mode,
     * and then this line would never fire again for it. So a change to any
     * display register other than the movie's per-field FBP select forces the
     * line and the CRTC lines below, on a budget so a game that rewrites
     * DISPLAY every field cannot turn it into a per-field log. */
    bool forced_relog = false;
    if (display_geom_changed && scanout.image) {
        m_display_geom_changed = false;
        if (m_display_relog_left) {
            --m_display_relog_left;
            forced_relog = true;
        }
    }
    if (scanout.image && (forced_relog || std::memcmp(geom, m_aspect_log_geom, sizeof(geom)) != 0)) {
        std::memcpy(m_aspect_log_geom, geom, sizeof(geom));
        m_crtc_log_left = 6;
        if (aspect > 0.0) {
            /* "deinterlaced", not "interlaced": ScanoutResult::interlaced is
             * assigned should_deinterlace, so it describes what happened to
             * this result, not what the source mode was. */
            logf("paraLLEl-GS: scanout internal %ux%u, mode %ux%u, deinterlaced=%s"
                 " -> display aspect %.4f; ss=%ux hires=%s",
                 scanout.internal_width, scanout.internal_height,
                 scanout.mode_width, scanout.mode_height,
                 scanout.interlaced ? "yes" : "no", aspect,
                 m_opts.render_scale, scanout.high_resolution_scanout ? "yes" : "no");
            /* The placement, hand checkable against the raster: DW+1 clocks
             * from DX and DH+1 lines from DY are what the game asked for, and
             * the lines-per-field figure beside them is that DH+1 reduced to
             * the unit the frame, the circuit rect and the crop lines below
             * are all in (crtc_lines_per_field). The circuit rect is where the
             * renderer put the window in the output image (single-sampled,
             * after the CRTC shift and the horizontal adaptation, reported by
             * carried patch parallel-gs-0002), and the output frame is
             * internal_width by internal_height. Two windows that share DX and
             * DY must share the circuit rect origin; the one that asked for
             * more than the frame holds is the one with a crop. One line per
             * enabled circuit per geometry change, so gameplay and the movie
             * sit side by side in a log.
             *
             * Enable test: scanout.circuit_enabled, which is the renderer's
             * own, a non-empty crtc_rect. That is not PMODE EN alone, since a
             * circuit PMODE enables but whose window is empty is not drawn.
             * The crop lines below key on PMODE EN2 instead, because they
             * describe the registers the CRTC reads, and the crtc field lines
             * further down key on PMODE EN1 and EN2 per circuit. */
            const auto& pv = m_iface->get_priv_register_state();
            for (unsigned c = 0; c < 2; ++c) {
                if (!scanout.circuit_enabled[c]) continue;
                const auto& dp = c == 0 ? pv.display1 : pv.display2;
                const int32_t over_x = int32_t(scanout.circuit_x[c]) + int32_t(scanout.circuit_width[c])
                                     - int32_t(scanout.internal_width);
                const int32_t over_y = int32_t(scanout.circuit_y[c]) + int32_t(scanout.circuit_height[c])
                                     - int32_t(scanout.internal_height);
                logf("paraLLEl-GS: placement circuit%u: asked %u clocks from %u, %u lines from %u"
                     " (%u lines per field, magh %u magv %u)"
                     " -> rect (%d,%d) %ux%u in a %ux%u frame;"
                     " crop right %d bottom %d",
                     c + 1u,
                     (unsigned)dp.DW + 1u, (unsigned)dp.DX,
                     (unsigned)dp.DH + 1u, (unsigned)dp.DY,
                     crtc_lines_per_field(pv, dp),
                     (unsigned)dp.MAGH + 1u, (unsigned)dp.MAGV + 1u,
                     scanout.circuit_x[c], scanout.circuit_y[c],
                     scanout.circuit_width[c], scanout.circuit_height[c],
                     scanout.internal_width, scanout.internal_height,
                     over_x > 0 ? over_x : 0, over_y > 0 ? over_y : 0);
            }
        } else {
            logf("paraLLEl-GS: scanout reported no mode (internal %ux%u, mode %ux%u);"
                 " presenting stretched to the window, which is not the game's aspect",
                 scanout.internal_width, scanout.internal_height,
                 scanout.mode_width, scanout.mode_height);
        }
    }

    /* A display window wider or taller than the mode area is a crop, and a
     * crop is a divergence: the renderer's non-overscan NTSC mode area is 640
     * pixels by 224 lines per field (gs_renderer.cpp:4299-4302) and the game
     * can ask for more, as the attract movie's DW+1 = 2880 and DH+1 = 480 do
     * against gameplay's 2560 and 448. The extra columns and lines are not
     * scanned out. Named here with the numbers rather than quietly dropped;
     * the placement rule above holds the one position on why nothing is
     * changed on its own.
     *
     * Enable test: PMODE EN2 picks the circuit, because these lines describe
     * the registers the CRTC reads, which is the same EN2 ? DISPLAY2 :
     * DISPLAY1 choice the rest of this file makes. Both counts are in the
     * frame's own units, lines per field and single-sampled columns, so they
     * compare directly against the mode area and against the placement line
     * above.
     *
     * The vertical count is derived from the registers, mirroring the
     * renderer (crtc_lines_per_field). The horizontal one is not derived: the
     * adaptation that produces mode_width folds in both circuits' MAGH and
     * both circuit image widths (gs_renderer.cpp:4701-4739), so the circuit
     * rect patch parallel-gs-0002 reports is the measurement rather than a
     * second, drifting derivation of it.
     *
     * Change-detected and budgeted. The detector alone is not a bound: a game
     * that alternates two window heights changes it every field. */
    if (scanout.image && scanout.mode_height && scanout.mode_width) {
        const auto& priv = m_iface->get_priv_register_state();
        const unsigned c = priv.pmode.EN2 ? 1u : 0u;
        const auto& dp = c ? priv.display2 : priv.display1;
        const uint32_t dh = uint32_t(dp.DH);
        const uint32_t lines = crtc_lines_per_field(priv, dp);
        if (lines > scanout.mode_height && dh != m_crop_logged_dh && m_crop_log_left) {
            m_crop_logged_dh = dh;
            --m_crop_log_left;
            logf("paraLLEl-GS: circuit%u display window is %u lines per field"
                 " (DH+1 %u, MAGV %u, interlaced=%s) but the mode area is %u;"
                 " the bottom %u lines per field are not scanned out",
                 c + 1u, lines, dh + 1u, (unsigned)dp.MAGV + 1u,
                 crtc_is_interlaced(priv) ? "yes" : "no",
                 scanout.mode_height, lines - scanout.mode_height);
        }
        const uint32_t dw = uint32_t(dp.DW);
        const uint32_t cols = scanout.circuit_enabled[c] ? scanout.circuit_width[c] : 0u;
        if (cols > scanout.mode_width && dw != m_crop_logged_dw && m_crop_log_left) {
            m_crop_logged_dw = dw;
            --m_crop_log_left;
            logf("paraLLEl-GS: circuit%u display window is %u columns"
                 " (DW+1 %u, MAGH %u, measured from the reported circuit rect)"
                 " but the mode area is %u; the right %u columns are not scanned out",
                 c + 1u, cols, dw + 1u, (unsigned)dp.MAGH + 1u,
                 scanout.mode_width, cols - scanout.mode_width);
        }
    }

    /* The per-field half of the same picture, and the registers the scanout
     * rectangle is derived from. Everything the scanout does differently
     * between the two fields is either info.phase or one of these registers,
     * so a handful of consecutive lines is enough to check that nothing but
     * the phase alternates and that the phase this side hands over is the one
     * the game's own per-field XYOFFSET was written against (see the comment
     * on info.phase above). copy= is what the snoop saw this field (-1 for no
     * display copy at all), flip= is whether DISPFB changed value during the
     * field, and counter= is what the field counter alone would have said;
     * copy and counter differing is the residual wobble's signature, and
     * flip=1 with copy=-1 is the movie's own path, where the per-field
     * difference is fbp rather than an XYOFFSET. The per-circuit lines carry
     * every DISPFB and DISPLAY field, because the horizontal placement (DX,
     * DW, MAGH against the mode width) decides the same question for the
     * other axis. */
    if (scanout.image && m_crtc_log_left) {
        --m_crtc_log_left;
        const auto& priv = m_iface->get_priv_register_state();
        logf("paraLLEl-GS: crtc field %llu phase=%u copy=%d flip=%u counter=%u"
             " int=%u ffmd=%u cmod=%u en=%u%u mmod=%u slbg=%u alp=%u"
             " mode %ux%u hires=%s src=%s",
             (unsigned long long)m_vsyncs, info.phase, copy_parity,
             (unsigned)dispfb_flip, counter_phase,
             (unsigned)priv.smode2.INT, (unsigned)priv.smode2.FFMD,
             (unsigned)priv.smode1.CMOD,
             (unsigned)priv.pmode.EN1, (unsigned)priv.pmode.EN2,
             (unsigned)priv.pmode.MMOD, (unsigned)priv.pmode.SLBG,
             (unsigned)priv.pmode.ALP,
             scanout.mode_width, scanout.mode_height,
             scanout.high_resolution_scanout ? "yes" : "no",
             m_hires_from_copy ? "copy" : "dispfb");
        for (unsigned c = 0; c < 2; ++c) {
            if (!(c == 0 ? priv.pmode.EN1 : priv.pmode.EN2)) continue;
            const auto& fb = c == 0 ? priv.dispfb1 : priv.dispfb2;
            const auto& dp = c == 0 ? priv.display1 : priv.display2;
            logf("paraLLEl-GS: crtc field %llu circuit%u"
                 " dispfb fbp=%u fbw=%u psm=%u dbx=%u dby=%u"
                 " display dx=%u dy=%u dw=%u dh=%u magh=%u magv=%u",
                 (unsigned long long)m_vsyncs, c + 1u,
                 (unsigned)fb.FBP, (unsigned)fb.FBW, (unsigned)fb.PSM,
                 (unsigned)fb.DBX, (unsigned)fb.DBY,
                 (unsigned)dp.DX, (unsigned)dp.DY, (unsigned)dp.DW,
                 (unsigned)dp.DH, (unsigned)dp.MAGH, (unsigned)dp.MAGV);
        }
    }

#ifdef ICORECOMP_PGS_SDL
    /* The attract movie's two buffers are the two halves of one picture, not
     * two pictures.
     *
     * func_0023EE28 (../ico asm/nonmatchings/ito/mpeg/mv_sub/func_0023EE28.s)
     * runs a three state machine over one decoded picture: at .L0023EEBC,
     * state 2 and CSR.FIELD 0, it kicks the GIF chain at self+0x20240, sets
     * DISPFB2 FBP to self+0x28 (which func_0023E578 stores as 0) and moves to
     * state 1; at .L0023EF2C, state 1 and CSR.FIELD 1, it kicks the chain at
     * self+0x40, sets FBP to self+0x2C (96 for this movie) and moves to state
     * 0, where it waits for the decoder. Two chains, two fields, one picture:
     * 29.97 pictures per second, each uploaded as two halves.
     *
     * Which half is which follows from the CRTC rather than from the chains.
     * Under FFMD = 1 each buffer is read as a packed field and shown on
     * alternate fields of the raster, so buffer A's row n lands on display
     * line 2n and buffer B's row n on 2n+1. Both buffers are read with the
     * same DBY 12 and the same 240 lines (measured, every crtc circuit2 line
     * of the movie). A top half and bottom half split would therefore
     * interleave two vertically squashed copies of the picture on the CRT,
     * which is not an image; the even lines in A and the odd lines in B is
     * the only assignment that is one. So it is a true field pair and a weave
     * is the composition, which is what declining high-resolution scanout
     * already asks the renderer for.
     *
     * What was left is the pairing. fastmad_deinterlace weaves the current
     * scanout with the previous one (gs_renderer.cpp:5047-5062), and the
     * previous one is the other half of the same picture only on the field
     * that completed the pair. On the other field it is the second half of
     * the picture before, so every second output frame wove two different
     * pictures together: a comb at 29.97 Hz on anything that moved, which is
     * the interlaced look that remained once the jitter was gone.
     *
     * The completing field is the CSR.FIELD 1 one, the second arm above, and
     * that is the field this shim gives phase 1 (the phase is CSR.FIELD during
     * the field the DISPFB write happened in; see the derivation on
     * info.phase). So a new composition is presented on phase 1 and the phase
     * 0 field repeats it. The result is the movie's own 29.97 picture rate
     * with both halves of each picture and no cross picture weave, which is
     * what the CRT integrates over the two fields.
     *
     * Only the deinterlaced movie path is affected: a field whose picture came
     * from the display copy, or any field the renderer scanned out at high
     * resolution, presents its own composition exactly as before. */
    const bool movie_field_pair = scanout.image && scanout.interlaced &&
                                  !scanout.high_resolution_scanout && !m_hires_from_copy;
    const ParallelGS::ScanoutResult* to_present = &scanout;
    double present_aspect = aspect;
    /* Windowed only. Nothing presents in a headless run, so holding a scanout
     * image there would retain a Vulkan image no one reads. */
    if (m_wsi_active) {
        if (movie_field_pair) {
            /* Two conditions, because the phase alone is not enough. A field
             * that produced nothing inherits the previous field's phase
             * (phase_held), so on a held phase 1 the phase test alone would
             * store that field's scanout over the good pair, which is a weave
             * of one half against itself: once for every field the decoder was
             * late for, and again at the end of the movie before the first
             * display copy comes back. A held field has to repeat the last
             * complete pair instead.
             *
             * The test is !phase_held rather than phase_from_flip, because
             * both of the other sources are per-field answers about this
             * field. phase_from_flip is the movie's own case. So is the
             * pre-calibration case, where m_copy_ofy_base is still -1, none of
             * the three flags is set and the phase is the field counter: a
             * movie that plays before the game has ever drawn the display copy
             * would otherwise never refresh the pair and would freeze on its
             * first field. */
            if ((!phase_held && phase == 1u) || !m_held_scanout.image) {
                m_held_scanout = scanout;
                m_held_aspect = aspect;
            } else {
                to_present = &m_held_scanout;
                present_aspect = m_held_aspect;
                ++m_pair_repeats;
                const uint64_t n = m_pair_repeats;
                if ((n & (n - 1)) == 0) {
                    logf("paraLLEl-GS: field %llu repeats the composed field pair"
                         " (phase 0 does not complete a picture); repeats=%llu",
                         (unsigned long long)m_vsyncs, (unsigned long long)n);
                }
            }
        } else if (scanout.image && m_held_scanout.image) {
            /* Dropped only for a real field from the other producer. A field
             * the renderer produced no image for keeps the pair, because
             * clearing it would leave the next movie field with nothing to
             * repeat and it would compose across two pictures instead.
             *
             * The cost of holding is that the held result carries the geometry
             * and the aspect of the field it was composed in, so a DISPLAY2
             * change in the middle of the movie shows one frame at the
             * previous geometry before the next complete pair replaces it. */
            m_held_scanout = {};
            m_held_aspect = 0.0;
        }
        present(*to_present, present_aspect);
    }
#endif
    if (m_screenshot_path && scanout.image) {
        /* Raw scanout pixels, deliberately NOT aspect-corrected: this file is
         * the regression baseline for rendering, so it has to stay a function
         * of the GS output alone and byte-comparable against a gs-replay dump.
         * It is therefore not the shape the game has on screen (512x448 here
         * against a 4:3 display); the display aspect for the same frame is in
         * the "display aspect" log line above.
         *
         * The overlay (draw_overlay) never lands here: it is drawn straight
         * to the swapchain backbuffer in present_frame/present_ui_windowed,
         * not to scanout.image, so a screenshot never contains it. */
        if (!rt_gs_write_scanout_ppm(*m_device, *scanout.image, m_screenshot_path)) {
            logf("paraLLEl-GS: screenshot write to %s failed", m_screenshot_path);
            m_screenshot_path = nullptr; /* do not spam every field */
        }
    }

    uint32_t flags = presented ? RT_PGS_VSYNC_PRESENTED : 0u;
    if (m_window_closed) flags |= RT_PGS_VSYNC_WINDOW_CLOSED;
    return flags;
}
