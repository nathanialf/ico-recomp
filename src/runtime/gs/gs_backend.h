/* gs/gs_backend.h: narrow interface between the EE-side graphics transport
 * (hw/dmac, hw/vif1, hw/gif, hw/gspriv) and a GS implementation.
 *
 * Implementations:
 *   gs_dumpwriter.cpp  records the GIF/priv-register traffic in the
 *                      paraLLEl-GS raw stream format (and acts as the
 *                      register shadow when no dump path is configured).
 *   gs_parallel.cpp    paraLLEl-GS itself (third_party/parallel-gs, linked
 *                      as libicorecomp-parallel-gs.so), rendering live on a
 *                      Vulkan device and presenting per field.
 *   gs_select.cpp      rt_gs_backend() singleton: picks an implementation
 *                      from ICORECOMP_GS=dump|parallel|both ("both" tees
 *                      every call to the live backend and the dump writer).
 *
 * CSR/IMR ownership: ee/intc.cpp owns the CSR event flags, FIELD bit and
 * IMR semantics because interrupt delivery depends on them. The backend
 * only receives CSR as an opaque priv write (snapshotted at vsync) so the
 * dump stays coherent. See hw/gspriv.cpp.
 */
#ifndef ICORECOMP_GS_BACKEND_H
#define ICORECOMP_GS_BACKEND_H

#include <cstdint>

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

    /* End-of-run statistics dump (atexit; see gs_select.cpp). */
    virtual void report_stats() {}
};

/* Singleton accessor (gs_select.cpp). First call creates the backend per
 * ICORECOMP_GS; the dump flavor writes a file only when ICORECOMP_GS_DUMP
 * is set, otherwise it is the register shadow + statistics collector. */
GsBackend* rt_gs_backend();

/* Factories. rt_gs_make_parallel_backend() exists only when built with
 * ICORECOMP_HAVE_PARALLEL_GS; it fatals (loudly) if no usable Vulkan device
 * is found. */
GsBackend* rt_gs_make_dump_backend(const char* dump_path);
#ifdef ICORECOMP_HAVE_PARALLEL_GS
GsBackend* rt_gs_make_parallel_backend();
#endif

#endif /* ICORECOMP_GS_BACKEND_H */
