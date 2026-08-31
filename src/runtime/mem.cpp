/* mem.cpp: guest address space setup. Owns g_pages, g_functab,
 * g_functab_orig and the function-dispatch helpers (rt_override,
 * rt_original, rt_call_indirect), per the ABI declared in recomp_api.h.
 *
 * Layout (see recomp_api.h's memory-model comment and CLAUDE.md P1 scope):
 *   0x00000000-0x01FFFFFF  32 MB EE RAM
 *   0x20000000-0x21FFFFFF  RAM alias (uncached accelerated)
 *   0x30000000-0x31FFFFFF  RAM alias (uncached accelerated, mirror B)
 *   0x80000000-0x81FFFFFF  RAM alias (kseg0, cached)
 *   0xA0000000-0xA1FFFFFF  RAM alias (kseg1, uncached)
 *   0x70000000-0x7000FFFF  scratchpad (16 KB architectural, see below)
 *   0x11000000-0x1100FFFF  VU0/VU1 micro+data memory window (see below)
 *   0x10xxxxxx, 0x12xxxxxx MMIO; left NULL, routed to rt_mmio_*
 *
 * Deviation / documented tradeoff -- scratchpad:
 *   The page table is 64 KB-granular but the EE scratchpad (SPR) is only
 *   16 KB; on real hardware the low 14 bits of the in-page offset are what's
 *   architecturally decoded, so the 16 KB region repeats 4x across the 64 KB
 *   page. We do not reproduce that repeat: we simply back the whole 64 KB
 *   page with one zeroed block and let all 64 KB be addressable, independent
 *   storage. Every legitimate access uses vaddr 0x70000000-0x70003FFF (the
 *   16 KB compilers/games are told exists), so this is unobservable for
 *   correct guest code; it only differs from hardware if guest code
 *   deliberately computes an SPR address outside that first 16 KB. Chosen
 *   over leaving the page NULL (routed to MMIO) or a true small-buffer
 *   mirror because it keeps every scratchpad access a flat load/store with
 *   no extra masking on the hot path.
 *
 * VU memory window:
 *   Real VU0/VU1 instruction and data memories are separate small regions
 *   (4 KB micro0 + 4 KB data0 + 16 KB micro1 + 16 KB data1 = 40 KB) that the
 *   EE bus maps into one 0x11000000 64 KB page at fixed offsets. Since P3
 *   this page is owned by hw/vu1rt.cpp: the window's data1 region
 *   (+0xC000..+0xFFFF) is Vu1State::mem itself, so the VIF1 UNPACK HLE,
 *   recompiled VU1 microprograms and direct EE window accesses all see the
 *   same bytes. See the overlay note in hw/vu1rt.cpp for alignment and the
 *   micro1 caveat.
 */
#include "runtime.h"

#include "ee/kernel.h"
#include "host/portable.h"
#include "hw/hw.h"

#include <cstdlib>
#include <cstring>

/* ---- ABI-mandated globals (recomp_api.h) -------------------------------- */

recomp_fn_t g_functab[RECOMP_FUNCTAB_SLOTS];
recomp_fn_t g_functab_orig[RECOMP_FUNCTAB_SLOTS];
uint8_t* g_pages[0x10000];

namespace {

constexpr size_t kPageSize = 0x10000;
constexpr size_t kRamSize = RT_RAM_SIZE;
constexpr size_t kScratchpadBlockSize = kPageSize; /* see tradeoff comment above */

uint8_t* g_ram = nullptr;
uint8_t* g_scratchpad = nullptr;

uint8_t* aligned_zalloc(size_t align, size_t size) {
    void* p = rt_aligned_zalloc(align, size);
    if (!p) rt_fatal("mem", nullptr, "rt_aligned_zalloc(align=%zu, size=%zu) failed", align, size);
    return static_cast<uint8_t*>(p);
}

void map_ram_prefix(uint32_t prefix, uint32_t page_count) {
    uint32_t base_idx = prefix >> 16;
    for (uint32_t i = 0; i < page_count; ++i) {
        g_pages[base_idx + i] = g_ram + size_t(i) * kPageSize;
    }
}

} // namespace

void rt_mem_init() {
    static_assert(kRamSize % 16 == 0, "RAM size must be 16-byte aligned");
    g_ram = aligned_zalloc(16, kRamSize);
    g_scratchpad = aligned_zalloc(16, kScratchpadBlockSize);

    std::memset(g_pages, 0, sizeof(g_pages));

    const uint32_t ram_pages = uint32_t(kRamSize / kPageSize); /* 512 */
    map_ram_prefix(0x00000000u, ram_pages);
    map_ram_prefix(0x20000000u, ram_pages);
    map_ram_prefix(0x30000000u, ram_pages);
    map_ram_prefix(0x80000000u, ram_pages);
    map_ram_prefix(0xA0000000u, ram_pages);

    g_pages[0x70000000u >> 16] = g_scratchpad;
    g_pages[0x11000000u >> 16] = rt_vu1_window_page();

    /* MMIO ranges (0x10xxxxxx device regs, 0x12xxxxxx GS privileged) are left
     * NULL by the memset above; the rc_read.. / rc_write.. helpers in
     * recomp_ops.h fall through to rt_mmio_.. for a NULL page. */

    rt_log("mem", "EE RAM: %zu bytes at host %p, guest 0x00000000 (+ aliases at 0x20000000/0x30000000/0x80000000/0xA0000000)",
        kRamSize, static_cast<void*>(g_ram));
    rt_log("mem", "scratchpad: %zu-byte page at host %p, guest 0x70000000 (16 KB architectural, see mem.cpp comment)",
        kScratchpadBlockSize, static_cast<void*>(g_scratchpad));
    rt_log("mem", "VU mem window: page at host %p, guest 0x11000000 (micro0@+0x0 data0@+0x4000 micro1@+0x8000 data1@+0xC000 = Vu1State::mem)",
        static_cast<void*>(g_pages[0x11000000u >> 16]));
}

/* ---- function dispatch --------------------------------------------------- */

void rt_override(uint32_t vram, recomp_fn_t fn) {
    if (vram < RECOMP_TEXT_BASE || vram >= RECOMP_TEXT_LIMIT) {
        rt_log("functab", "rt_override: vram 0x%08x out of [0x%08x, 0x%08x)", vram, RECOMP_TEXT_BASE, RECOMP_TEXT_LIMIT);
        return;
    }
    uint32_t idx = RECOMP_FUNC_IDX(vram);
    if (!g_functab_orig[idx]) g_functab_orig[idx] = g_functab[idx];
    g_functab[idx] = fn;
}

recomp_fn_t rt_original(uint32_t vram) {
    if (vram < RECOMP_TEXT_BASE || vram >= RECOMP_TEXT_LIMIT) return nullptr;
    uint32_t idx = RECOMP_FUNC_IDX(vram);
    return g_functab_orig[idx] ? g_functab_orig[idx] : g_functab[idx];
}

void rt_call_indirect(R5900Context* ctx, uint32_t target, uint32_t caller_vram) {
    if (target == RT_CLEAN_EXIT_VRAM) {
        rt_log("functab", "clean exit: caller 0x%08x returned through the boot sentinel", caller_vram);
        rt_dump_registers(ctx);
        std::exit(0);
    }
    if (target < RECOMP_TEXT_BASE || target >= RECOMP_TEXT_LIMIT) {
        rt_bad_indirect(target, caller_vram);
        return;
    }
    uint32_t idx = RECOMP_FUNC_IDX(target);
    recomp_fn_t fn = g_functab[idx];
    if (!fn) {
        /* Untranslated target: give the SIF HLE a chance to claim it (the
         * sifcmd system handlers are data-referenced sub-entries inside a
         * handwritten libkernel blob; see sif/sif.cpp). */
        if (rt_sif_try_resolve_indirect(ctx, target, caller_vram)) return;
        rt_bad_indirect(target, caller_vram);
        return;
    }
    fn(ctx);
}
