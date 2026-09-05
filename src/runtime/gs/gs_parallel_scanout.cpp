/* gs/gs_parallel_scanout.cpp: the vsync path of the paraLLEl-GS shim.
 *
 * Part of libicorecomp-parallel-gs; see gs_parallel_lib.cpp for the library
 * overview and gs_parallel_impl.h for the RtPgs type. Holds the display
 * aspect derivation, the display copy phase snoop that feeds VSyncInfo::phase,
 * RtPgs::vsync itself with its scanout geometry and CRTC logging, and the
 * screenshot readback of the scanout image.
 *
 * vsync does not present. It latches the finished field into the
 * latest-scanout slot and returns; RtPgs::present_pump
 * (gs_parallel_present.cpp) is what shows it, at whatever rate the host
 * asked for. See rt_pgs_present_pump in gs_parallel_api.h.
 */
#include "gs_parallel_impl.h"

#include "gs_interface.hpp"
#include "gs_readback.h"

#include <chrono>
#include <cstring>

namespace {

/* Present-path timings, reported to the host through
 * rt_pgs_present_timings. The host's profiler bills the whole vsync hook to
 * one "present" bucket and cannot see inside this library, so the split
 * between renderer flush, scanout and swapchain present is measured here.
 * The present half is measured in present_pump, which is where the present
 * happens now. */
using PgsProfClock = std::chrono::steady_clock;

uint64_t elapsed_ns(PgsProfClock::time_point since) {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        PgsProfClock::now() - since).count();
}

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
 * The target is SCES_507.60, which is PAL. What a PAL run has settled, from
 * the 2026-09-04 boot: SMODE1 CMOD 3, SMODE2 INT 0 with DISPLAY2 DH+1 256 and
 * MAGV 0, and the renderer reported a 516x512 scanout frame against that
 * 512x256 circuit. The attract movie's two field buffers are 720x288 and the
 * ELF's setDispEnv programs that size (0x0025C0B8, see below). What a PAL run
 * has NOT settled, because no PAL log in this tree carries them: the gameplay
 * DISPLAY2 DW, DX, DY and MAGH. Whoever next runs a PAL boot should read them
 * off the crtc field lines and put them here, in front of the NTSC set.
 *
 * The NTSC set below is the earlier measurement, kept because the derivation
 * was written against it and because every step of the derivation is a
 * statement about the renderer rather than about one disc. Measured for ICO
 * (US), from the GS_DISPLAY2/GS_SMODE2 values that game programs: NTSC,
 * INT=1, FFMD=1, DISPLAY2 DW+1=2560 with MAGH+1=5 (512
 * pixels) and DH+1=448. FFMD with INT forces a deinterlaced scanout, so
 * gs_renderer.cpp does not double mode_height and the mode lands at 512x224
 * after adapt_to_internal_horizontal_resolution rescales mode_width by
 * clock_divider/(MAGH+1). internal then equals mode, the fraction terms
 * cancel, and the target is a plain 4:3. Presenting at 512:224 (2.29:1),
 * which is what this file used to do, stretched the picture 1.71x
 * horizontally and letterboxed the result inside the window.
 *
 * None of the arithmetic below is NTSC specific: mode_lines() takes the line
 * count from CMOD at every field and the fraction form cancels whatever the
 * mode area is. The NTSC-first prose is history, not the shipped case.
 *
 * The image's own size is no better a source: force_deinterlace runs
 * fastmad_deinterlace, so result.image comes back 512x448 here (512x224,
 * the raw field, for a movie field under display.deinterlace bob) while
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
 * 4. The mode fraction below has no such dependency and is right whenever
 * the frame is the fixed mode area, which is display.raster crt. Window mode
 * grows the frame to the display window and derives that window's aspect
 * from DW and DH in RtPgs::vsync, on the circuit PMODE enables, and only once
 * a circuit is enabled and the frame equals its rect, which is the answer to
 * "which circuit, and what if none is enabled". */

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
     *    PAL. Anyone lifting that fatal has to derive the aspect here first.
     *
     * PAL is not a third family either. Its non-overscan mode area is
     * 640x256 per field against NTSC's 640x224, and both are the active
     * area of an analog set, which is displayed 4:3. The fraction form
     * below is the derivation for both: the constant is the mode area's
     * own display aspect, not a pixel count.
     *
     * A frame grown to hold an oversized window (grow_mode_area_to_circuits)
     * is not a third family: on the merge path the internal size still
     * equals the mode size, the two fractions below cancel, this returns
     * kModeDisplayAspect, and the window is presented at 4:3. */
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
    const bool latched = m_transfer_since_vsync;
    m_transfer_since_vsync = false;

    m_timing_fields.fetch_add(1, std::memory_order_relaxed);
    const auto t_flush = PgsProfClock::now();
    m_iface->flush();
    m_flush_ns.fetch_add(elapsed_ns(t_flush), std::memory_order_relaxed);

    ParallelGS::VSyncInfo info = {};
    /* The phase is the vertical position the field in VRAM was drawn for,
     * not a property of the clock, so it is taken from the copy itself when
     * the copy can be seen, and from the field counter otherwise.
     *
     * What the game does. ICO renders the frame at 512x448 into VRAM 0x80000
     * and once per field draws two sprites into the 512x224 PSMCT32 buffer at
     * VRAM 0 that the CRTC reads: a black sprite over the whole buffer, then
     * one textured sprite mapping the 448 rows onto the 224 rows.
     * Inferred, from the shape of the traffic and not read off the ELF:
     * XYOFFSET_1 OFY = (0x800 - height/4) * 16, sprite y in [-0.25, 223.75),
     * V in [0.5, 448.5).
     *
     * Measured, in the retail ELF (read through the disc's own objdump
     * listing, which carries the vendor and game symbol names): the SDK
     * helper sceGsSetHalfOffset at 0x00261048 rewrites OFY as either t or
     * t + 8, half a pixel in the 4-bit fraction, according to its fourth
     * argument (the +8 is taken when the argument is nonzero, at
     * 0x002610AC-0x002610B4, and the result is stored to the draw
     * environment's XYOFFSET_1 slot at 0x002610CC).
     *
     * Which field gets the +8. Both callers in the frame path,
     * gsb_UpdateGSSystem (0x00114370) and gsb_ResetGSSystem (0x001144C8),
     * read GS_CSR at 0x12001000 on entry, keep bit 13 (FIELD) in a global,
     * and then pass "that global == 0" as the fourth argument
     * (gsb_UpdateGSSystem: the CSR read at 0x0011438C and the compare at
     * 0x0011443C; gsb_ResetGSSystem: 0x0011453C and 0x0011459C). So on this
     * build the argument is the complement of CSR.FIELD.
     *
     * That is the opposite of what this comment used to claim, which came
     * from the US decomp's routing of the same value through pad.c. The
     * fallback branch below still uses the parity that the 4x measurement
     * picked (see "The field counter can only guess at that"), because that
     * measurement is of the picture and this reading is of one build's call
     * sites; the two have not been reconciled. Nothing in the shipped path
     * depends on it while the copy snoop is calibrated, which it is from the
     * first half pixel field onwards.
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
     * (Renderer line numbers in this file are the pinned submodule commit
     * with third_party/patches/parallel-gs-0001-full-pixel-raster-snap.patch
     * applied and nothing else: 0001 inserts 17 lines at gs_renderer.cpp:2609,
     * so a citation below that point is the pristine number plus 17. A
     * configured tree also carries 0002 through 0005 and reads higher below
     * each of their insertion points (gs_renderer.cpp: 0004 at 4590 and 4648,
     * 0004 again at 4739, 0002 at 4790, 0005 near 5064, 0003 near 5125;
     * gs_interface.hpp: 0004 at 184, 0003 at 197; gs_renderer.hpp: 0002 at
     * 50). The patch files are the translation table.)
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
     * VRAM, and the movie's own vblank handler does the switching. Measured
     * in the retail ELF, through the disc's objdump listing: vblankHandler
     * (0x0025C830) reads GS_CSR at 0x12001000 and keeps bit 13 (FIELD), then
     * calls dispSwitch (0x0025C7F8) with a selector taken from that bit.
     * dispSwitch picks one of two stored words, self+0x28 for field 0 and
     * self+0x2C for field 1, and rewrites the low 9 bits of the DISPFB2 word
     * at self+0x10 with it, which is the FBP field, then hands the display
     * environment to the SDK's sceGsPutDispEnv (0x0025F928). DBY and
     * DISPLAY2 stay put, so the whole per-field difference is which buffer
     * the CRTC reads, and the field that buffer belongs to is the CSR.FIELD
     * the handler read.
     *
     * The two values are set once, by setDispEnv (0x0025C0B8): it stores 0
     * to self+0x28 at 0x0025C110 and 108 to self+0x2C at 0x0025C118, beside
     * a picture size of 720 by 288 at self+0x30 and self+0x34. FBP counts
     * 8192 byte pages of 64 by 32 pixels, and
     * ceil(720 / 64) * ceil(288 / 32) = 12 * 9 = 108, so the second buffer
     * starts exactly where a 720 by 288 PSMCT32 buffer at page 0 ends. All
     * four are compile-time constants on this build; there is no NTSC arm in
     * that function. So the
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
            warnf("paraLLEl-GS: no fractional XYOFFSET OFY in the first %llu fields;"
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
     * The numbers below are the NTSC measurement, and the target is PAL. The
     * rule itself is mode independent, the mode area comes from the renderer
     * per field, and mode_lines() takes the line count from CMOD; what has
     * not been read off a PAL log is the PAL disc's own DISPLAY2 window. See
     * the note at the head of this file.
     *
     * The crt frame (RT_PGS_RASTER_CRT, display.raster crt) is the area
     * the renderer models as visible for the video mode: for non-overscan
     * NTSC that is 640 raster pixels by 224 lines per field starting at
     * raster (159, 25), which in DISPLAY units is 2560 clocks from clock 636
     * and 448 lines from line 50 (clock divider 4, gs_registers.hpp:723).
     * Measured, ICO's gameplay window is exactly that: DW+1 2560, DH+1 448,
     * DX 636, DY 50, MAGH 4. The attract movie programs DW+1 2880, DH+1 480,
     * MAGH 3 from the same DX 636 and DY 50, which is 720 pixels by 240
     * lines per field: it shares the top left corner and runs 320 clocks
     * (80 pixels) further right and 32 lines (16 lines per field) further
     * down. Both windows come out of the SDK's sceGsSetDefDispEnv
     * (0x0025F6B8 in the retail ELF). The two call sites outside the SDK
     * itself are gsb_SetFrame at 0x00112504 for gameplay and setDispEnv at
     * 0x0025C12C for the movie, and both pass 0 for its dx and dy arguments.
     *
     * In crt the movie is cropped against that frame rather than fitted to
     * it, so it keeps gameplay's origin and scale: 640x224 is what the
     * renderer models the mode as showing, and how much of the 80 columns a
     * real set would have shown behind its own overscan is a property of the
     * set, not of this file. The left and top of the picture are not the
     * frame's doing anyway: the movie's DISPFB2 carries DBX 36, DBY 12,
     * measured on every crtc circuit2 line of the movie in this port's own
     * log. In the retail ELF the pair reaches DISPFB2 through setDispEnv
     * (0x0025C0B8), which takes them as its fourth and fifth arguments and
     * splices them into the DBX and DBY fields at 0x0025C140-0x0025C17C;
     * the constants themselves are handed down from mv_main.c's movie
     * start and have not been traced to their literal. So on hardware the
     * CRTC never reads the picture's left 36 columns, nor the buffer rows
     * above DBY. How many rows that is depends on the FFMD row rule, which
     * is contested (see gs/render/gs_crtc.cpp): 12 under the stride 1
     * reading paraLLEl-GS uses, which is the reading this backend inherits,
     * and 24 under the stride 2 reading. Either way it is the game's own
     * overscan allowance. What
     * the 640x224 frame takes off on top of that is the right 80 columns and
     * the bottom 16 lines per field.
     *
     * RT_PGS_RASTER_WINDOW (display.raster window), the default, does two
     * things. It grows
     * the frame until it contains every enabled CRTC window instead of
     * cropping on the right and bottom, and it reads each circuit from
     * DBX = DBY = 0 instead of from the buffer offset the game programmed.
     * For the movie that means the whole 720 by 480 buffer the display
     * window points into is presented from its own origin, at 4:3 (see
     * scanout_display_aspect), which is what PCSX2 shows by default.
     *
     * With the read offset ignored, the picture is framed by its own black
     * borders rather than by the frame or by DBX/DBY. Measured off a decoded
     * I frame, the picture carries 40 blank columns on the left and 38 on
     * the right, 8 blank rows on top and 17 on the bottom, so the content
     * sits centred within a pixel horizontally and about four rows above
     * centre vertically. Read at DBX 36, DBY 12 instead, the same content is
     * flush against the top and the right of the frame, which is what the
     * game's own overscan allowance is for on a set that cuts those edges
     * off and what window mode would otherwise show as a picture pushed into
     * the top right corner.
     *
     * DBX/DBY is the one game-supplied value this mode overrides, and it is
     * a host presentation choice, not an accuracy fix. It is the default
     * because the user chose it after seeing both modes on the running movie
     * (2026-09-03); crt is one menu step away, and the raster log line names
     * the override (rt_pgs_raster_log_text) so a log always says which frame
     * ran. Gameplay is unaffected either way: its window fits the frame and
     * its DISPFB carries DBX 0, DBY 0.
     *
     * Fitting the movie to the frame is not on offer in either mode: it
     * moves the movie's origin away from gameplay's and rescales a picture
     * the game sized itself. The placement lines below report where each
     * window actually landed, so all of this stays checkable from a log. */
    info.grow_mode_area_to_circuits = m_opts.raster == RT_PGS_RASTER_WINDOW;
    info.ignore_display_buffer_offset = m_opts.raster == RT_PGS_RASTER_WINDOW;
    /* The override, named at the moment it happens and not only at startup:
     * once per distinct non-zero (DBX, DBY) pair on the circuit the CRTC
     * reads, so a log shows which game values window mode discarded. */
    if (info.ignore_display_buffer_offset) {
        const auto& pv = m_iface->get_priv_register_state();
        const auto& fb = pv.pmode.EN2 ? pv.dispfb2 : pv.dispfb1;
        const uint32_t pair = (uint32_t(fb.DBX) << 16) | uint32_t(fb.DBY);
        if ((fb.DBX || fb.DBY) && pair != m_dbxy_ignored_logged) {
            m_dbxy_ignored_logged = pair;
            /* Warn: this is the one game-supplied value the window raster
             * overrides, so it is a divergence and the accuracy rule wants
             * it loud rather than filed with the startup facts. Bounded to
             * one line per distinct pair by the latch above. */
            warnf("paraLLEl-GS: raster window: circuit%u DISPFB DBX %u DBY %u ignored,"
                  " the buffer is read from its origin (display.raster)",
                  pv.pmode.EN2 ? 2u : 1u, (unsigned)fb.DBX, (unsigned)fb.DBY);
        }
    }
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
     * That filter is not a pure weave. weave.frag (FastMAD) keeps the rows
     * where (y & 1) == phase from the current field, keeps row 0 and the
     * last row from the previous field, and makes every other row
     * mix(previous field, 0.5 * (the current field's two neighbouring rows),
     * smoothstep(0.04, 0.06, diff)), where diff is a luma difference against
     * the field two back. Whether that motion test is the right one for the
     * movie is a property of the source, not of how the game splits it.
     *
     * The split. The decoder uploads the decoded frame to VRAM and then
     * draws a point sampled 2:1 vertical sprite into each of the two field
     * buffers, dropping alternate rows, so the first buffer holds that
     * frame's even rows and the second its odd rows. Measured, in the
     * retail ELF: dispSetTags (0x0025C3C8) builds the two GIF chains, each
     * with its own UV pair and XYZ pair (0x0025C76C-0x0025C7C4), and the
     * two buffer pages are the 0 and 108 setDispEnv stores, derived above.
     * Inferred, not measured: that the two chains' V origins are exactly
     * 0.5 and 1.5, that is, that the offset between them is one source row
     * and not something else. The UV values are computed from dispSetTags's
     * arguments rather than being literals in the function, and that chain
     * has not been traced. The two newest fields are the two row sets of
     * one decoded picture.
     *
     * What that does not settle is whether they are one moment, and measured
     * they are not: a decoded I frame of this movie shows comb teeth on
     * moving figures inside the single picture, so its even and odd rows
     * were captured about 1/60 s apart. The MPEG source is interlaced video.
     * The field pair is two moments, not one picture, and a pure weave of
     * the pair reproduces the source's own comb on anything that moves. An
     * earlier revision of this comment claimed the opposite, that a pure
     * weave was exact for this content; the comb inside one picture is the
     * measurement that says it is not.
     *
     * So the composition is a host choice with no single right answer. It is
     * compiled in as bob (the display.deinterlace key was retired
     * 2026-09-04) and is still read fresh every field just below, because
     * the replay tool and a CI run can set it through the ABI. Bob is the
     * default: the user compared all three on the running movie (2026-09-03)
     * and chose it. Adaptive asks the renderer for nothing special: FastMAD weaves
     * the still parts and bobs the moving parts, and for a source whose
     * fields really are 1/60 s apart in both directions there is nothing
     * wrong with the field it compares against. Motion then runs at the
     * field rate, 59.94 Hz, which is what the source carries.
     *
     * Declining hires still follows. The alternative is field_aware_rendering
     * on buffers whose sub-samples are all equal, which reconstructs nothing
     * at all and only shifts alternate fields by a merged row. The same
     * info.phase drives the deinterlace path, so the derivation above serves
     * both paths unchanged.
     *
     * m_hires_from_copy is that test, carried from the phase derivation:
     * the copy wrote this buffer, or a DISPFB flip pointed at it. It is not a
     * setting and not a heuristic on content; it is which producer the field
     * that is being scanned out came from. */
    info.high_resolution_scanout = m_opts.render_scale >= 4 && m_hires_from_copy;
    /* The deinterlace mode, read fresh every field so a change made through
     * the ABI applies at the next one. There is no menu control and no
     * settings key: display.deinterlace was retired on 2026-09-04 and the
     * shipped value is the compiled-in bob. Both flags are ignored unless
     * the renderer
     * deinterlaces at all, so a high-resolution scanout is unaffected by all
     * three modes.
     *
     *   adaptive  neither flag: FastMAD as described above.
     *   bob       skip_deinterlace, so the renderer returns the field itself
     *             with its phase and composes nothing. present_frame stretches
     *             it to the frame height and offsets it by the half line the
     *             field sits at, which is what a CRT does with it. Movie
     *             fields only, like weave below: a field the display copy
     *             produced is one of two resamples of a single rendered frame
     *             (gameplay at render scale 1), and FastMAD gives the whole
     *             448-row frame back for those, so the setting never halves
     *             gameplay's vertical detail. The first field after a switch
     *             into weave weaves with itself for one frame, because the
     *             renderer's field ring was reset while skipping; cosmetic.
     *   weave     weave_only, and only for a field the movie produced (the
     *             same producer test as the hires decision above: a DISPFB
     *             flip with no display copy behind it). The renderer binds
     *             the two newest fields to all four FastMAD inputs, so diff
     *             is zero, the motion term drops out and every row is the
     *             weave of the current field with the previous one. Kept
     *             because it is the only mode that shows all 480 source rows
     *             of a still picture at once; on moving figures it shows the
     *             source's comb. */
    const uint32_t deinterlace_mode = m_opts.deinterlace;
    info.skip_deinterlace = deinterlace_mode == RT_PGS_DEINTERLACE_BOB && !m_hires_from_copy;
    info.weave_only = deinterlace_mode == RT_PGS_DEINTERLACE_WEAVE && !m_hires_from_copy;
    /* Both consumers of the scanout image here (swapchain blit, screenshot
     * readback) want a transfer source. */
    info.dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    info.dst_stage = VK_PIPELINE_STAGE_2_BLIT_BIT;
    info.dst_access = VK_ACCESS_2_TRANSFER_READ_BIT;

    const auto t_scanout = PgsProfClock::now();
    ParallelGS::ScanoutResult scanout = m_iface->vsync(info);
    m_scanout_ns.fetch_add(elapsed_ns(t_scanout), std::memory_order_relaxed);

    /* No image means no picture this field: every log line below is gated on
     * scanout.image, and the field goes on to present a cleared backbuffer
     * and an empty present rectangle, so without this a black window says
     * nothing at all in the log. One warn when the pictures stop and one
     * info with the count when they come back, never a line a field. */
    if (!scanout.image) {
        ++m_no_image_fields;
        ++m_no_image_total;
        if (!m_no_image_logged) {
            m_no_image_logged = true;
            warnf("paraLLEl-GS: field %llu: the renderer produced no scanout image, so there is"
                  " no picture for it; a windowed run presents a cleared backbuffer and an"
                  " empty present rectangle. Further fields without an image are counted,"
                  " not logged.",
                  (unsigned long long)m_vsyncs);
        }
    } else if (m_no_image_fields) {
        logf("paraLLEl-GS: field %llu carries a picture again after %llu field(s) with no"
             " scanout image",
             (unsigned long long)m_vsyncs, (unsigned long long)m_no_image_fields);
        m_no_image_fields = 0;
    }
    if (m_vsyncs <= kBootTraceFields) boot_trace_registers(bool(scanout.image));

    /* Logged here rather than in present() so headless runs report it too,
     * and once per geometry change so a mode switch is visible without
     * spamming every field. */
    double aspect = scanout_display_aspect(scanout);
    /* Window mode: the frame is the display window itself, so the mode
     * fraction above, which describes the fixed 4:3 area, no longer
     * describes it. The window's raster aspect follows from the registers
     * the game programmed: DW+1 clocks against the 2560 clocks and DH+1
     * frame lines against the frame lines of the 4:3 mode area the crt
     * frame stands for: 448 on NTSC and 512 on PAL, which are the two
     * active areas the renderer models (224 and 256 lines per field,
     * doubled). Gameplay (2560 x 448 on NTSC) gives 4:3 exactly; the NTSC
     * movie (2880 x 480) gives 1.4, which keeps its pixels the same size as
     * gameplay's: its 642 content columns span 2568 clocks, the width of the
     * gameplay picture, so the title screen and the movie share one scale.
     * Presenting the grown frame at 4:3 instead (what PCSX2 does) squeezes
     * the movie 4.8 percent horizontally.
     *
     * The clock reference, 2560, is the same in both modes: 640 pixels at
     * the standard clock divider is 2560 DW units on NTSC and on PAL, which
     * is why only the line count changes. On PAL the movie's own display
     * env is 720 by 288 lines per field (its mv_disp setDispEnv passes
     * those two literals with no branch, read off the objdump listing the
     * PAL disc ships, so behaviour rather than an address), so 2880 x 576
     * against 2560 x 512 comes out at 4:3 exactly: a PAL 720x576 picture
     * fills its 4:3 area, where the NTSC 720x480 one does not.
     *
     * Applied only when the frame is the circuit's own window (mode equals
     * the circuit rect). A CMOD that is neither NTSC nor PAL cannot reach
     * here (rt_gs_program_crt is fatal on one), but the arm is kept and
     * says so once rather than presenting a number it did not derive. */
    if (m_opts.raster == RT_PGS_RASTER_WINDOW && scanout.image) {
        const auto& pv = m_iface->get_priv_register_state();
        const unsigned c = pv.pmode.EN2 ? 1u : 0u;
        const auto& dp = c ? pv.display2 : pv.display1;
        if (scanout.circuit_enabled[c] &&
            scanout.circuit_width[c] == scanout.internal_width &&
            scanout.circuit_height[c] == scanout.internal_height) {
            if (pv.smode1.CMOD == 2u || pv.smode1.CMOD == 3u) {
                const double clocks = double(dp.DW) + 1.0;
                const double lines = (double(dp.DH) + 1.0) * (pv.smode2.INT ? 1.0 : 2.0);
                const double mode_lines = pv.smode1.CMOD == 3u ? 512.0 : 448.0;
                aspect = kModeDisplayAspect * (clocks / 2560.0) * (mode_lines / lines);
            } else if (!m_window_aspect_cmod_logged) {
                m_window_aspect_cmod_logged = true;
                warnf("paraLLEl-GS: raster window: display aspect for CMOD %u is not derived here;"
                      " presenting the window at %.4f", (unsigned)pv.smode1.CMOD, aspect);
            }
        }
    }
    /* display.widescreen's presentation half. The 3D picture is drawn into
     * the same 512x448 buffer whatever the mode: the game's own projection
     * was widened at the matrix composer (guest/widescreen.cpp), so the
     * frustum is wider but the raster is the size it always was. Presenting
     * that raster at the target aspect is what makes the wider frustum
     * visible; display.fit then letterboxes whatever the window has left
     * over, exactly as it does at 4:3.
     *
     * It is applied to every scanout, including one that carries no 3D at
     * all: the attract movie, the game's own menus and the fades reach this
     * point as an image and a set of CRTC registers with nothing in them
     * that says whether the picture was projected. Their geometry was held
     * at 4:3 by the transform in hw/gif.cpp instead, which scales 2D X
     * coordinates back by the same factor, so full-screen 2D content still
     * fills the picture and only content the transform classified as
     * inside-frame sits at retail proportions over it. Which of the two
     * every 2D pass in this game is is what the five dumps named in
     * docs/SETTINGS.md section 6 decide; until they are read, this is
     * stated rather than assumed. */
    if (m_widescreen_aspect > 0.0 && scanout.image) {
        if (m_widescreen_aspect != m_widescreen_aspect_logged) {
            m_widescreen_aspect_logged = m_widescreen_aspect;
            logf("paraLLEl-GS: widescreen: presenting at %.4f instead of the derived %.4f"
                 " (display.widescreen)", m_widescreen_aspect, aspect);
        }
        aspect = m_widescreen_aspect;
    }
    /* A non-null image always carries non-zero mode and internal dimensions,
     * so an all-zero m_aspect_log_geom already means "nothing logged yet". */
    /* The super-sampling rate and the renderer's own high-resolution answer
     * are part of the key so a live render-scale change re-logs the line
     * once. scanout.high_resolution_scanout is the result flag: the renderer
     * declines the request on its own for some scanout configurations, and
     * this is the only place that shows whether it actually engaged. */
    const uint32_t geom[8] = {
        scanout.internal_width, scanout.internal_height,
        scanout.mode_width, scanout.mode_height,
        scanout.interlaced ? 1u : 0u,
        m_opts.render_scale,
        scanout.high_resolution_scanout ? 1u : 0u,
        deinterlace_mode,
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
                 " -> display aspect %.4f; ss=%ux hires=%s deint=%s",
                 scanout.internal_width, scanout.internal_height,
                 scanout.mode_width, scanout.mode_height,
                 scanout.interlaced ? "yes" : "no", aspect,
                 m_opts.render_scale, scanout.high_resolution_scanout ? "yes" : "no",
                 rt_pgs_deinterlace_name(deinterlace_mode));
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
            warnf("paraLLEl-GS: scanout reported no mode (internal %ux%u, mode %ux%u);"
                  " presenting stretched to the window, which is not the game's aspect",
                  scanout.internal_width, scanout.internal_height,
                  scanout.mode_width, scanout.mode_height);
        }
    }

    /* A display window wider or taller than the mode area is a crop, and a
     * crop is a divergence: the renderer's non-overscan mode area is 640
     * pixels by 224 lines per field on NTSC and 640 by 256 on PAL
     * (gs_renderer.cpp:4299-4302) and the game can ask for more, as the
     * attract movie's DW+1 = 2880 and DH+1 = 480 do against gameplay's 2560
     * and 448. The test below is against the mode area the renderer
     * reported, so it holds in either mode without naming one. The extra columns and lines are not
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
            warnf("paraLLEl-GS: circuit%u display window is %u lines per field"
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
            warnf("paraLLEl-GS: circuit%u display window is %u columns"
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
    /* Weave mode only (display.deinterlace weave). The attract movie's two
     * buffers are the two row sets of one decoded picture, so weaving them
     * is a composition of a whole picture, and a whole picture is worth
     * holding for the field that does not complete one. Adaptive and bob
     * compose or present per field and hold nothing.
     *
     * The measurement above stands over this whole comment: the picture the
     * pair composes is itself two moments 1/60 s apart, so what is held is a
     * complete pair, not a progressive frame.
     *
     * Measured, in the retail ELF: vblankHandler (0x0025C830) runs a three
     * state machine over one decoded picture. In state 2 with CSR.FIELD 0 it
     * kicks the GIF chain at self+0x26740, calls dispSwitch (0x0025C7F8) with
     * selector 0, which puts self+0x28 (0) in DISPFB2's FBP, and moves to
     * state 1. In state 1 with CSR.FIELD 1 it kicks the chain at self+0x40,
     * calls dispSwitch with selector 1, which puts self+0x2C (108 for this
     * movie, derived from setDispEnv above) in FBP, and moves to state 0,
     * where it waits for the decoder. Two chains, two fields, one picture,
     * each uploaded as two halves: one picture per two fields, so half the
     * field rate of whatever video mode is running.
     *
     * Which half is which follows from the CRTC rather than from the chains.
     * Under FFMD = 1 each buffer is read as a packed field and shown on
     * alternate fields of the raster, so buffer A's row n lands on display
     * line 2n and buffer B's row n on 2n+1. Both buffers are read with the
     * same DBY 12 and the same line count (measured, every crtc circuit2
     * line of the movie in this port's own log; the run that produced the
     * 240 lines predates the PAL retarget, and the PAL ELF's setDispEnv
     * programs 288 per field). A top half and bottom half split would
     * therefore
     * interleave two vertically squashed copies of the picture on the CRT,
     * which is not an image; the even lines in A and the odd lines in B is
     * the only assignment that is one. So it is a true field pair, and in
     * weave mode a weave of the pair is the composition, which is what
     * declining high-resolution scanout already asks the renderer for.
     *
     * What is left in that mode is the pairing. fastmad_deinterlace weaves the current
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
     * with both row sets of each picture and no cross picture weave. That is
     * the whole of what weave mode offers: every source row of a picture at
     * once, at the picture rate, with the source's own comb wherever the two
     * row sets are 1/60 s apart. Adaptive and bob both run at the field rate
     * instead and hold nothing.
     *
     * Only the deinterlaced movie path is affected: a field whose picture came
     * from the display copy, or any field the renderer scanned out at high
     * resolution, presents its own composition exactly as before. */
    const bool movie_field_pair = deinterlace_mode == RT_PGS_DEINTERLACE_WEAVE &&
                                  scanout.image && scanout.interlaced &&
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
        } else if (m_held_scanout.image &&
                   (scanout.image || deinterlace_mode != RT_PGS_DEINTERLACE_WEAVE)) {
            /* Dropped for a real field from the other producer, and for any
             * field at all once the mode is no longer weave: nothing repeats
             * the pair in adaptive or bob, so holding the image there would
             * only retain a Vulkan image no one reads. Within weave, a field
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
        /* The field is finished. It is latched here rather than presented:
         * rt_pgs_present_pump (gs_parallel_present.cpp) decides when it
         * reaches the window, so the host can present at its own rate
         * without this call, and therefore the guest field boundary, waiting
         * for a swapchain. The held pair above still decides which result
         * becomes the latest one, and storing the result keeps its image
         * alive for exactly as long as the slot names it, the same way the
         * held pair does.
         *
         * Every windowed field bumps the serial, including one the renderer
         * produced no image for: that field presents a cleared backbuffer
         * and an empty present rectangle, which is a change on screen and
         * has to reach it. */
        m_latest_scanout = *to_present;
        m_latest_aspect = present_aspect;
        ++m_latest_serial;
    }
#endif
    if (m_screenshot_path && scanout.image) {
        /* ICORECOMP_GS_SCREENSHOT, a rendering diagnostic. This is NOT the
         * user-facing screenshot: that one is a separate path
         * (rt_pgs_request_screenshot in gs_parallel_api.h, driven by
         * host/screenshot.cpp) which copies the window backbuffer over the
         * present rectangle in present_frame, so it is the presented picture
         * at presented size with the aspect applied and the letterbox
         * excluded. The two answer different questions and neither replaces
         * the other.
         *
         * Raw scanout pixels, deliberately NOT aspect-corrected: this file is
         * the regression baseline for rendering, so it has to stay a function
         * of the GS output alone and byte-comparable against a gs-replay dump.
         * It is therefore not the shape the game has on screen (512x448 here,
         * or the raw 720x240 field for a movie field under bob, so a movie
         * screenshot depends on display.deinterlace; gameplay does not, since
         * bob and weave are movie-only) against a 4:3 display; the display
         * aspect for the same frame is in the "display aspect" log line
         * above.
         *
         * The overlay (draw_overlay) never lands here: it is drawn straight
         * to the swapchain backbuffer in present_frame/present_ui_windowed,
         * not to scanout.image, so a screenshot never contains it. */
        std::string why;
        if (!rt_gs_write_scanout_ppm(*m_device, *scanout.image, m_screenshot_path,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, &why)) {
            warnf("paraLLEl-GS: screenshot write to %s failed (%s); no further field is"
                  " written (ICORECOMP_GS_SCREENSHOT)", m_screenshot_path, why.c_str());
            m_screenshot_path = nullptr; /* do not spam every field */
        }
    }

    /* No RT_PGS_VSYNC_PRESENTED here. This call presents nothing, and
     * whether the present pump is keeping up is what rt_pgs_present_pump's
     * own return value and the `present` verbose channel report; see
     * gs_parallel_api.h. */
    uint32_t flags = latched ? RT_PGS_VSYNC_LATCHED : 0u;
    if (m_window_closed.load(std::memory_order_acquire)) flags |= RT_PGS_VSYNC_WINDOW_CLOSED;
    return flags;
}

/* Boot trace, register stream. See gs_parallel_impl.h next to kBootTraceFields. */
void RtPgs::boot_trace_registers(bool have_image) {
    const auto& pv = m_iface->get_priv_register_state();
    BootRegSig sig;
    sig.en1 = pv.pmode.EN1;
    sig.en2 = pv.pmode.EN2;
    /* The circuit the lines describe is the one the CRTC reads, EN2 ?
     * circuit 2 : circuit 1, the same rule the placement lines above use. */
    const auto& fb = pv.pmode.EN2 ? pv.dispfb2 : pv.dispfb1;
    const auto& dp = pv.pmode.EN2 ? pv.display2 : pv.display1;
    sig.fbp = fb.FBP; sig.fbw = fb.FBW; sig.psm = fb.PSM; sig.dbx = fb.DBX; sig.dby = fb.DBY;
    sig.dx = dp.DX; sig.dy = dp.DY; sig.dw = dp.DW; sig.dh = dp.DH;
    sig.magh = dp.MAGH; sig.magv = dp.MAGV;
    sig.bgr = pv.bgcolor.R; sig.bgg = pv.bgcolor.G; sig.bgb = pv.bgcolor.B;
    sig.cmod = pv.smode1.CMOD; sig.inter = pv.smode2.INT; sig.ffmd = pv.smode2.FFMD;
    sig.image = have_image;
    const bool changed = !m_boot_sig_valid || !(sig == m_boot_sig);
    if (changed) {
        m_boot_sig_valid = true;
        m_boot_sig = sig;
        logf("paraLLEl-GS: boot trace: field %llu: PMODE EN1=%u EN2=%u; circuit%u DISPFB"
             " FBP=0x%x (block 0x%x) FBW=%u PSM=%u DBX=%u DBY=%u, DISPLAY DX=%u DY=%u DW=%u"
             " DH=%u MAGH=%u MAGV=%u; BGCOLOR %u,%u,%u; SMODE1 CMOD=%u SMODE2 INT=%u FFMD=%u;"
             " %s",
             (unsigned long long)m_vsyncs, sig.en1, sig.en2, pv.pmode.EN2 ? 2u : 1u,
             sig.fbp, sig.fbp * 32u, sig.fbw, sig.psm, sig.dbx, sig.dby,
             sig.dx, sig.dy, sig.dw, sig.dh, sig.magh, sig.magv,
             sig.bgr, sig.bgg, sig.bgb, sig.cmod, sig.inter, sig.ffmd,
             have_image ? "the renderer gave a scanout image"
                        : "no scanout image (the window shows the clear)");
    }
    if (m_vsyncs == kBootTraceFields) {
        logf("paraLLEl-GS: boot trace: register trace ends at field %llu",
             (unsigned long long)m_vsyncs);
    }
}
