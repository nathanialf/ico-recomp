/* sif/sif_selftest.cpp: standalone exercise of the sifcmd layer of the
 * virtual IOP.
 *
 * Links sif.cpp and rpc.cpp against stub implementations of the runtime
 * services they use and drives the boot handshake exactly as SCES_507.60
 * drives it, in the order the retail ELF drives it:
 *
 *     sceSifInitRpc  -> sceSifInitCmd sends INIT_CMD opt=0 (20 bytes)
 *                    -> then INIT_CMD opt=1 (16 bytes), and spins on
 *                       sceSifGetSreg(0)
 *     sceSifRebootIop-> sceSifExitRpc clears the "rpc up" flag
 *                    -> sceSifResetIop DMAs RESET_CMD (104 bytes) and then
 *                       clears SIFINIT and CMDINIT out of SMFLAG
 *     sceSifSyncIop  -> spins on SMFLAG BOOTEND, clears it
 *     sceSifInitRpc  -> sceSifInitCmd spins on SMFLAG CMDINIT, and the
 *                       whole handshake runs again
 *
 * What it is guarding. Two facts about this boot sequence are read off
 * SCES_507.60 rather than assumed, and both used to be modeled the other
 * way round:
 *
 *  1. SMFLAG is the IOP's register, so an EE write CLEARS the bits written.
 *     The two writers in the retail ELF are sceSifResetIop (PAL 0x00264838,
 *     clearing SIFINIT and CMDINIT) and sceSifSyncIop (PAL 0x00264990,
 *     clearing BOOTEND). Modeling those as sets left the spins passing for
 *     the wrong reason and would have hidden a missing reboot response.
 *  2. The SET_SREG(0,1) reply belongs to the opt=1 INIT_CMD, not the opt=0
 *     one. sceSifInitRpc (PAL 0x0025F770) sends opt=1 and then spins at
 *     0x0025F8C8 on sceSifGetSreg(0); sceSifInitCmd (PAL 0x00265418) sends
 *     opt=0 and waits for nothing.
 *
 * Neither can be checked by running the port, because both are satisfied by
 * accident in the working order. This test pins the order-independent
 * behaviour instead.
 *
 *     ./build/rel/icorecomp-sif-selftest
 *
 * Exit code 0 = every check passed.
 */
#include "rpc.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ---- runtime stubs -------------------------------------------------------- */

extern "C" {
uint8_t* g_pages[1 << 16]; /* satisfies the extern; rt_gptr below owns RAM */
}

namespace {
uint8_t g_fake_ram[8 * 1024 * 1024];
uint64_t g_clock = 0;
int g_sif0_raised = 0;
int g_failures = 0;
}

void rt_log_line(const char* component, const char* fmt, va_list ap);

void rt_log_error(const char* c, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(c, fmt, ap); va_end(ap);
}
void rt_log_warn(const char* c, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(c, fmt, ap); va_end(ap);
}
void rt_log_info(const char* c, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(c, fmt, ap); va_end(ap);
}
void rt_log_debug(const char* c, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(c, fmt, ap); va_end(ap);
}

void rt_log_line(const char* component, const char* fmt, va_list ap) {
    std::printf("[%s] ", component);
    std::vprintf(fmt, ap);
    std::printf("\n");
}

void rt_fatal(const char* component, const R5900Context*, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("[%s] FATAL: ", component);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
    std::exit(2);
}

R5900Context* rt_sched_current_ctx() { return nullptr; }
/* What rpc.cpp's fatals pass so the dump carries the registers of whatever
 * guest code was running. There is no guest here. */
R5900Context* rt_fault_ctx() { return nullptr; }
/* The RPC call history records which guest thread made each call; this test
 * drives the layer directly, with no scheduler behind it. */
int rt_thread_current_id() { return 1; }
uint64_t rt_clock_now() { return g_clock; }
bool rt_trace() { return false; }
bool rt_verbose(const char*) { return false; }
void rt_run_note_rpc(const char*, uint32_t) {}
void rt_dmac_raise(int ch) { if (ch == 5) ++g_sif0_raised; }
void rt_override(uint32_t, void (*)(R5900Context*)) {}

/* rt_gptr is an inline over g_pages, so the fake RAM is published by
 * pointing the page table at it rather than by redefining the accessor. */
void map_fake_ram() {
    for (uint32_t page = 0; page < sizeof(g_fake_ram) >> 16; ++page) {
        g_pages[page] = &g_fake_ram[page << 16];
    }
}

uint32_t rt_gread32(uint32_t addr) {
    uint32_t v = 0;
    if (addr + 4 <= sizeof(g_fake_ram)) std::memcpy(&v, &g_fake_ram[addr], 4);
    return v;
}
void rt_gwrite32(uint32_t addr, uint32_t v) {
    if (addr + 4 <= sizeof(g_fake_ram)) std::memcpy(&g_fake_ram[addr], &v, 4);
}

/* Services the RPC layer registers at init. The sifcmd handshake this test
 * drives touches none of them, so they stay empty. */
void rt_cdvd_register_services() {}
void rt_cdvd_dump_state() {}
void rt_pad_register_services() {}
uint64_t rt_pad_next_event() { return UINT64_MAX; }
void rt_pad_run_due() {}
void rt_mc_register_service() {}
void rt_sndn2_register_service() {}
void rt_snd_pcm_note_iop_write(uint32_t, uint32_t) {}

/* ---- test driver ---------------------------------------------------------- */

namespace {

constexpr uint32_t SIF_STAT_SIFINIT = 0x10000;
constexpr uint32_t SIF_STAT_CMDINIT = 0x20000;
constexpr uint32_t SIF_STAT_BOOTEND = 0x40000;
constexpr uint32_t SIF_REG_SMFLAG = 4;
constexpr uint32_t SIF_REG_MSFLAG = 3;

/* Where the test stages the EE-side buffers in its fake RAM. */
constexpr uint32_t kEePktBuf = 0x00200000;  /* EE sifcmd receive buffer */
constexpr uint32_t kEePacket = 0x00300000;  /* the packet the EE DMAs */

void check(bool ok, const char* what) {
    std::printf("[test] %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

void put32(uint32_t addr, uint32_t v) { rt_gwrite32(addr, v); }

/* Runs the deferred timeline forward far enough for anything queued to be
 * due, the way the scheduler does between guest blocks. */
void advance() {
    for (int i = 0; i < 8; ++i) {
        g_clock += 8192;
        rt_sif_run_due();
    }
}

/* The EE's sifcmd interrupt handler zeroes the psize byte after copying a
 * packet out; the delivery path in rpc.cpp will not write a second packet
 * until it does. Returns the cid of whatever was sitting there. */
uint32_t drain_ee_packet() {
    uint32_t hdr = rt_gread32(kEePktBuf);
    if ((hdr & 0xFF) == 0) return 0;
    uint32_t cid = rt_gread32(kEePktBuf + 8);
    rt_gwrite32(kEePktBuf, 0);
    return cid;
}

/* One SifSetDma of a sifcmd packet built at kEePacket. */
void send_cmd(uint32_t cid, uint32_t opt, uint32_t size,
              const uint8_t* tail = nullptr, uint32_t tail_len = 0) {
    std::memset(&g_fake_ram[kEePacket], 0, 256);
    put32(kEePacket + 0, size);            /* psize in the low byte */
    put32(kEePacket + 4, RT_SIF_IOP_CMDBUF);
    put32(kEePacket + 8, cid);
    put32(kEePacket + 12, opt);
    if (tail && tail_len) std::memcpy(&g_fake_ram[kEePacket + 16], tail, tail_len);
    rt_rpc_on_dma_entry(kEePacket, RT_SIF_IOP_CMDBUF, size);
}

/* sceSifInitCmd's INIT_CMD: opt 0, 20 bytes, EE receive buffer at +0x10. */
void send_init_cmd_opt0() {
    uint8_t tail[4];
    std::memcpy(tail, &kEePktBuf, 4);
    send_cmd(0x80000002u, 0, 20, tail, 4);
}

/* sceSifInitRpc's INIT_CMD: opt 1, 16 bytes, no buffer field. */
void send_init_cmd_opt1() { send_cmd(0x80000002u, 1, 16); }

/* An RPC BIND packet (cid 0x80000009): +0x14 pkt_addr, +0x1C client,
 * +0x20 server id. */
void send_bind(uint32_t pkt_addr, uint32_t client, uint32_t sid) {
    uint8_t tail[24] = {0};
    std::memcpy(tail + 4, &pkt_addr, 4);   /* +0x14 */
    std::memcpy(tail + 12, &client, 4);    /* +0x1C */
    std::memcpy(tail + 16, &sid, 4);       /* +0x20 */
    send_cmd(0x80000009u, 0, 36, tail, sizeof(tail));
}

/* An RPC CALL packet (cid 0x8000000A): +0x14 pkt_addr, +0x1C client,
 * +0x20 fno, +0x24 send_size, +0x28 recvbuf, +0x2C recv_size, +0x30 rmode,
 * +0x34 server. */
void send_call(uint32_t pkt_addr, uint32_t client, uint32_t fno, uint32_t send_size,
               uint32_t recvbuf, uint32_t recv_size, uint32_t rmode, uint32_t server) {
    uint8_t tail[40] = {0};
    std::memcpy(tail + 4, &pkt_addr, 4);
    std::memcpy(tail + 12, &client, 4);
    std::memcpy(tail + 16, &fno, 4);
    std::memcpy(tail + 20, &send_size, 4);
    std::memcpy(tail + 24, &recvbuf, 4);
    std::memcpy(tail + 28, &recv_size, 4);
    std::memcpy(tail + 32, &rmode, 4);
    std::memcpy(tail + 36, &server, 4);
    send_cmd(0x8000000Au, 0, 56, tail, sizeof(tail));
}

/* A service that answers every call with a recognisable word, so a test can
 * tell "the handler ran and its bytes reached the EE" from "the packet was
 * accepted and nothing came back". */
uint32_t g_svc_calls = 0;
void test_service(uint32_t fno, const uint8_t*, uint32_t, uint8_t* recv, uint32_t recv_size) {
    ++g_svc_calls;
    if (recv_size >= 4) {
        uint32_t v = 0xC0DE0000u | (fno & 0xFFFFu);
        std::memcpy(recv, &v, 4);
    }
}

/* The same, plus the completion hold a slow IOP server asks for. g_hold is
 * what the next call to it charges; the cdvd read charges the transfer time
 * this way. */
uint64_t g_hold = 0;
void holding_service(uint32_t fno, const uint8_t* send, uint32_t send_size,
                     uint8_t* recv, uint32_t recv_size) {
    test_service(fno, send, send_size, recv, recv_size);
    if (g_hold) rt_rpc_hold_completion(g_hold);
}

/* Moves the timeline by an exact number of cycles and runs whatever that
 * makes due. The tests below need the exact figure, not advance()'s
 * generous sweep. */
void tick(uint64_t cycles) {
    g_clock += cycles;
    rt_sif_run_due();
}

/* sceSifResetIop's RESET_CMD: 104 bytes, arg_len at +0x10, mode at +0x14,
 * NUL terminated image path from +0x18. */
void send_reset_cmd(const char* image) {
    uint8_t tail[88] = {0};
    uint32_t len = (uint32_t)std::strlen(image);
    std::memcpy(tail + 0, &len, 4);        /* +0x10 arg_len */
    uint32_t mode = 0;
    std::memcpy(tail + 4, &mode, 4);       /* +0x14 mode */
    std::memcpy(tail + 8, image, len < 80 ? len : 79);  /* +0x18 path */
    send_cmd(0x80000003u, 0, 104, tail, sizeof(tail));
}

} // namespace

int main() {
    map_fake_ram();
    rt_sif_init();

    /* ---- 1. SMFLAG boot state and its write direction --------------------- */

    check(rt_sif_get_reg(SIF_REG_SMFLAG) ==
              (SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND),
          "SMFLAG boots SIFINIT|CMDINIT|BOOTEND");

    /* sceSifSyncIop clears BOOTEND once it has seen it. */
    rt_sif_set_reg(SIF_REG_SMFLAG, SIF_STAT_BOOTEND);
    check((rt_sif_get_reg(SIF_REG_SMFLAG) & SIF_STAT_BOOTEND) == 0,
          "EE write to SMFLAG clears BOOTEND, does not set it");
    check((rt_sif_get_reg(SIF_REG_SMFLAG) & SIF_STAT_CMDINIT) != 0,
          "that write left the other SMFLAG bits alone");

    /* The MMIO alias has to agree with the register call: the translated
     * libkernel may take either. */
    rt_sif_mmio_write(0x1000F230, SIF_STAT_CMDINIT);
    uint32_t smflag = 0;
    rt_sif_mmio_read(0x1000F230, &smflag);
    check((smflag & SIF_STAT_CMDINIT) == 0, "MMIO write to SMFLAG clears too");

    /* MSFLAG is the EE's own register, so a write there sets. */
    rt_sif_set_reg(SIF_REG_MSFLAG, 0x1234);
    check(rt_sif_get_reg(SIF_REG_MSFLAG) == 0x1234, "EE write to MSFLAG sets");

    /* The reboot completes at the EE's first SMFLAG read, after the guest's
     * own post-reset clears (sceSifResetIop clears SIFINIT then CMDINIT
     * right after sending the packet), so a clear between the reset and
     * the first read must not be able to erase the rebooted IOP's bits. */
    rt_sif_iop_boot_end();
    rt_sif_set_reg(SIF_REG_SMFLAG, SIF_STAT_SIFINIT);
    rt_sif_set_reg(SIF_REG_SMFLAG, SIF_STAT_CMDINIT);
    check(rt_sif_get_reg(SIF_REG_SMFLAG) ==
              (SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND),
          "rt_sif_iop_boot_end raises all three SMFLAG bits at the first read, after the guest's clears");
    rt_sif_set_reg(SIF_REG_SMFLAG, SIF_STAT_BOOTEND);
    check((rt_sif_get_reg(SIF_REG_SMFLAG) & SIF_STAT_BOOTEND) == 0,
          "a clear after the completed reboot stays cleared (sceSifSyncIop's BOOTEND clear)");

    /* ---- 2. INIT_CMD: which form gets the SET_SREG reply ------------------ */

    /* opt=0 registers the receive buffer and answers nothing. */
    send_init_cmd_opt0();
    advance();
    check(drain_ee_packet() == 0, "INIT_CMD opt=0 queues no reply");

    /* opt=1 is the one sceSifInitRpc spins on. */
    send_init_cmd_opt1();
    advance();
    check(drain_ee_packet() == 0x80000001u,
          "INIT_CMD opt=1 answers SET_SREG (cid 0x80000001)");

    /* And the payload is sreg 0 = 1, which is what releases the spin. The
     * packet was drained above, so re-send and read it before draining. */
    send_init_cmd_opt1();
    advance();
    check(rt_gread32(kEePktBuf + 16) == 0 && rt_gread32(kEePktBuf + 20) == 1,
          "SET_SREG payload is sregs[0] = 1");
    drain_ee_packet();

    /* ---- 3. RESET_CMD: the IOP reboot ------------------------------------- */

    /* Put SMFLAG where sceSifResetIop leaves it: the EE clears SIFINIT and
     * CMDINIT right after the DMA, and sceSifSyncIop then clears BOOTEND. */
    send_reset_cmd("cdrom0:\\IOPRP224.IMG;1");
    rt_sif_set_reg(SIF_REG_SMFLAG, SIF_STAT_SIFINIT);
    rt_sif_set_reg(SIF_REG_SMFLAG, SIF_STAT_CMDINIT);
    check((rt_sif_get_reg(SIF_REG_SMFLAG) & SIF_STAT_CMDINIT) == 0,
          "the EE's post-reset clears really clear CMDINIT");
    rt_sif_set_reg(SIF_REG_SMFLAG, SIF_STAT_BOOTEND);
    check(rt_sif_get_reg(SIF_REG_SMFLAG) == 0,
          "after the EE's own clears SMFLAG is empty");

    /* Nothing but the deferred reboot can put those bits back. If it does
     * not fire, sceSifInitCmd's spin on CMDINIT never ends. */
    advance();
    check((rt_sif_get_reg(SIF_REG_SMFLAG) & SIF_STAT_CMDINIT) != 0,
          "the deferred reboot re-sets CMDINIT (sceSifInitCmd's spin)");
    check((rt_sif_get_reg(SIF_REG_SMFLAG) & SIF_STAT_BOOTEND) != 0,
          "the deferred reboot re-sets BOOTEND (sceSifSyncIop's spin)");

    /* The reboot must not be delivered inside the SifSetDma that carried the
     * packet: the EE's clears run a few instructions later and would undo
     * it. Re-run the reset without advancing the clock and check the bits
     * are still down. */
    rt_sif_set_reg(SIF_REG_SMFLAG,
                   SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND);
    send_reset_cmd("cdrom0:\\IOPRP224.IMG;1");
    check(rt_sif_get_reg(SIF_REG_SMFLAG) == 0,
          "RESET_CMD does not set SMFLAG synchronously");
    advance();
    check(rt_sif_get_reg(SIF_REG_SMFLAG) ==
              (SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND),
          "it lands on the deferred timeline instead");

    /* ---- 4. the whole boot order, end to end ------------------------------ */

    /* This is file_Init's sequence (PAL 0x0010ED10 onwards). Every spin in
     * it has to be satisfiable. */
    rt_sif_init();
    g_sif0_raised = 0;
    rt_gwrite32(kEePktBuf, 0);

    send_init_cmd_opt0();
    send_init_cmd_opt1();
    advance();
    check(drain_ee_packet() == 0x80000001u, "boot: first InitRpc handshake answered");

    send_reset_cmd("cdrom0:\\IOPRP224.IMG;1");
    rt_sif_set_reg(SIF_REG_SMFLAG, SIF_STAT_SIFINIT);
    rt_sif_set_reg(SIF_REG_SMFLAG, SIF_STAT_CMDINIT);
    /* sceSifSyncIop spins until BOOTEND, then clears it. */
    advance();
    check((rt_sif_get_reg(SIF_REG_SMFLAG) & SIF_STAT_BOOTEND) != 0,
          "boot: sceSifSyncIop's BOOTEND wait can complete");
    rt_sif_set_reg(SIF_REG_SMFLAG, SIF_STAT_BOOTEND);
    /* sceSifInitCmd then spins until CMDINIT. */
    check((rt_sif_get_reg(SIF_REG_SMFLAG) & SIF_STAT_CMDINIT) != 0,
          "boot: sceSifInitCmd's CMDINIT wait can complete");

    send_init_cmd_opt0();
    send_init_cmd_opt1();
    advance();
    check(drain_ee_packet() == 0x80000001u,
          "boot: post-reset InitRpc handshake answered again");
    check(g_sif0_raised > 0, "boot: SIF0 was raised for the delivered packets");

    /* ---- 5. BIND and CALL, and the completion accounting ------------------
     *
     * The half of this layer the boot handshake does not touch, and the half
     * a stalled boot turns on: every guest wait for an IOP service ends
     * either with an RPC END packet in the EE's receive buffer or with the
     * EE-side call packet being released, and the inventory's "END
     * delivered / NOT DELIVERED" column is only worth reading if those two
     * are really wired to the two rmode cases. */
    rt_sif_init();                  /* clears the registry and the queue */
    g_sif0_raised = 0;
    g_svc_calls = 0;
    rt_gwrite32(kEePktBuf, 0);
    send_init_cmd_opt0();           /* register the EE receive buffer again */
    advance();
    rt_rpc_register_service(0x0BADF00Du, "test-service", test_service);

    constexpr uint32_t kEeRpcPkt = 0x00310000; /* the EE-side rpc packet */
    constexpr uint32_t kEeRecv = 0x00320000;   /* where the reply lands */

    send_bind(kEeRpcPkt, 0x00330000, 0x0BADF00Du);
    advance();
    check(drain_ee_packet() == 0x80000008u, "BIND is answered with an RPC END packet");

    /* The server address the BIND handed back is the one a CALL has to
     * address; it is the first service registered, so it is the first
     * minted server struct. */
    const uint32_t server = 0x00070000u;

    /* rmode = 1: the client is waiting for an END packet. */
    send_call(kEeRpcPkt, 0x00330000, 0x42, 0, kEeRecv, 4, 1, server);
    check(g_svc_calls == 1, "an RPC CALL reaches the registered handler");
    check(rt_gread32(kEeRecv) == 0xC0DE0042u,
          "the handler's receive bytes land in the EE receive buffer synchronously");
    check(rt_gread32(kEePktBuf) == 0, "the END packet is not delivered inside the CALL");
    advance();
    check(drain_ee_packet() == 0x80000008u, "rmode=1 is completed by an RPC END packet");

    /* rmode = 0: no END packet; the EE-side call packet is released instead,
     * which is what sceSifCheckStatRpc polls (rpc_id at +0x18 cleared and
     * the alloc bit at +0x10 cleared). */
    rt_gwrite32(kEeRpcPkt + 16, 1);
    rt_gwrite32(kEeRpcPkt + 24, 0x1234);
    send_call(kEeRpcPkt, 0x00330000, 0x43, 0, kEeRecv, 4, 0, server);
    advance();
    check(drain_ee_packet() == 0, "rmode=0 sends no END packet");
    check(rt_gread32(kEeRpcPkt + 24) == 0 && (rt_gread32(kEeRpcPkt + 16) & 1) == 0,
          "rmode=0 releases the EE-side call packet instead");

    /* A call whose completion is still queued must read as not delivered:
     * that is the state the inventory exists to name. */
    send_call(kEeRpcPkt, 0x00330000, 0x44, 0, kEeRecv, 4, 1, server);
    rt_gwrite32(kEePktBuf, 0x40);   /* EE has not consumed the last packet */
    advance();
    check(rt_gread32(kEePktBuf) == 0x40,
          "a delivery is held while the EE receive buffer still holds a packet");
    rt_gwrite32(kEePktBuf, 0);      /* the EE's SIF0 handler clears psize */
    advance();
    check(drain_ee_packet() == 0x80000008u, "and lands once the buffer is free again");

    /* ---- 6. the held completion and the due-order queue -------------------
     *
     * The two behaviours the 2026-09-04 PAL boot freeze was fixed with, and
     * the ones nothing else here covered. A cdvd read holds its own END for
     * the transfer time (rt_rpc_hold_completion), and the delivery order is
     * by due time rather than FIFO so that one held completion does not
     * delay another server's ready reply.
     *
     * kSifLatency in rpc.cpp is 4096 cycles; the hold is added on top of
     * it. The numbers below are picked either side of that. */
    rt_sif_init();
    g_svc_calls = 0;
    rt_gwrite32(kEePktBuf, 0);
    send_init_cmd_opt0();
    advance();
    rt_rpc_register_service(0x0BADF00Du, "held-service", holding_service);
    rt_rpc_register_service(0x0BADF00Eu, "prompt-service", test_service);
    const uint32_t held_server = 0x00070000u;   /* first registered */
    const uint32_t prompt_server = 0x00070080u; /* kServerStride later */

    constexpr uint64_t kHold = 100000;
    g_hold = kHold;
    send_call(kEeRpcPkt, 0x00330000, 0x50, 0, kEeRecv, 4, 1, held_server);
    tick(8192); /* well past the 4096-cycle SIF latency, far short of the hold */
    check(rt_gread32(kEePktBuf) == 0,
          "a held completion is not delivered at the SIF latency");
    tick(kHold);
    check(drain_ee_packet() == 0x80000008u,
          "and it is delivered once the hold has elapsed");

    /* Due order, not queue order: the held call is queued first, the
     * unheld one second, and the unheld one has to arrive first. The END
     * packet echoes the server address at +0x24, which is how each reply
     * says which service it came from. */
    g_hold = kHold;
    send_call(kEeRpcPkt, 0x00330000, 0x51, 0, kEeRecv, 4, 1, held_server);
    g_hold = 0;
    send_call(kEeRpcPkt, 0x00330000, 0x52, 0, kEeRecv, 4, 1, prompt_server);
    tick(8192);
    check(rt_gread32(kEePktBuf + 36) == prompt_server,
          "an unheld reply overtakes a held completion queued before it");
    drain_ee_packet();
    tick(kHold);
    check(rt_gread32(kEePktBuf + 36) == held_server,
          "and the held completion follows when its own time comes");
    drain_ee_packet();

    /* Prints the call history, the delivered-packet history and the
     * deferred queue. Exercised here so the formatting cannot rot
     * unnoticed between the runs that need it. */
    rt_rpc_dump_inventory();

    std::printf("[test] %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
