/* hw/gspriv.cpp: GS privileged register MMIO (0x12000000-0x12001FFF)
 * routed to the GS backend's write_priv/read_priv shadow.
 *
 * CSR/IMR ownership decision: ee/intc.cpp keeps ownership of CSR and IMR
 * semantics (event flags, write-1-clear, RESET, FIELD bit, and the values
 * GsGetIMR/GsPutIMR see) because interrupt delivery depends on them and
 * moving that state would couple the delivery path to the backend. This
 * module forwards every privileged write to the backend as well, so the
 * dump's PrivRegisters snapshots stay coherent, and it snapshots the live
 * CSR value into the backend at each vsync (rt_gs_vsync_hook). Reads of
 * CSR/IMR come from intc's shadow; reads of everything else come from the
 * backend shadow (last value written, which is what the game expects from
 * write-mostly display registers).
 */
#include "hw.h"

#include "../ee/kernel.h"
#include "../gs/gs_backend.h"

namespace {

bool is_priv(uint32_t addr) {
    return addr >= 0x12000000u && addr < 0x12002000u;
}

} // namespace

void rt_hw_init() {
    rt_vu1_window_page(); /* also constructs the Vu1State overlay */
    rt_vu1_init();        /* registers generated VU1 microprograms */
    rt_gs_backend();      /* opens the dump file early if configured */
    rt_log("hw", "graphics transport initialized (DMAC, VIF1, GIF, GS priv)");
}

bool rt_gspriv_mmio_read(uint32_t addr, uint64_t* out) {
    if (!is_priv(addr)) return false;
    /* CSR/IMR: intc.cpp is the authority. */
    if (rt_gs_mmio_read(addr, out)) return true;
    *out = rt_gs_backend()->read_priv(addr & 0x1FFF);
    return true;
}

bool rt_gspriv_mmio_write(uint32_t addr, uint64_t v) {
    if (!is_priv(addr)) return false;
    /* Record everything in the backend shadow (dump coherence), then let
     * intc.cpp apply CSR/IMR semantics on top. */
    rt_gs_backend()->write_priv(addr & 0x1FFF, v);
    rt_gs_mmio_write(addr, v);
    return true;
}

void rt_gs_vsync_hook(unsigned field) {
    GsBackend* be = rt_gs_backend();
    uint64_t csr = 0;
    if (rt_gs_mmio_read(0x12001000u, &csr)) be->write_priv(0x1000, csr);
    be->write_priv(0x1010, rt_gs_get_imr());
    be->vsync(field);
}
