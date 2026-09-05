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
 *
 * Two witnesses live here beside the RAM itself, because "the game keyed a
 * voice and nothing was heard" has to be answerable from a plain log:
 *
 *   - what the EE staged in IOP RAM (iop_stage.h), so a cmd 0x20 whose
 *     source was never written by a SifSetDma is reported as the lost
 *     transfer it would be, rather than copying zeros in silence;
 *   - which bank transfer last wrote a given SPU address, so a key-on can
 *     say whether the region it starts from was ever uploaded. That is a
 *     fact the runtime holds. Reading the bytes there and calling the
 *     region empty is a different claim, and a weaker one: a 16-byte run of
 *     zeros inside an uploaded bank is ordinary content. Measured
 *     2026-09-04: that inference reported "no bank uploaded" for SPU
 *     0x081f70, an address the same run's bank transfer #12 had covered
 *     (0x03ee90..0x08db10).
 */
#include "snd.h"

#include "iop_stage.h"

/* For rt_fault_ctx: a fatal here is raised from guest-driven work (a cmd
 * 0x20 the EE queued), so the dump should carry that code's registers. */
#include "../ee/kernel.h"

#include "../prof.h"
#include "../runtime.h"

#include <cstdlib>
#include <cstring>

namespace {

uint8_t* g_spu_ram = nullptr;
uint32_t g_upload_count = 0;
uint32_t g_upload_bytes = 0;

/* What the EE has staged in virtual IOP RAM and no cmd 0x20 has consumed
 * yet. sif/rpc.cpp hands every EE to IOP DMA entry to the sound side
 * (rt_snd_pcm_note_iop_write), which forwards the range here. */
RtIopStageMap g_stage;

/* Bank transfer history, newest last. A key-on scans it backwards for the
 * most recent transfer covering the voice's start address. The PAL boot path
 * issues seven (measured, native-crosscheck.log 2026-09-04); when the table
 * is full the oldest goes rather than letting it grow without bound, and a
 * query that finds nothing says so instead of guessing. */
constexpr int kMaxRanges = 256;
struct Range {
    uint32_t begin = 0, end = 0;
    uint32_t transfer = 0;
};
Range g_ranges[kMaxRanges];
int g_range_count = 0;

void record_range(uint32_t begin, uint32_t end, uint32_t transfer) {
    if (g_range_count == kMaxRanges) {
        std::memmove(&g_ranges[0], &g_ranges[1], sizeof(Range) * (kMaxRanges - 1));
        --g_range_count;
    }
    g_ranges[g_range_count++] = Range{begin, end, transfer};
}

} // namespace

uint8_t* rt_spu_ram() {
    if (!g_spu_ram) {
        g_spu_ram = (uint8_t*)std::calloc(1, RT_SPU_RAM_SIZE);
        if (!g_spu_ram) rt_fatal("snd", nullptr, "SPU RAM allocation failed");
    }
    return g_spu_ram;
}

void rt_spu_note_iop_write(uint32_t iop_addr, uint32_t size) {
    g_stage.note_write(iop_addr, size);
}

uint32_t rt_spu_staged_bytes(uint32_t iop_addr, uint32_t size) {
    return g_stage.staged_bytes(iop_addr, size);
}

uint32_t rt_spu_covered_by(uint32_t spu_addr, uint32_t len) {
    for (int i = g_range_count - 1; i >= 0; --i) {
        if (spu_addr >= g_ranges[i].begin && (uint64_t)spu_addr + len <= g_ranges[i].end) {
            return g_ranges[i].transfer;
        }
    }
    return 0;
}

void rt_spu_upload(const uint8_t* src, uint32_t spu_addr, uint32_t len) {
    RT_PROF_ZONE(RT_PROF_AUDIO);
    uint8_t* ram = rt_spu_ram();
    if (spu_addr >= RT_SPU_RAM_SIZE || len > RT_SPU_RAM_SIZE - spu_addr) {
        rt_fatal("snd", rt_fault_ctx(),
            "bank transfer out of SPU RAM bounds: dest=0x%06x len=0x%x (SPU RAM is 0x%x bytes)",
            spu_addr, len, RT_SPU_RAM_SIZE);
    }
    std::memcpy(ram + spu_addr, src, len);
    ++g_upload_count;
    g_upload_bytes += len;
    record_range(spu_addr, spu_addr + len, g_upload_count);
    rt_log_debug("snd", "SPU upload #%u: 0x%06x..0x%06x (%u bytes, %u total)",
        g_upload_count, spu_addr, spu_addr + len, len, g_upload_bytes);
}

void rt_spu_consume_staging(uint32_t iop_addr, uint32_t size) {
    g_stage.consume(iop_addr, size);
}
