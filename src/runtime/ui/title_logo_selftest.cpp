/* ui/title_logo_selftest.cpp: standalone check of the title-logo pipeline.
 *
 * Mounts the disc image the runtime would mount, reads the three letter
 * meshes and the title animation out of DFDATAS/DATA.DF, reduces them to the
 * wordmark's geometry, rasterises it and writes the result to a PNG so the
 * layout can be looked at without running the game. Then builds a second time
 * to exercise the cache-hit path.
 *
 * It also checks ui/cursor_image.cpp, which cuts the drawn menu cursor out of
 * that same image. That half runs first and against synthetic glyphs, so it
 * needs no disc and its answers are known rather than looked at: which
 * component gets picked, which end is found to be the point, and what happens
 * to an image with nothing in it. The real wordmark is then put through the
 * same call and its cursor written next to the first PNG, for the one thing a
 * synthetic image cannot answer, which is whether it looks like a cursor.
 *
 * Links the ISO reader, the loader (for rt_base_dir and the config), the log
 * and the DEFLATE decoder. No RmlUi, no SDL, no GS: nothing here draws.
 *
 *   icorecomp-title-logo-selftest <out.png> [surface-height] [keep-cache]
 *
 * The output path is required, and it must be outside the repository: the PNG
 * is game-derived pixels. There is deliberately no default, because a default
 * writes them into whatever directory the run happened to start in. The
 * surface height picks the raster size the same way the launcher does, so
 * passing the height of a real window produces exactly the pixels that window
 * would show; it defaults to 1920, the height of the 2560x1920 overlay this
 * was last checked against. Any third argument keeps the cache instead of
 * dropping it, which is how to measure the warm path; without it the run
 * always starts cold.
 */
#include "title_logo.h"

#include "cursor_image.h"

#include "../host/png_write.h"
#include "../host/portable.h"
#include "../iso/iso9660.h"
#include "../runtime.h"
#include "recomp_api.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* loader.cpp and log.cpp reference the guest page table; nothing here loads
 * an ELF or dumps registers, so an empty one satisfies the link. The
 * recomp_api.h include above is required, not decorative: it declares
 * g_pages inside extern "C", and without it this definition gets plain C++
 * linkage, which MSVC mangles and GCC does not, leaving the loader unable
 * to find the symbol under MSVC (see guest/menu_nav_selftest.cpp, which
 * carries the same fix for the same reason). The IPU selftest defines an
 * equivalent empty array for the same underlying reason. */
uint8_t* g_pages[0x10000];

namespace {

/* PNG writing itself now lives in host/png_write.cpp (rt_png_write), shared
 * with ui/icon_extract.cpp.
 *
 * ---- the menu cursor, against synthetic glyphs ---------------------------
 *
 * One premultiplied RGBA image built by hand, so what the extraction is
 * supposed to pick is known before it runs. */

struct Synth {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;

    Synth(uint32_t width, uint32_t height) : w(width), h(height), px(size_t(width) * height * 4, 0) {}

    /* Inclusive of both corners, so the numbers in the cases below are the
     * glyph's own bounding box. Opaque, which for premultiplied bytes means
     * the colour goes in as it is. */
    void rect(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint8_t r, uint8_t g, uint8_t b) {
        for (uint32_t y = y0; y <= y1 && y < h; ++y) {
            for (uint32_t x = x0; x <= x1 && x < w; ++x) {
                uint8_t* p = &px[(size_t(y) * w + x) * 4];
                p[0] = r;
                p[1] = g;
                p[2] = b;
                p[3] = 255;
            }
        }
    }

    void flip_vertically() {
        std::vector<uint8_t> out(px.size());
        for (uint32_t y = 0; y < h; ++y) {
            std::memcpy(&out[size_t(y) * w * 4], &px[size_t(h - 1 - y) * w * 4], size_t(w) * 4);
        }
        px.swap(out);
    }
};

/* The three shapes case 1 puts in, left to right: a short red speck, the
 * white bar that is the answer, and a tall green blob. The bar is tapered
 * over its top eight rows, which is exactly the fifth of its height the tip
 * test averages over. */
Synth synth_three_shapes() {
    Synth s(80, 60);
    s.rect(2, 20, 4, 22, 255, 0, 0);    /* speck: 3 tall, leftmost, must be skipped */
    s.rect(10, 16, 17, 47, 255, 255, 255); /* bar: 8 wide, rows 16..47 */
    s.rect(13, 8, 14, 15, 255, 255, 255);  /* its taper: 2 wide, rows 8..15 */
    s.rect(40, 5, 69, 49, 0, 255, 0);   /* blob: 45 tall, the tallest component */
    return s;
}

/* The mean colour of the cursor's opaque pixels, which says which of the
 * three shapes above was picked. It is read as a balance rather than as a
 * level: the dark outline is opaque too and drags every channel down by the
 * same amount, so a white glyph stays neutral while a red or a green one
 * comes out with one channel well clear of the others. */
bool mean_solid_colour(const RtCursorImage& img, double rgb[3]) {
    double sum[3] = {0, 0, 0};
    size_t n = 0;
    for (size_t i = 0; i < img.rgba.size(); i += 4) {
        if (img.rgba[i + 3] < 250) continue;
        for (int c = 0; c < 3; ++c) sum[c] += double(img.rgba[i + c]);
        ++n;
    }
    if (n == 0) return false;
    for (int c = 0; c < 3; ++c) rgb[c] = sum[c] / double(n);
    return true;
}

bool cursor_synthetic_checks() {
    char err[256];

    /* 1: the leftmost component that is tall enough, not the leftmost one and
     * not the tallest one, and its tip is the narrow end. */
    const Synth three = synth_three_shapes();
    RtCursorImage a;
    if (!rt_cursor_image_build(three.px.data(), three.w, three.h, 1.0f, a, err, sizeof(err))) {
        std::fprintf(stderr, "cursor selftest: the three-shape image built nothing: %s\n", err);
        return false;
    }
    double mean[3];
    if (!mean_solid_colour(a, mean)) {
        std::fprintf(stderr, "cursor selftest: the cursor has no solid pixels\n");
        return false;
    }
    const double spread = std::max(mean[0], std::max(mean[1], mean[2])) -
                          std::min(mean[0], std::min(mean[1], mean[2]));
    if (spread > 15.0 || mean[0] < 100.0) {
        std::fprintf(stderr, "cursor selftest: the opaque pixels average (%.0f, %.0f, %.0f); the"
                             " white bar was not the component that got picked\n",
            mean[0], mean[1], mean[2]);
        return false;
    }
    if (!a.tip_at_top) {
        std::fprintf(stderr, "cursor selftest: the tapered end is the top one, but the tip came"
                             " out at the bottom\n");
        return false;
    }
    if (a.light_outline) {
        std::fprintf(stderr, "cursor selftest: a white glyph must get the dark outline\n");
        return false;
    }
    if (a.hotspot_x >= a.width || a.hotspot_y >= a.height) {
        std::fprintf(stderr, "cursor selftest: hotspot (%u, %u) is outside the %ux%u image\n",
            a.hotspot_x, a.hotspot_y, a.width, a.height);
        return false;
    }
    /* The hotspot is the tip, so the glyph has to be under it. A 3x3 window
     * covers the half-pixel the resample can spread the point over. */
    uint32_t around = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const int64_t x = int64_t(a.hotspot_x) + dx, y = int64_t(a.hotspot_y) + dy;
            if (x < 0 || y < 0 || x >= int64_t(a.width) || y >= int64_t(a.height)) continue;
            if (a.rgba[(size_t(y) * a.width + size_t(x)) * 4 + 3] != 0) ++around;
        }
    }
    if (around == 0) {
        std::fprintf(stderr, "cursor selftest: nothing is drawn at the hotspot (%u, %u)\n",
            a.hotspot_x, a.hotspot_y);
        return false;
    }
    std::printf("cursor: three shapes -> %ux%u, hotspot (%u, %u), tip at the top, dark outline\n",
        a.width, a.height, a.hotspot_x, a.hotspot_y);

    /* Same image upside down: the same glyph, pointed at the other end. The
     * cursor still has to come out the same size, because the tip end is
     * turned upwards either way. */
    Synth flipped = synth_three_shapes();
    flipped.flip_vertically();
    RtCursorImage b;
    if (!rt_cursor_image_build(flipped.px.data(), flipped.w, flipped.h, 1.0f, b, err, sizeof(err))) {
        std::fprintf(stderr, "cursor selftest: the flipped image built nothing: %s\n", err);
        return false;
    }
    if (b.tip_at_top) {
        std::fprintf(stderr, "cursor selftest: the flipped image's taper is at the bottom, but the"
                             " tip came out at the top\n");
        return false;
    }
    if (b.width != a.width || b.height != a.height) {
        std::fprintf(stderr, "cursor selftest: the flipped image gave %ux%u, not %ux%u\n", b.width,
            b.height, a.width, a.height);
        return false;
    }
    std::printf("cursor: the same shapes flipped -> tip at the bottom, %ux%u\n", b.width, b.height);

    /* A bar with no taper: neither end is narrower, so the top is used. */
    Synth plain(40, 60);
    plain.rect(4, 8, 11, 47, 255, 255, 255);
    RtCursorImage c;
    if (!rt_cursor_image_build(plain.px.data(), plain.w, plain.h, 1.0f, c, err, sizeof(err))) {
        std::fprintf(stderr, "cursor selftest: the untapered bar built nothing: %s\n", err);
        return false;
    }
    if (!c.tip_at_top) {
        std::fprintf(stderr, "cursor selftest: an untapered bar has no pointed end and must take"
                             " the top, but the tip came out at the bottom\n");
        return false;
    }
    std::printf("cursor: an untapered bar -> tip at the top, %ux%u\n", c.width, c.height);

    /* A dark glyph gets a light outline. */
    Synth dark(40, 60);
    dark.rect(4, 8, 11, 47, 20, 20, 24);
    RtCursorImage d;
    if (!rt_cursor_image_build(dark.px.data(), dark.w, dark.h, 1.0f, d, err, sizeof(err))) {
        std::fprintf(stderr, "cursor selftest: the dark bar built nothing: %s\n", err);
        return false;
    }
    if (!d.light_outline) {
        std::fprintf(stderr, "cursor selftest: a dark glyph must get the light outline\n");
        return false;
    }
    std::printf("cursor: a dark glyph -> light outline\n");

    /* Density scales the whole thing: twice the ratio, about twice the size. */
    RtCursorImage e;
    if (!rt_cursor_image_build(three.px.data(), three.w, three.h, 2.0f, e, err, sizeof(err))) {
        std::fprintf(stderr, "cursor selftest: the 2x build failed: %s\n", err);
        return false;
    }
    const double grew = double(e.height) / double(a.height);
    if (grew < 1.8 || grew > 2.2) {
        std::fprintf(stderr, "cursor selftest: doubling the density took the height from %u to %u"
                             " (x%.2f)\n", a.height, e.height, grew);
        return false;
    }
    std::printf("cursor: density 2 -> %ux%u, hotspot (%u, %u)\n", e.width, e.height, e.hotspot_x,
        e.hotspot_y);

    /* The same input twice gives the same bytes: nothing here may depend on
     * anything but the image it is handed. */
    RtCursorImage again;
    if (!rt_cursor_image_build(three.px.data(), three.w, three.h, 1.0f, again, err, sizeof(err)) ||
        again.width != a.width || again.height != a.height || again.rgba != a.rgba ||
        again.hotspot_x != a.hotspot_x || again.hotspot_y != a.hotspot_y) {
        std::fprintf(stderr, "cursor selftest: two builds of the same image disagree\n");
        return false;
    }

    /* Nothing to cut out is a false return with a reason, not a crash and not
     * a blank image. */
    const Synth empty(16, 16);
    RtCursorImage none;
    if (rt_cursor_image_build(empty.px.data(), empty.w, empty.h, 1.0f, none, err, sizeof(err))) {
        std::fprintf(stderr, "cursor selftest: an empty image produced a cursor\n");
        return false;
    }
    std::printf("cursor: an empty image is refused (%s)\n", err);
    return true;
}

/* The cursor flattened over a mid grey, which is the one background both a
 * light glyph and its dark outline show up against. */
bool write_cursor_png(const std::string& path, const RtCursorImage& img) {
    std::vector<uint8_t> rgb(size_t(img.width) * img.height * 3);
    for (size_t i = 0, o = 0; i < img.rgba.size(); i += 4, o += 3) {
        const uint32_t a = img.rgba[i + 3];
        for (int c = 0; c < 3; ++c) {
            const uint32_t v = img.rgba[i + c] + 0x80u * (255u - a) / 255u;
            rgb[o + c] = uint8_t(v > 255 ? 255 : v);
        }
    }
    char err[256];
    if (!rt_png_write(path.c_str(), img.width, img.height, rgb.data(), 3, err, sizeof(err))) {
        std::fprintf(stderr, "title-logo selftest: %s\n", err);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    /* No default output path. The PNG is game-derived pixels and the obvious
     * default, the working directory, is the repository root often enough to
     * have put one in the tracked tree once already. */
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out.png> [surface-height] [keep-cache]\n"
                             "the output path is required and must be outside the repository:"
                             " the PNG is game-derived pixels\n",
            argv[0] ? argv[0] : "icorecomp-title-logo-selftest");
        return 2;
    }
    const std::string out_path = argv[1];
    const unsigned surface_height = argc > 2 ? unsigned(std::strtoul(argv[2], nullptr, 10)) : 1920u;

    /* Same rule as density_for() in ui/ui.cpp: the documents are authored
     * against a 640x480 surface, and the ratio is clamped to [1, 4]. */
    float dp_ratio = surface_height ? float(surface_height) / 480.0f : 1.0f;
    dp_ratio = std::min(std::max(dp_ratio, 1.0f), 4.0f);
    uint32_t box_w = 0, box_h = 0;
    rt_title_logo_pixel_box(dp_ratio, &box_w, &box_h);
    std::printf("surface height %u -> dp ratio %.3f -> raster box %ux%u (%ux%u dp)\n", surface_height,
        double(dp_ratio), box_w, box_h, kRtTitleLogoDpWidth, kRtTitleLogoDpHeight);

    /* The cursor's own checks first, and against images built here: they
     * need no disc, so a machine with no image still runs them. */
    if (!cursor_synthetic_checks()) return 1;

    char err[1024];
    if (!rt_iso_probe_mount(err, sizeof(err))) {
        std::fprintf(stderr, "title-logo selftest: no disc: %s\n", err);
        return 1;
    }
    std::printf("disc: %s (%s), %u sectors of %u bytes\n", rt_iso_mounted_path(),
        rt_iso_mounted_source(), rt_iso_total_sectors(), rt_iso_sector_size());

    /* Drop any cache first, so this run measures the extraction and not a
     * previous run's result, unless the caller asked to keep it. */
    const std::string cache = rt_title_logo_cache_path();
    if (argc > 3) {
        std::printf("keeping the cache at %s\n", cache.c_str());
    } else if (!cache.empty()) {
        std::remove(cache.c_str());
    }

    RtTitleLogo logo;
    if (!rt_title_logo_build(box_w, box_h, logo, err, sizeof(err))) {
        std::fprintf(stderr, "title-logo selftest: build failed: %s\n", err);
        return 1;
    }
    std::printf("raster: %ux%u, %zu bytes, wordmark aspect %.4f\n", logo.width, logo.height,
        logo.rgba.size(), double(rt_title_logo_aspect()));

    size_t opaque = 0;
    uint8_t peak = 0;
    for (size_t i = 0; i < logo.rgba.size(); i += 4) {
        if (logo.rgba[i + 3] != 0) ++opaque;
        if (logo.rgba[i + 3] > peak) peak = logo.rgba[i + 3];
    }
    std::printf("alpha: %zu of %zu pixels non-zero, peak %u\n", opaque, logo.rgba.size() / 4, peak);

    /* Flattened over the launcher panel's colour (#0d0d0d in base.rcss), so
     * the preview is what the launcher will show. The raster is premultiplied,
     * so the source term is added straight in. */
    std::vector<uint8_t> rgb(size_t(logo.width) * logo.height * 3);
    for (size_t i = 0, o = 0; i < logo.rgba.size(); i += 4, o += 3) {
        const uint32_t a = logo.rgba[i + 3];
        for (int c = 0; c < 3; ++c) {
            const uint32_t v = logo.rgba[i + c] + 0x0Du * (255u - a) / 255u;
            rgb[o + c] = uint8_t(v > 255 ? 255 : v);
        }
    }
    if (!rt_png_write(out_path.c_str(), logo.width, logo.height, rgb.data(), 3, err, sizeof(err))) {
        std::fprintf(stderr, "title-logo selftest: %s\n", err);
        return 1;
    }
    std::printf("wrote %s\n", out_path.c_str());

    /* The drawn menu cursor, cut out of the wordmark that was just built.
     * This is the only place the real glyph goes through it, so the numbers
     * it prints are the measurement: which end of the disc's own letter I is
     * the pointed one, and how big the cursor comes out at this density. */
    RtCursorImage cursor;
    if (!rt_cursor_image_build(logo.rgba.data(), logo.width, logo.height, dp_ratio, cursor, err,
                               sizeof(err))) {
        std::fprintf(stderr, "title-logo selftest: no cursor from the wordmark: %s\n", err);
        return 1;
    }
    std::printf("cursor from the disc's wordmark: %ux%u px, hotspot (%u, %u), tip at the %s,"
                " %s outline, %.1f dp tall at ratio %.3f\n",
        cursor.width, cursor.height, cursor.hotspot_x, cursor.hotspot_y,
        cursor.tip_at_top ? "top" : "bottom", cursor.light_outline ? "light" : "dark",
        double(cursor.height) / double(dp_ratio), double(dp_ratio));
    const std::string cursor_path = out_path + ".cursor.png";
    if (!write_cursor_png(cursor_path, cursor)) {
        std::fprintf(stderr, "title-logo selftest: cannot write %s\n", cursor_path.c_str());
        return 1;
    }
    std::printf("wrote %s\n", cursor_path.c_str());

    /* Edges must be hard: the GS draws these polygons with antialiasing off,
     * so every pixel is either fully transparent or fully opaque. A partial
     * alpha anywhere means coverage blending has crept back in. */
    for (size_t i = 3; i < logo.rgba.size(); i += 4) {
        if (logo.rgba[i] != 0 && logo.rgba[i] != 255) {
            std::fprintf(stderr, "title-logo selftest: pixel %zu has alpha %u; the raster is"
                                 " supposed to be hard edged\n", i / 4, logo.rgba[i]);
            return 1;
        }
    }
    std::printf("edges are hard (every pixel alpha 0 or 255)\n");

    /* Asking again at the same size must give the same pixels. */
    RtTitleLogo again;
    if (!rt_title_logo_build(box_w, box_h, again, err, sizeof(err))) {
        std::fprintf(stderr, "title-logo selftest: second build failed: %s\n", err);
        return 1;
    }
    if (again.width != logo.width || again.height != logo.height || again.rgba != logo.rgba) {
        std::fprintf(stderr, "title-logo selftest: the second build does not match the first\n");
        return 1;
    }

    /* And asking at another size must re-raster rather than scale, which is
     * what a window-scale change does. The geometry is already in memory, so
     * this touches no disc and should be well under a millisecond. */
    uint32_t half_w = 0, half_h = 0;
    rt_title_logo_pixel_box(dp_ratio * 0.5f, &half_w, &half_h);
    RtTitleLogo smaller;
    if (!rt_title_logo_build(half_w, half_h, smaller, err, sizeof(err))) {
        std::fprintf(stderr, "title-logo selftest: re-raster failed: %s\n", err);
        return 1;
    }
    if (smaller.width != half_w || smaller.height != half_h || smaller.width == logo.width) {
        std::fprintf(stderr, "title-logo selftest: re-raster gave %ux%u, wanted %ux%u\n",
            smaller.width, smaller.height, half_w, half_h);
        return 1;
    }
    std::printf("re-rastered at %ux%u without touching the disc\n", smaller.width, smaller.height);

    std::printf("cache written to %s\n", rt_title_logo_cache_path());
    return 0;
}
