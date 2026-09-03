/* host/png_write.cpp: see png_write.h. */
#include "png_write.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>

namespace {

void set_err(char* err, size_t err_len, const char* fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* Built once, on first use. A function-local static with a dynamic
 * initialiser is thread-safe by the language rule; the hand-rolled
 * "built" flag it replaces was not, and this writer is now shared. */
const std::array<uint32_t, 256>& crc_table() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

uint32_t crc32_of(const uint8_t* data, size_t len, uint32_t crc) {
    const std::array<uint32_t, 256>& table = crc_table();
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

} // namespace

std::vector<uint8_t> rt_png_encode(uint32_t width, uint32_t height, const uint8_t* pixels, int channels) {
    if (channels != 3 && channels != 4) return {};
    /* A zero dimension would emit a zlib stream with no deflate block in it
     * at all, which is not a decodable PNG; a null pixel pointer would be
     * read anyway. Both are caller mistakes, not encodable images. */
    if (width == 0 || height == 0 || !pixels) return {};

    const size_t stride = size_t(width) * size_t(channels);
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (1 + stride));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); /* filter type 0 (none) a row */
        raw.insert(raw.end(), pixels + size_t(y) * stride, pixels + size_t(y + 1) * stride);
    }

    std::vector<uint8_t> z;
    z.push_back(0x78); /* zlib header, deflate, 32K window */
    z.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        const uint16_t n = uint16_t(std::min<size_t>(65535, raw.size() - pos));
        const uint16_t n_inv = uint16_t(~uint32_t(n));
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        /* NLEN, the one's complement of LEN, as a u16: computed in unsigned
         * arithmetic so the high byte does not come from a right shift of a
         * negative int, which is what promoting a uint16_t through ~ gives. */
        z.push_back(uint8_t(n_inv));
        z.push_back(uint8_t(n_inv >> 8));
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
    ihdr.push_back(channels == 4 ? 6 : 2); /* colour type: 6 truecolour+alpha, 2 truecolour */
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    push_chunk(out, "IHDR", ihdr);
    push_chunk(out, "IDAT", z);
    push_chunk(out, "IEND", {});
    return out;
}

bool rt_png_write(const char* path, uint32_t width, uint32_t height, const uint8_t* pixels, int channels,
                  char* err, size_t err_len) {
    if (channels != 3 && channels != 4) {
        set_err(err, err_len, "rt_png_write: channels must be 3 or 4, got %d", channels);
        return false;
    }
    const std::vector<uint8_t> png = rt_png_encode(width, height, pixels, channels);
    if (png.empty()) {
        set_err(err, err_len, "rt_png_write: cannot encode %ux%u%s (a dimension is zero, or the"
                              " pixel pointer is null)",
            width, height, pixels ? "" : " from a null pointer");
        return false;
    }
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        set_err(err, err_len, "cannot open '%s' for writing", path);
        return false;
    }
    const bool wrote = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) {
        set_err(err, err_len, "write to '%s' failed", path);
        return false;
    }
    return true;
}
