/* hw/vif1.cpp: VIF1 command stream interpreter.
 *
 * Fed synchronously with 32-bit words from three sources: DMA ch1 payload
 * (dmac.cpp), TTE tag-word transfers (words 2-3 of each source-chain tag),
 * and direct FIFO stores to 0x10005000. Commands may split across feeds;
 * payload words are buffered until a command is complete, then the command
 * executes atomically.
 *
 * Effects:
 *   UNPACK        writes VU1 data memory (rt_vu1_state()->mem), with
 *                 STCYCL CL/WL skipping and filling cycles, STMASK/STROW/
 *                 STCOL masking, STMOD offset/difference modes, signed and
 *                 unsigned extension, V4-5 expansion, and the 16 KB wrap.
 *   MPG           writes the VU1 micro shadow (rt_vu1_micro()).
 *   DIRECT/HL     forwards qwords to GIF PATH2.
 *   MSCAL/F/CNT   dispatches through rt_vu1_mscal with xtop/itop from
 *                 TOPS/ITOPS and BASE/OFFSET double-buffer flipping.
 *   i-bit         raises INTC cause 5 (VIF1), delivered deferred.
 *
 * VIF0 (FIFO 0x10004000) is not interpreted: ICO only uses it for the SDK
 * reset sequence (STCYCL/STMASK/ITOP/STMOD/MARK); words are counted and
 * loud-logged, then discarded.
 *
 * All command encodings are public PS2 hardware documentation (ps2tek,
 * "VIFcode reference").
 */
#include "hw.h"

#include "../ee/kernel.h"

#include <cinttypes>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kVuQw = 1024; /* VU1 data memory in qwords */

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

struct Vif1 {
    /* Persistent register state. */
    uint32_t cl = 0, wl = 0;      /* STCYCL */
    uint32_t mask = 0;            /* STMASK */
    uint32_t mode = 0;            /* STMOD */
    uint32_t row[4] = {};         /* STROW */
    uint32_t col[4] = {};         /* STCOL */
    uint32_t itops = 0, itop = 0;
    uint32_t base = 0, ofst = 0;
    uint32_t tops = 0, top = 0;
    uint32_t mark = 0;
    uint32_t err = 0;
    bool dbf = false;
    bool mskpath3 = false;

    /* In-flight command. */
    bool pending = false;
    uint32_t code = 0;
    uint32_t need_words = 0;
    std::vector<uint32_t> payload;

    /* Stats. */
    uint64_t cmds = 0, unpacks = 0, directs = 0, mpgs = 0, mscals = 0;
};

Vif1 g_vif;
uint64_t g_vif0_words = 0;

uint32_t sext8(uint8_t v, bool usn) { return usn ? v : (uint32_t)(int32_t)(int8_t)v; }
uint32_t sext16(uint16_t v, bool usn) { return usn ? v : (uint32_t)(int32_t)(int16_t)v; }

/* ---- UNPACK ------------------------------------------------------------- */

void exec_unpack(Vif1& v) {
    const uint32_t code = v.code;
    const uint32_t cmd = (code >> 24) & 0x7F;
    const uint32_t vn = (cmd >> 2) & 3;
    const uint32_t vl = cmd & 3;
    const bool masked = (cmd & 0x10) != 0;
    const bool usn = (code >> 14) & 1;
    const bool flg = (code >> 15) & 1;
    uint32_t num = (code >> 16) & 0xFF;
    if (num == 0) num = 256;
    uint32_t addr = code & 0x3FF;
    if (flg) addr = (addr + v.tops) & 0x3FF;

    uint32_t cl = v.cl, wl = v.wl;
    if (wl == 0) wl = 256;
    if (cl == 0 && wl > cl) {
        /* CL=0 with filling would consume no input at all; loud and clamp. */
        static uint64_t n = 0;
        if (is_pow2(++n)) rt_log("vif1", "UNPACK with CL=0 (STCYCL=0x%02x%02x); clamping CL to 1", v.wl, v.cl);
        cl = 1;
    }
    const bool filling = wl > cl;
    static uint64_t fill_uses = 0;
    if (filling && is_pow2(++fill_uses)) {
        rt_log("vif1", "UNPACK filling mode in use (CL=%u WL=%u) [#%" PRIu64 "]", cl, wl, fill_uses);
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(v.payload.data());
    const uint32_t nbytes = (uint32_t)v.payload.size() * 4;
    uint32_t cursor = 0; /* byte cursor into payload */

    Vu1State* vu = rt_vu1_state();
    uint32_t last_data[4] = {0, 0, 0, 0}; /* buffered input for filling cycles */
    static uint64_t missing_field_data = 0;

    for (uint32_t k = 0; k < num; ++k) {
        uint32_t cycle = filling ? (k % wl) : (k % wl);
        bool has_input = !filling || cycle < cl;

        uint32_t data[4] = {0, 0, 0, 0};
        uint32_t nfields = vn + 1;
        if (has_input) {
            switch (vl) {
                case 0: /* 32-bit */
                    for (uint32_t f = 0; f < nfields; ++f) {
                        if (cursor + 4 <= nbytes) std::memcpy(&data[f], bytes + cursor, 4);
                        cursor += 4;
                    }
                    break;
                case 1: /* 16-bit */
                    for (uint32_t f = 0; f < nfields; ++f) {
                        uint16_t h = 0;
                        if (cursor + 2 <= nbytes) std::memcpy(&h, bytes + cursor, 2);
                        cursor += 2;
                        data[f] = sext16(h, usn);
                    }
                    break;
                case 2: /* 8-bit */
                    for (uint32_t f = 0; f < nfields; ++f) {
                        uint8_t b = (cursor < nbytes) ? bytes[cursor] : 0;
                        ++cursor;
                        data[f] = sext8(b, usn);
                    }
                    break;
                default: { /* V4-5 (vl==3 is only defined for vn==3) */
                    uint16_t h = 0;
                    if (cursor + 2 <= nbytes) std::memcpy(&h, bytes + cursor, 2);
                    cursor += 2;
                    data[0] = (uint32_t)(h & 0x1F) << 3;
                    data[1] = (uint32_t)((h >> 5) & 0x1F) << 3;
                    data[2] = (uint32_t)((h >> 10) & 0x1F) << 3;
                    data[3] = (uint32_t)((h >> 15) & 1) << 7;
                    nfields = 4;
                    break;
                }
            }
            if (vn == 0) { /* S-xx broadcasts to all four fields */
                data[1] = data[2] = data[3] = data[0];
                nfields = 4;
            }
            std::memcpy(last_data, data, sizeof(data));
        } else {
            std::memcpy(data, last_data, sizeof(data));
            nfields = 4;
        }

        /* Destination qword. Skipping mode: after WL writes, skip to the
         * next CL boundary. Filling mode: consecutive. */
        uint32_t qw;
        if (filling) {
            qw = (addr + k) & (kVuQw - 1);
        } else {
            qw = (addr + (k / wl) * cl + (k % wl)) & (kVuQw - 1);
        }
        uint8_t* dst = vu->mem + (size_t)qw * 16;

        uint32_t mcycle = cycle < 3 ? cycle : 3;
        for (uint32_t f = 0; f < 4; ++f) {
            uint32_t m = masked ? (v.mask >> (mcycle * 8 + f * 2)) & 3 : 0;
            uint32_t out;
            switch (m) {
                case 0: {
                    if (f >= nfields) {
                        /* Field not provided by this format and not masked:
                         * hardware writes indeterminate data. Write 0, loud
                         * once in a while. */
                        if (is_pow2(++missing_field_data)) {
                            rt_log("vif1", "UNPACK cmd=0x%02x writes field %u with no source data (mask says data) [#%" PRIu64 "]",
                                cmd, f, missing_field_data);
                        }
                        out = 0;
                        break;
                    }
                    out = data[f];
                    if (v.mode == 1) {
                        out += v.row[f];
                    } else if (v.mode == 2) {
                        out += v.row[f];
                        v.row[f] = out;
                    } else if (v.mode == 3) {
                        static uint64_t n = 0;
                        if (is_pow2(++n)) rt_log("vif1", "UNPACK with undefined STMOD=3; treating as 0 [#%" PRIu64 "]", n);
                    }
                    break;
                }
                case 1: out = v.row[f]; break;
                case 2: out = v.col[mcycle]; break;
                default: continue; /* 3: write-protected */
            }
            std::memcpy(dst + f * 4, &out, 4);
        }
    }

    ++v.unpacks;
    if (rt_trace() || is_pow2(v.unpacks)) {
        rt_log("vif1", "UNPACK #%" PRIu64 " cmd=0x%02x addr=0x%x num=%u flg=%d usn=%d cl=%u wl=%u mode=%u masked=%d",
            v.unpacks, cmd, addr, num, flg ? 1 : 0, usn ? 1 : 0, cl, wl, v.mode, masked ? 1 : 0);
    }
}

/* Payload words required for an UNPACK before it can execute. */
uint32_t unpack_words_needed(const Vif1& v, uint32_t code) {
    const uint32_t cmd = (code >> 24) & 0x7F;
    const uint32_t vn = (cmd >> 2) & 3;
    const uint32_t vl = cmd & 3;
    uint32_t num = (code >> 16) & 0xFF;
    if (num == 0) num = 256;
    uint32_t cl = v.cl, wl = v.wl;
    if (wl == 0) wl = 256;
    if (cl == 0) cl = 1;
    uint32_t input_vectors;
    if (wl > cl) {
        input_vectors = (num / wl) * cl;
        uint32_t rem = num % wl;
        input_vectors += rem < cl ? rem : cl;
    } else {
        input_vectors = num;
    }
    uint32_t bits_per_vector = (vl == 3) ? 16 : (vn + 1) * (32u >> vl);
    uint64_t total_bits = (uint64_t)input_vectors * bits_per_vector;
    return (uint32_t)((total_bits + 31) / 32);
}

/* ---- command execution -------------------------------------------------- */

void mscal_common(Vif1& v, uint32_t pc_bytes, const char* how) {
    ++v.mscals;
    v.top = v.tops;
    v.itop = v.itops;
    /* Double-buffer flip: TOPS points at the other half for the next
     * frame's uploads while the microprogram reads TOP. */
    v.dbf = !v.dbf;
    v.tops = (v.base + (v.dbf ? v.ofst : 0)) & 0x3FF;
    rt_vu1_mscal(pc_bytes, v.top, v.itop, how);
}

void exec_command(Vif1& v) {
    const uint32_t code = v.code;
    const uint32_t cmd = (code >> 24) & 0x7F;
    const uint32_t imm = code & 0xFFFF;

    if ((code & 0x80000000u) != 0) {
        /* i-bit: VIF1 interrupt, delivered deferred like everything else. */
        rt_log("vif1", "i-bit set on cmd 0x%02x: raising INTC VIF1", cmd);
        rt_intc_raise(5 /* VIF1 */);
    }

    if ((cmd & 0x60) == 0x60) {
        exec_unpack(v);
        return;
    }

    switch (cmd) {
        case 0x00: break; /* NOP */
        case 0x01: v.cl = imm & 0xFF; v.wl = (imm >> 8) & 0xFF; break; /* STCYCL */
        case 0x02: /* OFFSET */
            v.ofst = imm & 0x3FF;
            v.dbf = false;
            v.tops = v.base;
            break;
        case 0x03: v.base = imm & 0x3FF; break;  /* BASE */
        case 0x04: v.itops = imm & 0x3FF; break; /* ITOP */
        case 0x05: v.mode = imm & 3; break;      /* STMOD */
        case 0x06: /* MSKPATH3 */
            v.mskpath3 = (imm >> 15) & 1;
            rt_log("vif1", "MSKPATH3 %s (arbitration is synchronous; informational only)",
                v.mskpath3 ? "masked" : "unmasked");
            break;
        case 0x07: v.mark = imm; break;          /* MARK */
        case 0x10: case 0x11: case 0x13: break;  /* FLUSHE/FLUSH/FLUSHA: everything is synchronous */
        case 0x14: mscal_common(v, (uint32_t)imm * 8, "MSCAL"); break;
        case 0x15: mscal_common(v, (uint32_t)imm * 8, "MSCALF"); break;
        case 0x17: mscal_common(v, rt_vu1_state()->pc, "MSCNT"); break;
        case 0x20: v.mask = v.payload[0]; break; /* STMASK */
        case 0x30: for (int i = 0; i < 4; ++i) v.row[i] = v.payload[i]; break; /* STROW */
        case 0x31: for (int i = 0; i < 4; ++i) v.col[i] = v.payload[i]; break; /* STCOL */
        case 0x4A: { /* MPG */
            uint32_t num = (code >> 16) & 0xFF;
            if (num == 0) num = 256;
            uint32_t dst = ((uint32_t)imm * 8) & 0x3FFF;
            uint32_t bytes = num * 8;
            if (dst + bytes > 16384) {
                rt_log("vif1", "MPG target [0x%x, 0x%x) exceeds micro memory; clamping", dst, dst + bytes);
                bytes = 16384 - dst;
            }
            std::memcpy(rt_vu1_micro() + dst, v.payload.data(), bytes);
            rt_vu1_micro_written(dst, bytes);
            ++v.mpgs;
            rt_log("vif1", "MPG #%" PRIu64 ": %u instructions to micro 0x%x", v.mpgs, num, dst);
            break;
        }
        case 0x50: case 0x51: { /* DIRECT / DIRECTHL */
            ++v.directs;
            uint32_t qw = (uint32_t)v.payload.size() / 4;
            if (rt_trace() || is_pow2(v.directs)) {
                rt_log("vif1", "DIRECT%s #%" PRIu64 ": %u qw to GIF PATH2", cmd == 0x51 ? "HL" : "", v.directs, qw);
            }
            rt_gif_submit(1, reinterpret_cast<const uint8_t*>(v.payload.data()), qw);
            break;
        }
        default:
            /* Unknown VIFcode desynchronizes the stream; that is a fatal
             * model error, not something to limp past. */
            rt_fatal("vif1", nullptr, "unknown VIFcode 0x%08x (cmd 0x%02x) after %" PRIu64 " commands",
                code, cmd, v.cmds);
    }
}

uint32_t words_needed(const Vif1& v, uint32_t code) {
    const uint32_t cmd = (code >> 24) & 0x7F;
    if ((cmd & 0x60) == 0x60) return unpack_words_needed(v, code);
    switch (cmd) {
        case 0x20: return 1;             /* STMASK */
        case 0x30: case 0x31: return 4;  /* STROW/STCOL */
        case 0x4A: {                     /* MPG */
            uint32_t num = (code >> 16) & 0xFF;
            return (num == 0 ? 256 : num) * 2;
        }
        case 0x50: case 0x51: {          /* DIRECT/DIRECTHL */
            uint32_t imm = code & 0xFFFF;
            return (imm == 0 ? 65536 : imm) * 4;
        }
        default: return 0;
    }
}

} // namespace

void rt_vif1_feed(const uint32_t* words, uint32_t count) {
    Vif1& v = g_vif;
    for (uint32_t i = 0; i < count; ++i) {
        if (!v.pending) {
            v.code = words[i];
            v.need_words = words_needed(v, v.code);
            ++v.cmds;
            if (v.need_words == 0) {
                v.payload.clear();
                exec_command(v);
            } else {
                v.pending = true;
                v.payload.clear();
                v.payload.reserve(v.need_words);
            }
        } else {
            v.payload.push_back(words[i]);
            if ((uint32_t)v.payload.size() == v.need_words) {
                v.pending = false;
                exec_command(v);
            }
        }
    }
}

bool rt_hw_fifo_write128(uint32_t addr, const rc_u128* val) {
    switch (addr) {
        case 0x10004000: { /* VIF0 FIFO: counted and discarded, see header */
            g_vif0_words += 4;
            if (is_pow2(g_vif0_words / 4)) {
                rt_log("vif1", "VIF0 FIFO write discarded (words=0x%08x 0x%08x 0x%08x 0x%08x) [qw #%" PRIu64 "]",
                    val->u32x[0], val->u32x[1], val->u32x[2], val->u32x[3], g_vif0_words / 4);
            }
            return true;
        }
        case 0x10005000: /* VIF1 FIFO */
            rt_vif1_feed(val->u32x, 4);
            return true;
        case 0x10006000: /* GIF FIFO: direct PATH3 qword */
            rt_gif_submit(2, val->u8x, 1);
            return true;
        case 0x10007010: /* toIPU FIFO: SETIQ table priming by CPU store */
            rt_ipu_fifo_feed(val->u8x);
            return true;
        default:
            return false;
    }
}

bool rt_vif_mmio_read(uint32_t addr, uint32_t* out) {
    const Vif1& v = g_vif;
    switch (addr) {
        case 0x10003C00: /* VIF1_STAT: idle, FIFO empty; DBF mirrors state */
            *out = v.dbf ? 0x80u : 0;
            return true;
        case 0x10003C20: *out = v.err; return true;
        case 0x10003C30: *out = v.mark; return true;
        case 0x10003C40: *out = v.cl | (v.wl << 8); return true;
        case 0x10003C50: *out = v.mode; return true;
        case 0x10003C60: *out = 0; return true; /* VIF1_NUM: nothing in flight */
        case 0x10003C70: *out = v.mask; return true;
        case 0x10003C80: *out = v.code; return true;
        case 0x10003C90: *out = v.itops; return true;
        case 0x10003CA0: *out = v.base; return true;
        case 0x10003CB0: *out = v.ofst; return true;
        case 0x10003CC0: *out = v.tops; return true;
        case 0x10003CD0: *out = v.itop; return true;
        case 0x10003CE0: *out = v.top; return true;
        case 0x10003C10: *out = 0; return true; /* VIF1_FBRST: write-only */
        case 0x10003800: *out = 0; return true; /* VIF0_STAT: idle */
        default:
            return false;
    }
}

bool rt_vif_mmio_write(uint32_t addr, uint32_t v) {
    switch (addr) {
        case 0x10003C10: /* VIF1_FBRST: RST clears the interpreter state */
            if (v & 1) {
                uint64_t cmds = g_vif.cmds;
                g_vif = Vif1{};
                g_vif.cmds = cmds;
                rt_log("vif1", "VIF1_FBRST reset");
            }
            return true;
        case 0x10003C20: g_vif.err = v; return true;  /* VIF1_ERR */
        case 0x10003C30: g_vif.mark = v; return true; /* VIF1_MARK */
        case 0x10003C00: return true;                 /* VIF1_STAT: FDR etc, accept */
        /* VIF0 register writes: accept silently (reset sequence). */
        case 0x10003800: case 0x10003810: case 0x10003820: case 0x10003830:
            return true;
        default:
            return false;
    }
}
