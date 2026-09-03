/* gs/gs_backend.h: narrow interface between the EE-side graphics transport
 * (hw/dmac, hw/vif1, hw/gif, hw/gspriv) and a GS implementation.
 *
 * Implementations:
 *   gs_dumpwriter.cpp  records the GIF/priv-register traffic in the
 *                      paraLLEl-GS raw stream format (and acts as the
 *                      register shadow when no dump path is configured).
 *   gs_parallel.cpp    adapter over libicorecomp-parallel-gs's C ABI
 *                      (gs_parallel_api.h); paraLLEl-GS itself renders live
 *                      on a Vulkan device inside that shared library and
 *                      presents per field.
 *   gs_select.cpp      rt_gs_backend() singleton: picks an implementation
 *                      from ICORECOMP_GS=dump|parallel|both ("both" tees
 *                      every call to the live backend and the dump writer).
 *   gs_threaded.cpp    ThreadedBackend, the wrapper gs_select.cpp puts in
 *                      front of whichever of the above it built: it encodes
 *                      every call below into a byte ring and replays it into
 *                      the inner backend. ICORECOMP_GS_THREAD=0 leaves it
 *                      out.
 *
 * CSR/IMR ownership: ee/intc.cpp owns the CSR event flags, FIELD bit and
 * IMR semantics because interrupt delivery depends on them. The backend
 * only receives CSR as an opaque priv write (snapshotted at vsync) so the
 * dump stays coherent. See hw/gspriv.cpp.
 */
#ifndef ICORECOMP_GS_BACKEND_H
#define ICORECOMP_GS_BACKEND_H

#include <cstdint>

/* Overlay geometry for the settings menu and the launcher, defined by the
 * paraLLEl-GS C ABI (gs_parallel_api.h). Only a pointer to it crosses this
 * interface, so the tag declaration is enough here and every includer of
 * this header stays free of that ABI. gs_parallel.cpp, gs_threaded.cpp and
 * the UI include gs_parallel_api.h themselves for the layout. */
struct RtPgsOverlayFrame;

/* Consumer-side cost of a backend that has a worker thread, since the last
 * read; all zero for one that does not. Filled by consumer_timings() below
 * and reported by the profile summary (prof.h) as its own "gs worker" line,
 * because once the ring is drained by a worker the EE's own "gs" and
 * "present" buckets hold only the cost of enqueueing.
 *
 *   gs_ns       replaying everything but a vsync record: packet parse,
 *               privileged writes, overlay uploads
 *   present_ns  replaying vsync records: flush, scanout, swapchain present
 *   idle_ns     parked with an empty ring, which is the worker's headroom
 *   fields      vsync records replayed, so the three above can be per field
 *   ee_wait_ns  time the EE thread spent waiting on the consumer (the field
 *               sync point, a reply, or a full ring), and ee_waits how many
 *               waits that was. This is what the thread costs the EE.
 */
struct RtGsConsumerTimings {
    uint64_t gs_ns;
    uint64_t present_ns;
    uint64_t idle_ns;
    uint64_t fields;
    uint64_t ee_wait_ns;
    uint64_t ee_waits;
};

class GsBackend {
public:
    virtual ~GsBackend() = default;

    /* path: 0 = PATH1, 1 = PATH2, 2 = PATH3. data is qwords*16 bytes of
     * GIF-tagged packet data (framing already validated by hw/gif.cpp). */
    virtual void submit_gif(int path, const uint8_t* data, uint32_t qwords) = 0;

    /* offset: byte offset into the 0x12000000 privileged block (0..0x1FF0).
     * The backend keeps the value as the readable shadow. */
    virtual void write_priv(uint32_t offset, uint64_t v) = 0;
    virtual uint64_t read_priv(uint32_t offset) = 0;

    /* End of field. Returns true when a frame's worth of traffic was
     * presented (any GIF transfer landed since the previous vsync). */
    virtual bool vsync(unsigned field) = 0;

    /* End-of-run statistics dump (atexit; see gs_select.cpp), called with any
     * consumer thread already stopped (see quiesce). */
    virtual void report_stats() {}

    /* ---- consumer thread ------------------------------------------------
     *
     * Only gs_threaded.cpp's ThreadedBackend has one; every other backend
     * runs on its caller's thread and takes the defaults here.
     *
     * field_sync() is the EE's one per-field wait: it returns when the
     * consumer has finished every vsync but the most recent, so at most one
     * field is in flight. hw/gspriv.cpp calls it right after vsync().
     *
     * window_closed() is the exit condition the EE polls after that wait.
     * The live backend sets it from the RT_PGS_VSYNC_WINDOW_CLOSED bit and
     * from the library's own window state, so it is true even while the
     * consumer is parked inside the library with no vsync to return.
     * hw/gspriv.cpp owns the policy: it logs and exits, on the EE thread,
     * so process teardown runs where it always did.
     *
     * quiesce() stops and joins the consumer after everything committed so
     * far has been replayed. Idempotent. Nothing may be enqueued after it.
     *
     * bind_consumer_thread() runs once on the consumer thread before the
     * first record is replayed, for a backend that needs the thread
     * registered with something (the live one registers it with Granite's
     * thread-index table; see gs_parallel_lib.cpp). */
    virtual void field_sync() {}
    virtual bool window_closed() { return false; }
    virtual void quiesce() {}
    virtual void bind_consumer_thread() {}
    /* See RtGsConsumerTimings. Reading clears; a backend with no consumer
     * thread leaves the caller's struct untouched, so callers zero it. */
    virtual void consumer_timings(RtGsConsumerTimings* /*out*/) {}

    /* Present-path decomposition for the profile summary (prof.h): the
     * nanoseconds spent flushing the renderer, scanning out and presenting
     * since the previous call, and the number of fields they cover. One
     * averaged "present" bucket cannot say which of the three a slow field
     * went to. Reading clears the counters. A backend with no present path
     * leaves the caller's values untouched, so callers zero them first. */
    virtual void present_timings(uint64_t* /*flush_ns*/, uint64_t* /*scanout_ns*/,
                                 uint64_t* /*present_ns*/, uint64_t* /*fields*/) {}

    /* ---- presentation and overlay ---------------------------------------
     *
     * These reach the same GS implementation as the calls above, so they
     * have to travel the same route: anything that bypasses the backend and
     * talks to the paraLLEl-GS library directly would be invisible to the
     * command ring, and so would land out of order once the ring is drained
     * by a worker thread. host/settings_apply.cpp and ui/ui.cpp call them
     * through rt_gs_backend() for that reason.
     *
     * The defaults are no-ops (0 for the two that return a value), which is
     * what the dump writer wants: it has no window, no swapchain and no
     * overlay pass. Only the live backend overrides them.
     *
     * Every one of them is a between-frames-only operation on the live
     * backend (gs_parallel_api.h: the library's m_in_frame guard fatals if
     * they are called from inside a present, which includes the event pump
     * callback). */
    virtual void set_presentation(uint32_t /*fit*/, uint32_t /*filter*/) {}
    virtual void set_present_mode(uint32_t /*mode*/) {}
    virtual void set_render_scale(uint32_t /*factor*/) {}
    /* display.raster and display.deinterlace (RT_PGS_RASTER_* and
     * RT_PGS_DEINTERLACE_* values, gs_parallel_api.h): stores only, read at
     * the next vsync. Routed through the backend so a queued GS worker sees
     * them in order with the fields they apply to. */
    virtual void set_raster(uint32_t /*raster*/) {}
    virtual void set_deinterlace(uint32_t /*deinterlace*/) {}

    /* Returns the texture id, or 0 when there is no overlay renderer. */
    virtual uint32_t overlay_texture_create(const uint8_t* /*rgba8*/, uint32_t /*width*/,
                                            uint32_t /*height*/) { return 0; }
    virtual void overlay_texture_destroy(uint32_t /*texture*/) {}
    /* Deep-copied by the implementation; null clears the retained frame. */
    virtual void overlay_set_frame(const RtPgsOverlayFrame* /*frame*/) {}
    /* One presented frame with no guest scanout (the launcher). Returns the
     * RT_PGS_VSYNC_* bitmask, 0 when there is nothing to present into. */
    virtual uint32_t present_ui() { return 0; }
};

/* Singleton accessor (gs_select.cpp). First call creates the backend per
 * ICORECOMP_GS; the dump flavor writes a file only when ICORECOMP_GS_DUMP
 * is set, otherwise it is the register shadow + statistics collector. */
GsBackend* rt_gs_backend();

/* The backend if rt_gs_backend() has already built it, otherwise nullptr.
 * For callers that must not be the thing that creates it: building the
 * backend opens the ICORECOMP_GS_DUMP file and, for the live flavor, the
 * Vulkan device and the window, and main.cpp's launcher gate depends on that
 * happening at rt_hw_init() and nowhere earlier. host/settings_apply.cpp
 * uses it: a settings change that arrives before the backend exists has
 * nothing to push into, exactly as it had when the guard there was
 * rt_gs_parallel_handle() != nullptr. */
GsBackend* rt_gs_backend_if_created();

/* Moves the command ring from inline draining to its worker thread. Called
 * once from main.cpp, after the launcher has handed over and before the
 * scheduler boots, so the whole launcher phase stays single threaded. A
 * no-op when the ring was bypassed (ICORECOMP_GS_THREAD=0) or when no
 * backend has been created. */
void rt_gs_backend_start_worker();

/* Factories. rt_gs_make_parallel_backend() exists only when built with
 * ICORECOMP_HAVE_PARALLEL_GS; it fatals (loudly) if no usable Vulkan device
 * is found. */
GsBackend* rt_gs_make_dump_backend(const char* dump_path);
#ifdef ICORECOMP_HAVE_PARALLEL_GS
GsBackend* rt_gs_make_parallel_backend();
#endif

#endif /* ICORECOMP_GS_BACKEND_H */
