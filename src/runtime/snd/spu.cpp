/* snd/spu.cpp: fake SPU RAM.
 *
 * The real SPU2 has 2 MB of sample RAM addressed in bytes by the IOP-side
 * driver (the EE library's bank loader hands out monotonically increasing
 * byte addresses starting at 0x5010, matching the classic libsd first-free
 * address). Bank payloads reach us pre-staged in virtual IOP RAM; cmd 0x20
 * copies them here. Voice fetches in engine.cpp read this array directly.
 *
 * No .hd header parse happens here: the EE-side library parses the bank
 * header itself and every note-on arrives with explicit SPU addresses,
 * pitch and ADSR words (see SNDN2_NOTES.md), so the IOP side only ever
 * needs the raw VAG bodies.
 */
#include "snd.h"

#include "../runtime.h"

#include <cstdlib>
#include <cstring>

namespace {

uint8_t* g_spu_ram = nullptr;
uint32_t g_upload_count = 0;
uint32_t g_upload_bytes = 0;

} // namespace

uint8_t* rt_spu_ram() {
    if (!g_spu_ram) {
        g_spu_ram = (uint8_t*)std::calloc(1, RT_SPU_RAM_SIZE);
        if (!g_spu_ram) rt_fatal("snd", nullptr, "SPU RAM allocation failed");
    }
    return g_spu_ram;
}

void rt_spu_upload(const uint8_t* src, uint32_t spu_addr, uint32_t len) {
    uint8_t* ram = rt_spu_ram();
    if (spu_addr >= RT_SPU_RAM_SIZE || len > RT_SPU_RAM_SIZE - spu_addr) {
        rt_fatal("snd", nullptr,
            "bank transfer out of SPU RAM bounds: dest=0x%06x len=0x%x (SPU RAM is 0x%x bytes)",
            spu_addr, len, RT_SPU_RAM_SIZE);
    }
    std::memcpy(ram + spu_addr, src, len);
    ++g_upload_count;
    g_upload_bytes += len;
    rt_log("snd", "SPU upload #%u: 0x%06x..0x%06x (%u bytes, %u total)",
        g_upload_count, spu_addr, spu_addr + len, len, g_upload_bytes);
}
