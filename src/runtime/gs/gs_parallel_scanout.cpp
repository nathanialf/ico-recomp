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
     * (Renderer line numbers here are the submodule with
     * third_party/patches/parallel-gs-0001-full-pixel-raster-snap.patch
     * applied, which is what the configure step leaves in the tree.)
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
     * At 1x the same value drives the weave instead (high_resolution_scanout
     * is off, force_deinterlace is on, gs_renderer.cpp:4246-4248 and
     * 5045-5062; weave.frag puts the current field on rows where
     * (y & 1) == phase). The argument-0 field reads v = 2Y+1 against the
     * argument-1 field's 2Y+2, so it is the earlier of the two and belongs on
     * the even rows, which is phase 0: the same assignment, so one derivation
     * serves both paths. */
    const unsigned counter_phase = (field & 1) ^ 1u;
    unsigned phase = counter_phase;
    const int copy_parity = m_copy_parity;
    bool phase_from_copy = false;
    bool phase_held = false;
    if (m_copy_ofy_base >= 0) {
        if (copy_parity >= 0) {
            phase = unsigned(copy_parity);
            phase_from_copy = true;
        } else if (m_last_phase >= 0) {
            phase = unsigned(m_last_phase);
            phase_held = true;
        }
    }
    info.phase = phase;
    m_last_phase = int(phase);
    m_copy_parity = -1;
    if (phase_held) ++m_phase_held;
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
    if ((phase_held || (phase_from_copy && phase != counter_phase))) {
        const uint64_t n = phase_held ? m_phase_held : m_phase_disagreed;
        if ((n & (n - 1)) == 0) {
            logf("paraLLEl-GS: field %llu phase %u from %s (counter would say %u);"
                 " held=%llu disagreed=%llu",
                 (unsigned long long)m_vsyncs, phase,
                 phase_held ? "the previous field, no display copy this field" : "the display copy",
                 counter_phase,
                 (unsigned long long)m_phase_held,
                 (unsigned long long)m_phase_disagreed);
        }
    }
    info.force_progressive = true;
    info.anti_blur = true;
    info.adapt_to_internal_horizontal_resolution = true;
    /* Paired with kModeDisplayAspect: that constant is the aspect of the
     * non-overscan mode area only. Flipping this to true without deriving a
     * new constant distorts geometry. */
    info.overscan = kScanoutOverscan;
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
     * as hires= on the geometry line. */
    info.high_resolution_scanout = m_opts.render_scale >= 4;
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
    if (scanout.image && std::memcmp(geom, m_aspect_log_geom, sizeof(geom)) != 0) {
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
        } else {
            logf("paraLLEl-GS: scanout reported no mode (internal %ux%u, mode %ux%u);"
                 " presenting stretched to the window, which is not the game's aspect",
                 scanout.internal_width, scanout.internal_height,
                 scanout.mode_width, scanout.mode_height);
        }
    }

    /* The per-field half of the same picture. Everything the scanout does
     * differently between the two fields is either info.phase or one of these
     * registers, so a handful of consecutive lines is enough to check that
     * nothing but the phase alternates and that the phase this side hands over
     * is the one the game's own per-field XYOFFSET was written against (see the
     * comment on info.phase above). copy= is what the snoop saw this field
     * (-1 for no display copy at all), counter= is what the field counter
     * alone would have said; the two differing is the residual wobble's
     * signature. Vertical fields only: the artefact this exists to settle is
     * vertical. */
    if (scanout.image && m_crtc_log_left) {
        --m_crtc_log_left;
        const auto& priv = m_iface->get_priv_register_state();
        logf("paraLLEl-GS: crtc field %llu phase=%u copy=%d counter=%u int=%u ffmd=%u cmod=%u en=%u%u"
             " dispfb1 dby=%u display1 dy=%u dh=%u magv=%u"
             " dispfb2 dby=%u display2 dy=%u dh=%u magv=%u",
             (unsigned long long)m_vsyncs, info.phase, copy_parity, counter_phase,
             (unsigned)priv.smode2.INT, (unsigned)priv.smode2.FFMD,
             (unsigned)priv.smode1.CMOD,
             (unsigned)priv.pmode.EN1, (unsigned)priv.pmode.EN2,
             (unsigned)priv.dispfb1.DBY, (unsigned)priv.display1.DY,
             (unsigned)priv.display1.DH, (unsigned)priv.display1.MAGV,
             (unsigned)priv.dispfb2.DBY, (unsigned)priv.display2.DY,
             (unsigned)priv.display2.DH, (unsigned)priv.display2.MAGV);
    }

#ifdef ICORECOMP_PGS_SDL
    if (m_wsi_active) present(scanout, aspect);
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
