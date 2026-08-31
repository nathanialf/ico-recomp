/* sif/sndn2.cpp: HLE of the game's own IOP sound server (SNDN2DRV.IRX,
 * RPC server id 0x736e646e, ASCII "sndn"). No audio is produced; this
 * models exactly the wire behavior the EE-side library needs to get past
 * its synchronization points. Protocol facts were reverse engineered from
 * the retail EE-side vendor sound library in the user's own game copy
 * (disassembly at decomp-repo asm/nonmatchings/src/cod/vendor_258CC0 and
 * one boot's RPC traffic); see SNDN2_NOTES.md next to this file.
 *
 * Wire summary:
 *   fno 0x65 (sync call, 64-byte send/recv): remote init. Send word 0 is
 *       0x1E, word 1 is the caller's init argument. The library ignores
 *       the receive data.
 *   fno 0x64 (NOWAIT call): command batch flush. The send data is N 16-byte
 *       command records drained from the EE-side double-buffered ring; the
 *       receive buffer is the 0x200-byte EE status block (passed via its
 *       uncached alias). Word +0x1C0 of the status block must echo the
 *       24-bit sequence tag of the most recent tagged command; the EE's
 *       sync primitive (retail func_0025C768) spins until that word equals
 *       its issued-command counter.
 *
 * Command record layout (packed by retail func_0025C6D8):
 *   w0 = command id
 *   w1 = (seq_tag << 8) | a1[23:16]     seq_tag == 0 for untagged commands
 *   w2 = (a1[15:0] << 16) | a2[23:8]
 *   w3 = (a2[7:0] << 24) | a3[23:0]
 * Untagged commands (ids 0xA-0xD, issued by the flush path itself) carry
 * per-tick level/status values and do not advance the ack.
 */
#include "rpc.h"

#include <cinttypes>
#include <cstring>

namespace {

uint32_t rd32(const uint8_t* p, uint32_t off) { uint32_t v; std::memcpy(&v, p + off, 4); return v; }
void wr32(uint8_t* p, uint32_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }

/* Offset of the ack word inside the 0x200-byte EE status block. */
constexpr uint32_t kAckOffset = 0x1C0;

uint32_t g_ack_tag = 0;      /* last seq tag processed, echoed at +0x1C0 */
uint64_t g_batches = 0;
uint64_t g_commands = 0;

const char* cmd_name(uint32_t cmd) {
    /* Names inferred from EE-side call sites; unknown ids keep "?" until a
     * call site is identified (SNDN2_NOTES.md tracks the evidence). */
    switch (cmd) {
        case 0x0A: return "tick-level-A";     /* flush path, from status+0x20 */
        case 0x0B: return "tick-level-B";     /* flush path, from status+0x28 */
        case 0x0C: return "tick-level-C";     /* flush path, from status+0x00/0x08 */
        case 0x0D: return "tick-level-D";     /* flush path, from status+0x10/0x18 */
        case 0x20: return "bank-transfer";    /* retail func_0025C680: iop_addr, offset, size */
        default: return "?";
    }
}

void handle_init(const uint8_t* send, uint32_t send_size) {
    g_ack_tag = 0;
    uint32_t w0 = send_size >= 4 ? rd32(send, 0) : 0;
    uint32_t w1 = send_size >= 8 ? rd32(send, 4) : 0;
    rt_log("sndn2", "init (fno=0x65): w0=0x%x w1=0x%x send_size=%u; ack counter reset", w0, w1, send_size);
    if (w0 != 0x1E) {
        rt_log("sndn2", "WARNING init w0=0x%x, expected 0x1E (voice count?); protocol drift?", w0);
    }
}

void handle_batch(const uint8_t* send, uint32_t send_size, uint8_t* recv, uint32_t recv_size) {
    ++g_batches;
    if (send_size % 16 != 0) {
        rt_log("sndn2", "WARNING batch send_size=%u not a multiple of 16", send_size);
    }
    uint32_t n = send_size / 16;
    for (uint32_t i = 0; i < n; ++i) {
        const uint8_t* c = send + i * 16;
        uint32_t w0 = rd32(c, 0), w1 = rd32(c, 4), w2 = rd32(c, 8), w3 = rd32(c, 12);
        uint32_t tag = w1 >> 8;
        uint32_t a1 = ((w1 & 0xFF) << 16) | (w2 >> 16);
        uint32_t a2 = ((w2 & 0xFFFF) << 8) | (w3 >> 24);
        uint32_t a3 = w3 & 0xFFFFFF;
        ++g_commands;
        if (tag) g_ack_tag = tag;
        rt_log("sndn2", "cmd 0x%02x (%s) tag=%u a1=0x%06x a2=0x%06x a3=0x%06x [command #%" PRIu64 "]",
            w0, cmd_name(w0), tag, a1, a2, a3, g_commands);
        /* bank-transfer (0x20): the payload was already staged into virtual
         * IOP RAM by the raw EE->IOP SifSetDma the game issued beforehand
         * (a1 selects the destination inside the iopheap allocation); with
         * no SPU model the ack below is the entire required behavior. */
    }
    if (recv_size >= kAckOffset + 4) {
        wr32(recv, kAckOffset, g_ack_tag);
    } else if (recv_size) {
        rt_log("sndn2", "WARNING batch recv_size=%u < 0x%x: ack word not delivered", recv_size, kAckOffset + 4);
    }
}

void svc_sndn2(uint32_t fno, const uint8_t* send, uint32_t send_size,
               uint8_t* recv, uint32_t recv_size) {
    switch (fno) {
        case 0x65:
            handle_init(send, send_size);
            break;
        case 0x64:
            handle_batch(send, send_size, recv, recv_size);
            break;
        default:
            if (recv_size >= 4) wr32(recv, 0, 0);
            rt_log("sndn2", "WARNING fno=0x%x NOT MODELED (send_size=%u recv_size=%u): "
                "zeroed recv", fno, send_size, recv_size);
            break;
    }
}

} // namespace

void rt_sndn2_register_service() {
    g_ack_tag = 0;
    g_batches = 0;
    g_commands = 0;
    rt_rpc_register_service(0x736e646e, "sndn2drv", svc_sndn2);
}
