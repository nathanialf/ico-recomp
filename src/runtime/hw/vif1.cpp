/* hw/vif1.cpp: VIF1 command stream interpreter.
 *
 * Fed synchronously with 32-bit words from three sources: DMA ch1 payload
 * (dmac.cpp), TTE tag-word transfers (words 2-3 of each source-chain tag),
 * and direct FIFO stores to 0x10005000. A command whose whole payload is
 * inside one feed call executes straight out of the caller's buffer; one
 * that splits across feed calls has its payload buffered until it is
 * complete. Either way the command executes atomically.
 *
 * Effects:
 *   UNPACK        writes VU1 data memory (rt_vu1_state()->mem), with
 *                 STCYCL CL/WL skipping and filling cycles, STMASK/STROW/
 *                 STCOL masking, STMOD offset/difference modes, signed and
 *                 unsigned extension, V4-5 expansion, and the 16 KB wrap.
 *   MPG           writes the VU1 micro shadow (rt_vu1_micro()).
 *   DIRECT/HL     forwards qwords to GIF PATH2.
 *   MSCAL/F/CNT   dispatches through rt_vu1_mscal with xtop/itop from
 *                 TOPS/ITOPS and BASE/OFFSET double-buffer flipping.
 *   i-bit         raises INTC cause 5 (VIF1), delivered deferred.
 *
 * VIF0 (FIFO 0x10004000) is not interpreted: ICO only uses it for the SDK
 * reset sequence (STCYCL/STMASK/ITOP/STMOD/MARK); words are counted and
 * loud-logged, then discarded.
 *
 * All command encodings are public PS2 hardware documentation (ps2tek,
 * "VIFcode reference").
 */
#include "hw.h"

#include "../ee/kernel.h"
#include "../prof.h"

#include <cinttypes>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kVuQw = 1024; /* VU1 data memory in qwords */

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

/* One entry of the VIFcode history ring. index is the 1-based command
 * number (Vif1::cmds at the time the code word was taken), addr the guest
 * address the word came from or RT_VIF1_ADDR_NONE. */
struct CodeRec {
    uint32_t code;
    uint32_t addr;
    uint64_t index;
    uint32_t need_words;
};

struct Vif1 {
    /* Persistent register state. */
    uint32_t cl = 0, wl = 0;      /* STCYCL */
    uint32_t mask = 0;            /* STMASK */
    uint32_t mode = 0;            /* STMOD */
    uint32_t row[4] = {};         /* STROW */
    uint32_t col[4] = {};         /* STCOL */
    uint32_t itops = 0, itop = 0;
    uint32_t base = 0, ofst = 0;
    uint32_t tops = 0, top = 0;
    uint32_t mark = 0;
    uint32_t err = 0;
    bool dbf = false;
    bool mskpath3 = false;

    /* In-flight command. */
    bool pending = false;
    uint32_t code = 0;
    uint32_t need_words = 0;
    std::vector<uint32_t> payload;

    /* Stats. */
    uint64_t cmds = 0, unpacks = 0, directs = 0, mpgs = 0, mscals = 0;

    /* Diagnostics: the last 32 code words taken and the guest address of
     * the one currently executing. */
    CodeRec recent[32] = {};
    uint32_t cur_addr = RT_VIF1_ADDR_NONE;
};

Vif1 g_vif;
uint64_t g_vif0_words = 0;

uint32_t sext8(uint8_t v, bool usn) { return usn ? v : (uint32_t)(int32_t)(int8_t)v; }
uint32_t sext16(uint16_t v, bool usn) { return usn ? v : (uint32_t)(int32_t)(int16_t)v; }

/* ---- UNPACK ------------------------------------------------------------- */

/* Payload bytes one input vector occupies, and the unit
 * unpack_words_needed sizes the payload in: V4-5 is 16 bits whatever VN
 * says, every other format is (VN+1) fields of 32 >> VL bits. Every
 * format is a whole number of bytes, so no rounding is lost here. */
uint32_t unpack_vector_bytes(uint32_t vn, uint32_t vl) {
    return (vl == 3) ? 2u : (vn + 1) * (4u >> vl);
}

/* Lanes a decoded vector provides. S-xx broadcasts its single field over
 * all four, V4-5 always expands to four, every other format gives VN+1.
 * Lanes past this are the "the mask says data and the format gave none"
 * case the counter below records. */
uint32_t unpack_lanes(uint32_t vn, uint32_t vl) {
    return (vn == 0 || vl == 3) ? 4u : vn + 1;
}

/* Vectors an UNPACK reads out of the payload, the count
 * unpack_words_needed sizes the payload from. */
uint32_t unpack_input_vectors(uint32_t num, uint32_t cl, uint32_t wl) {
    if (wl <= cl) return num;
    const uint32_t rem = num % wl;
    return (num / wl) * cl + (rem < cl ? rem : cl);
}

/* Fields written with no source data behind them, whole run. Hardware
 * writes indeterminate data there; this model writes 0 and samples a log
 * line at every power of two, which is what the counter is for. */
uint64_t g_missing_field_data = 0;

void note_missing_field(uint32_t cmd, uint32_t f) {
    if (is_pow2(++g_missing_field_data)) {
        rt_log_warn("vif1", "UNPACK cmd=0x%02x writes field %u with no source data (mask says data) [#%" PRIu64 "]",
            cmd, f, g_missing_field_data);
    }
}

/* Bulk form of note_missing_field for the unmasked path, where the pattern
 * is fixed: each of `count` vectors writes lanes `lanes`..3 with no source
 * data, in ascending lane order. Produces the same counter values and the
 * same sampled lines as one call per field, without a branch per field. */
void note_missing_field_run(uint32_t cmd, uint32_t lanes, uint32_t count) {
    if (lanes >= 4 || count == 0) return;
    const uint32_t per = 4 - lanes;
    const uint64_t start = g_missing_field_data;
    const uint64_t end = start + (uint64_t)count * per;
    g_missing_field_data = end;
    uint64_t p = 1;
    while (p != 0 && p <= start) p <<= 1;
    for (; p != 0 && p <= end; p <<= 1) {
        rt_log_warn("vif1", "UNPACK cmd=0x%02x writes field %u with no source data (mask says data) [#%" PRIu64 "]",
            cmd, lanes + (uint32_t)((p - start - 1) % per), p);
    }
}

/* Decodes one input vector into four lanes. Lanes the format does not
 * provide are left at 0, which is what the caller writes for them when the
 * mask selects data. src must have unpack_vector_bytes(vn, vl) readable
 * bytes; exec_unpack guarantees that by padding a short payload. */
inline void decode_vector(const uint8_t* src, uint32_t vn, uint32_t vl, bool usn, uint32_t out[4]) {
    if (vl == 3) { /* V4-5, and the S-5 spelling that broadcasts lane 0 */
        uint16_t h;
        std::memcpy(&h, src, 2);
        out[0] = (uint32_t)(h & 0x1F) << 3;
        if (vn == 0) {
            out[1] = out[2] = out[3] = out[0];
            return;
        }
        out[1] = (uint32_t)((h >> 5) & 0x1F) << 3;
        out[2] = (uint32_t)((h >> 10) & 0x1F) << 3;
        out[3] = (uint32_t)((h >> 15) & 1) << 7;
        return;
    }
    const uint32_t nf = vn + 1;
    switch (vl) {
        case 0:
            std::memcpy(out, src, (size_t)nf * 4);
            break;
        case 1:
            for (uint32_t f = 0; f < nf; ++f) {
                uint16_t h;
                std::memcpy(&h, src + f * 2, 2);
                out[f] = sext16(h, usn);
            }
            break;
        default:
            for (uint32_t f = 0; f < nf; ++f) out[f] = sext8(src[f], usn);
            break;
    }
    if (vn == 0) out[1] = out[2] = out[3] = out[0];
    else for (uint32_t f = nf; f < 4; ++f) out[f] = 0;
}

/* Unmasked, STMOD 0, no filling: n input vectors into n consecutive
 * quadwords, no wrap inside the run (fill_block splits for that). Every
 * lane is a data lane, so lanes the format does not provide are written as
 * 0 exactly as the masked path writes them. */
inline void fill_run(uint32_t* d, const uint8_t* src, uint32_t n, uint32_t vn, uint32_t vl, bool usn) {
    if (n == 0) return;
    if (vl == 3) { /* V4-5 */
        for (uint32_t i = 0; i < n; ++i, d += 4) {
            uint16_t h;
            std::memcpy(&h, src + (size_t)i * 2, 2);
            const uint32_t a = (uint32_t)(h & 0x1F) << 3;
            if (vn == 0) {
                d[0] = d[1] = d[2] = d[3] = a;
                continue;
            }
            d[0] = a;
            d[1] = (uint32_t)((h >> 5) & 0x1F) << 3;
            d[2] = (uint32_t)((h >> 10) & 0x1F) << 3;
            d[3] = (uint32_t)((h >> 15) & 1) << 7;
        }
        return;
    }
    if (vn == 0) { /* S-32/S-16/S-8: one field broadcast over four lanes */
        for (uint32_t i = 0; i < n; ++i, d += 4) {
            uint32_t w;
            if (vl == 0) {
                std::memcpy(&w, src + (size_t)i * 4, 4);
            } else if (vl == 1) {
                uint16_t h;
                std::memcpy(&h, src + (size_t)i * 2, 2);
                w = sext16(h, usn);
            } else {
                w = sext8(src[i], usn);
            }
            d[0] = d[1] = d[2] = d[3] = w;
        }
        return;
    }
    const uint32_t nf = vn + 1; /* 2, 3 or 4 provided lanes */
    if (vl == 0) {
        if (nf == 4) { /* V4-32: the hot case, one copy for the whole run */
            std::memcpy(d, src, (size_t)n * 16);
            return;
        }
        /* V3-32 and V2-32: the provided lanes copied, the rest zeroed. */
        const size_t keep = (size_t)nf * 4;
        for (uint32_t i = 0; i < n; ++i, d += 4, src += keep) {
            std::memcpy(d, src, keep);
            for (uint32_t f = nf; f < 4; ++f) d[f] = 0;
        }
        return;
    }
    if (vl == 1) { /* 16-bit fields */
        if (nf == 4) {
            for (uint32_t i = 0; i < n; ++i, d += 4, src += 8) {
                uint16_t h[4];
                std::memcpy(h, src, 8);
                d[0] = sext16(h[0], usn);
                d[1] = sext16(h[1], usn);
                d[2] = sext16(h[2], usn);
                d[3] = sext16(h[3], usn);
            }
            return;
        }
        const size_t stride = (size_t)nf * 2;
        for (uint32_t i = 0; i < n; ++i, d += 4, src += stride) {
            for (uint32_t f = 0; f < nf; ++f) {
                uint16_t h;
                std::memcpy(&h, src + f * 2, 2);
                d[f] = sext16(h, usn);
            }
            for (uint32_t f = nf; f < 4; ++f) d[f] = 0;
        }
        return;
    }
    /* vl == 2: 8-bit fields */
    if (nf == 4) {
        for (uint32_t i = 0; i < n; ++i, d += 4, src += 4) {
            d[0] = sext8(src[0], usn);
            d[1] = sext8(src[1], usn);
            d[2] = sext8(src[2], usn);
            d[3] = sext8(src[3], usn);
        }
        return;
    }
    for (uint32_t i = 0; i < n; ++i, d += 4, src += nf) {
        for (uint32_t f = 0; f < nf; ++f) d[f] = sext8(src[f], usn);
        for (uint32_t f = nf; f < 4; ++f) d[f] = 0;
    }
}

/* One contiguous run of the unmasked path, split at the 1024-quadword end
 * of data memory. qw is already masked and n is at most 256, so a run
 * crosses the wrap at most once. */
inline void fill_block(uint8_t* mem, uint32_t qw, const uint8_t* src, uint32_t n,
                       uint32_t vn, uint32_t vl, bool usn) {
    uint32_t first = kVuQw - qw;
    if (first > n) first = n;
    fill_run(reinterpret_cast<uint32_t*>(mem + (size_t)qw * 16), src, first, vn, vl, usn);
    if (first < n) {
        const size_t stride = unpack_vector_bytes(vn, vl);
        fill_run(reinterpret_cast<uint32_t*>(mem), src + (size_t)first * stride,
            n - first, vn, vl, usn);
    }
}

/* pay/paywords is the command's payload: a span into the feeding buffer
 * when the whole command arrived in one feed call, otherwise the straddle
 * vector. */
void exec_unpack(Vif1& v, const uint32_t* pay, uint32_t paywords) {
    const uint32_t code = v.code;
    const uint32_t cmd = (code >> 24) & 0x7F;
    const uint32_t vn = (cmd >> 2) & 3;
    const uint32_t vl = cmd & 3;
    const bool masked = (cmd & 0x10) != 0;
    const bool usn = (code >> 14) & 1;
    const bool flg = (code >> 15) & 1;
    uint32_t num = (code >> 16) & 0xFF;
    if (num == 0) num = 256;
    uint32_t addr = code & 0x3FF;
    if (flg) addr = (addr + v.tops) & 0x3FF;

    uint32_t cl = v.cl, wl = v.wl;
    if (wl == 0) wl = 256;
    if (cl == 0 && wl > cl) {
        /* CL=0 with filling would consume no input at all; loud and clamp. */
        static uint64_t n = 0;
        if (is_pow2(++n)) rt_log_warn("vif1", "UNPACK with CL=0 (STCYCL=0x%02x%02x); clamping CL to 1", v.wl, v.cl);
        cl = 1;
    }
    const bool filling = wl > cl;
    static uint64_t fill_uses = 0;
    if (filling && is_pow2(++fill_uses)) {
        rt_log_debug("vif1", "UNPACK filling mode in use (CL=%u WL=%u) [#%" PRIu64 "]", cl, wl, fill_uses);
    }

    /* Source bytes. A payload shorter than the format needs read as zeroes
     * field by field; padding a copy up front keeps that result without a
     * bounds check on every field. The feed path always delivers exactly
     * unpack_words_needed words, so this only fires for a direct call. */
    const uint32_t vecbytes = unpack_vector_bytes(vn, vl);
    const uint32_t needbytes = unpack_input_vectors(num, cl, wl) * vecbytes;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pay);
    const uint32_t nbytes = paywords * 4;
    if (nbytes < needbytes) {
        static uint8_t padded[256 * 16]; /* 256 vectors of at most 16 bytes */
        if (nbytes) std::memcpy(padded, bytes, nbytes);
        std::memset(padded + nbytes, 0, needbytes - nbytes);
        bytes = padded;
    }

    Vu1State* vu = rt_vu1_state();
    const uint32_t lanes = unpack_lanes(vn, vl);

    if (!masked && v.mode == 0 && !filling) {
        /* No mask, no STMOD offset, and every write cycle takes input, so
         * each block of WL vectors lands in WL consecutive quadwords and
         * the whole command is a handful of contiguous runs. CL == WL is
         * the degenerate case where the blocks join into one run, which is
         * what ICO's STCYCL=0x0101 streams are. */
        const uint32_t step = (cl == wl) ? num : wl;
        uint32_t done = 0;
        uint32_t block = addr;
        while (done < num) {
            uint32_t n = num - done;
            if (n > step) n = step;
            fill_block(vu->mem, block & (kVuQw - 1), bytes + (size_t)done * vecbytes,
                n, vn, vl, usn);
            done += n;
            block = (block + cl) & (kVuQw - 1);
        }
        note_missing_field_run(cmd, lanes, num);
    } else {
        /* Masking, an STMOD offset or filling cycles. The four mask
         * selectors per cycle are unpacked once instead of per field per
         * vector, and the destination comes from a running (cycle, block
         * base) pair rather than a division by WL per vector. Only cycles
         * 0..3 have mask bits; cycle 3's selectors cover every later
         * cycle, so four plans cover any WL. */
        uint8_t op[4][4];
        for (uint32_t c = 0; c < 4; ++c) {
            for (uint32_t f = 0; f < 4; ++f) {
                op[c][f] = masked ? (uint8_t)((v.mask >> (c * 8 + f * 2)) & 3) : 0;
            }
        }
        const uint32_t colv[4] = {v.col[0], v.col[1], v.col[2], v.col[3]};
        const uint32_t mode = v.mode;

        uint32_t last_data[4] = {0, 0, 0, 0}; /* buffered input for filling cycles */
        uint32_t cursor = 0;                  /* byte cursor into the payload */
        uint32_t cycle = 0;                   /* k % wl, kept as a counter */
        uint32_t block = addr;                /* addr + (k / wl) * cl */
        uint32_t lin = addr;                  /* addr + k, filling mode */

        for (uint32_t k = 0; k < num; ++k) {
            const bool has_input = !filling || cycle < cl;
            uint32_t data[4];
            uint32_t nfields;
            if (has_input) {
                decode_vector(bytes + cursor, vn, vl, usn, data);
                cursor += vecbytes;
                nfields = lanes;
                /* Only a filling cycle ever reads this back. */
                if (filling) std::memcpy(last_data, data, sizeof(data));
            } else {
                std::memcpy(data, last_data, sizeof(data));
                nfields = 4;
            }

            /* Skipping mode: after WL writes, skip to the next CL
             * boundary. Filling mode: consecutive. */
            const uint32_t qw = filling ? (lin & (kVuQw - 1)) : ((block + cycle) & (kVuQw - 1));
            uint32_t* dst = reinterpret_cast<uint32_t*>(vu->mem + (size_t)qw * 16);

            const uint32_t mcycle = cycle < 3 ? cycle : 3;
            for (uint32_t f = 0; f < 4; ++f) {
                uint32_t out;
                switch (op[mcycle][f]) {
                    case 0: {
                        if (f >= nfields) {
                            /* Field not provided by this format and not
                             * masked: hardware writes indeterminate data.
                             * Write 0, loud once in a while. */
                            note_missing_field(cmd, f);
                            out = 0;
                            break;
                        }
                        out = data[f];
                        if (mode == 1) {
                            out += v.row[f];
                        } else if (mode == 2) {
                            out += v.row[f];
                            v.row[f] = out;
                        } else if (mode == 3) {
                            static uint64_t n = 0;
                            if (is_pow2(++n)) rt_log_warn("vif1", "UNPACK with undefined STMOD=3; treating as 0 [#%" PRIu64 "]", n);
                        }
                        break;
                    }
                    case 1: out = v.row[f]; break;
                    case 2: out = colv[mcycle]; break;
                    default: continue; /* 3: write-protected */
                }
                dst[f] = out;
            }

            ++lin;
            if (++cycle == wl) {
                cycle = 0;
                block += cl;
            }
        }
    }

    ++v.unpacks;
    if (rt_trace() || is_pow2(v.unpacks)) {
        rt_log_debug("vif1", "UNPACK #%" PRIu64 " cmd=0x%02x addr=0x%x num=%u flg=%d usn=%d cl=%u wl=%u mode=%u masked=%d",
            v.unpacks, cmd, addr, num, flg ? 1 : 0, usn ? 1 : 0, cl, wl, v.mode, masked ? 1 : 0);
    }
}

#ifdef ICORECOMP_VIF1_SELFTEST
/* The implementation exec_unpack above replaced, kept verbatim as the
 * differential oracle for hw/vif1_selftest.cpp and compiled only into that
 * target. The only edit is the payload source: it read v.payload directly,
 * where the command now carries a span. Its sampled-log counters are its
 * own statics, so a reference run and a fast run do not share them; the
 * selftest compares data memory and the row register, not log output. */
void exec_unpack_reference(Vif1& v, const uint32_t* pay, uint32_t paywords) {
    const uint32_t code = v.code;
    const uint32_t cmd = (code >> 24) & 0x7F;
    const uint32_t vn = (cmd >> 2) & 3;
    const uint32_t vl = cmd & 3;
    const bool masked = (cmd & 0x10) != 0;
    const bool usn = (code >> 14) & 1;
    const bool flg = (code >> 15) & 1;
    uint32_t num = (code >> 16) & 0xFF;
    if (num == 0) num = 256;
    uint32_t addr = code & 0x3FF;
    if (flg) addr = (addr + v.tops) & 0x3FF;

    uint32_t cl = v.cl, wl = v.wl;
    if (wl == 0) wl = 256;
    if (cl == 0 && wl > cl) {
        /* CL=0 with filling would consume no input at all; loud and clamp. */
        static uint64_t n = 0;
        if (is_pow2(++n)) rt_log_warn("vif1", "UNPACK with CL=0 (STCYCL=0x%02x%02x); clamping CL to 1", v.wl, v.cl);
        cl = 1;
    }
    const bool filling = wl > cl;
    static uint64_t fill_uses = 0;
    if (filling && is_pow2(++fill_uses)) {
        rt_log_debug("vif1", "UNPACK filling mode in use (CL=%u WL=%u) [#%" PRIu64 "]", cl, wl, fill_uses);
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(pay);
    const uint32_t nbytes = paywords * 4;
    uint32_t cursor = 0; /* byte cursor into payload */

    Vu1State* vu = rt_vu1_state();
    uint32_t last_data[4] = {0, 0, 0, 0}; /* buffered input for filling cycles */
    static uint64_t missing_field_data = 0;

    for (uint32_t k = 0; k < num; ++k) {
        uint32_t cycle = filling ? (k % wl) : (k % wl);
        bool has_input = !filling || cycle < cl;

        uint32_t data[4] = {0, 0, 0, 0};
        uint32_t nfields = vn + 1;
        if (has_input) {
            switch (vl) {
                case 0: /* 32-bit */
                    for (uint32_t f = 0; f < nfields; ++f) {
                        if (cursor + 4 <= nbytes) std::memcpy(&data[f], bytes + cursor, 4);
                        cursor += 4;
                    }
                    break;
                case 1: /* 16-bit */
                    for (uint32_t f = 0; f < nfields; ++f) {
                        uint16_t h = 0;
                        if (cursor + 2 <= nbytes) std::memcpy(&h, bytes + cursor, 2);
                        cursor += 2;
                        data[f] = sext16(h, usn);
                    }
                    break;
                case 2: /* 8-bit */
                    for (uint32_t f = 0; f < nfields; ++f) {
                        uint8_t b = (cursor < nbytes) ? bytes[cursor] : 0;
                        ++cursor;
                        data[f] = sext8(b, usn);
                    }
                    break;
                default: { /* V4-5 (vl==3 is only defined for vn==3) */
                    uint16_t h = 0;
                    if (cursor + 2 <= nbytes) std::memcpy(&h, bytes + cursor, 2);
                    cursor += 2;
                    data[0] = (uint32_t)(h & 0x1F) << 3;
                    data[1] = (uint32_t)((h >> 5) & 0x1F) << 3;
                    data[2] = (uint32_t)((h >> 10) & 0x1F) << 3;
                    data[3] = (uint32_t)((h >> 15) & 1) << 7;
                    nfields = 4;
                    break;
                }
            }
            if (vn == 0) { /* S-xx broadcasts to all four fields */
                data[1] = data[2] = data[3] = data[0];
                nfields = 4;
            }
            std::memcpy(last_data, data, sizeof(data));
        } else {
            std::memcpy(data, last_data, sizeof(data));
            nfields = 4;
        }

        /* Destination qword. Skipping mode: after WL writes, skip to the
         * next CL boundary. Filling mode: consecutive. */
        uint32_t qw;
        if (filling) {
            qw = (addr + k) & (kVuQw - 1);
        } else {
            qw = (addr + (k / wl) * cl + (k % wl)) & (kVuQw - 1);
        }
        uint8_t* dst = vu->mem + (size_t)qw * 16;

        uint32_t mcycle = cycle < 3 ? cycle : 3;
        for (uint32_t f = 0; f < 4; ++f) {
            uint32_t m = masked ? (v.mask >> (mcycle * 8 + f * 2)) & 3 : 0;
            uint32_t out;
            switch (m) {
                case 0: {
                    if (f >= nfields) {
                        /* Field not provided by this format and not masked:
                         * hardware writes indeterminate data. Write 0, loud
                         * once in a while. */
                        if (is_pow2(++missing_field_data)) {
                            rt_log_warn("vif1", "UNPACK cmd=0x%02x writes field %u with no source data (mask says data) [#%" PRIu64 "]",
                                cmd, f, missing_field_data);
                        }
                        out = 0;
                        break;
                    }
                    out = data[f];
                    if (v.mode == 1) {
                        out += v.row[f];
                    } else if (v.mode == 2) {
                        out += v.row[f];
                        v.row[f] = out;
                    } else if (v.mode == 3) {
                        static uint64_t n = 0;
                        if (is_pow2(++n)) rt_log_warn("vif1", "UNPACK with undefined STMOD=3; treating as 0 [#%" PRIu64 "]", n);
                    }
                    break;
                }
                case 1: out = v.row[f]; break;
                case 2: out = v.col[mcycle]; break;
                default: continue; /* 3: write-protected */
            }
            std::memcpy(dst + f * 4, &out, 4);
        }
    }

    ++v.unpacks;
    if (rt_trace() || is_pow2(v.unpacks)) {
        rt_log_debug("vif1", "UNPACK #%" PRIu64 " cmd=0x%02x addr=0x%x num=%u flg=%d usn=%d cl=%u wl=%u mode=%u masked=%d",
            v.unpacks, cmd, addr, num, flg ? 1 : 0, usn ? 1 : 0, cl, wl, v.mode, masked ? 1 : 0);
    }
}
#endif /* ICORECOMP_VIF1_SELFTEST */

/* Payload words required for an UNPACK before it can execute. The vector
 * count and the bytes each vector occupies are the same two rules
 * exec_unpack reads the payload with, so both come from the same helpers:
 * a payload sized here and consumed there cannot drift apart. */
uint32_t unpack_words_needed(const Vif1& v, uint32_t code) {
    const uint32_t cmd = (code >> 24) & 0x7F;
    const uint32_t vn = (cmd >> 2) & 3;
    const uint32_t vl = cmd & 3;
    uint32_t num = (code >> 16) & 0xFF;
    if (num == 0) num = 256;
    uint32_t cl = v.cl, wl = v.wl;
    if (wl == 0) wl = 256;
    if (cl == 0) cl = 1;
    const uint64_t total_bytes =
        (uint64_t)unpack_input_vectors(num, cl, wl) * unpack_vector_bytes(vn, vl);
    return (uint32_t)((total_bytes + 3) / 4);
}

/* Mnemonic for a VIFcode command field, for the diagnostic dump only. */
const char* cmd_name(uint32_t cmd) {
    if ((cmd & 0x60) == 0x60) return "UNPACK";
    switch (cmd) {
        case 0x00: return "NOP";
        case 0x01: return "STCYCL";
        case 0x02: return "OFFSET";
        case 0x03: return "BASE";
        case 0x04: return "ITOP";
        case 0x05: return "STMOD";
        case 0x06: return "MSKPATH3";
        case 0x07: return "MARK";
        case 0x10: return "FLUSHE";
        case 0x11: return "FLUSH";
        case 0x13: return "FLUSHA";
        case 0x14: return "MSCAL";
        case 0x15: return "MSCALF";
        case 0x17: return "MSCNT";
        case 0x20: return "STMASK";
        case 0x30: return "STROW";
        case 0x31: return "STCOL";
        case 0x4A: return "MPG";
        case 0x50: return "DIRECT";
        case 0x51: return "DIRECTHL";
        default: return "?";
    }
}

/* Host pointer for one 16-byte-aligned guest qword, resolved the way
 * dmac.cpp's dma_ptr resolves DMA addresses (bit 31 selects the
 * scratchpad, which wraps at its architectural 16 KB). Null when the page
 * is not mapped; an aligned qword never crosses a 64 KB page boundary. */
const uint8_t* dump_qword_ptr(uint32_t addr) {
    if (addr & 0x80000000u) {
        const uint8_t* page = g_pages[0x70000000u >> 16];
        return page ? page + (addr & 0x3FFFu) : nullptr;
    }
    return rt_gptr(addr);
}

/* ICO's allocator (ios/memory.c in the decomp, the "<ALLOC>________" /
 * "<FREE AREA>____" magic strings) puts a 0x40-byte header ahead of every
 * block: a 16-byte ASCII magic tag, the 16-byte source file name of the
 * caller, then the partition pointer, the size in qwords and the caller's
 * line number. When the DMA tag feeding the failing stream is a REF, REFS
 * or REFE, those 64 bytes name the allocation the packet was built in. */
void dump_alloc_header(uint32_t payload_addr) {
    const uint32_t hdr = payload_addr - 0x40;
    rt_log_info("vif1", "allocation header before REF payload 0x%08x", payload_addr);
    char ascii[33];
    uint32_t n = 0;
    for (int line = 0; line < 4; ++line) {
        const uint32_t a = hdr + (uint32_t)line * 16;
        const uint8_t* p = dump_qword_ptr(a);
        if (!p) {
            rt_log_warn("vif1", "0x%08x: unmapped", a);
            if (line < 2) for (int i = 0; i < 16; ++i) ascii[n++] = '.';
            continue;
        }
        uint32_t w[4];
        std::memcpy(w, p, sizeof(w));
        rt_log_info("vif1", "0x%08x: %08x %08x %08x %08x", a, w[0], w[1], w[2], w[3]);
        /* First 32 bytes are the two ASCII fields. */
        if (line < 2) {
            for (int i = 0; i < 16; ++i) {
                const uint8_t c = p[i];
                ascii[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            }
        }
    }
    ascii[n] = '\0';
    rt_log_info("vif1", "header ascii: %s", ascii);
}

/* ---- command execution -------------------------------------------------- */

/* MPG upload deferral.
 *
 * vif1.cpp is the only caller of rt_vu1_micro_written, and that call does
 * two things: it marks the upload dirty, which makes the next MSCAL rehash
 * micro memory, and it moves vu1rt's upload extent, which is the length
 * that hash covers. When an MPG writes bytes that are already resident the
 * first is pure waste, but the second is still owed, so the notification is
 * deferred rather than dropped.
 *
 * g_micro_notified_len mirrors vu1rt's g_upload_len (same rule, applied to
 * the calls actually made). g_micro_deferred_len, when nonzero, is the end
 * of a run of MPG segments starting at offset 0 whose bytes all matched
 * what was resident and whose notification has not been made. The run is
 * settled at the next MSCAL: if it ends where the last notified upload
 * ended, vu1rt's extent is already right and nothing is owed; otherwise the
 * notification is made after all. A segment that does not match, or does
 * not continue the run, flushes it first.
 *
 * Not reset by VIF1_FBRST: these mirror vu1rt state, which a VIF1 reset
 * does not touch. */
uint32_t g_micro_notified_len = 0;
uint32_t g_micro_deferred_len = 0;
uint64_t g_mpg_deferred = 0;

/* One call for the whole deferred run leaves the same extent behind as the
 * per-segment calls would have: the first segment starts the extent at its
 * own length and each later one continues it, so both end at the run
 * length, and neither reaches vu1rt's out-of-order branch. */
void micro_flush_deferred() {
    if (g_micro_deferred_len == 0) return;
    rt_vu1_micro_written(0, g_micro_deferred_len);
    g_micro_notified_len = g_micro_deferred_len;
    g_micro_deferred_len = 0;
}

void micro_settle_deferred() {
    if (g_micro_deferred_len == 0) return;
    if (g_micro_deferred_len == g_micro_notified_len) {
        /* Same bytes and the same extent as the upload vu1rt last hashed:
         * a rehash could only return the hash already bound. */
        g_micro_deferred_len = 0;
        return;
    }
    micro_flush_deferred();
}

/* Writes one MPG segment into micro memory, or establishes that it is
 * already there and defers the notification. */
void micro_note_upload(uint32_t dst, uint32_t bytes, const uint32_t* pay) {
    uint8_t* micro = rt_vu1_micro();
    const bool same = std::memcmp(micro + dst, pay, bytes) == 0;
    const bool continues = (dst == 0) || (g_micro_deferred_len != 0 && dst == g_micro_deferred_len);
    if (same && continues) {
        g_micro_deferred_len = dst + bytes;
        ++g_mpg_deferred;
        return;
    }
    micro_flush_deferred();
    std::memcpy(micro + dst, pay, bytes);
    rt_vu1_micro_written(dst, bytes);
    /* Same rule as rt_vu1_micro_written applies to g_upload_len. */
    if (dst == 0) {
        g_micro_notified_len = bytes;
    } else if (dst == g_micro_notified_len) {
        g_micro_notified_len = dst + bytes;
    } else if (dst + bytes > g_micro_notified_len) {
        g_micro_notified_len = dst + bytes;
    }
}

/* Starting or continuing a microprogram: MSCAL, MSCALF and MSCNT.
 *
 * All three latch TOP from TOPS and flip the double buffer, MSCNT
 * included. That looks wrong for a "continue" and it was tried the other
 * way: leaving TOP and DBF alone across MSCNT makes the microprograms read
 * stale data and XGKICK malformed GIF tags within a second of boot.
 *
 * The reason is what these programs are. They process one batch per run,
 * stop at an E bit, and are resumed by MSCNT for the *next* batch, not to
 * finish the previous one. Both of ICO's resume points open with an `xtop`
 * instruction precisely because they are fetching a new buffer. So a
 * continuation needs the same buffer swap a start does. */
void mscal_common(Vif1& v, uint32_t pc_bytes, const char* how) {
    /* Settle any MPG run held back by micro_note_upload before the hash
     * this MSCAL may take. */
    micro_settle_deferred();
    ++v.mscals;
    v.top = v.tops;
    v.itop = v.itops;
    v.dbf = !v.dbf;
    v.tops = (v.base + (v.dbf ? v.ofst : 0)) & 0x3FF;
    rt_vu1_mscal(pc_bytes, v.top, v.itop, how);
}

/* pay/paywords is the command's payload, either a span into the buffer
 * rt_vif1_feed was called with or the straddle vector; null and 0 for a
 * command that has no payload. */
void exec_command(Vif1& v, const uint32_t* pay, uint32_t paywords) {
    const uint32_t code = v.code;
    const uint32_t cmd = (code >> 24) & 0x7F;
    const uint32_t imm = code & 0xFFFF;

    if ((code & 0x80000000u) != 0) {
        /* i-bit: VIF1 interrupt, delivered deferred like everything else. */
        rt_log_info("vif1", "i-bit set on cmd 0x%02x: raising INTC VIF1", cmd);
        rt_intc_raise(5 /* VIF1 */);
    }

    if ((cmd & 0x60) == 0x60) {
        exec_unpack(v, pay, paywords);
        return;
    }

    switch (cmd) {
        case 0x00: break; /* NOP */
        case 0x01: v.cl = imm & 0xFF; v.wl = (imm >> 8) & 0xFF; break; /* STCYCL */
        case 0x02: /* OFFSET */
            v.ofst = imm & 0x3FF;
            v.dbf = false;
            v.tops = v.base;
            break;
        case 0x03: v.base = imm & 0x3FF; break;  /* BASE */
        case 0x04: v.itops = imm & 0x3FF; break; /* ITOP */
        case 0x05: v.mode = imm & 3; break;      /* STMOD */
        case 0x06: /* MSKPATH3 */
            v.mskpath3 = (imm >> 15) & 1;
            rt_log_info("vif1", "MSKPATH3 %s (arbitration is synchronous; informational only)",
                v.mskpath3 ? "masked" : "unmasked");
            break;
        case 0x07: v.mark = imm; break;          /* MARK */
        case 0x10: case 0x11: case 0x13: break;  /* FLUSHE/FLUSH/FLUSHA: everything is synchronous */
        case 0x14: mscal_common(v, (uint32_t)imm * 8, "MSCAL"); break;
        case 0x15: mscal_common(v, (uint32_t)imm * 8, "MSCALF"); break;
        case 0x17: mscal_common(v, rt_vu1_state()->pc, "MSCNT"); break;
        case 0x20: v.mask = pay[0]; break; /* STMASK */
        case 0x30: for (int i = 0; i < 4; ++i) v.row[i] = pay[i]; break; /* STROW */
        case 0x31: for (int i = 0; i < 4; ++i) v.col[i] = pay[i]; break; /* STCOL */
        case 0x4A: { /* MPG */
            uint32_t num = (code >> 16) & 0xFF;
            if (num == 0) num = 256;
            uint32_t dst = ((uint32_t)imm * 8) & 0x3FFF;
            uint32_t bytes = num * 8;
            if (dst + bytes > 16384) {
                rt_log_warn("vif1", "MPG target [0x%x, 0x%x) exceeds micro memory; clamping", dst, dst + bytes);
                bytes = 16384 - dst;
            }
            /* ICO cycles the same small set of microprograms and
             * re-uploads them repeatedly within a field, so most MPGs
             * write bytes that are already resident. (The rate was
             * estimated during an earlier design pass, not measured here;
             * the deferral does not depend on the number, only on the
             * pattern.) When the bytes are already resident, the copy
             * changes nothing and the upload notification can be held
             * back: rt_vu1_micro_written exists only to mark the upload
             * dirty so the next MSCAL rehashes micro memory, and a rehash
             * of bytes that did not change can only return the hash that
             * is already bound. What the notification also does is move
             * vu1rt's upload extent, so the skip is a deferral, not a
             * drop: micro_note_upload settles the run at the next MSCAL
             * and notifies after all if the extent would end anywhere but
             * where the last notified upload did. See the note above
             * micro_flush_deferred. */
            micro_note_upload(dst, bytes, pay);
            ++v.mpgs;
            /* Sampled like DIRECT below: MPG is one of the most frequent
             * VIFcodes in a field and a line per upload says nothing the
             * sampled line does not. */
            if (rt_trace() || is_pow2(v.mpgs)) {
                rt_log_debug("vif1", "MPG #%" PRIu64 ": %u instructions to micro 0x%x (%" PRIu64 " uploads so far were already resident)",
                    v.mpgs, num, dst, g_mpg_deferred);
            }
            break;
        }
        case 0x50: case 0x51: { /* DIRECT / DIRECTHL */
            ++v.directs;
            uint32_t qw = paywords / 4;
            if (rt_trace() || is_pow2(v.directs)) {
                rt_log_debug("vif1", "DIRECT%s #%" PRIu64 ": %u qw to GIF PATH2", cmd == 0x51 ? "HL" : "", v.directs, qw);
            }
            /* The old code always handed on a std::vector<uint32_t>
             * buffer, so PATH2 consumers have only ever seen a payload at
             * an operator new alignment. A DIRECT payload starts on a
             * quadword boundary in the VIF FIFO, so a span into the feed
             * buffer is normally aligned too, but nothing in the feed
             * interface guarantees it: keep the guarantee rather than
             * assume the GS backend tolerates an unaligned quadword
             * stream. */
            const uint32_t* data = pay;
            if ((reinterpret_cast<uintptr_t>(data) & 15) != 0) {
                static std::vector<uint32_t> aligned;
                static uint64_t n = 0;
                if (is_pow2(++n)) {
                    rt_log_debug("vif1", "DIRECT payload is not quadword aligned in the feed buffer; copying [#%" PRIu64 "]", n);
                }
                aligned.assign(pay, pay + paywords);
                data = aligned.data();
            }
            rt_gif_submit(1, reinterpret_cast<const uint8_t*>(data), qw);
            break;
        }
        default:
            /* Unknown VIFcode desynchronizes the stream; that is a fatal
             * model error, not something to limp past. Dump the command
             * history and the source bytes first: the interesting question
             * is which DMA tag delivered the word, not the word itself. */
            rt_vif1_dump_state();
            rt_dmac_dump_recent_tags(1);
            {
                /* A REF/REFS/REFE payload sits inside an allocator block,
                 * so the 0x40 bytes ahead of it say who allocated it. */
                uint32_t tid = 0, taddr = 0, tqwc = 0;
                if (rt_dmac_current_tag(1, &tid, &taddr, &tqwc) &&
                    (tid == 0 || tid == 3 || tid == 4) && taddr >= 0x40) {
                    dump_alloc_header(taddr);
                }
            }
            rt_fatal("vif1", nullptr,
                "unknown VIFcode 0x%08x (cmd 0x%02x) after %" PRIu64 " commands at guest 0x%08x",
                code, cmd, v.cmds, v.cur_addr);
    }
}

uint32_t words_needed(const Vif1& v, uint32_t code) {
    const uint32_t cmd = (code >> 24) & 0x7F;
    if ((cmd & 0x60) == 0x60) return unpack_words_needed(v, code);
    switch (cmd) {
        case 0x20: return 1;             /* STMASK */
        case 0x30: case 0x31: return 4;  /* STROW/STCOL */
        case 0x4A: {                     /* MPG */
            uint32_t num = (code >> 16) & 0xFF;
            return (num == 0 ? 256 : num) * 2;
        }
        case 0x50: case 0x51: {          /* DIRECT/DIRECTHL */
            uint32_t imm = code & 0xFFFF;
            return (imm == 0 ? 65536 : imm) * 4;
        }
        default: return 0;
    }
}

} // namespace

/* payload= is the words buffered for a command whose payload straddles
 * feed calls. A command that arrived whole executes off a span into the
 * caller's buffer and leaves that counter at 0. */
void rt_vif1_dump_state() {
    const Vif1& v = g_vif;
    rt_log_info("vif1",
        "state: cl=%u wl=%u mask=0x%08x mode=%u itops=0x%03x itop=0x%03x base=0x%03x ofst=0x%03x "
        "tops=0x%03x top=0x%03x mark=0x%04x dbf=%d pending=%d need_words=%u payload=%u "
        "cmds=%" PRIu64 " unpacks=%" PRIu64 " directs=%" PRIu64 " mpgs=%" PRIu64 " mscals=%" PRIu64,
        v.cl, v.wl, v.mask, v.mode, v.itops, v.itop, v.base, v.ofst, v.tops, v.top, v.mark,
        v.dbf ? 1 : 0, v.pending ? 1 : 0, v.need_words, (uint32_t)v.payload.size(),
        v.cmds, v.unpacks, v.directs, v.mpgs, v.mscals);

    if (v.cmds == 0) {
        rt_log_info("vif1", "no VIFcodes have been taken yet");
    } else {
        const uint32_t shown = (uint32_t)(v.cmds < 32 ? v.cmds : 32);
        for (uint32_t i = 0; i < shown; ++i) {
            const CodeRec& r = v.recent[(v.cmds - shown + i) % 32];
            const uint32_t cmd = (r.code >> 24) & 0x7F;
            rt_log_info("vif1", "recent[%u]: 0x%08x cmd=0x%02x %s%s addr=0x%08x #%" PRIu64 " payload=%u",
                i, r.code, cmd, cmd_name(cmd), (r.code & 0x80000000u) ? " i" : "",
                r.addr, r.index, r.need_words);
        }
    }

    /* Guest memory around the current code word. Anything upstream of it is
     * the packet that desynchronized the stream, so the window leans
     * backwards. */
    if (v.cur_addr == RT_VIF1_ADDR_NONE) {
        rt_log_info("vif1", "current code word has no guest address (fed by a FIFO store); no hexdump");
        return;
    }
    const uint32_t base = v.cur_addr & ~15u;
    if (!dump_qword_ptr(base)) {
        rt_log_warn("vif1", "guest address 0x%08x of the current code word is unmapped; no hexdump", v.cur_addr);
        return;
    }
    for (int line = -8; line <= 4; ++line) {
        const uint32_t a = base + (uint32_t)(line * 16);
        const uint8_t* p = dump_qword_ptr(a);
        if (!p) continue;
        uint32_t w[4];
        std::memcpy(w, p, sizeof(w));
        rt_log_info("vif1", "0x%08x: %08x %08x %08x %08x%s", a, w[0], w[1], w[2], w[3],
            line == 0 ? " <- code" : "");
    }
}

/* The callers pass a contiguous buffer that outlives the call: dmac.cpp
 * gathers each ch1 transfer into one scratch vector before feeding it, and
 * the FIFO path below passes the four words of a 128-bit store. So a
 * command whose whole payload is already in `words` is executed straight
 * out of that buffer and never copied. Only a command that straddles feed
 * calls goes through v.payload, and that copy is one bulk insert per feed
 * call rather than a push_back per word. */
void rt_vif1_feed(const uint32_t* words, uint32_t count, uint32_t guest_addr) {
    RT_PROF_ZONE(RT_PROF_VIF1);
    Vif1& v = g_vif;
    uint32_t i = 0;
    while (i < count) {
        if (v.pending) {
            const uint32_t want = v.need_words - (uint32_t)v.payload.size();
            const uint32_t avail = count - i;
            const uint32_t take = want < avail ? want : avail;
            v.payload.insert(v.payload.end(), words + i, words + i + take);
            i += take;
            if ((uint32_t)v.payload.size() == v.need_words) {
                v.pending = false;
                exec_command(v, v.payload.data(), v.need_words);
            }
            continue;
        }

        v.code = words[i];
        v.need_words = words_needed(v, v.code);
        ++v.cmds;
        /* Scratchpad addresses keep bit 31; the word offset only ever
         * touches the low bits. */
        v.cur_addr = guest_addr == RT_VIF1_ADDR_NONE ? RT_VIF1_ADDR_NONE : guest_addr + i * 4;
        v.recent[(v.cmds - 1) % 32] = {v.code, v.cur_addr, v.cmds, v.need_words};
        ++i;

        if (v.need_words == 0) {
            v.payload.clear();
            exec_command(v, nullptr, 0);
            continue;
        }
        /* Held in a local so the loop's advance does not depend on
         * v.need_words surviving exec_command, which the payload span
         * itself already depends on. */
        const uint32_t need = v.need_words;
        const uint32_t avail = count - i;
        if (avail >= need) {
            /* Whole payload is in this feed call: hand over a span. */
            v.payload.clear();
            exec_command(v, words + i, need);
            i += need;
        } else {
            v.pending = true;
            v.payload.clear();
            v.payload.reserve(need);
            v.payload.insert(v.payload.end(), words + i, words + count);
            i = count;
        }
    }
}

bool rt_hw_fifo_write128(uint32_t addr, const rc_u128* val) {
    switch (addr) {
        case 0x10004000: { /* VIF0 FIFO: counted and discarded, see header */
            g_vif0_words += 4;
            if (is_pow2(g_vif0_words / 4)) {
                rt_log_warn("vif1", "VIF0 FIFO write discarded (words=0x%08x 0x%08x 0x%08x 0x%08x) [qw #%" PRIu64 "]",
                    val->u32x[0], val->u32x[1], val->u32x[2], val->u32x[3], g_vif0_words / 4);
            }
            return true;
        }
        case 0x10005000: /* VIF1 FIFO */
            rt_vif1_feed(val->u32x, 4, RT_VIF1_ADDR_NONE);
            return true;
        case 0x10006000: /* GIF FIFO: direct PATH3 qword */
            rt_gif_submit(2, val->u8x, 1);
            return true;
        case 0x10007010: /* toIPU FIFO: SETIQ table priming by CPU store */
            rt_ipu_fifo_feed(val->u8x);
            return true;
        default:
            return false;
    }
}

bool rt_vif_mmio_read(uint32_t addr, uint32_t* out) {
    const Vif1& v = g_vif;
    switch (addr) {
        case 0x10003C00: /* VIF1_STAT: idle, FIFO empty; DBF mirrors state */
            *out = v.dbf ? 0x80u : 0;
            return true;
        case 0x10003C20: *out = v.err; return true;
        case 0x10003C30: *out = v.mark; return true;
        case 0x10003C40: *out = v.cl | (v.wl << 8); return true;
        case 0x10003C50: *out = v.mode; return true;
        case 0x10003C60: *out = 0; return true; /* VIF1_NUM: nothing in flight */
        case 0x10003C70: *out = v.mask; return true;
        case 0x10003C80: *out = v.code; return true;
        case 0x10003C90: *out = v.itops; return true;
        case 0x10003CA0: *out = v.base; return true;
        case 0x10003CB0: *out = v.ofst; return true;
        case 0x10003CC0: *out = v.tops; return true;
        case 0x10003CD0: *out = v.itop; return true;
        case 0x10003CE0: *out = v.top; return true;
        case 0x10003C10: *out = 0; return true; /* VIF1_FBRST: write-only */
        case 0x10003800: *out = 0; return true; /* VIF0_STAT: idle */
        default:
            return false;
    }
}

bool rt_vif_mmio_write(uint32_t addr, uint32_t v) {
    switch (addr) {
        case 0x10003C10: /* VIF1_FBRST: RST clears the interpreter state */
            if (v & 1) {
                /* cmds keeps counting across a reset and indexes the code
                 * ring, so the ring and cur_addr carry over with it. */
                Vif1 fresh;
                fresh.cmds = g_vif.cmds;
                std::memcpy(fresh.recent, g_vif.recent, sizeof(fresh.recent));
                fresh.cur_addr = g_vif.cur_addr;
                g_vif = fresh;
                rt_log_info("vif1", "VIF1_FBRST reset");
            }
            return true;
        case 0x10003C20: g_vif.err = v; return true;  /* VIF1_ERR */
        case 0x10003C30: g_vif.mark = v; return true; /* VIF1_MARK */
        case 0x10003C00: return true;                 /* VIF1_STAT: FDR etc, accept */
        /* VIF0 register writes: accept silently (reset sequence). */
        case 0x10003800: case 0x10003810: case 0x10003820: case 0x10003830:
            return true;
        default:
            return false;
    }
}


#ifdef ICORECOMP_VIF1_SELFTEST
/* ---- selftest hooks (hw/vif1_selftest.cpp) ------------------------------
 *
 * Compiled only into icorecomp-vif1-selftest. They give the differential
 * test direct access to the register state exec_unpack reads and to both
 * implementations of it, so a case can be run twice from identical state.
 */
void rt_vif1_selftest_reset() {
    g_vif = Vif1();
}

void rt_vif1_selftest_set_regs(uint32_t cl, uint32_t wl, uint32_t mask, uint32_t mode,
                               const uint32_t row[4], const uint32_t col[4], uint32_t tops) {
    g_vif.cl = cl;
    g_vif.wl = wl;
    g_vif.mask = mask;
    g_vif.mode = mode;
    for (int i = 0; i < 4; ++i) {
        g_vif.row[i] = row[i];
        g_vif.col[i] = col[i];
    }
    g_vif.tops = tops;
}

void rt_vif1_selftest_get_row(uint32_t row[4]) {
    for (int i = 0; i < 4; ++i) row[i] = g_vif.row[i];
}

int rt_vif1_selftest_pending() {
    return g_vif.pending ? 1 : 0;
}

uint32_t rt_vif1_selftest_words_needed(uint32_t code) {
    return words_needed(g_vif, code);
}

void rt_vif1_selftest_unpack(uint32_t code, const uint32_t* pay, uint32_t words, int reference) {
    g_vif.code = code;
    if (reference) exec_unpack_reference(g_vif, pay, words);
    else exec_unpack(g_vif, pay, words);
}
#endif /* ICORECOMP_VIF1_SELFTEST */
