/* hw/vu1rt.cpp: VU1 execution-side runtime state.
 *
 * Owns:
 *   - THE Vu1State: the one instance both the VIF1 HLE (UNPACK/MSCAL) and
 *     the recompiled VU1 microprograms operate on.
 *   - the 0x11000000 EE-bus window backing, overlaid so the window's data1
 *     region (offset 0xC000..0xFFFF) is Vu1State::mem itself. EE code that
 *     reads VU1 results through the window and recompiled code that writes
 *     vu->mem see the same bytes.
 *   - the VU1 micro memory shadow (16 KB) written by VIF1 MPG, plus the
 *     upload extent and hash bookkeeping.
 *   - the microprogram registry (rt_vu1_register) and dispatch (MSCAL).
 *   - rt_xgkick: GIF packet parse out of vu->mem onto PATH1.
 *
 * Hash and binding contract (matches the VU1 recompiler's generated
 * vu1_table.c):
 *   - The hash function is rc_vu1_hash (recomp_ops.h): FNV-1a 32 seeded
 *     with 0x811C9DC5 ^ byte_length over the uploaded instruction bytes.
 *   - An upload is the concatenation of MPG payloads starting at micro
 *     address 0 (MPG to address 0 begins a new upload; higher-address MPG
 *     segments extend it). DMA tags and VIF codes never land in micro mem.
 *   - The hash is computed over the full uploaded extent from address 0 at
 *     the first MSCAL after the upload changes, and resolves to at most
 *     ONE bound program. Every MSCAL/MSCALF/MSCNT (including ICO's
 *     nonzero stub offsets 0x10..0xC0) routes through that one bound entry
 *     with vu->pc set to the byte offset (MSCAL operand * 8); the entry's
 *     own pc switch does the rest. MSCNT calls the entry with vu->pc left
 *     exactly as the previous run stored it at its E-bit stop.
 *
 * Overlay layout (see rt_vu1_window_page):
 *   Vu1State::mem sits at byte offset 604 inside the struct (fixed by the
 *   ABI header). To make window_base + 0xC000 == state->mem while keeping
 *   the struct 16-byte aligned, the window base is placed at raw + 12 in a
 *   16-aligned raw allocation. Guest-aligned accesses through the window
 *   therefore map to host addresses that are only 4-byte aligned; that is
 *   fine because every guest access goes through the memcpy-based
 *   rc_readN/rc_writeN helpers (recomp_ops.h).
 *
 * Documented deviations:
 *   - The window's micro1 region (+0x8000..+0xBFFF) is plain memory, NOT
 *     the micro shadow used for hashing: VU1 microcode is uploaded via
 *     VIF1 MPG in this game, and its top 604 bytes alias the Vu1State
 *     header. Direct EE stores into 0x11008000+ would bypass hashing (and
 *     the top 604 bytes would clobber VU1 registers); the boot log shows
 *     no such access. micro0/data0 (+0x0..+0x7FFF) are plain flat memory
 *     as in P1.
 */
#include "hw.h"

#include "../ee/kernel.h"
#include "../prof.h"
#include "recomp_ops.h"

#include <algorithm>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../host/portable.h"

#ifdef ICORECOMP_HAVE_VU1_GENERATED
extern "C" void rt_vu1_register_all(void);
#endif

namespace {

constexpr uint32_t kVuDataBytes = 16384;
constexpr uint32_t kVuDataQw = kVuDataBytes / 16;
constexpr uint32_t kWindowSkew = 12; /* (16 - offsetof(Vu1State, mem) % 16) % 16 */

uint8_t* g_window = nullptr;  /* 64 KB guest-visible window base */
Vu1State* g_vu1 = nullptr;
alignas(16) uint8_t g_micro[kVuDataBytes];

/* Upload tracking: byte length of the current upload (MPG concatenation
 * from address 0) and whether the bound program needs re-resolving. */
uint32_t g_upload_len = 0;
bool g_upload_dirty = false;

struct Prog {
    uint32_t hash = 0;
    uint32_t size = 0;
    void (*entry)(Vu1State*) = nullptr;
    uint64_t calls = 0;
    uint64_t binds = 0;
    /* Profiler window: calls and exclusive microprogram time since the last
     * summary. The "vu1" bucket alone cannot say whether a rise is one
     * program growing or all five, and with five programs sharing one
     * average the two look identical. Cleared by rt_vu1_prof_report. */
    uint64_t win_calls = 0;
    uint64_t win_ns = 0;
};
std::vector<Prog> g_progs;
Prog* g_bound = nullptr;
uint32_t g_bound_hash = 0;
uint32_t g_entry_pc = 0; /* pc of the MSCAL currently executing */

uint64_t g_mscal_misses = 0;
uint64_t g_xgkicks = 0;

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

/* ---- VU1 state capture (ICORECOMP_VU1_CAPTURE=path) --------------------- */

/* Record: a fixed header then the whole Vu1State. The reader keys on
 * (hash, pc) and needs nothing else; sizeof(Vu1State) is pinned by the ABI
 * header, so a mismatched build is caught by the size field. */
struct CaptureHeader {
    uint32_t magic;   /* 'V','U','1','C' */
    uint32_t version;
    uint32_t state_size;
    uint32_t reserved;
};
constexpr uint32_t kCaptureMagic = 0x43315556u; /* "VU1C" little-endian */

std::FILE* g_capture = nullptr;
/* Per (hash, entry pc) sample budget, so a long run does not fill a disk. */
std::map<std::pair<uint32_t, uint32_t>, uint32_t> g_capture_counts;
uint32_t g_capture_per_site = 8;

void capture_open() {
    /* Opt in: ICORECOMP_VU1_CAPTURE=path records the real states the VU1
     * differential gate replays. Bounded at g_capture_per_site states per
     * (program, entry). */
    const char* path = std::getenv("ICORECOMP_VU1_CAPTURE");
    if (!path || !*path) return;
    if (const char* n = std::getenv("ICORECOMP_VU1_CAPTURE_PER_SITE")) {
        uint32_t v = (uint32_t)std::strtoul(n, nullptr, 10);
        if (v) g_capture_per_site = v;
    }
    g_capture = rt_fopen_utf8(path, "wb");
    if (!g_capture) {
        rt_log("vu1", "VU1 capture: could not open '%s'; capture disabled", path);
        return;
    }
    CaptureHeader h{kCaptureMagic, 1, (uint32_t)sizeof(Vu1State), 0};
    std::fwrite(&h, sizeof h, 1, g_capture);
    rt_log("vu1", "VU1 capture: writing up to %u states per (program, entry) to %s",
        g_capture_per_site, path);
}

void capture_state(const Vu1State* vu, uint32_t hash) {
    auto key = std::make_pair(hash, vu->pc);
    uint32_t& n = g_capture_counts[key];
    if (n >= g_capture_per_site) return;
    ++n;
    std::fwrite(&hash, sizeof hash, 1, g_capture);
    std::fwrite(vu, sizeof(Vu1State), 1, g_capture);
    std::fflush(g_capture);
}

} // namespace

uint8_t* rt_vu1_window_page() {
    if (!g_window) {
        /* Room for the 64 KB window at raw+12 plus the Vu1State tail
         * padding (4 bytes past the window end). */
        const size_t raw_size = 0x10020;
        void* raw = rt_aligned_zalloc(16, raw_size);
        if (!raw) rt_fatal("vu1", nullptr, "rt_aligned_zalloc(%zu) for VU window failed", raw_size);
        g_window = static_cast<uint8_t*>(raw) + kWindowSkew;
        uint8_t* state_base = g_window + 0xC000 - offsetof(Vu1State, mem);
        if ((reinterpret_cast<uintptr_t>(state_base) & 15) != 0) {
            rt_fatal("vu1", nullptr, "Vu1State overlay misaligned (offsetof mem=%zu)", offsetof(Vu1State, mem));
        }
        g_vu1 = reinterpret_cast<Vu1State*>(state_base);
        /* vf[0] invariant (0,0,0,1); vi[0] == 0 by the memset. */
        g_vu1->vf[0].f32x[3] = 1.0f;
    }
    return g_window;
}

Vu1State* rt_vu1_state() {
    rt_vu1_window_page();
    return g_vu1;
}

uint8_t* rt_vu1_micro() { return g_micro; }

void rt_vu1_init() {
    capture_open();
#ifdef ICORECOMP_HAVE_VU1_GENERATED
    rt_vu1_register_all();
    rt_log("vu1", "generated VU1 code linked: rt_vu1_register_all() registered %zu programs", g_progs.size());
#else
    rt_log("vu1", "no generated VU1 code linked (generated/vu1 missing at configure time); MSCAL will loud-skip");
#endif
}

void rt_vu1_micro_written(uint32_t offset, uint32_t bytes) {
    if (offset == 0) {
        g_upload_len = bytes; /* MPG to address 0 starts a new upload */
    } else if (offset == g_upload_len) {
        g_upload_len = offset + bytes;
    } else {
        /* Out-of-order segment: keep the covering extent, loudly. */
        rt_log("vu1", "MPG segment at 0x%x does not continue the upload (len was 0x%x); extending extent",
            offset, g_upload_len);
        if (offset + bytes > g_upload_len) g_upload_len = offset + bytes;
    }
    g_upload_dirty = true;
}

extern "C" void rt_vu1_register(uint32_t hash, uint32_t size_bytes, void (*entry)(Vu1State* vu)) {
    Prog p;
    p.hash = hash;
    p.size = size_bytes;
    p.entry = entry;
    g_progs.push_back(p);
    g_bound = nullptr; /* vector may reallocate; rebind on next MSCAL */
    g_upload_dirty = true;
    rt_log("vu1", "registered microprogram hash=0x%08x size=%u entry=%p (registry now %zu)",
        hash, size_bytes, (void*)entry, g_progs.size());
}

void rt_vu1_mscal(uint32_t pc_bytes, uint32_t xtop, uint32_t itop, const char* how) {
    Vu1State* vu = rt_vu1_state();
    const bool is_mscnt = (std::strcmp(how, "MSCNT") == 0);
    if (!is_mscnt) vu->pc = pc_bytes & (kVuDataBytes - 1);
    /* Set on every entry, MSCNT included: a continuation resumes for the
     * next batch and needs the buffer VIF1 just latched (see vif1.cpp). */
    vu->xtop = xtop;
    vu->itop = itop;

    if (g_upload_dirty) {
        g_bound = nullptr;
        g_bound_hash = rc_vu1_hash(g_micro, g_upload_len);
        for (Prog& p : g_progs) {
            if (p.hash == g_bound_hash) {
                g_bound = &p;
                if (p.size != g_upload_len) {
                    rt_log("vu1", "upload of %u bytes matched hash 0x%08x but registry says %u bytes",
                        g_upload_len, p.hash, p.size);
                }
                /* The first bind of each program is the useful line: it
                 * says the upload resolved and to what. After that the game
                 * rebinds the same five programs every field, so sample. */
                ++p.binds;
                if (rt_trace() || is_pow2(p.binds)) {
                    rt_log("vu1", "upload of %u bytes bound to program hash=0x%08x entry=%p [bind #%" PRIu64 "]",
                        g_upload_len, p.hash, (void*)p.entry, p.binds);
                }
                break;
            }
        }
        g_upload_dirty = false;
        if (!g_bound) {
            rt_log("vu1", "upload of %u bytes has hash 0x%08x: NO registered program (registry has %zu); MSCALs will skip",
                g_upload_len, g_bound_hash, g_progs.size());
        }
    }

    if (!g_bound) {
        ++g_mscal_misses;
        if (is_pow2(g_mscal_misses)) {
            rt_log("vu1", "%s pc=0x%x xtop=0x%x itop=0x%x: unbound upload (hash 0x%08x, %u bytes); SKIPPING [miss #%" PRIu64 "]",
                how, vu->pc, xtop, itop, g_bound_hash, g_upload_len, g_mscal_misses);
        }
        return;
    }

    /* Seed capture for the VU1 differential harness. Writes the exact
     * Vu1State the game hands each microprogram, which is the one input
     * synthetic seeding cannot reproduce: these programs are stateful
     * across MSCAL, so the deep geometry paths are only reachable from a
     * state a previous call and a VIF1 unpack built up. Sampled per
     * (program, entry) pair so the file stays small. The records are
     * ROM-derived; write them outside the repository. */
    if (g_capture) capture_state(vu, g_bound->hash);

    ++g_bound->calls;
    ++g_bound->win_calls;
    if (rt_trace() || is_pow2(g_bound->calls)) {
        rt_log("vu1", "%s pc=0x%x xtop=0x%x itop=0x%x -> program hash=0x%08x [call #%" PRIu64 "]",
            how, vu->pc, xtop, itop, g_bound->hash, g_bound->calls);
    }
    /* The entry pc this call was dispatched at, for the geometry checker:
     * these programs have ten entry points doing quite different work, and
     * "normal_c emits bad vertices" is only actionable once it says which
     * entry. Set before the call because vu->pc moves during it. */
    g_entry_pc = vu->pc;
    {
        /* Recompiled microprogram. rt_xgkick calls out of here open
         * their own zones, so "vu1" is microprogram code only. */
        const uint64_t before = rt_prof_zone_ns(RT_PROF_VU1);
        {
            RT_PROF_ZONE(RT_PROF_VU1);
            g_bound->entry(vu);
        }
        /* The "vu1" bucket is exclusive time, so its increase across that
         * scope is this call's own cost with the XGKICKs already
         * subtracted. Reusing it costs two loads rather than a second
         * clock reading on the hot path, and cannot disagree with the
         * bucket it decomposes. Both reads are 0 with profiling off. */
        g_bound->win_ns += rt_prof_zone_ns(RT_PROF_VU1) - before;
    }
}

/* One line per microprogram that ran this window, largest first, under the
 * "vu1" bucket it decomposes. Hashes are not named here: the names live in
 * the decomp and naming them would copy its symbols into this repo. Map a
 * hash to a name with the translator's own report, `recomp-cli vu1`. */
extern "C" void rt_vu1_prof_report(double fields) {
    /* Clear the window counters on every path, including this one: prof.h
     * states that as the contract for these hooks, and carrying them into
     * the next window would double-report the time. */
    if (fields <= 0.0) {
        for (Prog& p : g_progs) { p.win_calls = 0; p.win_ns = 0; }
        return;
    }
    std::vector<const Prog*> ran;
    for (const Prog& p : g_progs) {
        if (p.win_calls) ran.push_back(&p);
    }
    if (!ran.empty()) {
        std::sort(ran.begin(), ran.end(),
                  [](const Prog* a, const Prog* b) { return a->win_ns > b->win_ns; });
        for (const Prog* p : ran) {
            const double ms = (double)p->win_ns / 1e6;
            rt_log("prof", "    vu1 0x%08x %8.1f ms %7.3f ms/field  n=%-9llu mean %8.2f us",
                p->hash, ms, ms / fields, (unsigned long long)p->win_calls,
                (double)p->win_ns / 1e3 / (double)p->win_calls);
        }
    }
    for (Prog& p : g_progs) {
        p.win_calls = 0;
        p.win_ns = 0;
    }
}

uint32_t rt_vu1_bound_hash() { return g_bound_hash; }

uint32_t rt_vu1_entry_pc() { return g_entry_pc; }



extern "C" void rt_xgkick(Vu1State* vu, uint32_t qw_addr) {
    RT_PROF_ZONE(RT_PROF_GIF);
    ++g_xgkicks;
    static const bool geom = rt_verbose("geom");
    if (geom) rt_geom_note_clip(vu->clip, g_bound_hash);
    uint32_t addr = qw_addr & (kVuDataQw - 1);

    /* Walk GIF tags to find the packet end (EOP), handling the 16 KB wrap.
     * Collect into a scratch buffer so the backend sees one contiguous
     * packet. */
    static std::vector<uint8_t> buf;
    buf.clear();
    bool eop = false;
    for (uint32_t guard = 0; !eop && guard < kVuDataQw; ++guard) {
        const uint8_t* tag = vu->mem + (size_t)addr * 16;
        uint64_t lo;
        std::memcpy(&lo, tag, 8);
        uint32_t nloop = (uint32_t)(lo & 0x7FFF);
        eop = (lo >> 15) & 1;
        uint32_t flg = (uint32_t)((lo >> 58) & 3);
        uint32_t nreg = (uint32_t)((lo >> 60) & 15);
        if (nreg == 0) nreg = 16;
        uint32_t len; /* qwords including the tag */
        switch (flg) {
            case 0: len = 1 + nloop * nreg; break;                 /* PACKED */
            case 1: len = 1 + (nloop * nreg + 1) / 2; break;       /* REGLIST */
            default: len = 1 + nloop; break;                       /* IMAGE */
        }
        if (len > kVuDataQw) {
            rt_log("vu1", "xgkick: malformed GIF tag at qw 0x%x (len %u qw), truncating packet", addr, len);
            break;
        }
        for (uint32_t q = 0; q < len; ++q) {
            const uint8_t* src = vu->mem + (size_t)((addr + q) & (kVuDataQw - 1)) * 16;
            buf.insert(buf.end(), src, src + 16);
        }
        addr = (addr + len) & (kVuDataQw - 1);
    }
    if (!eop && is_pow2(g_xgkicks)) {
        rt_log("vu1", "xgkick #%" PRIu64 ": no EOP within VU1 data memory, packet dropped", g_xgkicks);
        return;
    }
    if (is_pow2(g_xgkicks)) {
        rt_log("vu1", "xgkick #%" PRIu64 ": qw_addr=0x%x -> PATH1 packet of %zu qw",
            g_xgkicks, qw_addr, buf.size() / 16);
    }
    if (!buf.empty()) rt_gif_submit(0, buf.data(), (uint32_t)(buf.size() / 16));
}
