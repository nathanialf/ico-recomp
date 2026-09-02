/* hw/hw.h: internal declarations for the EE-local graphics transport
 * (P3 milestone): DMAC channel execution, VIF1 command interpreter, GIF
 * packet framing, VU1 runtime state, and GS privileged register routing.
 *
 * Module map:
 *   dmac.cpp   channel register file + synchronous transfer execution on
 *              CHCR.STR writes; D_CTRL/D_PCR/D_SQWC and the loud MFIFO and
 *              stall-control stubs. Completion raises D_STAT via
 *              ee/intc.cpp (rt_dmac_raise); handlers run deferred.
 *   vif1.cpp   VIF1 command stream interpreter (FIFO writes, DMA ch1
 *              payloads, TTE tag words). UNPACK writes VU1 data memory,
 *              MPG writes the micro shadow, DIRECT goes to GIF PATH2,
 *              MSCAL resolves microprograms through vu1rt.cpp.
 *   gif.cpp    GIF tag framing for PATH1/2/3 submissions; GIF_CTRL/
 *              GIF_STAT. Forwards packets to the GS backend.
 *   vu1rt.cpp  owns the one Vu1State, the VU1 micro memory shadow, the
 *              microprogram registry (rt_vu1_register) and rt_xgkick.
 *              Also owns the 0x11000000 window backing so EE stores to
 *              0x1100C000+ land in Vu1State::mem itself.
 *   gspriv.cpp GS privileged MMIO (0x12000000): routes to the backend's
 *              write_priv/read_priv shadow. CSR/IMR semantics stay owned
 *              by ee/intc.cpp (see the ownership note in gspriv.cpp).
 *   geomcheck.cpp
 *              ICORECOMP_VERBOSE=geom vertex validation of the GIF
 *              stream, attributed to the VU1 program that produced it.
 *              Read-only; it never changes a submission.
 *
 * This is runtime-internal, NOT part of the ABI contract.
 */
#ifndef ICORECOMP_HW_H
#define ICORECOMP_HW_H

#include "runtime.h"

/* ---- init --------------------------------------------------------------- */

/* Creates the GS backend (dump writer when ICORECOMP_GS_DUMP is set) and
 * logs the hardware model configuration. Call once from main() after
 * rt_mem_init(). */
void rt_hw_init();

/* True when ICORECOMP_GS selects a live backend (parallel or both,
 * including the per-platform default an unset variable resolves to).
 * Answers the question without creating anything, so a caller that has to
 * know before rt_hw_init() can ask; gs/gs_select.cpp keeps it and
 * make_backend in step. */
bool rt_gs_backend_selects_live();

/* ---- DMAC (dmac.cpp) ---------------------------------------------------- */

bool rt_dmac_mmio_read(uint32_t addr, uint32_t* out);
bool rt_dmac_mmio_write(uint32_t addr, uint32_t v);

/* Logs the last 32 source-chain tags walked on a channel, oldest first,
 * across kicks. The newest entry is marked "<- current": when a peripheral
 * raises a fatal from inside sink_payload, that tag is the one whose
 * payload is being fed. Channels 0-9; ch1, ch2 and ch9 walk chains in
 * dmac.cpp, ch4 in hw/ipu.cpp through rt_dmac_record_tag. */
void rt_dmac_dump_recent_tags(int ch);

/* Records one source-chain tag in that ring. For a channel whose chain
 * walk lives outside dmac.cpp: hw/ipu.cpp owns ch4 and calls this as it
 * reads each tag, so the ch4 dump is not empty. */
void rt_dmac_record_tag(int ch, uint32_t tadr, uint32_t id, uint32_t qwc, uint32_t taddr);

/* True while the DMAC is held and no channel may advance: D_ENABLE
 * (0x1000F590) bit 16 (suspend) is set, or D_CTRL.DMAE is clear. A CHCR
 * write with STR=1 in that state arms the channel without starting it;
 * dmac.cpp runs the armed channels, in channel-number order, when the hold
 * is lifted. hw/ipu.cpp asks before pulling the next qword of its ch4
 * source, since that walk is otherwise lazy and would advance the channel
 * registers the MPEG library reads back while suspended. */
bool rt_dmac_suspended();

/* The newest tag of that ring: the one whose payload is being fed when a
 * peripheral raises a fatal. id is the DMAtag ID (0 REFE, 3 REF, 4 REFS,
 * ...), addr the tag's ADDR field, qwc its transfer length. False when the
 * channel has walked no tags; the out pointers may be null. */
bool rt_dmac_current_tag(int ch, uint32_t* id, uint32_t* addr, uint32_t* qwc);

/* ---- VIF (vif1.cpp) ----------------------------------------------------- */

bool rt_vif_mmio_read(uint32_t addr, uint32_t* out);
bool rt_vif_mmio_write(uint32_t addr, uint32_t v);
/* guest_addr value for words that do not come from guest memory (a CPU
 * store into the VIF1 FIFO window). */
constexpr uint32_t RT_VIF1_ADDR_NONE = 0xFFFFFFFFu;

/* Feed 32-bit words into the VIF1 command stream (DMA ch1 payload, TTE tag
 * words, FIFO writes). Fully synchronous. guest_addr is the guest address
 * of words[0] as the DMAC saw it, bit 31 selecting the scratchpad the same
 * way MADR/TADR do, or RT_VIF1_ADDR_NONE when there is no such address. */
void rt_vif1_feed(const uint32_t* words, uint32_t count, uint32_t guest_addr);

/* Logs VIF1 register state, the last 32 VIFcodes taken, and a hexdump of
 * guest memory around the code word being executed. Called from the
 * unknown-VIFcode fatal path. */
void rt_vif1_dump_state();

/* FIFO windows take 128-bit stores; mmio.cpp routes them here so the upper
 * 64 bits are not lost. Returns true when the address is a FIFO. */
bool rt_hw_fifo_write128(uint32_t addr, const rc_u128* v);

/* ---- IPU (ipu.cpp) ------------------------------------------------------ */

/* IPU registers 0x10002000-0x10002030 and the FIFO windows at 0x10007000/
 * 0x10007010. IPU_CMD/IPU_TOP reads are 64-bit (busy flag in bit 63). */
bool rt_ipu_mmio_read(uint32_t addr, uint64_t* out);
bool rt_ipu_mmio_write(uint32_t addr, uint64_t v);
/* dmac.cpp forwards ch3 (fromIPU) / ch4 (toIPU) CHCR.STR kicks here; the
 * IPU owns transfer execution and completion for those channels (clears
 * STR and raises D_STAT itself, possibly later, when a command produces or
 * consumes the data). */
void rt_ipu_dma_kick(int ch);
/* dmac.cpp forwards a guest CHCR write that clears STR on ch3/ch4 here.
 * The transfer stops where it stands, with MADR/QWC/TADR and the input
 * FIFO left exactly as the stop found them; that frozen state is what the
 * MPEG library samples and rewinds from. No-op when the channel is idle. */
void rt_ipu_dma_stop(int ch);
/* dmac.cpp: the DMAC hold was just lifted and no ch3/ch4 kick was queued.
 * Gives a command that stalled for input while the ch4 walk was frozen
 * another chance to pull. No-op when neither IPU channel is active. */
void rt_ipu_dma_resume();
/* 128-bit CPU store to the toIPU FIFO window (0x10007010): appends one
 * qword to the bitstream input and resumes a stalled command. The game
 * primes SETIQ table data this way before the movie player switches the
 * feed to DMA ch4. */
void rt_ipu_fifo_feed(const uint8_t* qw16);
/* dmac.cpp: pointers into the ch3/ch4 register file for the IPU module.
 * which: 0=CHCR 1=MADR 2=QWC 3=TADR. */
uint32_t* rt_dmac_ipu_reg(int ch, int which);
/* Selftest hooks (hw/ipu_selftest.cpp): direct feed into the input queue
 * and reads from the output queue, bypassing the DMA bridge. */
void rt_ipu_test_feed(const uint8_t* data, size_t len);
size_t rt_ipu_test_out_avail();
size_t rt_ipu_test_read_out(uint8_t* dst, size_t maxlen);
/* Bits between the decode cursor and the end of the resident input. The
 * restart test asserts this stays inside the input FIFO window, which is
 * the invariant IPU_BP's IFC/FP fields claim. */
size_t rt_ipu_test_avail_bits();
/* Resident quadwords as IPU_BP reports them (IFC + FP) before the 4-bit
 * IFC field saturates, so the test can see an overflow the register
 * cannot express. */
size_t rt_ipu_test_resident_qw();

/* ---- GIF (gif.cpp) ------------------------------------------------------ */

/* path: 0 = PATH1 (XGKICK), 1 = PATH2 (VIF1 DIRECT), 2 = PATH3 (GIF DMA).
 * data must be a contiguous host buffer of qwords*16 bytes. */
void rt_gif_submit(int path, const uint8_t* data, uint32_t qwords);

/* ---- geometry diagnostics (geomcheck.cpp) -------------------------------- */

/* Validates one submitted packet. `vu1_hash` attributes PATH1 traffic to
 * the bound microprogram and is ignored for the other paths. Called from
 * rt_gif_submit only when ICORECOMP_VERBOSE names "geom". */
void rt_geom_scan(int path, const uint8_t* data, uint32_t qwords, uint32_t vu1_hash);
/* Records the CLIP judgment register at XGKICK time, which says whether a
 * microprogram's clip-cull path is running at all. */
void rt_geom_note_clip(uint32_t clip, uint32_t vu1_hash);
/* Emits the per-field summary and resets the field counters. */
void rt_geom_field(unsigned field);
/* Whole-run totals, for the end-of-run stats block. */
void rt_geom_report();
bool rt_gif_mmio_read(uint32_t addr, uint32_t* out);
bool rt_gif_mmio_write(uint32_t addr, uint32_t v);

/* ---- VU1 runtime (vu1rt.cpp) -------------------------------------------- */

/* 64 KB backing block for guest page 0x11000000. Allocated on first call.
 * Layout matches the EE bus window: micro0 at +0x0000, data0 at +0x4000,
 * micro1 at +0x8000, data1 at +0xC000. The data1 region IS
 * rt_vu1_state()->mem (same bytes). See vu1rt.cpp for the overlay note. */
uint8_t* rt_vu1_window_page();
Vu1State* rt_vu1_state();
/* 16 KB VU1 micro memory shadow written by VIF1 MPG (hash source). */
uint8_t* rt_vu1_micro();
/* Registers the generated microprograms (rt_vu1_register_all) when the
 * build linked generated/vu1. Called from rt_hw_init. */
void rt_vu1_init();
/* MPG upload notification so vu1rt can track the uploaded extent. */
void rt_vu1_micro_written(uint32_t offset, uint32_t bytes);
/* MSCAL/MSCALF/MSCNT: set pc/xtop/itop on the VU1 state, resolve the
 * current upload against the registry, run the program if registered,
 * loud-log and skip if not. */
void rt_vu1_mscal(uint32_t pc_bytes, uint32_t xtop, uint32_t itop, const char* how);
/* Upload hash of the microprogram currently bound, 0 before the first
 * MSCAL. Used to attribute GIF traffic back to the program that built it. */
uint32_t rt_vu1_bound_hash();
/* Micro-memory byte offset the MSCAL now executing was dispatched at.
 * These programs have ten entry points doing quite different work, so the
 * hash alone does not localise geometry to a code path; the pair does. */
uint32_t rt_vu1_entry_pc();

/* ---- GS privileged registers (gspriv.cpp) ------------------------------- */

bool rt_gspriv_mmio_read(uint32_t addr, uint64_t* out);
bool rt_gspriv_mmio_write(uint32_t addr, uint64_t v);
/* Called from ee/intc.cpp at vblank start: snapshots CSR into the backend
 * shadow and emits the backend vsync (dump Vsync packet). */
void rt_gs_vsync_hook(unsigned field);
/* Measured field rate, for the menu's FPS readout (ui/ui_settings_model.cpp).
 * `fields_per_second` is the number of rt_gs_vsync_hook calls in the last
 * completed one-second window divided by that window's real duration, and
 * `field_ms` is the mean interval between them in milliseconds. Both are 0
 * until the first window completes. Either pointer may be null.
 *
 * This counts vsync hook calls, which are fields: when the window is live
 * every one of them ends in a present, so it is the presented field rate.
 * A dump or headless backend still counts fields; it just never presents
 * them. */
void rt_gs_field_stats(double* fields_per_second, double* field_ms);
/* Called from the SetGsCrt syscall HLE: programs SMODE1/SMODE2 in the
 * backend priv shadow the way the real kernel does. Without SMODE1 (games
 * never write it directly) the GS cannot deduce the video mode and refuses
 * to scan out. Fatal on modes outside NTSC/PAL (not used by this binary). */
void rt_gs_program_crt(uint32_t interlace, uint32_t mode, uint32_t ffmd);

#endif /* ICORECOMP_HW_H */
