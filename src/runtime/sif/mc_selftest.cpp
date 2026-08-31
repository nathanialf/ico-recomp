/* sif/mc_selftest.cpp: standalone exercise of the mcserv RPC service.
 *
 * Links mc.cpp against stub implementations of the runtime services it
 * uses (logging, guest RAM, the RPC registry) and drives the wire protocol
 * exactly as the retail libmc does: Init handshake, GetInfo, Mkdir, ChDir,
 * Open/Write/Close, Open/Read/Seek, GetDir, SetFileInfo, Rename, Delete,
 * GetEntSpace. Run with ICORECOMP_SAVES_DIR pointing at a scratch
 * directory:
 *
 *     ICORECOMP_SAVES_DIR=/tmp/mc-selftest ./build/rel/icorecomp-mc-selftest
 *
 * Exit code 0 = every check passed. This is the pad/mc verification story
 * for the save path until the game itself reaches a save point.
 */
#include "rpc.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ---- runtime stubs -------------------------------------------------------- */

extern "C" {
uint8_t* g_pages[1 << 16]; /* unused by mc.cpp; satisfies the extern */
}

static uint8_t g_fake_ram[4 * 1024 * 1024];
static RtRpcServiceFn g_mc_fn = nullptr;

void rt_log(const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("[%s] ", component);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
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

void rt_gread_bytes(uint32_t addr, void* dst, uint32_t n) {
    std::memcpy(dst, &g_fake_ram[addr % sizeof(g_fake_ram)], n);
}

void rt_gwrite_bytes(uint32_t addr, const void* src, uint32_t n) {
    std::memcpy(&g_fake_ram[addr % sizeof(g_fake_ram)], src, n);
}

void rt_rpc_register_service(uint32_t sid, const char* name, RtRpcServiceFn fn) {
    std::printf("[test] registered 0x%08x %s\n", sid, name);
    if (sid == 0x80000400) g_mc_fn = fn;
}

/* ---- test driver ---------------------------------------------------------- */

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("[test] %-40s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

void put32(uint8_t* p, uint32_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }
int32_t res32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }

/* One RPC round trip: a zeroed 0x414 send block the caller fills. */
struct Call {
    uint8_t send[0x414] = {0};
    uint8_t recv[64] = {0};
    int32_t run(uint32_t fno, uint32_t send_size = 0x414, uint32_t recv_size = 4) {
        g_mc_fn(fno, send, send_size, recv, recv_size);
        return res32(recv);
    }
    void name(const char* n) { std::snprintf((char*)send + 0x14, 0x3FF, "%s", n); }
};

} // namespace

int main() {
    if (!std::getenv("ICORECOMP_SAVES_DIR")) {
        std::printf("[test] set ICORECOMP_SAVES_DIR to a scratch directory first\n");
        return 2;
    }
    rt_mc_register_service();
    if (!g_mc_fn) { std::printf("[test] no mcserv handler registered\n"); return 2; }

    { /* Init: version handshake */
        Call c;
        c.run(0xFE, 0x30, 12);
        uint32_t v1, v2;
        std::memcpy(&v1, c.recv + 4, 4);
        std::memcpy(&v2, c.recv + 8, 4);
        check(res32(c.recv) == 0 && v1 >= 0x20A && v2 >= 0x20E, "Init versions");
    }
    { /* GetInfo: changed-card first, then steady */
        Call c;
        put32(c.send, 4, 0);            /* port */
        put32(c.send, 0x1C, 0x1000);    /* writeback */
        int32_t first = c.run(1, 0x30);
        int32_t second = c.run(1, 0x30);
        uint32_t type, free_kb, fmt;
        std::memcpy(&type, &g_fake_ram[0x1000], 4);
        std::memcpy(&free_kb, &g_fake_ram[0x1004], 4);
        std::memcpy(&fmt, &g_fake_ram[0x1090], 4);
        check(first == -1 && second == 0, "GetInfo change flag");
        check(type == 2 && fmt == 1 && free_kb > 0, "GetInfo type/format/free");
    }
    { /* Mkdir (Open with 0x40) + ChDir */
        Call c;
        put32(c.send, 8, 0x40);
        c.name("/BASCUS-97113ico");
        check(c.run(2) == 0, "Mkdir /BASCUS-97113ico");
        Call cd;
        put32(cd.send, 0x10, 0x2000);   /* pwd writeback */
        cd.name("/BASCUS-97113ico");
        check(cd.run(0xC) == 0, "ChDir into save dir");
        check(std::strcmp((char*)&g_fake_ram[0x2000], "/") == 0, "ChDir pwd writeback");
    }
    { /* Create + write (cwd-relative), close */
        Call c;
        put32(c.send, 8, 0x203);        /* CREAT | RDWR */
        c.name("icon.sys");
        int32_t fd = c.run(2);
        check(fd >= 0, "Open create icon.sys");
        /* Write: 5 inline head bytes + 11 from fake EE RAM. */
        const char* tail = "0123456789A";
        std::memcpy(&g_fake_ram[0x3000], tail, 11);
        Call w;
        put32(w.send, 0, (uint32_t)fd);
        put32(w.send, 0xC, 11);         /* tail size */
        put32(w.send, 0x14, 5);         /* head size */
        put32(w.send, 0x18, 0x3000);    /* tail addr */
        std::memcpy(w.send + 0x20, "HEAD.", 5);
        check(w.run(6, 0x30) == 16, "Write 5+11 bytes");
        Call cl;
        put32(cl.send, 0, (uint32_t)fd);
        check(cl.run(3, 0x30) == 0, "Close");
    }
    { /* Read it back with a seek */
        Call c;
        put32(c.send, 8, 1);            /* RDONLY */
        c.name("/BASCUS-97113ico/icon.sys");
        int32_t fd = c.run(2);
        check(fd >= 0, "Open read icon.sys");
        Call s;
        put32(s.send, 0, (uint32_t)fd);
        put32(s.send, 0x10, 5);
        put32(s.send, 0x14, 0);         /* SEEK_SET */
        check(s.run(4, 0x30) == 5, "Seek to 5");
        Call r;
        put32(r.send, 0, (uint32_t)fd);
        put32(r.send, 0xC, 11);
        put32(r.send, 0x18, 0x4000);
        put32(r.send, 0x1C, 0x5000);
        check(r.run(5, 0x30) == 11, "Read 11 bytes");
        check(std::memcmp(&g_fake_ram[0x4000], "0123456789A", 11) == 0, "Read data matches");
        uint32_t e0, e1;
        std::memcpy(&e0, &g_fake_ram[0x5000], 4);
        std::memcpy(&e1, &g_fake_ram[0x5004], 4);
        check(e0 == 0 && e1 == 0, "Read edge block zeroed");
        Call cl;
        put32(cl.send, 0, (uint32_t)fd);
        cl.run(3, 0x30);
    }
    { /* GetDir: root pattern, then subdir pattern with dot entries */
        Call c;
        put32(c.send, 0xC, 8);          /* maxent */
        put32(c.send, 0x10, 0x6000);    /* table */
        c.name("/*");
        check(c.run(0xD) == 1, "GetDir /* finds the save dir");
        check(std::strcmp((char*)&g_fake_ram[0x6020], "BASCUS-97113ico") == 0, "GetDir entry name");
        uint16_t attr;
        std::memcpy(&attr, &g_fake_ram[0x6014], 2);
        check(attr == 0x8427, "GetDir dir attr");
        Call c2;
        put32(c2.send, 0xC, 8);
        put32(c2.send, 0x10, 0x7000);
        c2.name("/BASCUS-97113ico/*");
        /* ".", "..", icon.sys */
        check(c2.run(0xD) == 3, "GetDir subdir with dot entries");
        check(std::strcmp((char*)&g_fake_ram[0x7000 + 2 * 0x40 + 0x20], "icon.sys") == 0,
            "GetDir file entry name");
    }
    { /* SetFileInfo: attributes + timestamps via sidecar */
        Call c;
        uint8_t info[0x40] = {0};
        info[1] = 30; info[2] = 15; info[3] = 12; info[4] = 24; info[5] = 12;
        info[6] = (uint8_t)(2001 & 0xFF); info[7] = (uint8_t)(2001 >> 8);
        info[0x14] = 0x97; info[0x15] = 0x84;
        rt_gwrite_bytes(0x8000, info, sizeof(info));
        put32(c.send, 8, 5);            /* set create time + attr */
        put32(c.send, 0x10, 0x8000);
        c.name("/BASCUS-97113ico/icon.sys");
        check(c.run(0xE) == 0, "SetFileInfo ctime+attr");
        Call g;
        put32(g.send, 0xC, 4);
        put32(g.send, 0x10, 0x9000);
        g.name("/BASCUS-97113ico/icon.sys");
        check(g.run(0xD) == 1, "GetDir stat one file");
        check(g_fake_ram[0x9006] == (2001 & 0xFF) && g_fake_ram[0x9007] == (2001 >> 8),
            "create year persisted");
    }
    { /* Rename (SetFileInfo flag 0x10) and Delete */
        Call c;
        uint8_t info[0x40] = {0};
        std::memcpy(info + 0x20, "renamed.bin", 12);
        rt_gwrite_bytes(0xA000, info, sizeof(info));
        put32(c.send, 8, 0x10);
        put32(c.send, 0x10, 0xA000);
        c.name("/BASCUS-97113ico/icon.sys");
        check(c.run(0xE) == 0, "Rename icon.sys");
        Call d;
        d.name("/BASCUS-97113ico/renamed.bin");
        check(d.run(0xF) == 0, "Delete renamed.bin");
        Call d2;
        d2.name("/BASCUS-97113ico");
        check(d2.run(0xF) == 0, "Delete empty save dir");
        Call d3;
        d3.name("/BASCUS-97113ico");
        check(d3.run(0xF) == -4, "Delete missing dir -> -4");
    }
    { /* GetEntSpace on root */
        Call c;
        c.name("/");
        check(c.run(0x12) > 0, "GetEntSpace root");
    }

    std::printf("[test] %s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
        g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
