/* gs/render/gs_crtc.h: the CRTC. Turns the privileged register block into the
 * geometry one output frame is built at, and into the push constants the
 * scanout shader reads.
 *
 * Ours (MIT). The register fields are the GS User's Manual's; the two frame
 * modes are docs/SETTINGS.md section 6's, and the NTSC numbers in
 * mode_area() are the ones measured in this project and recorded in
 * gs/gs_parallel_scanout.cpp's placement comment.
 *
 * What the CRTC does, in the order this file does it:
 *
 *   1. Video mode. SMODE1 CMOD picks NTSC or PAL; SMODE2 INT and FFMD say
 *      whether the raster is interlaced and whether a field reads every line
 *      of the buffer or every other one.
 *   2. Circuit rectangles. Each of DISPLAY1 and DISPLAY2 gives a position and
 *      a size in video clocks and raster lines, which MAGH and MAGV divide
 *      down to pixels and lines per field.
 *   3. The frame. display.raster crt makes the frame the mode's own visible
 *      area and crops anything past it; display.raster window grows the frame
 *      until it holds every enabled circuit and reads each buffer from its
 *      origin instead of from DBX/DBY.
 *   4. The merge. PMODE decides which circuits are enabled, what the
 *      background is, and where the blend factor comes from.
 *   5. Deinterlace. The output frame is two fields tall for an interlaced
 *      scanout; bob fills all of it from the current field, weave fills this
 *      field's own rows and leaves the other set standing, and adaptive
 *      decides per pixel between the two. shaders/scanout.comp holds all
 *      three.
 */
#ifndef ICORECOMP_GS_CRTC_H
#define ICORECOMP_GS_CRTC_H

#include "gs_regs.h"

#include <cstddef>   /* offsetof, for the push block layout assertions */
#include <cstdint>

namespace gsr {

/* Frame modes, same meaning as RT_PGS_RASTER_* in gs/gs_parallel_api.h so a
 * setting reaches either backend unchanged. */
enum : uint32_t {
    GSR_RASTER_CRT    = 0,
    GSR_RASTER_WINDOW = 1,
};

/* Deinterlace modes, same meaning as RT_PGS_DEINTERLACE_*. All three are
 * implemented; adaptive is this renderer's own motion filter and is excluded
 * from the parity gate, because the hardware has no adaptive mode to be
 * accurate to. shaders/scanout.comp states the filter. */
enum : uint32_t {
    GSR_DEINTERLACE_ADAPTIVE = 0,
    GSR_DEINTERLACE_BOB      = 1,
    GSR_DEINTERLACE_WEAVE    = 2,
};

/* The scanout shader's push constants. Plain scalars, no arrays and no
 * vectors, so std140 and std430 lay it out identically and the C++ struct can
 * be memcpy'd into the command list. 32 words, 128 bytes, which is exactly the
 * RHI's push constant budget: the next field to go in here needs a uniform
 * buffer instead, and rhi.h has four slots for one.
 *
 * Circuit fields are suffixed 1 and 2 for DISPLAY1/DISPFB1 and
 * DISPLAY2/DISPFB2. A disabled circuit has enable 0 and its other fields are
 * not read. */
struct ScanoutPush {
    uint32_t frame_w;      /* output frame width in pixels */
    uint32_t frame_h;      /* output frame height in pixels, both fields */
    uint32_t field;        /* 0 or 1, the field being scanned out */
    uint32_t deinterlace;  /* GSR_DEINTERLACE_ADAPTIVE, _BOB or _WEAVE */

    uint32_t interlaced;   /* SMODE2 INT, after the progressive override */
    uint32_t ffmd;         /* SMODE2 FFMD: 1 means a field reads every other buffer line */
    uint32_t bgcolor;      /* BGCOLOR as 0x00BBGGRR */
    uint32_t merge;        /* EN1 | EN2<<1 | MMOD<<2 | AMOD<<3 | SLBG<<4 | ALP<<8 */

    uint32_t c1_enable, c1_base_block, c1_fbw, c1_psm;
    uint32_t c1_dbx, c1_dby, c1_x, c1_y;
    uint32_t c1_w, c1_h;

    uint32_t c2_enable, c2_base_block, c2_fbw, c2_psm;
    uint32_t c2_dbx, c2_dby, c2_x, c2_y;
    uint32_t c2_w, c2_h;

    /* Render scale. crtc_plan() leaves these zero: whether the sub-samples
     * of the displayed buffer exist is a question about what has been drawn,
     * not about the registers, so gs_native.cpp answers it and fills them in.
     * With hires set the output image is twice frame_w by twice frame_h, it
     * is read from the super-sampled shadow, and it is not deinterlaced at
     * all: the buffer holds a whole frame because the game drew one. */
    uint32_t hires;
    uint32_t samples;
    uint32_t hist_valid;   /* 0 when the adaptive filter has no previous field */
};

/* 31 words, in the order shaders/scanout.comp declares them. Nothing checked
 * the comment above against the struct, and a field inserted in one and not
 * the other reads the neighbouring register's value in the shader with no
 * symptom but a wrong picture. The 32nd word was a diagnostic scanout mode;
 * it went with the probe that set it on 2026-09-05. */
static_assert(sizeof(ScanoutPush) == 31 * sizeof(uint32_t),
              "ScanoutPush must stay 31 words, in step with shaders/scanout.comp");
static_assert(offsetof(ScanoutPush, c1_enable) == 8 * sizeof(uint32_t),
              "circuit 1 starts after the eight frame and mode words");
static_assert(offsetof(ScanoutPush, c2_enable) == 18 * sizeof(uint32_t),
              "circuit 2 starts after circuit 1's ten words");
static_assert(offsetof(ScanoutPush, hires) == 28 * sizeof(uint32_t),
              "the render scale words come after circuit 2's ten");
static_assert(offsetof(ScanoutPush, hist_valid) == 30 * sizeof(uint32_t),
              "hist_valid is the last word of the block");

/* Everything the host needs about one field, beyond the push constants. */
struct ScanoutPlan {
    ScanoutPush push{};
    bool have_picture = false; /* false when no circuit is enabled */
    uint32_t mode_width = 0;   /* the visible area of the video mode, in pixels */
    uint32_t mode_height = 0;  /* and in lines per field */
    double display_aspect = 0.0; /* what the finished frame should be shown at */
    uint32_t cmod = 0;         /* SMODE1 CMOD, 2 NTSC, 3 PAL */

    /* The registers the plan was derived from, carried for the field
     * diagnostics in gs_native.cpp and read by nothing else. A log that
     * reports only the result of the window arithmetic cannot be checked
     * against the arithmetic, which is what the black PAL picture of
     * 2026-09-04 cost a round trip to the user's machine to find out. */
    uint64_t pmode_raw = 0;
    uint64_t smode2_raw = 0;
    Display display1{};
    Display display2{};
};

/* Builds the plan for one field. `raster` and `deinterlace` are the settings
 * values; `field` is the field parity the vsync carries.
 *
 * Logs at most one line per distinct condition, so this is safe to call every
 * field. */
ScanoutPlan crtc_plan(const RegisterFile& regs, uint32_t raster, uint32_t deinterlace,
                      uint32_t field);

} // namespace gsr

#endif /* ICORECOMP_GS_CRTC_H */
