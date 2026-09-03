/* gs/gs_threaded.cpp: the GS command ring and ThreadedBackend, the wrapper
 * that encodes every GsBackend call into it.
 *
 * Why this exists
 * ---------------
 * Measured on the live backend, the EE thread spends about 5.0 ms per field
 * inside paraLLEl-GS packet parsing (gif_transfer) and another 5.8 ms in the
 * WSI present, on top of 13 ms of its own work, against a 16.68 ms field
 * budget. Both are pure producer-side stalls: nothing the EE does next
 * depends on either finishing. Moving them to a worker thread is the fix,
 * and a command ring is the mechanism.
 *
 * This file is the ring plus the wrapper, with the consumer still on the
 * producer's thread: every wrapper method commits its record and then calls
 * drain(), which replays the record into the inner backend immediately. The
 * program stays single threaded and every observable result, including the
 * bytes an ICORECOMP_GS=dump run writes, is identical to not having the
 * wrapper at all. What changes later is one line: commit() wakes a worker
 * instead of draining inline.
 *
 * Record format
 * -------------
 * One byte ring, capacity a power of two, allocated once. Head and tail are
 * monotonically increasing byte counters (std::atomic<uint64_t>, never
 * wrapped): the buffer offset is counter & (capacity - 1), head - tail is
 * the number of live bytes, head == tail means empty, and head - tail ==
 * capacity means full. One producer, one consumer, no locks.
 *
 * Every record starts with a 16-byte header:
 *
 *     u32 kind      Kind below
 *     u32 size      whole record in bytes, header included, multiple of 16
 *     u64 arg       kind-specific, see the table
 *
 * followed by `size - 16` bytes of payload. Records are 16-byte aligned and
 * the buffer is allocated 16-byte aligned, so a record never straddles the
 * end of the buffer: when the record being written does not fit in the bytes
 * left before the end, a Wrap record fills exactly those bytes and the real
 * record starts at offset 0. That also means every payload pointer handed to
 * the inner backend is 16-byte aligned, which is what paraLLEl-GS's
 * gif_transfer wants for its 128-bit qword reads.
 *
 *   Kind            arg                                    payload
 *   ----            ---                                    -------
 *   Wrap            0                                       padding to the end
 *   Gif             path | qwords << 32                     qwords * 16 packet bytes
 *   Priv            the 64-bit value written                PrivPayload (offset)
 *   Vsync           field | sequence << 32                  none
 *   Fence           sequence                                none
 *   SetPresentation fit | filter << 32                      none
 *   SetPresentMode  RT_PGS_PRESENT_* mode                   none
 *   SetRenderScale  super-sampling factor                   none
 *   SetRaster       RT_PGS_RASTER_* value                   none
 *   SetDeinterlace  RT_PGS_DEINTERLACE_* value              none
 *   OverlaySetFrame 0 = clear, 1 = frame follows            FramePayload + arrays
 *   OverlayTexDestroy texture id                            none
 *
 * OverlaySetFrame's payload is a FramePayload counts header followed by the
 * vertex, index and command arrays back to back, at the offsets
 * frame_layout() computes. frame_layout() is the single definition of that
 * layout: the encoder and the decoder both call it, so they cannot drift.
 *
 * Copy policy
 * -----------
 * Packet bytes are always copied into the ring, never referenced. Verified
 * at third_party/parallel-gs/gs/gs_interface.cpp:4294 (GSInterface::
 * gif_transfer): it walks the buffer and dispatches register writes and
 * image uploads inside the call and retains no pointer to it. Every caller
 * on our side hands over transient memory anyway (hw/gif.cpp passes a
 * pointer into guest RAM or into a VIF1 staging buffer, ui/ui_render.cpp
 * passes vectors it reuses next tick), so once the consumer runs on another
 * thread there is nothing safe to reference. Copying now is what makes the
 * ring's contents self-contained.
 *
 * No RT_PROF_ZONE anywhere in this file. prof.h's zones are single-threaded
 * by construction (see its header comment) and drain() is what moves to the
 * worker thread. The existing zone in hw/gif.cpp around submit_gif stays and
 * now measures the enqueue plus the inline drain, which is the same work it
 * measured before.
 */
#include "gs_threaded.h"

#include "gs_parallel_api.h"
#include "runtime.h"

#include <cstring>
#include <cstdlib>
#include <new>

namespace {

constexpr uint32_t kRecordAlign = 16;

/* Record kinds. Values are internal to this file (nothing persists a ring),
 * but they are fixed rather than reordered casually because the selftest
 * prints them by name. */
enum RecordKind : uint32_t {
    kWrap = 0,
    kGif,
    kPriv,
    kVsync,
    kFence,
    kSetPresentation,
    kSetPresentMode,
    kSetRenderScale,
    kSetRaster,
    kSetDeinterlace,
    kOverlaySetFrame,
    kOverlayTexDestroy,
    kKindCount,
};

const char* kind_name(uint32_t kind) {
    static const char* const kNames[kKindCount] = {
        "wrap", "gif", "priv", "vsync", "fence", "set-presentation",
        "set-present-mode", "set-render-scale", "set-raster", "set-deinterlace",
        "overlay-set-frame",
        "overlay-tex-destroy",
    };
    return kind < kKindCount ? kNames[kind] : "?";
}

struct RecHeader {
    uint32_t kind;
    uint32_t size;
    uint64_t arg;
};
static_assert(sizeof(RecHeader) == 16, "record header must be one 16-byte unit");

/* Priv payload: the register offset. The value rides in the header's arg
 * because it is the only 64-bit field. Padded so the record stays 16-byte
 * aligned without the encoder doing arithmetic. */
struct PrivPayload {
    uint32_t offset;
    uint32_t pad[3];
};
static_assert(sizeof(PrivPayload) == 16, "priv payload must stay 16 bytes");

/* Overlay frame payload header: the counts and the surface size the geometry
 * was laid out for. The three arrays follow at frame_layout()'s offsets. */
struct FramePayload {
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t cmd_count;
    uint32_t surface_width;
    uint32_t surface_height;
    uint32_t pad[3];
};
static_assert(sizeof(FramePayload) == 32, "frame payload header must stay 32 bytes");

/* The one definition of where the three overlay arrays sit inside an
 * OverlaySetFrame payload. Both the encoder and the decoder call it. All
 * three element types have alignment 4 and the payload base is 16-byte
 * aligned, so every offset below is correctly aligned by construction. */
struct FrameLayout {
    uint64_t vertices_off;
    uint64_t indices_off;
    uint64_t cmds_off;
    uint64_t total;
};

FrameLayout frame_layout(uint32_t vertex_count, uint32_t index_count, uint32_t cmd_count) {
    FrameLayout l = {};
    l.vertices_off = sizeof(FramePayload);
    l.indices_off = l.vertices_off + uint64_t(vertex_count) * sizeof(RtPgsOverlayVertex);
    l.cmds_off = l.indices_off + uint64_t(index_count) * sizeof(uint32_t);
    l.total = l.cmds_off + uint64_t(cmd_count) * sizeof(RtPgsOverlayCmd);
    return l;
}

static_assert(alignof(RtPgsOverlayVertex) <= 4 && alignof(uint32_t) <= 4 &&
                  alignof(RtPgsOverlayCmd) <= 4,
              "frame_layout assumes 4-byte alignment for the overlay arrays");

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

bool is_pow2(size_t v) { return v != 0 && (v & (v - 1)) == 0; }

} // namespace

ThreadedBackend::ThreadedBackend(GsBackend* inner, size_t ring_bytes, bool inline_drain)
    : m_inner(inner), m_capacity(ring_bytes), m_inline_drain(inline_drain) {
    if (!is_pow2(ring_bytes) || ring_bytes < 4096 || ring_bytes > 0xFFFFFFF0u) {
        /* The upper bound is RecHeader::size: a Wrap record can be as long as
         * the whole buffer, and its size has to fit in the header's u32. */
        rt_fatal("gs", nullptr,
                 "GS ring size %zu is not a power of two between 4096 and 0xFFFFFFF0",
                 ring_bytes);
    }
    m_mask = uint64_t(ring_bytes) - 1;
    m_buf = static_cast<uint8_t*>(::operator new[](ring_bytes, std::align_val_t(kRecordAlign)));
}

ThreadedBackend::~ThreadedBackend() {
    /* Guarded inside drain(): a window-closed std::exit() from inside
     * replay() lands here with m_in_drain still set, and re-replaying the
     * record that just exited would recurse. */
    const bool from_replay = m_in_drain;
    drain();
    const uint64_t pending = m_head.load(std::memory_order_relaxed) -
                             m_tail.load(std::memory_order_relaxed);
    if (pending) {
        /* from_replay is the ordinary window-close path: ParallelBackend::
         * vsync called std::exit() from inside replay(), so the record being
         * replayed did run and its bytes are still counted as live because
         * the tail never advanced past it. Anything else is a real loss and
         * says so. */
        rt_log("gs", "GS ring: %llu bytes still queued at teardown%s",
               (unsigned long long)pending,
               from_replay ? " (exit from inside a replayed call; the record ran)"
                           : ", discarded");
    }
    delete m_inner;
    m_inner = nullptr;
    ::operator delete[](m_buf, std::align_val_t(kRecordAlign));
    m_buf = nullptr;
}

/* ---- producer side ------------------------------------------------------ */

bool ThreadedBackend::ensure_space(uint64_t bytes) {
    const uint64_t head = m_head.load(std::memory_order_relaxed);
    const uint64_t tail = m_tail.load(std::memory_order_acquire);
    if (m_capacity - (head - tail) >= bytes) return true;
    back_pressure(bytes);
    return false;
}

void ThreadedBackend::back_pressure(uint64_t bytes) {
    ++m_stalls;
    if (m_head.load(std::memory_order_relaxed) == m_tail.load(std::memory_order_acquire)) {
        /* Unreachable while begin_record rejects oversized records up front;
         * kept because a silent spin here would be the worst way to find
         * out that it stopped being true. */
        rt_fatal("gs", nullptr, "GS ring: %llu bytes do not fit an empty %zu-byte ring",
                 (unsigned long long)bytes, m_capacity);
    }
    if (m_in_drain) {
        /* The consumer is this thread and it is already inside replay(), so
         * drain() below would return without freeing a byte and
         * begin_record's retry loop would spin forever. That means the inner
         * backend called back into this wrapper from inside a replayed call
         * (the event pump runs from inside ParallelBackend::vsync) and filled
         * the ring while doing it. Nothing does that today; fatal rather
         * than hang if something starts to. */
        rt_fatal("gs", nullptr,
                 "GS ring: full (%llu bytes wanted) while replaying; the inner backend "
                 "enqueued from inside drain()",
                 (unsigned long long)bytes);
    }
    /* Step 5: a timed wait on the consumer's condvar that also pumps the
     * window, so a stalled producer does not freeze the event loop. Today
     * the consumer is this thread, so the only way to make room is to run
     * it. */
    drain();
}

uint8_t* ThreadedBackend::begin_record(uint32_t kind, uint64_t arg, uint64_t payload_bytes) {
    const uint64_t size = align_up(uint64_t(sizeof(RecHeader)) + payload_bytes, kRecordAlign);
    if (size > m_capacity) {
        rt_fatal("gs", nullptr,
                 "GS ring: %s record of %llu bytes exceeds the %zu-byte ring",
                 kind_name(kind), (unsigned long long)size, m_capacity);
    }
    for (;;) {
        const uint64_t head = m_head.load(std::memory_order_relaxed);
        const uint64_t offset = head & m_mask;
        const uint64_t to_end = uint64_t(m_capacity) - offset;
        if (size > to_end) {
            /* Fill the tail of the buffer with a Wrap record so the real one
             * starts at offset 0 and stays contiguous. to_end is a positive
             * multiple of 16 because both the capacity and every record size
             * are, so it always holds at least a bare header. */
            if (!ensure_space(to_end)) continue;
            const RecHeader wrap = { kWrap, uint32_t(to_end), 0 };
            std::memcpy(m_buf + offset, &wrap, sizeof(wrap));
            ++m_wrap_markers;
            m_head.store(head + to_end, std::memory_order_release);
            continue;
        }
        if (!ensure_space(size)) continue;
        const RecHeader h = { kind, uint32_t(size), arg };
        std::memcpy(m_buf + offset, &h, sizeof(h));
        m_reserved_head = head + size;
        return m_buf + offset + sizeof(RecHeader);
    }
}

void ThreadedBackend::commit() {
    ++m_records_written;
    /* Release: the payload stores above must be visible to the consumer
     * before the head that exposes them. */
    m_head.store(m_reserved_head, std::memory_order_release);
    /* Step 5 replaces this with a condvar notify. */
    if (m_inline_drain) drain();
}

void ThreadedBackend::sync_point() {
    /* Ordering marker for the two calls that need a value back. In step 5
     * this record grows a reply slot and the producer waits on it; today
     * emitting it and draining to it is the same thing. */
    begin_record(kFence, ++m_fence_seq, 0);
    commit();
    if (!m_inline_drain) drain();
}

/* ---- GsBackend, producer side ------------------------------------------- */

void ThreadedBackend::submit_gif(int path, const uint8_t* data, uint32_t qwords) {
    const uint64_t bytes = uint64_t(qwords) * 16u;
    const uint64_t arg = uint64_t(uint32_t(path)) | (uint64_t(qwords) << 32);
    uint8_t* p = begin_record(kGif, arg, bytes);
    std::memcpy(p, data, size_t(bytes));
    commit();
}

void ThreadedBackend::write_priv(uint32_t offset, uint64_t v) {
    m_priv[(offset & 0x1FFF) >> 4] = v;
    const PrivPayload pl = { offset, { 0, 0, 0 } };
    uint8_t* p = begin_record(kPriv, v, sizeof(pl));
    std::memcpy(p, &pl, sizeof(pl));
    commit();
}

/* EE-side privileged register read, answered from this side's shadow without
 * consulting the inner backend, so a read never has to synchronize with the
 * consumer. Both inner flavors are last-written-value per 16-byte register
 * slot off a zero-initialized array, which is what this shadow is; they
 * split the 0x2000-byte block into two banks where this one uses a single
 * flat array, so the index expression differs but the mapping from offset to
 * slot does not. Verified rather than assumed:
 *
 *   - The live backend's RtPgs::read_priv (gs_parallel_lib.cpp:317) returns
 *     GSInterface's PrivRegisterState slot for the offset and nothing else
 *     (priv.qwords_lo[(offset >> 4) * 2] below 0x1000, qwords_hi above).
 *     RtPgs::write_priv (:305) is the only writer of that state that outlives
 *     a call: the one other writer in paraLLEl-GS's own code that runs here
 *     is GSInterface::vsync, which patches priv_registers.smode2.FFMD to 0
 *     (gs_interface.cpp:4663) and restores the saved value unconditionally
 *     before returning (:4692), so no read outside that call can see it.
 *     (grep get_priv_register_state: the remaining users are paraLLEl-GS's
 *     own dump generator and parser, neither of which runs in this process.)
 *     GSInterface value-initializes the state (gs_interface.hpp:368,
 *     `PrivRegisterState priv_registers = {}`), which matches this shadow
 *     starting zeroed, so a never-written offset reads 0 on both sides.
 *   - The dump writer keeps the same kind of shadow
 *     (gs_dumpwriter.cpp:107-122, m_lo/m_hi banks indexed
 *     (offset >> 4) * 2 and ((offset - 0x1000) >> 4) * 2, both `= {}` at
 *     :177-178), and TeeBackend::read_priv (gs_select.cpp:59) already
 *     answers from it. Answering here first changes nothing the dump file
 *     records: the dump writes on write_priv and at vsync, never on a read.
 *   - CSR and IMR never reach a backend read at all:
 *     hw/gspriv.cpp:47-51 asks rt_gs_mmio_read (ee/intc.cpp) first, and
 *     intc answers 0x12001000 and 0x12001010 from its own event-flag state,
 *     which is where the FIELD bit and the write-1-clear semantics live.
 *     The backend only ever sees them as opaque priv writes.
 */
uint64_t ThreadedBackend::read_priv(uint32_t offset) {
    return m_priv[(offset & 0x1FFF) >> 4];
}

bool ThreadedBackend::vsync(unsigned field) {
    const uint64_t arg = uint64_t(field) | (uint64_t(++m_vsync_seq) << 32);
    begin_record(kVsync, arg, 0);
    commit();
    /* Set by replay(). Inline draining makes it this field's answer; once
     * the consumer runs ahead on its own thread the producer will have to
     * decide what "a frame was presented" means without waiting, which is a
     * step 5 question and deliberately not answered here. */
    return m_last_presented;
}

void ThreadedBackend::set_presentation(uint32_t fit, uint32_t filter) {
    begin_record(kSetPresentation, uint64_t(fit) | (uint64_t(filter) << 32), 0);
    commit();
}

void ThreadedBackend::set_present_mode(uint32_t mode) {
    begin_record(kSetPresentMode, mode, 0);
    commit();
}

void ThreadedBackend::set_render_scale(uint32_t factor) {
    begin_record(kSetRenderScale, factor, 0);
    commit();
}

void ThreadedBackend::set_raster(uint32_t raster) {
    begin_record(kSetRaster, raster, 0);
    commit();
}

void ThreadedBackend::set_deinterlace(uint32_t deinterlace) {
    begin_record(kSetDeinterlace, deinterlace, 0);
    commit();
}

void ThreadedBackend::overlay_texture_destroy(uint32_t texture) {
    begin_record(kOverlayTexDestroy, texture, 0);
    commit();
}

void ThreadedBackend::overlay_set_frame(const RtPgsOverlayFrame* frame) {
    if (!frame) {
        begin_record(kOverlaySetFrame, 0, 0);
        commit();
        return;
    }
    const FrameLayout l = frame_layout(frame->vertex_count, frame->index_count, frame->cmd_count);
    uint8_t* p = begin_record(kOverlaySetFrame, 1, l.total);
    const FramePayload head = {
        frame->vertex_count, frame->index_count, frame->cmd_count,
        frame->surface_width, frame->surface_height, { 0, 0, 0 },
    };
    std::memcpy(p, &head, sizeof(head));
    if (frame->vertex_count) {
        std::memcpy(p + l.vertices_off, frame->vertices,
                    size_t(frame->vertex_count) * sizeof(RtPgsOverlayVertex));
    }
    if (frame->index_count) {
        std::memcpy(p + l.indices_off, frame->indices,
                    size_t(frame->index_count) * sizeof(uint32_t));
    }
    if (frame->cmd_count) {
        std::memcpy(p + l.cmds_off, frame->cmds,
                    size_t(frame->cmd_count) * sizeof(RtPgsOverlayCmd));
    }
    commit();
}

uint32_t ThreadedBackend::overlay_texture_create(const uint8_t* rgba8, uint32_t width,
                                                 uint32_t height) {
    sync_point();
    return m_inner->overlay_texture_create(rgba8, width, height);
}

uint32_t ThreadedBackend::present_ui() {
    sync_point();
    return m_inner->present_ui();
}

void ThreadedBackend::report_stats() {
    rt_log("gs", "GS ring: %llu records (%llu replayed), %llu bytes, %llu wrap markers,"
                 " %llu back-pressure stalls, %zu-byte ring",
           (unsigned long long)m_records_written, (unsigned long long)m_records_replayed,
           (unsigned long long)m_head.load(std::memory_order_relaxed),
           (unsigned long long)m_wrap_markers, (unsigned long long)m_stalls, m_capacity);
    m_inner->report_stats();
}

void ThreadedBackend::present_timings(uint64_t* flush_ns, uint64_t* scanout_ns,
                                      uint64_t* present_ns, uint64_t* fields) {
    m_inner->present_timings(flush_ns, scanout_ns, present_ns, fields);
}

/* ---- consumer side ------------------------------------------------------ */

void ThreadedBackend::replay(uint32_t kind, uint64_t arg, const uint8_t* payload) {
    switch (kind) {
    case kGif:
        m_inner->submit_gif(int(uint32_t(arg)), payload, uint32_t(arg >> 32));
        break;
    case kPriv: {
        PrivPayload pl;
        std::memcpy(&pl, payload, sizeof(pl));
        m_inner->write_priv(pl.offset, arg);
        break;
    }
    case kVsync: {
        /* The header's arg only has 32 bits for the sequence number, so the
         * comparison is on the low 32 bits too: a full-width compare would
         * fatal spuriously after 2^32 fields (about 2.3 years at 60 Hz). */
        const uint64_t seq = arg >> 32;
        const uint64_t want = ++m_vsync_seen & 0xFFFFFFFFull;
        if (seq != want) {
            rt_fatal("gs", nullptr, "GS ring: vsync record out of order (got %llu, expected %llu)",
                     (unsigned long long)seq, (unsigned long long)want);
        }
        m_last_presented = m_inner->vsync(unsigned(uint32_t(arg)));
        break;
    }
    case kFence:
        /* Ordering marker only. Step 5 posts the reply here. */
        break;
    case kSetPresentation:
        m_inner->set_presentation(uint32_t(arg), uint32_t(arg >> 32));
        break;
    case kSetPresentMode:
        m_inner->set_present_mode(uint32_t(arg));
        break;
    case kSetRenderScale:
        m_inner->set_render_scale(uint32_t(arg));
        break;
    case kSetRaster:
        m_inner->set_raster(uint32_t(arg));
        break;
    case kSetDeinterlace:
        m_inner->set_deinterlace(uint32_t(arg));
        break;
    case kOverlayTexDestroy:
        m_inner->overlay_texture_destroy(uint32_t(arg));
        break;
    case kOverlaySetFrame: {
        if (arg == 0) {
            m_inner->overlay_set_frame(nullptr);
            break;
        }
        FramePayload head;
        std::memcpy(&head, payload, sizeof(head));
        const FrameLayout l = frame_layout(head.vertex_count, head.index_count, head.cmd_count);
        RtPgsOverlayFrame frame = {};
        frame.vertex_count = head.vertex_count;
        frame.index_count = head.index_count;
        frame.cmd_count = head.cmd_count;
        frame.surface_width = head.surface_width;
        frame.surface_height = head.surface_height;
        /* Pointers into the ring: valid for the duration of the call, which
         * is all rt_pgs_overlay_set_frame needs (it deep-copies). The tail
         * is not advanced past this record until the call returns. */
        frame.vertices = head.vertex_count
                             ? reinterpret_cast<const RtPgsOverlayVertex*>(payload + l.vertices_off)
                             : nullptr;
        frame.indices = head.index_count
                            ? reinterpret_cast<const uint32_t*>(payload + l.indices_off)
                            : nullptr;
        frame.cmds = head.cmd_count
                         ? reinterpret_cast<const RtPgsOverlayCmd*>(payload + l.cmds_off)
                         : nullptr;
        m_inner->overlay_set_frame(&frame);
        break;
    }
    default:
        rt_fatal("gs", nullptr, "GS ring: unknown record kind %u", kind);
    }
}

void ThreadedBackend::drain() {
    if (m_in_drain) return;
    m_in_drain = true;
    uint64_t tail = m_tail.load(std::memory_order_relaxed);
    const uint64_t head = m_head.load(std::memory_order_acquire);
    while (tail != head) {
        const uint8_t* rec = m_buf + (tail & m_mask);
        RecHeader h;
        std::memcpy(&h, rec, sizeof(h));
        if (h.size < sizeof(RecHeader) || (h.size & (kRecordAlign - 1)) != 0 ||
            tail + h.size > head) {
            rt_fatal("gs", nullptr,
                     "GS ring: corrupt record at %llu (kind %u, size %u, head %llu)",
                     (unsigned long long)tail, h.kind, h.size, (unsigned long long)head);
        }
        if (h.kind != kWrap) {
            ++m_records_replayed;
            replay(h.kind, h.arg, rec + sizeof(RecHeader));
        }
        /* Published only after the record has been consumed: the payload is
         * still the producer's to overwrite the moment the tail moves past
         * it. */
        tail += h.size;
        m_tail.store(tail, std::memory_order_release);
    }
    m_in_drain = false;
}

GsBackend* rt_gs_make_threaded_backend(GsBackend* inner) {
    return new ThreadedBackend(inner);
}
