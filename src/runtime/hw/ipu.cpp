/* hw/ipu.cpp: EE IPU (image processor) model, sized to this one binary.
 *
 * The retail ELF drives the IPU from Sony's libmpeg/libipu (translated
 * vendor code) to play the single FMV (attract movie). The measured command
 * census of that driver is: BCLR, BDEC, VDEC, FDEC, SETIQ, SETVQ, CSC,
 * SETTH. IDEC and PACK are never issued and are loud-fatal stubs here, per
 * the coverage policy (no speculative ops).
 *
 * Input FIFO contract (this is what the retail MPEG library measures, so it
 * is reproduced exactly rather than approximated):
 *
 *   stream byte position = D4_MADR - (IFC + FP) * 16 + (IPU_BP.BP >> 3)
 *
 * That expression is the retail code's own, at 0x2407FC-0x240848 in the
 * decomp (asm/nonmatchings/src/GobjProc/func_002407C0.s): it loads D4_MADR
 * and IPU_BP, takes IFC = (BP >> 8) & 0xF and FP = (BP >> 16) & 3, forms
 * (IFC + FP) << 4, subtracts that from MADR and adds (BP & 0x7F) >> 3. The
 * library's stop/restart bracket does the same arithmetic on the DMA
 * registers themselves (asm/nonmatchings/src/cod/vendor_2575C0/
 * func_002587E0.s at 0x258804-0x258830 and 0x2588DC-0x258910): after the
 * snapshot taken by func_002586F8 it restarts ch4 with
 *   MADR' = MADR - (IFC + FP) * 16      QWC' = QWC + IFC + FP
 * and issues BCLR with the saved BP first, i.e. it hands back to the DMA
 * exactly the quadwords that were still resident in the IPU and expects
 * BCLR to have thrown them away. Three observable properties follow, and
 * the model below exists to satisfy them:
 *   1. MADR/QWC/TADR advance only by what actually entered the input FIFO,
 *      so the DMA never runs more than the FIFO depth ahead of the decoder.
 *   2. IPU_BP.BP is the bit offset inside the first resident quadword, and
 *      IFC + FP is the number of resident quadwords, counted from wherever
 *      the decoder will read next.
 *   3. A CHCR write with STR=0 stops the transfer with those registers
 *      frozen where the FIFO fill left them.
 *
 * Model:
 *   - Bitstream input is a byte queue that stands for the hardware input
 *     FIFO plus the decode window. The toIPU DMA (ch4, normal and source
 *     chain) pushes quadwords into it and stalls the copy when it is full:
 *     the depth is the hardware 8 quadwords (IPU_CTRL.IFC / IPU_BP.IFC are
 *     4-bit fields that saturate at 8) plus the one quadword the decoder is
 *     working inside, reported as FP=1. The chain walk itself is not
 *     demand-driven: reading a tag costs no FIFO space, so the walk runs on
 *     to the end of the chain even with the FIFO full, and the transfer
 *     completes (STR clear, D_STAT bit 4) as soon as the last tag's payload
 *     has entered the FIFO, whether or not the IPU has consumed it. A CHCR
 *     write that clears STR stops it where it stands (rt_ipu_dma_stop).
 *   - Commands execute synchronously at the IPU_CMD write. A command that
 *     runs out of bitstream rewinds to its start and stays pending (BUSY
 *     reads 1); it is retried after the next ch4 kick supplies data.
 *     A command whose input is larger than the FIFO commits instead of
 *     replaying, because hardware has no rewind: CSC after every
 *     macroblock, and BDEC once its macroblock is out, so its trailing
 *     start-code scan across this stream's zero stuffing (up to 18389
 *     bytes) carries the retry point with it. Without that the model asks
 *     IPU_BP to name a rewind point hundreds of quadwords back, which the
 *     register cannot encode, and the command can never finish once the
 *     ch4 chain runs dry.
 *     Two positions come out of that. The decode cursor (g_in_pos) is where
 *     the next attempt will start reading, and it is what IPU_BP reports:
 *     the library's restart rewinds the DMA to BP and re-sends those
 *     quadwords, so BP has to name the bits the replay will read, not the
 *     ones a rolled-back attempt got to. The high-water mark (g_in_hw) is
 *     how far the decoder has actually read, and only the DMA back pressure
 *     uses it, so a command larger than the FIFO can keep streaming instead
 *     of stalling against its own starting point.
 *   - BCLR discards the FIFO and sets BP, and a command waiting for input
 *     survives it: the library issues BCLR from inside its restart without
 *     waiting for BUSY, and the pending command resumes at the new BP.
 *   - Output is an unbounded byte queue drained by the fromIPU DMA (ch3,
 *     normal mode). A ch3 kick that cannot be satisfied yet stays pending
 *     (STR stays set) and completes when a command produces enough data.
 *     The drain is not gated on command completion: an armed ch3 takes
 *     each macroblock as the decoder produces it, because the library's
 *     snapshot spins on IPU_CTRL.OFC reading zero before it goes on
 *     (func_00240090.s at 0x240140-0x240158 masks the readback with 0xF0
 *     and loops while it is nonzero), and a BDEC that has emitted its
 *     macroblock and is still scanning would otherwise hold that wait open
 *     for the whole scan.
 *   - Command completion raises INTC cause 8 (IPU), matching hardware.
 *
 * Diagnostics: the sampled power-of-two lines below describe a healthy run
 * and go quiet exactly when one wedges, so the toIPU path also carries a
 * bounded unsampled event trace, on by default, prefixed "[trace N]". See
 * the comment on kTraceEvents for what it covers and how it is metered.
 *
 * The stop/restart round trip is verified rather than argued: the dma
 * passes in hw/ipu_selftest.cpp replay func_00240090/func_00240218 over the
 * real FMV bitstream at nine byte cadences, four ring sizes and with and
 * without a command pending, and require the decoded I picture to match the
 * uninterrupted decode exactly.
 *
 * Sources: EE User's Manual IPU chapter and ps2tek for the register layout,
 * command encodings and the documented integer CSC method. ISO/IEC 13818-2
 * tables B-1..B-15 for the VLC code assignments (transcribed below in this
 * file's own table format; the code/value assignments are facts fixed by
 * the standard). PCSX2's IPU was read as a behavioral reference for the
 * result-register encodings that the manual leaves vague (VDEC result =
 * value | length << 16, the CMD-read bitstream peek, the BDEC trailing
 * start-code scan, CTRL write masking); no code was copied from it, per the
 * license rules in CLAUDE.md. The IDCT is this file's own implementation of
 * the standard's definition (Annex A) in double precision.
 */
#include "hw.h"

#include "../ee/kernel.h"
#include "../prof.h"

#include <cinttypes>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

constexpr int RT_INTC_IPU = 8; /* ee/kernel.h cause numbering */

/* ---- bounded event trace -------------------------------------------------
 *
 * The power-of-two sampled lines the rest of this file emits describe a
 * healthy run and say nothing about a wedged one: once a counter is past
 * its last power of two, the event that finally goes wrong is silent. This
 * trace is the other half. Every event on the toIPU path gets one
 * unsampled line prefixed "[trace N]", so a log can be filtered down to the
 * channel's whole story with a single grep.
 *
 * It is bounded, because the alternative is an unbounded log. kTraceEvents
 * is the whole budget; when it is spent, one line says so and the sampled
 * lines are all that is left.
 *
 * Two classes are metered inside that budget so they cannot eat it. Both
 * are polled rather than driven by the channel, and both run to six
 * figures in a movie, which would spend 4096 lines in the first second and
 * leave the DMA events that matter untraced:
 *   - Command starts. The movie player scans for start codes with FDEC one
 *     byte at a time; the last Windows run issued 131072 of them.
 *   - Polled register states: the walk-stop reason and the IPU_BP
 *     readback, both answered inside guest spin loops. Consecutive lines
 *     with identical text also collapse into one followed by a count, so
 *     nothing is lost when the answer does not change.
 * What is left, the DMA events themselves (kick, tag, stop, completion,
 * BCLR, command stall), runs to a few hundred lines over a whole movie and
 * so is traced from the first field to the last.
 *
 * ICORECOMP_TRACE lifts every cap. */
constexpr uint64_t kTraceEvents = 4096;    /* whole budget */
constexpr uint64_t kTraceCmdStarts = 1024; /* share for command starts */
/* Share for polled states, per slot rather than shared between them. One
 * pool would let a new slot take the volume of an existing one: the
 * every-field "ch4 stopped when it was already idle" line was 584 of the
 * 3430 lines in the last Windows trace, and out of a single 1024-line pool
 * that is 584 IPU_BP reads the trace no longer shows. */
constexpr uint64_t kTracePolled = 1024;

/* Polled-state slots. Each keeps its own "last line" so two spin loops
 * interleaving (a walk stop and an IPU_BP read, say) still collapse. */
enum TraceSlot { TS_WALK_FULL, TS_WALK_HELD, TS_BP, TS_HOLD_LIFT, TS_STOP_IDLE, TS_COUNT };
const char* kTraceSlotName[TS_COUNT] = {
    "ch4 walk stops: input FIFO full",
    "ch4 walk stops: the DMAC is held",
    "IPU_BP read",
    "DMAC hold lifted with no IPU kick queued",
    "ch4 stopped when it was already idle",
};

struct TraceState {
    char last[400];
    bool valid;
    uint64_t repeat;
};
TraceState g_ts[TS_COUNT];

uint64_t g_trace_n = 0;        /* lines emitted */
uint64_t g_trace_cmds = 0;     /* command-start lines emitted */
uint64_t g_trace_polled[TS_COUNT] = {0}; /* polled-state lines emitted, per slot */
bool g_trace_spent = false;    /* budget notice already printed */

void trace_out(const char* text) {
    rt_log("ipu", "[trace %" PRIu64 "] %s", ++g_trace_n, text);
}

/* False once the whole budget is gone; says so once. */
bool trace_room() {
    if (rt_trace()) return true;
    if (g_trace_n < kTraceEvents) return true;
    if (!g_trace_spent) {
        g_trace_spent = true;
        rt_log("ipu", "[trace %" PRIu64 "] the %" PRIu64 "-line toIPU trace is full; from here the "
            "power-of-two sampled lines are all there is", g_trace_n, kTraceEvents);
    }
    return false;
}

/* True when a line was actually emitted, so a metered class only spends
 * its share on lines the log really got: a collapsed repeat costs nothing. */
bool trace_v(int slot, const char* fmt, va_list ap) {
    char buf[400];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    if (slot >= 0) {
        TraceState& st = g_ts[slot];
        if (st.valid && std::strcmp(buf, st.last) == 0) {
            ++st.repeat;
            return false;
        }
        if (!trace_room()) { st.valid = false; st.repeat = 0; return false; }
        if (st.repeat != 0) {
            /* Carried on the line that replaces them rather than a line of
             * its own, so collapsing a run of two still saves a line. */
            char note[560];
            std::snprintf(note, sizeof(note), "[the same %s answer was given %" PRIu64
                " more time%s first] %s", kTraceSlotName[slot], st.repeat,
                st.repeat == 1 ? "" : "s", buf);
            st.repeat = 0;
            trace_out(note);
        } else {
            trace_out(buf);
        }
        std::memcpy(st.last, buf, sizeof(st.last));
        st.valid = true;
        return true;
    }
    if (!trace_room()) return false;
    trace_out(buf);
    return true;
}

RT_PRINTF_FORMAT(1, 2) void trace(const char* fmt, ...);

/* True while a metered class may still emit; says so once when it stops. */
bool trace_share(uint64_t* used, uint64_t share, const char* what) {
    if (rt_trace()) return true;
    if (*used < share) return true;
    if (*used == share) {
        ++*used;
        trace("%s: the %" PRIu64 "-line share of the trace is used up; the rest of the run traces "
            "only the channel events", what, share);
    }
    return false;
}

/* One DMA event: never metered beyond the whole budget. */
void trace(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    trace_v(-1, fmt, ap);
    va_end(ap);
}

/* A polled register state: identical consecutive lines from the same slot
 * collapse into a count, and each slot has its own share of the budget. */
RT_PRINTF_FORMAT(2, 3) void trace_state(int slot, const char* fmt, ...);
void trace_state(int slot, const char* fmt, ...) {
    if (!trace_share(&g_trace_polled[slot], kTracePolled, kTraceSlotName[slot])) return;
    va_list ap;
    va_start(ap, fmt);
    if (trace_v(slot, fmt, ap)) ++g_trace_polled[slot];
    va_end(ap);
}

/* A command start, with its own share of the budget. */
RT_PRINTF_FORMAT(1, 2) void trace_cmd(const char* fmt, ...);
void trace_cmd(const char* fmt, ...) {
    if (!trace_share(&g_trace_cmds, kTraceCmdStarts, "command starts")) return;
    va_list ap;
    va_start(ap, fmt);
    if (trace_v(-1, fmt, ap)) ++g_trace_cmds;
    va_end(ap);
}

/* DMAtag ID names, in the DMAC chapter's order. */
const char* tag_name(uint32_t id) {
    static const char* kNames[8] = {"REFE", "CNT", "NEXT", "REF", "REFS", "CALL", "RET", "END"};
    return kNames[id & 7];
}

/* ---- guest memory access ------------------------------------------------- */

uint8_t* ipu_dma_ptr(uint32_t addr) {
    if (addr & 0x80000000u) {
        uint8_t* page = g_pages[0x70000000u >> 16];
        return page + (addr & 0x3FFF);
    }
    uint8_t* p = g_pages[addr >> 16];
    if (!p) {
        rt_fatal("ipu", nullptr, "IPU DMA touches unmapped guest address 0x%08x", addr);
    }
    return p + (addr & 0xFFFF);
}

/* ---- toIPU DMA (ch4) pull source ----------------------------------------- */

struct ToIpuState {
    bool active = false;
    bool chain = false;
    bool tag_end = false; /* current payload is the last (REFE/END or IRQ+TIE) */
    uint64_t kicks = 0;
    uint64_t tags = 0;      /* tags read since the current kick started */
    uint64_t delivered = 0; /* quadwords into the FIFO since the current kick */
};
ToIpuState g_to;

struct FromIpuState {
    bool active = false;
    bool drain_all = false; /* QWC=0 kick: complete when the output queue is empty */
    uint64_t kicks = 0;
};
FromIpuState g_from;

/* dmac.cpp register file pointers (0 CHCR, 1 MADR, 2 QWC, 3 TADR). */
uint32_t* g_ch3_reg[4];
uint32_t* g_ch4_reg[4];
bool g_reg_bound = false;

void bind_regs() {
    if (g_reg_bound) return;
    for (int i = 0; i < 4; ++i) {
        g_ch3_reg[i] = rt_dmac_ipu_reg(3, i);
        g_ch4_reg[i] = rt_dmac_ipu_reg(4, i);
    }
    g_reg_bound = true;
}

/* ---- bitstream input ----------------------------------------------------- */

/* Input FIFO depth in quadwords (EE User's Manual IPU chapter; ps2tek gives
 * the same 128-byte figure, and IPU_CTRL.IFC / IPU_BP.IFC are the 4-bit
 * fields that count it). The decoder holds one further quadword open while
 * it reads bits out of it, which is what IPU_BP.FP counts. */
constexpr size_t kFifoQw = 8;

std::vector<uint8_t> g_in;   /* resident input bytes (FIFO + decode window) */
size_t g_in_pos = 0;         /* decode cursor, in bits within g_in; rewinds on retry */
size_t g_in_hw = 0;          /* hardware bit position: the high-water mark of g_in_pos */
bool g_underflow = false;    /* set when a decode ran out of input */

/* Quadwords from the one holding bit position `at` through the end of what
 * the DMA has delivered. */
size_t qw_from(size_t at) {
    size_t total = (g_in.size() + 15) / 16;
    size_t head = at / 128;
    return total > head ? total - head : 0;
}

/* What the guest sees: IFC + FP, counted from the decode cursor.
 *
 * The cursor, not the high-water mark of an attempt that was rolled back. A
 * command that runs out of input rewinds to its start and will replay from
 * there, so its start is the position the library has to be handed back for
 * the replay to read the same bits. Reporting the high-water mark instead
 * makes MADR' = MADR - (IFC + FP) * 16 (func_002587E0.s at 0x258828) stop
 * short of bits the replay still needs, and the decode loses bitstream sync
 * at the restart. That is measured: hw/ipu_selftest.cpp's dma passes replay
 * the library bracket and only agree with the direct feed this way. */
size_t held_qw() { return qw_from(g_in_pos); }
uint32_t bp_bp() { return (uint32_t)(g_in_pos % 128); }

/* What the DMA is allowed to run ahead of: the furthest the decoder has
 * actually read. Hardware consumes as it decodes and never replays, so back
 * pressure follows that, not the rolled-back cursor. A command whose input
 * is larger than the FIFO (CSC takes 24 quadwords per macroblock) would
 * otherwise stall against its own starting point and never finish. */
size_t walk_qw() { return qw_from(g_in_hw); }

/* The quadword the decoder is inside counts as FP, the rest as IFC; the
 * split does not matter to the library (func_002587E0 and func_002407C0
 * only ever use IFC + FP), the sum does. */
uint32_t bp_fp() { return held_qw() ? 1u : 0u; }
/* Declared here so the IPU_BP overrun below can name the command that
 * rewound past what the register can encode; defined with the rest of the
 * decoder state further down. */
extern bool g_busy;
extern uint32_t g_cur_cmd;

/* IFC saturates at the FIFO depth, as the 4-bit field does on hardware. The
 * DMA bound keeps the real count inside that range while no command is
 * mid-replay; past that the register cannot describe the state the library
 * would have to rewind to, so say so rather than report a quiet lie. */
uint32_t bp_ifc() {
    size_t h = held_qw();
    if (h == 0) return 0;
    if (h > kFifoQw + 1) {
        static uint64_t n = 0;
        if (rt_trace() || is_pow2(++n)) {
            rt_log("ipu", "%zu quadwords sit between the decode cursor and the end of the input, more "
                "than the %zu the input FIFO holds; IPU_BP.IFC saturates and the library's "
                "MADR - (IFC + FP) * 16 rewind would stop short. The command that rewound this "
                "far is 0x%08x%s [#%" PRIu64 "]",
                h, kFifoQw + 1, g_busy ? g_cur_cmd : 0u,
                g_busy ? "" : " (none pending, so this is DMA run-ahead)", n);
        }
    }
    h -= 1;
    return (uint32_t)(h > kFifoQw ? kFifoQw : h);
}

void complete_ch4() {
    g_to.active = false;
    *g_ch4_reg[0] &= ~0x100u; /* STR */
    rt_dmac_raise(4);
    trace("ch4 walk ends: transfer complete, madr=0x%08x qwc=%u tadr=0x%08x chcr=0x%08x after %" PRIu64
        " tag%s and %" PRIu64 " qword%s this kick, %" PRIu64 " qword%s resident [kick #%" PRIu64 "]",
        *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3], *g_ch4_reg[0],
        g_to.tags, g_to.tags == 1 ? "" : "s", g_to.delivered, g_to.delivered == 1 ? "" : "s",
        (uint64_t)held_qw(), held_qw() == 1 ? "" : "s", g_to.kicks);
    if (rt_trace() || is_pow2(g_to.kicks)) {
        rt_log("ipu", "toIPU ch4 transfer complete (madr=0x%08x qwc=%u tadr=0x%08x) with %zu qword%s "
            "still resident [kick #%" PRIu64 "]",
            *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3], held_qw(), held_qw() == 1 ? "" : "s",
            g_to.kicks);
    }
}

/* Runs the ch4 source (normal mode or source chain) as far as it will go.
 *
 * The DMAC does not wait to be asked. It walks the chain on its own,
 * transferring each tag's payload into the IPU's 8-quadword input FIFO as
 * space allows, and the transfer is over the moment the last tag (REFE, END,
 * or a tag with IRQ set while CHCR.TIE is set) has had all its payload
 * transferred: STR clears, QWC is 0, TADR and MADR sit past the last tag,
 * and the channel interrupt goes up. None of that waits for the IPU to
 * consume what is in the FIFO. (EE User's Manual, DMAC chapter, source chain
 * mode and the transfer-end conditions; IPU chapter for the input FIFO the
 * channel writes into.)
 *
 * Two consequences the model has to keep apart. Copying a payload quadword
 * needs FIFO room, so a full FIFO stalls the copy. Reading a tag does not
 * put anything in the FIFO (TTE is 0 here), so a full FIFO must not stop the
 * walk from reading the tag that ends the chain: otherwise a chain whose
 * last tag carries no payload would never complete while the decoder waits
 * for the library and the library waits for the completion.
 *
 * MADR, QWC and TADR still only ever describe what actually entered the
 * FIFO, which is the property the library's stop/restart arithmetic depends
 * on; see the file header. */
void advance_walk() {
    if (!g_to.active) return;
    /* No channel advances while the DMAC is held (D_ENABLE suspend bit or
     * D_CTRL.DMAE=0). The MPEG library reads ch4 MADR/TADR/QWC back inside
     * that window to work out how much the IPU consumed, so this walk must
     * leave them exactly where the library put them. The stall is the
     * hardware behaviour: the IPU waits for input until the hold lifts. */
    if (rt_dmac_suspended()) {
        trace_state(TS_WALK_HELD, "ch4 walk stops: the DMAC is held, madr=0x%08x qwc=%u tadr=0x%08x",
            *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3]);
        static uint64_t n = 0;
        if (rt_trace() || is_pow2(++n)) {
            rt_log("ipu", "toIPU walk held off: the DMAC is suspended [#%" PRIu64 "]", n);
        }
        return;
    }
    if (!(*g_ch4_reg[0] & 0x100u)) {
        /* The driver stopped the channel manually (libmpeg's stop/restart
         * sequence writes CHCR without STR under D_ENABLE suspend). */
        trace("ch4 walk stops: CHCR.STR is clear (chcr=0x%08x), madr=0x%08x qwc=%u tadr=0x%08x",
            *g_ch4_reg[0], *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3]);
        g_to.active = false;
        return;
    }
    for (;;) {
        if (*g_ch4_reg[2] > 0) {
            if (walk_qw() > kFifoQw) {
                /* Input FIFO full: the payload copy stalls until the decoder
                 * frees a quadword. This is ordinary back pressure, but it
                 * is also the state the channel used to wedge in, so name
                 * it. */
                trace_state(TS_WALK_FULL, "ch4 walk stops: input FIFO full (%" PRIu64 " qword%s past the "
                    "hardware bit position, %" PRIu64 " past the decode cursor, depth %" PRIu64 "), qwc=%u "
                    "left at madr=0x%08x tadr=0x%08x", (uint64_t)walk_qw(), walk_qw() == 1 ? "" : "s",
                    (uint64_t)held_qw(), (uint64_t)kFifoQw,
                    *g_ch4_reg[2], *g_ch4_reg[1], *g_ch4_reg[3]);
                static uint64_t n = 0;
                if (rt_trace() || is_pow2(++n)) {
                    rt_log("ipu", "toIPU walk stalls: input FIFO full with qwc=%u left at madr=0x%08x "
                        "(tadr=0x%08x) [#%" PRIu64 "]", *g_ch4_reg[2], *g_ch4_reg[1], *g_ch4_reg[3], n);
                }
                return;
            }
            uint32_t madr = *g_ch4_reg[1];
            size_t base = g_in.size();
            g_in.resize(base + 16);
            std::memcpy(g_in.data() + base, ipu_dma_ptr(madr), 16);
            *g_ch4_reg[1] = madr + 16;
            *g_ch4_reg[2] -= 1;
            ++g_to.delivered;
            if (*g_ch4_reg[2] == 0 && (!g_to.chain || g_to.tag_end)) {
                complete_ch4();
                return;
            }
            continue;
        }
        if (!g_to.chain || g_to.tag_end) {
            complete_ch4();
            return;
        }
        /* Read the next source-chain tag. The count is per kick, not per
         * run: a lifetime counter would fatal on a long movie that walked
         * more than 65536 tags legitimately, and the runaway it is guarding
         * against is a chain that never reaches an end tag inside one
         * kick. */
        if (++g_to.tags > 65536) {
            rt_dmac_dump_recent_tags(4);
            rt_fatal("ipu", nullptr, "toIPU source chain exceeded 65536 tags in one kick; runaway TADR=0x%08x",
                *g_ch4_reg[3]);
        }
        uint32_t tadr = *g_ch4_reg[3];
        uint64_t lo;
        std::memcpy(&lo, ipu_dma_ptr(tadr), 8);
        if (lo == 0) {
            /* An all-zero quadword is not a tag the retail ring builder can
             * have written: every tag it stores carries a payload address
             * and QWC 0x80 (func_0023FDF0.s at 0x23FF64-0x23FFC8). So the
             * walk has caught up with the producer and is looking at a ring
             * slot the guest has not filled yet. That is the ordinary
             * steady state whenever the decoder drains the ring faster than
             * the demux refills it, not an error.
             *
             * The walk stops here and leaves TADR on that slot. Read as raw
             * DMAtag bits the zeros are a REFE with QWC 0 and ADDR 0, which
             * would end the transfer and advance TADR one slot; that is
             * what a DMAC reading those bits would do. It is not what this
             * guest can have meant, because what it does next is write this
             * very slot and restart at this same TADR (func_0023FDF0.s at
             * 0x23FF90-0x23FFC8, then the CHCR store at 0x240054), so
             * consuming the slot would lose 2048 bytes of bitstream and put
             * every later append behind the walk. Which of the two hardware
             * does is not measured, so the choice is the one that keeps the
             * guest's own protocol working, and it is stated here and
             * logged rather than left silent. */
            static uint64_t n = 0;
            if (rt_trace() || is_pow2(++n)) {
                rt_log("ipu", "toIPU chain has caught up with the ring: the tag at TADR=0x%08x has "
                    "not been written yet (madr=0x%08x qwc=%u chcr=0x%08x, %zu qword%s resident). The "
                    "walk stops with TADR left on that slot, so the guest's next ring extension is "
                    "still read [#%" PRIu64 "]",
                    tadr, *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[0], held_qw(),
                    held_qw() == 1 ? "" : "s", n);
            }
            trace("ch4 walk stops: the tag at TADR=0x%08x has not been written yet (an unfilled ring "
                "slot); TADR left where it is", tadr);
            --g_to.tags; /* nothing was consumed */
            complete_ch4();
            return;
        }
        uint32_t qwc = (uint32_t)(lo & 0xFFFF);
        uint32_t id = (uint32_t)((lo >> 28) & 7);
        bool irq = (lo >> 31) & 1;
        uint32_t taddr = (uint32_t)((lo >> 32) & 0x7FFFFFF0u);
        bool tspr = (lo >> 63) & 1;
        bool tie = (*g_ch4_reg[0] >> 7) & 1;
        /* CHCR.TAG mirrors tag bits 16-31. */
        *g_ch4_reg[0] = (*g_ch4_reg[0] & 0xFFFFu) | ((uint32_t)(lo >> 16) & 0xFFFF0000u);
        if ((*g_ch4_reg[0] >> 6) & 1) {
            static uint64_t n = 0;
            if (is_pow2(++n)) rt_log("ipu", "toIPU chain tag with TTE set; tag words 2-3 dropped [#%" PRIu64 "]", n);
        }
        switch (id) {
            case 0: /* REFE */
                *g_ch4_reg[1] = taddr | (tspr ? 0x80000000u : 0);
                *g_ch4_reg[2] = qwc;
                *g_ch4_reg[3] = tadr + 16;
                g_to.tag_end = true;
                break;
            case 1: /* CNT */
                *g_ch4_reg[1] = tadr + 16;
                *g_ch4_reg[2] = qwc;
                *g_ch4_reg[3] = tadr + 16 + qwc * 16;
                break;
            case 2: /* NEXT */
                *g_ch4_reg[1] = tadr + 16;
                *g_ch4_reg[2] = qwc;
                *g_ch4_reg[3] = taddr | (tspr ? 0x80000000u : 0);
                break;
            case 3: /* REF */
            case 4: /* REFS */
                *g_ch4_reg[1] = taddr | (tspr ? 0x80000000u : 0);
                *g_ch4_reg[2] = qwc;
                *g_ch4_reg[3] = tadr + 16;
                break;
            case 7: /* END */
                *g_ch4_reg[1] = tadr + 16;
                *g_ch4_reg[2] = qwc;
                *g_ch4_reg[3] = tadr + 16 + qwc * 16;
                g_to.tag_end = true;
                break;
            default: /* CALL/RET have no ASR on ch4 */
                rt_dmac_dump_recent_tags(4);
                rt_fatal("ipu", nullptr, "toIPU chain tag id %u (CALL/RET) not supported on ch4", id);
        }
        if (irq && tie) g_to.tag_end = true;
        /* The dump ring lives in hw/dmac.cpp and is filled by its own chain
         * walker, which never runs for ch4; record here so a ch4 fatal can
         * still name the tags it came from. */
        rt_dmac_record_tag(4, tadr, id, qwc, taddr);
        trace("ch4 tag %" PRIu64 " @0x%08x: %s qwc=%u addr=0x%08x irq=%u tie=%u -> madr=0x%08x qwc=%u "
            "tadr=0x%08x%s", g_to.tags, tadr, tag_name(id), qwc, taddr, irq ? 1u : 0u, tie ? 1u : 0u,
            *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3], g_to.tag_end ? " (ends the chain)" : "");
        if (*g_ch4_reg[2] == 0 && g_to.tag_end) {
            complete_ch4();
            return;
        }
    }
}

/* Makes more input available if the walk can still deliver any. Reports
 * whether anything new reached the FIFO, so a caller waiting on bits knows
 * when to stop asking. */
bool pull_qword() {
    size_t before = g_in.size();
    advance_walk();
    return g_in.size() > before;
}

/* Bits still to decode. BCLR leaves the cursor pointing partway into the
 * first quadword that has not arrived yet (g_in_pos > g_in.size() * 8), so
 * this saturates at zero instead of wrapping. */
size_t avail_bits() {
    size_t total = g_in.size() * 8;
    return total > g_in_pos ? total - g_in_pos : 0;
}

bool ensure_bits(size_t n) {
    /* A single request larger than the FIFO can hold could never be
     * satisfied once the DMA is FIFO-bounded: it would stall, rewind and
     * stall again forever. No command in this binary's census does that
     * (the largest is SETIQ's 64 bytes), so treat it as a model error. */
    if (n > kFifoQw * 128) {
        rt_fatal("ipu", nullptr, "a command asked for %zu bits at once; more than the %zu-bit input "
            "FIFO, so the request could never be satisfied once the FIFO is full", n, kFifoQw * 128);
    }
    while (avail_bits() < n) {
        if (!pull_qword()) {
            g_underflow = true;
            return false;
        }
    }
    return true;
}

/* MSB-first peek of up to 32 bits at the current position. Caller must have
 * ensured availability. */
uint32_t peek_bits(unsigned n) {
    uint64_t acc = 0;
    size_t byte = g_in_pos >> 3;
    unsigned sh = (unsigned)(g_in_pos & 7);
    for (int i = 0; i < 6; ++i) {
        acc = (acc << 8) | (byte + i < g_in.size() ? g_in[byte + i] : 0);
    }
    /* acc holds 48 bits starting at byte; drop the sub-byte offset. */
    acc = (acc >> (48 - sh - n)) & ((n == 32) ? 0xFFFFFFFFull : ((1ull << n) - 1));
    return (uint32_t)acc;
}

void advance_bits(size_t n) {
    g_in_pos += n;
    /* The hardware bit position only moves forward: a retry replays bits the
     * hardware decoder had already taken out of the FIFO, and the guest must
     * not see them come back. */
    if (g_in_pos > g_in_hw) g_in_hw = g_in_pos;
}

uint32_t get_bits(unsigned n) {
    uint32_t v = peek_bits(n);
    advance_bits(n);
    return v;
}

int32_t get_signed(unsigned n) {
    int32_t v = (int32_t)peek_bits(n);
    advance_bits(n);
    v <<= (32 - n);
    v >>= (32 - n);
    return v;
}

/* Retire quadwords the decoder has left behind.
 *
 * Callable with a command mid-flight, because commit_scan pairs it with a
 * fresh take_snapshot: the invariant is that a retry rewinds to the
 * snapshot, and the snapshot is taken after the compaction, never before
 * it. Whole quadwords only: the FIFO is a quadword queue and IPU_BP.BP is
 * an offset inside its head, so the grid the position arithmetic runs on
 * must not shift. */
void compact_in() {
    size_t drop = (g_in_pos / 128) * 16;
    if (drop > g_in.size()) drop = (g_in.size() / 16) * 16;
    if (drop == 0) return;
    g_in.erase(g_in.begin(), g_in.begin() + (ptrdiff_t)drop);
    g_in_pos -= drop * 8;
    g_in_hw = g_in_hw > drop * 8 ? g_in_hw - drop * 8 : 0;
}

/* ---- output queue and fromIPU DMA (ch3) ---------------------------------- */

std::vector<uint8_t> g_out;
size_t g_out_head = 0;
uint64_t g_out_produced = 0; /* bytes ever pushed; the retry snapshot's mark */

size_t out_avail() { return g_out.size() - g_out_head; }

void out_push(const void* data, size_t len) {
    size_t base = g_out.size();
    g_out.resize(base + len);
    std::memcpy(g_out.data() + base, data, len);
    g_out_produced += len;
}

void complete_ch3() {
    g_from.active = false;
    g_from.drain_all = false;
    *g_ch3_reg[0] &= ~0x100u;
    rt_dmac_raise(3);
    if (rt_trace() || is_pow2(g_from.kicks)) {
        rt_log("ipu", "fromIPU ch3 transfer complete (madr=0x%08x) [kick #%" PRIu64 "]",
            *g_ch3_reg[1], g_from.kicks);
    }
}

void drain_ch3() {
    if (!g_from.active) return;
    if (!(*g_ch3_reg[0] & 0x100u)) {
        g_from.active = false; /* stopped manually by the driver */
        g_from.drain_all = false;
        return;
    }
    while ((*g_ch3_reg[2] > 0 || g_from.drain_all) && out_avail() >= 16) {
        std::memcpy(ipu_dma_ptr(*g_ch3_reg[1]), g_out.data() + g_out_head, 16);
        g_out_head += 16;
        *g_ch3_reg[1] += 16;
        if (*g_ch3_reg[2] > 0) *g_ch3_reg[2] -= 1;
    }
    if (g_out_head == g_out.size()) {
        g_out.clear();
        g_out_head = 0;
    }
    if (g_from.drain_all ? out_avail() == 0 : *g_ch3_reg[2] == 0) complete_ch3();
}

/* ---- register / decoder state -------------------------------------------- */

uint32_t g_ctrl_bits = 0; /* stored IDP/AS/IVF/QST/MP1/PCT (bits 16-26) */
bool g_ecd = false, g_scd = false;
uint32_t g_cbp_reg = 0;
uint32_t g_cmd_data = 0;  /* IPU_CMD read result (VDEC/FDEC) */
uint32_t g_top = 0;
bool g_busy = false;      /* command pending on input */
size_t g_stall_in = 0;    /* g_in.size() when the pending command last stalled */
uint32_t g_cur_cmd = 0;
uint32_t g_last_cmd_code = 0xF; /* command nibble of the last accepted command */

uint8_t g_iq[64];  /* intra quantizer matrix, transmission (zigzag) order */
uint8_t g_niq[64]; /* non-intra */
uint8_t g_vq[32];  /* VQCLUT, stored only (PACK is not used) */
uint16_t g_th[2] = {0, 0};

int g_dcpred[3];

/* Retry snapshot: taken at command acceptance, restored on input underflow. */
struct Snapshot {
    size_t in_pos;
    uint64_t out_produced;
    int dcpred[3];
    uint32_t cbp;
};
Snapshot g_snap;

uint64_t g_cmd_census[16];

int ctrl_idp() { return (g_ctrl_bits >> 16) & 3; }
int ctrl_as() { return (g_ctrl_bits >> 20) & 1; }
int ctrl_ivf() { return (g_ctrl_bits >> 21) & 1; }
int ctrl_qst() { return (g_ctrl_bits >> 22) & 1; }
int ctrl_mp1() { return (g_ctrl_bits >> 23) & 1; }
int ctrl_pct() { return (g_ctrl_bits >> 24) & 7; }

/* ---- VLC tables ----------------------------------------------------------
 * Code assignments are ISO/IEC 13818-2 facts: B-1 (macroblock address
 * increment), B-2/3/4 (macroblock type), B-9 (coded block pattern), B-10
 * (motion code), B-11 (dmvector), B-12/13 (DC size), B-14/15 (DCT
 * coefficients). Entries are {code, length, value...}; a code is matched
 * when the next `length` bits equal `code`. Sign bits, escape payloads and
 * the first-coefficient rule are handled by the decoders below. */

struct Vlc {
    uint16_t code;
    uint8_t len;
    int16_t val;
};

/* B-1. Values 1..33; 0x22 = stuffing (MPEG1 only), 0x23 = escape; the
 * escape/stuffing VDEC result encoding (0xb0022/0xb0023) matches what the
 * libmpeg driver in this binary tests for. */
constexpr Vlc kMbai[] = {
    {0x0001, 1, 1}, {0x0002, 3, 3}, {0x0003, 3, 2}, {0x0002, 4, 5},
    {0x0003, 4, 4}, {0x0002, 5, 7}, {0x0003, 5, 6}, {0x0006, 7, 9},
    {0x0007, 7, 8}, {0x0006, 8, 15}, {0x0007, 8, 14}, {0x0008, 8, 13},
    {0x0009, 8, 12}, {0x000A, 8, 11}, {0x000B, 8, 10}, {0x0012, 10, 21},
    {0x0013, 10, 20}, {0x0014, 10, 19}, {0x0015, 10, 18}, {0x0016, 10, 17},
    {0x0017, 10, 16}, {0x0008, 11, 0x23}, {0x000F, 11, 0x22}, {0x0018, 11, 33},
    {0x0019, 11, 32}, {0x001A, 11, 31}, {0x001B, 11, 30}, {0x001C, 11, 29},
    {0x001D, 11, 28}, {0x001E, 11, 27}, {0x001F, 11, 26}, {0x0020, 11, 25},
    {0x0021, 11, 24}, {0x0022, 11, 23}, {0x0023, 11, 22},
};

/* Macroblock type flag values in the VDEC result (hardware encoding,
 * confirmed by what this binary's driver masks): */
constexpr int MB_INTRA = 1, MB_PATTERN = 2, MB_BACKWARD = 4, MB_FORWARD = 8, MB_QUANT = 16;
constexpr int MB_MC_FRAME = 128; /* motion_type frame, bits 6-7 = 2 */

constexpr Vlc kMbtI[] = {
    {0x0001, 1, MB_INTRA}, {0x0001, 2, MB_INTRA | MB_QUANT},
};
constexpr Vlc kMbtP[] = {
    {0x0001, 1, MB_FORWARD | MB_PATTERN}, {0x0001, 2, MB_PATTERN},
    {0x0001, 3, MB_FORWARD}, {0x0001, 5, MB_QUANT | MB_PATTERN},
    {0x0002, 5, MB_QUANT | MB_FORWARD | MB_PATTERN}, {0x0003, 5, MB_INTRA},
    {0x0001, 6, MB_QUANT | MB_INTRA},
};
constexpr Vlc kMbtB[] = {
    {0x0002, 2, MB_FORWARD | MB_BACKWARD},
    {0x0003, 2, MB_FORWARD | MB_BACKWARD | MB_PATTERN},
    {0x0002, 3, MB_BACKWARD}, {0x0003, 3, MB_BACKWARD | MB_PATTERN},
    {0x0002, 4, MB_FORWARD}, {0x0003, 4, MB_FORWARD | MB_PATTERN},
    {0x0002, 5, MB_QUANT | MB_FORWARD | MB_BACKWARD | MB_PATTERN},
    {0x0003, 5, MB_INTRA},
    {0x0001, 6, MB_QUANT | MB_INTRA},
    {0x0002, 6, MB_QUANT | MB_BACKWARD | MB_PATTERN},
    {0x0003, 6, MB_QUANT | MB_FORWARD | MB_PATTERN},
};

/* B-10 motion_code magnitude (the sign bit follows the code). */
constexpr Vlc kMotion[] = {
    {0x0001, 1, 0}, {0x0001, 2, 1}, {0x0001, 3, 2}, {0x0001, 4, 3},
    {0x0003, 6, 4}, {0x0003, 7, 7}, {0x0004, 7, 6}, {0x0005, 7, 5},
    {0x0009, 9, 10}, {0x000A, 9, 9}, {0x000B, 9, 8}, {0x000C, 10, 16},
    {0x000D, 10, 15}, {0x000E, 10, 14}, {0x000F, 10, 13}, {0x0010, 10, 12},
    {0x0011, 10, 11},
};

constexpr Vlc kDmv[] = {
    {0x0000, 1, 0}, {0x0002, 2, 1}, {0x0003, 2, -1},
};

constexpr Vlc kCbp[] = {
    {0x0007, 3, 0x3C}, {0x000A, 4, 0x20}, {0x000B, 4, 0x10}, {0x000C, 4, 0x08},
    {0x000D, 4, 0x04}, {0x0008, 5, 0x3E}, {0x0009, 5, 0x02}, {0x000A, 5, 0x3D},
    {0x000B, 5, 0x01}, {0x000C, 5, 0x38}, {0x000D, 5, 0x34}, {0x000E, 5, 0x2C},
    {0x000F, 5, 0x1C}, {0x0010, 5, 0x28}, {0x0011, 5, 0x14}, {0x0012, 5, 0x30},
    {0x0013, 5, 0x0C}, {0x000C, 6, 0x3F}, {0x000D, 6, 0x03}, {0x000E, 6, 0x24},
    {0x000F, 6, 0x18}, {0x0010, 7, 0x22}, {0x0011, 7, 0x12}, {0x0012, 7, 0x0A},
    {0x0013, 7, 0x06}, {0x0014, 7, 0x21}, {0x0015, 7, 0x11}, {0x0016, 7, 0x09},
    {0x0017, 7, 0x05}, {0x0004, 8, 0x3A}, {0x0005, 8, 0x36}, {0x0006, 8, 0x2E},
    {0x0007, 8, 0x1E}, {0x0008, 8, 0x39}, {0x0009, 8, 0x35}, {0x000A, 8, 0x2D},
    {0x000B, 8, 0x1D}, {0x000C, 8, 0x26}, {0x000D, 8, 0x1A}, {0x000E, 8, 0x25},
    {0x000F, 8, 0x19}, {0x0010, 8, 0x2B}, {0x0011, 8, 0x17}, {0x0012, 8, 0x33},
    {0x0013, 8, 0x0F}, {0x0014, 8, 0x2A}, {0x0015, 8, 0x16}, {0x0016, 8, 0x32},
    {0x0017, 8, 0x0E}, {0x0018, 8, 0x29}, {0x0019, 8, 0x15}, {0x001A, 8, 0x31},
    {0x001B, 8, 0x0D}, {0x001C, 8, 0x23}, {0x001D, 8, 0x13}, {0x001E, 8, 0x0B},
    {0x001F, 8, 0x07}, {0x0001, 9, 0x00}, {0x0002, 9, 0x27}, {0x0003, 9, 0x1B},
    {0x0004, 9, 0x3B}, {0x0005, 9, 0x37}, {0x0006, 9, 0x2F}, {0x0007, 9, 0x1F},
};

constexpr Vlc kDcLuma[] = {
    {0x0000, 2, 1}, {0x0001, 2, 2}, {0x0004, 3, 0}, {0x0005, 3, 3},
    {0x0006, 3, 4}, {0x000E, 4, 5}, {0x001E, 5, 6}, {0x003E, 6, 7},
    {0x007E, 7, 8}, {0x00FE, 8, 9}, {0x01FE, 9, 10}, {0x01FF, 9, 11},
};
constexpr Vlc kDcChroma[] = {
    {0x0000, 2, 0}, {0x0001, 2, 1}, {0x0002, 2, 2}, {0x0006, 3, 3},
    {0x000E, 4, 4}, {0x001E, 5, 5}, {0x003E, 6, 6}, {0x007E, 7, 7},
    {0x00FE, 8, 8}, {0x01FE, 9, 9}, {0x03FE, 10, 10}, {0x03FF, 10, 11},
};

/* B-14/B-15 DCT coefficient codes. run 64 = end of block, 65 = escape. */
struct DctVlc {
    uint16_t code;
    uint8_t len;
    uint8_t run;
    uint8_t level;
};

/* B-14; the first-coefficient rule ('1s' = (0,1) as the first coefficient
 * of a non-intra block) is handled in dct_vlc_decode. */
constexpr DctVlc kDct14[] = {
    {0x0002, 2, 64, 0}, {0x0003, 2, 0, 1}, {0x0003, 3, 1, 1}, {0x0004, 4, 0, 2},
    {0x0005, 4, 2, 1}, {0x0005, 5, 0, 3}, {0x0006, 5, 4, 1}, {0x0007, 5, 3, 1},
    {0x0001, 6, 65, 0}, {0x0004, 6, 7, 1}, {0x0005, 6, 6, 1}, {0x0006, 6, 1, 2},
    {0x0007, 6, 5, 1}, {0x0004, 7, 2, 2}, {0x0005, 7, 9, 1}, {0x0006, 7, 0, 4},
    {0x0007, 7, 8, 1}, {0x0020, 8, 13, 1}, {0x0021, 8, 0, 6}, {0x0022, 8, 12, 1},
    {0x0023, 8, 11, 1}, {0x0024, 8, 3, 2}, {0x0025, 8, 1, 3}, {0x0026, 8, 0, 5},
    {0x0027, 8, 10, 1}, {0x0008, 10, 16, 1}, {0x0009, 10, 5, 2}, {0x000A, 10, 0, 7},
    {0x000B, 10, 2, 3}, {0x000C, 10, 1, 4}, {0x000D, 10, 15, 1}, {0x000E, 10, 14, 1},
    {0x000F, 10, 4, 2}, {0x0010, 12, 0, 11}, {0x0011, 12, 8, 2}, {0x0012, 12, 4, 3},
    {0x0013, 12, 0, 10}, {0x0014, 12, 2, 4}, {0x0015, 12, 7, 2}, {0x0016, 12, 21, 1},
    {0x0017, 12, 20, 1}, {0x0018, 12, 0, 9}, {0x0019, 12, 19, 1}, {0x001A, 12, 18, 1},
    {0x001B, 12, 1, 5}, {0x001C, 12, 3, 3}, {0x001D, 12, 0, 8}, {0x001E, 12, 6, 2},
    {0x001F, 12, 17, 1}, {0x0010, 13, 10, 2}, {0x0011, 13, 9, 2}, {0x0012, 13, 5, 3},
    {0x0013, 13, 3, 4}, {0x0014, 13, 2, 5}, {0x0015, 13, 1, 7}, {0x0016, 13, 1, 6},
    {0x0017, 13, 0, 15}, {0x0018, 13, 0, 14}, {0x0019, 13, 0, 13}, {0x001A, 13, 0, 12},
    {0x001B, 13, 26, 1}, {0x001C, 13, 25, 1}, {0x001D, 13, 24, 1}, {0x001E, 13, 23, 1},
    {0x001F, 13, 22, 1}, {0x0010, 14, 0, 31}, {0x0011, 14, 0, 30}, {0x0012, 14, 0, 29},
    {0x0013, 14, 0, 28}, {0x0014, 14, 0, 27}, {0x0015, 14, 0, 26}, {0x0016, 14, 0, 25},
    {0x0017, 14, 0, 24}, {0x0018, 14, 0, 23}, {0x0019, 14, 0, 22}, {0x001A, 14, 0, 21},
    {0x001B, 14, 0, 20}, {0x001C, 14, 0, 19}, {0x001D, 14, 0, 18}, {0x001E, 14, 0, 17},
    {0x001F, 14, 0, 16}, {0x0010, 15, 0, 40}, {0x0011, 15, 0, 39}, {0x0012, 15, 0, 38},
    {0x0013, 15, 0, 37}, {0x0014, 15, 0, 36}, {0x0015, 15, 0, 35}, {0x0016, 15, 0, 34},
    {0x0017, 15, 0, 33}, {0x0018, 15, 0, 32}, {0x0019, 15, 1, 14}, {0x001A, 15, 1, 13},
    {0x001B, 15, 1, 12}, {0x001C, 15, 1, 11}, {0x001D, 15, 1, 10}, {0x001E, 15, 1, 9},
    {0x001F, 15, 1, 8}, {0x0010, 16, 1, 18}, {0x0011, 16, 1, 17}, {0x0012, 16, 1, 16},
    {0x0013, 16, 1, 15}, {0x0014, 16, 6, 3}, {0x0015, 16, 16, 2}, {0x0016, 16, 15, 2},
    {0x0017, 16, 14, 2}, {0x0018, 16, 13, 2}, {0x0019, 16, 12, 2}, {0x001A, 16, 11, 2},
    {0x001B, 16, 31, 1}, {0x001C, 16, 30, 1}, {0x001D, 16, 29, 1}, {0x001E, 16, 28, 1},
    {0x001F, 16, 27, 1},
};

constexpr DctVlc kDct15[] = {
    {0x0002, 2, 0, 1}, {0x0002, 3, 1, 1}, {0x0006, 3, 0, 2}, {0x0006, 4, 64, 0},
    {0x0007, 4, 0, 3}, {0x0005, 5, 2, 1}, {0x0006, 5, 1, 2}, {0x0007, 5, 3, 1},
    {0x001C, 5, 0, 4}, {0x001D, 5, 0, 5}, {0x0001, 6, 65, 0}, {0x0004, 6, 0, 7},
    {0x0005, 6, 0, 6}, {0x0006, 6, 4, 1}, {0x0007, 6, 5, 1}, {0x0004, 7, 7, 1},
    {0x0005, 7, 8, 1}, {0x0006, 7, 6, 1}, {0x0007, 7, 2, 2}, {0x0078, 7, 9, 1},
    {0x0079, 7, 1, 3}, {0x007A, 7, 10, 1}, {0x007B, 7, 0, 8}, {0x007C, 7, 0, 9},
    {0x0020, 8, 1, 5}, {0x0021, 8, 11, 1}, {0x0022, 8, 0, 11}, {0x0023, 8, 0, 10},
    {0x0024, 8, 13, 1}, {0x0025, 8, 12, 1}, {0x0026, 8, 3, 2}, {0x0027, 8, 1, 4},
    {0x00FA, 8, 0, 12}, {0x00FB, 8, 0, 13}, {0x00FC, 8, 2, 3}, {0x00FD, 8, 4, 2},
    {0x00FE, 8, 0, 14}, {0x00FF, 8, 0, 15}, {0x0004, 9, 5, 2}, {0x0005, 9, 14, 1},
    {0x0007, 9, 15, 1}, {0x000C, 10, 2, 4}, {0x000D, 10, 16, 1}, {0x0010, 12, 0, 11},
    {0x0011, 12, 8, 2}, {0x0012, 12, 4, 3}, {0x0013, 12, 0, 10}, {0x0014, 12, 2, 4},
    {0x0015, 12, 7, 2}, {0x0016, 12, 21, 1}, {0x0017, 12, 20, 1}, {0x0018, 12, 0, 9},
    {0x0019, 12, 19, 1}, {0x001A, 12, 18, 1}, {0x001B, 12, 1, 5}, {0x001C, 12, 3, 3},
    {0x001D, 12, 0, 8}, {0x001E, 12, 6, 2}, {0x001F, 12, 17, 1}, {0x0010, 13, 10, 2},
    {0x0011, 13, 9, 2}, {0x0012, 13, 5, 3}, {0x0013, 13, 3, 4}, {0x0014, 13, 2, 5},
    {0x0015, 13, 1, 7}, {0x0016, 13, 1, 6}, {0x0017, 13, 0, 15}, {0x0018, 13, 0, 14},
    {0x0019, 13, 0, 13}, {0x001A, 13, 0, 12}, {0x001B, 13, 26, 1}, {0x001C, 13, 25, 1},
    {0x001D, 13, 24, 1}, {0x001E, 13, 23, 1}, {0x001F, 13, 22, 1}, {0x0010, 14, 0, 31},
    {0x0011, 14, 0, 30}, {0x0012, 14, 0, 29}, {0x0013, 14, 0, 28}, {0x0014, 14, 0, 27},
    {0x0015, 14, 0, 26}, {0x0016, 14, 0, 25}, {0x0017, 14, 0, 24}, {0x0018, 14, 0, 23},
    {0x0019, 14, 0, 22}, {0x001A, 14, 0, 21}, {0x001B, 14, 0, 20}, {0x001C, 14, 0, 19},
    {0x001D, 14, 0, 18}, {0x001E, 14, 0, 17}, {0x001F, 14, 0, 16}, {0x0010, 15, 0, 40},
    {0x0011, 15, 0, 39}, {0x0012, 15, 0, 38}, {0x0013, 15, 0, 37}, {0x0014, 15, 0, 36},
    {0x0015, 15, 0, 35}, {0x0016, 15, 0, 34}, {0x0017, 15, 0, 33}, {0x0018, 15, 0, 32},
    {0x0019, 15, 1, 14}, {0x001A, 15, 1, 13}, {0x001B, 15, 1, 12}, {0x001C, 15, 1, 11},
    {0x001D, 15, 1, 10}, {0x001E, 15, 1, 9}, {0x001F, 15, 1, 8}, {0x0010, 16, 1, 18},
    {0x0011, 16, 1, 17}, {0x0012, 16, 1, 16}, {0x0013, 16, 1, 15}, {0x0014, 16, 6, 3},
    {0x0015, 16, 16, 2}, {0x0016, 16, 15, 2}, {0x0017, 16, 14, 2}, {0x0018, 16, 13, 2},
    {0x0019, 16, 12, 2}, {0x001A, 16, 11, 2}, {0x001B, 16, 31, 1}, {0x001C, 16, 30, 1},
    {0x001D, 16, 29, 1}, {0x001E, 16, 28, 1}, {0x001F, 16, 27, 1},
};

/* Non-linear quantiser scale, ISO 13818-2 7.4.2.2 (q_scale_type = 1). */
constexpr int kNonLinearQ[32] = {
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 10, 12, 14, 16, 18, 20, 22,
    24, 28, 32, 36, 40, 44, 48, 52,
    56, 64, 72, 80, 88, 96, 104, 112,
};

/* Inverse scan patterns, ISO 13818-2 figures 7-2 / 7-3. */
constexpr uint8_t kScanZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
constexpr uint8_t kScanAlt[64] = {
    0, 8, 16, 24, 1, 9, 2, 10, 17, 25, 32, 40, 48, 56, 57, 49,
    41, 33, 26, 18, 3, 11, 4, 12, 19, 27, 34, 42, 50, 58, 35, 43,
    51, 59, 20, 28, 5, 13, 6, 14, 21, 29, 36, 44, 52, 60, 37, 45,
    53, 61, 22, 30, 7, 15, 23, 31, 38, 46, 54, 62, 39, 47, 55, 63,
};

/* Generic VLC match: peeks up to 16 bits and scans the table. Returns the
 * index, or -1 for no match (caller raises ECD). Consumes the code. */
template <size_t N>
int vlc_decode(const Vlc (&tab)[N], int* len_out) {
    if (!ensure_bits(16)) {
        /* Near end of stream fewer than 16 bits can still hold a valid
         * short code; retry with what is available. */
        if (avail_bits() == 0) return -2;
        g_underflow = false;
    }
    size_t have = avail_bits();
    uint32_t window = 0;
    unsigned wbits = (unsigned)(have < 16 ? have : 16);
    window = peek_bits(wbits);
    for (size_t i = 0; i < N; ++i) {
        if (tab[i].len > wbits) {
            if (tab[i].len <= 16 && have < tab[i].len) {
                /* Cannot rule the longer codes out yet. */
                g_underflow = true;
                return -2;
            }
            continue;
        }
        if ((window >> (wbits - tab[i].len)) == tab[i].code) {
            advance_bits(tab[i].len);
            if (len_out) *len_out = tab[i].len;
            return (int)i;
        }
    }
    return -1;
}

/* DCT coefficient decode (B-14/B-15). Returns 0 ok (run/level/sign filled),
 * 1 end-of-block, -1 invalid code, -2 underflow. */
int dct_vlc_decode(bool intra_vlc, bool first, int* run, int* level_mag, bool* negative) {
    if (!ensure_bits(17)) {
        if (avail_bits() < 17) {
            /* A short code plus sign may still fit; simplest correct
             * behavior is to require the full window and retry when more
             * data arrives. Real streams end with start codes, never mid
             * block. */
            return -2;
        }
    }
    g_underflow = false;
    uint32_t window = peek_bits(17);
    if (!intra_vlc && first && (window >> 16) == 1) {
        /* First coefficient of a non-intra block: '1s'. */
        advance_bits(1);
        *run = 0;
        *level_mag = 1;
        *negative = get_bits(1) != 0;
        return 0;
    }
    const DctVlc* tab = intra_vlc ? kDct15 : kDct14;
    size_t n = intra_vlc ? sizeof(kDct15) / sizeof(kDct15[0]) : sizeof(kDct14) / sizeof(kDct14[0]);
    for (size_t i = 0; i < n; ++i) {
        if ((window >> (17 - tab[i].len)) != tab[i].code) continue;
        advance_bits(tab[i].len);
        if (tab[i].run == 64) return 1; /* EOB */
        if (tab[i].run == 65) {         /* escape: 6-bit run + 12-bit signed level */
            if (!ensure_bits(18)) return -2;
            *run = (int)get_bits(6);
            int32_t lv = get_signed(12);
            if (lv == 0 || lv == -2048) return -1; /* forbidden codes */
            *negative = lv < 0;
            *level_mag = lv < 0 ? -lv : lv;
            return 0;
        }
        *run = tab[i].run;
        *level_mag = tab[i].level;
        if (!ensure_bits(1)) return -2;
        *negative = get_bits(1) != 0;
        return 0;
    }
    return -1;
}

/* ---- IDCT ----------------------------------------------------------------
 * Straight double-precision implementation of the 8x8 inverse DCT as
 * defined by ISO/IEC 13818-2 Annex A. Own code, no external source. */

double g_idct_mat[8][8]; /* [x][u] = C(u)/2 * cos((2x+1)u pi / 16) */
bool g_idct_init = false;

void idct_init() {
    if (g_idct_init) return;
    for (int x = 0; x < 8; ++x) {
        for (int u = 0; u < 8; ++u) {
            double cu = (u == 0) ? (1.0 / std::sqrt(2.0)) : 1.0;
            g_idct_mat[x][u] = 0.5 * cu * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0);
        }
    }
    g_idct_init = true;
}

void idct8x8(const int16_t in[64], double out[64]) {
    double tmp[64];
    for (int y = 0; y < 8; ++y) { /* rows: over u */
        for (int x = 0; x < 8; ++x) {
            double s = 0;
            for (int u = 0; u < 8; ++u) s += g_idct_mat[x][u] * in[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    }
    for (int x = 0; x < 8; ++x) { /* columns: over v */
        for (int y = 0; y < 8; ++y) {
            double s = 0;
            for (int v = 0; v < 8; ++v) s += g_idct_mat[y][v] * tmp[v * 8 + x];
            out[y * 8 + x] = s;
        }
    }
}

/* ---- BDEC ---------------------------------------------------------------- */

int16_t g_dct[64]; /* coefficient staging block */

void saturate_coeff(int32_t* v) {
    if (*v > 2047) *v = 2047;
    if (*v < -2048) *v = -2048;
}

/* Decodes the AC (and for non-intra, all) coefficients of one block into
 * g_dct. Returns false either on input underflow (g_ecd clear: the caller
 * rewinds and retries when more input arrives) or on a bitstream error
 * (g_ecd set: the decode stops there). *any_ac reports whether any
 * coefficient other than g_dct[0] was written (DC-only blocks skip the
 * full IDCT). */
bool decode_block_coeffs(bool intra, int qscale, bool* any_ac) {
    const uint8_t* scan = ctrl_as() ? kScanAlt : kScanZigzag;
    const uint8_t* w = intra ? g_iq : g_niq;
    int i = intra ? 0 : -1; /* coefficient index in scan order */
    for (;;) {
        int run, mag;
        bool neg;
        int r = dct_vlc_decode(intra && ctrl_ivf(), !intra && i < 0, &run, &mag, &neg);
        if (r == -2) { g_underflow = true; return false; }
        if (r == 1) return true; /* EOB */
        if (r == -1) {
            /* No table entry matches the next bits. Hardware raises
             * IPU_CTRL.ECD and stops the command there (EE User's Manual,
             * IPU chapter: ECD is set on a decoding error and the command
             * ends); carrying on would consume the rest of the stream one
             * bad macroblock at a time, which is what the attract movie was
             * doing when it wedged. */
            g_ecd = true;
            return false;
        }
        i = (i < 0 ? 0 : i + 1) + run;
        if (i >= 64) return true;
        int j = scan[i];
        int32_t val;
        if (intra) {
            val = ((int32_t)mag * qscale * w[i]) >> 4;
        } else {
            val = ((2 * (int32_t)mag + 1) * qscale * w[i]) >> 5;
        }
        if (neg) val = -val;
        saturate_coeff(&val);
        g_dct[j] = (int16_t)val;
        if (j != 0) *any_ac = true;
    }
}

/* Output macroblock staging: Y 16x16, Cb 8x8, Cr 8x8, 16-bit, 768 bytes.
 * This is the BDEC output layout (RAW16). */
struct MacroBlock16 {
    int16_t y[16][16];
    int16_t cb[8][8];
    int16_t cr[8][8];
};
MacroBlock16 g_mb16;

/* CSC input layout (RAW8), 384 bytes. */
struct MacroBlock8 {
    uint8_t y[16][16];
    uint8_t cb[8][8];
    uint8_t cr[8][8];
};

void store_luma_block(int b, bool field_dct, const double* px, bool intra) {
    for (int i = 0; i < 8; ++i) {
        int row = field_dct ? ((b >> 1) + 2 * i) : ((b >> 1) * 8 + i);
        int col = (b & 1) * 8;
        for (int x = 0; x < 8; ++x) {
            long v = std::lround(px[i * 8 + x]);
            if (intra) {
                if (v < 0) v = 0;
                if (v > 255) v = 255;
            } else {
                if (v < -256) v = -256;
                if (v > 255) v = 255;
            }
            g_mb16.y[row][col + x] = (int16_t)v;
        }
    }
}

void store_chroma_block(int16_t (*plane)[8], const double* px, bool intra) {
    for (int i = 0; i < 8; ++i) {
        for (int x = 0; x < 8; ++x) {
            long v = std::lround(px[i * 8 + x]);
            if (intra) {
                if (v < 0) v = 0;
                if (v > 255) v = 255;
            } else {
                if (v < -256) v = -256;
                if (v > 255) v = 255;
            }
            plane[i][x] = (int16_t)v;
        }
    }
}

void take_snapshot();

/* The scan below is committed rather than replayed, so a stall inside it
 * rewinds to the byte it had reached and not to the BDEC that started it.
 * compact_in moves the cursor, so the snapshot is taken after it. */
void commit_scan() {
    compact_in();
    take_snapshot();
}

/* True once the macroblock has been emitted and only the trailing scan is
 * left, so a retry of the pending BDEC resumes the scan instead of decoding
 * the macroblock again. Cleared when a command is accepted. */
bool g_bdec_tail = false;

/* True once that scan has entered the zero run, so a retry resumes inside
 * the run instead of re-testing the gate that opened it.
 *
 * The gate is "the next 8 bits are zero", read before the stream is byte
 * aligned. The alignment step then commits the cursor onto the following
 * byte, of which only the leading misalign bits have been proved zero, so
 * if the scan starves after that commit the retry would re-read the gate at
 * a byte that can be nonzero, skip the run entirely and return with ECD
 * clear where the uninterrupted decode sets it. Committed alongside the
 * cursor for that reason, and cleared with g_bdec_tail. */
bool g_bdec_tail_run = false;

/* Trailing start-code scan after a BDEC: if the next 8 bits are zero, the
 * stream aligns to the byte boundary and zero bytes are skipped; a
 * following 000001 sets SCD, other nonzero data sets ECD. (Behavior per
 * hardware as documented by the reference emulator; libmpeg relies on SCD
 * to find the end of each slice.)
 *
 * The scan is not short. This movie is padded to a constant bit rate with
 * zero bytes, and the run that follows the last slice of a picture is
 * measured at up to 18389 bytes in the retail elementary stream (read off
 * the disc by hw/ipu_selftest.cpp), which is 143 times what the input FIFO
 * holds. Hardware takes those bytes out of the FIFO as it scans and the
 * DMA refills behind it, so the position is never given back. The model
 * has to do the same, which is why every skipped byte commits: a scan that
 * stalled for input and rewound to the command start instead would ask
 * IPU_BP to name a rewind point hundreds of quadwords back, which the
 * register cannot encode, and once the ch4 chain ran dry the command could
 * never finish at all. That is measured: it is where the retail movie
 * wedged on Windows, with a BDEC 10078 bytes into the stuffing after an I
 * picture and 630 quadwords sitting between the cursor and the end of the
 * input. */
bool bdec_tail_scan() {
    if (!g_bdec_tail_run) {
        if (!ensure_bits(8)) return false;
        if (peek_bits(8) == 0) {
            /* Inside the run from here, whatever the retries do. */
            g_bdec_tail_run = true;
            /* Align to the byte boundary. */
            size_t misalign = g_in_pos & 7;
            if (misalign) {
                advance_bits(8 - misalign);
                commit_scan();
            }
        }
    }
    if (g_bdec_tail_run) {
        for (;;) {
            if (!ensure_bits(24)) return false;
            uint32_t sc = peek_bits(24);
            if (sc != 0) {
                if (sc == 1) g_scd = true;
                else g_ecd = true;
                break;
            }
            advance_bits(8);
            commit_scan();
        }
        /* The break above sets ECD or SCD from bits it did not consume, so
         * a retry that re-enters the loop reads the same 24 bits and sets
         * the same flag; the run stays open until the command is done. */
    }
    if (!ensure_bits(32)) return false;
    g_top = peek_bits(32);
    return true;
}

/* Shared tail for the BDEC bitstream errors above: hardware stops the
 * command with IPU_CTRL.ECD set and no macroblock output, so the model does
 * the same and names the code that did not decode. */
void bdec_error(const char* what) {
    static uint64_t n = 0;
    if (rt_trace() || is_pow2(++n)) {
        rt_log("ipu", "BDEC: no %s code matches the bitstream at bit %zu; IPU_CTRL.ECD set and the "
            "command stops with no macroblock output (the decoder has lost sync) [#%" PRIu64 "]",
            what, g_in_pos, n);
    }
}

bool exec_bdec(uint32_t val) {
    /* A retry of a BDEC whose macroblock is already out resumes the
     * committed trailing scan; the macroblock is not decoded twice. */
    if (g_bdec_tail) return bdec_tail_scan();

    uint32_t fb = val & 0x3F;
    uint32_t qsc = (val >> 16) & 0x1F;
    bool dt = (val >> 25) & 1;
    bool dcr = (val >> 26) & 1;
    bool mbi = (val >> 27) & 1;

    if (ctrl_mp1()) {
        rt_fatal("ipu", nullptr, "BDEC with IPU_CTRL.MP1 (MPEG1) set; not modeled (this stream is MPEG2)");
    }
    if (!ensure_bits(fb)) return false;
    advance_bits(fb);

    if (dcr) {
        g_dcpred[0] = g_dcpred[1] = g_dcpred[2] = 128 << ctrl_idp();
    }
    int qscale = ctrl_qst() ? kNonLinearQ[qsc] : (int)(qsc << 1);
    std::memset(&g_mb16, 0, sizeof(g_mb16));

    uint32_t cbp = 0x3F;
    if (!mbi) {
        int len;
        int idx = vlc_decode(kCbp, &len);
        if (idx == -2) { g_underflow = true; return false; }
        if (idx < 0) {
            g_ecd = true;
            bdec_error("coded_block_pattern");
            return true;
        }
        cbp = (uint32_t)kCbp[idx].val;
    }

    double px[64];
    for (int b = 0; b < 6; ++b) {
        if (!(cbp & (0x20u >> b))) continue;
        std::memset(g_dct, 0, sizeof(g_dct));
        if (mbi) {
            /* DC coefficient: size VLC + differential. */
            int len;
            int idx;
            if (b < 4) idx = vlc_decode(kDcLuma, &len);
            else idx = vlc_decode(kDcChroma, &len);
            if (idx == -2) { g_underflow = true; return false; }
            if (idx < 0) {
                g_ecd = true;
                bdec_error("dct_dc_size");
                return true;
            }
            int size = b < 4 ? kDcLuma[idx].val : kDcChroma[idx].val;
            int diff = 0;
            if (size) {
                if (!ensure_bits((unsigned)size)) return false;
                diff = (int)get_bits((unsigned)size);
                if (!(diff & (1 << (size - 1)))) diff -= (1 << size) - 1;
            }
            int cc = b < 4 ? 0 : (b == 4 ? 1 : 2);
            g_dcpred[cc] += diff;
            g_dct[0] = (int16_t)(g_dcpred[cc] << (3 - ctrl_idp()));
        }
        bool any_ac = false;
        if (!decode_block_coeffs(mbi, qscale, &any_ac)) {
            if (!g_ecd) return false; /* input underflow: rewind and retry */
            bdec_error("DCT coefficient");
            return true;
        }
        if (any_ac) {
            idct8x8(g_dct, px);
        } else {
            /* DC-only block: the IDCT is a constant dc/8. */
            double c = g_dct[0] / 8.0;
            for (int i = 0; i < 64; ++i) px[i] = c;
        }
        if (b < 4) store_luma_block(b, dt, px, mbi);
        else if (b == 4) store_chroma_block(g_mb16.cb, px, mbi);
        else store_chroma_block(g_mb16.cr, px, mbi);
    }

    g_cbp_reg = cbp;
    out_push(&g_mb16, sizeof(g_mb16)); /* 48 qwords */
    /* The macroblock is out and the DC predictors are updated: nothing left
     * can be replayed, so the retry point moves here and only the trailing
     * scan is still in flight. */
    g_bdec_tail = true;
    take_snapshot();
    /* An armed ch3 takes it now rather than at the end of the command. The
     * scan below can run for thousands of bytes, and func_00240090.s waits
     * for IPU_CTRL.OFC to read 0 before it takes its snapshot, so leaving a
     * decoded macroblock in the output queue for the length of the scan
     * would park the library in that wait. Hardware does not: the fromIPU
     * channel drains the output FIFO while the command is still running. */
    drain_ch3();
    if (!bdec_tail_scan()) return false;
    return true;
}

/* ---- VDEC ---------------------------------------------------------------- */

bool exec_vdec(uint32_t val) {
    uint32_t fb = val & 0x3F;
    uint32_t tbl = (val >> 26) & 3;
    if (!ensure_bits(fb)) return false;
    advance_bits(fb);

    uint32_t result = 0;
    switch (tbl) {
        case 0: { /* macroblock address increment */
            int len;
            int idx = vlc_decode(kMbai, &len);
            if (idx == -2) return false;
            if (idx >= 0) {
                if (kMbai[idx].val == 0x22 && !ctrl_mp1()) {
                    result = 0; /* stuffing is MPEG1-only */
                } else {
                    result = (uint32_t)kMbai[idx].val | ((uint32_t)len << 16);
                }
            }
            break;
        }
        case 1: { /* macroblock type, table selected by CTRL.PCT */
            int pct = ctrl_pct() ? ctrl_pct() : 1;
            int len;
            if (pct == 1) {
                int idx = vlc_decode(kMbtI, &len);
                if (idx == -2) return false;
                if (idx >= 0) result = (uint32_t)kMbtI[idx].val;
            } else if (pct == 2) {
                int idx = vlc_decode(kMbtP, &len);
                if (idx == -2) return false;
                if (idx >= 0) {
                    uint32_t modes = (uint32_t)kMbtP[idx].val;
                    /* frame picture, frame_pred_frame_dct: motion type is
                     * implicitly frame MC. */
                    if (modes & MB_FORWARD) modes |= MB_MC_FRAME;
                    result = modes;
                }
            } else if (pct == 3) {
                int idx = vlc_decode(kMbtB, &len);
                if (idx == -2) return false;
                if (idx >= 0) {
                    uint32_t modes = (uint32_t)kMbtB[idx].val | MB_MC_FRAME;
                    result = modes | ((uint32_t)len << 16);
                }
            } else {
                static uint64_t n = 0;
                if (is_pow2(++n)) rt_log("ipu", "VDEC MBT with PCT=%d (D picture?); returning error [#%" PRIu64 "]", pct, n);
            }
            break;
        }
        case 2: { /* motion code */
            if (!ensure_bits(1)) return false;
            if (peek_bits(1) == 1) {
                advance_bits(1);
                result = 0x00010000; /* value 0, length 1 */
            } else {
                int len;
                int idx = vlc_decode(kMotion, &len);
                if (idx == -2) return false;
                if (idx > 0) { /* idx 0 is the '1' code, handled above */
                    int32_t mag = kMotion[idx].val;
                    if (!ensure_bits(1)) return false;
                    int32_t sign = get_bits(1) ? -1 : 0;
                    int32_t v = (mag ^ sign) - sign;
                    result = (uint32_t)v | ((uint32_t)len << 16);
                }
            }
            break;
        }
        default: { /* dmvector */
            int len;
            int idx = vlc_decode(kDmv, &len);
            if (idx == -2) return false;
            if (idx >= 0) {
                result = (uint32_t)(int32_t)kDmv[idx].val | ((uint32_t)len << 16);
            }
            break;
        }
    }

    g_cmd_data = result;
    g_ecd = (result == 0);
    if (!ensure_bits(32)) return false;
    g_top = peek_bits(32);
    return true;
}

/* ---- CSC ----------------------------------------------------------------- */

/* Documented integer BT.601 conversion of the IPU (EE User's Manual):
 * Y coefficient 0x95, R/Cr 0xCC, G/Cr -0x68, G/Cb -0x32, B/Cb 0x102,
 * biases 16 (Y) and 128 (C), >>6 then rounded >>1. Alpha is 0x80 before
 * thresholding. */
void csc_one_mb(const MacroBlock8* mb, uint8_t* rgb_out /* 1024 bytes */) {
    for (int yy = 0; yy < 16; ++yy) {
        for (int xx = 0; xx < 16; ++xx) {
            int ylev = mb->y[yy][xx] - 16;
            if (ylev < 0) ylev = 0;
            int lum = (0x95 * ylev) >> 6;
            int cr = mb->cr[yy >> 1][xx >> 1] - 128;
            int cb = mb->cb[yy >> 1][xx >> 1] - 128;
            int r = (lum + ((0xCC * cr) >> 6) + 1) >> 1;
            int g = (lum + ((-0x68 * cr) >> 6) + ((-0x32 * cb) >> 6) + 1) >> 1;
            int b = (lum + ((0x102 * cb) >> 6) + 1) >> 1;
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            int a = 0x80;
            if (g_th[0] > 0 && r < g_th[0] && g < g_th[0] && b < g_th[0]) {
                r = g = b = a = 0;
            } else if (g_th[1] > 0 && r < g_th[1] && g < g_th[1] && b < g_th[1]) {
                a = 0x40;
            }
            uint8_t* p = rgb_out + (yy * 16 + xx) * 4;
            p[0] = (uint8_t)r;
            p[1] = (uint8_t)g;
            p[2] = (uint8_t)b;
            p[3] = (uint8_t)a;
        }
    }
}

/* Macroblocks of the current CSC that are already converted and pushed.
 * CSC is the one command whose input dwarfs the FIFO: the retail player
 * hands it a whole 720x480 frame, 1350 macroblocks of RAW8, as a single
 * 32400-quadword normal-mode ch4 transfer, in runs of up to 0x147
 * macroblocks per command. Committing each macroblock keeps a retry from
 * replaying the whole run, which is what let the input queue grow far past
 * anything IPU_BP could describe. */
uint32_t g_csc_done = 0;

bool exec_csc(uint32_t val) {
    uint32_t mbc = val & 0x7FF;
    bool ofm = (val >> 27) & 1;
    if (ofm) {
        rt_fatal("ipu", nullptr, "CSC with OFM=1 (RGB16 output); not in this binary's census");
    }
    MacroBlock8 mb;
    uint8_t rgb[1024];
    for (; g_csc_done < mbc; ++g_csc_done) {
        /* Input follows the bitstream position (normally byte aligned) and
         * is taken a byte at a time: a macroblock is 24 quadwords, three
         * times what the input FIFO holds, so the decoder has to drain the
         * FIFO as it goes and let the DMA refill behind it. */
        for (size_t i = 0; i < sizeof(mb); ++i) {
            if (!ensure_bits(8)) return false;
            ((uint8_t*)&mb)[i] = (uint8_t)get_bits(8);
        }
        csc_one_mb(&mb, rgb);
        out_push(rgb, sizeof(rgb));
        /* Committed: a stall inside the next macroblock rewinds to here. */
        take_snapshot();
        drain_ch3(); /* as above: ch3 is not gated on command completion */
    }
    return true;
}

/* ---- SETIQ / SETVQ / SETTH / BCLR ---------------------------------------- */

bool exec_setiq(uint32_t val) {
    uint32_t fb = val & 0x3F;
    bool niq = (val >> 27) & 1;
    if (!ensure_bits(fb)) return false;
    advance_bits(fb);
    if (!ensure_bits(64 * 8)) return false;
    uint8_t* dst = niq ? g_niq : g_iq;
    for (int i = 0; i < 64; ++i) dst[i] = (uint8_t)get_bits(8);
    if (rt_trace()) rt_log("ipu", "SETIQ %s matrix loaded", niq ? "non-intra" : "intra");
    return true;
}

bool exec_setvq() {
    if (!ensure_bits(32 * 8)) return false;
    for (int i = 0; i < 32; ++i) g_vq[i] = (uint8_t)get_bits(8);
    return true;
}

/* BCLR discards the input FIFO and sets the bit position. The library
 * restarts the toIPU DMA at the quadword the decoder was inside
 * (func_002587E0.s at 0x2588DC: MADR - (IFC + FP) * 16) and issues this
 * command with the saved BP, so everything resident here has to go: the
 * same bytes are about to be transferred again. The cursor is left partway
 * into a quadword that has not arrived yet, which is why avail_bits()
 * saturates at zero.
 *
 * A command waiting for input survives this. The library issues the BCLR
 * from inside its restart (func_00240218.s at 0x240514) and only waits for
 * BUSY to clear when it has frames in hand, so a starved command routinely
 * sees one; hardware resets the FIFO and the bit pointer under it and the
 * command carries on from the new BP when the DMA refills. The saved BP is
 * the position IPU_BP reported at the snapshot, which is where that command
 * will replay from, so rebasing its retry snapshot onto bp is exactly the
 * hardware behaviour. */
void exec_bclr(uint32_t val) {
    uint32_t bp = val & 0x7F;
    trace("BCLR bp=%u: %" PRIu64 " qword%s resident before (ifc=%u fp=%u bp=%u, cursor %" PRIu64 " bits, "
        "hardware position %" PRIu64 " bits), 0 after; pending command 0x%08x%s",
        bp, (uint64_t)held_qw(), held_qw() == 1 ? "" : "s",
        bp_ifc(), bp_fp(), bp_bp(), (uint64_t)g_in_pos, (uint64_t)g_in_hw, g_busy ? g_cur_cmd : 0u,
        g_busy ? ", its retry snapshot rebased onto bp" : "");
    static uint64_t n = 0;
    if (rt_trace() || is_pow2(++n)) {
        rt_log("ipu", "BCLR bp=%u: dropping %zu resident qword%s (ifc=%u fp=%u), pending command "
            "0x%08x [#%" PRIu64 "]", bp, held_qw(), held_qw() == 1 ? "" : "s", bp_ifc(), bp_fp(),
            g_busy ? g_cur_cmd : 0u, n);
    }
    g_in.clear();
    g_in_pos = bp;
    g_in_hw = bp;
    if (g_busy) {
        g_snap.in_pos = bp;
        g_stall_in = 0;
    }
    g_underflow = false;
}

void soft_reset() {
    g_in.clear();
    g_in_pos = 0;
    g_in_hw = 0;
    g_out.clear();
    g_out_head = 0;
    g_out_produced = 0;
    g_cbp_reg = 0;
    g_th[0] = g_th[1] = 0;
    g_ecd = g_scd = false;
    g_busy = false;
    g_bdec_tail = false;
    g_bdec_tail_run = false;
    g_top = 0;
    g_cmd_data = 0;
    g_underflow = false;
    /* The driver stops ch3/ch4 around a reset; drop any pending transfer
     * state so a stale completion cannot fire later. */
    g_to.active = false;
    g_from.active = false;
    g_from.drain_all = false;
    /* Keeps the IQ/VQ matrices and the picture-parameter CTRL bits, and
     * (a hardware quirk mirrored from the reference behavior) reinitializes
     * the DC predictors only at 8-bit precision. */
    if (ctrl_idp() == 0) {
        g_dcpred[0] = g_dcpred[1] = g_dcpred[2] = 128;
    }
    rt_intc_raise(RT_INTC_IPU);
    rt_log("ipu", "soft reset (IPU_CTRL.RST)");
}

/* ---- command dispatch / retry -------------------------------------------- */

void take_snapshot() {
    g_snap.in_pos = g_in_pos;
    g_snap.out_produced = g_out_produced;
    std::memcpy(g_snap.dcpred, g_dcpred, sizeof(g_dcpred));
    g_snap.cbp = g_cbp_reg;
}

void restore_snapshot() {
    /* g_in_hw is deliberately not restored: see the file header. */
    g_in_pos = g_snap.in_pos;
    /* Output pushed since the snapshot goes back. Both producers (BDEC's
     * macroblock, CSC's per-macroblock commit) snapshot the instant they
     * push, so there is never any to take back; if there is, the fromIPU
     * DMA may already have moved it out of the model's reach, which is a
     * model error rather than something to paper over. */
    if (g_out_produced != g_snap.out_produced) {
        uint64_t drop = g_out_produced - g_snap.out_produced;
        if (drop > (uint64_t)out_avail()) {
            rt_fatal("ipu", nullptr, "a stalled command has to take back %" PRIu64 " output bytes but "
                "only %zu are still queued; the fromIPU DMA has already moved the rest out",
                drop, out_avail());
        }
        g_out.resize(g_out.size() - (size_t)drop);
        g_out_produced = g_snap.out_produced;
    }
    std::memcpy(g_dcpred, g_snap.dcpred, sizeof(g_dcpred));
    g_cbp_reg = g_snap.cbp;
}

void run_pending() {
    if (!g_busy) return;
    idct_init();
    restore_snapshot();
    g_underflow = false;
    g_ecd = false;
    g_scd = false;

    uint32_t val = g_cur_cmd;
    bool done = false;
    switch (val >> 28) {
        case 2: done = exec_bdec(val); break;
        case 3: done = exec_vdec(val); break;
        case 4: { /* FDEC: skip FB, then the next 32 bits (no advance) */
            uint32_t fb = val & 0x3F;
            if (!ensure_bits(fb)) break;
            advance_bits(fb);
            if (!ensure_bits(32)) break;
            g_cmd_data = peek_bits(32);
            g_top = g_cmd_data;
            done = true;
            break;
        }
        case 5: done = exec_setiq(val); break;
        case 6: done = exec_setvq(); break;
        case 7: done = exec_csc(val); break;
        default:
            rt_fatal("ipu", nullptr, "unreachable pending command 0x%08x", val);
    }

    if (!done) {
        /* Input underflow: rewind and wait for more input. The attempt still
         * moved the hardware bit position, so it freed FIFO space; run the
         * walk again so the channel takes that space and, if the chain has
         * run out, completes now rather than at the next guest access. */
        restore_snapshot();
        advance_walk();
        g_stall_in = g_in.size();
        trace("command 0x%08x stalls on input: %" PRIu64 " bits available at cursor %" PRIu64 " (hardware "
            "position %" PRIu64 ", %" PRIu64 " qword%s resident); ch4 %s chcr=0x%08x madr=0x%08x qwc=%u "
            "tadr=0x%08x", val, (uint64_t)avail_bits(), (uint64_t)g_in_pos, (uint64_t)g_in_hw,
            (uint64_t)held_qw(), held_qw() == 1 ? "" : "s",
            g_to.active ? "running" : "idle", *g_ch4_reg[0], *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3]);
        static uint64_t n = 0;
        if (rt_trace() || is_pow2(++n)) {
            rt_log("ipu", "command 0x%08x stalls on input (have %zu bits); pending [#%" PRIu64 "]",
                val, avail_bits(), n);
        }
        return;
    }

    g_busy = false;
    compact_in();
    advance_walk();
    rt_intc_raise(RT_INTC_IPU);
    drain_ch3();
}

/* Hardware runs the toIPU DMA and the decoder alongside the guest. This
 * model advances both at every point the guest could observe them, so a
 * command stalled for input still makes progress while the guest sits in
 * its BUSY poll (func_002587E0.s at 0x258870 and 0x2588A0 are two such
 * polls, and the movie player has more). */
void service_input() {
    advance_walk();
    /* Only replay a stalled command when the fill actually delivered
     * something. Every stall is an input underflow, so with the same input
     * the retry would take the same path, and the guest polls hard enough
     * for that to matter (a whole CSC macroblock run replays from its
     * start). */
    if (g_busy && g_in.size() == g_stall_in) return;
    run_pending();
}

void cmd_write(uint32_t val) {
    uint32_t code = val >> 28;
    static const char* names[16] = {"BCLR", "IDEC", "BDEC", "VDEC", "FDEC", "SETIQ", "SETVQ", "CSC",
                                    "PACK", "SETTH", "?", "?", "?", "?", "?", "?"};
    ++g_cmd_census[code];
    if (is_pow2(g_cmd_census[code]) || rt_trace()) {
        rt_log("ipu", "%s 0x%08x [#%" PRIu64 "]", names[code], val, g_cmd_census[code]);
    }
    trace_cmd("IPU_CMD 0x%08x (%s #%" PRIu64 "): %" PRIu64 " bits available at cursor %" PRIu64 " (hardware "
        "position %" PRIu64 ", %" PRIu64 " qword%s resident, bp=%u ifc=%u fp=%u); ch4 %s madr=0x%08x qwc=%u "
        "tadr=0x%08x; previous 0x%08x %s", val, names[code], g_cmd_census[code], (uint64_t)avail_bits(),
        (uint64_t)g_in_pos, (uint64_t)g_in_hw,
        (uint64_t)held_qw(), held_qw() == 1 ? "" : "s", bp_bp(), bp_ifc(), bp_fp(),
        g_to.active ? "running" : "idle", *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3],
        g_cur_cmd, g_busy ? "still pending" : "done");
    if (g_busy && code != 0) {
        /* Off census for anything but BCLR: the library polls BUSY before
         * every other command. Drop the pending one and put the hardware bit
         * position back with the decode cursor, so IPU_BP keeps describing
         * the bits the next command will actually read. BCLR is the
         * exception and is handled in exec_bclr: the library issues it under
         * a pending command on purpose. */
        rt_log("ipu", "command 0x%08x written while 0x%08x is still pending; previous dropped", val, g_cur_cmd);
        g_busy = false;
        g_in_hw = g_in_pos; /* the stall already rewound g_in_pos */
    }
    g_ecd = false;
    g_scd = false;
    g_last_cmd_code = code;

    switch (code) {
        case 0: /* BCLR: immediate */
            exec_bclr(val);
            rt_intc_raise(RT_INTC_IPU);
            return;
        case 9: /* SETTH: immediate */
            g_th[0] = (uint16_t)(val & 0x1FF);
            g_th[1] = (uint16_t)((val >> 16) & 0x1FF);
            rt_intc_raise(RT_INTC_IPU);
            return;
        case 1:
            rt_fatal("ipu", nullptr, "IDEC issued (0x%08x); not in this binary's measured census", val);
        case 8:
            rt_fatal("ipu", nullptr, "PACK issued (0x%08x); not in this binary's measured census", val);
        case 2: case 3: case 4: case 5: case 6: case 7:
            g_cur_cmd = val;
            g_busy = true;
            g_csc_done = 0;
            g_bdec_tail = false;
            g_bdec_tail_run = false;
            take_snapshot();
            run_pending();
            return;
        default:
            rt_fatal("ipu", nullptr, "unknown IPU command 0x%08x", val);
    }
}

uint64_t g_busy_polls = 0;

} // namespace

/* ---- DMA bridge (called from hw/dmac.cpp) -------------------------------- */

void rt_ipu_dma_kick(int ch) {
    RT_PROF_ZONE(RT_PROF_IPU);
    bind_regs();
    idct_init();
    if (ch == 4) {
        ++g_to.kicks;
        if (g_to.active) {
            rt_log("ipu", "toIPU ch4 kicked while a transfer is active; continuing with new registers");
        }
        uint32_t chcr = *g_ch4_reg[0];
        uint32_t mode = (chcr >> 2) & 3;
        if (mode == 2) {
            rt_fatal("ipu", nullptr, "toIPU ch4 kicked in interleave mode");
        }
        g_to.active = true;
        g_to.chain = (mode == 1);
        /* A kick restarts the chain. The channel moves whatever QWC is
         * outstanding and then reads the tag at TADR; it does not end on
         * the tag id sitting in CHCR bits 28-31, even when that is REFE.
         *
         * This used to end there, on the reading that CHCR.TAG carries the
         * current tag across a stop. The retail library rules that reading
         * out, because both of its restart paths hand the channel back a
         * CHCR whose TAG field is whatever its own stop left behind, and
         * that stop writes the bare constant 5 (func_0023FDF0.s at
         * 0x23FE54-0x23FE68 and func_00240090.s at 0x2400A4-0x2400E8), so
         * the id reads back as 0. func_0023FDF0 puts an id of 3 back only
         * when it appended tags (the "beqz $18, .L0024001C" at 0x240004),
         * and func_00240218.s's same-block path never puts one back at all
         * ("ori $19, $5, 0x100" at 0x240278 restarts with the saved CHCR).
         * Ending the chain on that id therefore stops the channel after
         * the outstanding QWC on every one of those restarts and leaves
         * the decoder starved with the ring still full.
         *
         * That is measured, not argued. In the Windows trace of the movie
         * every restart that came through the same-block path (kicks 8,
         * 11, 14, 17, 22, 25, 28, 31, 34, 39, 42 and 45) moved between 24
         * and 111 quadwords and then reported "transfer complete ... after
         * 0 tags", while the kicks whose CHCR carried an id of 3 walked
         * nine REF tags and delivered eighteen kilobytes. Between them the
         * pending FDEC stalled with 32 bits and the guest spun in its BUSY
         * poll until the next field: one picture every four seconds, with
         * the FDEC census running to 131072 against 1024 BDECs. */
        g_to.tag_end = false;
        g_to.tags = 0;
        g_to.delivered = 0;
        trace("ch4 kick #%" PRIu64 ": %s chcr=0x%08x madr=0x%08x qwc=%u tadr=0x%08x; chcr tag %s "
            "irq=%u tie=%u (not consulted); %s; %" PRIu64 " qword%s resident (bp=%u ifc=%u fp=%u), pending command "
            "0x%08x %s",
            g_to.kicks, g_to.chain ? "chain" : "normal", chcr, *g_ch4_reg[1], *g_ch4_reg[2],
            *g_ch4_reg[3], tag_name((chcr >> 28) & 7), (chcr >> 31) & 1u, (chcr >> 7) & 1u,
            *g_ch4_reg[2] ? "moves QWC, then reads a tag at TADR" : "reads a tag at TADR",
            (uint64_t)held_qw(), held_qw() == 1 ? "" : "s", bp_bp(), bp_ifc(), bp_fp(),
            g_cur_cmd, g_busy ? "still pending" : "done");
        if (!g_to.chain && *g_ch4_reg[2] == 0) {
            /* Normal-mode kick with nothing to move completes at once. */
            complete_ch4();
            return;
        }
        /* A very large normal-mode kick is not a runaway: the movie player
         * feeds CSC a whole reconstructed frame this way, 720x480 in 4:2:0
         * RAW8 macroblock order, 1350 * 384 bytes = 32400 quadwords, and
         * then issues CSC in runs of a few hundred macroblocks. The
         * bitstream chain is the source-chain kick; this is the other one. */
        if (rt_trace() || is_pow2(g_to.kicks)) {
            rt_log("ipu", "toIPU ch4 kick: %s madr=0x%08x qwc=%u tadr=0x%08x [#%" PRIu64 "]",
                g_to.chain ? "chain" : "normal", *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3], g_to.kicks);
        }
        advance_walk();
        run_pending();
        drain_ch3();
    } else {
        ++g_from.kicks;
        if (g_from.active) {
            rt_log("ipu", "fromIPU ch3 kicked while a transfer is pending; continuing with new registers");
        }
        g_from.active = true;
        g_from.drain_all = (*g_ch3_reg[2] == 0);
        if (g_from.drain_all) {
            static uint64_t n = 0;
            if (is_pow2(++n)) {
                rt_log("ipu", "fromIPU ch3 kicked with QWC=0; draining the whole output queue (%zu qw) [#%" PRIu64 "]",
                    out_avail() / 16, n);
            }
        }
        if (rt_trace() || is_pow2(g_from.kicks)) {
            rt_log("ipu", "fromIPU ch3 kick: madr=0x%08x qwc=%u (out has %zu qw) [#%" PRIu64 "]",
                *g_ch3_reg[1], *g_ch3_reg[2], out_avail() / 16, g_from.kicks);
        }
        drain_ch3();
    }
}

/* One wording for the two ch4 stop lines below. A macro rather than a
 * const char*, because both call sites take a printf format attribute and
 * that only checks a literal; the two differ in the metering, not the
 * text. */
#define IPU_CH4_STOP_FMT \
    "ch4 stopped by a CHCR write without STR (chcr=0x%08x): the channel was %s, " \
    "madr=0x%08x qwc=%u tadr=0x%08x, %" PRIu64 " qword%s resident (bp=%u ifc=%u fp=%u), " \
    "pending command 0x%08x %s"

/* CHCR written with STR=0: the transfer stops where it is. The registers
 * are left alone (they are what the library reads back straight after:
 * func_002586F8.s at 0x258710-0x258754 loads MADR, TADR, QWC and CHCR the
 * instant the stop returns), the input FIFO keeps what it already holds,
 * and no completion interrupt is raised because nothing completed. */
void rt_ipu_dma_stop(int ch) {
    if (ch == 4) {
        bind_regs();
        /* A stop of a channel that is already idle changes nothing, and the
         * player writes one every field whether or not ch4 is running: 584
         * of the 3430 lines in the last Windows trace were that one line
         * repeated. It is metered and collapsed with the other polled
         * states so it cannot crowd out the events that do change
         * something. A stop that actually catches the channel running is
         * one of those events and stays unmetered. */
        if (g_to.active) {
            trace(IPU_CH4_STOP_FMT, *g_ch4_reg[0], "running", *g_ch4_reg[1], *g_ch4_reg[2],
                *g_ch4_reg[3], (uint64_t)held_qw(), held_qw() == 1 ? "" : "s", bp_bp(), bp_ifc(),
                bp_fp(), g_cur_cmd, g_busy ? "still pending" : "done");
        } else {
            trace_state(TS_STOP_IDLE, IPU_CH4_STOP_FMT, *g_ch4_reg[0], "already idle",
                *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3], (uint64_t)held_qw(),
                held_qw() == 1 ? "" : "s", bp_bp(), bp_ifc(), bp_fp(), g_cur_cmd,
                g_busy ? "still pending" : "done");
            return;
        }
        g_to.active = false;
        static uint64_t n = 0;
        if (rt_trace() || is_pow2(++n)) {
            bind_regs();
            rt_log("ipu", "toIPU ch4 stopped (CHCR without STR) at madr=0x%08x qwc=%u tadr=0x%08x "
                "with %zu qword%s resident (ifc=%u fp=%u bp=%u) [#%" PRIu64 "]",
                *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3], held_qw(), held_qw() == 1 ? "" : "s",
                bp_ifc(), bp_fp(), bp_bp(), n);
        }
        return;
    }
    if (ch == 3) {
        if (!g_from.active) return;
        g_from.active = false;
        g_from.drain_all = false;
        static uint64_t n = 0;
        if (rt_trace() || is_pow2(++n)) {
            bind_regs();
            rt_log("ipu", "fromIPU ch3 stopped (CHCR without STR) at madr=0x%08x qwc=%u [#%" PRIu64 "]",
                *g_ch3_reg[1], *g_ch3_reg[2], n);
        }
    }
}
#undef IPU_CH4_STOP_FMT

void rt_ipu_dma_resume() {
    bind_regs();
    trace_state(TS_HOLD_LIFT, "DMAC hold lifted with no IPU kick queued: ch4 %s (chcr=0x%08x madr=0x%08x qwc=%u "
        "tadr=0x%08x), ch3 %s, %" PRIu64 " qword%s resident, pending command 0x%08x %s",
        g_to.active ? "running" : "idle", *g_ch4_reg[0], *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3],
        g_from.active ? "running" : "idle", (uint64_t)held_qw(), held_qw() == 1 ? "" : "s",
        g_cur_cmd, g_busy ? "still pending" : "done");
    if (!g_to.active && !g_from.active) return;
    RT_PROF_ZONE(RT_PROF_IPU);
    idct_init();
    /* Says which channel is still running, because "the toIPU walk" with
     * ch4 already complete sent one diagnosis down the wrong path. */
    static uint64_t n = 0;
    if (rt_trace() || is_pow2(++n)) {
        rt_log("ipu", "DMAC hold lifted: ch4 %s (madr=0x%08x qwc=%u tadr=0x%08x), ch3 %s "
            "(madr=0x%08x qwc=%u), %zu qword%s resident [#%" PRIu64 "]",
            g_to.active ? "running" : "idle", *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3],
            g_from.active ? "running" : "idle", *g_ch3_reg[1], *g_ch3_reg[2],
            held_qw(), held_qw() == 1 ? "" : "s", n);
    }
    advance_walk();
    run_pending();
    drain_ch3();
}

/* ---- MMIO ---------------------------------------------------------------- */

/* No RT_PROF_ZONE here or in rt_ipu_mmio_write/rt_ipu_fifo_feed: mmio.cpp
 * already opens RT_PROF_IPU for this address range, and a second nested
 * zone would double the profiler's two clock reads on the movie's hottest
 * path for no extra information. The DMA entry points below keep theirs;
 * they are reached from a DMAC register write, which is billed to
 * RT_PROF_MMIO, and they run per transfer rather than per access. */
bool rt_ipu_mmio_read(uint32_t addr, uint64_t* out) {
    switch (addr) {
        case 0x10002000: { /* IPU_CMD */
            service_input();
            uint32_t data = g_cmd_data;
            if (g_last_cmd_code != 3 && g_last_cmd_code != 4) {
                /* Live 32-bit bitstream peek (hardware behavior after
                 * non-VDEC/FDEC commands). */
                bool saved = g_underflow;
                if (ensure_bits(32)) data = peek_bits(32);
                g_underflow = saved;
            }
            *out = (uint64_t)data | (g_busy ? (1ull << 63) : 0);
            if (g_busy && is_pow2(++g_busy_polls)) {
                rt_log("ipu", "IPU_CMD busy poll while 0x%08x waits for input [#%" PRIu64 "]",
                    g_cur_cmd, g_busy_polls);
            }
            return true;
        }
        case 0x10002010: { /* IPU_CTRL */
            service_input();
            uint32_t ifc = bp_ifc();
            uint32_t ofc = (uint32_t)(out_avail() / 16);
            if (ofc > 8) ofc = 8;
            uint32_t v = ifc | (ofc << 4) | ((g_cbp_reg & 0x3F) << 8) |
                (g_ecd ? 1u << 14 : 0) | (g_scd ? 1u << 15 : 0) |
                (g_ctrl_bits & 0x07F30000u) | (g_busy ? 1u << 31 : 0);
            *out = v;
            return true;
        }
        case 0x10002020: { /* IPU_BP */
            /* BP bits 0-6, IFC bits 8-11, FP bits 16-17. Field positions
             * measured from the retail library itself: func_002587E0.s at
             * 0x258804-0x258818 masks the readback with 0x7F, (>>8)&0xF and
             * (>>16)&3, and func_002407C0.s at 0x240808-0x240818 does the
             * same. */
            bind_regs(); /* the trace below reads the ch4 registers */
            service_input();
            const uint32_t bp = bp_bp(), ifc = bp_ifc(), fp = bp_fp();
            *out = bp | (ifc << 8) | (fp << 16);
            /* The library reads this to work out where in the stream the
             * decoder is, so the trace carries its arithmetic as well as
             * the raw value: stream byte = MADR - (IFC + FP) * 16 + BP / 8
             * (func_002407C0.s at 0x2407FC-0x240848), and the restart it
             * feeds (func_00240218.s at 0x240240-0x240278). */
            trace_state(TS_BP, "IPU_BP read = 0x%05x: bp=%u ifc=%u fp=%u; cursor %" PRIu64 " bits, hardware "
                "position %" PRIu64 " bits, %" PRIu64 " qword%s resident; ch4 %s madr=0x%08x qwc=%u "
                "tadr=0x%08x -> the library would rewind to madr=0x%08x qwc=%u and call the stream "
                "byte 0x%08x",
                (unsigned)*out, bp, ifc, fp, (uint64_t)g_in_pos, (uint64_t)g_in_hw,
                (uint64_t)held_qw(), held_qw() == 1 ? "" : "s",
                g_to.active ? "running" : "idle", *g_ch4_reg[1], *g_ch4_reg[2], *g_ch4_reg[3],
                *g_ch4_reg[1] - (ifc + fp) * 16, *g_ch4_reg[2] + ifc + fp,
                *g_ch4_reg[1] - (ifc + fp) * 16 + (bp >> 3));
            return true;
        }
        case 0x10002030: { /* IPU_TOP */
            service_input();
            *out = (uint64_t)g_top | (g_busy ? (1ull << 63) : 0);
            return true;
        }
        case 0x10007000: case 0x10007010: {
            static uint64_t n = 0;
            if (is_pow2(++n)) rt_log("ipu", "IPU FIFO window read at 0x%08x; returns 0 [#%" PRIu64 "]", addr, n);
            *out = 0;
            return true;
        }
        default:
            return false;
    }
}

bool rt_ipu_mmio_write(uint32_t addr, uint64_t v) {
    switch (addr) {
        case 0x10002000: /* IPU_CMD */
            bind_regs();
            idct_init();
            cmd_write((uint32_t)v);
            return true;
        case 0x10002010: { /* IPU_CTRL: bits 16-26 stored (18-19 reserved), RST acts */
            uint32_t nv = (uint32_t)v;
            g_ctrl_bits = nv & 0x07F30000u;
            if (((g_ctrl_bits >> 16) & 3) == 3) {
                rt_log("ipu", "IPU_CTRL.IDP=3 is reserved; treating as 9-bit precision");
                g_ctrl_bits = (g_ctrl_bits & ~0x30000u) | 0x10000u;
            }
            if (nv & 0x40000000u) soft_reset();
            return true;
        }
        case 0x10002020: case 0x10002030:
            rt_log("ipu", "write to read-only IPU register 0x%08x = 0x%llx ignored", addr, (unsigned long long)v);
            return true;
        case 0x10007010:
            /* Only 128-bit stores are in this binary's census; those are
             * routed through rt_ipu_fifo_feed by mmio.cpp before this
             * dispatcher runs. A narrower store reaching here is new. */
            rt_fatal("ipu", nullptr, "sub-qword write to the toIPU FIFO window (0x%08x = 0x%llx); "
                "not in this binary's census (feeds are DMA ch4 and 128-bit stores)",
                addr, (unsigned long long)v);
        case 0x10007000:
            rt_fatal("ipu", nullptr, "write to the fromIPU FIFO window 0x10007000");
        default:
            return false;
    }
}

void rt_ipu_fifo_feed(const uint8_t* qw16) {
    static uint64_t n = 0;
    /* Hardware stalls the store until the FIFO has room. Commands here run
     * synchronously, so give the pending one a chance to drain first; if the
     * FIFO is still full the store is accepted anyway and the overflow is
     * reported, because dropping guest data would be worse than modeling a
     * FIFO one quadword too deep. */
    if (walk_qw() > kFifoQw) run_pending();
    if (walk_qw() > kFifoQw) {
        static uint64_t full = 0;
        if (is_pow2(++full)) {
            rt_log("ipu", "toIPU FIFO qword store with the input FIFO already full (%zu qwords); "
                "hardware would stall the store [#%" PRIu64 "]", walk_qw(), full);
        }
    }
    size_t base = g_in.size();
    g_in.resize(base + 16);
    std::memcpy(g_in.data() + base, qw16, 16);
    if (rt_trace() || is_pow2(++n)) {
        rt_log("ipu", "toIPU FIFO qword store (input now %zu bits) [#%" PRIu64 "]", avail_bits(), n);
    }
    run_pending();
}

/* ---- selftest hooks (used by hw/ipu_selftest.cpp only) ------------------- */

void rt_ipu_test_feed(const uint8_t* data, size_t len) {
    size_t base = g_in.size();
    g_in.resize(base + len);
    std::memcpy(g_in.data() + base, data, len);
}

size_t rt_ipu_test_out_avail() { return out_avail(); }

size_t rt_ipu_test_avail_bits() { return avail_bits(); }

size_t rt_ipu_test_resident_qw() { return held_qw(); }

size_t rt_ipu_test_read_out(uint8_t* dst, size_t maxlen) {
    size_t n = out_avail();
    if (n > maxlen) n = maxlen;
    std::memcpy(dst, g_out.data() + g_out_head, n);
    g_out_head += n;
    if (g_out_head == g_out.size()) {
        g_out.clear();
        g_out_head = 0;
    }
    return n;
}
