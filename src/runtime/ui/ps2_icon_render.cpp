/* ui/ps2_icon_render.cpp: see ps2_icon_render.h. */
#include "ps2_icon_render.h"

#include "raster_edge.h"

#include "../runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <exception>

namespace {

void set_err(char* err, size_t err_len, const char* fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

constexpr uint32_t kMaxIconDim = 4096;

/* Supersample factor by output size. Every size a .ico or a window icon
 * asks for gets a soft edge rather than a jagged one; the factor drops with
 * size only to keep the fragment buffer small (the 4x tier at 64 px already
 * rasterises 256x256). One tier a size band, so the 64 px window icon and
 * its 128 px alternate do not come out with visibly different edges, which
 * is what a single 64 px threshold gave. */
uint32_t supersample_factor(uint32_t size_px) {
    if (size_px <= 64) return 4;
    if (size_px <= 256) return 2;
    return 1;
}

constexpr float kFovVDeg = 30.0f;
constexpr float kFillFraction = 0.88f;
constexpr float kPi = 3.14159265358979323846f;

float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

uint8_t to_byte(float v) {
    v = v * 255.0f + 0.5f;
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return uint8_t(v);
}

/* One rasterised fragment: the nearest triangle's interpolated colour and
 * depth at this pixel, or unset when no triangle covers it. */
struct Fragment {
    bool set = false;
    double depth = 0.0;
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

/* Gouraud- and z-shaded triangle fill, sharing the GS top-left rule with
 * ui/title_logo.cpp's binary-coverage fill_triangle (raster_edge.h). Unlike
 * that one this keeps a winning fragment (nearest depth) instead of a mask
 * bit, so overlapping triangles resolve correctly without a sort. */
void fill_triangle_z(std::vector<Fragment>& frags, uint32_t mw, uint32_t mh, const double px[3],
                     const double py[3], const double pz[3], const float pr[3], const float pg[3],
                     const float pb[3], const float pa[3]) {
    int o0 = 0, o1 = 1, o2 = 2;
    double ax = px[o0], ay = py[o0];
    double bx = px[o1], by = py[o1];
    double cx = px[o2], cy = py[o2];
    double area = rt_orient2d(ax, ay, bx, by, cx, cy);
    if (area == 0.0) return;
    if (area < 0.0) {
        std::swap(o1, o2);
        bx = px[o1];
        by = py[o1];
        cx = px[o2];
        cy = py[o2];
        area = -area;
    }

    const double fx_lo = std::floor(std::min(ax, std::min(bx, cx)) - 0.5);
    const double fx_hi = std::ceil(std::max(ax, std::max(bx, cx)) + 0.5);
    const double fy_lo = std::floor(std::min(ay, std::min(by, cy)) - 0.5);
    const double fy_hi = std::ceil(std::max(ay, std::max(by, cy)) + 0.5);
    const double last_x = double(mw) - 1.0, last_y = double(mh) - 1.0;
    if (fx_hi < 0.0 || fx_lo > last_x || fy_hi < 0.0 || fy_lo > last_y) return;
    const int x_lo = int(std::max(fx_lo, 0.0));
    const int x_hi = int(std::min(fx_hi, last_x));
    const int y_lo = int(std::max(fy_lo, 0.0));
    const int y_hi = int(std::min(fy_hi, last_y));

    const bool tl0 = rt_top_left(ax, ay, bx, by);
    const bool tl1 = rt_top_left(bx, by, cx, cy);
    const bool tl2 = rt_top_left(cx, cy, ax, ay);

    for (int y = y_lo; y <= y_hi; ++y) {
        const double py_ = double(y) + 0.5;
        for (int x = x_lo; x <= x_hi; ++x) {
            const double px_ = double(x) + 0.5;
            const double w0 = rt_orient2d(ax, ay, bx, by, px_, py_);
            const double w1 = rt_orient2d(bx, by, cx, cy, px_, py_);
            const double w2 = rt_orient2d(cx, cy, ax, ay, px_, py_);
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
            if ((w0 == 0.0 && !tl0) || (w1 == 0.0 && !tl1) || (w2 == 0.0 && !tl2)) continue;

            /* Barycentric weights. Each w is twice the area of the
             * sub-triangle on one edge, so it is the weight of the vertex
             * that edge does not touch: w0 is on edge ab, so it weights c;
             * w1 is on bc, so it weights a; w2 is on ca, so it weights b. */
            const double lc = w0 / area, la = w1 / area, lb = w2 / area;
            const double z = la * pz[o0] + lb * pz[o1] + lc * pz[o2];

            Fragment& f = frags[size_t(y) * mw + size_t(x)];
            if (f.set && z >= f.depth) continue;
            f.set = true;
            f.depth = z;
            f.r = float(la) * pr[o0] + float(lb) * pr[o1] + float(lc) * pr[o2];
            f.g = float(la) * pg[o0] + float(lb) * pg[o1] + float(lc) * pg[o2];
            f.b = float(la) * pb[o0] + float(lb) * pb[o1] + float(lc) * pb[o2];
            f.a = float(la) * pa[o0] + float(lb) * pa[o1] + float(lc) * pa[o2];
        }
    }
}

/* icon.sys's four background corner colours, converted once. Corner order
 * in the file (index 0..3) is assumed top-left, top-right, bottom-left,
 * bottom-right; that order is not verified, because this disc's four
 * corners are equal, so any permutation of them gives the same picture.
 *
 * A component outside 0..255 is out of the range the field is understood to
 * hold, so it is clamped and named in the log rather than changed quietly. */
struct BackgroundCorners {
    float c[4][3] = {};
};

BackgroundCorners background_corners(const RtPs2IconSys& icon_sys) {
    BackgroundCorners bg;
    uint32_t worst = 0;
    for (int i = 0; i < 4; ++i) {
        for (int k = 0; k < 3; ++k) {
            const uint32_t comp = icon_sys.bg_corner[i][k];
            if (comp > 255) worst = std::max(worst, comp);
            bg.c[i][k] = float(comp > 255 ? 255 : comp);
        }
    }
    if (worst != 0) {
        rt_log("ui", "ps2 icon: icon.sys has a background corner component of %u, outside 0..255;"
                    " clamped to 255 for the render",
            worst);
    }
    return bg;
}

/* Bilinear blend of the four corners. u, v in [0, 1]. */
void background_colour(const BackgroundCorners& bg, float u, float v, uint8_t out_rgb[3]) {
    float top[3], bottom[3];
    for (int k = 0; k < 3; ++k) {
        top[k] = bg.c[0][k] + (bg.c[1][k] - bg.c[0][k]) * u;
        bottom[k] = bg.c[2][k] + (bg.c[3][k] - bg.c[2][k]) * u;
    }
    for (int k = 0; k < 3; ++k) out_rgb[k] = to_byte((top[k] + (bottom[k] - top[k]) * v) / 255.0f);
}

void box_filter_down(const std::vector<uint8_t>& src, uint32_t sdim, uint32_t factor,
                     std::vector<uint8_t>& dst, uint32_t ddim) {
    dst.assign(size_t(ddim) * ddim * 4, 0);
    const uint32_t n = factor * factor;
    for (uint32_t y = 0; y < ddim; ++y) {
        for (uint32_t x = 0; x < ddim; ++x) {
            uint32_t sum[4] = {0, 0, 0, 0};
            for (uint32_t sy = 0; sy < factor; ++sy) {
                const uint8_t* row = &src[((size_t(y) * factor + sy) * sdim + size_t(x) * factor) * 4];
                for (uint32_t sx = 0; sx < factor; ++sx) {
                    const uint8_t* p = row + size_t(sx) * 4;
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
            }
            uint8_t* d = &dst[(size_t(y) * ddim + x) * 4];
            for (int c = 0; c < 4; ++c) d[c] = uint8_t((sum[c] + n / 2) / n);
        }
    }
}

} // namespace

bool rt_ps2_icon_render(const RtPs2Icon& icon, const RtPs2IconSys& icon_sys, uint32_t size_px,
                        uint32_t shape, RtPs2IconImage& out, char* err, size_t err_len) try {
    out = RtPs2IconImage();
    if (shape >= icon.shapes) {
        set_err(err, err_len, "shape %u is outside the icon's %u shapes", shape, icon.shapes);
        return false;
    }
    if (size_px == 0 || size_px > kMaxIconDim) {
        set_err(err, err_len, "%u px is outside the 1..%u this renders", size_px, kMaxIconDim);
        return false;
    }
    if (icon.vertex_count == 0 ||
        icon.pos.size() < size_t(icon.shapes) * icon.vertex_count * 3 ||
        icon.normal.size() < size_t(icon.vertex_count) * 3 ||
        icon.rgba.size() < size_t(icon.vertex_count) * 4) {
        set_err(err, err_len, "icon has no usable vertex data for shape %u", shape);
        return false;
    }

    auto shape_pos = [&](uint32_t v, int axis) {
        return float(icon.pos[(size_t(shape) * icon.vertex_count + v) * 3 + size_t(axis)]) / 4096.0f;
    };

    float minv[3], maxv[3];
    for (int a = 0; a < 3; ++a) minv[a] = maxv[a] = shape_pos(0, a);
    for (uint32_t v = 1; v < icon.vertex_count; ++v) {
        for (int a = 0; a < 3; ++a) {
            const float p = shape_pos(v, a);
            minv[a] = std::min(minv[a], p);
            maxv[a] = std::max(maxv[a], p);
        }
    }
    const float center[3] = {(minv[0] + maxv[0]) * 0.5f, (minv[1] + maxv[1]) * 0.5f,
                             (minv[2] + maxv[2]) * 0.5f};
    const float ext[3] = {(maxv[0] - minv[0]) * 0.5f, (maxv[1] - minv[1]) * 0.5f,
                          (maxv[2] - minv[2]) * 0.5f};
    /* The camera looks straight down +z, so depth never changes the
     * on-screen size: fit the larger of the x and y half-extents, not the
     * box diagonal, or a deep mesh renders far smaller than the fill
     * fraction says. */
    const float radius = std::max(ext[0], ext[1]);
    if (!(radius > 0.0f) || !std::isfinite(radius)) {
        set_err(err, err_len, "shape %u's vertices have a degenerate bounding box", shape);
        return false;
    }

    const float half_fov = (kFovVDeg * 0.5f) * (kPi / 180.0f);
    const float tan_half = std::tan(half_fov);
    const float dist = radius / (kFillFraction * tan_half);
    if (!(dist > 0.0f) || !std::isfinite(dist)) {
        set_err(err, err_len, "shape %u's camera distance did not come out finite", shape);
        return false;
    }
    const float cam_x = center[0], cam_y = center[1], cam_z = center[2] - dist;

    /* The near-plane guard in the vertex loop below moves any vertex the
     * camera would otherwise be level with or behind, which changes the
     * picture. It cannot fire while the camera sits clear of the front of
     * the bounding box, so say up front whether it can, rather than letting
     * the loop reshape geometry without a word. */
    if (!(dist > ext[2])) {
        rt_log("ui", "ps2 icon: camera distance %.3f is not clear of the model's z half-extent"
                    " %.3f, so the near-plane guard will move vertices and the render is not the"
                    " shape the file describes",
            double(dist), double(ext[2]));
    }

    const uint32_t factor = supersample_factor(size_px);
    const uint32_t render_dim = size_px * factor;

    std::vector<Fragment> frags(size_t(render_dim) * render_dim);
    const BackgroundCorners bg_corners = background_corners(icon_sys);

    for (uint32_t t = 0; t + 2 < icon.vertex_count; t += 3) {
        double sx[3], sy[3], sz[3];
        float sr[3], sg[3], sb[3], sa[3];
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = t + uint32_t(k);
            const float wx = shape_pos(v, 0), wy = shape_pos(v, 1), wz = shape_pos(v, 2);
            const float rx = wx - cam_x, ry = wy - cam_y;
            float rz = wz - cam_z;
            if (rz < 1e-4f) rz = 1e-4f; /* camera never fatal-crosses the model; guard the divide */
            const float ndc_x = (rx / rz) / tan_half;
            const float ndc_y = (ry / rz) / tan_half;
            sx[k] = (double(ndc_x) + 1.0) * 0.5 * double(render_dim);
            sy[k] = (double(ndc_y) + 1.0) * 0.5 * double(render_dim);
            sz[k] = double(rz);

            const float nx = float(icon.normal[v * 3 + 0]);
            const float ny = float(icon.normal[v * 3 + 1]);
            const float nz = float(icon.normal[v * 3 + 2]);
            const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
            float ux = 0.0f, uy = 0.0f, uz = 0.0f;
            if (nlen > 0.0f) {
                ux = nx / nlen;
                uy = ny / nlen;
                uz = nz / nlen;
            }

            float total[3] = {icon_sys.ambient[0], icon_sys.ambient[1], icon_sys.ambient[2]};
            for (int li = 0; li < 3; ++li) {
                float lx = icon_sys.light_dir[li][0], ly = icon_sys.light_dir[li][1],
                     lz = icon_sys.light_dir[li][2];
                const float llen = std::sqrt(lx * lx + ly * ly + lz * lz);
                if (!(llen > 0.0f)) continue;
                lx /= llen;
                ly /= llen;
                lz /= llen;
                float ndotl = ux * lx + uy * ly + uz * lz;
                if (ndotl < 0.0f) ndotl = 0.0f;
                total[0] += icon_sys.light_colour[li][0] * ndotl;
                total[1] += icon_sys.light_colour[li][1] * ndotl;
                total[2] += icon_sys.light_colour[li][2] * ndotl;
            }

            const float base_r = float(icon.rgba[v * 4 + 0]) / 128.0f;
            const float base_g = float(icon.rgba[v * 4 + 1]) / 128.0f;
            const float base_b = float(icon.rgba[v * 4 + 2]) / 128.0f;
            const float alpha_frac = clamp01(float(icon.rgba[v * 4 + 3]) / 128.0f);

            sr[k] = clamp01(base_r * total[0]) * 255.0f;
            sg[k] = clamp01(base_g * total[1]) * 255.0f;
            sb[k] = clamp01(base_b * total[2]) * 255.0f;
            sa[k] = alpha_frac * 255.0f;
        }
        fill_triangle_z(frags, render_dim, render_dim, sx, sy, sz, sr, sg, sb, sa);
    }

    std::vector<uint8_t> canvas(size_t(render_dim) * render_dim * 4);
    for (uint32_t y = 0; y < render_dim; ++y) {
        const float v = (float(y) + 0.5f) / float(render_dim);
        for (uint32_t x = 0; x < render_dim; ++x) {
            const float u = (float(x) + 0.5f) / float(render_dim);
            uint8_t bg[3];
            background_colour(bg_corners, u, v, bg);

            const Fragment& f = frags[size_t(y) * render_dim + x];
            uint8_t* dst = &canvas[(size_t(y) * render_dim + x) * 4];
            if (f.set) {
                const float a = clamp01(f.a / 255.0f);
                dst[0] = to_byte((f.r * a + float(bg[0]) * (1.0f - a)) / 255.0f);
                dst[1] = to_byte((f.g * a + float(bg[1]) * (1.0f - a)) / 255.0f);
                dst[2] = to_byte((f.b * a + float(bg[2]) * (1.0f - a)) / 255.0f);
            } else {
                dst[0] = bg[0];
                dst[1] = bg[1];
                dst[2] = bg[2];
            }
            dst[3] = 255;
        }
    }

    if (factor > 1) {
        box_filter_down(canvas, render_dim, factor, out.rgba, size_px);
    } else {
        out.rgba = std::move(canvas);
    }
    out.width = size_px;
    out.height = size_px;

    if (factor > 1) {
        rt_log("ui", "ps2 icon: rendered shape %u at %ux%u, box-filtered down from %ux%u (%ux),"
                    " bbox radius %.2f, camera distance %.2f",
            shape, size_px, size_px, render_dim, render_dim, factor, double(radius), double(dist));
    } else {
        rt_log("ui", "ps2 icon: rendered shape %u at %ux%u with no supersampling, bbox radius %.2f,"
                    " camera distance %.2f",
            shape, size_px, size_px, double(radius), double(dist));
    }
    return true;
} catch (const std::exception& e) {
    out = RtPs2IconImage();
    set_err(err, err_len, "ps2 icon render threw: %s", e.what());
    return false;
} catch (...) {
    out = RtPs2IconImage();
    set_err(err, err_len, "ps2 icon render threw a non-standard exception");
    return false;
}
