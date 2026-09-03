/* gs/gs_threaded.h: the GS command ring, the backend wrapper that fills it,
 * and the worker thread that drains it.
 *
 * ThreadedBackend is a GsBackend that wraps whichever backend gs_select.cpp
 * built (dump writer, live paraLLEl-GS, or the tee over both) and encodes
 * every call into a single-producer single-consumer byte ring instead of
 * forwarding it. The record format, the copy policy and the wrap rule are
 * documented in gs_threaded.cpp's file header.
 *
 * Three drain modes, one ring:
 *
 *   Inline   every wrapper method commits its record and then replays it on
 *            the caller's own thread. The program is single threaded and
 *            behaves exactly as it did without the wrapper. This is what the
 *            launcher runs on, and what a run stays on until
 *            rt_gs_backend_start_worker() is called.
 *   Manual   nothing is replayed until drain() is called by hand. Only
 *            gs/gs_ring_selftest.cpp uses it: it is the only way to get
 *            several records in flight and exercise the wrap marker with a
 *            non-empty ring.
 *   Worker   a thread owns drain(). The producer commits and wakes it; the
 *            EE thread only waits for it at the sync points below.
 *
 * Threading contract (Worker mode):
 *   - Exactly one producer, the EE thread, which is also the process's main
 *     thread (guest threads are coroutines on it) and therefore the only
 *     thread allowed to touch SDL.
 *   - Exactly one consumer, the worker, which is the only thread that calls
 *     into the inner backend after start_worker() returns. Everything the
 *     library does per field (packet parse, flush, scanout, present, overlay
 *     upload) happens there.
 *   - The EE waits on the worker in exactly three places: field_sync() (one
 *     field in flight), the two calls that need a value back
 *     (overlay_texture_create, present_ui), and a full ring. Every one of
 *     those waits pumps the window, because the worker can be parked inside
 *     Granite's block_until_wsi_forward_progress waiting for a restore event
 *     that only the EE-side pump can deliver.
 *   - Values the EE reads back from the backend never cross the ring:
 *     read_priv is answered from this side's shadow, and the live backend's
 *     present rectangle, present timings and window-closed flag are atomics
 *     (or published under a mutex) on the library side.
 */
#ifndef ICORECOMP_GS_THREADED_H
#define ICORECOMP_GS_THREADED_H

#include "gs_backend.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

class ThreadedBackend final : public GsBackend {
public:
    /* 32 MB. Sized for a worst-case field's worth of GIF payload plus slack,
     * so the producer never has to wait in practice; allocated once at
     * construction and never grown. Must be a power of two. */
    static constexpr size_t kDefaultRingBytes = 32u * 1024 * 1024;

    /* Takes ownership of `inner`. `ring_bytes` must be a power of two of at
     * least 4 KB. `inline_drain` false leaves records in the ring until
     * drain() is called by hand (Manual mode); only the selftest does that. */
    explicit ThreadedBackend(GsBackend* inner,
                             size_t ring_bytes = kDefaultRingBytes,
                             bool inline_drain = true);
    ~ThreadedBackend() override;

    /* GsBackend: encoded into the ring. */
    void submit_gif(int path, const uint8_t* data, uint32_t qwords) override;
    void write_priv(uint32_t offset, uint64_t v) override;
    bool vsync(unsigned field) override;
    void set_presentation(uint32_t fit, uint32_t filter) override;
    void set_present_mode(uint32_t mode) override;
    void set_render_scale(uint32_t factor) override;
    void set_raster(uint32_t raster) override;
    void set_deinterlace(uint32_t deinterlace) override;
    void overlay_set_frame(const RtPgsOverlayFrame* frame) override;
    void overlay_texture_destroy(uint32_t texture) override;

    /* Answered from this side's own shadow; the inner backend is not
     * consulted. See gs_threaded.cpp for why that is exact. */
    uint64_t read_priv(uint32_t offset) override;

    /* Need a value back. They ride the ring like everything else, as a record
     * carrying a call sequence number; the producer then waits, pumping the
     * window, until the consumer has posted the reply for that sequence. In
     * Inline and Manual mode the drain that precedes the wait is what posts
     * it, so the wait returns without blocking. */
    uint32_t overlay_texture_create(const uint8_t* rgba8, uint32_t width, uint32_t height) override;
    uint32_t present_ui() override;

    /* The one per-field sync point (hw/gspriv.cpp): returns once the consumer
     * has finished every vsync but the most recent one, so at most one field
     * is in flight. Pumps the window while it waits, and gives up early if
     * the window has closed. A no-op outside Worker mode. */
    void field_sync() override;

    /* Asked by the EE after field_sync(); answered by the inner backend
     * (an atomic on the live one), so it is true even while the consumer is
     * parked inside the library and cannot return a vsync result. */
    bool window_closed() override;

    /* Stops the consumer thread after it has replayed everything committed so
     * far, and joins it. Idempotent; called by gs_select.cpp's atexit handler
     * before report_stats() so the statistics below are final, and again by
     * the destructor. */
    void quiesce() override;

    /* Straight through: neither touches GS state, and report_stats runs at
     * exit with the worker already joined. */
    void report_stats() override;
    void present_timings(uint64_t* flush_ns, uint64_t* scanout_ns,
                         uint64_t* present_ns, uint64_t* fields) override;

    /* This wrapper's own consumer-side costs. See gs_backend.h. Reading
     * clears. */
    void consumer_timings(RtGsConsumerTimings* out) override;

    /* Moves from Inline to Worker mode: spawns the consumer thread. Called
     * once, from rt_gs_backend_start_worker() (gs_select.cpp) after the
     * launcher has handed over, so the whole launcher phase runs single
     * threaded exactly as it did before this thread existed. */
    void start_worker();

    /* Replays every committed record into the inner backend and advances the
     * tail. Public for the selftest and for the worker loop; calling it from
     * any thread but the consumer once the worker is up is fatal. */
    void drain();

    /* Records committed and replayed since construction; the selftest and
     * report_stats() use them. */
    uint64_t records_written() const { return m_records_written; }
    uint64_t records_replayed() const { return m_records_replayed.load(std::memory_order_relaxed); }
    uint64_t wrap_markers() const { return m_wrap_markers; }
    uint64_t back_pressure_stalls() const { return m_stalls; }
    uint64_t bytes_written() const { return m_head.load(std::memory_order_relaxed); }

private:
    enum class Mode { Inline, Manual, Worker };

    uint8_t* begin_record(uint32_t kind, uint64_t arg, uint64_t payload_bytes);
    void commit();
    bool ensure_space(uint64_t bytes);
    void back_pressure(uint64_t bytes);
    uint64_t await_reply(uint64_t seq);
    void post_reply(uint64_t seq, uint64_t value);
    bool closed_window_timeout(uint64_t wait_started_ns) const;
    void wake_consumer();
    void wake_producer();
    /* One bounded wait on the consumer's progress, then one pump of the
     * window with event dispatch held (see wait_step's own comment). */
    void wait_step(std::unique_lock<std::mutex>& lk);
    void worker_main();
    void replay(uint32_t kind, uint64_t arg, const uint8_t* payload);
    [[noreturn]] void abandon_worker(const char* why);

    GsBackend* m_inner = nullptr;
    uint8_t* m_buf = nullptr;
    size_t m_capacity = 0;
    uint64_t m_mask = 0;
    Mode m_mode = Mode::Inline;

    /* Monotonic byte counters, never wrapped: the buffer offset is
     * counter & m_mask, and head - tail is the number of live bytes. */
    std::atomic<uint64_t> m_head{0};
    std::atomic<uint64_t> m_tail{0};
    uint64_t m_reserved_head = 0; /* producer-private: head after the record being filled */

    /* Worker plumbing. m_mu guards nothing but the two condition variables:
     * the ring itself is lock free. What makes a wakeup impossible to lose is
     * not the mutex but a seq_cst fence on both sides of the store-then-load
     * between each waiter's flag and the word it is waiting on; see the
     * Wakeups section of gs_threaded.cpp and wake_consumer(). */
    std::thread m_worker;
    /* Written by start_worker() before m_worker_started is set and read by
     * the worker after it, so the worker's own drain() can check that it is
     * the consumer without racing the assignment. */
    std::thread::id m_worker_id;
    std::atomic<bool> m_worker_started{false};
    std::mutex m_mu;
    std::condition_variable m_cv_consumer; /* worker waits for records */
    std::condition_variable m_cv_producer; /* EE waits for progress */
    std::atomic<bool> m_consumer_waiting{false};  /* worker parked on an empty ring */
    /* How many producer-side waits are outstanding, not whether one is:
     * the event pump a wait runs can enqueue and wait again (a settings
     * applier, an overlay texture from a UI event), and a bool would be
     * cleared by the inner wait while the outer still needs waking. */
    std::atomic<uint32_t> m_progress_waiting{0};  /* EE waiting for a vsync or a reply */
    std::atomic<uint32_t> m_space_waiting{0};     /* EE waiting for ring space */
    std::atomic<bool> m_shutdown{false};
    std::atomic<bool> m_worker_exited{false};

    /* Reentrancy guard for drain(). Two distinct cases:
     *
     *   Inline mode: an exit from inside a replayed call lands in the
     *   destructor with this still set (std::exit does not unwind), so the
     *   destructor's drain is a no-op instead of replaying the same record
     *   forever.
     *
     *   Worker mode: only the worker ever sets it, and the destructor's
     *   equivalent test is the thread identity check in abandon_worker: a
     *   fatal raised on the worker reaches ~ThreadedBackend on the worker's
     *   own stack, where joining itself would deadlock and destroying the
     *   inner backend would free it from inside one of its own calls.
     *
     * Its other consequence in Inline mode: a record the inner backend
     * enqueues from inside a replayed call (the event pump runs from inside
     * ParallelBackend::vsync) is committed but not replayed until the next
     * drain. back_pressure() fatals if such a caller ever fills the ring,
     * because drain() could not free it. In Worker mode there is no such
     * restriction: a settings applier that runs from the pump enqueues a
     * record like any other and the worker replays it in order, between
     * frames, which is exactly where the library requires it. */
    bool m_in_drain = false;

    /* EE-side privileged register shadow, one 64-bit value per 16-byte
     * register slot across the 0x2000-byte block, indexed
     * (offset & 0x1FFF) >> 4. The dump writer and RtPgs hold the same
     * last-written value per slot in two banks rather than one flat array,
     * so the index expression differs and the mapping does not; see the
     * comment on read_priv in gs_threaded.cpp. */
    static constexpr uint32_t kPrivSlots = 0x2000 / 16;
    uint64_t m_priv[kPrivSlots] = {};

    uint64_t m_vsync_seq = 0;                /* producer: vsyncs enqueued */
    std::atomic<uint64_t> m_vsync_done{0};   /* consumer: vsyncs completed */
    uint64_t m_vsync_seen = 0;               /* consumer: order check */
    uint64_t m_call_seq = 0;                 /* producer: reply-carrying records */
    std::atomic<uint64_t> m_reply_seq{0};    /* consumer: last reply posted */
    std::atomic<uint64_t> m_reply_value{0};
    std::atomic<bool> m_last_presented{false};

    /* One-shot: the field sync point says once that it has been waiting for
     * seconds on a live window, and does not repeat it every field
     * afterwards. */
    bool m_field_sync_stuck_logged = false;

    uint64_t m_records_written = 0;
    std::atomic<uint64_t> m_records_replayed{0};
    uint64_t m_wrap_markers = 0;
    uint64_t m_stalls = 0;

    /* Consumer-side profile counters, published for the EE's profile summary
     * (prof.h's "gs worker" line). Plain atomics rather than RT_PROF_ZONE:
     * prof.h is single threaded by construction. */
    std::atomic<uint64_t> m_worker_gs_ns{0};
    std::atomic<uint64_t> m_worker_present_ns{0};
    std::atomic<uint64_t> m_worker_idle_ns{0};
    std::atomic<uint64_t> m_worker_fields{0};
    std::atomic<uint64_t> m_ee_wait_ns{0};
    std::atomic<uint64_t> m_ee_waits{0};
};

/* gs_select.cpp's factory: wraps `inner` unless ICORECOMP_GS_THREAD=0. */
GsBackend* rt_gs_make_threaded_backend(GsBackend* inner);

#endif /* ICORECOMP_GS_THREADED_H */
