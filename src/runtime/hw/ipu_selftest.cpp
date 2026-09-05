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
 * There are two feed modes.
 *
 *   direct: the whole elementary stream is handed to the model up front
 *     through rt_ipu_test_feed, so no command ever stalls and the DMA
 *     bridge is not involved. This is the decode reference.
 *
 *   dma: the stream lives in guest RAM behind a ch4 source chain of REF
 *     tags over 2048-byte slots, exactly the shape viBufAddDMA.s builds,
 *     and the model pulls it through rt_ipu_dma_kick. In this mode the test
 *     also replays the retail MPEG library's stop/restart bracket
 *     (viBufStopDMA.s snapshot, viBufRestartDMA.s restart) at a chosen
 *     stream-byte cadence, using only guest-visible registers: CHCR without
 *     STR, read MADR/TADR/QWC/CHCR, wait for IPU_CTRL.OFC == 0, read
 *     IPU_BP, BCLR with the saved BP, then
 *         MADR' = MADR - (IFC + FP) * 16      QWC' = QWC + IFC + FP
 *     and CHCR = the saved CHCR with STR set, TAG field and all: the
 *     stop wrote the bare constant 5, so the id in it reads back as
 *     REFE and the restart has to carry on past it anyway.
 *
 * Pass criteria:
 *   - the first I picture decodes to exactly mb_width x mb_height
 *     macroblocks with no ECD,
 *   - luma statistics are plausible (mean inside video range, nonzero
 *     spread),
 *   - the first P picture's residual pass decodes with no ECD,
 *   - CSC converts the reconstructed I frame; the result is written as a
 *     BMP for eyeball verification (untracked output path, /tmp by
 *     default; override with ICORECOMP_IPU_SELFTEST_OUT),
 *   - every dma pass decodes the first I picture bit-identically to the
 *     direct pass, whatever the restart cadence, and IPU_BP never claims
 *     more resident quadwords than the input FIFO can hold,
 *   - the drained-chain restart (drained_chain_restart_check) behaves as
 *     modelled: the ring runs dry with a command pending, the library's
 *     BCLR empties the input FIFO without dropping that command, and the
 *     QWC' = 0 restart that follows lands on a ring slot the guest has not
 *     written.
 *
 * This binary links ipu.cpp + iso9660.cpp + loader/log/sha1 and stubs the
 * scheduler/INTC/DMAC symbols ipu.cpp expects.
 */
#include "hw.h"

#include "../video_mode.h"

#include "../iso/iso9660.h"
#include "../prof.h"

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

/* ---- stubs for symbols ipu.cpp pulls in (no scheduler/DMAC linked) ------- */

uint8_t* g_pages[0x10000];

/* Cached, like the runtime's (ee/sched.cpp): the decoder calls this on
 * every command and every walk step, and a getenv per call would put the
 * benchmark below in a different cost regime from the real runtime. */
bool rt_trace() {
    static const bool on = std::getenv("ICORECOMP_TRACE") != nullptr;
    return on;
}
void rt_intc_raise(int) {}
void rt_dmac_raise(int) {}
/* No DMAC is linked, so nothing can hold it: the selftest feeds the input
 * queue directly through rt_ipu_test_feed and never uses the DMA bridge. */
bool rt_dmac_suspended() { return false; }
/* The tag ring and its dump live in hw/dmac.cpp, which is not linked here;
 * the test's own chain builder already knows every tag it wrote. */
void rt_dmac_record_tag(int, uint32_t, uint32_t, uint32_t, uint32_t) {}
void rt_dmac_dump_recent_tags(int) {}

uint32_t* rt_dmac_ipu_reg(int ch, int which) {
    static uint32_t dummy[2][4];
    return &dummy[ch == 4 ? 1 : 0][which];
}

namespace {

/* ---- MMIO driver helpers ------------------------------------------------- */

constexpr uint32_t IPU_CMD = 0x10002000, IPU_CTRL = 0x10002010, IPU_BP = 0x10002020;
constexpr uint32_t IPU_TOP = 0x10002030;

/* Input FIFO depth in quadwords; the same figure hw/ipu.cpp models, and the
 * bound IPU_BP.IFC (4 bits) plus IPU_BP.FP (2 bits) has to stay inside. */
constexpr uint32_t kFifoQw = 8;

uint64_t r64(uint32_t addr) {
    uint64_t v = 0;
    if (!rt_ipu_mmio_read(addr, &v)) rt_fatal("selftest", nullptr, "mmio read 0x%08x unhandled", addr);
    return v;
}
void w32(uint32_t addr, uint32_t v) {
    if (!rt_ipu_mmio_write(addr, v)) rt_fatal("selftest", nullptr, "mmio write 0x%08x unhandled", addr);
}

/* ---- guest RAM and the toIPU source chain (dma feed mode) ----------------
 *
 * Slot size and tag shape come from the retail ring builder: decomp
 * asm/nonmatchings/ito/mpeg/mv_vibuf/viBufAddDMA.s at 0x259ABC-0x23FF80 and
 * 0x259AE8-0x259B20 stores "(ring_base + slot * 2048) << 32 | id << 28 |
 * 0x80" quadword tags, id 3 (REF) for every slot but the last, which gets
 * id 0 (REFE). */
constexpr uint32_t kRamBase = 0x00400000;
constexpr uint32_t kTagQwc = 128; /* 0x80 quadwords = one 2048-byte slot */

std::vector<uint8_t> g_ram;
uint32_t g_tag_first = 0, g_tag_last = 0;
size_t g_tag_off = 0;      /* byte offset of tag 0 inside g_ram */
size_t g_slots = 0;        /* slots the chain can cover */
size_t g_published = 0;    /* slots the guest has handed to the DMAC so far */
size_t g_slot_batch = 0;   /* slots published per refill */
uint64_t g_refills = 0;
size_t g_slot_batch_want = 0; /* slots per publish for the next pass */

bool g_dma = false;               /* feed through ch4 instead of the test hook */
uint32_t g_restart_bytes = 0;     /* stream bytes between restarts, 0 = never */
bool g_restart_mid_cmd = false;   /* also restart from inside a busy poll */
uint32_t g_last_restart_pos = 0;
uint64_t g_restarts = 0, g_restarts_busy = 0, g_restarts_dry = 0;
size_t g_max_avail_bits = 0, g_max_resident_qw = 0;
bool g_in_restart = false;
const char* g_pass = "direct";

uint32_t* ch4r(int which) { return rt_dmac_ipu_reg(4, which); }

size_t g_ram_mapped = 0;

void map_ram(size_t bytes) {
    for (size_t off = 0; off < g_ram_mapped; off += 0x10000) {
        g_pages[(kRamBase + (uint32_t)off) >> 16] = nullptr;
    }
    g_ram.assign(bytes, 0);
    for (size_t off = 0; off < bytes; off += 0x10000) {
        g_pages[(kRamBase + (uint32_t)off) >> 16] = g_ram.data() + off;
    }
    g_ram_mapped = bytes;
}

/* Copies the two out-of-band quantizer matrices and `bytes` of elementary
 * stream into guest RAM and reserves the tag area over them. */
void build_chain(const uint8_t* iq, const uint8_t* niq, const std::vector<uint8_t>& es, size_t bytes) {
    const size_t slot = (size_t)kTagQwc * 16;
    size_t data = 128 + bytes;
    g_slots = (data + slot - 1) / slot;
    size_t data_span = (g_slots * slot + 0xFFFF) & ~(size_t)0xFFFF;
    size_t tag_span = (g_slots * 16 + 0xFFFF) & ~(size_t)0xFFFF;
    map_ram(data_span + tag_span);
    std::memcpy(&g_ram[0], iq, 64);
    std::memcpy(&g_ram[64], niq, 64);
    std::memcpy(&g_ram[128], es.data(), bytes);
    g_tag_off = data_span;
    g_tag_first = kRamBase + (uint32_t)data_span;
    g_tag_last = g_tag_first + (uint32_t)((g_slots - 1) * 16);
    rt_log_info("selftest", "dma chain: %zu slots of %u qwords over %zu stream bytes "
        "(data 0x%08x, tags 0x%08x..0x%08x)", g_slots, kTagQwc, bytes, kRamBase, g_tag_first, g_tag_last);
}

void write_tag(size_t i, uint32_t id) {
    uint64_t addr = kRamBase + i * (size_t)kTagQwc * 16;
    uint64_t tag = (addr << 32) | ((uint64_t)id << 28) | kTagQwc;
    std::memcpy(&g_ram[g_tag_off + i * 16], &tag, 8);
}

/* Publishes the first batch of slots. batch == 0 means the whole chain at
 * once (the DMAC then never runs dry, so no command ever stalls). */
void reset_chain(size_t batch) {
    g_slot_batch = batch ? batch : g_slots;
    g_published = g_slot_batch < g_slots ? g_slot_batch : g_slots;
    g_refills = 0;
    /* Unpublished slots read back as an all-zero tag (REFE, qwc 0), so a
     * walk that runs past the published REFE stops instead of picking up a
     * tag left behind by an earlier pass. */
    std::memset(&g_ram[g_tag_off], 0, g_slots * 16);
    for (size_t i = 0; i < g_published; ++i) write_tag(i, i + 1 == g_published ? 0u : 3u);
}

void ch4_start() {
    *ch4r(1) = 0;
    *ch4r(2) = 0;
    *ch4r(3) = g_tag_first;
    *ch4r(0) = 0x105; /* DIR=1 (to IPU), MOD=1 (source chain), STR */
    rt_ipu_dma_kick(4);
}

/* Appends the next batch of slots the way the retail ring extender does:
 * decomp asm/nonmatchings/ito/mpeg/mv_vibuf/viBufAddDMA.s stops ch4 with
 * CHCR = 5 inside the D_ENABLE bracket (0x2599AC-0x2599C0), reads CHCR back
 * (0x23FE98), rewrites the REFE that closed the ring into a REF and writes
 * the new tags (0x23FF48-0x259B20), then restarts with
 * CHCR = (readback & 0x0FFFFFFF) | 0x30000100 at 0x24000C-0x259BAC. */
bool publish_more() {
    if (g_published >= g_slots) return false;
    *ch4r(0) = 5;
    rt_ipu_dma_stop(4);
    uint32_t chcr = *ch4r(0);
    write_tag(g_published - 1, 3); /* the tag that closed the ring becomes a REF */
    size_t upto = g_published + g_slot_batch;
    if (upto > g_slots) upto = g_slots;
    for (size_t i = g_published; i < upto; ++i) write_tag(i, i + 1 == upto ? 0u : 3u);
    g_published = upto;
    *ch4r(0) = (chcr & 0x0FFFFFFFu) | 0x30000100u;
    rt_ipu_dma_kick(4);
    ++g_refills;
    return true;
}

/* viBufGetTs.s at 0x25A354-0x240844: stream byte position =
 * D4_MADR - (IFC + FP) * 16 + (BP & 0x7F) >> 3. */
uint32_t stream_pos() {
    uint32_t bp = (uint32_t)r64(IPU_BP);
    return *ch4r(1) - (((bp >> 8) & 0xF) + ((bp >> 16) & 3)) * 16 + ((bp & 0x7F) >> 3);
}

void check_fifo_bound(const char* where) {
    uint32_t bp = (uint32_t)r64(IPU_BP);
    uint32_t ifc = (bp >> 8) & 0xF, fp = (bp >> 16) & 3;
    size_t resident = rt_ipu_test_resident_qw();
    if (resident > g_max_resident_qw) g_max_resident_qw = resident;
    if (ifc > kFifoQw || fp > 1 || ifc + fp > kFifoQw + 1) {
        rt_fatal("selftest", nullptr, "%s: IPU_BP=0x%05x claims IFC=%u FP=%u at %s; the input FIFO "
            "holds %u quadwords plus the one the decoder is inside", g_pass, bp, ifc, fp, where, kFifoQw);
    }
    if (resident > kFifoQw + 1) {
        rt_fatal("selftest", nullptr, "%s: %zu quadwords resident at %s but IPU_BP can only report "
            "%u; the library's MADR - (IFC + FP) * 16 rewind cannot reach them",
            g_pass, resident, where, kFifoQw + 1);
    }
    size_t bits = rt_ipu_test_avail_bits();
    if (bits > g_max_avail_bits) g_max_avail_bits = bits;
    if (bits > (size_t)(kFifoQw + 1) * 128) {
        rt_fatal("selftest", nullptr, "%s: %zu bits available to the decoder at %s; the input FIFO "
            "plus the open quadword is %u bits", g_pass, bits, where, (kFifoQw + 1) * 128);
    }
}

/* The retail stop/restart bracket, replayed with guest-visible registers
 * only. Snapshot: viBufStopDMA.s (CHCR = 5, then MADR/TADR/QWC/CHCR, wait
 * IPU_CTRL.OFC == 0, then IPU_BP). Restart: viBufRestartDMA.s at
 * 0x259D98-0x259DD0 (bp = BP & 0x7F, ifc = (BP >> 8) & 0xF,
 * fp = (BP >> 16) & 3, MADR' = MADR - (ifc + fp) * 16, QWC' = QWC + ifc + fp,
 * CHCR' = saved & 0x0FFFFFFF | id << 28 | 0x100), the BCLR at 0x25A06C, and
 * the MADR/TADR/QWC stores at 0x240544-0x240554. */
void library_stop_restart(const char* where) {
    g_in_restart = true;

    *ch4r(0) = 5; /* DIR=1, MOD=1, STR clear */
    rt_ipu_dma_stop(4);
    uint32_t madr = *ch4r(1), tadr = *ch4r(3), qwc = *ch4r(2), chcr = *ch4r(0);

    uint32_t ofc = (uint32_t)((r64(IPU_CTRL) >> 4) & 0xF);
    if (ofc != 0) {
        rt_fatal("selftest", nullptr, "%s: IPU_CTRL.OFC = %u at the stop; the library spins here "
            "and the test drains the output queue after every BDEC", g_pass, ofc);
    }
    check_fifo_bound(where);
    bool busy = (r64(IPU_CTRL) >> 31) & 1;
    if (busy) ++g_restarts_busy;

    uint32_t bpreg = (uint32_t)r64(IPU_BP);
    uint32_t bp = bpreg & 0x7F, ifc = (bpreg >> 8) & 0xF, fp = (bpreg >> 16) & 3;

    w32(IPU_CMD, bp); /* BCLR */

    uint32_t madr2 = madr - (ifc + fp) * 16;
    uint32_t qwc2 = qwc + ifc + fp;
    /* sceIpuRestartDMA.s at 0x2588BC skips its own restart when the rewound
     * MADR is zero. viBufRestartDMA has no such test: it stores MADR, TADR
     * and QWC at 0x240544-0x240554 and restarts the channel at 0x24059C
     * whenever its ring counter says there is buffered data, QWC' = 0
     * included. That is the case the retail movie hits when the chain has
     * drained and the input FIFO is empty, so it is replayed here too. */
    if (madr2 == 0) {
        g_in_restart = false;
        return;
    }
    if (qwc2 == 0) ++g_restarts_dry;
    *ch4r(1) = madr2;
    *ch4r(3) = tadr;
    *ch4r(2) = qwc2;
    /* Verbatim viBufRestartDMA.s at 0x259DD0: the saved CHCR with STR, TAG
     * field included. The stop above wrote the bare constant 5, so the id
     * in it reads back as 0 (REFE); the library does not put one back on
     * this path and the channel has to carry on regardless. Writing an id
     * of 3 here instead, which an earlier version of this test did, is the
     * test being kinder to the model than the game is. */
    *ch4r(0) = chcr | 0x100u;
    rt_ipu_dma_kick(4);
    ++g_restarts;
    g_in_restart = false;
}

void maybe_restart(const char* where) {
    if (!g_dma || g_restart_bytes == 0 || g_in_restart) return;
    uint32_t pos = stream_pos();
    if (pos - g_last_restart_pos < g_restart_bytes) return;
    g_last_restart_pos = pos;
    library_stop_restart(where);
}

/* The movie player arms ch3 before every BDEC, so the output FIFO is
 * drained while the command is still running and IPU_CTRL.OFC is back to 0
 * by the time viBufStopDMA takes its snapshot. This stands in for that
 * channel: everything the IPU has produced is taken out as it appears and
 * held here until the decoder asks for it. */
std::vector<uint8_t> g_out_held;

void drain_out() {
    uint8_t buf[4096];
    for (;;) {
        size_t n = rt_ipu_test_read_out(buf, sizeof(buf));
        if (n == 0) break;
        g_out_held.insert(g_out_held.end(), buf, buf + n);
    }
}

/* Takes exactly `n` bytes the drain collected, and says so if the command
 * produced a different amount. */
bool take_out(void* dst, size_t n) {
    drain_out();
    if (g_out_held.size() != n) return false;
    std::memcpy(dst, g_out_held.data(), n);
    g_out_held.clear();
    return true;
}

uint64_t cmd_result() {
    drain_out();
    uint64_t r = r64(IPU_CMD);
    uint64_t spins = 0;
    bool restarted = false;
    while (r >> 63) {
        if (!g_dma) {
            rt_fatal("selftest", nullptr, "IPU stayed busy after a command; the test feed ran dry or the model stalled");
        }
        if (++spins > 4000000) {
            rt_fatal("selftest", nullptr, "%s: IPU stayed busy for %" PRIu64 " polls "
                "(ch4 chcr=0x%08x madr=0x%08x qwc=%u tadr=0x%08x, %zu bits and %zu qwords resident); "
                "the model wedged", g_pass, spins, *ch4r(0), *ch4r(1), *ch4r(2), *ch4r(3),
                rt_ipu_test_avail_bits(), rt_ipu_test_resident_qw());
        }
        drain_out();
        check_fifo_bound("a busy poll with a command pending");
        if (*ch4r(0) & 0x100u) {
            maybe_restart("a busy poll");
        } else if (g_restart_mid_cmd && !restarted) {
            /* The decoder is starved and the ring is drained: this is the
             * state the movie player takes its snapshot in, with a command
             * still pending. The library does not test STR first. */
            restarted = true;
            library_stop_restart("a busy poll with a command pending");
        } else if (publish_more()) {
            restarted = false;
        } else {
            rt_fatal("selftest", nullptr, "%s: command 0x%08x is still pending with the whole chain "
                "consumed (%zu bits resident); the stream ran out mid-picture",
                g_pass, (uint32_t)r, rt_ipu_test_avail_bits());
        }
        r = r64(IPU_CMD);
    }
    drain_out();
    if (g_dma) check_fifo_bound("a command boundary");
    return r;
}

/* Issues one IPU command, giving the restart cadence a chance first. */
void ipu_cmd(uint32_t v) {
    maybe_restart("a command boundary");
    w32(IPU_CMD, v);
}

/* FDEC: skip `skip` bits (0..63) and return the next 32 bits. */
uint32_t fdec(unsigned skip) {
    ipu_cmd(0x40000000u | skip);
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
    ipu_cmd(0x30000000u | (tbl << 26));
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
    rt_log_info("selftest", "program stream found at LBA %u (DATA.DF + %u sectors)", pss_lsn, pss_lsn - df.lsn);

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
    rt_log_info("selftest", "demuxed %zu bytes of video elementary stream", es.size());
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

/* Flat non-intra matrix, fed out of band before the stream in both modes. */
const uint8_t* flat_niq() {
    static uint8_t flat[64];
    static bool init = false;
    if (!init) {
        std::memset(flat, 16, sizeof(flat));
        init = true;
    }
    return flat;
}

SeqState g_seq;
PicState g_pic;

/* Reconstructed I frame planes. */
std::vector<uint8_t> g_y, g_cb, g_cr;

uint64_t g_bdec_count = 0;

/* FNV-1a over every BDEC result in order, tagged with the macroblock
 * address. Two passes that agree on this agree on the decoded picture bit
 * for bit, including the macroblock ordering. */
uint64_t g_mb_hash = 0;
void hash_bytes(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) {
        g_mb_hash ^= b[i];
        g_mb_hash *= 0x100000001B3ull;
    }
}

void parse_sequence_header() {
    advance(32);
    g_seq.width = getbits(12);
    g_seq.height = getbits(12);
    /* Aspect ratio and frame rate codes are read rather than skipped: they
     * are how this target's one measurement of the movie's geometry and
     * rate is made, and the PAL/US comparison in docs/TARGET.md rests on
     * them. Frame rate codes, MPEG-2 13818-2 table 6-4: 1 = 24000/1001,
     * 2 = 24, 3 = 25, 4 = 30000/1001, 5 = 30, 6 = 50, 7 = 60000/1001,
     * 8 = 60. */
    const unsigned aspect_code = getbits(4);
    const unsigned frame_rate_code = getbits(4);
    advance(18 + 1 + 10 + 1); /* bitrate, marker, vbv, constrained */
    if (getbits(1)) { /* load_intra_quantiser_matrix: in-band SETIQ */
        ipu_cmd(0x50000000u);
        cmd_result();
        rt_log_info("selftest", "sequence header loads a custom intra matrix");
    }
    if (getbits(1)) {
        ipu_cmd(0x58000000u);
        cmd_result();
        rt_log_info("selftest", "sequence header loads a custom non-intra matrix");
    }
    g_seq.mb_w = (g_seq.width + 15) / 16;
    g_seq.mb_h = (g_seq.height + 15) / 16;
    rt_log_info("selftest", "sequence: %ux%u (%ux%u macroblocks), aspect code %u, frame rate code %u",
        g_seq.width, g_seq.height, g_seq.mb_w, g_seq.mb_h, aspect_code, frame_rate_code);
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
        rt_log_info("selftest", "pic coding ext: fcode %u/%u %u/%u idp=%u struct=%u fpfd=%u conceal=%u qst=%u ivf=%u as=%u",
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
    ipu_cmd(0x20000000u | (mbi ? 1u << 27 : 0) | (dcr ? 1u << 26 : 0) |
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
            if (!take_out(mb16, sizeof(mb16))) {
                rt_fatal("selftest", nullptr, "BDEC produced %zu bytes, expected 768", g_out_held.size());
            }
            uint32_t tag = (uint32_t)addr;
            hash_bytes(&tag, sizeof(tag));
            hash_bytes(mb16, sizeof(mb16));
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

/* ---- one decode pass ----------------------------------------------------- */

struct PassResult {
    unsigned i_mbs = 0, p_mbs = 0, p_slices = 0, b_mbs = 0, b_slices = 0;
    uint64_t i_hash = 0;
    std::vector<uint8_t> y, cb, cr;
};

/* Resets the model, loads the two out-of-band quantizer matrices, and walks
 * the stream. In direct mode the whole stream is fed up front; in dma mode
 * it comes through the ch4 chain built by build_chain(). i_only stops after
 * the first I picture, which is what the restart passes compare. */
PassResult decode_pass(const std::vector<uint8_t>& es, bool i_only) {
    w32(IPU_CTRL, 1u << 30);   /* soft reset: drops both queues and the DMA state */
    w32(IPU_CMD, 0x00000000u); /* BCLR */
    g_out_held.clear();
    g_mb_hash = 0xCBF29CE484222325ull;
    g_bdec_count = 0;
    g_last_restart_pos = 0;
    g_restarts = 0;
    g_restarts_busy = 0;
    g_restarts_dry = 0;
    g_max_avail_bits = 0;
    g_max_resident_qw = 0;

    if (g_dma) {
        reset_chain(g_slot_batch_want);
        ch4_start();
        g_last_restart_pos = stream_pos();
    } else {
        rt_ipu_test_feed(kDefaultIntraQ, 64);
        rt_ipu_test_feed(flat_niq(), 64);
        rt_ipu_test_feed(es.data(), es.size());
    }
    ipu_cmd(0x50000000u); /* SETIQ intra, from the first 64 stream bytes */
    cmd_result();
    ipu_cmd(0x58000000u); /* SETIQ non-intra, from the next 64 */
    cmd_result();

    PassResult res;
    unsigned i_mbs = 0, p_mbs = 0, p_slices = 0, b_mbs = 0, b_slices = 0;
    bool seq_seen = false, i_done = false, p_done = false, b_done = false, skip_pic = false;
    while (!(i_done && p_done && b_done)) {
        if (i_only && i_done) break;
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
            rt_log_info("selftest", "picture type %u", g_pic.type);
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
                if (i_mbs >= g_seq.mb_w * g_seq.mb_h) {
                    i_done = true;
                    res.i_hash = g_mb_hash;
                    res.y = g_y;
                    res.cb = g_cb;
                    res.cr = g_cr;
                }
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
        rt_fatal("selftest", nullptr, "%s: I picture decoded %u macroblocks, expected %u",
            g_pass, i_mbs, g_seq.mb_w * g_seq.mb_h);
    }
    res.i_mbs = i_mbs;
    res.p_mbs = p_mbs;
    res.p_slices = p_slices;
    res.b_mbs = b_mbs;
    res.b_slices = b_slices;
    return res;
}

/* ---- the drained-chain restart -------------------------------------------
 *
 * The register state the retail movie player was last seen wedged in on
 * Windows, replayed directly because the decode passes above cannot wander
 * into it: while a command is pending the model always keeps at least the
 * quadword the command rewound into, so IFC + FP is never zero there.
 *
 * The player reaches it in two steps. Its ch4 chain drains (the walk reads
 * the REFE that closed the ring, so STR clears, QWC is 0 and TADR sits on
 * the ring slot the guest has not written yet), and then viBufRestartDMA
 * issues its BCLR at 0x25A06C with the command still pending, which throws
 * the input FIFO away. The next snapshot therefore reads QWC = 0 with
 * IFC = FP = 0, so the restart at 0x240544-0x24059C hands the channel back
 * MADR' = MADR and QWC' = 0 at that same TADR. viBufAddDMA then appends
 * behind the REFE it rewrote into a REF and restarts at the same TADR
 * again.
 *
 * What this checks is the property that sequence depends on: a restart
 * with QWC' = 0 at a tag slot the guest has not written must not move the
 * channel on, because the ring extender is about to write that slot and
 * expects the walk to read it. If the model consumes the unwritten slot,
 * the append lands behind the walk and 2048 bytes of bitstream are
 * skipped. */
void drained_chain_restart_check() {
    g_pass = "drained-chain restart";
    w32(IPU_CTRL, 1u << 30);   /* soft reset */
    w32(IPU_CMD, 0x00000000u); /* BCLR */
    reset_chain(1);            /* one 2048-byte slot, closed by a REFE */
    ch4_start();

    /* Consume the published slot with FDECs until one cannot be satisfied.
     * Driven with raw register writes, not fdec(), so nothing extends the
     * ring behind the test's back. */
    uint64_t r = 0;
    unsigned guard = 0;
    do {
        w32(IPU_CMD, 0x4000003Fu); /* FDEC, skip 63 bits */
        r = r64(IPU_CMD);
        if (++guard > 100000) {
            rt_fatal("selftest", nullptr, "%s: the single published slot never ran out", g_pass);
        }
    } while (!(r >> 63));

    if (*ch4r(0) & 0x100u) {
        rt_fatal("selftest", nullptr, "%s: the command stalled with CHCR.STR still set (chcr=0x%08x "
            "madr=0x%08x qwc=%u tadr=0x%08x); the chain had not drained",
            g_pass, *ch4r(0), *ch4r(1), *ch4r(2), *ch4r(3));
    }
    if (*ch4r(2) != 0) {
        rt_fatal("selftest", nullptr, "%s: the drained chain left QWC = %u", g_pass, *ch4r(2));
    }

    const uint32_t madr = *ch4r(1), tadr = *ch4r(3), chcr = *ch4r(0);
    const uint32_t next_slot = (tadr - g_tag_first) / 16;
    if (next_slot != g_published) {
        rt_fatal("selftest", nullptr, "%s: TADR 0x%08x is slot %u, not the %zu the guest has "
            "published", g_pass, tadr, next_slot, g_published);
    }

    /* viBufRestartDMA's BCLR, issued with the command still pending. */
    w32(IPU_CMD, (uint32_t)r64(IPU_BP) & 0x7F);
    const uint32_t bpreg = (uint32_t)r64(IPU_BP);
    const uint32_t ifc = (bpreg >> 8) & 0xF, fp = (bpreg >> 16) & 3;
    if (ifc + fp != 0) {
        rt_fatal("selftest", nullptr, "%s: IPU_BP=0x%05x still claims %u resident quadwords after the "
            "BCLR; the snapshot this case is built on cannot arise", g_pass, bpreg, ifc + fp);
    }
    if (!(r64(IPU_CMD) >> 63)) {
        rt_fatal("selftest", nullptr, "%s: the BCLR dropped the pending command; the library issues "
            "it from inside its restart and expects the command to survive", g_pass);
    }

    /* The restart: MADR' = MADR - (IFC + FP) * 16 = MADR, QWC' = 0. */
    *ch4r(1) = madr;
    *ch4r(3) = tadr;
    *ch4r(2) = 0;
    *ch4r(0) = (chcr & 0x0FFFFFFFu) | 0x100u;
    rt_ipu_dma_kick(4);
    ++g_restarts;
    ++g_restarts_dry;
    if (rt_ipu_test_resident_qw() != 0) {
        rt_fatal("selftest", nullptr, "%s: the QWC' = 0 restart delivered %zu quadwords out of a tag "
            "slot the guest has not written", g_pass, rt_ipu_test_resident_qw());
    }
    if (*ch4r(0) & 0x100u) {
        rt_fatal("selftest", nullptr, "%s: the QWC' = 0 restart left CHCR.STR set (chcr=0x%08x "
            "madr=0x%08x qwc=%u tadr=0x%08x); the unwritten slot decodes as REFE with QWC 0, which "
            "ends the transfer", g_pass, *ch4r(0), *ch4r(1), *ch4r(2), *ch4r(3));
    }
    if (!(r64(IPU_CMD) >> 63)) {
        rt_fatal("selftest", nullptr, "%s: the pending command completed on a restart that delivered "
            "nothing", g_pass);
    }
    /* The slot is unwritten, so the walk has to leave TADR on it: the ring
     * extender is about to write this very slot and restart at this same
     * TADR, and a channel that had moved past it would never read what it
     * wrote. */
    if (*ch4r(3) != tadr) {
        rt_fatal("selftest", nullptr, "%s: the walk moved TADR from 0x%08x to 0x%08x over a ring slot "
            "the guest has not written; the ring extension that follows would land behind it",
            g_pass, tadr, *ch4r(3));
    }

    /* viBufAddDMA: rewrite the closing REFE into a REF, append behind it
     * and restart at the same TADR. The walk has to pick up the slot it
     * was sitting on. */
    if (!publish_more()) rt_fatal("selftest", nullptr, "%s: the chain had nothing left to publish", g_pass);
    const uint32_t slot_base = kRamBase + next_slot * kTagQwc * 16;
    if (*ch4r(1) < slot_base || *ch4r(1) > slot_base + (kFifoQw + 1) * 16) {
        rt_fatal("selftest", nullptr, "%s: after the ring extension the walk is at MADR 0x%08x, "
            "outside the %u quadwords at the head of slot %u (0x%08x); the appended slot was skipped",
            g_pass, *ch4r(1), kFifoQw + 1, next_slot, slot_base);
    }
    if (r64(IPU_CMD) >> 63) {
        rt_fatal("selftest", nullptr, "%s: the command is still pending after the ring extension "
            "delivered slot %u", g_pass, next_slot);
    }
    rt_log_info("selftest", "%s: the drained chain, the BCLR under a pending command and the QWC' = 0 "
        "restart all behaved as modelled; the unpublished slot at 0x%08x stopped the walk without "
        "consuming it, and the ring extension resumed at MADR 0x%08x in slot %u",
        g_pass, tadr, *ch4r(1), next_slot);
}

/* ---- a restart continues the chain ---------------------------------------
 *
 * The retail library stops ch4 by storing the bare constant 5 into CHCR
 * (viBufAddDMA.s at 0x2599AC-0x2599C0, viBufStopDMA.s at
 * 0x259BFC-0x259C40), which leaves the TAG field reading back as an id of
 * 0, REFE. Both restart paths then hand that CHCR back with STR set:
 * viBufRestartDMA.s's same-block path stores the saved value unchanged
 * ("ori $19, $5, 0x100" at 0x259DD0), and viBufAddDMA only forces an id
 * of 3 when it appended tags. A model that ends the chain on that id stops
 * the channel after the outstanding QWC on every one of those restarts and
 * starves the decoder with the ring still full, which is what the movie
 * did: a picture every four seconds. */
void restart_continues_chain_check() {
    g_pass = "a restart continues the chain";
    w32(IPU_CTRL, 1u << 30);
    w32(IPU_CMD, 0x00000000u);
    reset_chain(4);            /* four slots published, closed by a REFE */
    ch4_start();

    /* Get the walk inside the first tag's payload with plenty still to
     * come, the state the player's stop always finds it in. */
    for (int i = 0; i < 32 && !(r64(IPU_CMD) >> 63); ++i) w32(IPU_CMD, 0x4000003Fu);

    *ch4r(0) = 5; /* the library's stop, and what clears CHCR.TAG */
    rt_ipu_dma_stop(4);
    const uint32_t madr = *ch4r(1), tadr = *ch4r(3), qwc = *ch4r(2), chcr = *ch4r(0);
    if (qwc == 0) {
        rt_fatal("selftest", nullptr, "%s: the stop found QWC = 0; the check needs the walk inside a "
            "tag's payload", g_pass);
    }
    if (((chcr >> 28) & 7) != 0) {
        rt_fatal("selftest", nullptr, "%s: CHCR = 0x%08x after a store of 5; the check is about the "
            "TAG field that store clears", g_pass, chcr);
    }
    const uint32_t bpreg = (uint32_t)r64(IPU_BP);
    const uint32_t bp = bpreg & 0x7F, ifc = (bpreg >> 8) & 0xF, fp = (bpreg >> 16) & 3;
    w32(IPU_CMD, bp); /* BCLR */

    /* viBufRestartDMA's same-block restart, verbatim: the saved CHCR with STR. */
    *ch4r(1) = madr - (ifc + fp) * 16;
    *ch4r(3) = tadr;
    *ch4r(2) = qwc + ifc + fp;
    *ch4r(0) = chcr | 0x100u;
    rt_ipu_dma_kick(4);

    /* Drain far enough for the walk to run past the outstanding QWC. */
    for (int i = 0; i < 20000 && *ch4r(3) == tadr; ++i) {
        w32(IPU_CMD, 0x4000003Fu);
        if (r64(IPU_CMD) >> 63) break;
    }
    if (*ch4r(3) == tadr) {
        rt_fatal("selftest", nullptr, "%s: the restart ended the chain at TADR 0x%08x with %zu slots "
            "of the ring still published; CHCR = 0x%08x madr=0x%08x qwc=%u, %zu qwords resident. The "
            "channel stopped on the tag id the guest's own CHCR store cleared instead of reading the "
            "tag at TADR", g_pass, tadr, g_published, *ch4r(0), *ch4r(1), *ch4r(2),
            rt_ipu_test_resident_qw());
    }
    rt_log_info("selftest", "%s: the restart walked past its %u outstanding quadwords into the tag at "
        "0x%08x and carried on to TADR 0x%08x", g_pass, qwc + ifc + fp, tadr, *ch4r(3));
}

/* ---- a BDEC trailing scan across the stream's zero stuffing --------------
 *
 * The movie is padded to a constant bit rate with zero bytes, and near the
 * start it is almost all padding: the elementary stream read off the disc
 * here has runs of up to 18389 zero bytes after the last slice of a
 * picture. The BDEC that decodes that last macroblock scans them all
 * looking for the next start code, so one command consumes far more than
 * the 8-quadword input FIFO.
 *
 * The passes above never reach that. They start 4000 sectors into the movie
 * where the pictures are large and the padding between them is short, so
 * every command fits inside the FIFO and the model's rewind-and-replay of a
 * starved command is never asked to reach past what IPU_BP can describe.
 *
 * The retail movie starts at sector 0 of the stream and hits it on the
 * first I picture of the second GOP. In the Windows trace of that run
 * (dist/windows/icorecomp.log, trace 2310) BDEC 0x28010000 stalled 10078
 * bytes into the padding with 630 quadwords sitting between the decode
 * cursor and the end of the input, IPU_BP.IFC saturated at 8, and the
 * command never completed again: the chain had drained and every replay
 * started from the same rewound cursor. The movie stopped there.
 *
 * So this pass decodes one of those picture-then-padding stretches through
 * the ch4 chain, published one 2048-byte slot at a time so the scan starves
 * repeatedly, and requires both that IPU_BP keeps describing the state (the
 * check_fifo_bound call in the busy poll) and that the picture comes out
 * bit-identical to the direct feed. */
void stuffing_tail_scan_check() {
    std::vector<uint8_t> lead;
    extract_video_es(lead, 768u << 10, 0);

    /* Find a sequence header whose own first picture, and nothing after it,
     * runs into a stuffing run longer than the input FIFO: that picture is
     * the I picture the pass decodes, and its last macroblock's BDEC is the
     * one whose trailing scan has to cross the run. A second picture start
     * code before the run means the run belongs to a later picture, so that
     * sequence header is no good. Fatal if the stream has none, because the
     * check would otherwise go quiet without saying it had stopped covering
     * anything. */
    const size_t kRunMin = (kFifoQw + 1) * 16 * 4; /* four FIFOs of padding */
    auto code_at = [&lead](size_t at, uint8_t id) {
        return at + 4 <= lead.size() && lead[at] == 0 && lead[at + 1] == 0 && lead[at + 2] == 1 &&
               lead[at + 3] == id;
    };
    size_t seq = 0, run_at = 0, run_len = 0;
    for (size_t i = 0; i + 4 < lead.size() && !run_at; ++i) {
        if (!code_at(i, 0xB3)) continue;
        size_t pic = i;
        while (pic + 4 < lead.size() && !code_at(pic, 0x00)) ++pic;
        bool second_picture = false;
        /* One byte at a time: a start code is preceded by zero bytes, so
         * stepping over a short zero run would step over the code with
         * it. */
        for (size_t z = pic + 4; z + kRunMin < lead.size(); ++z) {
            if (code_at(z, 0x00)) { second_picture = true; break; }
            if (lead[z] != 0) continue;
            size_t e = z;
            while (e < lead.size() && lead[e] == 0) ++e;
            if (e - z >= kRunMin) { run_at = z; run_len = e - z; break; }
        }
        if (second_picture) { run_at = run_len = 0; continue; }
        if (run_at) seq = i;
    }
    if (!run_at) {
        rt_fatal("selftest", nullptr, "no sequence header whose first picture runs into a stuffing "
            "run of at least %zu bytes in the first %zu bytes of the stream; this check has nothing "
            "to cover", kRunMin, lead.size());
    }

    /* The segment runs from that sequence header to a little past the end of
     * the stuffing, so the scan has a start code to find. */
    size_t end = run_at + run_len + (8u << 10);
    if (end > lead.size()) end = lead.size();
    std::vector<uint8_t> seg(lead.begin() + (ptrdiff_t)seq, lead.begin() + (ptrdiff_t)end);
    rt_log_info("selftest", "stuffing pass: sequence header at stream byte %zu, %zu bytes of headers and "
        "picture data, then %zu bytes of zero stuffing for the last macroblock's BDEC to scan "
        "(%zu times the %zu-quadword input FIFO); segment %zu bytes",
        seq, run_at - seq, run_len, run_len / ((kFifoQw + 1) * 16), (size_t)(kFifoQw + 1),
        seg.size());

    const char* prev_pass = g_pass;
    const size_t prev_batch = g_slot_batch_want;
    const uint32_t prev_restart = g_restart_bytes;
    const bool prev_mid = g_restart_mid_cmd;
    g_dma = false;
    g_restart_bytes = 0;
    g_restart_mid_cmd = false;
    g_pass = "stuffing direct";
    PassResult ref = decode_pass(seg, true);

    build_chain(kDefaultIntraQ, flat_niq(), seg, seg.size());
    g_dma = true;
    /* Static, because g_pass points at it and outlives the loop: every
     * fatal after this function returns would otherwise name a dead
     * stack buffer. */
    static char name[96];
    static const size_t kBatch[] = {1, 2, 5};
    for (size_t batch : kBatch) {
        std::snprintf(name, sizeof(name), "stuffing dma batch %zu slot%s", batch, batch == 1 ? "" : "s");
        g_pass = name;
        g_slot_batch_want = batch;
        PassResult r = decode_pass(seg, true);
        if (r.i_hash != ref.i_hash || r.y != ref.y) {
            rt_fatal("selftest", nullptr, "%s: the picture differs from the direct feed "
                "(hash 0x%016" PRIx64 " vs 0x%016" PRIx64 ", %" PRIu64 " ring extensions)",
                g_pass, r.i_hash, ref.i_hash, g_refills);
        }
        rt_log_info("selftest", "%s: the scan crossed the stuffing and the picture matches "
            "(%u macroblocks, %" PRIu64 " ring extensions, peak %zu qwords / %zu bits resident)",
            g_pass, r.i_mbs, g_refills, g_max_resident_qw, g_max_avail_bits);
    }
    g_dma = false;
    g_slot_batch_want = prev_batch;
    g_restart_bytes = prev_restart;
    g_restart_mid_cmd = prev_mid;
    g_pass = prev_pass;
}

/* ---- the trailing scan's resume point ------------------------------------
 *
 * A BDEC's trailing start-code scan commits every byte it skips, so a stall
 * inside it resumes where it had reached instead of replaying the command.
 * The point it resumes at carries state that the bitstream alone does not
 * describe.
 *
 * The scan opens by testing whether the next 8 bits are zero, read at
 * whatever bit position the macroblock ended on, and then aligns to the
 * following byte. Only the leading bits of that byte were part of the test:
 * the rest of it can be anything. A scan that starves right after the
 * alignment therefore has to remember that it is already inside the zero
 * run. Re-reading the opening test at the aligned byte can answer
 * differently, and the command then returns with IPU_CTRL.ECD clear where
 * an uninterrupted decode sets it, which is the difference between libmpeg
 * seeing the end of a slice and not seeing it.
 *
 * The retail stream cannot be steered onto that position on demand, so this
 * case builds the eight bytes that stand on it and decodes them three ways:
 * with the whole segment in front of the decoder, with the feed cut at the
 * byte the stalled scan needs next, and through the ch4 chain with the
 * macroblock at the end of the first published slot so the DMA starves at
 * the same place. All three have to agree on ECD, SCD and the macroblock. */

/* One intra macroblock, every block DC only with dct_dc_size 0: luma '100'
 * (Table B-12) plus EOB '10' (Table B-14) is 5 bits, chroma '00' (B-13)
 * plus EOB is 4, so the six blocks are 4 * 5 + 2 * 4 = 28 bits and the
 * command ends four bits short of a byte. Bits 28..31 are zero and so is
 * the top nibble of the byte after them, which is what makes the scan's
 * opening 8-bit test read zero; the low nibble is not, so the aligned byte
 * the scan commits onto is 0x0F. The 24 bits there are neither 0 nor 1, so
 * an uninterrupted scan sets ECD and stops. */
const uint8_t kTailMb[] = {0x94, 0xA5, 0x22, 0x20, 0x0F, 0x00, 0x00, 0x01};

/* How much of it the starved feed hands over: the macroblock, the zero gate
 * and the aligned byte, but not the third byte the scan's 24-bit read needs
 * after it has aligned and committed. */
constexpr size_t kTailStall = 6;

constexpr uint32_t kTailBdec = 0x2C000000u; /* BDEC, MBI = 1, DCR = 1, QSC = 0 */
constexpr size_t kMbBytes = 768;            /* Y 16x16 + Cb 8x8 + Cr 8x8, 16-bit */

struct TailResult {
    uint32_t ecd = 0, scd = 0;
    std::vector<uint8_t> mb;
};

/* Drops both queues and programs IPU_CTRL the way libmpeg does before an I
 * picture's slices: 8-bit DC precision, zigzag scan, table B-14, linear
 * quantiser scale. */
void tail_reset_model() {
    w32(IPU_CTRL, 1u << 30);   /* soft reset */
    w32(IPU_CMD, 0x00000000u); /* BCLR */
    g_out_held.clear();
    w32(IPU_CTRL, 1u << 24);   /* PCT = 1 (I picture), every other field 0 */
}

TailResult tail_collect() {
    TailResult r;
    const uint32_t ctrl = ipu_ctrl();
    r.ecd = (ctrl >> 14) & 1;
    r.scd = (ctrl >> 15) & 1;
    drain_out();
    r.mb = g_out_held;
    g_out_held.clear();
    return r;
}

void tail_feed_direct(const std::vector<uint8_t>& es, size_t bytes) {
    rt_ipu_test_feed(kDefaultIntraQ, 64);
    rt_ipu_test_feed(flat_niq(), 64);
    rt_ipu_test_feed(es.data(), bytes);
}

/* The two SETIQ commands that take the matrices out of the head of the
 * feed, then the FDEC walk up to the crafted macroblock. */
void tail_seek(size_t mb_at) {
    ipu_cmd(0x50000000u);
    cmd_result();
    ipu_cmd(0x58000000u);
    cmd_result();
    advance((unsigned)(mb_at * 8));
}

/* Issues the BDEC and requires it to stall with the macroblock already
 * out, which is the state the trailing scan stalls in and nothing else
 * does. */
void tail_bdec_must_stall(size_t fed) {
    w32(IPU_CMD, kTailBdec);
    if (!(r64(IPU_CMD) >> 63)) {
        rt_fatal("selftest", nullptr, "%s: the BDEC completed with %zu bytes of the segment in front "
            "of the decoder; this case only covers anything while it stalls inside the trailing scan",
            g_pass, fed);
    }
    if (rt_ipu_test_out_avail() != kMbBytes) {
        rt_fatal("selftest", nullptr, "%s: the stalled BDEC has %zu output bytes queued, expected the "
            "%zu-byte macroblock; the stall is before the trailing scan, not inside it",
            g_pass, rt_ipu_test_out_avail(), kMbBytes);
    }
}

void tail_compare(const TailResult& ref, const TailResult& r) {
    if (r.ecd != ref.ecd || r.scd != ref.scd) {
        rt_fatal("selftest", nullptr, "%s: the trailing scan ended with ECD=%u SCD=%u; the "
            "uninterrupted decode of the same bits ends with ECD=%u SCD=%u. The retry re-read the "
            "scan's opening test at the byte the alignment committed onto instead of resuming "
            "inside the zero run", g_pass, r.ecd, r.scd, ref.ecd, ref.scd);
    }
    if (r.mb != ref.mb) {
        rt_fatal("selftest", nullptr, "%s: the macroblock differs from the uninterrupted decode "
            "(%zu bytes against %zu)", g_pass, r.mb.size(), ref.mb.size());
    }
}

void bdec_tail_resume_check() {
    const char* prev_pass = g_pass;
    const size_t prev_batch = g_slot_batch_want;
    const uint32_t prev_restart = g_restart_bytes;
    const bool prev_mid = g_restart_mid_cmd;
    const bool prev_dma = g_dma;
    g_restart_bytes = 0;
    g_restart_mid_cmd = false;

    /* The macroblock sits at the end of the first 2048-byte chain slot, so
     * the dma pass runs out of input at the same read the direct one is cut
     * at: the bytes the 24-bit read wants are in the slot behind it. The
     * 128 bytes are the two quantiser matrices build_chain puts in front of
     * the stream. */
    const size_t kSlot = (size_t)kTagQwc * 16;
    const size_t mb_at = kSlot - 128 - kTailStall;
    std::vector<uint8_t> es(mb_at + sizeof kTailMb + 64, 0);
    std::memcpy(&es[mb_at], kTailMb, sizeof kTailMb);
    rt_log_info("selftest", "tail resume: one crafted macroblock at stream byte %zu, ending 28 bits in "
        "with the byte after the alignment nonzero (0x%02x); the feed is cut %zu bytes later",
        mb_at, kTailMb[4], kTailStall);

    /* Reference: the whole segment in front of the decoder, no stall. */
    g_dma = false;
    g_pass = "bdec tail resume, direct";
    tail_reset_model();
    tail_feed_direct(es, es.size());
    tail_seek(mb_at);
    ipu_cmd(kTailBdec);
    cmd_result();
    TailResult ref = tail_collect();
    if (!ref.ecd || ref.scd) {
        rt_fatal("selftest", nullptr, "%s: the uninterrupted scan ended with ECD=%u SCD=%u; the "
            "crafted bytes are meant to put nonzero data at the aligned byte, which sets ECD",
            g_pass, ref.ecd, ref.scd);
    }
    if (ref.mb.size() != kMbBytes) {
        rt_fatal("selftest", nullptr, "%s: the BDEC produced %zu output bytes, expected %zu",
            g_pass, ref.mb.size(), kMbBytes);
    }
    rt_log_info("selftest", "%s: ECD set on the aligned byte, %zu-byte macroblock out", g_pass, ref.mb.size());

    /* The same bits, with the feed cut at the byte the scan needs next. */
    g_pass = "bdec tail resume, starved direct";
    tail_reset_model();
    tail_feed_direct(es, mb_at + kTailStall);
    tail_seek(mb_at);
    tail_bdec_must_stall(mb_at + kTailStall);
    rt_ipu_test_feed(es.data() + mb_at + kTailStall, es.size() - mb_at - kTailStall);
    if (r64(IPU_CMD) >> 63) {
        rt_fatal("selftest", nullptr, "%s: the BDEC is still pending with the rest of the segment fed",
            g_pass);
    }
    TailResult starved = tail_collect();
    tail_compare(ref, starved);
    rt_log_info("selftest", "%s: the scan resumed inside the zero run and agrees with the direct decode "
        "(ECD=%u SCD=%u)", g_pass, starved.ecd, starved.scd);

    /* And through the chain, one slot at a time. */
    g_pass = "bdec tail resume, dma";
    g_dma = true;
    g_slot_batch_want = 1;
    build_chain(kDefaultIntraQ, flat_niq(), es, es.size());
    tail_reset_model();
    reset_chain(g_slot_batch_want);
    ch4_start();
    tail_seek(mb_at);
    if (g_published != 1) {
        rt_fatal("selftest", nullptr, "%s: %zu slots were published before the BDEC; this case needs "
            "the crafted macroblock to be the last bytes of the first one", g_pass, g_published);
    }
    tail_bdec_must_stall(kSlot);
    if (!publish_more()) {
        rt_fatal("selftest", nullptr, "%s: the chain had nothing left to publish", g_pass);
    }
    cmd_result();
    TailResult dma = tail_collect();
    tail_compare(ref, dma);
    rt_log_info("selftest", "%s: the scan crossed the slot boundary and agrees with the direct decode "
        "(ECD=%u SCD=%u, %" PRIu64 " ring extensions)", g_pass, dma.ecd, dma.scd, g_refills);

    g_dma = prev_dma;
    g_slot_batch_want = prev_batch;
    g_restart_bytes = prev_restart;
    g_restart_mid_cmd = prev_mid;
    g_pass = prev_pass;
}

/* ---- host cost of one IPU register access --------------------------------
 *
 * The movie is register-bound: the MPEG library scans the stream's zero
 * stuffing one byte at a time with FDEC, and the 22:45 Windows log measured
 * about 33000 register accesses per host field with the "ipu" and "mmio"
 * prof buckets together taking 9.8 ms of a 16.7 ms field. At the rate a
 * real-time movie needs, roughly two million accesses a second, the host
 * cost per access is what decides whether the port holds the field rate the
 * game programmed, 59.94 on NTSC or 50 on PAL.
 *
 * This replays that scan against the real decoder state fed from the disc
 * and reports it. Only the IPU handler is measured, not the mmio.cpp
 * wrapper around it (this target does not link it), so the number to
 * compare it against is the log's "ipu" bucket, 0.18 us per access. The
 * second and third loops price the two things the wrapper adds on top: the
 * profiler's clock reads and log_access's per-address hash map. */
void register_access_benchmark(const std::vector<uint8_t>& es) {
    const size_t kIters = 262144;
    size_t bytes = 128 + kIters + (64u << 10);
    if (bytes > es.size()) bytes = es.size();
    build_chain(kDefaultIntraQ, flat_niq(), es, bytes);
    g_dma = true;
    g_restart_bytes = 0;
    g_restart_mid_cmd = false;
    g_pass = "register access benchmark";
    w32(IPU_CTRL, 1u << 30);
    w32(IPU_CMD, 0x00000000u);
    g_out_held.clear();
    reset_chain(0); /* whole chain published: the walk never runs dry */
    ch4_start();

    /* The library's per-byte sequence: issue FDEC with a skip of 8, read
     * the result and the status back, and peek IPU_TOP. Four accesses, the
     * ratio the log's per-register access counts show. */
    auto t0 = std::chrono::steady_clock::now();
    uint64_t sink = 0;
    size_t n = 0;
    for (; n < kIters; ++n) {
        w32(IPU_CMD, 0x40000008u);
        sink += r64(IPU_CMD);
        if (sink >> 63) break; /* stalled: the chain ran out, stop here */
        sink += r64(IPU_CTRL);
        sink += r64(IPU_TOP);
    }
    auto t1 = std::chrono::steady_clock::now();
    const double ns = (double)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    const uint64_t accesses = (uint64_t)n * 4;
    if (n < kIters / 2) {
        rt_fatal("selftest", nullptr, "benchmark stalled after %zu of %zu iterations; the chain ran "
            "dry and the number would be meaningless", n, kIters);
    }
    const double per = ns / (double)accesses;
    /* Two million accesses a second is what a real-time movie asks for. The
     * field the budget is spent in is the video mode's, not a constant:
     * 1/59.94 s on NTSC and 1/50 s on PAL, and the PAL disc plays a movie
     * in either mode from its own display option, so both are reported. A
     * PAL field is 20 ms against NTSC's 16.7, which is the more forgiving
     * of the two, so the NTSC number stays the one to judge by. */
    const double ms_per_field_ntsc = 2.0e6 * per / 1.0e9 / RT_FIELD_HZ_NTSC * 1000.0;
    const double ms_per_field_pal = 2.0e6 * per / 1.0e9 / RT_FIELD_HZ_PAL * 1000.0;
    rt_log_info("selftest", "register access benchmark: %" PRIu64 " accesses (%zu FDEC bytes scanned) in "
        "%.1f ms = %.1f ns per access; at two million accesses a second that is %.2f ms of host time "
        "per NTSC field and %.2f ms per PAL field (sink 0x%016" PRIx64 ")",
        accesses, n, ns / 1.0e6, per, ms_per_field_ntsc, ms_per_field_pal, sink);

    /* What the profiler costs when it is on: two clock reads per zone. The
     * movie path used to open two nested zones per access (mmio.cpp's plus
     * hw/ipu.cpp's own) and now opens one, so both are timed here. */
    const bool was_on = g_rt_prof_on;
    g_rt_prof_on = true;
    auto p0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kIters; ++i) {
        RT_PROF_ZONE(RT_PROF_MMIO);
        RT_PROF_ZONE(RT_PROF_IPU);
    }
    auto p1 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kIters; ++i) {
        RT_PROF_ZONE(RT_PROF_IPU);
    }
    auto p2 = std::chrono::steady_clock::now();
    g_rt_prof_on = was_on;
    rt_log_info("selftest", "profiler cost: %.1f ns per access for two nested zones (four clock reads), "
        "%.1f ns for the one zone mmio.cpp now opens",
        (double)std::chrono::duration_cast<std::chrono::nanoseconds>(p1 - p0).count() / (double)kIters,
        (double)std::chrono::duration_cast<std::chrono::nanoseconds>(p2 - p1).count() / (double)kIters);

    /* What mmio.cpp's per-address access counter costs: the hash map probe
     * it used to do on every access, against the direct-mapped front now in
     * front of it. Five registers in rotation, as the movie uses. */
    {
        struct Stat { uint64_t count = 0; uint64_t last = 0; };
        std::unordered_map<uint32_t, Stat> stats;
        auto h0 = std::chrono::steady_clock::now();
        for (size_t i = 0; i < kIters; ++i) {
            Stat& st = stats[0x10002000u + ((i % 5) << 4)];
            ++st.count;
            st.last = i;
        }
        auto h1 = std::chrono::steady_clock::now();
        uint32_t caddr[128] = {0};
        Stat* cst[128] = {nullptr};
        for (size_t i = 0; i < kIters; ++i) {
            const uint32_t a = 0x10002000u + (uint32_t)((i % 5) << 4);
            const size_t k = (a >> 4) & 127;
            Stat* st = (cst[k] && caddr[k] == a) ? cst[k] : nullptr;
            if (!st) { st = &stats[a]; caddr[k] = a; cst[k] = st; }
            ++st->count;
            st->last = i;
        }
        auto h2 = std::chrono::steady_clock::now();
        rt_log_info("selftest", "per-address access counter: %.1f ns per access through the hash map, "
            "%.1f ns through the direct-mapped front",
            (double)std::chrono::duration_cast<std::chrono::nanoseconds>(h1 - h0).count() / (double)kIters,
            (double)std::chrono::duration_cast<std::chrono::nanoseconds>(h2 - h1).count() / (double)kIters);
    }
    g_dma = false;
}

} // namespace

int main() {
    /* Info, not the runtime's warn default: this tool's output is its
     * progress lines, and they are all info. */
    rt_log_set_initial_level(RT_LOG_INFO);

    rt_log_info("selftest", "IPU selftest starting");
    rt_iso_mount();

    std::vector<uint8_t> es;
    const char* skip_env = std::getenv("ICORECOMP_IPU_SELFTEST_SKIP");
    uint32_t skip = skip_env ? (uint32_t)std::strtoul(skip_env, nullptr, 0) : 4000;
    extract_video_es(es, 6u << 20, skip);
    if (es.size() < (1u << 20)) {
        rt_fatal("selftest", nullptr, "too little video ES extracted (%zu bytes)", es.size());
    }

    /* ---- pass 1: direct feed, the decode reference ---------------------- */
    g_pass = "direct";
    g_dma = false;
    g_restart_bytes = 0;
    PassResult ref = decode_pass(es, false);

    rt_log_info("selftest", "I picture: %u macroblocks; P pass: %u coded MBs / %u slices; B pass: %u coded MBs / %u slices;"
        " %" PRIu64 " BDECs total; I hash 0x%016" PRIx64,
        ref.i_mbs, ref.p_mbs, ref.p_slices, ref.b_mbs, ref.b_slices, g_bdec_count, ref.i_hash);

    /* Luma statistics. */
    double sum = 0, sum2 = 0;
    for (uint8_t v : ref.y) { sum += v; sum2 += (double)v * v; }
    double mean = sum / ref.y.size();
    double var = sum2 / ref.y.size() - mean * mean;
    rt_log_info("selftest", "I frame luma: mean %.1f stddev %.1f", mean, var > 0 ? std::sqrt(var) : 0.0);
    if (mean < 10.0 || mean > 245.0 || var < 4.0) {
        rt_fatal("selftest", nullptr, "implausible luma statistics (mean %.1f var %.1f)", mean, var);
    }
    if (ref.p_mbs == 0) rt_fatal("selftest", nullptr, "P residual pass decoded no macroblocks");

    /* CSC pass: convert the reconstructed I frame through the IPU. */
    w32(IPU_CMD, 0x00000000u); /* BCLR: drop the remaining ES */
    w32(IPU_CMD, 0x90000000u); /* SETTH 0/0 */
    std::vector<uint8_t> rgba((size_t)g_seq.width * g_seq.height * 4);
    uint8_t mb8[384], rgbmb[1024];
    for (unsigned my = 0; my < g_seq.mb_h; ++my) {
        for (unsigned mx = 0; mx < g_seq.mb_w; ++mx) {
            for (int yy = 0; yy < 16; ++yy)
                for (int xx = 0; xx < 16; ++xx)
                    mb8[yy * 16 + xx] = ref.y[(my * 16 + yy) * g_seq.width + mx * 16 + xx];
            for (int yy = 0; yy < 8; ++yy)
                for (int xx = 0; xx < 8; ++xx) {
                    size_t at = (my * 8 + yy) * (g_seq.width / 2) + mx * 8 + xx;
                    mb8[256 + yy * 8 + xx] = ref.cb[at];
                    mb8[320 + yy * 8 + xx] = ref.cr[at];
                }
            rt_ipu_test_feed(mb8, sizeof(mb8));
            ipu_cmd(0x70000001u); /* CSC, 1 macroblock */
            cmd_result();
            if (!take_out(rgbmb, sizeof(rgbmb))) {
                rt_fatal("selftest", nullptr, "CSC produced %zu bytes, expected 1024", g_out_held.size());
            }
            for (int yy = 0; yy < 16; ++yy)
                std::memcpy(&rgba[((my * 16 + yy) * g_seq.width + mx * 16) * 4], &rgbmb[yy * 64], 64);
        }
    }

    const char* out = std::getenv("ICORECOMP_IPU_SELFTEST_OUT");
    std::string path = out ? out : "/tmp/ipu_selftest_frame0.bmp";
    write_bmp(path.c_str(), rgba.data(), g_seq.width, g_seq.height);
    rt_log_info("selftest", "wrote decoded I frame to %s", path.c_str());

    /* ---- pass 2..n: the same I picture, fed through the ch4 chain, with
     * the library's stop/restart bracket replayed at several cadences.
     *
     * The I picture needs only the head of the stream, so the chain is
     * built over the first 1 MB: enough for the sequence header, the
     * picture headers and every slice, and small enough to keep the run
     * under a second. */
    size_t chain_bytes = es.size() < (1u << 20) ? es.size() : (size_t)(1u << 20);
    build_chain(kDefaultIntraQ, flat_niq(), es, chain_bytes);
    g_dma = true;

    /* Cadences in stream bytes. 0 is the no-restart control (does the DMA
     * feed alone reproduce the direct feed?); the rest are chosen to land
     * restarts on and off quadword boundaries, inside and across the
     * 2048-byte tag slots.
     *
     * The slot batch is how many 2048-byte slots the ring holds before the
     * guest extends it. With the whole chain published at once the DMAC
     * never runs dry and no command ever stalls; with one or two slots the
     * decoder starves regularly, which is the state the movie player is in
     * every time it takes its stop/restart snapshot. */
    static const uint32_t kCadence[] = {0, 16, 48, 113, 128, 1024, 2048, 2053, 8192};
    static const size_t kBatch[] = {0, 1, 2, 5};
    char name[96];
    for (size_t batch : kBatch) {
        for (int mid = 0; mid < 2; ++mid) {
            for (uint32_t cad : kCadence) {
                if (mid && cad == 0) continue;
                std::snprintf(name, sizeof(name), "dma batch %zu slot%s, restart every %u bytes%s",
                    batch ? batch : g_slots, (batch == 1) ? "" : "s", cad, mid ? ", mid-command too" : "");
                g_pass = name;
                g_slot_batch_want = batch;
                g_restart_bytes = cad;
                g_restart_mid_cmd = mid != 0;
                PassResult r = decode_pass(es, true);
                if (r.i_hash != ref.i_hash || r.y != ref.y || r.cb != ref.cb || r.cr != ref.cr) {
                    size_t first = 0;
                    while (first < r.y.size() && first < ref.y.size() && r.y[first] == ref.y[first]) ++first;
                    rt_fatal("selftest", nullptr, "%s: the decoded I picture differs from the direct feed "
                        "(hash 0x%016" PRIx64 " vs 0x%016" PRIx64 ", first luma difference at sample %zu = "
                        "macroblock %zu; %" PRIu64 " restarts, %" PRIu64 " of them with a command pending, "
                        "%" PRIu64 " ring extensions)",
                        g_pass, r.i_hash, ref.i_hash, first,
                        first < r.y.size() ? (first / g_seq.width / 16) * g_seq.mb_w + (first % g_seq.width) / 16 : 0,
                        g_restarts, g_restarts_busy, g_refills);
                }
                rt_log_info("selftest", "%s: I picture matches (%u macroblocks, %" PRIu64 " restarts, "
                    "%" PRIu64 " with a command pending, %" PRIu64 " of them with QWC' = 0, "
                    "%" PRIu64 " ring extensions, peak %zu qwords / %zu bits resident)",
                    g_pass, r.i_mbs, g_restarts, g_restarts_busy, g_restarts_dry, g_refills,
                    g_max_resident_qw, g_max_avail_bits);
            }
        }
    }

    /* ---- the two chain-restart shapes ------------------------------------ */
    restart_continues_chain_check();
    drained_chain_restart_check();
    stuffing_tail_scan_check();
    bdec_tail_resume_check();
    register_access_benchmark(es);

    g_dma = false;
    g_restart_mid_cmd = false;
    g_pass = "direct";

    rt_log_info("selftest", "IPU selftest PASSED");
    return 0;
}
