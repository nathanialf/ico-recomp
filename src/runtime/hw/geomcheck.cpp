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

/* Totals keyed by (VU1 program hash, MSCAL entry pc) for PATH1, and by a
 * synthetic key for the paths that carry no microprogram. The entry pc is
 * in the key because these programs have ten entry points doing quite
 * different work: "normal_c emits vertices behind the eye" only becomes
 * actionable once it says which entry did it.
 *
 * Three scopes: g_field is cleared every field and drives the per-field
 * line, g_window is cleared by the profiler summary, and g_run runs to the
 * end. The two wider scopes are folded from g_field at the field boundary
 * rather than incremented per vertex, which keeps the per-vertex path to
 * one already-resolved pointer. */
using Key = uint64_t;
constexpr Key kKeyPath2 = 0xFFFFFFFEull << 32;
constexpr Key kKeyPath3 = 0xFFFFFFFDull << 32;

Key path1_key(uint32_t hash, uint32_t entry_pc) {
    return ((Key)hash << 32) | entry_pc;
}

std::map<Key, Bucket> g_field;
std::map<Key, Bucket> g_window;
Bucket g_run;
uint64_t g_field_no = 0;
uint64_t g_window_fields = 0;

void fold(Bucket& dst, const Bucket& src) {
    dst.prims += src.prims;
    dst.verts += src.verts;
    dst.bad_q += src.bad_q;
    dst.wild_prims += src.wild_prims;
    dst.kicks += src.kicks;
    dst.kicks_clipped += src.kicks_clipped;
}

/* Worst primitive seen this field, for the one example line. */
struct Worst {
    bool seen = false;
    Key key = 0;
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
    /* The GS vertex queue, three deep. Modelled rather than accumulated
     * because of ADC: a vertex with the skip bit set still enters the
     * queue and still forms part of the following triangles of a strip,
     * it only suppresses the drawing kick at its own position. Dropping
     * such a vertex, or restarting the strip at it, both close primitives
     * over vertex sets the GS never drew. */
    float qx[3] = {0, 0, 0};
    float qy[3] = {0, 0, 0};
    float qq[3] = {0, 0, 0};
    uint32_t nverts = 0;   /* vertices currently queued, 0..3 */
    /* Resolved once per packet in rt_geom_scan. This used to be a
     * std::map lookup per vertex, which made the checker cost more than
     * the geometry it was measuring and left a geom run unreadable as a
     * performance capture. */
    Bucket* b = nullptr;
    Key key = 0;
};

float bits2f(uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

/* Closes one primitive over the newest `n` queued vertices. */
void close_prim(ScanState& st, uint32_t n) {
    if (n == 0 || n > st.nverts) return;
    ++st.b->prims;
    const uint32_t first = st.nverts - n;
    float x0 = st.qx[first], x1 = x0, y0 = st.qy[first], y1 = y0;
    float qmin = st.qq[first], qmax = qmin;
    for (uint32_t i = first + 1; i < st.nverts; ++i) {
        if (st.qx[i] < x0) x0 = st.qx[i];
        if (st.qx[i] > x1) x1 = st.qx[i];
        if (st.qy[i] < y0) y0 = st.qy[i];
        if (st.qy[i] > y1) y1 = st.qy[i];
        if (st.qq[i] < qmin) qmin = st.qq[i];
        if (st.qq[i] > qmax) qmax = st.qq[i];
    }
    const float w = x1 - x0;
    const float h = y1 - y0;
    if (w > kGuardBandPx || h > kGuardBandPx) {
        ++st.b->wild_prims;
        if (!g_worst.seen || (w + h) > (g_worst.x1 - g_worst.x0) + (g_worst.y1 - g_worst.y0)) {
            g_worst = Worst{true, st.key, st.prim, n, x0, x1, y0, y1, qmin, qmax};
        }
    }
}

/* One vertex, in raw 12.4 GS screen coordinates.
 *
 * `adc` is the GS skip bit: the vertex is queued as normal, but no drawing
 * kick happens at its position. It exists in the packet, so it counts as a
 * submitted vertex and still forms later primitives of a strip. */
void add_vertex(ScanState& st, uint32_t xy_lo, bool adc) {
    const bool ctxt2 = (st.prim >> 9) & 1;
    const float ofx = ctxt2 ? g_off.x2 : g_off.x1;
    const float ofy = ctxt2 ? g_off.y2 : g_off.y1;
    const float sx = float(xy_lo & 0xFFFF) / 16.0f - ofx;
    const float sy = float((xy_lo >> 16) & 0xFFFF) / 16.0f - ofy;

    ++st.b->verts;

    /* Q only means anything on a textured primitive addressed by ST
     * (PRIM.TME set, PRIM.FST clear); with UV addressing there is no
     * perspective divide and Q is left wherever it was. */
    const bool tme = (st.prim >> 4) & 1;
    const bool fst = (st.prim >> 8) & 1;
    if (st.have_prim && tme && !fst && (!(st.q > 0.0f) || !std::isfinite(st.q))) {
        ++st.b->bad_q;
    }

    /* Queue the vertex, dropping the oldest when full. */
    if (st.nverts == 3) {
        st.qx[0] = st.qx[1]; st.qx[1] = st.qx[2];
        st.qy[0] = st.qy[1]; st.qy[1] = st.qy[2];
        st.qq[0] = st.qq[1]; st.qq[1] = st.qq[2];
        st.nverts = 2;
    }
    st.qx[st.nverts] = sx;
    st.qy[st.nverts] = sy;
    st.qq[st.nverts] = st.q;
    ++st.nverts;

    /* PRIM[2:0]: 0 point, 1 line, 2 line strip, 3 triangle, 4 tristrip,
     * 5 trifan, 6 sprite, 7 reserved. */
    const uint32_t kind = st.prim & 7;
    uint32_t need = 3;
    bool strip = false;
    switch (kind) {
        case 0: need = 1; break;                          /* point */
        case 1: need = 2; break;                          /* line */
        case 2: need = 2; strip = true; break;            /* line strip */
        case 6: need = 2; break;                          /* sprite */
        case 4: case 5: need = 3; strip = true; break;    /* tristrip, trifan */
        default: need = 3; break;                         /* triangle */
    }
    if (st.nverts >= need) {
        if (!adc) close_prim(st, need);
        /* An independent primitive consumes its vertices; a strip or fan
         * keeps them and slides, so the next vertex completes the next
         * triangle. The kick is what ADC suppresses, not the queueing,
         * so this happens either way.
         *
         * Trifan is treated as a strip: it should pivot on the first
         * vertex rather than slide, which makes the extent of the second
         * and later triangles of a fan slightly wrong here. The five
         * programs in this game emit tristrips and sprites, so nothing
         * measured goes through that path; it is called out rather than
         * left to be discovered. */
        if (!strip) st.nverts = 0;
    }
}

void set_prim(ScanState& st, uint32_t prim) {
    /* A PRIM write restarts assembly: the queue does not carry across it. */
    st.nverts = 0;
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
    Bucket& b = g_field[path1_key(vu1_hash, rt_vu1_entry_pc())];
    ++b.kicks;
    if (clip != 0) ++b.kicks_clipped;
}

void rt_geom_scan(int path, const uint8_t* data, uint32_t qwords, uint32_t vu1_hash) {
    Key key = path1_key(vu1_hash, rt_vu1_entry_pc());
    if (path == 1) key = kKeyPath2;
    else if (path == 2) key = kKeyPath3;

    ScanState st;
    st.key = key;
    st.b = &g_field[key]; /* one map lookup for the whole packet */
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
        if (pre) set_prim(st, prim);

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
                            set_prim(st, (uint32_t)(d0 & 0x7FF));
                            break;
                        case 0x2: /* ST: S, T, and the Q that the next XYZ uses */
                            st.q = bits2f((uint32_t)(d1 & 0xFFFFFFFFu));
                            break;
                        case 0x4: /* XYZF2 */
                        case 0x5: /* XYZ2 */
                            /* PACKED XYZ2/XYZF2 carry ADC at bit 111. */
                            add_vertex(st, (uint32_t)((d0 & 0xFFFFu) | (((d0 >> 32) & 0xFFFFu) << 16)),
                                       ((d1 >> 47) & 1) != 0);
                            break;
                        case 0xE: { /* A+D */
                            const uint32_t addr = (uint32_t)(d1 & 0xFF);
                            if (addr == 0x00) {
                                set_prim(st, (uint32_t)(d0 & 0x7FF));
                            } else if (addr == 0x01) { /* RGBAQ carries Q too */
                                st.q = bits2f((uint32_t)((d0 >> 32) & 0xFFFFFFFFu));
                            } else if (addr == 0x05 || addr == 0x04) {
                                /* XYZ2/XYZF2 as a register value carry no
                                 * ADC bit; that exists only in the PACKED
                                 * layout. The no-draw form is a different
                                 * register, handled below. */
                                add_vertex(st, (uint32_t)(d0 & 0xFFFFFFFFu), false);
                            } else if (addr == 0x0D || addr == 0x0C) {
                                /* XYZ3/XYZF3: queued, never kicked. */
                                add_vertex(st, (uint32_t)(d0 & 0xFFFFFFFFu), true);
                            } else if (addr == 0x18) { /* XYOFFSET_1 */
                                st.nverts = 0;
                                g_off.x1 = float(d0 & 0xFFFFu) / 16.0f;
                                g_off.y1 = float((d0 >> 32) & 0xFFFFu) / 16.0f;
                            } else if (addr == 0x19) { /* XYOFFSET_2 */
                                st.nverts = 0;
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
                        set_prim(st, (uint32_t)(d & 0x7FF));
                        break;
                    case 0x1: /* RGBAQ */
                        st.q = bits2f((uint32_t)((d >> 32) & 0xFFFFFFFFu));
                        break;
                    case 0x4:
                    case 0x5: /* XYZ2/XYZF2: X in 0..15, Y in 16..31. No ADC
                               * bit in this layout; bit 47 is Z bit 15. */
                        add_vertex(st, (uint32_t)(d & 0xFFFFFFFFu), false);
                        break;
                    case 0xC:
                    case 0xD: /* XYZ3/XYZF3: queued, never kicked. */
                        add_vertex(st, (uint32_t)(d & 0xFFFFFFFFu), true);
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
}

/* Names one bucket: which path, and for PATH1 which microprogram and
 * which of its entry points. Hashes are not resolved to names here; that
 * would copy the decomp's symbols into this repo. `recomp-cli vu1` prints
 * the hash for each name. */
namespace {

void describe(char* out, size_t n, Key key) {
    if (key == kKeyPath2) std::snprintf(out, n, "PATH2 (VIF1 DIRECT)");
    else if (key == kKeyPath3) std::snprintf(out, n, "PATH3 (DMA ch2)");
    else std::snprintf(out, n, "vu1 0x%08x pc=0x%04x",
        (uint32_t)(key >> 32), (uint32_t)(key & 0xFFFFFFFFu));
}

} // namespace

/* One block under the profiler summary: how much geometry the window
 * carried and how much of it is geometry no correct VU1 program emits.
 * This is what the profiler's own buckets cannot say. They count zone
 * entries, so a rise in "vu1" reads the same whether each call got bigger
 * batches or took a slower path; verts per call separates the two. */
extern "C" void rt_geom_prof_report(double fields) {
    if (fields <= 0.0 || g_window.empty()) {
        g_window.clear();
        g_window_fields = 0;
        return;
    }
    Bucket t;
    for (const auto& kv : g_window) fold(t, kv.second);
    if (t.verts == 0) {
        g_window.clear();
        g_window_fields = 0;
        return;
    }
    rt_log("prof", "  geom: %" PRIu64 " verts, %" PRIu64 " prims over %" PRIu64
        " fields with geometry (%.0f verts/field)  behind-eye %" PRIu64 " (%.2f%%)"
        "  oversized %" PRIu64 " (%.2f%%)",
        t.verts, t.prims, g_window_fields,
        g_window_fields ? (double)t.verts / (double)g_window_fields : 0.0,
        t.bad_q, 100.0 * (double)t.bad_q / (double)t.verts,
        t.wild_prims, 100.0 * (double)t.wild_prims / (double)(t.prims ? t.prims : 1));
    for (const auto& kv : g_window) {
        if (kv.second.verts == 0) continue;
        char who[64];
        describe(who, sizeof(who), kv.first);
        rt_log("prof", "    %-26s verts=%-9" PRIu64 " behind-eye=%-8" PRIu64
            " oversized=%-7" PRIu64 " kicks=%" PRIu64,
            who, kv.second.verts, kv.second.bad_q, kv.second.wild_prims, kv.second.kicks);
    }
    g_window.clear();
    g_window_fields = 0;
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
    /* Fold once per field rather than once per vertex: the wider scopes
     * are only ever read at a window or run boundary. */
    for (const auto& kv : g_field) {
        fold(g_window[kv.first], kv.second);
        fold(g_run, kv.second);
    }
    if (verts == 0) {
        g_field.clear();
        g_worst = Worst{};
        return;
    }
    ++g_window_fields;
    rt_logv("geom", "field %" PRIu64 " (parity %u): prims=%" PRIu64 " verts=%" PRIu64
        "  behind-eye verts=%" PRIu64 " (%.2f%%)  oversized prims=%" PRIu64 " (%.2f%%)",
        g_field_no, field, prims, verts,
        bad_q, 100.0 * double(bad_q) / double(verts),
        wild, 100.0 * double(wild) / double(prims ? prims : 1));

    for (const auto& kv : g_field) {
        if (kv.second.bad_q == 0 && kv.second.wild_prims == 0) continue;
        char who[64];
        describe(who, sizeof(who), kv.first);
        rt_logv("geom", "  %-28s prims=%" PRIu64 " verts=%" PRIu64
            " behind-eye=%" PRIu64 " oversized=%" PRIu64 " kicks=%" PRIu64 " with-clip=%" PRIu64,
            who, kv.second.prims, kv.second.verts, kv.second.bad_q, kv.second.wild_prims,
            kv.second.kicks, kv.second.kicks_clipped);
    }
    if (g_worst.seen) {
        char who[64];
        describe(who, sizeof(who), g_worst.key);
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
