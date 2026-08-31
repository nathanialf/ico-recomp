/* sif/pad.cpp: the padman RPC service (old wire protocol) of the virtual IOP.
 *
 * The retail ICO ELF links the old (SDK 2.2-era) EE libpad; every protocol
 * fact below was read out of that library's disassembly in the decomp
 * checkout (asm/.../src/cod/vendor_24AAC8 + vendor_24E9D8, functions
 * identified against the Aug-6-2001 map's scePad* names) plus the game's
 * own pad layer (ios/pad.c, iosPadRead/iosPadDevInit asm). Facts, with the
 * function they were read from:
 *
 *  - RPC server ids 0x80000100 (commands) and 0x80000101 (bound but never
 *    called by this library). Every request is a 128-byte block, fno=1,
 *    word 0 = command; the server answers in-place (same block layout).
 *  - scePadPortOpen (func_0024E228): cmd 0x01, port@+4 slot@+8 and the EE
 *    pad data area address@+16; reply result@+12 and an IOP-side actuator
 *    receive address@+20 (see actuators below).
 *  - scePadSetMainMode (func_0024EB10): cmd 0x06, offs@+12 lock@+16, reply
 *    result@+20 == 1, then the EE sets reqState=BUSY in the pad area and
 *    polls scePadGetReqState until the IOP-written frames clear it.
 *  - scePadEnterPressMode (func_0024EF20 -> func_0024EE10): cmd 0x0A,
 *    button mask@+12 (0xFFF), reply result@+16 == 1.
 *  - scePadSetActAlign (func_0024EC80): cmd 0x08, 6 align bytes@+12..17,
 *    reply result@+20. ICO sends {0,1,0xFF,0xFF,0xFF,0xFF}.
 *  - scePadInit (func_0024DFC8/func_0024E108): GET_MODVER cmd 0x12 (reply
 *    @+12, must have major 4) then INIT cmd 0x10 (reply@+12).
 *
 * EE pad data area (registered at OPEN): two 128-byte pad_data halves the
 * IOP rewrites wholesale every field; libpad reads whichever half has the
 * larger frame counter (func_0024E4C8). Field offsets consumed by this
 * libpad (matching ps2sdk's old-padman pad_data struct, a public SDK
 * layout):
 *    +0x00 32-byte controller frame  (scePadRead copies length bytes of it)
 *    +0x30 actuator info, 4 bytes per actuator      (scePadInfoAct)
 *    +0x50 u16 modeTable[4]                          (scePadInfoMode)
 *    +0x58 u32 frame counter    +0x60 u32 length
 *    +0x64 modeConfig  +0x65 modeCurId  +0x66 model  +0x67 buttonDataReady
 *    +0x68 nrOfModes   +0x69 modeCurOffs +0x6A nrOfActuators +0x6B numActComb
 *    +0x6D mode        +0x6E lock
 *    +0x70 state       +0x71 reqState    +0x72 currentTask
 *    +0x79..+0x7C press-mode capability bits         (scePadInfoPressMode,
 *          must read 0x0003FFFF for scePadEnterPressMode to proceed)
 *
 * Controller frame (public DS2 pad protocol): status, id (0x41 digital /
 * 0x73 analog / 0x79 DUALSHOCK2 with pressures), 16 button bits active-low,
 * 4 stick axes (rx ry lx ly, 0x80 centered), 12 pressure bytes in the order
 * right left up down triangle circle cross square L1 R1 L2 R2.
 *
 * Actuators: the EE never sends per-frame rumble over RPC. scePadSetActDirect
 * (func_0024EBC8 + func_0024DE98) raw-DMAs a 32-byte block {counter, dirty,
 * size, 6 value bytes@+12} to the IOP address the OPEN reply named,
 * alternating +0x00/+0x20 by counter parity. The per-field tick reads the
 * fresher block and forwards the values to the host input layer.
 *
 * The game's init state machine (iosPadDevInit) requires, in order: state
 * STABLE(6), InfoMode CURID/CUREXID = 7, SET_MMODE(offs=1, lock=3) success,
 * reqState COMPLETE, press capability 0x3FFFF, cmd 0x0A success, InfoAct
 * count > 0, SET_ACTALIGN success, then treats state 0x63 as "running".
 * A powered-on DS2 starts digital (0x41); the CUREXID=4 path sets the
 * analog-wanted flag and locks analog mode first, so this virtual pad also
 * starts digital and switches on SET_MMODE, like real hardware.
 */
#include "rpc.h"

#include "../host/input.h"

#include <cinttypes>
#include <cstring>

namespace {

/* Minted virtual-IOP addresses for the actuator receive blocks handed out
 * by OPEN: between the RPC server structs (0x1A0000+) and the staging
 * buffers (0x1C0000+). 0x40 bytes per port (two 0x20 halves). */
constexpr uint32_t kActBufBase = 0x001B0000u;
constexpr uint32_t kActBufStride = 0x40;

constexpr uint32_t kPorts = 2;

uint32_t rd32(const uint8_t* p, uint32_t off) { uint32_t v; std::memcpy(&v, p + off, 4); return v; }
void wr32(uint8_t* p, uint32_t off, uint32_t v) { if (p) std::memcpy(p + off, &v, 4); }

struct VirtualPort {
    bool open = false;
    uint32_t ee_area = 0;       /* 256-byte pad data area (2 x 128) */
    uint32_t frame = 0;
    uint8_t mode_offs = 0;      /* 0 = digital (0x41), 1 = analog */
    uint8_t lock = 0;           /* SET_MMODE lock argument (3 = locked) */
    bool pressures = false;     /* cmd 0x0A received */
    uint8_t act_align[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t act_counter = 0;   /* last consumed actuator DMA counter */
};

VirtualPort g_port[kPorts];
uint64_t g_field = 0;           /* pad field counter, ticks from boot */
uint64_t g_next_tick = RT_CYCLES_PER_FIELD;

/* ---- per-field frame delivery -------------------------------------------- */

void build_frame_connected(uint8_t d[128], VirtualPort& vp, const RtPadState& st) {
    uint8_t id = vp.mode_offs == 0 ? 0x41 : (vp.pressures ? 0x79 : 0x73);
    d[0] = 0x00;                            /* status: ok */
    d[1] = id;
    uint16_t wire = (uint16_t)~st.buttons;  /* active-low */
    d[2] = (uint8_t)(wire & 0xFF);
    d[3] = (uint8_t)(wire >> 8);
    d[4] = st.rx; d[5] = st.ry; d[6] = st.lx; d[7] = st.ly;
    if (vp.pressures) {
        const uint16_t order[12] = {
            RT_PAD_RIGHT, RT_PAD_LEFT, RT_PAD_UP, RT_PAD_DOWN,
            RT_PAD_TRIANGLE, RT_PAD_CIRCLE, RT_PAD_CROSS, RT_PAD_SQUARE,
            RT_PAD_L1, RT_PAD_R1, RT_PAD_L2, RT_PAD_R2,
        };
        for (int i = 0; i < 12; ++i) {
            d[8 + i] = (st.buttons & order[i]) ? 0xFF : 0x00;
        }
    }
    /* actData: {func, sub, size, curr} per actuator; DS2 has the on/off
     * small motor and the 0..255 big motor. */
    d[0x30] = 1; d[0x31] = 1; d[0x32] = 1; d[0x33] = 0;
    d[0x34] = 1; d[0x35] = 2; d[0x36] = 1; d[0x37] = 0;
    d[0x50] = 4; d[0x51] = 0;               /* modeTable[0] = DIGITAL */
    d[0x52] = 7; d[0x53] = 0;               /* modeTable[1] = ANALOG */
    wr32(d, 0x58, vp.frame);
    wr32(d, 0x5C, 0);                       /* findPadRetries */
    wr32(d, 0x60, 32);                      /* length */
    d[0x64] = 2;                            /* modeConfig: configured */
    d[0x65] = id;                           /* modeCurId */
    d[0x66] = 3;                            /* model: DUALSHOCK2 */
    d[0x67] = 1;                            /* buttonDataReady */
    d[0x68] = 2;                            /* nrOfModes */
    d[0x69] = vp.mode_offs;                 /* modeCurOffs */
    d[0x6A] = 2;                            /* nrOfActuators */
    d[0x6B] = 1;                            /* numActComb */
    d[0x6D] = vp.mode_offs;                 /* mode */
    d[0x6E] = vp.lock;                      /* lock */
    d[0x6F] = 6;                            /* actDirSize */
    d[0x70] = 6;                            /* state: STABLE */
    d[0x71] = 0;                            /* reqState: COMPLETE */
    d[0x72] = 1;                            /* currentTask */
    /* Press-mode capability: 18 pressure-capable bits (DS2). */
    d[0x79] = 0xFF; d[0x7A] = 0xFF; d[0x7B] = 0x03; d[0x7C] = 0x00;
}

void build_frame_empty(uint8_t d[128], const VirtualPort& vp) {
    d[0] = 0xFF;                            /* status: no response */
    wr32(d, 0x58, vp.frame);
    wr32(d, 0x60, 0);                       /* length 0 */
    d[0x70] = 0;                            /* state: DISCONNECTED */
    d[0x71] = 0;
    d[0x72] = 1;
}

void consume_actuators(int port, VirtualPort& vp) {
    /* The fresher of the two 0x20 actuator blocks the EE DMAs to the minted
     * IOP address (see the header comment). */
    const uint8_t* base = rt_iop_ptr(kActBufBase + (uint32_t)port * kActBufStride);
    uint32_t c0 = rd32(base, 0x00);
    uint32_t c1 = rd32(base, 0x20);
    const uint8_t* blk = c1 > c0 ? base + 0x20 : base;
    uint32_t counter = c1 > c0 ? c1 : c0;
    if (counter == 0 || counter == vp.act_counter) return;
    if (vp.act_counter == 0 || (counter & (counter - 1)) == 0) {
        /* The counter is written by the game's own per-frame pad loop
         * (iosPadRead -> ShockRequestBox -> scePadSetActDirect DMA), which
         * only runs after the DS2 init state machine consumed our frames:
         * it advancing is the "pad reads alive" acceptance signal. */
        rt_log("pad", "port %d pad loop alive: game actuator counter=%u (pad frame %u)",
            port, counter, vp.frame);
    }
    vp.act_counter = counter;
    /* Block: +4 dirty flag, +8 size, +12 six value bytes in act-align
     * order. ICO aligns {0=small, 1=big}. */
    uint8_t small_v = 0, big_v = 0;
    for (int i = 0; i < 6; ++i) {
        uint8_t act = vp.act_align[i];
        if (act == 0) small_v = blk[12 + i];
        else if (act == 1) big_v = blk[12 + i];
    }
    rt_input_set_actuators(port, small_v, big_v);
}

void pad_field_tick() {
    ++g_field;
    rt_input_poll(g_field);
    for (uint32_t port = 0; port < kPorts; ++port) {
        VirtualPort& vp = g_port[port];
        if (!vp.open || !vp.ee_area) continue;
        uint8_t d[128] = {0};
        RtPadState st;
        if (rt_input_get((int)port, &st)) build_frame_connected(d, vp, st);
        else build_frame_empty(d, vp);
        rt_gwrite_bytes(vp.ee_area + (vp.frame & 1u) * 0x80u, d, sizeof(d));
        ++vp.frame;
        consume_actuators((int)port, vp);
    }
}

/* ---- the RPC service ------------------------------------------------------ */

void svc_padman(uint32_t cmd_or_fno, const uint8_t* send, uint32_t send_size,
                uint8_t* recv, uint32_t recv_size) {
    /* fno is always 1 in the old protocol; the command is word 0 of the
     * 128-byte block, echoed back with result words patched in place. */
    if (!send || send_size < 20) {
        rt_log("pad", "WARNING padman call with short send (fno=%u, %u bytes): ignored",
            cmd_or_fno, send_size);
        return;
    }
    uint32_t cmd = rd32(send, 0);
    uint32_t n = send_size < recv_size ? send_size : recv_size;
    if (n) std::memcpy(recv, send, n);
    uint32_t port = rd32(send, 4);
    uint32_t slot = rd32(send, 8);
    switch (cmd) {
        case 0x01: { /* OPEN */
            uint32_t area = rd32(send, 16) & 0x1FFFFFFFu;
            if (port >= kPorts || slot != 0) {
                wr32(recv, 12, 0);
                rt_log("pad", "OPEN port=%u slot=%u: out of range -> 0", port, slot);
                break;
            }
            VirtualPort& vp = g_port[port];
            vp.open = true;
            vp.ee_area = area;
            vp.frame = 0;
            vp.mode_offs = 0;
            vp.lock = 0;
            vp.pressures = false;
            vp.act_counter = 0;
            wr32(recv, 12, 1);
            wr32(recv, 20, kActBufBase + port * kActBufStride); /* IOP actuator buf */
            rt_log("pad", "OPEN port=%u slot=%u pad_area=0x%08x -> 1 (actbuf=0x%06x, "
                "frames start next field)", port, slot, area,
                kActBufBase + port * kActBufStride);
            break;
        }
        case 0x02: /* CLOSE / END */
            if (port < kPorts) g_port[port].open = false;
            wr32(recv, 12, 1);
            rt_log("pad", "CLOSE port=%u slot=%u -> 1", port, slot);
            break;
        case 0x06: { /* SET_MMODE (analog lock) */
            uint32_t offs = rd32(send, 12);
            uint32_t lock = rd32(send, 16);
            if (port < kPorts) {
                g_port[port].mode_offs = offs ? 1 : 0;
                g_port[port].lock = (uint8_t)lock;
            }
            wr32(recv, 20, 1);
            rt_log("pad", "SET_MMODE port=%u slot=%u offs=%u lock=%u -> 1 (%s)",
                port, slot, offs, lock, offs ? "analog" : "digital");
            break;
        }
        case 0x03: case 0x04: case 0x05:
            /* INFO_ACT / INFO_COMB / INFO_MODE over RPC: this libpad reads
             * them out of the pad data area instead; answered empty. */
            wr32(recv, 20, 0);
            rt_log("pad", "info cmd=0x%02x port=%u slot=%u -> 0 (libpad reads the pad area)",
                cmd, port, slot);
            break;
        case 0x07: { /* SET_ACTDIRECT (this game uses the raw-DMA path instead) */
            if (port < kPorts && send_size >= 18) {
                const VirtualPort& vp = g_port[port];
                uint8_t small_v = 0, big_v = 0;
                for (int i = 0; i < 6; ++i) {
                    if (vp.act_align[i] == 0) small_v = send[12 + i];
                    else if (vp.act_align[i] == 1) big_v = send[12 + i];
                }
                rt_input_set_actuators((int)port, small_v, big_v);
            }
            wr32(recv, 20, 1);
            rt_log("pad", "SET_ACTDIRECT port=%u slot=%u -> 1", port, slot);
            break;
        }
        case 0x08: { /* SET_ACTALIGN */
            uint8_t a[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            if (send_size >= 18) std::memcpy(a, send + 12, 6);
            if (port < kPorts) std::memcpy(g_port[port].act_align, a, 6);
            wr32(recv, 20, 1);
            rt_log("pad", "SET_ACTALIGN port=%u slot=%u {%u,%u,%u,%u,%u,%u} -> 1",
                port, slot, a[0], a[1], a[2], a[3], a[4], a[5]);
            break;
        }
        case 0x0A: /* SET_BUTTON_INFO: enter press mode */
            if (port < kPorts) g_port[port].pressures = true;
            wr32(recv, 16, 1);
            rt_log("pad", "SET_BUTTON_INFO port=%u slot=%u mask=0x%x -> 1 (pressures on)",
                port, slot, send_size >= 16 ? rd32(send, 12) : 0);
            break;
        case 0x10: /* INIT */
            wr32(recv, 12, 1);
            rt_log("pad", "INIT -> 1");
            break;
        case 0x12: /* GET_MODVER */
            wr32(recv, 12, 0x0400); /* major 4: what this libpad requires */
            rt_log("pad", "GET_MODVER -> 4.0");
            break;
        default:
            rt_log("pad", "WARNING padman cmd=0x%02x NOT MODELED (fno=%u send_size=%u "
                "recv_size=%u): echoed request back unchanged", cmd, cmd_or_fno,
                send_size, recv_size);
            break;
    }
}

} // namespace

void rt_pad_register_services() {
    rt_input_init();
    rt_rpc_register_service(0x80000100, "padman", svc_padman);
    rt_rpc_register_service(0x80000101, "padman-ext(stub)", nullptr);
}

uint64_t rt_pad_next_event() { return g_next_tick; }

void rt_pad_run_due() {
    while (g_next_tick <= rt_clock_now()) {
        g_next_tick += RT_CYCLES_PER_FIELD;
        pad_field_tick();
    }
}
