/* ui/cursor_image.cpp: cut the letter I out of the title image and turn it
 * into a mouse cursor.
 *
 * Four steps, each of which decides from the image it is given and not from
 * anything written down here about this game's wordmark:
 *
 *   Extract. Threshold alpha, label the connected components, and take the
 *   leftmost component whose height is at least half the tallest one. In the
 *   wordmark the letters are separate shapes and the I is the leftmost of
 *   them; the height guard is what keeps a stray speck or a dot of
 *   antialiasing from being picked instead of a letter. The component is
 *   copied out on its own, so a neighbouring letter that overlaps its
 *   bounding box does not come with it, with a one pixel transparent margin
 *   around it.
 *
 *   Find the point. A cursor needs a tip. The mean number of opaque pixels
 *   per row over the top fifth of the glyph is compared with the same over
 *   the bottom fifth, and the narrower end is the tip. When the two are
 *   within 15 percent of each other the glyph has no pointed end to find
 *   (a plain rectangular I, which is what a squared-off wordmark gives) and
 *   the top is used, which is the end an upright letter would be pointed at
 *   anyway. The tip itself is the centre of the opaque span in the outermost
 *   row at that end.
 *
 *   Rotate and scale, in one resample. A classic arrow cursor points up and
 *   to the left: the tip is at the top left and the body falls away below it
 *   and to the right, about 22 degrees off vertical. So the tip end is
 *   turned upwards and the glyph's long axis is put on that angle. Note the
 *   sign: in this image space y runs down, so the matrix below turns the
 *   glyph anticlockwise as it appears on screen. Sampling is bilinear on
 *   premultiplied RGBA, which is the space that interpolates linearly, and
 *   the canvas is grown to the rotated bounding box so nothing is cut off.
 *   The scale and the rotation are one map rather than two passes: a second
 *   resample would blur what the first produced.
 *
 *   Outline. The wordmark is white and the game's menus are drawn over
 *   whatever the scene behind them is, which is light as often as it is
 *   dark, so a white glyph alone disappears against it. One pixel of dilated
 *   alpha in a dark colour goes underneath the glyph. If the glyph itself is
 *   dark, the outline is light instead: the decision is the mean luminance
 *   of the glyph's opaque pixels, so a disc whose letters are not white
 *   still gets a cursor that reads.
 *
 * The hotspot is exact rather than rounded: the resample's translation is
 * nudged by the fraction of a pixel that puts the tip on a whole-pixel
 * corner, so the recorded hotspot is where the tip actually is.
 */
#include "cursor_image.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

/* A pixel is part of a glyph at half alpha or more. The wordmark's own
 * coverage is binary (title_logo.cpp draws with antialiasing off, as the GS
 * does), so on the real image this threshold has nothing to decide; it is
 * here so that a soft-edged image is still handled sensibly. */
constexpr uint8_t kAlphaThreshold = 128;

/* The angle the glyph's long axis is put on, measured from vertical. About
 * 22 degrees is where the common desktop arrow cursors sit. */
constexpr double kCursorAngleDegrees = 22.0;

/* The fraction of the glyph's rows, at each end, that the tip test averages
 * over, and how much narrower one end has to be than the other before the
 * difference counts. */
constexpr double kTipBandFraction = 0.20;
constexpr double kTipRatioMargin = 0.15;

/* The outline: one pixel of dilated alpha under the glyph, in whichever of
 * these two contrasts with it. The colours are the ones the drawn arrow in
 * ui/style/base.rcss uses for its outline and its fill, so the two cursors
 * read the same way. */
constexpr uint8_t kDarkOutline[3] = {0x0A, 0x0A, 0x0A};
constexpr uint8_t kLightOutline[3] = {0xF5, 0xF5, 0xF5};

/* Bounds. The input is the launcher's title raster (ui/title_logo.h) and the
 * output is a cursor: both are small. A malformed or absurd input must not be
 * able to ask for an unbounded allocation, and this file allocates several
 * buffers the size of the input (a component label per pixel is four bytes of
 * it), so the bound is set against what the one caller can actually produce
 * rather than against the raster's own 8192 ceiling. The title box is 238 by
 * 56 dp, so even a density ratio of 4 gives 952 by 224. 4096 on either side
 * leaves better than four times that, and the total pixel cap keeps a long
 * thin input from reaching 4096 by 4096, which would be 67 MB of RGBA and
 * another 67 MB of labels. */
constexpr uint32_t kMaxInputDim = 4096;
constexpr size_t kMaxInputPixels = 8u * 1024 * 1024;
constexpr uint32_t kMaxOutputDim = 1024;

void set_err(char* err, size_t err_len, const char* fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

struct Component {
    uint32_t min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    size_t pixels = 0;
    uint32_t width() const { return max_x - min_x + 1; }
    uint32_t height() const { return max_y - min_y + 1; }
};

/* Labels the opaque pixels into 8-connected components. `labels` comes back
 * the size of the image, -1 where the pixel is transparent and the index of
 * the component otherwise. Eight-connected rather than four so a glyph whose
 * strokes only touch at a corner, which a hard-edged raster of a diagonal
 * produces, stays one shape. */
void label_components(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<int32_t>& labels,
                      std::vector<Component>& comps) {
    labels.assign(size_t(w) * h, -1);
    comps.clear();
    std::vector<uint32_t> stack;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t at = size_t(y) * w + x;
            if (labels[at] != -1 || rgba[at * 4 + 3] < kAlphaThreshold) continue;

            const int32_t id = int32_t(comps.size());
            Component c;
            c.min_x = c.max_x = x;
            c.min_y = c.max_y = y;
            labels[at] = id;
            stack.clear();
            stack.push_back(uint32_t(at));
            while (!stack.empty()) {
                const uint32_t p = stack.back();
                stack.pop_back();
                const uint32_t px = p % w, py = p / w;
                ++c.pixels;
                c.min_x = std::min(c.min_x, px);
                c.max_x = std::max(c.max_x, px);
                c.min_y = std::min(c.min_y, py);
                c.max_y = std::max(c.max_y, py);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        const int64_t nx = int64_t(px) + dx, ny = int64_t(py) + dy;
                        if (nx < 0 || ny < 0 || nx >= int64_t(w) || ny >= int64_t(h)) continue;
                        const size_t n = size_t(ny) * w + size_t(nx);
                        if (labels[n] != -1 || rgba[n * 4 + 3] < kAlphaThreshold) continue;
                        labels[n] = id;
                        stack.push_back(uint32_t(n));
                    }
                }
            }
            comps.push_back(c);
        }
    }
}

/* One RGBA image in the same layout as the input: premultiplied, row-major,
 * R first. */
struct Bitmap {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;

    void resize(uint32_t width, uint32_t height) {
        w = width;
        h = height;
        px.assign(size_t(w) * h * 4u, 0);
    }
    uint8_t* at(uint32_t x, uint32_t y) { return &px[(size_t(y) * w + x) * 4u]; }
    const uint8_t* at(uint32_t x, uint32_t y) const { return &px[(size_t(y) * w + x) * 4u]; }
};

/* Bilinear sample of `src` at the corner-space point (sx, sy), where (0, 0)
 * is the top left corner of pixel (0, 0) and a pixel's centre is at its
 * index plus a half. Outside the image reads as transparent, which is what
 * makes the rotated canvas fade out at its edges rather than smear the
 * border row across it. Premultiplied throughout: it is the only space in
 * which a weighted sum of colours is the right answer. */
void sample_bilinear(const Bitmap& src, double sx, double sy, double out[4]) {
    const double fx = sx - 0.5, fy = sy - 0.5;
    const double x0 = std::floor(fx), y0 = std::floor(fy);
    const double tx = fx - x0, ty = fy - y0;
    const int64_t ix = int64_t(x0), iy = int64_t(y0);
    for (int c = 0; c < 4; ++c) out[c] = 0.0;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            const int64_t px = ix + i, py = iy + j;
            if (px < 0 || py < 0 || px >= int64_t(src.w) || py >= int64_t(src.h)) continue;
            const double weight = (i ? tx : 1.0 - tx) * (j ? ty : 1.0 - ty);
            if (weight <= 0.0) continue;
            const uint8_t* s = src.at(uint32_t(px), uint32_t(py));
            for (int c = 0; c < 4; ++c) out[c] += weight * double(s[c]);
        }
    }
}

uint8_t to_byte(double v) {
    const double r = std::floor(v + 0.5);
    if (r <= 0.0) return 0;
    if (r >= 255.0) return 255;
    return uint8_t(r);
}

} // namespace

bool rt_cursor_image_build(const uint8_t* rgba, uint32_t width, uint32_t height, float dp_ratio,
                           RtCursorImage& out, char* err, size_t err_len) {
    out = RtCursorImage();
    if (!rgba || width == 0 || height == 0) {
        set_err(err, err_len, "the source image is %ux%u", width, height);
        return false;
    }
    if (width > kMaxInputDim || height > kMaxInputDim ||
        size_t(width) * size_t(height) > kMaxInputPixels) {
        set_err(err, err_len, "a %ux%u source image is outside what this reads", width, height);
        return false;
    }
    if (!(dp_ratio > 0.0f)) dp_ratio = 1.0f;

    /* ---- extract the leftmost tall component ---------------------------- */

    std::vector<int32_t> labels;
    std::vector<Component> comps;
    label_components(rgba, width, height, labels, comps);
    if (comps.empty()) {
        set_err(err, err_len, "the %ux%u source image has no opaque pixels", width, height);
        return false;
    }

    uint32_t tallest = 0;
    for (const Component& c : comps) tallest = std::max(tallest, c.height());
    /* Half the tallest, rounded up, so a two-pixel tallest still admits a
     * one-pixel component rather than nothing. */
    const uint32_t min_height = (tallest + 1) / 2;

    size_t chosen = comps.size();
    for (size_t i = 0; i < comps.size(); ++i) {
        if (comps[i].height() < min_height) continue;
        if (chosen == comps.size() || comps[i].min_x < comps[chosen].min_x) chosen = i;
    }
    if (chosen == comps.size()) {
        /* Not reachable while `tallest` came out of this same list, but the
         * loop above is a search and a search that found nothing must say so
         * rather than index past the end. */
        set_err(err, err_len, "no component of the %ux%u source image is at least %u pixels tall",
            width, height, min_height);
        return false;
    }
    const Component& glyph = comps[chosen];
    const int32_t glyph_id = int32_t(chosen);

    /* Copied out on its own, with a one pixel transparent margin: only the
     * chosen component's pixels come, so an overlapping neighbour does not,
     * and the margin gives the outline pass and the rotation's edge samples
     * somewhere to land. */
    Bitmap crop;
    crop.resize(glyph.width() + 2, glyph.height() + 2);
    for (uint32_t y = glyph.min_y; y <= glyph.max_y; ++y) {
        for (uint32_t x = glyph.min_x; x <= glyph.max_x; ++x) {
            if (labels[size_t(y) * width + x] != glyph_id) continue;
            std::memcpy(crop.at(x - glyph.min_x + 1, y - glyph.min_y + 1),
                        &rgba[(size_t(y) * width + x) * 4u], 4);
        }
    }

    /* ---- which end is the point ----------------------------------------- */

    const uint32_t gh = glyph.height(), gw = glyph.width();
    const uint32_t band = std::max<uint32_t>(1, uint32_t(double(gh) * kTipBandFraction + 0.5));
    /* The two bands can only overlap on a glyph a handful of rows tall; when
     * they do the two means come out equal and the ambiguity rule below
     * takes the top, which is the right answer for a glyph with no room for
     * a taper anyway. */
    double top_sum = 0.0, bottom_sum = 0.0;
    for (uint32_t i = 0; i < band; ++i) {
        for (uint32_t x = 0; x < gw; ++x) {
            if (labels[size_t(glyph.min_y + i) * width + glyph.min_x + x] == glyph_id) top_sum += 1.0;
            if (labels[size_t(glyph.max_y - i) * width + glyph.min_x + x] == glyph_id) bottom_sum += 1.0;
        }
    }
    const double top_mean = top_sum / double(band);
    const double bottom_mean = bottom_sum / double(band);
    const double lo = std::min(top_mean, bottom_mean), hi = std::max(top_mean, bottom_mean);
    const bool ambiguous = !(hi > 0.0) || lo / hi > 1.0 - kTipRatioMargin;
    out.tip_at_top = ambiguous ? true : (top_mean < bottom_mean);

    /* The tip: the centre of the opaque span in the outermost row at that
     * end. A component that spans those rows has at least one pixel in each
     * of them, so the span always exists. */
    const uint32_t tip_row = out.tip_at_top ? glyph.min_y : glyph.max_y;
    uint32_t span_lo = glyph.max_x, span_hi = glyph.min_x;
    for (uint32_t x = glyph.min_x; x <= glyph.max_x; ++x) {
        if (labels[size_t(tip_row) * width + x] != glyph_id) continue;
        span_lo = std::min(span_lo, x);
        span_hi = std::max(span_hi, x);
    }
    /* Corner space in the crop: the crop's pixel (0, 0) is the source's
     * (min_x - 1, min_y - 1), and the tip sits on the outer edge of its own
     * row rather than at its centre. */
    const double tip_x = (double(span_lo) + double(span_hi) + 1.0) * 0.5 - double(glyph.min_x) + 1.0;
    const double tip_y = out.tip_at_top ? 1.0 : double(gh) + 1.0;

    /* ---- the outline's colour ------------------------------------------- */

    /* Mean luminance of the glyph's own opaque pixels. Coverage in the
     * wordmark is binary, so for an opaque pixel the premultiplied bytes are
     * the colour itself and no division is needed. Rec. 709 weights. */
    double lum_sum = 0.0;
    size_t lum_count = 0;
    for (uint32_t y = glyph.min_y; y <= glyph.max_y; ++y) {
        for (uint32_t x = glyph.min_x; x <= glyph.max_x; ++x) {
            if (labels[size_t(y) * width + x] != glyph_id) continue;
            const uint8_t* p = &rgba[(size_t(y) * width + x) * 4u];
            lum_sum += 0.2126 * double(p[0]) + 0.7152 * double(p[1]) + 0.0722 * double(p[2]);
            ++lum_count;
        }
    }
    const double mean_luminance = lum_count ? lum_sum / double(lum_count) : 255.0;
    out.light_outline = mean_luminance < 128.0;
    const uint8_t* outline_rgb = out.light_outline ? kLightOutline : kDarkOutline;

    /* ---- rotate and scale ------------------------------------------------ */

    /* The map is dst = scale * R(src) + offset, with R the y-down matrix
     * [c, -s; s, c]. The tip-to-base direction is (0, +1) when the tip is at
     * the top of the crop and (0, -1) when it is at the bottom, and it has to
     * come out as (sin A, cos A): down and to the right, which puts the tip
     * up and to the left. Solving R(u) = (sin A, cos A) for each case gives
     * the two coefficient pairs below. */
    const double angle = kCursorAngleDegrees * 3.14159265358979323846 / 180.0;
    const double sin_a = std::sin(angle), cos_a = std::cos(angle);
    const double c = out.tip_at_top ? cos_a : -cos_a;
    const double s = out.tip_at_top ? -sin_a : sin_a;

    /* Against the glyph's own long axis, which is what cursor_image.h
     * promises. On the retail wordmark the letter I is taller than it is
     * wide, so this is its height and the finished cursor is the same size it
     * was when this line used the height outright; a glyph that came out
     * wider than tall would now be scaled by that width instead of being
     * blown up past the target. */
    const uint32_t axis_px = std::max(gw, gh);
    const double target_axis = double(kRtCursorAxisDp) * double(dp_ratio);
    const double scale = target_axis / double(axis_px);
    if (!(scale > 0.0) || !std::isfinite(scale)) {
        set_err(err, err_len, "a %u pixel long glyph axis at density %.3f gives no usable scale",
            axis_px, double(dp_ratio));
        return false;
    }

    const double corners_x[4] = {0.0, double(crop.w), double(crop.w), 0.0};
    const double corners_y[4] = {0.0, 0.0, double(crop.h), double(crop.h)};
    double min_x = 0.0, max_x = 0.0, min_y = 0.0, max_y = 0.0;
    for (int i = 0; i < 4; ++i) {
        const double rx = scale * (c * corners_x[i] - s * corners_y[i]);
        const double ry = scale * (s * corners_x[i] + c * corners_y[i]);
        if (i == 0) {
            min_x = max_x = rx;
            min_y = max_y = ry;
        } else {
            min_x = std::min(min_x, rx);
            max_x = std::max(max_x, rx);
            min_y = std::min(min_y, ry);
            max_y = std::max(max_y, ry);
        }
    }

    /* Two pixels of padding on every side: one for the outline's dilation,
     * one for the sub-pixel nudge below, which moves the content by up to a
     * whole pixel towards the origin. One spare pixel on the far side covers
     * the same nudge there. */
    constexpr uint32_t kPad = 2;
    double off_x = double(kPad) - min_x;
    double off_y = double(kPad) - min_y;
    /* Put the tip on a whole-pixel corner, so the recorded hotspot is exactly
     * where the tip is rather than the nearest pixel to it. */
    const double tip_dst_x = scale * (c * tip_x - s * tip_y) + off_x;
    const double tip_dst_y = scale * (s * tip_x + c * tip_y) + off_y;
    off_x -= tip_dst_x - std::floor(tip_dst_x);
    off_y -= tip_dst_y - std::floor(tip_dst_y);

    const double span_w = max_x - min_x, span_h = max_y - min_y;
    if (!std::isfinite(span_w) || !std::isfinite(span_h)) {
        set_err(err, err_len, "the rotated glyph has no finite extent");
        return false;
    }
    const double want_w = std::ceil(span_w) + double(2 * kPad) + 1.0;
    const double want_h = std::ceil(span_h) + double(2 * kPad) + 1.0;
    if (want_w > double(kMaxOutputDim) || want_h > double(kMaxOutputDim)) {
        set_err(err, err_len, "a %.0fx%.0f cursor is outside what this builds", want_w, want_h);
        return false;
    }
    Bitmap rotated;
    rotated.resize(uint32_t(want_w), uint32_t(want_h));

    /* Inverse map, per destination pixel centre. R is a rotation, so its
     * inverse is its transpose. */
    for (uint32_t y = 0; y < rotated.h; ++y) {
        for (uint32_t x = 0; x < rotated.w; ++x) {
            const double dx = (double(x) + 0.5 - off_x) / scale;
            const double dy = (double(y) + 0.5 - off_y) / scale;
            const double sx = c * dx + s * dy;
            const double sy = -s * dx + c * dy;
            double t[4];
            sample_bilinear(crop, sx, sy, t);
            if (t[3] <= 0.0) continue;
            uint8_t* d = rotated.at(x, y);
            for (int i = 0; i < 4; ++i) d[i] = to_byte(t[i]);
        }
    }

    /* ---- outline --------------------------------------------------------- */

    out.width = rotated.w;
    out.height = rotated.h;
    out.rgba.assign(size_t(out.width) * out.height * 4u, 0);
    for (uint32_t y = 0; y < rotated.h; ++y) {
        for (uint32_t x = 0; x < rotated.w; ++x) {
            /* Dilated alpha: the widest alpha in the 3x3 neighbourhood,
             * which is the glyph grown by one pixel in every direction. */
            uint32_t dilated = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int64_t nx = int64_t(x) + dx, ny = int64_t(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= int64_t(rotated.w) || ny >= int64_t(rotated.h))
                        continue;
                    dilated = std::max<uint32_t>(dilated, rotated.at(uint32_t(nx), uint32_t(ny))[3]);
                }
            }
            const uint8_t* g = rotated.at(x, y);
            uint8_t* d = &out.rgba[(size_t(y) * out.width + x) * 4u];
            /* The glyph over the outline, both premultiplied:
             * dst = glyph + outline * (1 - glyph_alpha). */
            const double keep = 1.0 - double(g[3]) / 255.0;
            const double outline_a = double(dilated) * keep;
            for (int i = 0; i < 3; ++i) {
                d[i] = to_byte(double(g[i]) + outline_a * double(outline_rgb[i]) / 255.0);
            }
            d[3] = to_byte(double(g[3]) + outline_a);
        }
    }

    /* tip_dst_* were measured before the nudge, and the nudge subtracted
     * exactly their fractional part, so the tip is now on these two whole
     * numbers. Both are at least kPad - 1, so the outline has its pixel. */
    out.hotspot_x = uint32_t(std::floor(tip_dst_x));
    out.hotspot_y = uint32_t(std::floor(tip_dst_y));
    if (out.hotspot_x >= out.width || out.hotspot_y >= out.height) {
        set_err(err, err_len, "the tip landed at (%u, %u), outside the %ux%u cursor",
            out.hotspot_x, out.hotspot_y, out.width, out.height);
        out = RtCursorImage();
        return false;
    }

    /* ---- trim ------------------------------------------------------------
     *
     * The canvas was sized to hold the crop's rotated rectangle, and a
     * rotated rectangle leaves empty corners: on the real wordmark, whose I
     * is a horn tapering to a point, that is a dozen fully transparent rows
     * above the tip. Cropping to what was actually drawn keeps the texture
     * honest about its size and the hotspot close to its corner. The hotspot
     * moves with the crop; it cannot fall outside it, because the tip is a
     * point on the glyph and the glyph is what the box is measured from. */
    uint32_t bx0 = out.width, by0 = out.height, bx1 = 0, by1 = 0;
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            if (out.rgba[(size_t(y) * out.width + x) * 4u + 3] == 0) continue;
            bx0 = std::min(bx0, x);
            bx1 = std::max(bx1, x);
            by0 = std::min(by0, y);
            by1 = std::max(by1, y);
        }
    }
    if (bx0 > bx1 || by0 > by1) {
        set_err(err, err_len, "the %ux%u cursor came out empty", out.width, out.height);
        out = RtCursorImage();
        return false;
    }
    if (out.hotspot_x < bx0 || out.hotspot_x > bx1 || out.hotspot_y < by0 || out.hotspot_y > by1) {
        set_err(err, err_len, "the tip at (%u, %u) is outside the drawn box (%u, %u)-(%u, %u)",
            out.hotspot_x, out.hotspot_y, bx0, by0, bx1, by1);
        out = RtCursorImage();
        return false;
    }
    if (bx0 != 0 || by0 != 0 || bx1 + 1 != out.width || by1 + 1 != out.height) {
        const uint32_t tw = bx1 - bx0 + 1, th = by1 - by0 + 1;
        std::vector<uint8_t> trimmed(size_t(tw) * th * 4u);
        for (uint32_t y = 0; y < th; ++y) {
            std::memcpy(&trimmed[size_t(y) * tw * 4u],
                        &out.rgba[(size_t(y + by0) * out.width + bx0) * 4u], size_t(tw) * 4u);
        }
        out.rgba.swap(trimmed);
        out.width = tw;
        out.height = th;
        out.hotspot_x -= bx0;
        out.hotspot_y -= by0;
    }
    return true;
}
