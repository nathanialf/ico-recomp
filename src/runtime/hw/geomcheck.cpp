/* hw/geomcheck.cpp: vertex-level validation of the GIF stream.
 *
 * Off by default; ICORECOMP_VERBOSE=geom turns it on. It re-parses every
 * packet the runtime hands to the GS and reports, per field, the two
 * conditions that mark geometry the VU1 program should never have emitted:
 *
 *   Q <= 0 (or not finite) on a perspective-textured vertex. Q is 1/w, so
 *   a non-positive Q is a vertex behind the eye. VU1 microprograms reject
 *   or clip those; one that reaches the GS has had its X/Y divided by a
 *   negative w and lands mirrored somewhere arbitrary, and its texture
 *   coordinates run through a sign change across the primitive.
 *
 *   A primitive whose screen-space bounding box is wider or taller than
 *   the 2048-pixel guard band. Nothing the game draws is that large; a
 *   box that size means at least one vertex of the primitive was
 *   projected wrong, and the primitive is drawn as a smear across the
 *   whole framebuffer.
 *
 * Both are reported against the VU1 microprogram hash that was bound when
 * the packet was kicked, which is what turns "the picture is wrong" into
 * "this microprogram's clip path is wrong".
 *
 * GIF tag and register layouts here (PACKED/REGLIST field placement,
 * XYOFFSET, the 12.4 screen coordinate format) are public PS2 hardware
 * documentation (ps2tek), same as the framing in gif.cpp.
 *
 * This is a diagnostic reader. It never changes what is submitted.
 */
#include "hw.h"

#include <cinttypes>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <map>

namespace {

/* Screen-space span, in pixels, past which a primitive is treated as
 * mis-projected rather than merely large. The GS guard band is +/-2048
 * around the XYOFFSET origin, so a primitive spanning more than that
 * cannot be on screen in one piece however the camera is placed. */
constexpr float kGuardBandPx = 2048.0f;

struct Bucket {
    uint64_t prims = 0;
    uint64_t verts = 0;
    uint64_t bad_q = 0;      /* vertices with Q <= 0 or not finite */
    uint64_t wild_prims = 0; /* primitives spanning more than the guard band */
    uint64_t kicks = 0;      /* XGKICKs attributed to this program */
    uint64_t kicks_clipped = 0; /* of those, with a nonzero clip judgment */
};

/* Per-field totals, keyed by the VU1 program hash for PATH1 and by a
 * synthetic key for the paths that carry no microprogram. */
constexpr uint32_t kKeyPath2 = 0xFFFFFFFEu;
constexpr uint32_t kKeyPath3 = 0xFFFFFFFDu;

std::map<uint32_t, Bucket> g_field;
Bucket g_run;
uint64_t g_field_no = 0;

/* Worst primitive seen this field, for the one example line. */
struct Worst {
    bool seen = false;
    uint32_t key = 0;
    uint32_t prim = 0;
    uint32_t nverts = 0;
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    float qmin = 0, qmax = 0;
};
Worst g_worst;

const char* prim_kind(uint32_t prim) {
    static const char* const kNames[8] = {
        "point", "line", "linestrip", "tri", "tristrip", "trifan", "sprite", "invalid",
    };
    return kNames[prim & 7];
}

/* XYOFFSET is GS state and outlives the packet that set it: a game sets it
 * once and draws for the rest of the frame. Keeping it here rather than in
 * ScanState is what makes the screen-space span meaningful for the packets
 * that do not re-send it. */
struct Offsets {
    float x1 = 0, y1 = 0; /* XYOFFSET_1, in pixels */
    float x2 = 0, y2 = 0; /* XYOFFSET_2 */
};
Offsets g_off;

/* State carried across the registers of one packet. */
struct ScanState {
    uint32_t prim = 0;
    bool have_prim = false;
    float q = 1.0f;
    /* Accumulator for the primitive currently being assembled. */
    uint32_t nverts = 0;
    float x0 = 0, x1 = 0, y0 = 0, y1 = 0, qmin = 0, qmax = 0;
};

float bits2f(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

void close_prim(ScanState& st, uint32_t key) {
    if (st.nverts == 0) return;
    Bucket& b = g_field[key];
    ++b.prims;
    ++g_run.prims;
    const float w = st.x1 - st.x0;
    const float h = st.y1 - st.y0;
    if (w > kGuardBandPx || h > kGuardBandPx) {
        ++b.wild_prims;
        ++g_run.wild_prims;
        if (!g_worst.seen || (w + h) > (g_worst.x1 - g_worst.x0) + (g_worst.y1 - g_worst.y0)) {
            g_worst = Worst{true, key, st.prim, st.nverts, st.x0, st.x1, st.y0, st.y1, st.qmin, st.qmax};
        }
    }
    st.nverts = 0;
}

/* One vertex, in raw 12.4 GS screen coordinates. */
void add_vertex(ScanState& st, uint32_t key, uint32_t xy_lo) {
    const bool ctxt2 = (st.prim >> 9) & 1;
    const float ofx = ctxt2 ? g_off.x2 : g_off.x1;
    const float ofy = ctxt2 ? g_off.y2 : g_off.y1;
    const float sx = float(xy_lo & 0xFFFF) / 16.0f - ofx;
    const float sy = float((xy_lo >> 16) & 0xFFFF) / 16.0f - ofy;

    Bucket& b = g_field[key];
    ++b.verts;
    ++g_run.verts;

    /* Q only means anything on a textured primitive addressed by ST
     * (PRIM.TME set, PRIM.FST clear); with UV addressing there is no
     * perspective divide and Q is left wherever it was. */
    const bool tme = (st.prim >> 4) & 1;
    const bool fst = (st.prim >> 8) & 1;
    if (st.have_prim && tme && !fst && (!(st.q > 0.0f) || !std::isfinite(st.q))) {
        ++b.bad_q;
        ++g_run.bad_q;
    }

    if (st.nverts == 0) {
        st.x0 = st.x1 = sx;
        st.y0 = st.y1 = sy;
        st.qmin = st.qmax = st.q;
    } else {
        if (sx < st.x0) st.x0 = sx;
        if (sx > st.x1) st.x1 = sx;
        if (sy < st.y0) st.y0 = sy;
        if (sy > st.y1) st.y1 = sy;
        if (st.q < st.qmin) st.qmin = st.q;
        if (st.q > st.qmax) st.qmax = st.q;
    }
    ++st.nverts;

    /* Close as soon as this primitive kind has its vertices, instead of
     * accumulating everything between two PRIM writes. A tristrip sends
     * PRIM once and then hundreds of vertices, so folding them into one
     * bounding box measured whole draw batches: the box spans the entire
     * batch and trips the guard band even when every triangle in it is
     * small, while the primitive count undercounts by orders of magnitude.
     * PRIM[2:0]: 0 point, 1 line, 2 line strip, 3 triangle, 4 tristrip,
     * 5 trifan, 6 sprite, 7 reserved. */
    const uint32_t kind = st.prim & 7;
    uint32_t need = 3;
    switch (kind) {
        case 0: need = 1; break;             /* point */
        case 1: case 2: need = 2; break;     /* line, line strip */
        case 6: need = 2; break;             /* sprite: two corners */
        default: need = 3; break;            /* triangle, strip, fan */
    }
    if (st.nverts >= need) {
        close_prim(st, key);
        /* Strips and fans share vertices with the next primitive, but the
         * bounding box of the following one is what matters here, so the
         * accumulator simply restarts. This slightly undercounts primitives
         * in a strip; it does not inflate the box, which is the measurement
         * that matters. */
    }
}

void set_prim(ScanState& st, uint32_t key, uint32_t prim) {
    close_prim(st, key);
    st.prim = prim;
    st.have_prim = true;
}

} // namespace

/* The CLIP judgment register as it stands when a microprogram kicks its
 * packet. A program whose clip-cull path is alive leaves nonzero
 * judgments here on any frame with geometry near a frustum edge; one that
 * never does has a dead cull path, which is a different bug from a cull
 * path that runs and decides wrong. */
void rt_geom_note_clip(uint32_t clip, uint32_t vu1_hash) {
    Bucket& b = g_field[vu1_hash];
    ++b.kicks;
    ++g_run.kicks;
    if (clip != 0) {
        ++b.kicks_clipped;
        ++g_run.kicks_clipped;
    }
}

void rt_geom_scan(int path, const uint8_t* data, uint32_t qwords, uint32_t vu1_hash) {
    uint32_t key = vu1_hash;
    if (path == 1) key = kKeyPath2;
    else if (path == 2) key = kKeyPath3;

    ScanState st;
    uint32_t i = 0;
    while (i < qwords) {
        uint64_t lo, hi;
        std::memcpy(&lo, data + (size_t)i * 16, 8);
        std::memcpy(&hi, data + (size_t)i * 16 + 8, 8);
        ++i;
        const uint32_t nloop = (uint32_t)(lo & 0x7FFF);
        const bool pre = (lo >> 46) & 1;
        const uint32_t prim = (uint32_t)((lo >> 47) & 0x7FF);
        const uint32_t flg = (uint32_t)((lo >> 58) & 3);
        uint32_t nreg = (uint32_t)((lo >> 60) & 15);
        if (nreg == 0) nreg = 16;
        if (pre) set_prim(st, key, prim);

        if (flg == 0) { /* PACKED */
            for (uint32_t l = 0; l < nloop; ++l) {
                for (uint32_t r = 0; r < nreg; ++r) {
                    if (i >= qwords) return;
                    uint64_t d0, d1;
                    std::memcpy(&d0, data + (size_t)i * 16, 8);
                    std::memcpy(&d1, data + (size_t)i * 16 + 8, 8);
                    ++i;
                    switch ((uint32_t)((hi >> (4 * r)) & 15)) {
                        case 0x0: /* PRIM */
                            set_prim(st, key, (uint32_t)(d0 & 0x7FF));
                            break;
                        case 0x2: /* ST: S, T, and the Q that the next XYZ uses */
                            st.q = bits2f((uint32_t)(d1 & 0xFFFFFFFFu));
                            break;
                        case 0x4: /* XYZF2 */
                        case 0x5: /* XYZ2 */
                            add_vertex(st, key, (uint32_t)((d0 & 0xFFFFu) | (((d0 >> 32) & 0xFFFFu) << 16)));
                            break;
                        case 0xE: { /* A+D */
                            const uint32_t addr = (uint32_t)(d1 & 0xFF);
                            if (addr == 0x00) {
                                set_prim(st, key, (uint32_t)(d0 & 0x7FF));
                            } else if (addr == 0x01) { /* RGBAQ carries Q too */
                                st.q = bits2f((uint32_t)((d0 >> 32) & 0xFFFFFFFFu));
                            } else if (addr == 0x05 || addr == 0x04) {
                                add_vertex(st, key, (uint32_t)(d0 & 0xFFFFFFFFu));
                            } else if (addr == 0x18) { /* XYOFFSET_1 */
                                close_prim(st, key);
                                g_off.x1 = float(d0 & 0xFFFFu) / 16.0f;
                                g_off.y1 = float((d0 >> 32) & 0xFFFFu) / 16.0f;
                            } else if (addr == 0x19) { /* XYOFFSET_2 */
                                close_prim(st, key);
                                g_off.x2 = float(d0 & 0xFFFFu) / 16.0f;
                                g_off.y2 = float((d0 >> 32) & 0xFFFFu) / 16.0f;
                            }
                            break;
                        }
                        default:
                            break;
                    }
                }
            }
        } else if (flg == 1) { /* REGLIST: two 64-bit register writes per qword */
            const uint64_t total = (uint64_t)nloop * nreg;
            for (uint64_t n = 0; n < total; ++n) {
                if (i >= qwords) return;
                uint64_t qw[2];
                std::memcpy(qw, data + (size_t)i * 16, 16);
                const uint64_t d = qw[n & 1];
                switch ((uint32_t)((hi >> (4 * (n % nreg))) & 15)) {
                    case 0x0:
                        set_prim(st, key, (uint32_t)(d & 0x7FF));
                        break;
                    case 0x1: /* RGBAQ */
                        st.q = bits2f((uint32_t)((d >> 32) & 0xFFFFFFFFu));
                        break;
                    case 0x4:
                    case 0x5: /* XYZ2/XYZF2: X in 0..15, Y in 16..31 */
                        add_vertex(st, key, (uint32_t)(d & 0xFFFFFFFFu));
                        break;
                    default:
                        break;
                }
                if (n & 1) ++i;
            }
            if (total & 1) ++i; /* odd count leaves a half-used qword */
        } else if (flg == 2) { /* IMAGE */
            i += nloop;
        }
    }
    close_prim(st, key);
}

void rt_geom_field(unsigned field) {
    ++g_field_no;
    uint64_t verts = 0, bad_q = 0, prims = 0, wild = 0;
    for (const auto& kv : g_field) {
        verts += kv.second.verts;
        bad_q += kv.second.bad_q;
        prims += kv.second.prims;
        wild += kv.second.wild_prims;
    }
    if (verts == 0) {
        g_field.clear();
        g_worst = Worst{};
        return;
    }
    rt_logv("geom", "field %" PRIu64 " (parity %u): prims=%" PRIu64 " verts=%" PRIu64
        "  behind-eye verts=%" PRIu64 " (%.2f%%)  oversized prims=%" PRIu64 " (%.2f%%)",
        g_field_no, field, prims, verts,
        bad_q, 100.0 * double(bad_q) / double(verts),
        wild, 100.0 * double(wild) / double(prims ? prims : 1));

    for (const auto& kv : g_field) {
        if (kv.second.bad_q == 0 && kv.second.wild_prims == 0) continue;
        char who[64];
        if (kv.first == kKeyPath2) std::snprintf(who, sizeof(who), "PATH2 (VIF1 DIRECT)");
        else if (kv.first == kKeyPath3) std::snprintf(who, sizeof(who), "PATH3 (DMA ch2)");
        else std::snprintf(who, sizeof(who), "PATH1 vu1 hash=0x%08x", kv.first);
        rt_logv("geom", "  %-28s prims=%" PRIu64 " verts=%" PRIu64
            " behind-eye=%" PRIu64 " oversized=%" PRIu64 " kicks=%" PRIu64 " with-clip=%" PRIu64,
            who, kv.second.prims, kv.second.verts, kv.second.bad_q, kv.second.wild_prims,
            kv.second.kicks, kv.second.kicks_clipped);
    }
    if (g_worst.seen) {
        char who[64];
        if (g_worst.key == kKeyPath2) std::snprintf(who, sizeof(who), "PATH2");
        else if (g_worst.key == kKeyPath3) std::snprintf(who, sizeof(who), "PATH3");
        else std::snprintf(who, sizeof(who), "vu1 0x%08x", g_worst.key);
        rt_logv("geom", "  worst: %s prim=0x%03x %s%s%s n=%u  x %.0f..%.0f  y %.0f..%.0f  Q %g..%g",
            who, g_worst.prim, prim_kind(g_worst.prim),
            ((g_worst.prim >> 4) & 1) ? " tme" : "",
            ((g_worst.prim >> 6) & 1) ? " abe" : "",
            g_worst.nverts, g_worst.x0, g_worst.x1, g_worst.y0, g_worst.y1,
            g_worst.qmin, g_worst.qmax);
    }
    g_field.clear();
    g_worst = Worst{};
}

void rt_geom_report() {
    if (g_run.verts == 0) return;
    rt_log("geom", "totals over %" PRIu64 " fields: prims=%" PRIu64 " verts=%" PRIu64
        "  behind-eye verts=%" PRIu64 " (%.2f%%)  oversized prims=%" PRIu64 " (%.2f%%)",
        g_field_no, g_run.prims, g_run.verts,
        g_run.bad_q, 100.0 * double(g_run.bad_q) / double(g_run.verts),
        g_run.wild_prims, 100.0 * double(g_run.wild_prims) / double(g_run.prims ? g_run.prims : 1));
    rt_log("geom", "XGKICKs=%" PRIu64 ", of which %" PRIu64 " (%.2f%%) carried a nonzero CLIP judgment",
        g_run.kicks, g_run.kicks_clipped,
        100.0 * double(g_run.kicks_clipped) / double(g_run.kicks ? g_run.kicks : 1));
}
