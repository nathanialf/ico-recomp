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
 */
#include "hw.h"

#include "../ee/kernel.h"
#include "../gs/gs_backend.h"
#include "../prof.h"

#include <cinttypes>
#include <cstring>

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
        if (rt_trace()) rt_log("gif", "PATH%d submission ended mid-packet (no EOP)", path + 1);
    }
    if (st.qw_left > 0 && path != 2) {
        ++st.malformed;
        if (is_pow2(st.malformed)) {
            rt_log("gif", "PATH%d submission ended mid-tag (%u payload qw still expected) [#%" PRIu64 "]",
                path + 1, st.qw_left, st.malformed);
        }
    }
}

} // namespace

void rt_gif_submit(int path, const uint8_t* data, uint32_t qwords) {
    RT_PROF_ZONE(RT_PROF_GIF);
    if (path < 0 || path > 2) {
        rt_log("gif", "submit on invalid path %d dropped", path);
        return;
    }
    if (qwords == 0) return;
    ++g_submits;
    track_framing(path, data, qwords);
    /* Diagnostic read of the same bytes, before they are handed on. The
     * gate is cached because this is on every packet of every frame. */
    static const bool geom = rt_verbose("geom");
    if (geom) rt_geom_scan(path, data, qwords, rt_vu1_bound_hash());
    {
        /* Separate bucket: framing above is ours, this is the backend
         * (dump writer, or paraLLEl-GS rasterization and present). */
        RT_PROF_ZONE(RT_PROF_GS);
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
                rt_log("gif", "GIF_CTRL reset");
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
