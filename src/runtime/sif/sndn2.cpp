/* sif/sndn2.cpp: HLE of the game's own IOP sound server (SNDN2DRV.IRX,
 * RPC server id 0x736e646e, ASCII "sndn"). This file owns the wire
 * behavior (record unpacking + the seq-tag ack the EE's sync spins on) and
 * forwards every decoded command to the native sound engine (snd/engine.cpp),
 * which produces the actual audio. Protocol facts were reverse engineered
 * from the retail EE-side vendor sound library in the user's own game copy
 * (the disassembly of the sound object that begins at PAL 0x00272338, plus
 * boot RPC traffic); see SNDN2_NOTES.md next to this file.
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
 *       sync primitive (SgGetDmaTransferStatus, PAL 0x00276D58) spins
 *       until that word equals
 *       its issued-command counter.
 *
 * Command record layout (packed by retail _SgDmaCommon (PAL 0x00276CC8)):
 *   w0 = command id
 *   w1 = (seq_tag << 8) | a1[23:16]     seq_tag == 0 for untagged commands
 *   w2 = (a1[15:0] << 16) | a2[23:8]
 *   w3 = (a2[7:0] << 24) | a3[23:0]
 * Untagged commands (ids 0xA-0xD, issued by the flush path itself) carry
 * per-tick level/status values and do not advance the ack.
 */
#include "rpc.h"

#include "../snd/snd.h"

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
    /* Names from EE-side call sites (SNDN2_NOTES.md tracks the evidence). */
    switch (cmd) {
        case 0x01: return "voice-volume";
        case 0x02: return "voice-adsr";
        case 0x03: return "voice-addr";
        case 0x04: return "voice-note";
        case 0x0A: return "key-on-mask";
        case 0x0B: return "key-off-mask";
        case 0x0C: return "rev-send-mask";
        case 0x0D: return "mode-mask";
        case 0x14: return "reverb-endaddr";
        case 0x15: return "reverb-type";
        case 0x16: return "reverb-depth";
        case 0x17: return "reverb-delay";
        case 0x18: return "reverb-feedback";
        case 0x1F: return "quit";
        /* SgDmaWrite, PAL 0x00276C70: iop_addr, offset, size */
        case 0x20: return "bank-transfer";
        case 0x21: return "dma-read";
        case 0x28: return "master-vol";
        case 0x32: return "param";
        case 0x3C: return "st-adpcm-init";
        case 0x3D: return "st-adpcm-quit";
        case 0x3E: return "st-adpcm-open";
        case 0x3F: return "st-adpcm-close";
        case 0x40: return "st-adpcm-volume";
        case 0x41: return "st-adpcm-pitch";
        case 0x42: return "st-adpcm-play";
        case 0x43: return "st-adpcm-stop";
        default: return "?";
    }
}

void handle_init(const uint8_t* send, uint32_t send_size) {
    g_ack_tag = 0;
    uint32_t w0 = send_size >= 4 ? rd32(send, 0) : 0;
    uint32_t w1 = send_size >= 8 ? rd32(send, 4) : 0;
    rt_log_info("sndn2", "init (fno=0x65): w0=0x%x w1=0x%x send_size=%u; ack counter reset", w0, w1, send_size);
    if (w0 != 0x1E) {
        rt_log_warn("sndn2", "WARNING init w0=0x%x, expected 0x1E (voice count?); protocol drift?", w0);
    }
    rt_snd_engine_init(w0);
}

void handle_batch(const uint8_t* send, uint32_t send_size, uint8_t* recv, uint32_t recv_size) {
    ++g_batches;
    if (send_size % 16 != 0) {
        /* A batch is N 16-byte records; anything else means the runtime is
         * about to read a partial record. Batches arrive once a field, so
         * the first few are printed and the rest fold on this condition's
         * own counter. */
        static uint64_t ragged = 0;
        ++ragged;
        if (ragged <= 4 || (ragged & (ragged - 1)) == 0) {
            rt_log_warn("sndn2", "WARNING batch send_size=%u not a multiple of 16; the last "
                "partial record is not decoded [#%" PRIu64 "]", send_size, ragged);
        }
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
        if (w0 == 0x20 || w0 == 0x21) {
            /* DMA commands are the only records packed by retail
             * _SgDmaCommon (PAL 0x00276CC8) (24-bit operands + the seq tag the EE's sync
             * spins on); everything else is stored raw by _SgSetPkAdd (PAL 0x002737E0). */
            g_ack_tag = tag;
            if (rt_trace() || (g_commands & (g_commands + 1)) == 0) {
                rt_log_debug("sndn2", "cmd 0x%02x (%s) tag=%u a1=0x%06x a2=0x%06x a3=0x%06x [command #%" PRIu64 "]",
                    w0, cmd_name(w0), tag, a1, a2, a3, g_commands);
            }
            if (w0 == 0x20) {
                /* Every bank transfer at info: there are a handful per
                 * scene and each says which SPU RAM range holds data, which
                 * is what a later key-on is checked against (snd/spu.cpp
                 * rt_spu_covered_by). Measured on the PAL boot path
                 * 2026-09-04: seven of them, all from IOP 0x090000, filling
                 * SPU 0x005010..0x1b8210. */
                rt_log_info("sndn2", "bank transfer #%" PRIu64 ": IOP 0x%06x -> SPU RAM 0x%06x..0x%06x (%u bytes)",
                    g_commands, a1, a2, a2 + a3, a3);
                /* bank-transfer: the payload was already staged into
                 * virtual IOP RAM by the raw EE->IOP SifSetDma the game
                 * issued beforehand (a1 = source inside the iopheap
                 * allocation, a2 = SPU RAM byte destination, a3 = byte
                 * length). Consume it now: the EE reuses the staging
                 * buffer for the next chunk. */
                if (a1 + a3 > RT_IOP_RAM_SIZE) {
                    rt_fatal("sndn2", rt_fault_ctx(), "bank transfer source out of IOP RAM: src=0x%06x len=0x%06x", a1, a3);
                }
                /* The staging check. soundBDDataSet (PAL 0x00143D68,
                 * sound/s_init.c) writes each chunk into its IOP heap block
                 * with a raw sceSifSetDma and only then queues this record,
                 * so every byte of [a1, a1 + a3) must have been written by a
                 * SifSetDma since the last transfer consumed that address.
                 * If it was not, the copy takes whatever the staging buffer
                 * still holds, which for a fresh heap block is zeros, and
                 * the bank arrives empty with nothing to say so. */
                uint32_t staged = rt_spu_staged_bytes(a1, a3);
                if (staged < a3) {
                    rt_log_warn("sndn2", "WARNING bank transfer #%" PRIu64 " reads IOP 0x%06x..0x%06x "
                        "but only %u of those %u bytes were written by an EE to IOP SifSetDma since "
                        "the last transfer consumed them. The game stages every chunk immediately "
                        "before queueing the 0x20 (soundBDDataSet, PAL 0x00143D68, SifSetDma at "
                        "00143F54 then SgDmaWrite at 00143F90), so the missing bytes are a transfer "
                        "this runtime lost and the bank is being filled with stale staging memory",
                        g_commands, a1, a1 + a3, staged, a3);
                }
                rt_spu_upload(rt_iop_ptr(a1), a2, a3);
                rt_spu_consume_staging(a1, a3);
            } else {
                {
                    /* One record per occurrence would be one line a field
                     * if the game ever starts sending these. Fold. */
                    static uint64_t readbacks = 0;
                    ++readbacks;
                    if (readbacks <= 4 || (readbacks & (readbacks - 1)) == 0) {
                        rt_log_warn("sndn2", "WARNING cmd 0x21 (SgDmaRead) NOT MODELED: SPU to "
                            "IOP readback ignored [#%" PRIu64 "]", readbacks);
                    }
                }
            }
        } else {
            /* Open/close/play/stop are rare and each one matters: never
             * sample those away. Volume and pitch are not -- the game
             * re-issues them every frame during playback, which is what
             * used to make this branch's "each one matters" claim flood
             * the default log -- so they fall into the ordinary
             * power-of-two sampling below like any other command; the
             * "sndn2" verbose channel still keeps every one. */
            const bool stream_ctl = (w0 == 0x3E || w0 == 0x3F || w0 == 0x42 || w0 == 0x43);
            /* Key-ons at info, folded by powers of two. A run with no sound
             * has to be readable from the default log: "the game keyed
             * nothing" (no line here) is a different fact from "the game
             * keyed voices and the mixer produced silence" (lines here, then
             * look at the mixer). Measured 2026-09-04: three native-renderer
             * runs sat on the PAL boot screens, which key nothing, and the
             * log could not say so without this. */
            if (w0 == 0x0A) {
                static uint64_t keyons = 0;
                ++keyons;
                if ((keyons & (keyons - 1)) == 0) {
                    rt_log_info("sndn2", "voice key-on command #%" PRIu64 " (mask w1=0x%08x w2=0x%08x): "
                        "the game asked for a sound", keyons, w1, w2);
                }
            }
            if (rt_trace() || stream_ctl || (g_commands & (g_commands + 1)) == 0 || rt_verbose("sndn2")) {
                rt_log_debug("sndn2", "cmd 0x%02x (%s) w1=0x%08x w2=0x%08x w3=0x%08x [command #%" PRIu64 "]",
                    w0, cmd_name(w0), w1, w2, w3, g_commands);
            }
            rt_snd_command(w0, w1, w2, w3);
        }
    }
    /* The EE library flushes once per vblank field; render that field's
     * audio now (also covers empty batches, which are the common case). */
    rt_snd_flush_tick();
    if (recv_size >= kAckOffset + 4) {
        /* Stream read cursors at +0xC0 (read by SgSetAdpcmIopReadAddr,
         * retail SgStAdpcmIopReadAddr (PAL 0x002785A0)), then the DMA ack word. */
        rt_snd_fill_status(recv, recv_size);
        wr32(recv, kAckOffset, g_ack_tag);
    } else if (recv_size) {
        rt_log_warn("sndn2", "WARNING batch recv_size=%u < 0x%x: ack word not delivered", recv_size, kAckOffset + 4);
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
            rt_log_warn("sndn2", "WARNING fno=0x%x NOT MODELED (send_size=%u recv_size=%u): "
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
