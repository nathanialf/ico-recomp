/* sif/rpc.cpp: the SIF RPC layer of the virtual IOP.
 *
 * The EE side runs the game's own (translated) Sony sifrpc client code; this
 * module plays the IOP end of the wire protocol:
 *
 *   - Every SifSetDma entry is copied into a 2 MB virtual IOP RAM. Entries
 *     addressed to RT_SIF_IOP_CMDBUF are sifcmd packets and get parsed.
 *   - INIT_CMD (0x80000002, opt 0, 20 bytes): records the EE's sifcmd
 *     receive buffer. The client waits on nothing, so nothing is queued.
 *   - INIT_CMD (0x80000002, opt 1, 16 bytes): the sceSifInitRpc handshake.
 *     Queues the SET_SREG(0,1) "RPC layer up" response, which is what the
 *     client's spin on sceSifGetSreg(0) is waiting for.
 *   - RESET_CMD (0x80000003, 104 bytes): the IOP reboot with the IOPRP
 *     image path. Queues a deferred "boot end" that puts SIFINIT, CMDINIT
 *     and BOOTEND back into SMFLAG, which is what the client's sceSifSyncIop
 *     and sceSifInitCmd spins are waiting for.
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
 * Deliveries go out in due order, not queue order: a handler may hold its
 * own call's completion back for the time the IOP server's work would
 * take (rt_rpc_hold_completion; the cdvd read uses it for the transfer
 * time), and that must not delay the replies of every other server.
 *
 * Packet layouts per ps2sdk common/include/sifrpc-common.h and
 * ee/kernel/src/sifrpc.c (clean-room structural reference). All byte
 * offsets below are from the start of the packet:
 *   sifcmd header: +0 psize:8|dsize:24, +4 dest, +8 cid, +12 opt
 *   INIT_CMD:  +16 EE sifcmd receive buffer (opt 0 form only)
 *   RESET_CMD: +16 arg_len +20 mode +24.. NUL terminated image path
 *   BIND:  +16 rec_id +20 pkt_addr +24 rpc_id +28 client +32 sid
 *   CALL:  +16 rec_id +20 pkt_addr +24 rpc_id +28 client +32 fno
 *          +36 send_size +40 recvbuf +44 recv_size +48 rmode +52 server
 *   RDATA: +16 rec_id +20 pkt_addr +24 rpc_id +28 recv_data +32 src
 *          +36 dest +40 size
 *   END:   +16 rec_id +20 pkt_addr +24 rpc_id +28 client +32 request cid
 *          +36 server +40 buff +44 cbuff
 */
#include "rpc.h"

#include "../snd/snd.h" /* rt_snd_pcm_note_iop_write: the PCM ring write witness */

#include <cinttypes>
#include <cstring>
#include <deque>
#include <vector>

namespace {

constexpr uint32_t SIF_CMD_SET_SREG = 0x80000001u;
constexpr uint32_t SIF_CMD_INIT_CMD = 0x80000002u;
constexpr uint32_t SIF_CMD_RESET_CMD = 0x80000003u;
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
    enum class Kind { CmdPacket, FreeEePkt, IopBootEnd } kind;
    uint64_t due = 0;
    uint32_t pkt_size = 0;
    uint8_t pkt[kRpcPacketSize] = {0};
    uint32_t ee_pkt_addr = 0;   /* FreeEePkt: EE-side rpc packet to release */
    const char* what = "";
    /* Sequence number of the RPC CALL this delivery completes, or -1 for a
     * delivery that completes no call (SET_SREG, BIND, RDATA, the reboot).
     * Lets the call history below say whether the completion the client is
     * waiting for actually reached the EE, which is the difference between
     * "the runtime never answered" and "the runtime answered and the guest
     * did not act on it". */
    int64_t call_seq = -1;
};

std::deque<Delivery> g_queue;
uint64_t g_delivered = 0;

/* rt_rpc_hold_completion: the extra cycles the handler of the CALL now in
 * flight asked for. Read and cleared by handle_call when it queues that
 * call's completion. g_in_call is what makes "the current call only" a
 * checkable statement rather than a convention: a hold set from anywhere
 * else is refused. */
uint64_t g_completion_hold = 0;
bool g_in_call = false;

/* The delivery that goes next: the earliest due, first among equals, so a
 * held completion (a cdvd read still transferring) never sits in front of
 * a reply another server already has ready. Returns g_queue.end() when the
 * queue is empty. */
std::deque<Delivery>::iterator next_delivery() {
    auto best = g_queue.end();
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
        if (best == g_queue.end() || it->due < best->due) best = it;
    }
    return best;
}

/* ---- history rings -------------------------------------------------------
 *
 * The log is a stream and a stalled run is a still picture, so the inventory
 * needs the last handful of each of the two things that cross this layer,
 * in one block, whatever the log level was when they happened. Both are
 * plain arrays indexed by a monotonic count: no allocation, no ordering to
 * get wrong, and the oldest entry falls out on its own. */
constexpr size_t kCallRing = 16;

struct RpcCall {
    uint64_t vclk = 0;
    const char* server = "";
    uint32_t fno = 0, send_size = 0, recv_size = 0, recvbuf = 0, rmode = 0, client = 0;
    int thread = 0;
    bool end_done = false;      /* its END packet (or packet release) reached the EE */
    uint64_t end_vclk = 0;
};

RpcCall g_calls[kCallRing];
uint64_t g_call_count = 0;

/* Marks the call a delivery belonged to, if it is still in the ring. */
void call_note_end(int64_t seq) {
    if (seq < 0) return;
    if ((uint64_t)seq + kCallRing < g_call_count) return; /* aged out */
    RpcCall& c = g_calls[(uint64_t)seq % kCallRing];
    c.end_done = true;
    c.end_vclk = rt_clock_now();
}

constexpr size_t kDeliveredRing = 16;

struct SifPkt {
    uint64_t vclk = 0;
    uint32_t cid = 0, size = 0;
    const char* what = "";
};

SifPkt g_delivered_ring[kDeliveredRing];

const char* cid_name(uint32_t cid) {
    switch (cid) {
        case SIF_CMD_SET_SREG: return "SET_SREG";
        case SIF_CMD_INIT_CMD: return "INIT_CMD";
        case SIF_CMD_RESET_CMD: return "RESET_CMD";
        case SIF_CMD_RPC_END: return "RPC_END";
        case SIF_CMD_RPC_BIND: return "RPC_BIND";
        case SIF_CMD_RPC_CALL: return "RPC_CALL";
        case SIF_CMD_RPC_RDATA: return "RPC_RDATA";
        default: return "?";
    }
}

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

void queue_packet(const uint8_t* pkt, uint32_t size, const char* what, int64_t call_seq = -1,
                  uint64_t hold = 0) {
    Delivery d;
    d.kind = Delivery::Kind::CmdPacket;
    d.due = rt_clock_now() + kSifLatency + hold;
    /* Every packet this layer builds is kRpcPacketSize or smaller, and a
     * truncated one would reach the EE's sifcmd handler missing its tail.
     * Fatal rather than quietly short, so a future caller with a bigger
     * packet has to grow Delivery::pkt instead of losing bytes. */
    if (size > kRpcPacketSize) {
        rt_fatal("rpc", rt_sched_current_ctx(),
            "sifcmd packet for %s is %u bytes; the delivery buffer holds %u",
            what ? what : "(unnamed)", size, (unsigned)kRpcPacketSize);
    }
    d.pkt_size = size;
    std::memcpy(d.pkt, pkt, d.pkt_size);
    d.what = what;
    d.call_seq = call_seq;
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

/* SIF_CMD_INIT_CMD (0x80000002) arrives in two distinct forms, and which
 * one gets the SET_SREG reply is not a guess: it is read off SCES_507.60.
 *
 *   opt = 0, 20 bytes, EE receive buffer at +0x10.
 *       Sent by sceSifInitCmd (PAL 0x00265418, a function entry in the
 *       retail ELF; packet build at 0x0026564C..0x00265690:
 *       opt written at packet+0xC, buffer at packet+0x10, sceSifSendCmd
 *       length 0x14). It hands the IOP the EE's sifcmd receive buffer.
 *       sceSifInitCmd does not wait for any reply to it; the only thing it
 *       waits on is SMFLAG's CMDINIT bit, before it sends.
 *
 *   opt = 1, 16 bytes, no buffer field.
 *       Sent by sceSifInitRpc (PAL 0x0025F770, same file, packet build at
 *       0x0025F8A0: opt written at packet+0xC from a register holding a
 *       constant 1, sceSifSendCmd length 0x10). This is the one the EE
 *       waits on: immediately after sending it, sceSifInitRpc spins at
 *       0x0025F8C8 on sceSifGetSreg(0) until that software register reads
 *       nonzero. The IOP's sifrpc answers with SET_SREG(0, 1), which is
 *       what releases the spin.
 *
 * The runtime used to answer the opt=0 form and log the opt=1 form as "no
 * response modeled". The boot survived only because the opt=0 packet always
 * arrives first (sceSifInitRpc calls sceSifInitCmd before sending its own),
 * so the reply to the wrong packet happened to land before the spin. That
 * is not the protocol, and it stops being true the moment anything reorders
 * the two, so the reply now goes to the packet the client actually waits on.
 * sceSifRebootIop clears the "rpc initialised" flag through sceSifExitRpc
 * (PAL 0x0025F910), so the whole handshake runs a second time after the IOP
 * reset and the spin is entered twice per boot. */
void handle_init_cmd(const uint8_t* pkt, uint32_t size) {
    uint32_t opt = get32(pkt, 12);
    if (opt == 0) {
        if (size < 20) {
            rt_log_warn("rpc", "WARNING sifcmd INIT_CMD opt=0 is %u bytes, expected at least 20 "
                "(sceSifInitCmd sends 0x14): no EE receive buffer field to read, "
                "keeping pktbuf=0x%08x. Every deferred sifcmd response needs it",
                size, g_ee_pktbuf);
            return;
        }
        g_ee_pktbuf = get32(pkt, 16) & 0x1FFFFFFFu;
        rt_log_info("rpc", "sifcmd INIT_CMD opt=0 (sceSifInitCmd, size=%u): EE sifcmd receive "
            "buffer = 0x%08x. No reply is modeled because the client waits on none",
            size, g_ee_pktbuf);
        return;
    }
    if (opt == 1) {
        /* SET_SREG(sreg 0 = RPCINIT, value 1), the reply sceSifInitRpc's
         * spin at PAL 0x0025F8C8 is waiting for. Packet shape per ps2sdk
         * ee/kernel/src/sifcmd.c (public wire format). */
        uint8_t resp[24] = {0};
        put32(resp, 0, 24);                 /* psize=24, dsize=0 */
        put32(resp, 8, SIF_CMD_SET_SREG);
        put32(resp, 16, 0);                 /* sreg index 0 */
        put32(resp, 20, 1);                 /* value 1 */
        queue_packet(resp, sizeof(resp), "SET_SREG(0,1)");
        rt_log_info("rpc", "sifcmd INIT_CMD opt=1 (sceSifInitRpc, size=%u): SET_SREG(0,1) queued, "
            "which releases the client's spin on sceSifGetSreg(0)", size);
        if (!g_ee_pktbuf) {
            rt_log_warn("rpc", "WARNING INIT_CMD opt=1 arrived before any opt=0 registered an EE "
                "sifcmd receive buffer: the SET_SREG(0,1) reply has nowhere to land and "
                "sceSifInitRpc will spin on sceSifGetSreg(0) forever");
        }
        return;
    }
    rt_log_warn("rpc", "WARNING sifcmd INIT_CMD with opt=%u (size=%u) NOT MODELED: no reply sent. "
        "SCES_507.60 only ever sends opt=0 (sceSifInitCmd, 20 bytes) and opt=1 "
        "(sceSifInitRpc, 16 bytes); if this opt carries a wait, it will hang", opt, size);
}

/* SIF_CMD_RESET_CMD (0x80000003), 104 bytes: the IOP reboot.
 *
 * Packet layout read off sceSifResetIop (PAL 0x00264838, a function entry
 * in the retail ELF): psize byte = 0x68 at +0x00, dest = 0 at +0x04,
 * cid at +0x08, argument length at +0x10, mode at +0x14, and the NUL
 * terminated argument string from +0x18 (the caller's string is rejected
 * above 0x50 bytes, which is what fixes the packet at 104).
 *
 * The only caller in this binary is sceSifRebootIop (PAL 0x002649D8), and
 * the only call site is the boot path in file_Init (the jal at PAL
 * 0x0010ED58; the disc's own link map, MAIN.MAP, names file_Init as
 * FileManager.o's entry), which passes the IOPRP image path. After the DMA the EE
 * clears SIFINIT and CMDINIT out of SMFLAG, then spins in sceSifSyncIop
 * until BOOTEND appears and in sceSifInitCmd until CMDINIT appears. So the
 * reboot has to be answered by re-setting those bits, and it has to be
 * answered late: doing it inside this call would be undone by the EE's own
 * clears a few instructions later. It goes on the deferred timeline like
 * every other response. */
void handle_reset_cmd(const uint8_t* pkt, uint32_t size) {
    uint32_t arg_len = size >= 20 ? get32(pkt, 16) : 0;
    uint32_t mode = size >= 24 ? get32(pkt, 20) : 0;
    char arg[81] = {0};
    if (size > 24) {
        uint32_t n = size - 24;
        if (n > 80) n = 80;
        std::memcpy(arg, pkt + 24, n);
    }
    rt_log_info("rpc", "sifcmd RESET_CMD (IOP reboot, size=%u): arg_len=%u mode=%u image=\"%s\". "
        "The PAL disc ships this image as IOPRP224.IMG (201065 bytes), byte for byte the same "
        "image the US disc ships, so the rebooted IOP kernel, cdvdman, cdvdfsv and sifcmd are "
        "the same on both", size, arg_len, mode, arg);
    Delivery d;
    d.kind = Delivery::Kind::IopBootEnd;
    d.due = rt_clock_now() + kSifLatency;
    d.what = "IOP reboot";
    g_queue.push_back(d);
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
    rt_log_info("rpc", "BIND sid=0x%08x (%s) client=0x%08x -> server=0x%06x buff=0x%06x",
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
    /* The last RPC the end-of-run summary reports. s->name is a registry
     * entry with static lifetime, and this is recorded before the handler
     * runs so a call that ends the run names itself. */
    rt_run_note_rpc(s->name, fno);

    /* The call history the inventory prints, recorded before the handler
     * runs so a call that ends the run is in it. */
    const int64_t seq = (int64_t)g_call_count;
    {
        RpcCall& c = g_calls[g_call_count % kCallRing];
        c = RpcCall{};
        c.vclk = rt_clock_now();
        c.server = s->name;
        c.fno = fno;
        c.send_size = send_size;
        c.recv_size = recv_size;
        c.recvbuf = recvbuf;
        c.rmode = rmode;
        c.client = client;
        c.thread = rt_thread_current_id();
        ++g_call_count;
    }
    /* info, not debug. This line and the delivery line below are the two
     * halves of every IOP request the guest makes, and a run that stops
     * waiting for one of them cannot be read without both: the run that
     * made this change ended with 20 s of idle fields and a log in which
     * the last RPC could only be named by the end-of-run summary. The cost
     * is a few lines a field. */
    rt_log_info("rpc", "CALL #%" PRIu64 " %s fno=0x%x send=%u recv=0x%08x+%u rmode=%u "
        "client=0x%08x thread=%d",
        (uint64_t)seq, s->name, fno, send_size, recvbuf, recv_size, rmode, client,
        rt_thread_current_id());

    /* Over-allocate so a handler writing a fixed-size result struct cannot
     * overrun a caller's smaller recv declaration; only recv_size bytes are
     * copied back to the EE. */
    std::vector<uint8_t> recv(recv_size < 64 ? 64 : recv_size, 0);
    const uint8_t* send = send_size ? rt_iop_ptr(s->buf_iop) : nullptr;
    /* rpc.h's contract for rt_rpc_hold_completion is "the current call
     * only". A hold left over from anything but this handler would be
     * charged to the wrong call, so the entry state is checked rather than
     * assumed. */
    if (g_completion_hold != 0) {
        rt_log_warn("rpc", "CALL #%" PRIu64 " %s fno=0x%x entered with a completion hold of "
            "%" PRIu64 " cycles already set. That hold was not this call's; it is discarded",
            (uint64_t)seq, s->name, fno, g_completion_hold);
        g_completion_hold = 0;
    }
    g_in_call = true;
    if (s->fn) {
        s->fn(fno, send, send_size, recv.data(), recv_size);
    } else {
        static uint64_t stub_calls = 0;
        ++stub_calls;
        rt_log_debug("rpc", "STUB CALL %s fno=0x%x send_size=%u recv_size=%u: returning zeroed recv "
            "[stub call #%" PRIu64 "]", s->name, fno, send_size, recv_size, stub_calls);
    }

    g_in_call = false;

    /* Receive data lands synchronously (DMA is synchronous, CLAUDE.md); the
     * completion below is deferred. */
    if (recv_size && recvbuf) rt_gwrite_bytes(recvbuf, recv.data(), recv_size);

    /* How long the IOP server would have stayed inside its handler before
     * answering (rt_rpc_hold_completion, set by the handler above; 0 when
     * it set none). Logged whenever it is nonzero: a completion the guest
     * has to wait for is the kind of fact a stalled run needs stated. */
    const uint64_t hold = g_completion_hold;
    g_completion_hold = 0;
    if (hold) {
        rt_log_info("rpc", "CALL #%" PRIu64 " %s fno=0x%x: completion held %.3f ms past the SIF "
            "latency (the server's work takes that long); the client waits until then",
            (uint64_t)seq, s->name, fno, (double)hold * 1000.0 / (double)RT_BUSCLK_HZ);
    }
    if (rmode) {
        uint8_t end[kRpcPacketSize];
        build_end_packet(end, pkt, SIF_CMD_RPC_CALL);
        put32(end, 36, server);
        queue_packet(end, kRpcPacketSize, s->name, seq, hold);
    } else {
        /* NOWAIT call without an end function: the IOP sends no END packet;
         * it releases the EE-side call packet so sceSifCheckStatRpc sees the
         * call complete (ps2sdk sceSifExecRequest, rmode==0 path). */
        Delivery d;
        d.kind = Delivery::Kind::FreeEePkt;
        d.due = rt_clock_now() + kSifLatency + hold;
        d.ee_pkt_addr = get32(pkt, 20) & 0x1FFFFFFFu;
        d.what = s->name;
        d.call_seq = seq;
        g_queue.push_back(d);
    }
}

void handle_rdata(const uint8_t* pkt) {
    uint32_t src = get32(pkt, 32);
    uint32_t dest = get32(pkt, 36) & 0x1FFFFFFFu;
    uint32_t size = get32(pkt, 40);
    rt_log_info("rpc", "RDATA (SifGetOtherData): IOP 0x%06x -> EE 0x%08x, %u bytes", src, dest, size);
    if (size > RT_IOP_RAM_SIZE) {
        rt_fatal("rpc", rt_sched_current_ctx(), "RDATA size %u is implausible", size);
    }
    /* One bounded copy, not a page lookup per byte. rt_iop_ptr masks the
     * address into the ring, so a read that would run off the end of IOP
     * RAM is split at the wrap rather than reading past it. */
    {
        const uint32_t base = src & (RT_IOP_RAM_SIZE - 1);
        const uint32_t first = RT_IOP_RAM_SIZE - base;
        const uint32_t n = size < first ? size : first;
        rt_gwrite_bytes(dest, rt_iop_ptr(src), n);
        if (n < size) rt_gwrite_bytes(dest + n, rt_iop_ptr(0), size - n);
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
    g_call_count = 0;
    for (RpcCall& c : g_calls) c = RpcCall{};
    for (SifPkt& r : g_delivered_ring) r = SifPkt{};
    rt_cdvd_register_services();
    rt_log_info("rpc", "RPC layer up: %d services registered", g_service_count);
}

void rt_rpc_register_service(uint32_t sid, const char* name, RtRpcServiceFn fn) {
    if (g_service_count >= kMaxServices) {
        rt_fatal("rpc", rt_fault_ctx(), "service registry full registering 0x%08x (%s)", sid, name);
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

/* A packet shorter than its own layout, or a transfer too big to be one.
 * Every other unexpected shape in this file is loud, and these used to be
 * the exception: the EE is then waiting for a reply that will never come,
 * with nothing in the log to say which packet was dropped. Folded by
 * powers of two on one counter, because a malformed sender repeats. */
static void report_short_packet(const char* what, uint32_t cid, uint32_t size, uint32_t minimum) {
    static uint64_t dropped = 0;
    ++dropped;
    if ((dropped & (dropped - 1)) == 0) {
        rt_log_warn("rpc", "WARNING sifcmd %s (cid=0x%08x) DROPPED: size %u, this packet's layout "
            "needs %u. No reply is sent, so whatever the EE is waiting on will not arrive "
            "[#%" PRIu64 "]",
            what, cid, size, minimum, dropped);
    }
}

void rt_rpc_on_dma_entry(uint32_t src_ee, uint32_t dest_iop, uint32_t size) {
    if (size == 0 || size > RT_IOP_RAM_SIZE) {
        if (size) {
            static uint64_t oversize = 0;
            ++oversize;
            if ((oversize & (oversize - 1)) == 0) {
                rt_log_warn("rpc", "WARNING EE to IOP transfer of %u bytes to 0x%08x DROPPED: "
                    "larger than the IOP's %u bytes of RAM, so it cannot be a real transfer and "
                    "nothing is staged [#%" PRIu64 "]",
                    size, dest_iop, (unsigned)RT_IOP_RAM_SIZE, oversize);
            }
        }
        return;
    }
    /* Stage the payload into virtual IOP RAM (send data for a later CALL,
     * or the command packet itself). */
    uint8_t* dst = rt_iop_ptr(dest_iop);
    uint32_t room = RT_IOP_RAM_SIZE - (dest_iop & (RT_IOP_RAM_SIZE - 1));
    uint32_t n = size < room ? size : room;
    rt_gread_bytes(src_ee, dst, n);
    /* The attract movie refills its SgStPcm ring with raw SifSetDma entries
     * (ito/mpeg sendToIOP2area, PAL 0x00257828), so this is the one place
     * the runtime
     * sees the EE's writes to that ring. The engine needs them to tell a block
     * the EE never refreshed from one it refreshed with the same bytes; a
     * transfer anywhere else costs one compare inside the callee. */
    rt_snd_pcm_note_iop_write(dest_iop & (RT_IOP_RAM_SIZE - 1), n);

    if ((dest_iop & 0x1FFFFFFFu) != RT_SIF_IOP_CMDBUF || size < 16) return;

    const uint8_t* pkt = rt_iop_ptr(RT_SIF_IOP_CMDBUF);
    uint32_t cid = get32(pkt, 8);
    switch (cid) {
        case SIF_CMD_INIT_CMD:
            handle_init_cmd(pkt, size);
            break;
        case SIF_CMD_RESET_CMD:
            handle_reset_cmd(pkt, size);
            break;
        case SIF_CMD_RPC_BIND:
            if (size >= 36) handle_bind(pkt);
            else report_short_packet("BIND", cid, size, 36);
            break;
        case SIF_CMD_RPC_CALL:
            if (size >= 56) handle_call(pkt);
            else report_short_packet("CALL", cid, size, 56);
            break;
        case SIF_CMD_RPC_RDATA:
            if (size >= 44) handle_rdata(pkt);
            else report_short_packet("RDATA", cid, size, 44);
            break;
        default:
            rt_log_warn("rpc", "WARNING sifcmd packet cid=0x%08x size=%u to the IOP NOT MODELED: "
                "ignored, no reply sent. SCES_507.60 sends only INIT_CMD (0x80000002), "
                "RESET_CMD (0x80000003) and the RPC BIND/CALL/RDATA ids; if this cid carries "
                "a wait, the EE will spin on it", cid, size);
            break;
    }
}

void rt_rpc_hold_completion(uint64_t cycles) {
    /* Additive on purpose (rpc.h says so): a handler that charges two
     * pieces of work in one call means the client waits for both. Outside a
     * call there is nothing to charge it to, and the next unrelated call
     * would inherit it, so refuse and say why. */
    if (!g_in_call) {
        static uint64_t stray = 0;
        ++stray;
        if ((stray & (stray - 1)) == 0) {
            rt_log_warn("rpc", "rt_rpc_hold_completion(%" PRIu64 ") called outside an RPC CALL "
                "handler; ignored rather than charged to the next call [#%" PRIu64 "]",
                cycles, stray);
        }
        return;
    }
    g_completion_hold += cycles;
}

uint64_t rt_rpc_next_event() {
    auto it = next_delivery();
    return it == g_queue.end() ? UINT64_MAX : it->due;
}

void rt_rpc_run_due() {
    for (;;) {
        auto it = next_delivery();
        if (it == g_queue.end()) break;
        Delivery& d = *it;
        if (d.due > rt_clock_now()) break;
        if (d.kind == Delivery::Kind::IopBootEnd) {
            rt_sif_iop_boot_end();
            g_queue.erase(it);
            continue;
        }
        if (d.kind == Delivery::Kind::FreeEePkt) {
            /* Release the EE-side rpc packet: rpc_id=0, alloc bit clear
             * (ps2sdk rpc_packet_free; sceSifCheckStatRpc then reports the
             * call finished). */
            if (rt_gptr(d.ee_pkt_addr)) {
                rt_gwrite32(d.ee_pkt_addr + 24, 0);
                rt_gwrite32(d.ee_pkt_addr + 16, rt_gread32(d.ee_pkt_addr + 16) & ~1u);
            } else {
                rt_log_warn("rpc", "FreeEePkt for %s dropped: EE packet 0x%08x unmapped", d.what, d.ee_pkt_addr);
            }
            rt_log_info("rpc", "released EE call packet 0x%08x (%s, rmode=0): "
                "sceSifCheckStatRpc now reports the call finished", d.ee_pkt_addr, d.what);
            call_note_end(d.call_seq);
            g_queue.erase(it);
            continue;
        }
        if (!g_ee_pktbuf || !rt_gptr(g_ee_pktbuf)) {
            rt_fatal("rpc", rt_fault_ctx(), "response (%s) due but no EE sifcmd receive buffer was "
                "registered via INIT_CMD", d.what);
        }
        if ((rt_gread32(g_ee_pktbuf) & 0xFFu) != 0) {
            /* Previous packet not yet consumed by the EE's SIF0 handler:
             * hold this delivery. Every other packet due by then is behind
             * the same buffer, so one retry time covers them all and the
             * due order among them is unchanged. */
            d.due = rt_clock_now() + kBusyRetry;
            break;
        }
        rt_gwrite_bytes(g_ee_pktbuf, d.pkt, d.pkt_size);
        rt_dmac_raise(RT_DMAC_SIF0);
        {
            const uint32_t cid = get32(d.pkt, 8);
            SifPkt& r = g_delivered_ring[g_delivered % kDeliveredRing];
            r.vclk = rt_clock_now();
            r.cid = cid;
            r.size = d.pkt_size;
            r.what = d.what;
            ++g_delivered;
            /* info for the same reason as the CALL line above: a queued
             * response and a delivered one are different states, and only
             * the second one can release the guest. */
            rt_log_info("rpc", "delivered %s %s packet (cid=0x%08x, %u bytes) to EE pktbuf 0x%08x, "
                "DMAC SIF0 raised", d.what, cid_name(cid), cid, d.pkt_size, g_ee_pktbuf);
        }
        call_note_end(d.call_seq);
        g_queue.erase(it);
        /* One packet per pass: the buffer is now busy until the EE handler
         * runs at the next rt_intc_deliver and zeroes psize. */
        break;
    }
}

void rt_rpc_dump_inventory() {
    const uint64_t now = rt_clock_now();
    rt_log_info("rpc", "EE pktbuf=0x%08x, %" PRIu64 " packets delivered, %zu pending, %" PRIu64 " calls made",
        g_ee_pktbuf, g_delivered, g_queue.size(), g_call_count);
    for (int i = 0; i < g_service_count; ++i) {
        const Service& s = g_services[i];
        rt_log_info("rpc", "  service 0x%08x %-16s %s binds=%" PRIu64 " calls=%" PRIu64,
            s.sid, s.name, s.fn ? "     " : "STUB ", s.binds, s.calls);
    }

    /* The last calls, newest last, each with whether its completion reached
     * the EE. A call with end=NO and a client still blocked is the shape of
     * a wait this layer owns; a call with end=yes and a client still
     * blocked is a wait it does not. */
    const uint64_t first = g_call_count > kCallRing ? g_call_count - kCallRing : 0;
    rt_log_info("rpc", "  last %" PRIu64 " RPC calls (oldest first):", g_call_count - first);
    for (uint64_t i = first; i < g_call_count; ++i) {
        const RpcCall& c = g_calls[i % kCallRing];
        rt_log_info("rpc", "    #%-4" PRIu64 " %-16s fno=0x%-4x rmode=%u send=%-6u recv=0x%08x+%-6u "
            "thread=%-3d at %.3f s, END %s",
            i, c.server, c.fno, c.rmode, c.send_size, c.recvbuf, c.recv_size, c.thread,
            (double)c.vclk / (double)RT_BUSCLK_HZ,
            c.end_done ? "delivered" : "NOT DELIVERED");
    }

    /* The last sifcmd packets that actually reached the EE receive buffer,
     * which is the only event that can raise SIF0 and run the guest's
     * handler. */
    const uint64_t dfirst = g_delivered > kDeliveredRing ? g_delivered - kDeliveredRing : 0;
    rt_log_info("rpc", "  last %" PRIu64 " sifcmd packets delivered to the EE (oldest first):",
        g_delivered - dfirst);
    for (uint64_t i = dfirst; i < g_delivered; ++i) {
        const SifPkt& r = g_delivered_ring[i % kDeliveredRing];
        rt_log_info("rpc", "    #%-4" PRIu64 " %-9s cid=0x%08x %-3u bytes  %-16s at %.3f s",
            i, cid_name(r.cid), r.cid, r.size, r.what, (double)r.vclk / (double)RT_BUSCLK_HZ);
    }

    /* Anything still on the deferred timeline. A response sitting here
     * because the EE never zeroed the receive buffer's psize byte is a
     * different failure from one that was never queued at all. */
    if (g_queue.empty()) {
        rt_log_info("rpc", "  deferred queue: empty");
    } else {
        rt_log_info("rpc", "  deferred queue: %zu waiting (EE pktbuf psize byte = %u; a nonzero one "
            "holds every delivery until the guest's SIF0 handler clears it)",
            g_queue.size(),
            g_ee_pktbuf && rt_gptr(g_ee_pktbuf) ? (rt_gread32(g_ee_pktbuf) & 0xFFu) : 0u);
        size_t n = 0;
        for (const Delivery& d : g_queue) {
            const char* kind = d.kind == Delivery::Kind::CmdPacket ? "packet"
                             : d.kind == Delivery::Kind::FreeEePkt ? "free EE pkt" : "IOP boot end";
            rt_log_info("rpc", "    %-12s %-16s cid=%-9s due in %.3f ms%s",
                kind, d.what,
                d.kind == Delivery::Kind::CmdPacket ? cid_name(get32(d.pkt, 8)) : "-",
                (double)((int64_t)d.due - (int64_t)now) * 1000.0 / (double)RT_BUSCLK_HZ,
                d.call_seq >= 0 ? " (completes an RPC call)" : "");
            if (++n >= 8) {
                rt_log_info("rpc", "    ... %zu more", g_queue.size() - n);
                break;
            }
        }
    }
}
