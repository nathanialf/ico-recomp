/* host/inflate.cpp: raw DEFLATE decoder (RFC 1951). Written for this project.
 *
 * Three block types (stored, fixed Huffman, dynamic Huffman) and LZ77 back
 * references copied byte by byte, so that the overlapping runs DEFLATE relies
 * on (distance 1, length 258) come out right.
 *
 * Huffman codes are decoded through a first-level lookup table indexed by the
 * next kFastBits input bits, with a per-length slow path for the codes that do
 * not fit in it. The canonical assignment of codes to symbols follows RFC 1951
 * section 3.2.2, steps 1 to 3: count how many codes exist at each length, turn
 * those counts into the smallest code of each length, then hand codes out in
 * symbol order.
 *
 * The one caller inflates about 8 MB once per disc, cached afterwards.
 */
#include "inflate.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

void set_err(char* err, size_t err_len, const char* fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* Width of the first-level lookup. 9 bits covers every code in the fixed
 * literal/length table and the great majority of the dynamic ones, at 1 KB of
 * table per code. */
constexpr int kFastBits = 9;

/* A decoding table for one canonical Huffman code.
 *
 * `fast` is the first level, indexed by the next kFastBits bits of input as
 * they arrive. DEFLATE writes a code most significant bit first while the
 * stream delivers bits least significant first, so a code of length L reaches
 * the reader bit-reversed, and every index whose low L bits equal that
 * reversal decodes to the same symbol; the build fills all of them. An entry
 * is (symbol << 4) | length, and 0 means no code of length kFastBits or fewer
 * starts here, because a present symbol always has a length of at least 1.
 *
 * The rest is the second level, walked one bit at a time from kFastBits + 1
 * up: `first_code[len]` is the smallest code of that length in the order the
 * RFC assigns them, and `first_index[len]` where that length's symbols begin
 * in `symbol`. */
struct Huffman {
    uint16_t fast[1 << kFastBits];
    uint32_t first_code[16];
    uint16_t first_index[16];
    uint16_t count[16];
    uint16_t symbol[288];
    int max_len;
};

uint32_t reverse_bits(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; ++i) {
        r = (r << 1) | (v & 1u);
        v >>= 1;
    }
    return r;
}

/* Builds both levels from a code-length array. A length of 0 means the symbol
 * is absent. Rejects a set that cannot be assigned canonical codes, which is
 * what over-subscription means here: at some length the codes run past the
 * 2^len that exist at it. An incomplete set is allowed, because a dynamic
 * block with a single distance code is legal; the indices it leaves empty stay
 * 0 and huffman_decode reports the hole if one is ever reached. */
bool huffman_build(Huffman& h, const uint8_t* lengths, uint16_t n) {
    if (n > 288) return false;
    std::memset(&h, 0, sizeof(h));

    for (uint16_t i = 0; i < n; ++i) {
        if (lengths[i] > 15) return false;
        ++h.count[lengths[i]];
    }
    if (h.count[0] == n) return true; /* no symbols at all */
    h.count[0] = 0;                   /* absent symbols are not a length */

    /* RFC 1951 section 3.2.2 step 2: the smallest code of each length is the
     * smallest of the previous length plus that length's count, shifted up by
     * one. The set is assignable exactly when no length runs out of codes. */
    uint32_t next_code[16] = {0};
    uint32_t code = 0;
    uint16_t index = 0;
    for (int len = 1; len <= 15; ++len) {
        code = (code + h.count[len - 1]) << 1;
        if (code + h.count[len] > (1u << len)) return false;
        next_code[len] = code;
        h.first_code[len] = code;
        h.first_index[len] = index;
        index = uint16_t(index + h.count[len]);
        if (h.count[len] != 0) h.max_len = len;
    }

    /* Step 3: codes go to symbols in symbol order within each length, which is
     * also the order `symbol` holds them in. */
    uint16_t at[16];
    for (int len = 0; len < 16; ++len) at[len] = h.first_index[len];
    for (uint16_t i = 0; i < n; ++i) {
        const int len = lengths[i];
        if (len == 0) continue;
        const uint32_t c = next_code[len]++;
        h.symbol[at[len]++] = i;
        if (len <= kFastBits) {
            const uint16_t entry = uint16_t((uint32_t(i) << 4) | uint32_t(len));
            const uint32_t step = 1u << len;
            for (uint32_t slot = reverse_bits(c, len); slot < (1u << kFastBits); slot += step) {
                h.fast[slot] = entry;
            }
        }
    }
    return true;
}

/* LSB-first bit reader over the compressed input.
 *
 * peek() has to be able to look kFastBits ahead even when fewer bits remain,
 * so the accumulator is padded with zeros past the end of the input and `pad`
 * counts how many of the bits it holds are fabricated. Consuming one of those
 * is what latches `ok` false, so the callers can check once per symbol instead
 * of once per bit. */
struct BitReader {
    const uint8_t* data;
    size_t len;
    size_t pos = 0;    /* next byte */
    uint32_t bits = 0; /* bit accumulator, LSB first */
    int held = 0;      /* bits currently in the accumulator */
    int pad = 0;       /* how many of those are fabricated zeros, at the top */
    bool ok = true;

    void fill(int n) {
        while (held < n) {
            uint32_t b = 0;
            if (pos < len) {
                b = data[pos++];
            } else {
                pad += 8;
            }
            bits |= b << held;
            held += 8;
        }
    }

    uint32_t peek(int n) {
        fill(n);
        return bits & ((1u << n) - 1u);
    }

    void drop(int n) {
        bits >>= n;
        held -= n;
        if (held < pad) {
            ok = false;
            pad = held > 0 ? held : 0;
        }
    }

    uint32_t get(int n) {
        if (n == 0) return 0;
        fill(n);
        const uint32_t v = bits & ((1u << n) - 1u);
        drop(n);
        return ok ? v : 0;
    }

    /* Byte alignment for a stored block: hand back the whole bytes still in
     * the accumulator and discard the part-byte below them. */
    void align() {
        const int real = held - pad;
        if (real > 0) pos -= size_t(real / 8);
        bits = 0;
        held = 0;
        pad = 0;
    }
};

/* One symbol: a table lookup for the common case, then the canonical walk for
 * anything longer than the table's index. Returns -1 for a code that is not in
 * the table and for a read that ran off the end of the input. */
int huffman_decode(BitReader& br, const Huffman& h) {
    const uint32_t look = br.peek(kFastBits);
    const uint16_t entry = h.fast[look];
    if (entry != 0) {
        br.drop(int(entry & 15u));
        return br.ok ? int(entry >> 4) : -1;
    }

    /* No code of kFastBits or fewer starts with these bits, so all kFastBits
     * of them are consumed and the code is rebuilt from them most significant
     * bit first, then extended one bit at a time. */
    uint32_t code = 0;
    for (int i = 0; i < kFastBits; ++i) code = (code << 1) | ((look >> i) & 1u);
    br.drop(kFastBits);
    if (!br.ok) return -1;

    for (int len = kFastBits + 1; len <= h.max_len; ++len) {
        code = (code << 1) | br.get(1);
        if (!br.ok) return -1;
        const uint32_t offset = code - h.first_code[len]; /* wraps below the range */
        if (offset < h.count[len]) return h.symbol[h.first_index[len] + offset];
    }
    return -1;
}

/* RFC 1951 section 3.2.5: the length and distance code tables. Data from the
 * specification, not from any implementation. */
const uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                  31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* Section 3.2.7: the order the code-length code's own lengths are stored in. */
const uint8_t kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* Section 3.2.6: the fixed code. Both tables are assignable by construction,
 * but the build's answer is checked rather than assumed. */
bool build_fixed(Huffman& lit, Huffman& dist) {
    uint8_t lengths[288];
    for (int i = 0; i < 144; ++i) lengths[i] = 8;
    for (int i = 144; i < 256; ++i) lengths[i] = 9;
    for (int i = 256; i < 280; ++i) lengths[i] = 7;
    for (int i = 280; i < 288; ++i) lengths[i] = 8;
    if (!huffman_build(lit, lengths, 288)) return false;
    for (int i = 0; i < 30; ++i) lengths[i] = 5;
    return huffman_build(dist, lengths, 30);
}

} // namespace

bool rt_inflate_raw(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out, size_t out_limit,
                    size_t out_ceiling, char* err, size_t err_len) {
    out.clear();
    if (out_limit == 0) return true;
    if (!in || in_len == 0) {
        set_err(err, err_len, "empty deflate stream");
        return false;
    }

    BitReader br{in, in_len};
    Huffman fixed_lit, fixed_dist;
    if (!build_fixed(fixed_lit, fixed_dist)) {
        set_err(err, err_len, "the fixed Huffman code could not be built");
        return false;
    }

    for (;;) {
        const uint32_t final_block = br.get(1);
        const uint32_t type = br.get(2);
        if (!br.ok) {
            set_err(err, err_len, "deflate stream ends inside a block header at byte %zu", br.pos);
            return false;
        }

        if (type == 0) {
            /* Stored: skip to the byte boundary, then LEN and its complement. */
            br.align();
            if (br.pos + 4 > br.len) {
                set_err(err, err_len, "stored block header runs past the end of the stream");
                return false;
            }
            const uint32_t len = uint32_t(in[br.pos]) | (uint32_t(in[br.pos + 1]) << 8);
            const uint32_t nlen = uint32_t(in[br.pos + 2]) | (uint32_t(in[br.pos + 3]) << 8);
            br.pos += 4;
            if ((len ^ 0xFFFFu) != nlen) {
                set_err(err, err_len, "stored block length %u does not match its complement %u", len, nlen);
                return false;
            }
            if (br.pos + len > br.len) {
                set_err(err, err_len, "stored block of %u bytes runs past the end of the stream", len);
                return false;
            }
            if (len > out_ceiling - out.size()) {
                set_err(err, err_len, "the stream expands past the %zu-byte ceiling", out_ceiling);
                return false;
            }
            out.insert(out.end(), in + br.pos, in + br.pos + len);
            br.pos += len;
        } else if (type == 1 || type == 2) {
            Huffman dyn_lit, dyn_dist;
            const Huffman* lit = &fixed_lit;
            const Huffman* dist = &fixed_dist;

            if (type == 2) {
                const uint32_t hlit = br.get(5) + 257;
                const uint32_t hdist = br.get(5) + 1;
                const uint32_t hclen = br.get(4) + 4;
                if (!br.ok) {
                    set_err(err, err_len, "dynamic block header is truncated");
                    return false;
                }
                if (hlit > 286 || hdist > 30) {
                    set_err(err, err_len, "dynamic block declares %u literal and %u distance codes", hlit, hdist);
                    return false;
                }

                uint8_t cl_lengths[19] = {0};
                for (uint32_t i = 0; i < hclen; ++i) cl_lengths[kCodeLengthOrder[i]] = uint8_t(br.get(3));
                if (!br.ok) {
                    set_err(err, err_len, "code-length code is truncated");
                    return false;
                }
                Huffman cl;
                if (!huffman_build(cl, cl_lengths, 19)) {
                    set_err(err, err_len, "code-length code is over-subscribed");
                    return false;
                }

                /* The literal and distance lengths are stored as one run,
                 * with 16/17/18 repeating. */
                uint8_t lengths[288 + 32] = {0};
                uint32_t n = 0;
                while (n < hlit + hdist) {
                    const int sym = huffman_decode(br, cl);
                    if (sym < 0) {
                        set_err(err, err_len, "bad code-length symbol at output byte %zu", out.size());
                        return false;
                    }
                    if (sym < 16) {
                        lengths[n++] = uint8_t(sym);
                        continue;
                    }
                    uint32_t repeat = 0;
                    uint8_t value = 0;
                    if (sym == 16) {
                        if (n == 0) {
                            set_err(err, err_len, "code length repeat with no previous length");
                            return false;
                        }
                        value = lengths[n - 1];
                        repeat = 3 + br.get(2);
                    } else if (sym == 17) {
                        repeat = 3 + br.get(3);
                    } else {
                        repeat = 11 + br.get(7);
                    }
                    if (!br.ok) {
                        set_err(err, err_len, "code length repeat is truncated");
                        return false;
                    }
                    if (n + repeat > hlit + hdist) {
                        set_err(err, err_len, "code length repeat overruns the declared code counts");
                        return false;
                    }
                    while (repeat--) lengths[n++] = value;
                }

                if (lengths[256] == 0) {
                    set_err(err, err_len, "dynamic block has no end-of-block code");
                    return false;
                }
                if (!huffman_build(dyn_lit, lengths, uint16_t(hlit)) ||
                    !huffman_build(dyn_dist, lengths + hlit, uint16_t(hdist))) {
                    set_err(err, err_len, "dynamic block Huffman code is over-subscribed");
                    return false;
                }
                lit = &dyn_lit;
                dist = &dyn_dist;
            }

            for (;;) {
                const int sym = huffman_decode(br, *lit);
                if (sym < 0) {
                    set_err(err, err_len, "bad literal/length code at output byte %zu", out.size());
                    return false;
                }
                if (sym < 256) {
                    if (out.size() >= out_ceiling) {
                        set_err(err, err_len, "the stream expands past the %zu-byte ceiling", out_ceiling);
                        return false;
                    }
                    out.push_back(uint8_t(sym));
                    /* Tested here rather than only at the block boundary: a
                     * single dynamic block can carry the whole of a large
                     * stream, so neither the caller's prefix limit nor the
                     * ceiling can wait for the block to end. */
                    if (out.size() >= out_limit) return true;
                    continue;
                }
                if (sym == 256) break; /* end of block */

                const int li = sym - 257;
                if (li >= 29) {
                    set_err(err, err_len, "reserved length code %d at output byte %zu", sym, out.size());
                    return false;
                }
                const uint32_t length = kLengthBase[li] + br.get(kLengthExtra[li]);

                const int di = huffman_decode(br, *dist);
                if (di < 0 || di >= 30) {
                    set_err(err, err_len, "bad distance code at output byte %zu", out.size());
                    return false;
                }
                const uint32_t distance = kDistBase[di] + br.get(kDistExtra[di]);
                if (!br.ok) {
                    set_err(err, err_len, "stream ends inside a back reference at output byte %zu", out.size());
                    return false;
                }
                if (distance > out.size()) {
                    set_err(err, err_len, "back reference of %u reaches before the start of the output"
                                          " at output byte %zu",
                        distance, out.size());
                    return false;
                }
                if (length > out_ceiling - out.size()) {
                    set_err(err, err_len, "the stream expands past the %zu-byte ceiling", out_ceiling);
                    return false;
                }

                /* Byte at a time: DEFLATE runs may overlap the bytes they are
                 * still producing (distance 1 is a run fill), so memcpy of the
                 * whole span would be wrong. The byte is copied out before the
                 * push_back because the push can reallocate. */
                const size_t src = out.size() - distance;
                out.reserve(out.size() + length);
                for (uint32_t i = 0; i < length; ++i) {
                    const uint8_t b = out[src + i];
                    out.push_back(b);
                }
                if (out.size() >= out_limit) return true;
            }
        } else {
            set_err(err, err_len, "reserved block type 3 at output byte %zu", out.size());
            return false;
        }

        if (out.size() >= out_limit) return true; /* caller only wanted a prefix */
        if (final_block) return true;
    }
}
