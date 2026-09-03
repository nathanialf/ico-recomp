/* ui/title_logo_selftest.cpp: standalone check of the title-logo pipeline.
 *
 * Mounts the disc image the runtime would mount, reads the three letter
 * meshes and the title animation out of DFDATAS/DATA.DF, reduces them to the
 * wordmark's geometry, rasterises it and writes the result to a PNG so the
 * layout can be looked at without running the game. Then builds a second time
 * to exercise the cache-hit path.
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

#include "../host/portable.h"
#include "../iso/iso9660.h"
#include "../runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* loader.cpp and log.cpp reference the guest page table; nothing here loads
 * an ELF or dumps registers, so an empty one satisfies the link. The IPU
 * selftest does the same for the same reason. */
uint8_t* g_pages[0x10000];

namespace {

/* ---- a minimal PNG writer ------------------------------------------------
 *
 * Stored (uncompressed) DEFLATE blocks inside the zlib wrapper, which is
 * legal and needs no compressor. This is a diagnostic image, so the file
 * being three times larger than it has to be does not matter. */

uint32_t crc32_of(const uint8_t* data, size_t len, uint32_t crc) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void push_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

void push_chunk(std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& body) {
    push_be32(out, uint32_t(body.size()));
    const size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), body.begin(), body.end());
    push_be32(out, crc32_of(out.data() + start, out.size() - start, 0));
}

/* `rgb` is width * height * 3 bytes. */
bool write_png(const char* path, uint32_t width, uint32_t height, const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (1 + size_t(width) * 3));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); /* filter type 0 */
        raw.insert(raw.end(), rgb.begin() + size_t(y) * width * 3,
                   rgb.begin() + size_t(y + 1) * width * 3);
    }

    std::vector<uint8_t> z;
    z.push_back(0x78); /* zlib header, deflate, 32K window */
    z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        const uint16_t n = uint16_t(std::min<size_t>(65535, raw.size() - pos));
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    push_be32(z, (b << 16) | a);

    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    push_be32(ihdr, width);
    push_be32(ihdr, height);
    ihdr.push_back(8); /* bit depth */
    ihdr.push_back(2); /* colour type 2: truecolour */
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    push_chunk(out, "IHDR", ihdr);
    push_chunk(out, "IDAT", z);
    push_chunk(out, "IEND", {});

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
    return std::fclose(f) == 0 && ok;
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
    if (!write_png(out_path.c_str(), logo.width, logo.height, rgb)) {
        std::fprintf(stderr, "title-logo selftest: cannot write %s\n", out_path.c_str());
        return 1;
    }
    std::printf("wrote %s\n", out_path.c_str());

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
