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

#include <cstddef>
#include <cstdint>

/* Overlay geometry for the settings menu and the launcher, defined by the
 * paraLLEl-GS C ABI (gs_parallel_api.h). Only a pointer to it crosses this
 * interface, so the tag declaration is enough here and every includer of
 * this header stays free of that ABI. gs_parallel.cpp, gs_threaded.cpp and
 * the UI include gs_parallel_api.h themselves for the layout. */
struct RtPgsOverlayFrame;

/* Which RHI backend the native renderer's device is created on
 * (gs/render/gs_native.h). These are exactly the three explicit values
 * ICORECOMP_GS_BACKEND takes beside `auto` and `parallel-gs`. It lives here
 * rather than in gs_native.h so gs_select.cpp can resolve the setting in a
 * build that has no native renderer and still name what it rejected;
 * rhi::Backend is not used for it so nothing outside rhi/ includes rhi.h.
 * `auto` is resolved before this enum is reached, so there is no Auto
 * member. */
enum class RtNativeRhi { Vulkan, D3D12, Metal };

/* Present-path decomposition for the profile summary (prof.h), since the
 * last read; all zero for a backend with no present path. Filled by
 * present_timings() below.
 *
 *   flush_ns    flushing the renderer at the field boundary
 *   scanout_ns  building the scanout image
 *   present_ns  the presents themselves, repeats included
 *   fields      the vsyncs the first two cover
 *   presents    presents that happened. Not the same as fields: with
 *               display.present_rate set, the newest field is presented
 *               again between fields, so present_ns is per present and the
 *               other two are per field.
 *   repeats     of those presents, the ones that showed a picture already on
 *               screen rather than a new field
 *
 * One averaged "present" bucket cannot say which of the three a slow field
 * went to, which is why the split crosses this interface at all. */
struct RtGsPresentTimings {
    uint64_t flush_ns;
    uint64_t scanout_ns;
    uint64_t present_ns;
    uint64_t fields;
    uint64_t presents;
    uint64_t repeats;
};

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
     * presented (any GIF transfer landed since the previous vsync).
     *
     * On the live backend this no longer puts anything on screen: the field
     * is latched and present_pump() below shows it. The bool keeps its
     * meaning on every backend (RT_PGS_VSYNC_LATCHED, gs_parallel_api.h);
     * no caller acts on it, hw/gspriv.cpp ignores the result. */
    virtual bool vsync(unsigned field) = 0;

    /* Presents the newest field the backend holds, and repeats it every
     * 1/max_hz seconds while no newer one arrives. max_hz 0 means one
     * present per field and no repeats, which is what every backend did
     * before this existed. See rt_pgs_present_pump in gs_parallel_api.h for
     * what a repeat is and is not.
     *
     * Consumer-thread only, like every other call that touches the
     * swapchain. Which thread that is depends on the wiring:
     *
     *   with the GS command ring   ThreadedBackend's worker calls this on
     *                              the inner backend, once after each vsync
     *                              record and again from its park loop; the
     *                              ring itself takes the default no-op, so
     *                              the EE's call below does nothing.
     *   with the ring bypassed     ICORECOMP_GS_THREAD=0 hands out the inner
     *                              backend directly and the EE thread is the
     *                              consumer, so hw/gspriv.cpp's call at the
     *                              field boundary is the one that presents.
     *
     * hw/gspriv.cpp calls it unconditionally for that second case. The rate
     * the ring's worker uses arrives separately through set_present_rate(),
     * because it has to reach the worker in order with the fields it applies
     * to.
     *
     * With the ring bypassed there are no repeats whatever the rate says:
     * the only thread that could run one is the EE thread, and its time
     * between fields belongs to the guest. That configuration is a developer
     * bisect switch (gs_select.cpp), not a way to play. */
    virtual void present_pump(double /*max_hz*/) {}

    /* display.present_rate, pushed at the field boundary like
     * display.present. Only the command ring implements it: it is the one
     * backend whose consumer is a thread the setting cannot be read from. */
    virtual void set_present_rate(double /*max_hz*/) {}

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

    /* See RtGsPresentTimings. Reading clears the counters. A backend with no
     * present path leaves the caller's struct untouched, so callers zero it
     * first. */
    virtual void present_timings(RtGsPresentTimings* /*out*/) {}

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
    /* display.widescreen's presentation half: the aspect the finished
     * scanout is presented at, or 0 for "whatever the scanout derived from
     * the CRTC registers", which is the retail 4:3. Stored only, read at the
     * next present. Routed through the backend for the same reason
     * set_raster is: the paraLLEl-GS shim is a separate shared library and
     * cannot call guest/widescreen.cpp, and a queued GS worker has to see
     * the value in order with the fields it applies to. */
    virtual void set_widescreen_aspect(double /*aspect*/) {}

    /* Returns the texture id, or 0 when there is no overlay renderer. */
    virtual uint32_t overlay_texture_create(const uint8_t* /*rgba8*/, uint32_t /*width*/,
                                            uint32_t /*height*/) { return 0; }
    virtual void overlay_texture_destroy(uint32_t /*texture*/) {}
    /* Deep-copied by the implementation; null clears the retained frame. */
    virtual void overlay_set_frame(const RtPgsOverlayFrame* /*frame*/) {}
    /* One presented frame with no guest scanout (the launcher). Returns the
     * RT_PGS_VSYNC_* bitmask, 0 when there is nothing to present into. */
    virtual uint32_t present_ui() { return 0; }

    /* Arms one user-facing screenshot of the presented picture
     * (rt_pgs_request_screenshot; host/screenshot.cpp owns the hotkey, the
     * folder and the PNG). `slots` is 1 for the picture on its own, or
     * RT_PGS_SHOT_SLOTS for the pre/post pair the screenshot verbose channel
     * asks for.
     *
     * This is the arm only, and it does ride the ring, because it changes
     * what a present does and has to land on the field the user asked
     * for. */
    virtual void request_screenshot(uint32_t /*slots*/) {}

    /* Reads one armed slot back. `slot` is RT_PGS_SHOT_PRE or
     * RT_PGS_SHOT_POST (gs_parallel_api.h). Returns 0 when the slot holds
     * nothing ready, otherwise the image size in bytes (width * height * 4).
     * `dst` null is the size query: *w and *h are filled and nothing is
     * copied. With `dst` non-null the slot is consumed, the rows are copied
     * out tightly packed as RGBA8 from the top, and a `dst_bytes` smaller
     * than the image copies nothing, keeps the slot and returns 0. Any of
     * `w` and `h` may be null.
     *
     * Unlike the arm above, this does NOT ride the command ring, and
     * gs_threaded.cpp passes it straight through: it is a read of state the
     * present that copied the pixels already published, it needs no ordering
     * against GIF traffic, and routing it through the ring would make the
     * host wait for the worker to answer. It goes through this interface
     * rather than through a backend-specific handle so that the native
     * renderer's own capture reaches host/screenshot.cpp by the same
     * route. */
    virtual size_t take_screenshot(uint32_t /*slot*/, uint32_t* w, uint32_t* h,
                                   uint8_t* /*dst*/, size_t /*dst_bytes*/) {
        if (w) *w = 0;
        if (h) *h = 0;
        return 0;
    }
};

/* The RT_PGS_PRESENT_* value (gs/gs_parallel_api.h) every live backend is
 * created with: ICORECOMP_GS_PRESENT when it is set, otherwise
 * display.present. Resolved once on the first call and remembered, with one
 * warning if the two disagree, so the launcher (ui/ui_launcher.cpp), which
 * forces FIFO while it is up and restores this at hand-off, reads the same
 * value the backend was made with rather than deriving the mapping a second
 * time. Defined in gs_select.cpp, which is built in every configuration.
 *
 * RT_PGS_PRESENT_MAILBOX before rt_settings_init() has run, which no caller
 * does. */
uint32_t rt_gs_present_mode();

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
 * a live-backend handle being non-null. */
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
