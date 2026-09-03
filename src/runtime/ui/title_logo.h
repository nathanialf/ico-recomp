/* ui/title_logo.h: the launcher's title image, built from the user's disc.
 *
 * The wordmark is the game's own: the three letter meshes I.p2o, C.p2o and
 * O.p2o drawn as flat white polygons at the positions the title animation
 * puts them at. Those three and the animation come out of the STGLOG.DF
 * archive of DFDATAS/DATA.DF. The launcher draws the result in place of its
 * "ICO" text, and falls back to that text whenever this fails: no disc
 * mounted, an unreadable archive, an unexpected mesh format.
 *
 * Two stages, because they cost three orders of magnitude apart:
 *
 *   Geometry. The meshes and their placement, reduced to a flat triangle
 *   list in a normalised box. This is the expensive half: it reads about
 *   3.6 MB off the disc and inflates about 8.2 MB of it. It happens once per
 *   run at most, and the cache file holds this rather than an image, so a
 *   later run skips all of it.
 *
 *   Raster. That triangle list filled at an exact pixel size, which is a
 *   fraction of a millisecond. The caller asks for the size the overlay will
 *   actually draw at, so the image never has to be scaled on screen: a
 *   minified texture is a blurred one, and these are hard-edged polygons the
 *   GS itself draws with no antialiasing. When the window scale changes, ask
 *   again at the new size rather than scaling what you have.
 *
 * No RmlUi and no ICORECOMP_UI guard: icorecomp-title-logo-selftest links
 * this file with the ISO reader and the log and nothing else.
 *
 * Runtime-internal, NOT part of the ABI contract.
 */
#ifndef ICORECOMP_UI_TITLE_LOGO_H
#define ICORECOMP_UI_TITLE_LOGO_H

#include <cstddef>
#include <cstdint>
#include <vector>

/* The rasterised wordmark. `rgba` is width * height * 4 bytes, row-major from
 * the top, R first, alpha premultiplied. Coverage is binary, so every pixel is
 * either transparent or the letter's colour; there are no blended edges. */
struct RtTitleLogo {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;

    bool valid() const { return width > 0 && height > 0 && rgba.size() == size_t(width) * height * 4u; }
};

/* Fills `out` with the wordmark rasterised into exactly width_px by height_px
 * pixels. The letters keep the proportions the game gives them and are
 * centred in that box under a single uniform scale, so a box whose aspect
 * does not match leaves a little empty margin rather than stretching them.
 *
 * The geometry is read once and kept: the first call for a given disc pays
 * for the archive, every later call is the raster alone, which is what makes
 * re-asking on a window-scale change cheap. Requires a mounted disc on that
 * first call (rt_iso_mounted() must be true, and the mount must stay put for
 * its duration); later calls do not touch the disc at all.
 *
 * Returns false with a one-line reason in `err` (may be null). Never fatal
 * and never partially successful: a false return leaves `out` invalid and the
 * caller keeps its text fallback. Every step logs under the "ui" channel with
 * its timing.
 *
 * Not reentrant and not thread safe against itself.
 */
bool rt_title_logo_build(uint32_t width_px, uint32_t height_px, RtTitleLogo& out, char* err,
                         size_t err_len);

/* Width divided by height of the wordmark's own box, or 0 before a successful
 * build. The stylesheet's dp box should match this so the raster fills it. */
float rt_title_logo_aspect();

/* The box the launcher's stylesheet gives the image, in density-independent
 * pixels. It lives here rather than only in ui/style/base.rcss because the
 * raster size has to be computed from it before the element exists: the
 * element is not in the document until the image is ready. The two copies must
 * agree, and ui_launcher.cpp logs a line naming both if the laid-out element
 * ever disagrees with what was rasterised. 238 by 56 is the height the title
 * block reserves at the wordmark's own proportions; the aspect the disc
 * actually gives is compared against this box at run time (verify_logo_box in
 * ui_launcher.cpp), and a drift between the two is logged rather than
 * resampled away quietly. */
constexpr uint32_t kRtTitleLogoDpWidth = 238;
constexpr uint32_t kRtTitleLogoDpHeight = 56;

/* That box in real pixels at `dp_ratio`, which is what to ask
 * rt_title_logo_build for. Rounded to whole pixels: the overlay draws the
 * texture across the element's box, so matching it means one texel a pixel and
 * no resampling. */
inline void rt_title_logo_pixel_box(float dp_ratio, uint32_t* width_px, uint32_t* height_px) {
    if (!(dp_ratio > 0.0f)) dp_ratio = 1.0f;
    if (width_px) *width_px = uint32_t(float(kRtTitleLogoDpWidth) * dp_ratio + 0.5f);
    if (height_px) *height_px = uint32_t(float(kRtTitleLogoDpHeight) * dp_ratio + 0.5f);
}

/* Where the cache for the currently mounted disc lives, or "" when no
 * writable location could be resolved. Exposed for the selftest and for the
 * log line the launcher prints. */
const char* rt_title_logo_cache_path();

#endif /* ICORECOMP_UI_TITLE_LOGO_H */
