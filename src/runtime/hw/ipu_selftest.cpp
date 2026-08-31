/* hw/ipu_selftest.cpp: standalone IPU decode test (icorecomp-ipu-selftest).
 *
 * Purpose: verify the IPU model (hw/ipu.cpp) against the real FMV bitstream
 * without booting the game. It mounts the user's disc image, locates the
 * MPEG-2 program stream inside DFDATAS/DATA.DF by scanning for pack start
 * codes (no ROM-derived offsets are hardcoded), demuxes the video
 * elementary stream exactly the way the game does EE-side, and then drives
 * the IPU through its MMIO interface the way libmpeg does: FDEC/VDEC for
 * headers and macroblock addressing, SETIQ for the quantizer matrices,
 * BDEC per macroblock, CSC for display conversion.
 *
 * Pass criteria:
 *   - the first I picture decodes to exactly mb_width x mb_height
 *     macroblocks with no ECD,
 *   - luma statistics are plausible (mean inside video range, nonzero
 *     spread),
 *   - the first P picture's residual pass decodes with no ECD,
 *   - CSC converts the reconstructed I frame; the result is written as a
 *     BMP for eyeball verification (untracked output path, /tmp by
 *     default; override with ICORECOMP_IPU_SELFTEST_OUT).
 *
 * This binary links ipu.cpp + iso9660.cpp + loader/log/sha1 and stubs the
 * scheduler/INTC/DMAC symbols ipu.cpp expects; the DMA bridge is bypassed
 * via the rt_ipu_test_* hooks (input is fed up front, so commands never
 * stall).
 */
#include "hw.h"

#include "../iso/iso9660.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* ---- stubs for symbols ipu.cpp pulls in (no scheduler/DMAC linked) ------- */

uint8_t* g_pages[0x10000];

bool rt_trace() { return std::getenv("ICORECOMP_TRACE") != nullptr; }
void rt_intc_raise(int) {}
void rt_dmac_raise(int) {}

uint32_t* rt_dmac_ipu_reg(int ch, int which) {
    static uint32_t dummy[2][4];
    return &dummy[ch == 4 ? 1 : 0][which];
}

namespace {

/* ---- MMIO driver helpers ------------------------------------------------- */

constexpr uint32_t IPU_CMD = 0x10002000, IPU_CTRL = 0x10002010;

uint64_t r64(uint32_t addr) {
    uint64_t v = 0;
    if (!rt_ipu_mmio_read(addr, &v)) rt_fatal("selftest", nullptr, "mmio read 0x%08x unhandled", addr);
    return v;
}
void w32(uint32_t addr, uint32_t v) {
    if (!rt_ipu_mmio_write(addr, v)) rt_fatal("selftest", nullptr, "mmio write 0x%08x unhandled", addr);
}

uint64_t cmd_result() {
    uint64_t r = r64(IPU_CMD);
    if (r >> 63) {
        rt_fatal("selftest", nullptr, "IPU stayed busy after a command; the test feed ran dry or the model stalled");
    }
    return r;
}

/* FDEC: skip `skip` bits (0..63) and return the next 32 bits. */
uint32_t fdec(unsigned skip) {
    w32(IPU_CMD, 0x40000000u | skip);
    return (uint32_t)cmd_result();
}

void advance(unsigned bits) {
    while (bits > 63) {
        fdec(63);
        bits -= 63;
    }
    if (bits) fdec(bits);
}

uint32_t peekbits(unsigned n) { return fdec(0) >> (32 - n); }

uint32_t getbits(unsigned n) {
    uint32_t v = peekbits(n);
    advance(n);
    return v;
}

/* VDEC: returns the raw 32-bit result (value low 16, code length high 16). */
uint32_t vdec(unsigned tbl) {
    w32(IPU_CMD, 0x30000000u | (tbl << 26));
    return (uint32_t)cmd_result();
}

uint32_t ipu_ctrl() { return (uint32_t)r64(IPU_CTRL); }

void byte_align() {
    uint32_t bp = (uint32_t)r64(0x10002020) & 0x7F;
    if (bp & 7) advance(8 - (bp & 7));
}

void find_start_code() {
    byte_align();
    while (peekbits(24) != 1) advance(8);
}

/* ---- PSS demux ----------------------------------------------------------- */

/* Scans the disc for the FMV program stream (a long run of sectors starting
 * with MPEG-2 pack start codes) and demuxes video PES (stream id 0xE0)
 * payload into `es` until it holds `want` bytes. */
void extract_video_es(std::vector<uint8_t>& es, size_t want, uint32_t skip_sectors) {
    RtIsoFile df;
    if (!rt_iso_search("\\DFDATAS\\DATA.DF;1", &df)) {
        rt_fatal("selftest", nullptr, "DFDATAS/DATA.DF not found on the mounted image");
    }
    uint32_t nsec = df.size / 2048;
    uint8_t sec[2048];
    uint32_t pss_lsn = 0;
    for (uint32_t s = 0; s < nsec; ++s) {
        if (!rt_iso_read_sector(df.lsn + s, sec)) break;
        if (sec[0] == 0 && sec[1] == 0 && sec[2] == 1 && sec[3] == 0xBA) {
            pss_lsn = df.lsn + s;
            break;
        }
    }
    if (!pss_lsn) rt_fatal("selftest", nullptr, "no MPEG-2 program stream found inside DATA.DF");
    rt_log("selftest", "program stream found at LBA %u (DATA.DF + %u sectors)", pss_lsn, pss_lsn - df.lsn);

    /* Stream the sectors through a small window buffer and walk the pack /
     * PES structure. skip_sectors starts the extraction a few seconds into
     * the movie (the lead-in is digital black, which makes for a useless
     * verification image); every sector starts with a pack header, so any
     * sector is a valid resync point. */
    std::vector<uint8_t> buf;
    uint32_t lsn = pss_lsn + skip_sectors;
    size_t pos = 0;
    auto refill = [&](size_t need) -> bool {
        while (buf.size() - pos < need) {
            if (!rt_iso_read_sector(lsn++, sec)) return false;
            buf.insert(buf.end(), sec, sec + 2048);
            if (pos > 1 << 20) {
                buf.erase(buf.begin(), buf.begin() + pos);
                pos = 0;
            }
        }
        return true;
    };
    auto rd16 = [&](size_t at) -> unsigned { return (buf[at] << 8) | buf[at + 1]; };

    while (es.size() < want) {
        if (!refill(6)) break;
        if (!(buf[pos] == 0 && buf[pos + 1] == 0 && buf[pos + 2] == 1)) {
            ++pos; /* resync (should not happen in a clean stream) */
            continue;
        }
        uint8_t sid = buf[pos + 3];
        if (sid == 0xBA) {
            if (!refill(14)) break;
            pos += 14 + (buf[pos + 13] & 7);
        } else if (sid == 0xB9) {
            break; /* program end */
        } else if (sid >= 0xBB) {
            if (!refill(6)) break;
            unsigned len = rd16(pos + 4);
            if (!refill(6 + len)) break;
            if (sid == 0xE0) {
                /* MPEG-2 PES: flags + header length, then payload. */
                unsigned hdrlen = buf[pos + 8];
                size_t payload = pos + 9 + hdrlen;
                size_t end = pos + 6 + len;
                if (payload < end) es.insert(es.end(), buf.begin() + payload, buf.begin() + end);
            }
            pos += 6 + len;
        } else {
            ++pos;
        }
    }
    rt_log("selftest", "demuxed %zu bytes of video elementary stream", es.size());
}

/* ---- MPEG-2 sequence / picture state (parsed EE-side, like libmpeg) ------ */

struct SeqState {
    unsigned width = 0, height = 0;
    unsigned mb_w = 0, mb_h = 0;
};
struct PicState {
    unsigned type = 0; /* 1 I, 2 P, 3 B */
    unsigned fcode[2][2] = {{15, 15}, {15, 15}};
    unsigned idp = 0, structure = 3;
    unsigned fpfd = 1, conceal = 0, qst = 0, ivf = 0, as = 0;
};

constexpr int MB_INTRA = 1, MB_PATTERN = 2, MB_BACKWARD = 4, MB_FORWARD = 8, MB_QUANT = 16;

/* Default quantizer matrices, ISO 13818-2 6.3.11, in transmission (zigzag)
 * order as SETIQ expects them. */
constexpr uint8_t kDefaultIntraQ[64] = {
    8, 16, 19, 22, 26, 27, 29, 34, 16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38, 22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48, 26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69, 27, 29, 35, 38, 46, 56, 69, 83,
};

SeqState g_seq;
PicState g_pic;

/* Reconstructed I frame planes. */
std::vector<uint8_t> g_y, g_cb, g_cr;

uint64_t g_bdec_count = 0;

void parse_sequence_header() {
    advance(32);
    g_seq.width = getbits(12);
    g_seq.height = getbits(12);
    advance(4 + 4 + 18 + 1 + 10 + 1); /* aspect, frame rate, bitrate, marker, vbv, constrained */
    if (getbits(1)) { /* load_intra_quantiser_matrix: in-band SETIQ */
        w32(IPU_CMD, 0x50000000u);
        cmd_result();
        rt_log("selftest", "sequence header loads a custom intra matrix");
    }
    if (getbits(1)) {
        w32(IPU_CMD, 0x58000000u);
        cmd_result();
        rt_log("selftest", "sequence header loads a custom non-intra matrix");
    }
    g_seq.mb_w = (g_seq.width + 15) / 16;
    g_seq.mb_h = (g_seq.height + 15) / 16;
    rt_log("selftest", "sequence: %ux%u (%ux%u macroblocks)", g_seq.width, g_seq.height, g_seq.mb_w, g_seq.mb_h);
}

void parse_picture_header() {
    advance(32);
    advance(10); /* temporal reference */
    g_pic.type = getbits(3);
    advance(16); /* vbv_delay */
    if (g_pic.type == 2) advance(4);      /* MPEG1 legacy full_pel/f_code */
    else if (g_pic.type == 3) advance(8);
}

void parse_extension() {
    advance(32);
    unsigned id = getbits(4);
    if (id == 8) { /* picture coding extension */
        for (int i = 0; i < 4; ++i) g_pic.fcode[i >> 1][i & 1] = getbits(4);
        g_pic.idp = getbits(2);
        g_pic.structure = getbits(2);
        advance(1); /* top_field_first */
        g_pic.fpfd = getbits(1);
        g_pic.conceal = getbits(1);
        g_pic.qst = getbits(1);
        g_pic.ivf = getbits(1);
        g_pic.as = getbits(1);
        advance(3); /* repeat_first_field, chroma_420_type, progressive_frame */
        if (getbits(1)) advance(20); /* composite display */
        rt_log("selftest", "pic coding ext: fcode %u/%u %u/%u idp=%u struct=%u fpfd=%u conceal=%u qst=%u ivf=%u as=%u",
            g_pic.fcode[0][0], g_pic.fcode[0][1], g_pic.fcode[1][0], g_pic.fcode[1][1],
            g_pic.idp, g_pic.structure, g_pic.fpfd, g_pic.conceal, g_pic.qst, g_pic.ivf, g_pic.as);
        /* Program IPU_CTRL exactly the way libmpeg does before slices. */
        w32(IPU_CTRL, (g_pic.idp << 16) | (g_pic.as << 20) | (g_pic.ivf << 21) |
            (g_pic.qst << 22) | (g_pic.type << 24));
        if (g_pic.structure != 3) {
            rt_fatal("selftest", nullptr, "field picture (structure %u); not expected in this stream", g_pic.structure);
        }
    } else {
        /* Other extensions: skip to the next start code. */
        find_start_code();
    }
}

void bdec(bool mbi, bool dcr, bool dt, unsigned qsc) {
    w32(IPU_CMD, 0x20000000u | (mbi ? 1u << 27 : 0) | (dcr ? 1u << 26 : 0) |
        (dt ? 1u << 25 : 0) | (qsc << 16));
    cmd_result();
    ++g_bdec_count;
}

/* One motion vector component: motion_code VLC (+ residual bits), and for
 * dual prime the dmvector VLC. */
void parse_mv_component(int dir, int sv, bool dual) {
    uint32_t r = vdec(2); /* motion code */
    int16_t code = (int16_t)(r & 0xFFFF);
    if (r == 0) rt_fatal("selftest", nullptr, "invalid motion code VLC");
    unsigned rsize = g_pic.fcode[dir][sv] - 1;
    if (code != 0 && rsize) advance(rsize);
    if (dual) {
        if (vdec(3) == 0) rt_fatal("selftest", nullptr, "invalid dmvector VLC");
    }
}

/* Parses forward (and for B, backward) motion vectors EE-side, per the
 * frame-picture motion types (1 field, 2 frame, 3 dual prime). */
void parse_motion_vectors(unsigned modes, unsigned motion_type) {
    for (int dir = 0; dir < 2; ++dir) {
        if (!(modes & (dir ? MB_BACKWARD : MB_FORWARD))) continue;
        switch (motion_type) {
            case 1: /* field MC in a frame picture: two vectors */
                for (int v = 0; v < 2; ++v) {
                    advance(1); /* motion_vertical_field_select */
                    parse_mv_component(dir, 0, false);
                    parse_mv_component(dir, 1, false);
                }
                break;
            case 3: /* dual prime: one vector with dmvectors */
                parse_mv_component(dir, 0, true);
                parse_mv_component(dir, 1, true);
                break;
            default: /* frame MC */
                parse_mv_component(dir, 0, false);
                parse_mv_component(dir, 1, false);
                break;
        }
    }
}

/* Decodes one slice of the current picture. store=true places BDEC output
 * into the I-frame planes. Returns decoded macroblock count. */
unsigned decode_slice(uint32_t start_code, bool store) {
    unsigned row = (start_code & 0xFF) - 1;
    advance(32);
    unsigned qsc = getbits(5);
    while (getbits(1)) advance(8); /* extra slice information */

    int addr = (int)(row * g_seq.mb_w) - 1;
    bool need_dcr = true;
    unsigned decoded = 0;
    int16_t mb16[384]; /* Y 256 + Cb 64 + Cr 64 */

    for (;;) {
        /* Macroblock address increment (escape adds 33). */
        int inc = 0;
        bool slice_end = false;
        for (;;) {
            uint32_t r = vdec(0);
            uint16_t v = (uint16_t)(r & 0xFFFF);
            if (r == 0) { slice_end = true; break; } /* start code follows */
            if (v == 0x23) { inc += 33; continue; }
            if (v == 0x22) continue; /* stuffing */
            inc += v;
            break;
        }
        if (slice_end) break;
        if (inc > 1) need_dcr = true; /* skipped macroblocks reset DC prediction */
        addr += inc;

        uint32_t r = vdec(1);
        unsigned modes = r & 0xFFFF;
        if (modes == 0) {
            rt_fatal("selftest", nullptr,
                "invalid macroblock type at mb %d (slice row %u, %u decoded, inc %d, next bits 0x%08x)",
                addr, row, decoded, inc, fdec(0));
        }

        unsigned motion_type = 2; /* frame MC when frame_pred_frame_dct */
        if (!g_pic.fpfd && (modes & (MB_FORWARD | MB_BACKWARD))) motion_type = getbits(2);
        bool dt = false;
        if (!g_pic.fpfd && (modes & (MB_PATTERN | MB_INTRA))) dt = getbits(1);
        if (modes & MB_QUANT) qsc = getbits(5);
        if (modes & MB_INTRA) {
            if (g_pic.conceal) { parse_motion_vectors(MB_FORWARD, 2); advance(1); }
        } else {
            parse_motion_vectors(modes, motion_type);
        }

        if (modes & (MB_INTRA | MB_PATTERN)) {
            bdec((modes & MB_INTRA) != 0, (modes & MB_INTRA) ? need_dcr : true, dt, qsc);
            uint32_t ctrl = ipu_ctrl();
            if (ctrl & (1u << 14)) {
                rt_fatal("selftest", nullptr, "ECD after BDEC at mb %d (ctrl=0x%08x)", addr, ctrl);
            }
            size_t got = rt_ipu_test_read_out((uint8_t*)mb16, sizeof(mb16));
            if (got != sizeof(mb16)) {
                rt_fatal("selftest", nullptr, "BDEC produced %zu bytes, expected 768", got);
            }
            if (store) {
                unsigned mx = (unsigned)addr % g_seq.mb_w, my = (unsigned)addr / g_seq.mb_w;
                for (int yy = 0; yy < 16; ++yy)
                    for (int xx = 0; xx < 16; ++xx) {
                        int v = mb16[yy * 16 + xx];
                        g_y[(my * 16 + yy) * g_seq.width + mx * 16 + xx] =
                            (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
                    }
                for (int yy = 0; yy < 8; ++yy)
                    for (int xx = 0; xx < 8; ++xx) {
                        int cbv = mb16[256 + yy * 8 + xx];
                        int crv = mb16[320 + yy * 8 + xx];
                        size_t at = (my * 8 + yy) * (g_seq.width / 2) + mx * 8 + xx;
                        g_cb[at] = (uint8_t)(cbv < 0 ? 0 : (cbv > 255 ? 255 : cbv));
                        g_cr[at] = (uint8_t)(crv < 0 ? 0 : (crv > 255 ? 255 : crv));
                    }
            }
            ++decoded;
            need_dcr = !(modes & MB_INTRA);
            /* SCD from the BDEC tail scan means a start code follows. */
            if (ipu_ctrl() & (1u << 15)) break;
        } else {
            need_dcr = true;
        }
    }
    return decoded;
}

void write_bmp(const char* path, const uint8_t* rgba, unsigned w, unsigned h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) rt_fatal("selftest", nullptr, "cannot open '%s' for writing", path);
    uint32_t rowbytes = w * 3;
    uint32_t pad = (4 - (rowbytes & 3)) & 3;
    uint32_t imgsize = (rowbytes + pad) * h;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    uint32_t fsize = 54 + imgsize;
    std::memcpy(hdr + 2, &fsize, 4);
    uint32_t off = 54; std::memcpy(hdr + 10, &off, 4);
    uint32_t bisize = 40; std::memcpy(hdr + 14, &bisize, 4);
    std::memcpy(hdr + 18, &w, 4);
    std::memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    std::memcpy(hdr + 34, &imgsize, 4);
    std::fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> rowbuf(rowbytes + pad, 0);
    for (int y = (int)h - 1; y >= 0; --y) {
        for (unsigned x = 0; x < w; ++x) {
            const uint8_t* p = rgba + (y * w + x) * 4;
            rowbuf[x * 3 + 0] = p[2];
            rowbuf[x * 3 + 1] = p[1];
            rowbuf[x * 3 + 2] = p[0];
        }
        std::fwrite(rowbuf.data(), 1, rowbuf.size(), f);
    }
    std::fclose(f);
}

} // namespace

int main() {
    rt_log("selftest", "IPU selftest starting");
    rt_iso_mount();

    std::vector<uint8_t> es;
    const char* skip_env = std::getenv("ICORECOMP_IPU_SELFTEST_SKIP");
    uint32_t skip = skip_env ? (uint32_t)std::strtoul(skip_env, nullptr, 0) : 4000;
    extract_video_es(es, 6u << 20, skip);
    if (es.size() < (1u << 20)) {
        rt_fatal("selftest", nullptr, "too little video ES extracted (%zu bytes)", es.size());
    }

    /* Reset, load default matrices (fed out of band, before the ES), then
     * feed the stream. */
    w32(IPU_CTRL, 1u << 30);
    w32(IPU_CMD, 0x00000000u); /* BCLR */
    rt_ipu_test_feed(kDefaultIntraQ, 64);
    w32(IPU_CMD, 0x50000000u);
    cmd_result();
    {
        uint8_t flat[64];
        std::memset(flat, 16, sizeof(flat));
        rt_ipu_test_feed(flat, 64);
        w32(IPU_CMD, 0x58000000u);
        cmd_result();
    }
    rt_ipu_test_feed(es.data(), es.size());

    /* Walk the stream: sequence header, then decode the first I picture
     * fully and the first P picture's residuals. */
    unsigned i_mbs = 0, p_mbs = 0, p_slices = 0, b_mbs = 0, b_slices = 0;
    bool seq_seen = false, i_done = false, p_done = false, b_done = false, skip_pic = false;
    while (!(i_done && p_done && b_done)) {
        find_start_code();
        uint32_t sc = peekbits(32);
        if (sc == 0x1B3) {
            parse_sequence_header();
            g_y.assign((size_t)g_seq.width * g_seq.height, 0);
            g_cb.assign((size_t)g_seq.width * g_seq.height / 4, 128);
            g_cr.assign((size_t)g_seq.width * g_seq.height / 4, 128);
            seq_seen = true;
        } else if (!seq_seen) {
            advance(32); /* mid-stream entry: scan until the sequence header */
        } else if (sc == 0x1B5) {
            parse_extension();
        } else if (sc == 0x1B8) {
            advance(32 + 27); /* GOP header */
        } else if (sc == 0x100) {
            parse_picture_header();
            rt_log("selftest", "picture type %u", g_pic.type);
            /* Decode the first I fully, plus residual passes over the first
             * B and first P (stream order in an open GOP is I B B P ...). */
            skip_pic = (g_pic.type == 1 && i_done) || (g_pic.type == 3 && (b_done || !i_done)) ||
                (g_pic.type == 2 && (p_done || !i_done));
        } else if (sc >= 0x101 && sc <= 0x1AF) {
            if (skip_pic) {
                advance(32); /* the scanner walks through the slice data */
                continue;
            }
            bool is_i = g_pic.type == 1;
            unsigned n = decode_slice(sc, is_i);
            if (is_i) {
                i_mbs += n;
                if (i_mbs >= g_seq.mb_w * g_seq.mb_h) i_done = true;
            } else if (g_pic.type == 2) {
                p_mbs += n;
                if (++p_slices >= g_seq.mb_h) p_done = true;
            } else {
                b_mbs += n;
                if (++b_slices >= g_seq.mb_h) b_done = true;
            }
        } else if (sc == 0x1B7) {
            break;
        } else {
            advance(32); /* user data etc: resume scanning */
        }
    }

    if (i_mbs != g_seq.mb_w * g_seq.mb_h) {
        rt_fatal("selftest", nullptr, "I picture decoded %u macroblocks, expected %u",
            i_mbs, g_seq.mb_w * g_seq.mb_h);
    }
    rt_log("selftest", "I picture: %u macroblocks; P pass: %u coded MBs / %u slices; B pass: %u coded MBs / %u slices;"
        " %" PRIu64 " BDECs total", i_mbs, p_mbs, p_slices, b_mbs, b_slices, g_bdec_count);

    /* Luma statistics. */
    double sum = 0, sum2 = 0;
    for (uint8_t v : g_y) { sum += v; sum2 += (double)v * v; }
    double mean = sum / g_y.size();
    double var = sum2 / g_y.size() - mean * mean;
    rt_log("selftest", "I frame luma: mean %.1f stddev %.1f", mean, var > 0 ? std::sqrt(var) : 0.0);
    if (mean < 10.0 || mean > 245.0 || var < 4.0) {
        rt_fatal("selftest", nullptr, "implausible luma statistics (mean %.1f var %.1f)", mean, var);
    }
    if (p_mbs == 0) rt_fatal("selftest", nullptr, "P residual pass decoded no macroblocks");

    /* CSC pass: convert the reconstructed I frame through the IPU. */
    w32(IPU_CMD, 0x00000000u); /* BCLR: drop the remaining ES */
    w32(IPU_CMD, 0x90000000u); /* SETTH 0/0 */
    std::vector<uint8_t> rgba((size_t)g_seq.width * g_seq.height * 4);
    uint8_t mb8[384], rgbmb[1024];
    for (unsigned my = 0; my < g_seq.mb_h; ++my) {
        for (unsigned mx = 0; mx < g_seq.mb_w; ++mx) {
            for (int yy = 0; yy < 16; ++yy)
                for (int xx = 0; xx < 16; ++xx)
                    mb8[yy * 16 + xx] = g_y[(my * 16 + yy) * g_seq.width + mx * 16 + xx];
            for (int yy = 0; yy < 8; ++yy)
                for (int xx = 0; xx < 8; ++xx) {
                    size_t at = (my * 8 + yy) * (g_seq.width / 2) + mx * 8 + xx;
                    mb8[256 + yy * 8 + xx] = g_cb[at];
                    mb8[320 + yy * 8 + xx] = g_cr[at];
                }
            rt_ipu_test_feed(mb8, sizeof(mb8));
            w32(IPU_CMD, 0x70000001u); /* CSC, 1 macroblock */
            cmd_result();
            size_t got = rt_ipu_test_read_out(rgbmb, sizeof(rgbmb));
            if (got != sizeof(rgbmb)) rt_fatal("selftest", nullptr, "CSC produced %zu bytes, expected 1024", got);
            for (int yy = 0; yy < 16; ++yy)
                std::memcpy(&rgba[((my * 16 + yy) * g_seq.width + mx * 16) * 4], &rgbmb[yy * 64], 64);
        }
    }

    const char* out = std::getenv("ICORECOMP_IPU_SELFTEST_OUT");
    std::string path = out ? out : "/tmp/ipu_selftest_frame0.bmp";
    write_bmp(path.c_str(), rgba.data(), g_seq.width, g_seq.height);
    rt_log("selftest", "wrote decoded I frame to %s", path.c_str());
    rt_log("selftest", "IPU selftest PASSED");
    return 0;
}
