/* gs/gs_backend.h: narrow interface between the EE-side graphics transport
 * (hw/dmac, hw/vif1, hw/gif, hw/gspriv) and a GS implementation.
 *
 * Implementations:
 *   gs_dumpwriter.cpp  records the GIF/priv-register traffic in the
 *                      paraLLEl-GS raw stream format (and acts as the
 *                      register shadow when no dump path is configured).
 *   (future)           paraLLEl-GS itself, as a shared library.
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
};

/* Singleton accessor. First call creates the backend: a dump writer when
 * ICORECOMP_GS_DUMP=path is set, otherwise the same class with no file
 * (shadow + statistics only). */
GsBackend* rt_gs_backend();

#endif /* ICORECOMP_GS_BACKEND_H */
