/* gs/gs_ring_selftest.cpp: round-trip check of the GS command ring
 * (icorecomp-gsring-selftest).
 *
 * gs_threaded.cpp's ThreadedBackend has to be transparent: every GsBackend
 * call made on it must reach the inner backend with exactly the same
 * arguments, in exactly the same order, no matter where the records land in
 * the ring. That property is what makes "an ICORECOMP_GS=dump run writes
 * byte-identical output with and without the wrapper" true, and the dump run
 * itself cannot be used to check it here (it needs a disc, a GS and a
 * display).
 *
 * So: two RecordingBackends. One is called directly and is the oracle. The
 * other is wrapped in a ThreadedBackend and driven with the identical
 * randomised call sequence. Both record every call into a byte blob,
 * including the full packet payload and the full overlay geometry, and the
 * two blob lists must match element for element at the end. Anything the
 * encoder drops, truncates, misaligns or reorders shows up as a mismatch.
 *
 * The interesting cases are the ones a normal run reaches rarely:
 *   - a record that does not fit before the end of the buffer, so a wrap
 *     marker has to be written and the record restarted at offset 0;
 *   - several records live in the ring at once, which only happens with
 *     inline draining turned off (with it on, the ring holds one record at a
 *     time by construction);
 *   - back pressure: a ring small enough that the producer runs into a full
 *     buffer and has to drain to make room;
 *   - a single record close to the whole ring in size.
 * The configurations below cover all four by shrinking the ring far below
 * the 32 MB the runtime uses.
 *
 * The privileged-register shadow is checked at the same time: the inner
 * backend's read_priv returns a poison value, so any read the wrapper
 * forwards instead of answering itself is caught immediately.
 *
 * Exit code 0 = every configuration matched.
 *
 * Usage: icorecomp-gsring-selftest [calls-per-config] [seed]
 */
#include "gs_threaded.h"

#include "gs_parallel_api.h"
#include "runtime.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* ---- stubs for the runtime services gs_threaded.cpp uses ---------------- */

namespace {
bool g_log_verbose = false;
uint64_t g_log_lines = 0;
/* The ring pumps the host's event loop from every producer-side wait; there
 * is no window here, so the stub below only counts the calls, and the total
 * shows that the waits in the worker configurations actually ran. */
uint64_t g_pumps = 0;
/* Event dispatch must be held for every one of those pumps: one of the waits
 * that reaches it runs from inside the UI's own renderer. */
int g_dispatch_held = 0;
uint64_t g_unheld_pumps = 0;
} // namespace

void rt_log(const char* component, const char* fmt, ...) {
    ++g_log_lines;
    if (!g_log_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    std::printf("[%s] ", component);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}

/* The ring holds event dispatch across every wait that pumps; there is no
 * SDL here, so the stub only checks that the hold is balanced and that no
 * pump ever runs unheld from a wait. */
void rt_window_hold_event_dispatch(bool on) {
    g_dispatch_held += on ? 1 : -1;
    if (g_dispatch_held < 0) {
        std::printf("rt_window_hold_event_dispatch released more times than held\n");
        std::exit(2);
    }
}

void rt_window_pump() {
    ++g_pumps;
    if (g_dispatch_held == 0) ++g_unheld_pumps;
}

/* Only reached by the ring's abandon path (a worker that will not stop),
 * which no configuration here provokes. */
void rt_log_drain() {}

/* No fatal is in progress in this harness. */
int rt_fatal_exit_code() { return -1; }

void rt_fatal(const char* component, const R5900Context*, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("[%s] FATAL: ", component);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    std::exit(2);
}

namespace {

/* ---- deterministic randomness ------------------------------------------ */

uint64_t g_rng = 0;

uint64_t next_u64() {
    /* xorshift64*, enough for shuffling test cases and nothing else. */
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return g_rng * 2685821657736338717ull;
}

uint32_t next_u32(uint32_t bound) { return bound ? uint32_t(next_u64() % bound) : 0; }

void fill_random(void* dst, size_t bytes) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t i = 0;
    while (i + 8 <= bytes) {
        const uint64_t v = next_u64();
        std::memcpy(p + i, &v, 8);
        i += 8;
    }
    while (i < bytes) {
        p[i++] = uint8_t(next_u64());
    }
}

/* ---- the oracle --------------------------------------------------------- */

/* Every call is appended as a self-describing blob: a one-byte tag followed
 * by the raw arguments. Comparing blobs compares every byte of every
 * argument, which is the point (a field the encoder silently drops would
 * still compare equal if this recorded a summary). */
enum Tag : uint8_t {
    kTagGif = 1,
    kTagPriv,
    kTagVsync,
    kTagSetPresentation,
    kTagSetPresentMode,
    kTagSetRenderScale,
    kTagSetRaster,
    kTagSetDeinterlace,
    kTagOverlaySetFrame,
    kTagOverlayTexCreate,
    kTagOverlayTexDestroy,
    kTagPresentUi,
};

/* The poison read_priv returns. ThreadedBackend must never forward a
 * privileged read, so this value must never come back out of the wrapper. */
constexpr uint64_t kPrivPoison = 0xBADC0FFEE0DDF00Dull;

class RecordingBackend final : public GsBackend {
public:
    void submit_gif(int path, const uint8_t* data, uint32_t qwords) override {
        std::string& e = open(kTagGif);
        append(e, int32_t(path));
        append(e, qwords);
        e.append(reinterpret_cast<const char*>(data), size_t(qwords) * 16u);
        /* paraLLEl-GS's gif_transfer reads the buffer as 128-bit qwords, so
         * the wrapper owes the inner backend 16-byte alignment. Only checked
         * on the wrapped recorder: the oracle is handed a std::vector's
         * storage, whose alignment is the allocator's business. */
        if (m_check_alignment && (reinterpret_cast<uintptr_t>(data) & 15u) != 0) {
            ++m_alignment_faults;
        }
    }

    void write_priv(uint32_t offset, uint64_t v) override {
        std::string& e = open(kTagPriv);
        append(e, offset);
        append(e, v);
    }

    uint64_t read_priv(uint32_t) override {
        ++m_priv_reads;
        return kPrivPoison;
    }

    bool vsync(unsigned field) override {
        std::string& e = open(kTagVsync);
        append(e, uint32_t(field));
        /* Deterministic answer so the wrapper's return value can be checked
         * against the oracle's without the two backends sharing state. */
        return (++m_vsyncs & 3u) != 0;
    }

    void set_presentation(uint32_t fit, uint32_t filter) override {
        std::string& e = open(kTagSetPresentation);
        append(e, fit);
        append(e, filter);
    }

    void set_present_mode(uint32_t mode) override {
        std::string& e = open(kTagSetPresentMode);
        append(e, mode);
    }

    void set_render_scale(uint32_t factor) override {
        std::string& e = open(kTagSetRenderScale);
        append(e, factor);
    }

    void set_raster(uint32_t raster) override {
        std::string& e = open(kTagSetRaster);
        append(e, raster);
    }

    void set_deinterlace(uint32_t deinterlace) override {
        std::string& e = open(kTagSetDeinterlace);
        append(e, deinterlace);
    }

    uint32_t overlay_texture_create(const uint8_t* rgba8, uint32_t width,
                                    uint32_t height) override {
        std::string& e = open(kTagOverlayTexCreate);
        append(e, width);
        append(e, height);
        e.append(reinterpret_cast<const char*>(rgba8), size_t(width) * height * 4u);
        return ++m_textures;
    }

    void overlay_texture_destroy(uint32_t texture) override {
        std::string& e = open(kTagOverlayTexDestroy);
        append(e, texture);
    }

    void overlay_set_frame(const RtPgsOverlayFrame* frame) override {
        std::string& e = open(kTagOverlaySetFrame);
        append(e, uint32_t(frame ? 1u : 0u));
        if (!frame) return;
        append(e, frame->vertex_count);
        append(e, frame->index_count);
        append(e, frame->cmd_count);
        append(e, frame->surface_width);
        append(e, frame->surface_height);
        e.append(reinterpret_cast<const char*>(frame->vertices),
                 size_t(frame->vertex_count) * sizeof(RtPgsOverlayVertex));
        e.append(reinterpret_cast<const char*>(frame->indices),
                 size_t(frame->index_count) * sizeof(uint32_t));
        e.append(reinterpret_cast<const char*>(frame->cmds),
                 size_t(frame->cmd_count) * sizeof(RtPgsOverlayCmd));
    }

    uint32_t present_ui() override {
        open(kTagPresentUi);
        return 0x8000u | ++m_present_ui;
    }

    void check_alignment() { m_check_alignment = true; }

    const std::vector<std::string>& log() const { return m_log; }
    uint64_t priv_reads() const { return m_priv_reads; }
    uint64_t alignment_faults() const { return m_alignment_faults; }

private:
    std::string& open(Tag tag) {
        m_log.emplace_back(1, char(tag));
        return m_log.back();
    }

    template <typename T>
    static void append(std::string& e, T v) {
        e.append(reinterpret_cast<const char*>(&v), sizeof(v));
    }

    std::vector<std::string> m_log;
    bool m_check_alignment = false;
    uint64_t m_vsyncs = 0;
    uint32_t m_textures = 0;
    uint32_t m_present_ui = 0;
    uint64_t m_priv_reads = 0;
    uint64_t m_alignment_faults = 0;
};

/* ---- call generation ---------------------------------------------------- */

enum Op {
    kOpGif = 0,
    kOpBigGif,
    kOpPriv,
    kOpReadPriv,
    kOpVsync,
    kOpSetPresentation,
    kOpSetPresentMode,
    kOpSetRenderScale,
    kOpSetRaster,
    kOpSetDeinterlace,
    kOpOverlaySetFrame,
    kOpOverlayClearFrame,
    kOpOverlayTexCreate,
    kOpOverlayTexDestroy,
    kOpPresentUi,
    kOpCount,
};

const char* const kOpNames[kOpCount] = {
    "gif", "gif-large", "priv", "read-priv", "vsync", "set-presentation",
    "set-present-mode", "set-render-scale", "set-raster", "set-deinterlace",
    "overlay-set-frame", "overlay-clear-frame", "overlay-tex-create",
    "overlay-tex-destroy", "present-ui",
};

/* Weights: GIF packets dominate a real run, so they dominate here too. */
const uint32_t kOpWeights[kOpCount] = { 40, 3, 20, 10, 6, 2, 2, 2, 2, 2, 6, 2, 2, 3, 2 };

Op pick_op() {
    uint32_t total = 0;
    for (uint32_t w : kOpWeights) total += w;
    uint32_t r = next_u32(total);
    for (int i = 0; i < kOpCount; ++i) {
        if (r < kOpWeights[i]) return Op(i);
        r -= kOpWeights[i];
    }
    return kOpGif;
}

struct Config {
    const char* name;
    size_t ring_bytes;
    bool inline_drain;
    /* Hands the ring to its worker thread after construction (which requires
     * inline_drain, the mode the runtime starts in). The test thread is then
     * the producer and the worker is the consumer, which is the shape the
     * game runs in: the two reply-carrying calls really wait for a reply,
     * the vsync sync point really waits for a field, and a small ring really
     * blocks the producer until the worker frees room. */
    bool worker;
    uint32_t max_gif_qwords;   /* ordinary packets */
    uint32_t big_gif_qwords;   /* the near-ring-size case */
};

const char* mode_name(const Config& cfg) {
    if (cfg.worker) return "worker-thread";
    return cfg.inline_drain ? "inline-drain" : "batched-drain";
}

/* Scratch buffers, reused so the ring is the only thing under test. */
std::vector<uint8_t> g_packet;
std::vector<RtPgsOverlayVertex> g_verts;
std::vector<uint32_t> g_indices;
std::vector<RtPgsOverlayCmd> g_cmds;
std::vector<uint8_t> g_texels;

/* Model of the privileged-register shadow, indexed the way every backend
 * indexes it. */
uint64_t g_priv_model[0x2000 / 16] = {};

bool run_config(const Config& cfg, uint32_t calls, uint64_t seed, uint64_t op_counts[kOpCount]) {
    g_rng = seed;
    std::memset(g_priv_model, 0, sizeof(g_priv_model));

    RecordingBackend oracle;
    RecordingBackend* wrapped = new RecordingBackend(); /* owned by the wrapper */
    wrapped->check_alignment();
    ThreadedBackend ring(wrapped, cfg.ring_bytes, cfg.inline_drain);
    if (cfg.worker) ring.start_worker();

    uint64_t priv_reads_checked = 0;
    uint64_t vsync_returns_checked = 0;

    for (uint32_t i = 0; i < calls; ++i) {
        const Op op = pick_op();
        ++op_counts[op];
        switch (op) {
        case kOpGif:
        case kOpBigGif: {
            const uint32_t qwords = op == kOpBigGif
                                        ? 1u + next_u32(cfg.big_gif_qwords)
                                        : 1u + next_u32(cfg.max_gif_qwords);
            g_packet.resize(size_t(qwords) * 16u);
            fill_random(g_packet.data(), g_packet.size());
            const int path = int(next_u32(3));
            oracle.submit_gif(path, g_packet.data(), qwords);
            ring.submit_gif(path, g_packet.data(), qwords);
            break;
        }
        case kOpPriv: {
            const uint32_t offset = next_u32(0x2000);
            const uint64_t v = next_u64();
            g_priv_model[(offset & 0x1FFF) >> 4] = v;
            oracle.write_priv(offset, v);
            ring.write_priv(offset, v);
            break;
        }
        case kOpReadPriv: {
            const uint32_t offset = next_u32(0x2000);
            const uint64_t got = ring.read_priv(offset);
            const uint64_t want = g_priv_model[(offset & 0x1FFF) >> 4];
            if (got != want) {
                std::printf("read_priv(0x%04x) = 0x%016" PRIx64 ", expected 0x%016" PRIx64
                            "%s\n",
                            offset, got, want,
                            got == kPrivPoison ? " (the wrapper forwarded the read)" : "");
                return false;
            }
            ++priv_reads_checked;
            /* Not recorded on the oracle: a read is not a backend call. */
            break;
        }
        case kOpVsync: {
            const unsigned field = next_u32(2);
            const bool want = oracle.vsync(field);
            const bool got = ring.vsync(field);
            /* The runtime's field boundary: enqueue the vsync, then wait for
             * the one before it. A no-op outside worker mode. */
            ring.field_sync();
            /* The return value is only this field's answer when the record
             * was replayed inside the call. With the consumer on its own
             * thread it is the previous field's, by design (gs_threaded.h),
             * so there is nothing to compare. */
            const bool answered_here = cfg.inline_drain && !cfg.worker;
            if (answered_here && got != want) {
                std::printf("vsync(%u) returned %d, expected %d\n", field, int(got), int(want));
                return false;
            }
            if (answered_here) ++vsync_returns_checked;
            break;
        }
        case kOpSetPresentation: {
            const uint32_t fit = next_u32(3), filter = next_u32(2);
            oracle.set_presentation(fit, filter);
            ring.set_presentation(fit, filter);
            break;
        }
        case kOpSetPresentMode: {
            const uint32_t mode = next_u32(3);
            oracle.set_present_mode(mode);
            ring.set_present_mode(mode);
            break;
        }
        case kOpSetRenderScale: {
            static const uint32_t kFactors[] = { 1, 4, 8, 16 };
            const uint32_t f = kFactors[next_u32(4)];
            oracle.set_render_scale(f);
            ring.set_render_scale(f);
            break;
        }
        case kOpSetRaster: {
            /* RT_PGS_RASTER_* (gs_parallel_api.h), plus one value outside the
             * range: the wrapper passes what it was handed, it does not
             * validate. */
            const uint32_t raster = next_u32(3);
            oracle.set_raster(raster);
            ring.set_raster(raster);
            break;
        }
        case kOpSetDeinterlace: {
            /* RT_PGS_DEINTERLACE_*, same rule. */
            const uint32_t deinterlace = next_u32(4);
            oracle.set_deinterlace(deinterlace);
            ring.set_deinterlace(deinterlace);
            break;
        }
        case kOpOverlaySetFrame: {
            const uint32_t vcount = next_u32(64);
            const uint32_t icount = next_u32(96);
            const uint32_t ccount = next_u32(8);
            g_verts.resize(vcount);
            g_indices.resize(icount);
            g_cmds.resize(ccount);
            /* Random bytes rather than plausible geometry: neither backend
             * interprets the values here, and filling every byte means
             * nothing the encoder skips can hide in a padding hole. Both
             * structs are padding-free (all members are 4 bytes wide), so
             * the byte comparison downstream is well defined. */
            if (vcount) fill_random(g_verts.data(), vcount * sizeof(RtPgsOverlayVertex));
            if (icount) fill_random(g_indices.data(), icount * sizeof(uint32_t));
            if (ccount) fill_random(g_cmds.data(), ccount * sizeof(RtPgsOverlayCmd));
            RtPgsOverlayFrame frame = {};
            frame.vertices = vcount ? g_verts.data() : nullptr;
            frame.vertex_count = vcount;
            frame.indices = icount ? g_indices.data() : nullptr;
            frame.index_count = icount;
            frame.cmds = ccount ? g_cmds.data() : nullptr;
            frame.cmd_count = ccount;
            frame.surface_width = 1 + next_u32(4096);
            frame.surface_height = 1 + next_u32(4096);
            oracle.overlay_set_frame(&frame);
            ring.overlay_set_frame(&frame);
            break;
        }
        case kOpOverlayClearFrame:
            oracle.overlay_set_frame(nullptr);
            ring.overlay_set_frame(nullptr);
            break;
        case kOpOverlayTexCreate: {
            const uint32_t w = 1 + next_u32(32), h = 1 + next_u32(32);
            g_texels.resize(size_t(w) * h * 4u);
            fill_random(g_texels.data(), g_texels.size());
            const uint32_t want = oracle.overlay_texture_create(g_texels.data(), w, h);
            const uint32_t got = ring.overlay_texture_create(g_texels.data(), w, h);
            if (got != want) {
                std::printf("overlay_texture_create returned %u, expected %u\n", got, want);
                return false;
            }
            break;
        }
        case kOpOverlayTexDestroy: {
            const uint32_t id = next_u32(64);
            oracle.overlay_texture_destroy(id);
            ring.overlay_texture_destroy(id);
            break;
        }
        case kOpPresentUi: {
            const uint32_t want = oracle.present_ui();
            const uint32_t got = ring.present_ui();
            if (got != want) {
                std::printf("present_ui returned 0x%x, expected 0x%x\n", got, want);
                return false;
            }
            break;
        }
        case kOpCount:
            break;
        }
    }

    /* Worker mode: stop and join the consumer, which replays whatever is
     * still queued first. Otherwise this thread is the consumer and drains
     * it itself. */
    if (cfg.worker) {
        ring.quiesce();
    } else {
        ring.drain();
    }

    const std::vector<std::string>& want = oracle.log();
    const std::vector<std::string>& got = wrapped->log();
    if (want.size() != got.size()) {
        std::printf("%s: replayed %zu calls, expected %zu\n", cfg.name, got.size(), want.size());
        return false;
    }
    for (size_t i = 0; i < want.size(); ++i) {
        if (want[i] == got[i]) continue;
        std::printf("%s: call %zu differs (tag %u vs %u, %zu vs %zu bytes)\n", cfg.name, i,
                    unsigned(uint8_t(want[i][0])), unsigned(uint8_t(got[i][0])),
                    want[i].size(), got[i].size());
        const size_t n = want[i].size() < got[i].size() ? want[i].size() : got[i].size();
        for (size_t b = 0; b < n; ++b) {
            if (want[i][b] != got[i][b]) {
                std::printf("  first differing byte at %zu: 0x%02x vs 0x%02x\n", b,
                            unsigned(uint8_t(want[i][b])), unsigned(uint8_t(got[i][b])));
                break;
            }
        }
        return false;
    }
    if (wrapped->priv_reads() != 0) {
        std::printf("%s: the wrapper forwarded %" PRIu64 " privileged reads\n", cfg.name,
                    wrapped->priv_reads());
        return false;
    }
    if (wrapped->alignment_faults() != 0) {
        std::printf("%s: %" PRIu64 " misaligned packet payloads\n", cfg.name,
                    wrapped->alignment_faults());
        return false;
    }
    if (ring.records_written() != ring.records_replayed()) {
        std::printf("%s: %" PRIu64 " records written but %" PRIu64 " replayed\n", cfg.name,
                    ring.records_written(), ring.records_replayed());
        return false;
    }

    std::printf("  %-22s ring %7zu %-14s calls %6zu  records %6" PRIu64
                "  bytes %10" PRIu64 "  wraps %5" PRIu64 "  stalls %5" PRIu64
                "  priv-reads %" PRIu64 "  vsync-returns %" PRIu64 "\n",
                cfg.name, cfg.ring_bytes, mode_name(cfg), want.size(),
                ring.records_replayed(), ring.bytes_written(), ring.wrap_markers(),
                ring.back_pressure_stalls(), priv_reads_checked, vsync_returns_checked);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    uint32_t calls = 20000;
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    if (argc > 1) calls = uint32_t(std::strtoul(argv[1], nullptr, 0));
    if (argc > 2) seed = std::strtoull(argv[2], nullptr, 0);
    if (seed == 0) seed = 1;
    g_log_verbose = std::getenv("ICORECOMP_GSRING_SELFTEST_VERBOSE") != nullptr;

    /* Ring sizes far below the runtime's 32 MB on purpose: the wrap marker,
     * the multi-record case and back pressure are all rare at production
     * size and must still be right. The last configuration's packets reach
     * most of the way across its ring, which is the case where a wrap marker
     * is followed immediately by a record that only just fits. */
    const Config configs[] = {
        { "production-shape",  ThreadedBackend::kDefaultRingBytes, true,  false, 256,  4096 },
        { "small-inline",      64u * 1024,                         true,  false, 256,  1024 },
        { "small-batched",     64u * 1024,                         false, false, 64,   256  },
        { "tiny-batched",      8u * 1024,                          false, false, 16,   64   },
        { "near-full-records", 8u * 1024,                          true,  false, 200,  480  },
        /* The same sequences again with the consumer on its own thread. The
         * production shape is the one the game runs; the small ring is the
         * one that actually reaches back pressure, which at 32 MB would take
         * a scene the test does not have. */
        { "production-worker", ThreadedBackend::kDefaultRingBytes, true,  true,  256,  4096 },
        { "small-worker",      64u * 1024,                         true,  true,  256,  1024 },
        { "tiny-worker",       8u * 1024,                          true,  true,  16,   200  },
    };

    std::printf("GS command ring round trip: %u calls per configuration, seed 0x%016" PRIx64 "\n",
                calls, seed);

    uint64_t op_counts[kOpCount] = {};
    for (const Config& cfg : configs) {
        if (!run_config(cfg, calls, seed, op_counts)) {
            std::printf("FAILED in configuration %s\n", cfg.name);
            return 1;
        }
    }

    std::printf("calls by kind:");
    for (int i = 0; i < kOpCount; ++i) {
        std::printf(" %s=%" PRIu64, kOpNames[i], op_counts[i]);
    }
    std::printf("\n");
    if (g_unheld_pumps != 0 || g_dispatch_held != 0) {
        std::printf("FAILED: %" PRIu64 " pump(s) ran with event dispatch not held,"
                    " hold depth %d at exit\n", g_unheld_pumps, g_dispatch_held);
        return 1;
    }
    std::printf("all %u calls in %zu configurations replayed identically"
                " (%" PRIu64 " event-pump calls from producer waits, all with event"
                " dispatch held)\n", calls,
                sizeof(configs) / sizeof(configs[0]), g_pumps);
    if (!g_log_verbose) {
        std::printf("(%" PRIu64 " log lines suppressed; set "
                    "ICORECOMP_GSRING_SELFTEST_VERBOSE to see them)\n",
                    g_log_lines);
    }
    return 0;
}
