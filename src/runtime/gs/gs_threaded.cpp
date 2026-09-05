/* gs/gs_threaded.cpp: the GS command ring, ThreadedBackend, and the worker
 * thread that drains it.
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
 * Record format
 * -------------
 * One byte ring, capacity a power of two, allocated once. Head and tail are
 * monotonically increasing byte counters (std::atomic<uint64_t>, never
 * wrapped): the buffer offset is counter & (capacity - 1), head - tail is
 * the number of live bytes, head == tail means empty, and head - tail ==
 * capacity means full. One producer, one consumer, no locks on the ring
 * itself; the mutex below exists only for the two condition variables.
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
 *   SetPresentation fit | filter << 32                      none
 *   SetPresentMode  RT_PGS_PRESENT_* mode                   none
 *   SetRenderScale  super-sampling factor                   none
 *   SetRaster       RT_PGS_RASTER_* value                   none
 *   SetDeinterlace  RT_PGS_DEINTERLACE_* value              none
 *   SetWideAspect   the widescreen present aspect, double bits  none
 *   SetPresentRate  display.present_rate as a double's bits    none
 *   OverlaySetFrame 0 = clear, 1 = frame follows            FramePayload + arrays
 *   OverlayTexDestroy texture id                            none
 *   OverlayTexCreate call sequence number                   TexPayload + RGBA8 texels
 *   PresentUi       call sequence number                    none
 *   RequestShot     screenshot slots to arm (1 or 2)        none
 *
 * OverlaySetFrame's payload is a FramePayload counts header followed by the
 * vertex, index and command arrays back to back, at the offsets
 * frame_layout() computes. frame_layout() is the single definition of that
 * layout: the encoder and the decoder both call it, so they cannot drift.
 *
 * The last two are the calls that need a value back. They carry a call
 * sequence number instead of a return path: the consumer runs the call and
 * publishes (m_reply_value, m_reply_seq), and the producer waits until
 * m_reply_seq has reached its own sequence number. Ordering against every
 * other record is what the ring already gives; the reply only adds the wait.
 *
 * Copy policy
 * -----------
 * Packet bytes are always copied into the ring, never referenced. Verified
 * at third_party/parallel-gs/gs/gs_interface.cpp:4294 (GSInterface::
 * gif_transfer): it walks the buffer and dispatches register writes and
 * image uploads inside the call and retains no pointer to it. Every caller
 * on our side hands over transient memory anyway (hw/gif.cpp passes a
 * pointer into guest RAM or into a VIF1 staging buffer, ui/ui_render.cpp
 * passes vectors it reuses next tick), so with the consumer on another
 * thread there is nothing safe to reference. Copying is what makes the
 * ring's contents self-contained.
 *
 * Wakeups
 * -------
 * The producer wakes the consumer only when the consumer has published that
 * it is about to park (m_consumer_waiting), and the consumer wakes the
 * producer only when the producer has published that it is waiting
 * (m_progress_waiting for a vsync or a reply, m_space_waiting for room in
 * the ring). What makes that handshake sound is a seq_cst fence on both
 * sides, not the mutex: each side stores its own word and then loads the
 * other's, and store-then-load is exactly the order a release store and an
 * acquire load do not give. Waiter: publish the flag, fence, test the
 * condition. Waker: publish the condition, fence, read the flag. Then at
 * least one of the two sees the other, so no wakeup is lost. Every wait is
 * bounded as well (2 ms), so the cost of getting this wrong would have been
 * latency rather than the run, which is why it is worth stating that it is
 * not being relied on. The two producer-side words are counters and not
 * flags because a wait runs the event pump, which can enqueue and wait
 * again.
 *
 * Every EE-side wait pumps the window between polls. That is not a nicety:
 * the worker can be parked inside Granite's block_until_wsi_forward_progress
 * with a minimized window, and only the EE thread may call SDL, so the
 * restore event that frees the worker can only come from an EE-side pump.
 *
 * No RT_PROF_ZONE anywhere in this file. prof.h's zones are single-threaded
 * by construction (see its header comment) and drain() runs on the worker.
 * The consumer's own costs are plain atomics instead, published through
 * consumer_timings() (gs_backend.h) and reported by prof.h on its own line.
 */
#include "gs_threaded.h"

#include "gs_parallel_api.h"
#include "runtime.h"

#include <chrono>
#include <cstring>
#include <iterator>
#include <cstdlib>
#include <new>

/* host/window.h's event pump and its dispatch hold. Declared here rather
 * than included: window.h pulls in host/settings.h, and the ring's selftest
 * target (icorecomp-gsring-selftest) links neither of those; it supplies its
 * own stubs for these two symbols instead. */
void rt_window_pump();
void rt_window_hold_event_dispatch(bool on);

namespace {

constexpr uint32_t kRecordAlign = 16;

/* How long an EE-side wait sleeps before it wakes to pump the window. Short
 * enough that a minimized window still gets its events at 500 Hz, long
 * enough that a wait about to be notified anyway almost never spends a
 * wakeup on it. */
constexpr auto kWaitStep = std::chrono::milliseconds(2);

/* How long the field sync point waits before it says out loud that the
 * consumer has stopped making progress. Long enough that no shader compile,
 * swapchain rebuild or driver hiccup reaches it, short enough that a user
 * who sees the picture freeze finds the reason in the log. */
constexpr uint64_t kFieldSyncStuckNs = 5ull * 1000 * 1000 * 1000;

/* How long one present may take before the consumer says so. A present on
 * a healthy device is bounded by the refresh interval; seconds means the
 * driver is waiting on something that is not coming. Two seconds is well
 * past a shader compile or a swapchain rebuild and well short of a user
 * deciding the program has hung. */
constexpr uint64_t kSlowPresentNs = 2ull * 1000 * 1000 * 1000;

/* How long quiesce() waits for the worker to park before giving up on it.
 * Only reachable when the worker cannot return: parked inside Granite with
 * an unusable swapchain, or wedged in a driver call. */
constexpr auto kJoinDeadline = std::chrono::milliseconds(1000);

using RingClock = std::chrono::steady_clock;

uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        RingClock::now().time_since_epoch()).count());
}

/* Record kinds. Values are internal to this file (nothing persists a ring),
 * but they are fixed rather than reordered casually because the selftest
 * prints them by name. */
enum RecordKind : uint32_t {
    kWrap = 0,
    kGif,
    kPriv,
    kVsync,
    kSetPresentation,
    kSetPresentMode,
    kSetRenderScale,
    kSetRaster,
    kSetDeinterlace,
    kSetWideAspect,
    kSetPresentRate,
    kOverlaySetFrame,
    kOverlayTexDestroy,
    kOverlayTexCreate,
    kPresentUi,
    kRequestShot,
    kKindCount,
};

const char* kind_name(uint32_t kind) {
    /* One name per RecordKind, in enum order. The bound is deduced and
     * asserted rather than written as [kKindCount]: an explicit bound lets a
     * short initialiser list compile, which is how every name past a missing
     * one came to be off by one and the last one a null pointer, inside the
     * oversized-record fatal that exists to explain what went wrong. */
    static const char* const kNames[] = {
        "wrap", "gif", "priv", "vsync", "set-presentation",
        "set-present-mode", "set-render-scale", "set-raster", "set-deinterlace",
        "set-wide-aspect", "set-present-rate", "overlay-set-frame",
        "overlay-tex-destroy", "overlay-tex-create", "present-ui",
        "request-screenshot",
    };
    static_assert(std::size(kNames) == size_t(kKindCount),
                  "kind_name needs one name per RecordKind, in enum order");
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

/* Overlay texture payload header; the RGBA8 texels follow it. `has_data`
 * records whether the caller's pointer was non-null, so a null one crosses
 * the ring as itself. Reconstructing it as a pointer at the payload would
 * hand the inner backend ring bytes this side never wrote, in place of the
 * argument the caller actually passed (the live backend answers a null
 * pointer with a log and a 0 texture id; that answer is its to give). */
struct TexPayload {
    uint32_t width;
    uint32_t height;
    uint32_t has_data;
    uint32_t pad;
};
static_assert(sizeof(TexPayload) == 16, "texture payload header must stay 16 bytes");

/* Overlay frame payload header: the counts and the surface size the geometry
 * was laid out for. The three arrays follow at frame_layout()'s offsets. */
struct FramePayload {
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t cmd_count;
    uint32_t surface_width;
    uint32_t surface_height;
    /* Bit 0 vertices, bit 1 indices, bit 2 commands: whether the caller's
     * pointer for that array was non-null, for the same reason TexPayload
     * carries has_data. A null pointer with a nonzero count is a caller bug
     * the inner backend has its own answer for (the live one logs and
     * ignores the frame), and copying from it here would fault before the
     * inner backend ever saw it. An array that is absent occupies no payload
     * bytes, so frame_layout is given the effective counts on both sides. */
    uint32_t arrays;
    uint32_t pad[2];
};
static_assert(sizeof(FramePayload) == 32, "frame payload header must stay 32 bytes");

constexpr uint32_t kFrameHasVertices = 1u;
constexpr uint32_t kFrameHasIndices = 2u;
constexpr uint32_t kFrameHasCmds = 4u;

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
    : m_inner(inner), m_capacity(ring_bytes),
      m_mode(inline_drain ? Mode::Inline : Mode::Manual) {
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
    /* Stops and joins the worker (a no-op if gs_select.cpp's atexit handler
     * already did, or if the worker never started), so everything below runs
     * with this thread the only user of the inner backend again. */
    quiesce();
    /* Guarded inside drain(): a std::exit() from inside replay() lands here
     * with m_in_drain still set, and re-replaying the record that just
     * exited would recurse. */
    const bool from_replay = m_in_drain;
    drain();
    const uint64_t pending = m_head.load(std::memory_order_relaxed) -
                             m_tail.load(std::memory_order_relaxed);
    if (pending) {
        /* from_replay is an exit from inside a replayed call: the record
         * being replayed did run and its bytes are still counted as live
         * because the tail never advanced past it. Anything else is a real
         * loss and says so. */
        rt_log_info("gs", "GS ring: %llu bytes still queued at teardown%s",
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
    if (m_mode == Mode::Worker) {
        /* Wait for the consumer to free room, pumping the window between
         * polls. The space test is re-read every time round: the pump itself
         * can enqueue (a settings applier reached from a UI event), so the
         * head moves under this wait as well as the tail. */
        const uint64_t t0 = now_ns();
        std::unique_lock<std::mutex> lk(m_mu);
        m_space_waiting.fetch_add(1, std::memory_order_relaxed);
        /* Pairs with the fence in wake_producer; see the comment there. It
         * has to sit between publishing the flag and the first test of the
         * condition below. */
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (;;) {
            const uint64_t head = m_head.load(std::memory_order_relaxed);
            const uint64_t tail = m_tail.load(std::memory_order_acquire);
            if (m_capacity - (head - tail) >= bytes) break;
            if (m_worker_exited.load(std::memory_order_acquire)) {
                m_space_waiting.fetch_sub(1, std::memory_order_relaxed);
                lk.unlock();
                rt_fatal("gs", nullptr,
                         "GS ring: full (%llu bytes wanted) and the worker thread has stopped",
                         (unsigned long long)bytes);
            }
            if (closed_window_timeout(t0)) {
                m_space_waiting.fetch_sub(1, std::memory_order_relaxed);
                lk.unlock();
                rt_log_info("gs", "GS ring: full for a second with the window closed and the"
                             " consumer parked; exiting");
                rt_run_set_exit_reason(true, "the window closed while the GS consumer was parked"
                    " and the command ring filled up behind it");
                std::exit(0);
            }
            wait_step(lk);
        }
        m_space_waiting.fetch_sub(1, std::memory_order_relaxed);
        lk.unlock();
        m_ee_wait_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
        m_ee_waits.fetch_add(1, std::memory_order_relaxed);
        return;
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
    /* Inline and Manual: the consumer is this thread, so the only way to make
     * room is to run it. */
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
        /* ensure_space either returns true having done nothing at all, or
         * returns false having possibly run the event pump, which can enqueue
         * records of its own and move the head. Only the false path loops, so
         * `head` below is always this producer's current head. */
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
    if (m_mode == Mode::Inline) {
        drain();
    } else if (m_mode == Mode::Worker) {
        wake_consumer();
    }
}

/* The waker half of the handshake described in the file header. The fence is
 * the whole of it: this thread has just released-stored what the other one is
 * waiting for (the head, a vsync sequence, a reply) and is about to load the
 * flag that says whether it is waiting. Store-then-load is the one ordering
 * a release store and an acquire load do not give -- on x86 both are plain
 * moves and the load may be satisfied before the store leaves the store
 * buffer -- so without a full fence on both sides the waiter can publish its
 * flag and read a stale head while this thread reads a stale flag, and
 * nothing wakes anybody. The waiters put the matching fence right after
 * their own flag store. The mutex below is only the condition variable's;
 * the fence, not the lock, is what makes the wakeup impossible to lose. */
void ThreadedBackend::wake_consumer() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!m_consumer_waiting.load(std::memory_order_relaxed)) return;
    { std::lock_guard<std::mutex> lk(m_mu); }
    m_cv_consumer.notify_one();
}

void ThreadedBackend::wake_producer() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (m_progress_waiting.load(std::memory_order_relaxed) == 0 &&
        m_space_waiting.load(std::memory_order_relaxed) == 0) {
        return;
    }
    { std::lock_guard<std::mutex> lk(m_mu); }
    m_cv_producer.notify_one();
}

/* Every producer-side wait can outlive the consumer's ability to make
 * progress: a window closed while the swapchain was unusable parks the
 * consumer inside the library for good (gs_parallel_impl.h,
 * block_until_wsi_forward_progress). field_sync() answers that by giving up
 * and letting hw/gspriv.cpp take the exit at the field boundary. The other
 * two waits have no such caller -- a producer waiting for room, or for a
 * texture id, has nothing to return -- so after a second of a closed window
 * they take that same exit themselves, on this thread, which is where
 * process teardown belongs. The second is what keeps a close that arrives
 * while the consumer is still working from cutting a field short: that wait
 * ends normally long before this is true.
 *
 * Every caller drops m_mu before it exits or fatals. std::exit runs
 * gs_select.cpp's atexit handler, which calls quiesce(), which takes m_mu:
 * leaving here with it held would deadlock the teardown against itself. */
bool ThreadedBackend::closed_window_timeout(uint64_t wait_started_ns) const {
    return m_inner->window_closed() && now_ns() - wait_started_ns >= 1000000000ull;
}

/* One bounded wait, then one pump.
 *
 * The pump must not run under the mutex: it can enqueue records, and
 * committing one takes this same mutex to wake the consumer.
 *
 * It also must not dispatch events. One of the three waits that reach here,
 * the reply to an overlay texture upload, runs from inside RmlUi's
 * Context::Render (ui/ui_render.cpp's GenerateTexture), and dispatching an
 * SDL event from there re-enters RmlUi's context while it is rendering.
 * rt_window_hold_event_dispatch turns the pump into the restricted form for
 * the duration: the platform message loop still runs (so a minimized window
 * can be restored and the consumer freed), quit and resize are still seen,
 * and every input event stays queued for the field boundary. Held around
 * every wait rather than only that one, because a rule with no exceptions is
 * the one that stays true. */
void ThreadedBackend::wait_step(std::unique_lock<std::mutex>& lk) {
    m_cv_producer.wait_for(lk, kWaitStep);
    lk.unlock();
    rt_window_hold_event_dispatch(true);
    rt_window_pump();
    rt_window_hold_event_dispatch(false);
    lk.lock();
}

uint64_t ThreadedBackend::await_reply(uint64_t seq) {
    if (m_mode != Mode::Worker) {
        /* Inline already replayed the record inside commit(); Manual needs
         * the drain here, which is what makes this call a sync point in
         * those two modes. */
        drain();
    } else {
        const uint64_t t0 = now_ns();
        std::unique_lock<std::mutex> lk(m_mu);
        m_progress_waiting.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst); /* see wake_producer */
        while (m_reply_seq.load(std::memory_order_acquire) < seq) {
            if (m_worker_exited.load(std::memory_order_acquire)) {
                m_progress_waiting.fetch_sub(1, std::memory_order_relaxed);
                lk.unlock();
                rt_fatal("gs", nullptr,
                         "GS ring: waiting for reply %llu but the worker thread has stopped",
                         (unsigned long long)seq);
            }
            if (closed_window_timeout(t0)) {
                m_progress_waiting.fetch_sub(1, std::memory_order_relaxed);
                lk.unlock();
                rt_log_info("gs", "GS ring: waited a second for a reply with the window closed and"
                             " the consumer parked; exiting");
                rt_run_set_exit_reason(true, "the window closed while the GS consumer was parked,"
                    " and the EE gave up waiting for reply %llu", (unsigned long long)seq);
                std::exit(0);
            }
            wait_step(lk);
        }
        m_progress_waiting.fetch_sub(1, std::memory_order_relaxed);
        lk.unlock();
        m_ee_wait_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
        m_ee_waits.fetch_add(1, std::memory_order_relaxed);
    }
    /* Exactly this reply, not "at least" it: there is one reply slot, so a
     * second reply-carrying call posted before this one read its value would
     * leave m_reply_value holding the wrong answer. Records are replayed in
     * order and this thread is the only producer, so a later sequence number
     * here means a reply-carrying call was made from inside this wait, which
     * the single slot cannot serve. Fatal rather than hand back another
     * call's texture id. */
    const uint64_t got = m_reply_seq.load(std::memory_order_acquire);
    if (got != seq) {
        rt_fatal("gs", nullptr, "GS ring: reply %llu overtaken by %llu",
                 (unsigned long long)seq, (unsigned long long)got);
    }
    return m_reply_value.load(std::memory_order_relaxed);
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
 *
 * With the worker running, this is also what keeps a privileged read off the
 * synchronization path entirely: it is a plain array load on the EE thread,
 * with no wait and no atomic.
 */
uint64_t ThreadedBackend::read_priv(uint32_t offset) {
    return m_priv[(offset & 0x1FFF) >> 4];
}

bool ThreadedBackend::vsync(unsigned field) {
    const uint64_t arg = uint64_t(field) | (uint64_t(++m_vsync_seq) << 32);
    begin_record(kVsync, arg, 0);
    commit();
    /* Set by replay(). In Worker mode this is the previous field's answer,
     * one field stale by construction, which is what "one field in flight"
     * means. No caller outside the ring reads it: hw/gspriv.cpp ignores the
     * result, and the only other reader (TeeBackend) is inside the ring. */
    return m_last_presented.load(std::memory_order_relaxed);
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

/* The aspect travels as the double's own bits, the way set_present_rate's
 * rate does, and for the same reason it rides the ring at all: it changes
 * what a present does and has to reach the consumer in order with the fields
 * it applies to. */
void ThreadedBackend::set_widescreen_aspect(double aspect) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(aspect), "a double must fit a record arg");
    std::memcpy(&bits, &aspect, sizeof(bits));
    begin_record(kSetWideAspect, bits, 0);
    commit();
}

/* The rate travels as the double's own bits in the header's arg. It is a
 * host pacing knob and not a value the guest supplied, so nothing is
 * rounded or clamped on the way: what settings.json holds is what the
 * consumer divides 1 by. */
void ThreadedBackend::set_present_rate(double max_hz) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(max_hz), "a double must fit a record arg");
    std::memcpy(&bits, &max_hz, sizeof(bits));
    begin_record(kSetPresentRate, bits, 0);
    commit();
}

/* Ordered against the GIF and vsync traffic like every other call that
 * changes what a present does, so the capture lands on the field the user
 * pressed the key on rather than one either side of it. The pixels come back
 * the other way (GsBackend::take_screenshot, passed straight through), never
 * through this ring: see the threading contract in gs_threaded.h. */
void ThreadedBackend::request_screenshot(uint32_t slots) {
    begin_record(kRequestShot, slots, 0);
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
    const uint32_t arrays = (frame->vertices ? kFrameHasVertices : 0u) |
                            (frame->indices ? kFrameHasIndices : 0u) |
                            (frame->cmds ? kFrameHasCmds : 0u);
    const uint32_t vn = frame->vertices ? frame->vertex_count : 0u;
    const uint32_t in = frame->indices ? frame->index_count : 0u;
    const uint32_t cn = frame->cmds ? frame->cmd_count : 0u;
    const FrameLayout l = frame_layout(vn, in, cn);
    uint8_t* p = begin_record(kOverlaySetFrame, 1, l.total);
    const FramePayload head = {
        frame->vertex_count, frame->index_count, frame->cmd_count,
        frame->surface_width, frame->surface_height, arrays, { 0, 0 },
    };
    std::memcpy(p, &head, sizeof(head));
    if (vn) std::memcpy(p + l.vertices_off, frame->vertices, size_t(vn) * sizeof(RtPgsOverlayVertex));
    if (in) std::memcpy(p + l.indices_off, frame->indices, size_t(in) * sizeof(uint32_t));
    if (cn) std::memcpy(p + l.cmds_off, frame->cmds, size_t(cn) * sizeof(RtPgsOverlayCmd));
    commit();
}

/* The texels are copied into the ring like packet bytes, for the same reason:
 * the caller's buffer (RmlUi's atlas, or a decoded image) is transient. An
 * RmlUi font atlas is a few megabytes at most against a 32 MB ring; a texture
 * larger than the whole ring is rejected by begin_record with both sizes in
 * the message rather than silently split. */
uint32_t ThreadedBackend::overlay_texture_create(const uint8_t* rgba8, uint32_t width,
                                                 uint32_t height) {
    const uint64_t texels = rgba8 ? uint64_t(width) * uint64_t(height) * 4u : 0u;
    const uint64_t seq = ++m_call_seq;
    uint8_t* p = begin_record(kOverlayTexCreate, seq, sizeof(TexPayload) + texels);
    const TexPayload pl = { width, height, rgba8 ? 1u : 0u, 0 };
    std::memcpy(p, &pl, sizeof(pl));
    if (texels) std::memcpy(p + sizeof(pl), rgba8, size_t(texels));
    commit();
    return uint32_t(await_reply(seq));
}

/* Launcher only (ui/ui_launcher.cpp), which runs before the worker starts, so
 * in practice this is replayed by the inline drain inside commit() and the
 * wait returns at once. It goes through the ring anyway: calling the inner
 * backend directly would be a call the ring cannot order against the rest,
 * and this way the launcher path is the same code in both modes. */
uint32_t ThreadedBackend::present_ui() {
    const uint64_t seq = ++m_call_seq;
    begin_record(kPresentUi, seq, 0);
    commit();
    return uint32_t(await_reply(seq));
}

void ThreadedBackend::field_sync() {
    if (m_mode != Mode::Worker) return;
    /* One field in flight: the consumer may still be inside the vsync just
     * enqueued, but every earlier one has to be done. */
    if (m_vsync_seq < 2) return;
    const uint64_t want = m_vsync_seq - 1;
    if (m_vsync_done.load(std::memory_order_acquire) >= want) return;
    const uint64_t t0 = now_ns();
    std::unique_lock<std::mutex> lk(m_mu);
    m_progress_waiting.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_seq_cst); /* see wake_producer */
    while (m_vsync_done.load(std::memory_order_acquire) < want) {
        if (m_worker_exited.load(std::memory_order_acquire)) {
            m_progress_waiting.fetch_sub(1, std::memory_order_relaxed);
            lk.unlock();
            rt_fatal("gs", nullptr,
                     "GS ring: waiting for vsync %llu but the worker thread has stopped",
                     (unsigned long long)want);
        }
        /* The window can close while the consumer is parked inside the
         * library with no vsync left to return (a close while minimized).
         * Stop waiting for a field that will never complete and let
         * hw/gspriv.cpp take the exit. */
        if (m_inner->window_closed()) break;
        /* A live window and a consumer that has not moved for five seconds
         * is not a wait any more, it is a hang, and the only thing worse
         * than one is a silent one. Say so once, with the state that decides
         * where to look next, and keep waiting: the consumer may still be
         * inside a driver call that returns, and killing a run that is only
         * very slow would be its own bug. */
        if (!m_field_sync_stuck_logged &&
            now_ns() - t0 >= kFieldSyncStuckNs) {
            m_field_sync_stuck_logged = true;
            /* warn, not info: at the shipped default level this is the only
             * line that would say the picture has stopped, and a frozen run
             * whose log says nothing is the failure this file's diagnostics
             * exist to remove. */
            rt_log_warn("gs", "GS ring: the EE has waited %.1f s for vsync %llu (consumer at %llu,"
                         " %llu bytes queued, consumer %s, window open); still waiting",
                   double(now_ns() - t0) / 1e9, (unsigned long long)want,
                   (unsigned long long)m_vsync_done.load(std::memory_order_relaxed),
                   (unsigned long long)(m_head.load(std::memory_order_relaxed) -
                                        m_tail.load(std::memory_order_relaxed)),
                   m_consumer_waiting.load(std::memory_order_relaxed) ? "parked on an empty ring"
                                                                     : "inside a record");
        }
        wait_step(lk);
    }
    m_progress_waiting.fetch_sub(1, std::memory_order_relaxed);
    lk.unlock();
    m_ee_wait_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
    m_ee_waits.fetch_add(1, std::memory_order_relaxed);
}

bool ThreadedBackend::window_closed() {
    /* An atomic read on the live backend, so it is honest whatever the
     * consumer is doing. */
    return m_inner->window_closed();
}

void ThreadedBackend::report_stats() {
    rt_log_info("gs", "GS ring: %llu records (%llu replayed), %llu bytes of ring written"
                 " (wrap padding included), %llu wrap markers,"
                 " %llu back-pressure stalls, %zu-byte ring",
           (unsigned long long)m_records_written,
           (unsigned long long)m_records_replayed.load(std::memory_order_relaxed),
           (unsigned long long)m_head.load(std::memory_order_relaxed),
           (unsigned long long)m_wrap_markers, (unsigned long long)m_stalls, m_capacity);
    m_inner->report_stats();
}

void ThreadedBackend::present_timings(RtGsPresentTimings* out) {
    /* Read from the EE thread while the consumer may be inside a present:
     * every counter is an atomic on the library side for exactly this call
     * (gs_parallel_impl.h). */
    m_inner->present_timings(out);
}

void ThreadedBackend::consumer_timings(RtGsConsumerTimings* out) {
    if (!out) return;
    out->gs_ns = m_worker_gs_ns.exchange(0, std::memory_order_relaxed);
    out->present_ns = m_worker_present_ns.exchange(0, std::memory_order_relaxed);
    out->idle_ns = m_worker_idle_ns.exchange(0, std::memory_order_relaxed);
    out->fields = m_worker_fields.exchange(0, std::memory_order_relaxed);
    out->ee_wait_ns = m_ee_wait_ns.exchange(0, std::memory_order_relaxed);
    out->ee_waits = m_ee_waits.exchange(0, std::memory_order_relaxed);
}

/* ---- worker thread ------------------------------------------------------ */

void ThreadedBackend::start_worker() {
    if (m_mode == Mode::Worker) {
        rt_fatal("gs", nullptr, "GS ring: the worker thread is already running");
    }
    if (m_mode == Mode::Manual) {
        rt_fatal("gs", nullptr,
                 "GS ring: the worker thread cannot take over a manually drained ring");
    }
    /* Everything committed so far was replayed inline, so the worker starts
     * on an empty ring and the handover needs no synchronization beyond the
     * thread's own start edge. */
    drain();
    m_mode = Mode::Worker;
    m_worker = std::thread([this] { worker_main(); });
    m_worker_id = m_worker.get_id();
    /* Releases the worker, which spins on this until the id above is stored:
     * its very first drain() checks that it is the consumer. */
    m_worker_started.store(true, std::memory_order_release);
    rt_log_info("gs", "GS worker thread started: the command ring is drained off the EE thread"
                 " (packet parse, flush, scanout and present move there, one field in flight)");
}

/* Consumer side, both of them. See gs_threaded.h.
 *
 * The stamp is taken before the call and not after it: what has to happen at
 * the interval is the asking, and a present that overran its own interval
 * must not immediately owe another one. The library takes the same view of
 * its own last-present stamp (gs_parallel_present.cpp), which is what
 * actually paces the picture; this only decides when to knock. */
void ThreadedBackend::pump_present() {
    m_last_pump_ns = now_ns();
    rt_run_note_gs_worker("inside a present");
    m_inner->present_pump(m_present_rate);
    /* A present is a swapchain acquire, a submit and a queue present, and on
     * a healthy device the longest of those is one refresh interval. Seconds
     * means the driver is waiting on something that is not coming: a lost
     * device, a display that went away, a compositor that stopped answering.
     * Said once, with how long it took, because a run that is doing this
     * every field would otherwise fill the log with the same line and a run
     * that did it once would say nothing at all. */
    const uint64_t took = now_ns() - m_last_pump_ns;
    if (took >= kSlowPresentNs && !m_slow_present_logged) {
        m_slow_present_logged = true;
        rt_log_warn("gs", "the GS consumer spent %.1f s inside one present. That is a driver or"
                     " display stall, not a slow frame: the picture is frozen for as long as it"
                     " lasts. The backend, device and graphics API are on the \"GS backend:\" and"
                     " \"Renderer:\" lines earlier in this log. Said once per run.",
              double(took) / 1e9);
    }
    rt_run_note_gs_worker("between records");
}

uint64_t ThreadedBackend::repeat_wait_ns() const {
    if (!(m_present_rate > 0.0)) return UINT64_MAX;
    const uint64_t interval = (uint64_t)(1e9 / m_present_rate);
    const uint64_t since = now_ns() - m_last_pump_ns;
    return since >= interval ? 0 : interval - since;
}

void ThreadedBackend::worker_main() {
    /* Named before anything else it does: a crash report and the end-of-run
     * summary both say which thread they are on, and "GS ring worker" is
     * the difference between reading the renderer and reading the EE side
     * (host/run_state.cpp). */
    rt_thread_set_name("GS ring worker");
    /* Per thread, and this is the other thread in the process that runs
     * driver code deep enough to overflow a stack (runtime.h). */
    rt_crash_reserve_stack();
    while (!m_worker_started.load(std::memory_order_acquire)) std::this_thread::yield();
    /* Registers this thread with whatever the inner backend needs it
     * registered with before the first replay (Granite's thread-index table
     * for the live one). */
    m_inner->bind_consumer_thread();
    for (;;) {
        drain();
        if (m_shutdown.load(std::memory_order_acquire) &&
            m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_relaxed)) {
            break;
        }
        const uint64_t t0 = now_ns();
        /* Time spent presenting from inside this park, so it lands on the
         * worker's present line rather than being reported as idle. */
        uint64_t pump_ns = 0;
        std::unique_lock<std::mutex> lk(m_mu);
        m_consumer_waiting.store(true, std::memory_order_relaxed);
        rt_run_note_gs_worker("parked on an empty ring");
        /* Pairs with the fence in wake_consumer; between publishing the flag
         * and re-testing the ring, which is what makes a producer that
         * commits right now either see this flag or be seen by this test. */
        std::atomic_thread_fence(std::memory_order_seq_cst);
        while (m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_relaxed) &&
               !m_shutdown.load(std::memory_order_acquire)) {
            /* With display.present_rate off, repeat_wait_ns is UINT64_MAX and
             * this is the 2 ms poll it has always been. With a rate set, the
             * step shortens to whatever is left of the repeat interval, so
             * the repeat lands on the interval instead of on the next 2 ms
             * boundary: at 144 Hz that is three 2 ms steps and then a 0.9 ms
             * one, and the presents come out 6.9 ms apart rather than 8. The
             * 2 ms cap stays because it is the ceiling on how long a lost
             * wakeup can delay this thread (see the Wakeups section above). */
            auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(kWaitStep);
            const uint64_t left = repeat_wait_ns();
            const auto left_d = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(left < (uint64_t)INT64_MAX ? left : (uint64_t)INT64_MAX));
            if (left_d < step) step = left_d;
            m_cv_consumer.wait_for(lk, step);
            if (repeat_wait_ns() != 0) continue;
            /* The interval expired with the ring still empty. Present
             * outside the mutex: it is the whole swapchain path, and holding
             * m_mu across it would make every producer commit wait on a
             * present, which is the coupling this thread exists to remove.
             * The waiting flag goes down for the same reason it is up here
             * at all, so a producer that commits during the present wakes
             * nobody and is seen by the loop test on the way back round. */
            m_consumer_waiting.store(false, std::memory_order_relaxed);
            lk.unlock();
            const uint64_t p0 = now_ns();
            pump_present();
            pump_ns += now_ns() - p0;
            lk.lock();
            m_consumer_waiting.store(true, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }
        m_consumer_waiting.store(false, std::memory_order_relaxed);
        lk.unlock();
        m_worker_present_ns.fetch_add(pump_ns, std::memory_order_relaxed);
        m_worker_idle_ns.fetch_add(now_ns() - t0 - pump_ns, std::memory_order_relaxed);
    }
    m_worker_exited.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(m_mu); }
    m_cv_producer.notify_all();
}

void ThreadedBackend::quiesce() {
    if (m_mode != Mode::Worker) return;
    if (std::this_thread::get_id() == m_worker_id) {
        /* Reached when a fatal was raised on the worker itself: rt_fatal
         * calls std::exit, which runs gs_select.cpp's atexit handler, which
         * lands here on the worker's own stack. Joining itself would
         * deadlock, and the inner backend is one frame below on this very
         * stack, so there is nothing safe to tear down. */
        abandon_worker("the GS worker thread is exiting through its own fatal");
    }
    m_shutdown.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(m_mu); }
    m_cv_consumer.notify_all();

    const auto deadline = RingClock::now() + kJoinDeadline;
    while (!m_worker_exited.load(std::memory_order_acquire)) {
        if (RingClock::now() >= deadline) {
            /* The worker cannot come back: parked inside Granite with an
             * unusable swapchain (a window closed while minimized), or stuck
             * in a driver call. Waiting longer would hang the exit, and
             * destroying the Vulkan device under a thread still inside it
             * would fault. */
            abandon_worker("the GS worker thread did not stop within one second");
        }
        /* Pumping matters here: a worker parked on a minimized window is
         * waiting for a restore or a close event only this thread can
         * deliver. Dispatch stays held for the same reason it is in
         * wait_step, and because this runs from an atexit handler, where the
         * UI may already be torn down. */
        rt_window_hold_event_dispatch(true);
        rt_window_pump();
        rt_window_hold_event_dispatch(false);
        std::unique_lock<std::mutex> lk(m_mu);
        m_progress_waiting.fetch_add(1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst); /* see wake_producer */
        if (!m_worker_exited.load(std::memory_order_acquire)) {
            m_cv_producer.wait_for(lk, kWaitStep);
        }
        m_progress_waiting.fetch_sub(1, std::memory_order_relaxed);
    }
    m_worker.join();
    /* Back to a single thread: anything enqueued from here (the last drain in
     * the destructor, a stray call from another atexit handler) replays on
     * this thread again. */
    m_mode = Mode::Inline;
    rt_log_info("gs", "GS worker thread joined: %llu records replayed, %llu bytes still queued",
           (unsigned long long)m_records_replayed.load(std::memory_order_relaxed),
           (unsigned long long)(m_head.load(std::memory_order_relaxed) -
                                m_tail.load(std::memory_order_relaxed)));
}

void ThreadedBackend::abandon_worker(const char* why) {
    /* Tell the other thread, whichever it is, that this consumer is done:
     * an EE thread waiting on a field the worker will never finish then
     * fatals with that reason instead of spinning, in the moments before
     * this call ends the process. */
    m_worker_exited.store(true, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    { std::lock_guard<std::mutex> lk(m_mu); }
    m_cv_producer.notify_all();
    /* The status a fatal already in progress was going to exit with, or 0
     * for the quiesce timeout, which is not itself a failure: the window is
     * gone and the run is over, only the tidy teardown is lost. Exiting 0
     * out of a worker-side fatal would report success for a crash. */
    const int code = rt_fatal_exit_code();
    rt_log_info("gs", "GS ring: %s; ending the process (status %d) without the backend teardown"
                 " (no Vulkan wait-idle and no pipeline cache write this run)",
           why, code >= 0 ? code : 0);
    /* _Exit runs no atexit handler, so the end-of-run summary has to be
     * written here: this is one of the two paths in the whole process that
     * leaves without the atexit chain, and it is the one taken when the GS
     * side is what went wrong. Idempotent, so a fatal that already wrote it
     * loses nothing. A quiesce timeout is not itself a failure (the window
     * is gone and only the tidy teardown is lost), which is what the status
     * says here. */
    rt_run_set_exit_reason(code >= 0 ? false : true, "%s", why);
    rt_run_summary();
    /* The log writer is another thread and _Exit runs no atexit handler, so
     * the queue has to be pushed out here or the lines above are lost. */
    rt_log_drain();
    std::_Exit(code >= 0 ? code : 0);
}

/* ---- consumer side ------------------------------------------------------ */

void ThreadedBackend::post_reply(uint64_t seq, uint64_t value) {
    m_reply_value.store(value, std::memory_order_relaxed);
    /* Release: the value above must be visible to the producer before the
     * sequence number that says it is there. */
    m_reply_seq.store(seq, std::memory_order_release);
    if (m_mode == Mode::Worker) wake_producer();
}

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
        m_last_presented.store(m_inner->vsync(unsigned(uint32_t(arg))),
                               std::memory_order_relaxed);
        /* Release: publishes this field's completion to the EE's field_sync,
         * after everything the vsync did. */
        m_vsync_done.store(m_vsync_seen, std::memory_order_release);
        /* Published before the present, not after it. The inner backend's
         * vsync() latches the finished field and presents nothing
         * (gs_parallel_api.h), so by here the scanout the EE is waiting on
         * is done and field_sync() can return. The present that follows is
         * this thread's alone; the EE never waits on a swapchain again. */
        if (m_mode == Mode::Worker) {
            /* Worker mode only, like the two nanosecond counters drain()
             * keeps: this is the divisor they are reported against, so a
             * field counted here without its cost counted there would
             * understate both. */
            m_worker_fields.fetch_add(1, std::memory_order_relaxed);
            wake_producer();
        }
        /* The new field goes up at once rather than waiting for the repeat
         * interval: the rate is a floor on how often the window is
         * refreshed, not a ceiling on the guest's pictures. */
        pump_present();
        break;
    }
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
    case kSetWideAspect: {
        double aspect = 0.0;
        std::memcpy(&aspect, &arg, sizeof(aspect));
        m_inner->set_widescreen_aspect(aspect);
        break;
    }
    case kSetPresentRate: {
        double hz = 0.0;
        std::memcpy(&hz, &arg, sizeof(hz));
        m_present_rate = hz;
        break;
    }
    case kOverlayTexDestroy:
        m_inner->overlay_texture_destroy(uint32_t(arg));
        break;
    case kOverlayTexCreate: {
        TexPayload pl;
        std::memcpy(&pl, payload, sizeof(pl));
        post_reply(arg, m_inner->overlay_texture_create(
                            pl.has_data ? payload + sizeof(pl) : nullptr, pl.width, pl.height));
        break;
    }
    case kPresentUi:
        post_reply(arg, m_inner->present_ui());
        break;
    case kRequestShot:
        m_inner->request_screenshot(uint32_t(arg));
        break;
    case kOverlaySetFrame: {
        if (arg == 0) {
            m_inner->overlay_set_frame(nullptr);
            break;
        }
        FramePayload head;
        std::memcpy(&head, payload, sizeof(head));
        const uint32_t vn = (head.arrays & kFrameHasVertices) ? head.vertex_count : 0u;
        const uint32_t in = (head.arrays & kFrameHasIndices) ? head.index_count : 0u;
        const uint32_t cn = (head.arrays & kFrameHasCmds) ? head.cmd_count : 0u;
        const FrameLayout l = frame_layout(vn, in, cn);
        RtPgsOverlayFrame frame = {};
        frame.vertex_count = head.vertex_count;
        frame.index_count = head.index_count;
        frame.cmd_count = head.cmd_count;
        frame.surface_width = head.surface_width;
        frame.surface_height = head.surface_height;
        /* Pointers into the ring: valid for the duration of the call, which
         * is all rt_pgs_overlay_set_frame needs (it deep-copies). The tail
         * is not advanced past this record until the call returns. */
        frame.vertices = (head.arrays & kFrameHasVertices)
                             ? reinterpret_cast<const RtPgsOverlayVertex*>(payload + l.vertices_off)
                             : nullptr;
        frame.indices = (head.arrays & kFrameHasIndices)
                            ? reinterpret_cast<const uint32_t*>(payload + l.indices_off)
                            : nullptr;
        frame.cmds = (head.arrays & kFrameHasCmds)
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
    /* Identity first: m_in_drain is a plain bool owned by whichever thread is
     * the consumer, so reading it from any other thread is the race this
     * check exists to report. */
    if (m_mode == Mode::Worker && std::this_thread::get_id() != m_worker_id) {
        rt_fatal("gs", nullptr,
                 "GS ring: drain() called off the worker thread; the consumer is single by "
                 "contract (see gs_threaded.h)");
    }
    if (m_in_drain) return;
    m_in_drain = true;
    /* One clock reading per record, not two: the end of one record is the
     * start of the next. About 60 ns each on the measured host, so under
     * 0.15 ms per field at 2000 packets, and on the worker's own budget.
     * Worker mode only, so the launcher's inline drain costs exactly what it
     * did before this instrument existed. */
    const bool timed = m_mode == Mode::Worker;
    uint64_t tail = m_tail.load(std::memory_order_relaxed);
    const uint64_t head = m_head.load(std::memory_order_acquire);
    uint64_t t_prev = (timed && tail != head) ? now_ns() : 0;
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
            m_records_replayed.fetch_add(1, std::memory_order_relaxed);
            /* Two relaxed stores per record, read by the field watchdog and
             * by the end-of-run summary from another thread entirely
             * (host/run_state.cpp). kind_name returns a string literal, so
             * storing the pointer is safe across threads. This is what makes
             * "the consumer stopped inside a Gif record with 8 MB queued"
             * sayable at all: pulling the same state would mean calling into
             * a consumer that is, by hypothesis, stuck. */
            rt_run_note_gs_record(kind_name(h.kind));
            rt_run_note_gs_worker("inside a record");
            replay(h.kind, h.arg, rec + sizeof(RecHeader));
            rt_run_note_gs_queued(head - tail, m_records_replayed.load(std::memory_order_relaxed));
        }
        /* Published only after the record has been consumed: the payload is
         * still the producer's to overwrite the moment the tail moves past
         * it. */
        tail += h.size;
        m_tail.store(tail, std::memory_order_release);
        if (timed) {
            const uint64_t t = now_ns();
            if (h.kind == kVsync) {
                m_worker_present_ns.fetch_add(t - t_prev, std::memory_order_relaxed);
            } else {
                m_worker_gs_ns.fetch_add(t - t_prev, std::memory_order_relaxed);
            }
            t_prev = t;
            /* Only when the EE is actually blocked on room, which is the rare
             * case this counter exists to keep cheap. Relaxed and unfenced on
             * purpose: a fence per record would cost more than the 2 ms a
             * missed check can delay a stalled producer by, and the check
             * after the loop is what actually closes the handshake. */
            if (m_space_waiting.load(std::memory_order_relaxed) != 0) wake_producer();
        }
    }
    m_in_drain = false;
    if (m_mode == Mode::Worker) {
        /* The fenced half of the check above, once per batch rather than
         * once per record. Without it the last record of a batch has no
         * next record to catch a producer that published its wait in the
         * same instant -- an unfenced store-then-load is exactly the pattern
         * the Wakeups section of this file's header says does not work --
         * and that producer would sit out its full 2 ms step with the room
         * it asked for already free. */
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (m_space_waiting.load(std::memory_order_relaxed) != 0) wake_producer();
    }
}

GsBackend* rt_gs_make_threaded_backend(GsBackend* inner) {
    return new ThreadedBackend(inner);
}
