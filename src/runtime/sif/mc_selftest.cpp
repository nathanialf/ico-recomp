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
#include <filesystem>
#include <string>
#include <cstdlib>
#include <cstring>

/* ---- runtime stubs -------------------------------------------------------- */

extern "C" {
uint8_t* g_pages[1 << 16]; /* unused by mc.cpp; satisfies the extern */
}

static uint8_t g_fake_ram[4 * 1024 * 1024];
static RtRpcServiceFn g_mc_fn = nullptr;

/* The runtime's four level entry points, all onto one line here: a
 * selftest has one reader and no level to filter by. */
void rt_log_line(const char* component, const char* fmt, va_list ap);

void rt_log_error(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_warn(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_info(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_debug(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
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

/* mc.cpp resolves config/local.toml and a relative saves dir against this;
 * the selftest always runs against the source tree. */
const char* rt_base_dir() { return ICORECOMP_SOURCE_ROOT; }

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
    /* The card lives under ICORECOMP_SAVES_DIR. When the caller set none
     * (CI runs the binary bare), a fresh directory under the system temp
     * path is used and named, so the test never touches a real saves/. */
    if (!std::getenv("ICORECOMP_SAVES_DIR")) {
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        std::filesystem::path dir;
        for (unsigned n = 0;; ++n) {
            dir = base / ("icorecomp-mc-selftest-" + std::to_string(n));
            std::error_code ec;
            if (std::filesystem::create_directory(dir, ec)) break;
            if (n > 10000) { std::printf("[test] no scratch directory under %s\n", base.string().c_str()); return 2; }
        }
        static std::string env_value;
        env_value = dir.string();
#ifdef _WIN32
        _putenv_s("ICORECOMP_SAVES_DIR", env_value.c_str());
#else
        setenv("ICORECOMP_SAVES_DIR", env_value.c_str(), 1);
#endif
        std::printf("[test] ICORECOMP_SAVES_DIR unset; using %s\n", env_value.c_str());
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
    /* The directory name below is measured, not invented: iosMcMgrChdirProduct
     * (0x00138D40) copies a 17-byte string constant into its request buffer and
     * hands it to sceMcMkdir and then to sceMcChdir, and a run of the port logs
     * that same name on both of the boot check's two passes. Nothing in mcserv
     * depends on the string; the exercise uses the real one so that these log
     * lines and the ones from a run read the same. */
    { /* Mkdir (Open with 0x40) + ChDir */
        Call c;
        put32(c.send, 8, 0x40);
        c.name("/BESCES-50760ico");
        check(c.run(2) == 0, "Mkdir /BESCES-50760ico");
        /* ICO re-runs Mkdir before every file it writes; mcman answers -4
         * (sceMcResNoEntry) for an entry that already exists, and the game's
         * save thread continues only on 0 or -4. The ChDir below then proves
         * the directory survived the second call. */
        Call again;
        put32(again.send, 8, 0x40);
        again.name("/BESCES-50760ico");
        check(again.run(2) == -4, "Mkdir existing dir -> -4");
        Call cd;
        put32(cd.send, 0x10, 0x2000);   /* pwd writeback */
        cd.name("/BESCES-50760ico");
        check(cd.run(0xC) == 0, "ChDir into save dir");
        /* The other half of the pair this file's header names: ChDir into a
         * directory that is not there answers -4, which
         * iosMcMgrChdirProduct rewrites to -14 so its caller can tell "no
         * save yet" from a card error. That is the path every fresh card
         * takes on its first boot. */
        Call cd_missing;
        put32(cd_missing.send, 0x10, 0x2000);
        cd_missing.name("/BESCES-50760nosuch");
        check(cd_missing.run(0xC) == -4, "ChDir into a missing dir -> -4");
        /* And the failed ChDir left the card where it was, which is what
         * makes the caller's retry meaningful. */
        Call cd_back;
        put32(cd_back.send, 0x10, 0x2000);
        cd_back.name("/BESCES-50760ico");
        check(cd_back.run(0xC) == 0, "the card is still in the save dir after that");
        /* mcman writes the directory the card is now in, with no leading
         * slash, not the one it was in before (ps2sdk ps2mc_fio.c,
         * mcman_chdir). */
        check(std::strcmp((char*)&g_fake_ram[0x2000], "BESCES-50760ico") == 0,
            "ChDir pwd writeback");
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
        c.name("/BESCES-50760ico/icon.sys");
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
        check(std::strcmp((char*)&g_fake_ram[0x6020], "BESCES-50760ico") == 0, "GetDir entry name");
        uint16_t attr;
        std::memcpy(&attr, &g_fake_ram[0x6014], 2);
        check(attr == 0x8427, "GetDir dir attr");
        Call c2;
        put32(c2.send, 0xC, 8);
        put32(c2.send, 0x10, 0x7000);
        c2.name("/BESCES-50760ico/*");
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
        c.name("/BESCES-50760ico/icon.sys");
        check(c.run(0xE) == 0, "SetFileInfo ctime+attr");
        Call g;
        put32(g.send, 0xC, 4);
        put32(g.send, 0x10, 0x9000);
        g.name("/BESCES-50760ico/icon.sys");
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
        c.name("/BESCES-50760ico/icon.sys");
        check(c.run(0xE) == 0, "Rename icon.sys");
        Call d;
        d.name("/BESCES-50760ico/renamed.bin");
        check(d.run(0xF) == 0, "Delete renamed.bin");
        Call d2;
        d2.name("/BESCES-50760ico");
        check(d2.run(0xF) == 0, "Delete empty save dir");
        Call d3;
        d3.name("/BESCES-50760ico");
        check(d3.run(0xF) == -4, "Delete missing dir -> -4");
    }
    { /* GetEntSpace on root */
        Call c;
        c.name("/");
        check(c.run(0x12) > 0, "GetEntSpace root");
    }

    { /* The fresh-card create sequence the retail game runs.
       *
       * Read off SCES_507.60 at the addresses below, not from a run.
       * iosMcMgrSaveSeg (0x00138FE0)
       * saves one segment at a time and always opens with iosMcMgrChdirProduct
       * (0x00138D40), which does GetInfo, then Mkdir of the save directory,
       * then ChDir into it; the segment's own file is then created with mode
       * 0x203, written, flushed and closed. The segment table at 0x0029B590
       * names the files: segment 1 is icon.sys, segments 2 to 4 are
       * boy_blk.ico, segment 0 is the product block (the save directory's own
       * name again) and segment 5 is "game." plus a "%3.3d" index. Segments
       * outside 1..4 get a trailing 4-byte checksum word written after the
       * body, which is the second Write on the product block below.
       *
       * The sizes are measured too: the icon descriptors at 0x0055F70C give
       * icon.sys as 964 bytes and boy_blk.ico as 95624, and the product block
       * is the 0x1F0-byte slot record the game keeps at 0x0029B5F0.
       *
       * la_system_save_processing (0x001BCBA8) is the layout item that creates
       * the whole set on a card that has none: GetBlockSaveInfo, SaveIconBlock
       * (segments 1 then 2), SaveProductBlock (segment 0), then SaveGameBlock
       * for indices 0 to 9. Its state 0 seeds the index from 0 and its state
       * 10 (0x001BCE74) re-enters state 7 with the index incremented while it
       * is below 10, so ten game blocks are written. That is what this
       * exercise replays, in order. */
        const char* kDir = "/BESCES-50760ico";
        struct Seg { const char* name; uint32_t size; uint32_t chunk; bool checksum; };
        const Seg segs[] = {
            {"icon.sys",        964,   964,  false},
            {"boy_blk.ico",     95624, 2048, false},
            {"BESCES-50760ico", 0x1F0, 0x1F0, true},
            {"game.000",        0x1F0, 0x1F0, true},
            {"game.001",        0x1F0, 0x1F0, true},
            {"game.002",        0x1F0, 0x1F0, true},
            {"game.003",        0x1F0, 0x1F0, true},
            {"game.004",        0x1F0, 0x1F0, true},
            {"game.005",        0x1F0, 0x1F0, true},
            {"game.006",        0x1F0, 0x1F0, true},
            {"game.007",        0x1F0, 0x1F0, true},
            {"game.008",        0x1F0, 0x1F0, true},
            {"game.009",        0x1F0, 0x1F0, true},
        };
        /* Source bytes for the writes, at a deliberately unaligned guest
         * address so that libmc's 16-byte split puts a head in the packet and
         * the rest in EE RAM, the way the real sceMcWrite does. */
        const uint32_t kSrc = 0x20004;
        for (uint32_t i = 0; i < 128 * 1024; ++i) g_fake_ram[kSrc + i] = (uint8_t)(i * 7 + 3);

        bool all_ok = true;
        for (size_t s = 0; s < sizeof(segs) / sizeof(segs[0]); ++s) {
            const Seg& sg = segs[s];
            Call gi;
            put32(gi.send, 4, 0);
            put32(gi.send, 0x1C, 0xB000);
            if (gi.run(1, 0x30) != 0) all_ok = false;    /* card present, steady */
            Call mk;
            put32(mk.send, 8, 0x40);
            mk.name(kDir);
            int32_t mkres = mk.run(2);
            /* 0 the first time, -4 for every segment after it: the two values
             * iosMcMgrChdirProduct accepts. */
            if (mkres != (s == 0 ? 0 : -4)) all_ok = false;
            Call cd;
            put32(cd.send, 0x10, 0xC000);
            cd.name(kDir);
            if (cd.run(0xC) != 0) all_ok = false;
            Call op;
            put32(op.send, 8, 0x203);
            op.name(sg.name);                            /* cwd-relative */
            int32_t fd = op.run(2);
            if (fd < 0) { all_ok = false; continue; }
            uint32_t sent = 0;
            while (sent < sg.size) {
                uint32_t n = sg.size - sent < sg.chunk ? sg.size - sent : sg.chunk;
                uint32_t addr = kSrc + sent;
                uint32_t head = n <= 16 ? n : (16 - (addr & 15)) & 15;
                uint32_t tail = n - head;
                Call w;
                put32(w.send, 0, (uint32_t)fd);
                put32(w.send, 0xC, tail);
                put32(w.send, 0x14, head);
                put32(w.send, 0x18, addr + head);
                std::memcpy(w.send + 0x20, &g_fake_ram[addr], head);
                if (w.run(6, 0x30) != (int32_t)n) all_ok = false;
                sent += n;
            }
            if (sg.checksum) {                           /* the trailing word */
                Call w;
                put32(w.send, 0, (uint32_t)fd);
                put32(w.send, 0xC, 0);
                put32(w.send, 0x14, 4);
                std::memcpy(w.send + 0x20, "CHK\0", 4);
                if (w.run(6, 0x30) != 4) all_ok = false;
            }
            Call fl;
            put32(fl.send, 0, (uint32_t)fd);
            if (fl.run(0xA, 0x30) != 0) all_ok = false;
            Call cl;
            put32(cl.send, 0, (uint32_t)fd);
            if (cl.run(3, 0x30) != 0) all_ok = false;
        }
        check(all_ok, "fresh-card create sequence");

        /* Every file is the length the guest wrote. Seek to the end reports
         * it, the same way mcman's McSeek does. */
        bool sizes_ok = true;
        for (size_t s = 0; s < sizeof(segs) / sizeof(segs[0]); ++s) {
            char path[64];
            std::snprintf(path, sizeof(path), "%s/%s", kDir, segs[s].name);
            Call op;
            put32(op.send, 8, 1);
            op.name(path);
            int32_t fd = op.run(2);
            if (fd < 0) { sizes_ok = false; continue; }
            Call sk;
            put32(sk.send, 0, (uint32_t)fd);
            put32(sk.send, 0x10, 0);
            put32(sk.send, 0x14, 2);                     /* SEEK_END */
            uint32_t want = segs[s].size + (segs[s].checksum ? 4u : 0u);
            if (sk.run(4, 0x30) != (int32_t)want) sizes_ok = false;
            Call cl;
            put32(cl.send, 0, (uint32_t)fd);
            cl.run(3, 0x30);
        }
        check(sizes_ok, "created files are the written length");

        /* The listing iosMcMgrGetBlockSaveInfo (0x001397E8) asks for: pattern
         * "*", mode 0, maxent 0x14, from inside the save directory. It expects
         * "." and ".." to be in it, and it reads a slot index out of the last
         * three characters of every name. */
        Call gd;
        put32(gd.send, 0xC, 0x14);
        put32(gd.send, 0x10, 0xD000);
        gd.name("*");
        int32_t nent = gd.run(0xD);
        check(nent == 15, "GetDir '*' in the save dir lists 15 entries");
        check(std::strcmp((char*)&g_fake_ram[0xD000 + 0x20], ".") == 0 &&
              std::strcmp((char*)&g_fake_ram[0xD040 + 0x20], "..") == 0,
            "dot entries come first");
        uint32_t slots = 0;
        for (int32_t i = 0; i < nent; ++i) {
            const char* nm = (const char*)&g_fake_ram[0xD000 + (uint32_t)i * 0x40 + 0x20];
            size_t len = std::strlen(nm);
            if (len >= 8 && std::strncmp(nm, "game.", 5) == 0) {
                slots |= 1u << std::atoi(nm + len - 3);
            }
        }
        check(slots == 0x3FFu, "game block indices decode to 0..9");

        /* Two identical listings must come back in the same order. Card entry
         * order is a property of the card, so it cannot depend on whatever
         * order the host filesystem happens to hand back. */
        Call gd2;
        put32(gd2.send, 0xC, 0x14);
        put32(gd2.send, 0x10, 0xE000);
        gd2.name("*");
        bool same = gd2.run(0xD) == nent &&
            std::memcmp(&g_fake_ram[0xD000], &g_fake_ram[0xE000], (size_t)nent * 0x40) == 0;
        check(same, "GetDir order is stable across calls");
    }

    std::printf("[test] %s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
        g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
