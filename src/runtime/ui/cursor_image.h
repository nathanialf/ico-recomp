/* ui/cursor_image.h: the drawn menu cursor, cut out of the title image.
 *
 * The pointer on the game's own menus (guest/menu_nav.h) needs a cursor of
 * its own, because relative mouse mode hides the system one. Rather than
 * draw a generic arrow, this takes the letter I out of the wordmark the
 * launcher already built from the user's disc (ui/title_logo.h), turns it to
 * the angle a mouse cursor is normally drawn at, and outlines it so it reads
 * over both a light and a dark picture.
 *
 * Input is the published wordmark: premultiplied RGBA, row-major from the
 * top, R first, exactly as RtTitleLogo carries it. Output is the same
 * layout, plus the hotspot: the point in the image that has to land on the
 * cursor's position.
 *
 * Nothing in here is disc-specific and nothing is written down about the
 * disc: every number comes out of the image it is handed. No RmlUi, no SDL,
 * no logging, so icorecomp-title-logo-selftest can link it on its own and
 * check it against a synthetic image.
 *
 * Runtime-internal, NOT part of the ABI contract.
 */
#ifndef ICORECOMP_UI_CURSOR_IMAGE_H
#define ICORECOMP_UI_CURSOR_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <vector>

/* The cursor image. `rgba` is width * height * 4 bytes, premultiplied, and
 * unlike the wordmark it has soft edges: it is a rotated resample of one.
 *
 * The hotspot is in corner space: (0, 0) is the top left corner of the top
 * left pixel, so a hotspot of (5, 2) means the image has to be drawn with
 * its left edge 5 pixels left of the cursor position and its top edge 2
 * pixels above it. The build places the tip exactly on that corner, so the
 * pair is exact and not a rounding of somewhere nearby. */
struct RtCursorImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
    uint32_t hotspot_x = 0;
    uint32_t hotspot_y = 0;

    /* Which end of the extracted glyph was found to be the pointed one, and
     * therefore which end was turned upwards. Reported in the log line so a
     * run says what it decided rather than leaving it to be guessed. */
    bool tip_at_top = true;
    /* True when the glyph came out dark and the outline had to be light.
     * See the luminance test in cursor_image.cpp. */
    bool light_outline = false;

    bool valid() const {
        return width > 0 && height > 0 && rgba.size() == size_t(width) * height * 4u;
    }
};

/* Builds the cursor from a premultiplied RGBA image.
 *
 * `dp_ratio` is the overlay context's density ratio (ui_density_ratio()).
 * The glyph is scaled so its own long axis is kRtCursorAxisDp times that
 * many pixels, which is what keeps the cursor the same apparent size at
 * every window scale, exactly as the title image is rasterised for its
 * density rather than scaled on screen.
 *
 * Returns false with a one-line reason in `err` (may be null) and leaves
 * `out` invalid. Never fatal: the caller keeps the drawn arrow.
 */
bool rt_cursor_image_build(const uint8_t* rgba, uint32_t width, uint32_t height, float dp_ratio,
                           RtCursorImage& out, char* err, size_t err_len);

/* The length of the glyph's own long axis in the finished image, in
 * density-independent pixels. The image itself comes out a little taller
 * and a little wider than this: the glyph is tilted, and the outline adds a
 * pixel on every side. */
constexpr float kRtCursorAxisDp = 32.0f;

#endif /* ICORECOMP_UI_CURSOR_IMAGE_H */
