/* sha1.cpp: self-contained SHA-1 (FIPS 180-4), used only to verify the ELF
 * SHA-1 pin from config/recomp.toml. No third-party dependency, no game
 * data: the algorithm is a public specification.
 */
#include "runtime.h"

#include <cstdio>
#include <cstring>

namespace {

struct Sha1State {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t total_len = 0;
    uint8_t buf[64];
    size_t buf_len = 0;
};

uint32_t rol32(uint32_t v, int bits) { return (v << bits) | (v >> (32 - bits)); }

void sha1_block(Sha1State& s, const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6u; }

        uint32_t temp = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = temp;
    }
    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d; s.h[4] += e;
}

void sha1_update(Sha1State& s, const uint8_t* data, size_t len) {
    s.total_len += len;
    while (len > 0) {
        size_t take = 64 - s.buf_len;
        if (take > len) take = len;
        std::memcpy(s.buf + s.buf_len, data, take);
        s.buf_len += take;
        data += take;
        len -= take;
        if (s.buf_len == 64) {
            sha1_block(s, s.buf);
            s.buf_len = 0;
        }
    }
}

Sha1Digest sha1_final(Sha1State& s) {
    uint64_t bit_len = s.total_len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    uint8_t zero = 0x00;
    while (s.buf_len != 56) sha1_update(s, &zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) len_be[i] = uint8_t(bit_len >> (56 - i * 8));
    /* Bypass the padding logic for the length field: append directly. */
    std::memcpy(s.buf + s.buf_len, len_be, 8);
    sha1_block(s, s.buf);
    s.buf_len = 0;

    Sha1Digest out{};
    for (int i = 0; i < 5; ++i) {
        out.bytes[i * 4 + 0] = uint8_t(s.h[i] >> 24);
        out.bytes[i * 4 + 1] = uint8_t(s.h[i] >> 16);
        out.bytes[i * 4 + 2] = uint8_t(s.h[i] >> 8);
        out.bytes[i * 4 + 3] = uint8_t(s.h[i]);
    }
    return out;
}

} // namespace

Sha1Digest rt_sha1_file(const char* path, bool* ok) {
    Sha1Digest digest{};
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        if (ok) *ok = false;
        return digest;
    }
    Sha1State state;
    uint8_t chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        sha1_update(state, chunk, n);
    }
    bool read_ok = !std::ferror(f);
    std::fclose(f);
    digest = sha1_final(state);
    if (ok) *ok = read_ok;
    return digest;
}

Sha1Digest rt_sha1_buffer(const uint8_t* data, size_t len) {
    Sha1State state;
    sha1_update(state, data, len);
    return sha1_final(state);
}

void rt_sha1_to_hex(const Sha1Digest& d, char* buf) {
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 20; ++i) {
        buf[i * 2] = hex[d.bytes[i] >> 4];
        buf[i * 2 + 1] = hex[d.bytes[i] & 0xF];
    }
    buf[40] = '\0';
}

bool rt_sha1_equals_hex(const Sha1Digest& d, const char* hex40) {
    char buf[41];
    rt_sha1_to_hex(d, buf);
    for (int i = 0; i < 40; ++i) {
        char a = buf[i];
        char b = hex40[i];
        if (b >= 'A' && b <= 'F') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}
