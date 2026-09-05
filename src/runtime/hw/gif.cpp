/* hw/gif.cpp: GIF packet framing for PATH1 (XGKICK), PATH2 (VIF1 DIRECT)
 * and PATH3 (DMA ch2), plus GIF_CTRL/GIF_MODE/GIF_STAT.
 *
 * The parser's job is packet framing and sanity: track the current GIF tag
 * per path (NLOOP/FLG/NREG/EOP) so packet boundaries are known and
 * malformed submissions are loud. Register interpretation (A+D and packed
 * register data) is entirely the backend's job; data is forwarded
 * verbatim. Path arbitration needs no modeling: everything is synchronous,
 * so submissions arrive in a legal serialized order by construction.
 *
 * PATH3 may legally split a packet across DMA kicks (tag state persists
 * between submissions). PATH1/PATH2 submissions are expected to end on a
 * packet boundary; a mid-packet end is loud-logged.
 *
 * Register facts (GIF tag layout, GIF_STAT bits) are public PS2 hardware
 * documentation (ps2tek).
 *
 * One exception to "register interpretation is the backend's job": the
 * widescreen 2D transform below. display.widescreen widens the 3D projection
 * by scaling one float in the game's own projection block
 * (guest/widescreen.cpp); 2D elements are not projected and would be
 * stretched with it, so their X coordinates are scaled back about the GS
 * window centre here, on the host's side of guest memory. It runs before the
 * backend, so the dump writer, paraLLEl-GS and the native renderer all see
 * the same packet and a runtime dump records what was actually drawn.
 *
 * Register facts for that transform (PRIM/PRMODE fields, SCISSOR and
 * XYOFFSET layouts, the 12.4 vertex format) are the same public
 * documentation; the decoding is gs/render/gif_decode.h, the renderer's own
 * GIF decoder. hw/geomcheck.cpp now decodes through the same header, so the
 * tag walk, the PACKED field placement and the ADC redirect exist once in the
 * tree and are covered by one selftest.
 */
#include "hw.h"

#include "../ee/kernel.h"
#include "../gs/gs_backend.h"
#include "../gs/render/gif_decode.h"
#include "../guest/widescreen.h"
#include "../prof.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct PathState {
    uint32_t loops_left = 0;   /* NLOOP remaining on the current tag */
    uint32_t qw_left = 0;      /* payload qwords remaining on the current tag */
    bool mid_packet = false;   /* inside a packet (EOP not yet reached) */
    bool eop_pending = false;  /* current tag is the packet's last */
    uint64_t packets = 0;
    uint64_t malformed = 0;
};

PathState g_path[3];
uint32_t g_gif_mode = 0;
uint64_t g_submits = 0;

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

/* Advance framing state over one submission. Returns false on a malformed
 * stream (logged; state reset so later packets can recover). */
void track_framing(int path, const uint8_t* data, uint32_t qwords) {
    PathState& st = g_path[path];
    uint32_t i = 0;
    while (i < qwords) {
        if (st.qw_left == 0) {
            /* At a tag boundary. */
            uint64_t lo;
            std::memcpy(&lo, data + (size_t)i * 16, 8);
            uint32_t nloop = (uint32_t)(lo & 0x7FFF);
            bool eop = (lo >> 15) & 1;
            uint32_t flg = (uint32_t)((lo >> 58) & 3);
            uint32_t nreg = (uint32_t)((lo >> 60) & 15);
            if (nreg == 0) nreg = 16;
            uint32_t payload;
            switch (flg) {
                case 0: payload = nloop * nreg; break;
                case 1: payload = (nloop * nreg + 1) / 2; break;
                default: payload = nloop; break;
            }
            st.qw_left = payload;
            st.eop_pending = eop;
            st.mid_packet = true;
            ++i;
        } else {
            uint32_t take = st.qw_left < (qwords - i) ? st.qw_left : (qwords - i);
            st.qw_left -= take;
            i += take;
        }
        if (st.qw_left == 0 && st.mid_packet && st.eop_pending) {
            st.mid_packet = false;
            st.eop_pending = false;
            ++st.packets;
        }
    }
    if (st.mid_packet && st.qw_left == 0 && path != 2) {
        /* Tag boundary but no EOP seen: legal only for PATH3 splits. Not an
         * error for PATH1/2 either (multi-kick packets exist) but worth a
         * trace note; keep quiet unless tracing. */
        if (rt_trace()) rt_log_warn("gif", "PATH%d submission ended mid-packet (no EOP)", path + 1);
    }
    if (st.qw_left > 0 && path != 2) {
        ++st.malformed;
        if (is_pow2(st.malformed)) {
            rt_log_warn("gif", "PATH%d submission ended mid-tag (%u payload qw still expected) [#%" PRIu64 "]",
                path + 1, st.qw_left, st.malformed);
        }
    }
}

/* ---- widescreen 2D transform --------------------------------------------
 *
 * display.widescreen widens the 3D frustum by scaling the X term of the
 * game's own projection block (guest/widescreen.cpp). Everything the game
 * draws in screen space instead of through that projection would be widened
 * with the window and is scaled back here by the same factor k, about the GS
 * window centre.
 *
 * PROVISIONAL CLASSIFICATION RULE. It has not been confirmed against a GS
 * dump of this game: no dump of a running ICO exists on the machine this was
 * written on. Turn the `widescreen` verbose channel on, dump the five scenes
 * named in docs/SETTINGS.md section 6 (title, a menu page, subtitles in a
 * cutscene, a fade, the bloom pass), and read the per-primitive lines this
 * emits against the picture. When the rule is confirmed or corrected, name
 * the dumps it was decided from right here, and say so in
 * docs/SETTINGS.md section 6. Until then this comment is the honest state of
 * it.
 *
 * The rule, as implemented:
 *
 *   - A SPRITE, on any path.
 *   - A triangle strip or fan, on PATH3 only, whose effective PRIM has
 *     FST = 1 (UV used directly, no texture perspective) and whose vertices
 *     all carry one Z.
 *   - In both cases only when the primitive's X extent lies strictly inside
 *     the current context's SCISSOR X range: xmin is right of SCAX0 and xmax
 *     is left of SCAX1 + 1. A primitive that spans the whole scissor width is
 *     a full-frame pass (a fade, a post-processing copy, a frame-sized quad)
 *     and is left exactly as the game wrote it, because widening the frustum
 *     did not move it and scaling it in would put bars inside the picture.
 *
 * Why sprites on any path but strips only on PATH3: PATH1 is VU1's XGKICK
 * output, which for this game is transformed geometry and must never be
 * touched. But the menu's own item quad reaches the GS through
 * gif_StartPacketPath1 (guest/menu_nav.cpp:102), so a 2D sprite can and does
 * arrive on PATH1. A sprite is two screen-space corners by definition and
 * carries no projected vertex, so the extent test is enough to judge it
 * whatever path it came in on. A strip is not: a 3D strip on PATH1 that
 * happened to be small and flat would pass the same test. Restricting
 * strips to PATH3 keeps VU1 output out of reach.
 *
 * X is 12.4 fixed point, so the two edges of a sprite round independently
 * and can land 1/16 of a pixel apart from the exact scaled width. That is
 * left visible rather than hidden by widening the type: a rounding this
 * small is below a pixel, and rounding both edges the same way (or snapping
 * to whole pixels) would move the element instead of scaling it.
 */

/* GS register addresses this transform reacts to. Same numbering
 * gif_decode.h delivers. */
enum : uint32_t {
    kRegPrim       = 0x00,
    kRegXyzf2      = 0x04,
    kRegXyz2       = 0x05,
    kRegXyzf3      = 0x0C,
    kRegXyz3       = 0x0D,
    kRegXyoffset1  = 0x18,
    kRegXyoffset2  = 0x19,
    kRegPrmodecont = 0x1A,
    kRegPrmode     = 0x1B,
    kRegScissor1   = 0x40,
    kRegScissor2   = 0x41,
};

/* PRIM primitive types (PRIM bits 0..2). */
enum : uint32_t {
    kPrimTriStrip = 4,
    kPrimTriFan   = 5,
    kPrimSprite   = 6,
};

/* The GS window centre. The game places its XYOFFSET at
 * (2048 - width/2, 2048 - height/2), the standard PS2 arrangement, so 2048
 * whole pixels is the centre of the picture whatever the buffer size, and
 * it is the fixed point a horizontal scale has to be taken about. In the
 * 12.4 fixed point the vertex registers carry, that is 2048 * 16. */
constexpr int32_t kGsCentre = 2048 * 16;

/* Vertex X is an unsigned 16-bit field. A factor above 1 (a window narrower
 * than 4:3) can push a coordinate past it; that is reported, not clamped. */
constexpr int32_t kXMax = 0xFFFF;

/* Per-primitive diagnostic lines are emitted for this many fields after each
 * factor change, then stop. Enough to cover a scene the user has just
 * switched the mode on for, without the log growing without bound. */
constexpr unsigned kDiagFields = 32;

struct GsCtx {
    int32_t ofx = 0;          /* XYOFFSET OFX, 12.4 */
    uint32_t scax0 = 0;       /* SCISSOR SCAX0, whole pixels, inclusive */
    uint32_t scax1 = 0;       /* SCISSOR SCAX1, whole pixels, inclusive */
    bool scissor_seen = false;
    bool offset_seen = false;
};

struct Vert {
    /* Byte offset of the 64-bit word this vertex was read from, inside the
     * submission being walked, or kStale when the vertex arrived in an
     * earlier submission and its bytes are gone. */
    uint32_t off;
    int32_t x;                /* 12.4, as written */
    uint32_t z;
};

constexpr uint32_t kStale = 0xFFFFFFFFu;

/* GS register state is one machine-wide set, not one per path, so this is
 * global. The vertex queue and the decode position are per path, because
 * PATH3 may split a packet across DMA kicks. */
struct WideGsState {
    GsCtx ctx[2];
    uint32_t prim = 0;        /* last PRIM write */
    uint32_t prmode = 0;      /* last PRMODE write, PRIM's bit positions */
    bool prmodecont = true;   /* PRMODECONT bit 0 */
    bool prmodecont_seen = false;
};

struct PathXform {
    gsr::GifDecodeState dec;
    std::vector<Vert> run;
    uint32_t run_attr = 0;    /* effective PRIM word when the run opened */
    uint32_t run_ctxt = 0;
    bool run_open = false;
};

struct Patch {
    uint32_t off;             /* byte offset of the X field */
    uint16_t x;               /* the value to store there, 12.4 */
};

struct WideDiag {
    uint64_t generation = 0;  /* rt_widescreen_generation() last seen */
    unsigned fields_since_change = 0;
    uint64_t transformed = 0;
    uint64_t full_frame = 0;
    uint64_t skipped_3d = 0;
    uint64_t skipped_unknown = 0;   /* no SCISSOR/XYOFFSET seen yet */
    uint64_t skipped_split = 0;     /* run split across submissions */
    uint64_t vu1_kicks = 0;
    /* The distinct microprogram hashes bound at the PATH1 kicks of this
     * field. Small and fixed: this is a comparison aid, not a census, and a
     * field that binds more than this many programs says so. */
    uint32_t hashes[16] = {};
    unsigned hash_count = 0;
    bool hash_overflow = false;
};

WideGsState g_ws;
PathXform g_wx[3];
std::vector<uint8_t> g_ws_scratch;
std::vector<Patch> g_ws_patches;
WideDiag g_wsd;
uint64_t g_ws_generation = 0;
bool g_ws_logged_out_of_range = false;
bool g_ws_logged_prmodecont = false;

bool ws_verbose() {
    /* Cached: this is consulted on every packet of every field. Turning the
     * channel on therefore needs a restart, the same as the geom channel.
     *
     * A function-local static and not a file-scope const, deliberately:
     * rt_verbose reads the log configuration, and a file-scope initialiser
     * would run during static initialisation, before the settings layer has
     * said anything. The cost of that choice is one relaxed guard-variable
     * load per call, which is what the comment in rt_gif_submit accounts
     * for. */
    static const bool on = rt_verbose("widescreen");
    return on;
}

/* Resets the register shadow. Called when the factor changes, because
 * between changes the transform returns before the walk and the shadow goes
 * stale; a primitive is not transformed until the game has written the
 * registers the decision needs again. */
void ws_reset_shadow() {
    g_ws = WideGsState{};
    for (PathXform& px : g_wx) {
        px.dec = gsr::GifDecodeState{};
        px.run.clear();
        px.run_open = false;
    }
}

/* The PRIM fields in force. PRMODECONT selects whether the attribute bits
 * come from PRIM or from PRMODE; the primitive type always comes from PRIM
 * (PRMODE has no type field). */
uint32_t ws_effective_attr() {
    if (g_ws.prmodecont) return g_ws.prim;
    return (g_ws.prim & 7u) | (g_ws.prmode & ~7u);
}

const char* ws_path_name(int path) {
    return path == 0 ? "PATH1" : (path == 1 ? "PATH2" : "PATH3");
}

const char* ws_prim_name(uint32_t type) {
    switch (type) {
    case 0: return "point";
    case 1: return "line";
    case 2: return "line-strip";
    case 3: return "triangle";
    case 4: return "tri-strip";
    case 5: return "tri-fan";
    case 6: return "sprite";
    default: return "prim7";
    }
}

/* One finished vertex run: classify it, and on a transform append the new X
 * of each vertex to g_ws_patches. */
void ws_decide(int path, PathXform& px, double k) {
    const uint32_t type = px.run_attr & 7u;
    const uint32_t fst = (px.run_attr >> 8) & 1u;
    const GsCtx& ctx = g_ws.ctx[px.run_ctxt];
    const size_t n = px.run.size();

    const bool eligible_shape =
        (type == kPrimSprite && n >= 2) ||
        ((type == kPrimTriStrip || type == kPrimTriFan) && path == 2 && n >= 3 && fst != 0);
    if (!eligible_shape) {
        ++g_wsd.skipped_3d;
        return;
    }
    if (type != kPrimSprite) {
        /* One Z across the run: a strip that is flat in depth is a candidate
         * for screen-space work, one that is not is projected geometry. */
        for (size_t i = 1; i < n; ++i) {
            if (px.run[i].z != px.run[0].z) {
                ++g_wsd.skipped_3d;
                return;
            }
        }
    }
    if (!ctx.scissor_seen || !ctx.offset_seen) {
        ++g_wsd.skipped_unknown;
        return;
    }

    int32_t xmin = px.run[0].x, xmax = px.run[0].x;
    bool split = false;
    for (const Vert& v : px.run) {
        if (v.x < xmin) xmin = v.x;
        if (v.x > xmax) xmax = v.x;
        if (v.off == kStale) split = true;
    }

    /* SCISSOR is in whole pixels and inclusive at both ends; the vertex is
     * 12.4 and carries XYOFFSET. Strictly inside means the left edge is
     * right of SCAX0 and the right edge is left of the far side of SCAX1. */
    const int32_t left = ctx.ofx + (int32_t)(ctx.scax0 << 4);
    const int32_t right = ctx.ofx + (int32_t)((ctx.scax1 + 1) << 4);
    const bool inside = xmin > left && xmax < right;

    const bool diag = ws_verbose() && g_wsd.fields_since_change < kDiagFields;
    const char* decision = inside ? (split ? "split (left alone)" : "transformed")
                                  : "full-frame (left alone)";
    if (diag) {
        rt_log_debug("widescreen", "%s %s n=%u x=[%.2f, %.2f]px scissor=[%u, %u]px ctx=%u fst=%u: %s",
            ws_path_name(path), ws_prim_name(type), (unsigned)n,
            (double)(xmin - ctx.ofx) / 16.0, (double)(xmax - ctx.ofx) / 16.0,
            ctx.scax0, ctx.scax1, px.run_ctxt, fst, decision);
    }

    if (!inside) {
        ++g_wsd.full_frame;
        return;
    }
    if (split) {
        /* The bytes of at least one of these vertices were handed to the
         * backend by an earlier submission and cannot be rewritten now.
         * Reported rather than half-applied: scaling only the vertices still
         * in hand would tear the primitive. */
        ++g_wsd.skipped_split;
        return;
    }

    /* Where this primitive's patches start, so an out-of-range vertex part
     * way through can take the earlier ones back out. Leaving them would
     * write the primitive with some vertices scaled and the rest as the game
     * wrote them, which is the tearing the split branch above refuses to
     * do. */
    const size_t mark = g_ws_patches.size();
    for (const Vert& v : px.run) {
        const long scaled = std::lround((double)(v.x - kGsCentre) * k);
        const int32_t nx = kGsCentre + (int32_t)scaled;
        if (nx < 0 || nx > kXMax) {
            if (!g_ws_logged_out_of_range) {
                g_ws_logged_out_of_range = true;
                rt_log_warn("widescreen",
                    "scaling X %d about %d by %.4f gives %d, outside the 16-bit vertex field;"
                    " this primitive is left as the game wrote it. Further such primitives are"
                    " not logged.", v.x, kGsCentre, k, nx);
            }
            g_ws_patches.resize(mark);
            return;
        }
        g_ws_patches.push_back(Patch{v.off, (uint16_t)nx});
    }
    ++g_wsd.transformed;
}

void ws_flush_run(int path, PathXform& px, double k) {
    if (!px.run_open) return;
    if (!px.run.empty()) ws_decide(path, px, k);
    px.run.clear();
    px.run_open = false;
}

struct XformSink {
    int path;
    PathXform& px;
    double k;

    void reg(uint32_t addr, uint64_t value) {
        switch (addr) {
        case kRegPrim:
            /* A PRIM write ends the previous primitive and empties the
             * hardware's vertex queue. */
            ws_flush_run(path, px, k);
            g_ws.prim = (uint32_t)(value & 0x7FFull);
            return;
        case kRegPrmode:
            ws_flush_run(path, px, k);
            /* PRMODE carries PRIM's attribute bits at PRIM's own positions;
             * bits 0..2 are not a primitive type there. */
            g_ws.prmode = (uint32_t)(value & 0x7FFull);
            return;
        case kRegPrmodecont:
            ws_flush_run(path, px, k);
            g_ws.prmodecont = (value & 1ull) != 0;
            g_ws.prmodecont_seen = true;
            return;
        case kRegXyoffset1:
        case kRegXyoffset2: {
            ws_flush_run(path, px, k);
            GsCtx& c = g_ws.ctx[addr == kRegXyoffset2 ? 1 : 0];
            c.ofx = (int32_t)(value & 0xFFFFull);
            c.offset_seen = true;
            return;
        }
        case kRegScissor1:
        case kRegScissor2: {
            ws_flush_run(path, px, k);
            GsCtx& c = g_ws.ctx[addr == kRegScissor2 ? 1 : 0];
            c.scax0 = (uint32_t)(value & 0x7FFull);
            c.scax1 = (uint32_t)((value >> 16) & 0x7FFull);
            c.scissor_seen = true;
            return;
        }
        case kRegXyz2:
        case kRegXyz3:
        case kRegXyzf2:
        case kRegXyzf3:
            break;
        default:
            return;
        }

        if (!px.run_open) {
            px.run_attr = ws_effective_attr();
            px.run_ctxt = (px.run_attr >> 9) & 1u;
            px.run_open = true;
            if (!g_ws.prmodecont_seen && !g_ws_logged_prmodecont) {
                g_ws_logged_prmodecont = true;
                rt_log_debug("widescreen", "no PRMODECONT write seen yet; PRIM's own attribute"
                    " bits are assumed to be the ones in force");
            }
        }
        /* XYZ2/XYZ3 carry a 32-bit Z; XYZF2/XYZF3 carry 24 bits of Z and
         * then F. Both have X in bits 0..15. */
        const uint32_t z = (addr == kRegXyz2 || addr == kRegXyz3)
                               ? (uint32_t)((value >> 32) & 0xFFFFFFFFull)
                               : (uint32_t)((value >> 32) & 0xFFFFFFull);
        px.run.push_back(Vert{px.dec.value_off, (int32_t)(value & 0xFFFFull), z});
        /* A sprite is exactly two vertices, so its run closes as soon as the
         * second arrives; a strip or fan runs until the next PRIM write or
         * the end of the packet. */
        if ((px.run_attr & 7u) == kPrimSprite && px.run.size() == 2) {
            ws_flush_run(path, px, k);
        }
    }

    void image(const uint8_t*, uint32_t) {}
    void note(const char*) {}
};

/* Walks one submission and, when the rule above says so, returns a rewritten
 * copy of it. Returns `data` unchanged when nothing was transformed, so the
 * common case copies nothing.
 *
 * The walk reads the packet as the game wrote it and the patches are applied
 * afterwards to a copy, so a later primitive's extent test never sees an
 * earlier primitive's scaled coordinates. */
const uint8_t* ws_transform(int path, const uint8_t* data, uint32_t qwords, double k) {
    /* The shadow is only maintained while the factor is on, so a change is
     * where it has to be dropped. Checked here rather than only in the
     * per-field diagnostic, which runs only when the channel is on. */
    const uint64_t gen = rt_widescreen_generation();
    if (gen != g_ws_generation) {
        g_ws_generation = gen;
        ws_reset_shadow();
    }
    PathXform& px = g_wx[path];
    g_ws_patches.clear();
    XformSink sink{path, px, k};
    gsr::gif_decode(px.dec, data, qwords, sink);

    if (!px.dec.in_tag) {
        /* The submission ended on a tag boundary, so whatever run is open is
         * complete and can be judged now. */
        ws_flush_run(path, px, k);
    } else {
        /* PATH3 split the packet. The run stays open across the kick, but
         * these bytes are about to be handed to the backend, so the vertices
         * already in it can no longer be rewritten. */
        for (Vert& v : px.run) v.off = kStale;
    }

    if (g_ws_patches.empty()) return data;

    const size_t bytes = (size_t)qwords * 16;
    if (g_ws_scratch.size() < bytes) g_ws_scratch.resize(bytes);
    std::memcpy(g_ws_scratch.data(), data, bytes);
    for (const Patch& pt : g_ws_patches) {
        if ((size_t)pt.off + 2 > bytes) continue; /* cannot happen; not worth a crash */
        std::memcpy(g_ws_scratch.data() + pt.off, &pt.x, sizeof(pt.x));
    }
    return g_ws_scratch.data();
}

} // namespace

/* Per-field diagnostic. Called from hw/gspriv.cpp's vsync next to
 * rt_geom_field, and only when the widescreen channel is on.
 *
 * The VU1 lines are the culling measurement: widening the frustum changes
 * what VU1's clip-cull path keeps, so two dumps taken at the same camera
 * with the mode off and on can be compared on kick count and on which
 * microprograms ran, rather than on a description of the picture. */
void rt_gif_widescreen_field(unsigned field) {
    if (!ws_verbose()) return;
    const uint64_t gen = rt_widescreen_generation();
    if (gen != g_wsd.generation) {
        g_wsd.generation = gen;
        g_wsd.fields_since_change = 0;
        g_ws_logged_out_of_range = false;
    }

    /* Eleven bytes an entry ("0x" plus eight digits plus one separator), so
     * the buffer holds the whole set with room to spare; the clamp is there
     * because snprintf returns what it would have written, not what it
     * did. */
    char hashes[16 * 12 + 8];
    size_t n = 0;
    hashes[0] = '\0';
    for (unsigned i = 0; i < g_wsd.hash_count && n + 1 < sizeof(hashes); ++i) {
        const int wrote = std::snprintf(hashes + n, sizeof(hashes) - n, "%s0x%08x",
                                        i ? " " : "", g_wsd.hashes[i]);
        if (wrote <= 0) break;
        n += (size_t)wrote;
        if (n >= sizeof(hashes)) {
            n = sizeof(hashes) - 1;
            break;
        }
    }
    hashes[n] = '\0';

    rt_log_debug("widescreen", "field %u: k=%.4f transformed=%" PRIu64 " full-frame=%" PRIu64
        " 3D=%" PRIu64 " unknown-state=%" PRIu64 " split=%" PRIu64,
        field, rt_widescreen_factor(), g_wsd.transformed, g_wsd.full_frame,
        g_wsd.skipped_3d, g_wsd.skipped_unknown, g_wsd.skipped_split);
    rt_log_debug("widescreen", "field %u: vu1 kicks=%" PRIu64 " programs={%s}%s",
        field, g_wsd.vu1_kicks, hashes, g_wsd.hash_overflow ? " (more not listed)" : "");

    g_wsd.transformed = 0;
    g_wsd.full_frame = 0;
    g_wsd.skipped_3d = 0;
    g_wsd.skipped_unknown = 0;
    g_wsd.skipped_split = 0;
    g_wsd.vu1_kicks = 0;
    g_wsd.hash_count = 0;
    g_wsd.hash_overflow = false;
    if (g_wsd.fields_since_change < kDiagFields) ++g_wsd.fields_since_change;
}

void rt_gif_submit(int path, const uint8_t* data, uint32_t qwords) {
    RT_PROF_ZONE(RT_PROF_GIF);
    if (path < 0 || path > 2) {
        rt_log_warn("gif", "submit on invalid path %d dropped", path);
        return;
    }
    if (qwords == 0) return;
    ++g_submits;
    track_framing(path, data, qwords);

    /* The widescreen 2D transform. With display.widescreen off the whole of
     * what it costs a packet is here: one relaxed atomic load through
     * rt_widescreen_factor, one guard-variable load through ws_verbose, and
     * two predictable branches. The walk is not entered and nothing is
     * copied. */
    const double k = rt_widescreen_factor();
    if (ws_verbose() && path == 0) {
        const uint32_t h = rt_vu1_bound_hash();
        ++g_wsd.vu1_kicks;
        bool known = false;
        for (unsigned i = 0; i < g_wsd.hash_count; ++i) {
            if (g_wsd.hashes[i] == h) known = true;
        }
        if (!known) {
            if (g_wsd.hash_count < (unsigned)(sizeof(g_wsd.hashes) / sizeof(g_wsd.hashes[0]))) {
                g_wsd.hashes[g_wsd.hash_count++] = h;
            } else {
                g_wsd.hash_overflow = true;
            }
        }
    }
    if (k != 1.0) data = ws_transform(path, data, qwords, k);

    /* Diagnostic read of the same bytes, before they are handed on. It
     * reads the transformed bytes for the same reason the dump writer does:
     * they are what was drawn. Compiled out of every shipped build: it
     * re-decodes every GIF packet of every frame, which is real work on the
     * path the hitch profile already names, so it is a build define
     * (docs/GS_RENDERER.md) and not a run-time switch. */
#ifdef ICORECOMP_GEOM_CHECK
    rt_geom_scan(path, data, qwords, rt_vu1_bound_hash());
#endif
    {
        /* Separate bucket: framing above is ours, this is the backend
         * (dump writer, or paraLLEl-GS rasterization and present). */
        RT_PROF_ZONE(RT_PROF_GS);
        /* One counter increment, read by the field watchdog: fields that
         * keep advancing with this number frozen is a guest looping with
         * nothing to draw, which looks identical to a working run from
         * every other instrument (host/run_state.cpp). */
        rt_run_note_gif();
        rt_gs_backend()->submit_gif(path, data, qwords);
    }
}

bool rt_gif_mmio_read(uint32_t addr, uint32_t* out) {
    switch (addr) {
        case 0x10003020: /* GIF_STAT: everything idle, FIFO empty (FQC=0) */
            *out = 0;
            return true;
        case 0x10003010: /* GIF_MODE */
            *out = g_gif_mode;
            return true;
        case 0x10003000: /* GIF_CTRL is write-only; reads return 0 */
            *out = 0;
            return true;
        default:
            return false;
    }
}

bool rt_gif_mmio_write(uint32_t addr, uint32_t v) {
    switch (addr) {
        case 0x10003000: /* GIF_CTRL: RST (bit 0) resets the path state */
            if (v & 1) {
                for (auto& p : g_path) p = PathState{};
                rt_log_info("gif", "GIF_CTRL reset");
            }
            return true;
        case 0x10003010:
            g_gif_mode = v;
            return true;
        case 0x10003020: /* GIF_STAT is read-only; accept and ignore */
            return true;
        default:
            return false;
    }
}
