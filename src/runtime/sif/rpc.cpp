/* sif/rpc.cpp: the SIF RPC layer of the virtual IOP.
 *
 * The EE side runs the game's own (translated) Sony sifrpc client code; this
 * module plays the IOP end of the wire protocol:
 *
 *   - Every SifSetDma entry is copied into a 2 MB virtual IOP RAM. Entries
 *     addressed to RT_SIF_IOP_CMDBUF are sifcmd packets and get parsed.
 *   - INIT_CMD (0x80000002, opt 0): records the EE's sifcmd receive buffer
 *     and queues the SET_SREG(0,1) "RPC layer up" response.
 *   - RPC BIND (0x80000009): looks the server id up in the service registry
 *     and queues an RPC END packet carrying a nonzero server pointer and the
 *     service's IOP receive-buffer addresses. The EE library's _request_end
 *     parses it, fills the client struct and signals the caller's semaphore.
 *   - RPC CALL (0x8000000a): dispatches (service, fno, send data staged in
 *     IOP RAM, recv size). The handler's receive bytes are written to the
 *     EE receive buffer immediately (DMA is synchronous per CLAUDE.md); the
 *     completion is an RPC END packet queued for deferred delivery. rmode=0
 *     calls (NOWAIT without an end function) get no END packet; instead the
 *     EE-side call packet is released directly (rpc_id=0, alloc bit clear),
 *     which is what sceSifCheckStatRpc polls.
 *   - RPC RDATA (0x8000000c, sceSifGetOtherData): copies virtual IOP RAM to
 *     the EE and queues an END packet.
 *
 * Delivery is deferred through the virtual-clock timeline (never inside the
 * SifSetDma syscall's register writeback): a queued packet is written into
 * the EE receive buffer and DMAC SIF0 raised only when its due time passes
 * AND the buffer's psize byte reads 0 (the EE interrupt handler zeroes it
 * after copying a packet out, so this models the real flow control).
 *
 * Packet layouts per ps2sdk common/include/sifrpc-common.h and
 * ee/kernel/src/sifrpc.c (clean-room structural reference). All byte
 * offsets below are from the start of the packet:
 *   sifcmd header: +0 psize:8|dsize:24, +4 dest, +8 cid, +12 opt
 *   BIND:  +16 rec_id +20 pkt_addr +24 rpc_id +28 client +32 sid
 *   CALL:  +16 rec_id +20 pkt_addr +24 rpc_id +28 client +32 fno
 *          +36 send_size +40 recvbuf +44 recv_size +48 rmode +52 server
 *   RDATA: +16 rec_id +20 pkt_addr +24 rpc_id +28 recv_data +32 src
 *          +36 dest +40 size
 *   END:   +16 rec_id +20 pkt_addr +24 rpc_id +28 client +32 request cid
 *          +36 server +40 buff +44 cbuff
 */
#include "rpc.h"

#include <cinttypes>
#include <cstring>
#include <deque>
#include <vector>

namespace {

constexpr uint32_t SIF_CMD_SET_SREG = 0x80000001u;
constexpr uint32_t SIF_CMD_INIT_CMD = 0x80000002u;
constexpr uint32_t SIF_CMD_RPC_END = 0x80000008u;
constexpr uint32_t SIF_CMD_RPC_BIND = 0x80000009u;
constexpr uint32_t SIF_CMD_RPC_CALL = 0x8000000Au;
constexpr uint32_t SIF_CMD_RPC_RDATA = 0x8000000Cu;

/* Virtual SIF latency for a response, in bus cycles (~28 us). Well under a
 * field; h_SifSetDma ticks the clock past this so a response is deliverable
 * at the very syscall that requested it. */
constexpr uint64_t kSifLatency = 4096;
/* Retry interval when the EE receive buffer still holds an unconsumed
 * packet. */
constexpr uint64_t kBusyRetry = 512;

constexpr uint32_t kRpcPacketSize = 64;

/* Minted virtual-IOP addresses. Server structs and per-service receive
 * buffers sit below the sifcmd command buffer and the iopheap heap; the
 * whole IOP address map is documented in rpc.h. kMaxServices is bounded by
 * that map rather than by the service list: 17 services register today, and
 * 24 x kBufStride is what fits between low memory and the command buffer.
 * Overflowing it is a fatal in rt_rpc_register_service, never a silently
 * dropped service. */
constexpr uint32_t kServerBase = 0x00070000u;
constexpr uint32_t kServerStride = 0x80;
constexpr uint32_t kBufBase = 0x00010000u;
constexpr uint32_t kBufStride = 0x4000; /* 16 KB staging per service */

uint8_t g_iop_ram[RT_IOP_RAM_SIZE];

uint32_t g_ee_pktbuf = 0;       /* EE sifcmd receive buffer (INIT_CMD) */

struct Service {
    uint32_t sid = 0;
    const char* name = nullptr;
    RtRpcServiceFn fn = nullptr;
    uint32_t server_iop = 0;    /* minted SifRpcServerData address */
    uint32_t buf_iop = 0;       /* minted receive/staging buffer */
    uint64_t binds = 0;
    uint64_t calls = 0;
};

constexpr int kMaxServices = 24;
Service g_services[kMaxServices];
int g_service_count = 0;

static_assert(kBufBase + uint32_t(kMaxServices) * kBufStride <= kServerBase,
    "RPC staging buffers run into the minted server structs");
static_assert(kServerBase + uint32_t(kMaxServices) * kServerStride <= RT_SIF_IOP_CMDBUF,
    "minted server structs run into the sifcmd command buffer");

struct Delivery {
    enum class Kind { CmdPacket, FreeEePkt } kind;
    uint64_t due = 0;
    uint32_t pkt_size = 0;
    uint8_t pkt[kRpcPacketSize] = {0};
    uint32_t ee_pkt_addr = 0;   /* FreeEePkt: EE-side rpc packet to release */
    const char* what = "";
};

std::deque<Delivery> g_queue;
uint64_t g_delivered = 0;

Service* find_service_by_sid(uint32_t sid) {
    for (int i = 0; i < g_service_count; ++i) {
        if (g_services[i].sid == sid) return &g_services[i];
    }
    return nullptr;
}

Service* find_service_by_server(uint32_t server_iop) {
    for (int i = 0; i < g_service_count; ++i) {
        if (g_services[i].server_iop == server_iop) return &g_services[i];
    }
    return nullptr;
}

void put32(uint8_t* p, uint32_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }
uint32_t get32(const uint8_t* p, uint32_t off) { uint32_t v; std::memcpy(&v, p + off, 4); return v; }

void queue_packet(const uint8_t* pkt, uint32_t size, const char* what) {
    Delivery d;
    d.kind = Delivery::Kind::CmdPacket;
    d.due = rt_clock_now() + kSifLatency;
    d.pkt_size = size <= kRpcPacketSize ? size : kRpcPacketSize;
    std::memcpy(d.pkt, pkt, d.pkt_size);
    d.what = what;
    g_queue.push_back(d);
}

/* Builds the common part of an RPC END packet from a request packet. */
void build_end_packet(uint8_t* out, const uint8_t* req, uint32_t req_cid) {
    std::memset(out, 0, kRpcPacketSize);
    put32(out, 0, kRpcPacketSize);          /* psize=64, dsize=0 */
    put32(out, 8, SIF_CMD_RPC_END);
    put32(out, 16, get32(req, 16));         /* rec_id echo */
    put32(out, 20, get32(req, 20));         /* pkt_addr echo */
    put32(out, 24, get32(req, 24));         /* rpc_id echo */
    put32(out, 28, get32(req, 28));         /* client */
    put32(out, 32, req_cid);
}

void handle_init_cmd(const uint8_t* pkt, uint32_t size) {
    uint32_t opt = get32(pkt, 12);
    if (size >= 20 && opt == 0) {
        g_ee_pktbuf = get32(pkt, 16) & 0x1FFFFFFFu;
        /* SET_SREG(sreg 0 = RPCINIT, value 1) response, per ps2sdk
         * ee/kernel/src/sifcmd.c init flow. */
        uint8_t resp[24] = {0};
        put32(resp, 0, 24);                 /* psize=24, dsize=0 */
        put32(resp, 8, SIF_CMD_SET_SREG);
        put32(resp, 16, 0);                 /* sreg index 0 */
        put32(resp, 20, 1);                 /* value 1 */
        queue_packet(resp, sizeof(resp), "SET_SREG(0,1)");
        rt_log("rpc", "sifcmd INIT_CMD: EE pktbuf=0x%08x, SET_SREG(0,1) queued", g_ee_pktbuf);
    } else {
        rt_log("rpc", "sifcmd INIT_CMD with opt=%u (size=%u): no response modeled", opt, size);
    }
}

void handle_bind(const uint8_t* pkt) {
    uint32_t client = get32(pkt, 28);
    uint32_t sid = get32(pkt, 32);
    Service* s = find_service_by_sid(sid);
    if (!s) {
        rt_fatal("rpc", rt_sched_current_ctx(),
            "RPC BIND for unregistered server id 0x%08x (client=0x%08x). "
            "Register a service or a loud stub in sif/cdvd.cpp.", sid, client);
    }
    ++s->binds;
    uint8_t end[kRpcPacketSize];
    build_end_packet(end, pkt, SIF_CMD_RPC_BIND);
    put32(end, 36, s->server_iop);          /* server: nonzero = bound */
    put32(end, 40, s->buf_iop);             /* buff: where CALL send data goes */
    put32(end, 44, 0);                      /* cbuff: unused by these services */
    queue_packet(end, kRpcPacketSize, s->name);
    rt_log("rpc", "BIND sid=0x%08x (%s) client=0x%08x -> server=0x%06x buff=0x%06x",
        sid, s->name, client, s->server_iop, s->buf_iop);
}

void handle_call(const uint8_t* pkt) {
    uint32_t client = get32(pkt, 28);
    uint32_t fno = get32(pkt, 32);
    uint32_t send_size = get32(pkt, 36);
    uint32_t recvbuf = get32(pkt, 40) & 0x1FFFFFFFu;
    uint32_t recv_size = get32(pkt, 44);
    uint32_t rmode = get32(pkt, 48);
    uint32_t server = get32(pkt, 52);
    Service* s = find_service_by_server(server);
    if (!s) {
        rt_fatal("rpc", rt_sched_current_ctx(),
            "RPC CALL to unknown server 0x%08x (client=0x%08x fno=0x%x): "
            "no service was bound at this address", server, client, fno);
    }
    ++s->calls;
    if (send_size > kBufStride) {
        rt_fatal("rpc", rt_sched_current_ctx(),
            "RPC CALL %s fno=0x%x send_size=%u exceeds the %u-byte service staging buffer",
            s->name, fno, send_size, kBufStride);
    }
    if (recv_size > 0x100000) {
        rt_fatal("rpc", rt_sched_current_ctx(),
            "RPC CALL %s fno=0x%x recv_size=%u is implausible", s->name, fno, recv_size);
    }
    if (rt_trace()) {
        rt_log("rpc", "CALL %s fno=0x%x send=%u recv=0x%08x+%u rmode=%u client=0x%08x",
            s->name, fno, send_size, recvbuf, recv_size, rmode, client);
    }

    /* Over-allocate so a handler writing a fixed-size result struct cannot
     * overrun a caller's smaller recv declaration; only recv_size bytes are
     * copied back to the EE. */
    std::vector<uint8_t> recv(recv_size < 64 ? 64 : recv_size, 0);
    const uint8_t* send = send_size ? rt_iop_ptr(s->buf_iop) : nullptr;
    if (s->fn) {
        s->fn(fno, send, send_size, recv.data(), recv_size);
    } else {
        static uint64_t stub_calls = 0;
        ++stub_calls;
        rt_log("rpc", "STUB CALL %s fno=0x%x send_size=%u recv_size=%u: returning zeroed recv "
            "[stub call #%" PRIu64 "]", s->name, fno, send_size, recv_size, stub_calls);
    }

    /* Receive data lands synchronously (DMA is synchronous, CLAUDE.md); the
     * completion below is deferred. */
    if (recv_size && recvbuf) rt_gwrite_bytes(recvbuf, recv.data(), recv_size);

    if (rmode) {
        uint8_t end[kRpcPacketSize];
        build_end_packet(end, pkt, SIF_CMD_RPC_CALL);
        put32(end, 36, server);
        queue_packet(end, kRpcPacketSize, s->name);
    } else {
        /* NOWAIT call without an end function: the IOP sends no END packet;
         * it releases the EE-side call packet so sceSifCheckStatRpc sees the
         * call complete (ps2sdk sceSifExecRequest, rmode==0 path). */
        Delivery d;
        d.kind = Delivery::Kind::FreeEePkt;
        d.due = rt_clock_now() + kSifLatency;
        d.ee_pkt_addr = get32(pkt, 20) & 0x1FFFFFFFu;
        d.what = s->name;
        g_queue.push_back(d);
    }
}

void handle_rdata(const uint8_t* pkt) {
    uint32_t src = get32(pkt, 32);
    uint32_t dest = get32(pkt, 36) & 0x1FFFFFFFu;
    uint32_t size = get32(pkt, 40);
    rt_log("rpc", "RDATA (SifGetOtherData): IOP 0x%06x -> EE 0x%08x, %u bytes", src, dest, size);
    if (size > RT_IOP_RAM_SIZE) {
        rt_fatal("rpc", rt_sched_current_ctx(), "RDATA size %u is implausible", size);
    }
    for (uint32_t i = 0; i < size; ++i) {
        rt_gwrite_bytes(dest + i, rt_iop_ptr(src + i), 1);
    }
    uint8_t end[kRpcPacketSize];
    build_end_packet(end, pkt, SIF_CMD_RPC_RDATA);
    queue_packet(end, kRpcPacketSize, "rdata");
}

} // namespace

uint8_t* rt_iop_ptr(uint32_t addr) {
    return &g_iop_ram[addr & (RT_IOP_RAM_SIZE - 1)];
}

void rt_gread_bytes(uint32_t addr, void* dst, uint32_t n) {
    uint8_t* out = static_cast<uint8_t*>(dst);
    uint32_t off = 0;
    while (off < n) {
        uint8_t* p = rt_gptr(addr + off);
        if (!p) rt_fatal("rpc", rt_sched_current_ctx(), "guest block read of unmapped 0x%08x", addr + off);
        uint32_t chunk = 0x10000u - ((addr + off) & 0xFFFFu);
        if (chunk > n - off) chunk = n - off;
        std::memcpy(out + off, p, chunk);
        off += chunk;
    }
}

void rt_gwrite_bytes(uint32_t addr, const void* src, uint32_t n) {
    const uint8_t* in = static_cast<const uint8_t*>(src);
    uint32_t off = 0;
    while (off < n) {
        uint8_t* p = rt_gptr(addr + off);
        if (!p) rt_fatal("rpc", rt_sched_current_ctx(), "guest block write of unmapped 0x%08x", addr + off);
        uint32_t chunk = 0x10000u - ((addr + off) & 0xFFFFu);
        if (chunk > n - off) chunk = n - off;
        std::memcpy(p, in + off, chunk);
        off += chunk;
    }
}

void rt_rpc_init() {
    std::memset(g_iop_ram, 0, sizeof(g_iop_ram));
    g_ee_pktbuf = 0;
    g_queue.clear();
    g_service_count = 0;
    g_delivered = 0;
    rt_cdvd_register_services();
    rt_log("rpc", "RPC layer up: %d services registered", g_service_count);
}

void rt_rpc_register_service(uint32_t sid, const char* name, RtRpcServiceFn fn) {
    if (g_service_count >= kMaxServices) {
        rt_fatal("rpc", nullptr, "service registry full registering 0x%08x (%s)", sid, name);
    }
    Service& s = g_services[g_service_count];
    s = Service{};
    s.sid = sid;
    s.name = name;
    s.fn = fn;
    s.server_iop = kServerBase + uint32_t(g_service_count) * kServerStride;
    s.buf_iop = kBufBase + uint32_t(g_service_count) * kBufStride;
    ++g_service_count;
}

void rt_rpc_on_dma_entry(uint32_t src_ee, uint32_t dest_iop, uint32_t size) {
    if (size == 0 || size > RT_IOP_RAM_SIZE) return;
    /* Stage the payload into virtual IOP RAM (send data for a later CALL,
     * or the command packet itself). */
    uint8_t* dst = rt_iop_ptr(dest_iop);
    uint32_t room = RT_IOP_RAM_SIZE - (dest_iop & (RT_IOP_RAM_SIZE - 1));
    uint32_t n = size < room ? size : room;
    rt_gread_bytes(src_ee, dst, n);

    if ((dest_iop & 0x1FFFFFFFu) != RT_SIF_IOP_CMDBUF || size < 16) return;

    const uint8_t* pkt = rt_iop_ptr(RT_SIF_IOP_CMDBUF);
    uint32_t cid = get32(pkt, 8);
    switch (cid) {
        case SIF_CMD_INIT_CMD:
            handle_init_cmd(pkt, size);
            break;
        case SIF_CMD_RPC_BIND:
            if (size >= 36) handle_bind(pkt);
            break;
        case SIF_CMD_RPC_CALL:
            if (size >= 56) handle_call(pkt);
            break;
        case SIF_CMD_RPC_RDATA:
            if (size >= 44) handle_rdata(pkt);
            break;
        default:
            rt_log("rpc", "unhandled sifcmd packet cid=0x%08x size=%u to the IOP: ignored", cid, size);
            break;
    }
}

uint64_t rt_rpc_next_event() {
    return g_queue.empty() ? UINT64_MAX : g_queue.front().due;
}

void rt_rpc_run_due() {
    while (!g_queue.empty()) {
        Delivery& d = g_queue.front();
        if (d.due > rt_clock_now()) break;
        if (d.kind == Delivery::Kind::FreeEePkt) {
            /* Release the EE-side rpc packet: rpc_id=0, alloc bit clear
             * (ps2sdk rpc_packet_free; sceSifCheckStatRpc then reports the
             * call finished). */
            if (rt_gptr(d.ee_pkt_addr)) {
                rt_gwrite32(d.ee_pkt_addr + 24, 0);
                rt_gwrite32(d.ee_pkt_addr + 16, rt_gread32(d.ee_pkt_addr + 16) & ~1u);
            } else {
                rt_log("rpc", "FreeEePkt for %s dropped: EE packet 0x%08x unmapped", d.what, d.ee_pkt_addr);
            }
            if (rt_trace()) rt_log("rpc", "released EE call packet 0x%08x (%s, rmode=0)", d.ee_pkt_addr, d.what);
            g_queue.pop_front();
            continue;
        }
        if (!g_ee_pktbuf || !rt_gptr(g_ee_pktbuf)) {
            rt_fatal("rpc", nullptr, "response (%s) due but no EE sifcmd receive buffer was "
                "registered via INIT_CMD", d.what);
        }
        if ((rt_gread32(g_ee_pktbuf) & 0xFFu) != 0) {
            /* Previous packet not yet consumed by the EE's SIF0 handler:
             * hold this delivery (FIFO order preserved). */
            d.due = rt_clock_now() + kBusyRetry;
            break;
        }
        rt_gwrite_bytes(g_ee_pktbuf, d.pkt, d.pkt_size);
        rt_dmac_raise(RT_DMAC_SIF0);
        ++g_delivered;
        if (rt_trace()) {
            rt_log("rpc", "delivered %s packet (cid=0x%08x, %u bytes) to EE pktbuf 0x%08x, DMAC SIF0 raised",
                d.what, get32(d.pkt, 8), d.pkt_size, g_ee_pktbuf);
        }
        g_queue.pop_front();
        /* One packet per pass: the buffer is now busy until the EE handler
         * runs at the next rt_intc_deliver and zeroes psize. */
        break;
    }
}

void rt_rpc_dump_inventory() {
    rt_log("rpc", "EE pktbuf=0x%08x, %" PRIu64 " packets delivered, %zu pending",
        g_ee_pktbuf, g_delivered, g_queue.size());
    for (int i = 0; i < g_service_count; ++i) {
        const Service& s = g_services[i];
        rt_log("rpc", "  service 0x%08x %-16s %s binds=%" PRIu64 " calls=%" PRIu64,
            s.sid, s.name, s.fn ? "     " : "STUB ", s.binds, s.calls);
    }
}
