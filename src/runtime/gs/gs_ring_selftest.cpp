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
 * The present pump has its own case at the end (run_present_pump_case). It
 * is not a recorded call: the ring generates it, once after each vsync
 * record and again from the consumer's park while a repeat interval
 * expires, so it has no counterpart on the oracle and is counted rather than
 * logged. What is checked is that display.present_rate 0 produces exactly
 * one pump per vsync and never a repeat, that a rate produces repeats while
 * the ring sits empty, and that the shortened park still wakes on a record.
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

#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>
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

/* The runtime's four level entry points, all onto one line here: a
 * selftest has one reader and no level to filter by. */
void rt_log_line(const char* component, const char* fmt, va_list ap);

void rt_log_error(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_warn(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_info(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_debug(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}

void rt_log_line(const char* component, const char* fmt, va_list ap) {
    ++g_log_lines;
    if (!g_log_verbose) return;
    std::printf("[%s] ", component);
    std::vprintf(fmt, ap);
    std::printf("\n");
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

/* The run-state notes the ring pushes for the end-of-run summary and the
 * field watchdog (host/run_state.cpp). Stubbed rather than linked: this
 * harness has no run to summarise, and linking run_state.cpp would drag in
 * the log sink this file replaces. Counted so the ring's own checks can see
 * the pushes happened at all. */
uint64_t g_run_notes = 0;
void rt_run_note_gs_record(const char*) { ++g_run_notes; }
void rt_run_note_gs_worker(const char*) { ++g_run_notes; }
void rt_run_note_gs_queued(uint64_t, uint64_t) { ++g_run_notes; }
void rt_run_set_exit_reason(bool, const char*, ...) {}
void rt_run_summary() {}
void rt_thread_set_name(const char*) {}
void rt_crash_reserve_stack() {}

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
    kTagSetWideAspect,
    kTagRequestShot,
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

    /* The bits, not the double: the ring carries this value through a
     * memcpy to a uint64_t record argument and back, and comparing the raw
     * bytes is what makes a lost or reinterpreted bit visible. */
    void set_widescreen_aspect(double aspect) override {
        std::string& e = open(kTagSetWideAspect);
        uint64_t bits = 0;
        std::memcpy(&bits, &aspect, sizeof(bits));
        append(e, bits);
    }

    void request_screenshot(uint32_t slots) override {
        std::string& e = open(kTagRequestShot);
        append(e, slots);
        m_armed_slots = slots;
    }

    /* Straight through, never over the ring (gs_threaded.h): the wrapper
     * calls this on the inner backend from the producer's own thread. A
     * deterministic answer per slot lets the test check the pass-through
     * without the two backends sharing state. */
    size_t take_screenshot(uint32_t slot, uint32_t* w, uint32_t* h, uint8_t* dst,
                           size_t dst_bytes) override {
        if (slot >= RT_PGS_SHOT_SLOTS) return 0;
        const uint32_t width = 4u + slot, height = 2u;
        const size_t need = size_t(width) * height * 4u;
        if (w) *w = width;
        if (h) *h = height;
        if (!dst) return need;
        if (dst_bytes < need) return 0;
        for (size_t i = 0; i < need; ++i) dst[i] = uint8_t(i + slot);
        return need;
    }

    uint32_t armed_slots() const { return m_armed_slots; }

    /* Counted, not logged. Every other method here records a call the test
     * made; this one is made by the ring itself, so logging it would put an
     * entry in the wrapped log that the oracle can never have. The counts
     * are read after quiesce() has joined the worker, which is what makes
     * reading these plain members from the test thread safe. */
    void present_pump(double max_hz) override {
        ++m_pumps;
        m_last_rate = max_hz;
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
    uint64_t pumps() const { return m_pumps; }
    double last_rate() const { return m_last_rate; }
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
    uint64_t m_pumps = 0;
    double m_last_rate = -1.0;
    uint32_t m_armed_slots = 0;
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
    kOpSetWideAspect,
    kOpSetPresentRate,
    kOpOverlaySetFrame,
    kOpOverlayClearFrame,
    kOpOverlayTexCreate,
    kOpOverlayTexDestroy,
    kOpPresentUi,
    kOpRequestShot,
    kOpCount,
};

/* One op per ThreadedBackend method that writes a record, in the same order
 * as gs_threaded.cpp's RecordKind. The count is asserted against the name
 * table below; the two lists are kept beside each other so a new record kind
 * that arrives with no op here is visible in one screen. Wrap has no
 * producer method (the encoder writes it), and read_priv and take_screenshot
 * are answered without a record but have their own cases in the sequence. */

const char* const kOpNames[] = {
    "gif", "gif-large", "priv", "read-priv", "vsync", "set-presentation",
    "set-present-mode", "set-render-scale", "set-raster", "set-deinterlace",
    "set-wide-aspect", "set-present-rate", "overlay-set-frame",
    "overlay-clear-frame", "overlay-tex-create", "overlay-tex-destroy",
    "present-ui", "request-screenshot",
};
static_assert(std::size(kOpNames) == size_t(kOpCount),
              "one name per Op, in Op order");

/* Weights: GIF packets dominate a real run, so they dominate here too. */
const uint32_t kOpWeights[] = { 40, 3, 20, 10, 6, 2, 2, 2, 2, 2, 2, 2, 6, 2, 2, 3, 2, 2 };
static_assert(std::size(kOpWeights) == size_t(kOpCount), "one weight per Op, in Op order");

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
    uint64_t vsyncs_enqueued = 0;

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
            ++vsyncs_enqueued;
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
        case kOpSetWideAspect: {
            /* Ordinary aspects and the two the derivation can produce at the
             * ends, so the double survives the record's bit round trip for
             * values that are not exactly representable in a few bits. */
            static const double kAspects[] = { 4.0 / 3.0, 16.0 / 9.0, 1.0, 2.3518, 0.0 };
            const double aspect = kAspects[next_u32(5)];
            oracle.set_widescreen_aspect(aspect);
            ring.set_widescreen_aspect(aspect);
            break;
        }
        case kOpRequestShot: {
            /* 1 or 2, the only two the host arms. The pixels never ride the
             * ring: take_screenshot below is the straight-through read, and
             * it must answer from this thread whatever the consumer is
             * doing. */
            const uint32_t slots = 1u + next_u32(2);
            oracle.request_screenshot(slots);
            ring.request_screenshot(slots);
            uint32_t w = 0, h = 0;
            const size_t need = ring.take_screenshot(RT_PGS_SHOT_PRE, &w, &h, nullptr, 0);
            std::vector<uint8_t> px(need, 0);
            const size_t got = ring.take_screenshot(RT_PGS_SHOT_PRE, &w, &h, px.data(), px.size());
            if (need == 0 || got != need || w == 0 || h == 0) {
                std::printf("take_screenshot passed through as %zu/%zu bytes, %ux%u\n",
                            got, need, w, h);
                return false;
            }
            break;
        }
        case kOpSetPresentRate:
            /* The ring keeps this one to itself: it does not forward the
             * rate to the inner backend, it hands it back as the argument of
             * every present_pump. So there is nothing to record on the
             * oracle, and what this op is here for is that a record kind
             * with no inner call still leaves the order of everything around
             * it exactly as it was. Always 0, so no repeat can fire and the
             * pump count stays one per vsync; the rate that does repeat has
             * its own case below. */
            ring.set_present_rate(0.0);
            break;
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
    /* Every vsync record pumps the present once, and with the rate at 0
     * nothing else does: no repeat may fire from the consumer's park. */
    if (wrapped->pumps() != vsyncs_enqueued) {
        std::printf("%s: %" PRIu64 " present pumps for %" PRIu64 " vsyncs"
                    " (display.present_rate 0 must never repeat)\n",
                    cfg.name, wrapped->pumps(), vsyncs_enqueued);
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

/* The present pump on its own. Two questions, both about the consumer's park
 * loop, neither reachable from the randomised sequence above (which never
 * leaves the ring empty for long enough).
 *
 *   1. display.present_rate 0 must produce exactly one pump per vsync, even
 *      with the worker sitting on an empty ring for many repeat intervals.
 *   2. A rate must produce repeats while the ring is empty, and the park
 *      whose wait was shortened for those repeats must still wake on a
 *      record.
 *
 * Every count is read after quiesce() has joined the worker; nothing here
 * reads the inner backend's state while it is running. */
bool run_present_pump_case() {
    { /* 1: no rate, no repeats */
        RecordingBackend* inner = new RecordingBackend(); /* owned by the ring */
        ThreadedBackend ring(inner, 64u * 1024, true);
        ring.start_worker();
        ring.set_present_rate(0.0);
        for (unsigned i = 0; i < 4; ++i) {
            ring.vsync(i & 1u);
            ring.field_sync();
        }
        /* Far longer than any repeat interval this build could pick. */
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        ring.quiesce();
        if (inner->pumps() != 4) {
            std::printf("present-pump: rate 0 pumped %" PRIu64 " times for 4 vsyncs\n",
                        inner->pumps());
            return false;
        }
        if (inner->last_rate() != 0.0) {
            std::printf("present-pump: rate 0 reached the backend as %g\n", inner->last_rate());
            return false;
        }
    }
    { /* 2: a rate repeats, and the park still wakes on a record */
        RecordingBackend* inner = new RecordingBackend();
        ThreadedBackend ring(inner, 64u * 1024, true);
        ring.start_worker();
        /* 500 Hz, a 2 ms interval: short enough that 60 ms of an empty ring
         * is unambiguous and long enough not to spin a core on a loaded
         * build machine. */
        ring.set_present_rate(500.0);
        ring.vsync(0);
        ring.field_sync();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));

        /* The park's wait is now bounded by what is left of the repeat
         * interval rather than by the ring's own 2 ms step. A record
         * committed into that park has to be replayed anyway. */
        const uint64_t replayed_before = ring.records_replayed();
        ring.write_priv(0x40, 0x1234u);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (ring.records_replayed() <= replayed_before) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::printf("present-pump: the parked consumer did not replay a record"
                            " committed during a repeat wait\n");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ring.quiesce();
        /* One pump for the vsync plus the repeats. 60 ms at 500 Hz is about
         * 30; the bound is deliberately loose, since what is under test is
         * that repeats happen at all and not the scheduler's accuracy. */
        if (inner->pumps() < 6) {
            std::printf("present-pump: 500 Hz over 60 ms of an empty ring pumped only %"
                        PRIu64 " times\n", inner->pumps());
            return false;
        }
        if (inner->last_rate() != 500.0) {
            std::printf("present-pump: the rate reached the backend as %g, expected 500\n",
                        inner->last_rate());
            return false;
        }
        std::printf("  %-22s rate 0: 1 pump per vsync; rate 500 Hz: %" PRIu64
                    " pumps over 60 ms of an empty ring, park woke on a record\n",
                    "present-pump", inner->pumps());
    }
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

    if (!run_present_pump_case()) {
        std::printf("FAILED in the present-pump case\n");
        return 1;
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
