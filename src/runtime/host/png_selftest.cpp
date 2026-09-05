/* host/png_selftest.cpp: the PNG writer, on its own.
 *
 * host/png_write.cpp is what the screenshot path (host/screenshot.cpp), the
 * diagnostic dumps and the packaged icon tool all write their files with,
 * and it had no test. It is a pure function over a pixel buffer, so it can
 * have one that needs no window, no GS backend and no disc.
 *
 * What is checked, and why each one is here rather than being taken on
 * trust:
 *
 *   - The refusals. A zero dimension, a null pointer and a channel count
 *     that is not 3 or 4 all have to come back empty rather than emit a
 *     file no decoder will open, and rt_png_write has to say which.
 *   - The container. The eight-byte signature, then IHDR, IDAT and IEND in
 *     that order, every chunk's length and CRC-32 recomputed here from the
 *     chunk bytes, IEND empty and last, and nothing after it.
 *   - The header fields. Width, height, bit depth 8, and colour type 2 for
 *     three channels against 6 for four.
 *   - The pixels. The IDAT payload is a zlib stream of stored DEFLATE
 *     blocks; this inflates it with host/inflate.cpp, which is a separate
 *     implementation written for a different caller, and compares the
 *     result byte for byte against the filter-0 scanlines the input should
 *     have produced. That is the round trip: a writer that emitted the
 *     wrong stride, the wrong filter byte or the wrong block framing fails
 *     here.
 *   - The zlib wrapper's own arithmetic: the 0x78 0x01 header and the
 *     Adler-32 trailer, recomputed here.
 *   - More than one stored block. A stored DEFLATE block carries at most
 *     65535 bytes, so an image whose scanlines exceed that exercises the
 *     loop, the LEN/NLEN pair of each block and the final-block flag. The
 *     case below is 90100 raw bytes, which is two blocks.
 *   - rt_png_write itself: the file it leaves on disk is byte for byte what
 *     rt_png_encode returns, and an unwritable path is a false return with
 *     a reason rather than a silent drop.
 *
 * Scratch files go in the system temporary directory, so running this from
 * the repository root leaves nothing behind in the checkout.
 */
#include "png_write.h"
#include "inflate.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                              \
    } while (0)

/* CRC-32 as PNG defines it (RFC 1952's polynomial, the same one the writer
 * uses), computed here from scratch so the check is against the standard and
 * not against the writer's own table. */
uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return ~crc;
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

uint32_t be32(const std::vector<uint8_t>& v, size_t at) {
    return (uint32_t(v[at]) << 24) | (uint32_t(v[at + 1]) << 16) | (uint32_t(v[at + 2]) << 8) |
           uint32_t(v[at + 3]);
}

struct Chunk {
    std::string tag;
    size_t body_at = 0;
    size_t body_len = 0;
};

/* Walks the chunk list, checking every length and CRC on the way. Returns an
 * empty vector when the file is not a walkable PNG, which is itself a
 * failure the caller reports. */
std::vector<Chunk> walk(const std::vector<uint8_t>& png) {
    std::vector<Chunk> out;
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 8 || std::memcmp(png.data(), kSig, 8) != 0) return {};
    size_t at = 8;
    while (at + 12 <= png.size()) {
        const uint32_t len = be32(png, at);
        if (at + 12 + len > png.size()) return {};
        Chunk c;
        c.tag.assign(reinterpret_cast<const char*>(png.data()) + at + 4, 4);
        c.body_at = at + 8;
        c.body_len = len;
        /* The CRC covers the tag and the body, not the length. */
        const uint32_t want = crc32(png.data() + at + 4, 4 + len);
        if (want != be32(png, at + 8 + len)) return {};
        out.push_back(c);
        at += 12 + len;
    }
    if (at != png.size()) return {};
    return out;
}

/* The scanlines rt_png_encode should have compressed: one filter byte of 0
 * in front of every row of pixels. */
std::vector<uint8_t> expected_raw(uint32_t w, uint32_t h, const std::vector<uint8_t>& px, int ch) {
    std::vector<uint8_t> raw;
    const size_t stride = size_t(w) * size_t(ch);
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), px.begin() + size_t(y) * stride, px.begin() + size_t(y + 1) * stride);
    }
    return raw;
}

/* A deterministic image: every byte is a function of its position, so a
 * writer that transposed rows or dropped a channel comes out different. */
std::vector<uint8_t> make_pixels(uint32_t w, uint32_t h, int ch) {
    std::vector<uint8_t> px(size_t(w) * size_t(h) * size_t(ch));
    for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t((i * 37 + (i >> 8) * 11) & 0xFF);
    return px;
}

void check_image(const char* what, uint32_t w, uint32_t h, int ch) {
    std::printf("\n== %s: %ux%u, %d channels ==\n", what, w, h, ch);
    const std::vector<uint8_t> px = make_pixels(w, h, ch);
    const std::vector<uint8_t> png = rt_png_encode(w, h, px.data(), ch);
    CHECK(!png.empty());
    if (png.empty()) return;

    const std::vector<Chunk> chunks = walk(png);
    CHECK(chunks.size() == 3);
    if (chunks.size() != 3) return;
    CHECK(chunks[0].tag == "IHDR");
    CHECK(chunks[1].tag == "IDAT");
    CHECK(chunks[2].tag == "IEND");
    CHECK(chunks[2].body_len == 0);

    /* IHDR: width, height, bit depth, colour type, then three zeroes. */
    CHECK(chunks[0].body_len == 13);
    CHECK(be32(png, chunks[0].body_at) == w);
    CHECK(be32(png, chunks[0].body_at + 4) == h);
    CHECK(png[chunks[0].body_at + 8] == 8);
    CHECK(png[chunks[0].body_at + 9] == (ch == 4 ? 6 : 2));
    CHECK(png[chunks[0].body_at + 10] == 0);
    CHECK(png[chunks[0].body_at + 11] == 0);
    CHECK(png[chunks[0].body_at + 12] == 0);

    /* The zlib wrapper around the IDAT payload. */
    const uint8_t* idat = png.data() + chunks[1].body_at;
    const size_t idat_len = chunks[1].body_len;
    CHECK(idat_len > 6);
    CHECK(idat[0] == 0x78);
    CHECK(idat[1] == 0x01);

    const std::vector<uint8_t> want_raw = expected_raw(w, h, px, ch);
    CHECK(be32(png, chunks[1].body_at + idat_len - 4) == adler32(want_raw.data(), want_raw.size()));

    /* And the round trip, through a decoder that shares no code with the
     * writer. The raw DEFLATE stream is the payload without the two-byte
     * zlib header and the four-byte Adler trailer. */
    std::vector<uint8_t> got;
    char err[128] = {0};
    const bool ok = rt_inflate_raw(idat + 2, idat_len - 6, got, SIZE_MAX, 64u << 20, err, sizeof err);
    CHECK(ok);
    if (!ok) std::printf("     inflate said: %s\n", err);
    CHECK(got.size() == want_raw.size());
    CHECK(got == want_raw);

    /* The number of stored blocks the payload should hold, from the same
     * 65535-byte rule the writer follows. */
    const size_t blocks = (want_raw.size() + 65534) / 65535;
    size_t seen = 0, at = 2;
    while (at + 5 <= idat_len - 4) {
        const uint8_t hdr = idat[at];
        CHECK((hdr & 0x06) == 0); /* BTYPE 00, a stored block */
        const uint16_t len = uint16_t(idat[at + 1] | (uint16_t(idat[at + 2]) << 8));
        const uint16_t nlen = uint16_t(idat[at + 3] | (uint16_t(idat[at + 4]) << 8));
        CHECK(uint16_t(~uint32_t(len)) == nlen);
        ++seen;
        at += 5 + len;
        if (hdr & 1) break; /* BFINAL */
    }
    CHECK(seen == blocks);
    CHECK(at == idat_len - 4);
}

void test_refusals() {
    std::printf("\n== refusals ==\n");
    const std::vector<uint8_t> px = make_pixels(4, 4, 4);
    CHECK(rt_png_encode(4, 4, px.data(), 0).empty());
    CHECK(rt_png_encode(4, 4, px.data(), 1).empty());
    CHECK(rt_png_encode(4, 4, px.data(), 2).empty());
    CHECK(rt_png_encode(4, 4, px.data(), 5).empty());
    CHECK(rt_png_encode(0, 4, px.data(), 4).empty());
    CHECK(rt_png_encode(4, 0, px.data(), 4).empty());
    CHECK(rt_png_encode(4, 4, nullptr, 4).empty());

    /* And every one of them is a false return from the writer with a
     * reason, never a truncated file. */
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "icorecomp-png-refused.png";
    std::error_code ec;
    std::filesystem::remove(p, ec);
    char err[160];
    err[0] = 0;
    CHECK(!rt_png_write(p.string().c_str(), 4, 4, px.data(), 2, err, sizeof err));
    CHECK(err[0] != 0);
    err[0] = 0;
    CHECK(!rt_png_write(p.string().c_str(), 0, 4, px.data(), 4, err, sizeof err));
    CHECK(err[0] != 0);
    err[0] = 0;
    CHECK(!rt_png_write(p.string().c_str(), 4, 4, nullptr, 4, err, sizeof err));
    CHECK(err[0] != 0);
    /* Nothing was created by any of those. */
    CHECK(!std::filesystem::exists(p, ec));

    /* A path that cannot be opened is a false return with a reason too. */
    err[0] = 0;
    const std::filesystem::path bad =
        std::filesystem::temp_directory_path() / "icorecomp-png-no-such-dir" / "x.png";
    CHECK(!rt_png_write(bad.string().c_str(), 4, 4, px.data(), 4, err, sizeof err));
    CHECK(err[0] != 0);

    /* A null error buffer is legal: the caller may not want the text. */
    CHECK(!rt_png_write(bad.string().c_str(), 4, 4, px.data(), 4, nullptr, 0));
}

void test_file_matches_the_encoder() {
    std::printf("\n== rt_png_write writes what rt_png_encode returns ==\n");
    const uint32_t w = 7, h = 5;
    const std::vector<uint8_t> px = make_pixels(w, h, 3);
    const std::vector<uint8_t> want = rt_png_encode(w, h, px.data(), 3);
    CHECK(!want.empty());

    const std::filesystem::path p = std::filesystem::temp_directory_path() / "icorecomp-png-selftest.png";
    std::error_code ec;
    std::filesystem::remove(p, ec);
    char err[160];
    err[0] = 0;
    CHECK(rt_png_write(p.string().c_str(), w, h, px.data(), 3, err, sizeof err));
    CHECK(err[0] == 0);

    std::vector<uint8_t> got;
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    CHECK(f != nullptr);
    if (f) {
        uint8_t buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) got.insert(got.end(), buf, buf + n);
        std::fclose(f);
    }
    CHECK(got == want);
    std::filesystem::remove(p, ec);
}

} // namespace

int main() {
    test_refusals();
    /* Three channels and four, both small enough for one stored block. */
    check_image("RGB", 7, 5, 3);
    check_image("RGBA", 16, 9, 4);
    /* 300 * 3 + 1 = 901 bytes a row, 100 rows: 90100 raw bytes, two stored
     * blocks. */
    check_image("RGB, more than one stored block", 300, 100, 3);
    /* A single pixel, the smallest encodable image. */
    check_image("one pixel", 1, 1, 4);
    test_file_matches_the_encoder();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) std::printf("png-selftest: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
