/* gs/gs_threaded.h: the GS command ring and the backend wrapper that fills
 * it.
 *
 * ThreadedBackend is a GsBackend that wraps whichever backend gs_select.cpp
 * built (dump writer, live paraLLEl-GS, or the tee over both) and encodes
 * every call into a single-producer single-consumer byte ring instead of
 * forwarding it. The record format, the copy policy and the wrap rule are
 * documented in gs_threaded.cpp's file header.
 *
 * Today the consumer is the producer: every wrapper method commits its
 * record and then calls drain() on the same thread, so the program stays
 * single threaded and behaves exactly as it did without the wrapper. The
 * point of the ring existing first is that moving the consumer onto a
 * worker thread later replaces one call (drain() at commit time) with a
 * condvar wake, and nothing else.
 *
 * The class is declared here rather than kept private to the .cpp so
 * gs/gs_ring_selftest.cpp (icorecomp-gsring-selftest) can build one over a
 * deliberately small ring with inline draining turned off, which is the only
 * way to get several records in flight and exercise the wrap marker with a
 * non-empty ring.
 */
#ifndef ICORECOMP_GS_THREADED_H
#define ICORECOMP_GS_THREADED_H

#include "gs_backend.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

class ThreadedBackend final : public GsBackend {
public:
    /* 32 MB. Sized for a worst-case field's worth of GIF payload plus slack,
     * so the producer never has to wait in practice; allocated once at
     * construction and never grown. Must be a power of two. */
    static constexpr size_t kDefaultRingBytes = 32u * 1024 * 1024;

    /* Takes ownership of `inner`. `ring_bytes` must be a power of two of at
     * least 4 KB. `inline_drain` false leaves records in the ring until
     * drain() is called by hand; only the selftest does that. */
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

    /* Need a value back, so they drain the ring to a fence and then call the
     * inner backend directly. Step 5 turns this into an RPC record with a
     * reply slot; the fence record already marks the point. */
    uint32_t overlay_texture_create(const uint8_t* rgba8, uint32_t width, uint32_t height) override;
    uint32_t present_ui() override;

    /* Straight through: neither touches GS state, and both are read by the
     * profile summary and atexit on the producer's own thread. */
    void report_stats() override;
    void present_timings(uint64_t* flush_ns, uint64_t* scanout_ns,
                         uint64_t* present_ns, uint64_t* fields) override;

    /* Replays every committed record into the inner backend and advances the
     * tail. Public for the selftest and for step 5's worker loop. */
    void drain();

    /* Records committed and replayed since construction; the selftest and
     * report_stats() use them. */
    uint64_t records_written() const { return m_records_written; }
    uint64_t records_replayed() const { return m_records_replayed; }
    uint64_t wrap_markers() const { return m_wrap_markers; }
    uint64_t back_pressure_stalls() const { return m_stalls; }
    uint64_t bytes_written() const { return m_head.load(std::memory_order_relaxed); }

private:
    uint8_t* begin_record(uint32_t kind, uint64_t arg, uint64_t payload_bytes);
    void commit();
    bool ensure_space(uint64_t bytes);
    void back_pressure(uint64_t bytes);
    void sync_point();
    void replay(uint32_t kind, uint64_t arg, const uint8_t* payload);

    GsBackend* m_inner = nullptr;
    uint8_t* m_buf = nullptr;
    size_t m_capacity = 0;
    uint64_t m_mask = 0;
    bool m_inline_drain = true;

    /* Monotonic byte counters, never wrapped: the buffer offset is
     * counter & m_mask, and head - tail is the number of live bytes. */
    std::atomic<uint64_t> m_head{0};
    std::atomic<uint64_t> m_tail{0};
    uint64_t m_reserved_head = 0; /* producer-private: head after the record being filled */

    /* Reentrancy guard for drain(). ParallelBackend::vsync() calls
     * std::exit() when the window closes, which runs gs_select.cpp's atexit
     * handler and destroys this object from inside replay(). The guard
     * stays set (std::exit does not unwind), so the destructor's drain is a
     * no-op instead of replaying the same vsync record forever.
     *
     * Its other consequence: a record the inner backend enqueues from inside
     * a replayed call (the event pump runs from inside
     * ParallelBackend::vsync) is committed but not replayed until the next
     * drain, so such a call is deferred past the frame instead of landing
     * inside it. Nothing does that today; host/settings_apply.cpp's display
     * appliers are the only candidates, and the only rt_settings_commit()
     * reachable from the pump (ui/ui_rebind.cpp) changes input.* keys, which
     * that file does not push to the GS. back_pressure() fatals if such a
     * caller ever fills the ring, because drain() could not free it. */
    bool m_in_drain = false;

    /* EE-side privileged register shadow, one 64-bit value per 16-byte
     * register slot across the 0x2000-byte block, indexed
     * (offset & 0x1FFF) >> 4. The dump writer and RtPgs hold the same
     * last-written value per slot in two banks rather than one flat array,
     * so the index expression differs and the mapping does not; see the
     * comment on read_priv in gs_threaded.cpp. */
    static constexpr uint32_t kPrivSlots = 0x2000 / 16;
    uint64_t m_priv[kPrivSlots] = {};

    uint64_t m_vsync_seq = 0;   /* producer side */
    uint64_t m_vsync_seen = 0;  /* consumer side; must match on replay */
    uint64_t m_fence_seq = 0;
    bool m_last_presented = false;

    uint64_t m_records_written = 0;
    uint64_t m_records_replayed = 0;
    uint64_t m_wrap_markers = 0;
    uint64_t m_stalls = 0;
};

/* gs_select.cpp's factory: wraps `inner` unless ICORECOMP_GS_THREAD=0. */
GsBackend* rt_gs_make_threaded_backend(GsBackend* inner);

#endif /* ICORECOMP_GS_THREADED_H */
