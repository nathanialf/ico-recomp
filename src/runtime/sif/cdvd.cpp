/* sif/cdvd.cpp: the cdvdman/cdvdfsv RPC services of the virtual IOP.
 *
 * Serves the five servers the EE libcdvd binds (server ids and per-fno
 * send/recv layouts per ps2sdk ee/rpc/cdvd + iop/cdvd/cdvdfsv, clean-room
 * structural reference; the ids and layouts are public SDK wire facts):
 *
 *   0x80000592 init        fno 0: send {u32 mode}, recv {result, versions}
 *   0x80000593 s-commands  readclock/disktype/error/status/mmode/...
 *   0x80000595 n-commands  read/seek/standby/stop/pause/stream/diskready
 *   0x80000597 searchfile  ISO9660 path -> sceCdlFILE written back to EE
 *   0x8000059A diskready   fno 0: recv {SCECdComplete}
 *
 * Disc data comes from iso/iso9660.cpp. The virtual drive is always ready,
 * holds a PS2 DVD, and never errors; sector reads land directly in EE RAM
 * (so the EE-side unaligned-read fixup callback gets all-zero sizes and
 * copies nothing).
 *
 * Also registers the non-cdvd IOP services: iopheap and loadfile (here),
 * padman (sif/pad.cpp), mcserv (sif/mc.cpp), the game's own sound server
 * (sif/sndn2.cpp), and one documented loud stub (sdrdrv) that answers every
 * CALL with a zeroed receive buffer, logged.
 */
#include "rpc.h"

#include "../iso/iso9660.h"

#include <cinttypes>
#include <cstring>
#include <ctime>

namespace {

/* libcdvd-common.h constants (public SDK facts). */
constexpr uint32_t SCECdPS2DVD = 0x14;
constexpr uint32_t SCECdComplete = 2;    /* sceCdDiskReady: ready */
constexpr uint32_t SCECdStatPause = 0x0A;
constexpr uint32_t SCECdErNO = 0;

uint32_t rd32(const uint8_t* p, uint32_t off) { uint32_t v; std::memcpy(&v, p + off, 4); return v; }
void wr32(uint8_t* p, uint32_t off, uint32_t v) { if (p) std::memcpy(p + off, &v, 4); }

uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* ---- 0x80000592: init ---------------------------------------------------- */

void svc_init(uint32_t fno, const uint8_t* send, uint32_t send_size,
              uint8_t* recv, uint32_t recv_size) {
    uint32_t mode = send_size >= 4 ? rd32(send, 0) : 0;
    rt_log("cdvd", "init fno=%u sceCdInit(mode=%u) -> 1 (recv_size=%u)", fno, mode, recv_size);
    /* CdInitPkt: {result, cdvdfsv_version, cdvdman_version, verbose}; the
     * 2001-era EE library only consumes result. */
    if (recv_size >= 4) wr32(recv, 0, 1);
}

/* ---- 0x80000593: S-commands ---------------------------------------------- */

void svc_scmd(uint32_t fno, const uint8_t* send, uint32_t send_size,
              uint8_t* recv, uint32_t recv_size) {
    switch (fno) {
        case 0x01: { /* sceCdReadClock: recv {result, sceCdCLOCK(8)} */
            std::time_t now = std::time(nullptr);
            std::tm tmv {};
            localtime_r(&now, &tmv);
            unsigned year = tmv.tm_year + 1900;
            unsigned y2k = year >= 2000 ? year - 2000 : 0;
            if (recv_size >= 16) {
                wr32(recv, 0, 1);           /* result */
                recv[4] = 0;                /* stat: ok */
                recv[5] = bcd((unsigned)tmv.tm_sec % 60);
                recv[6] = bcd((unsigned)tmv.tm_min);
                recv[7] = bcd((unsigned)tmv.tm_hour);
                recv[8] = 0;                /* pad */
                recv[9] = bcd((unsigned)tmv.tm_mday);
                recv[10] = bcd((unsigned)tmv.tm_mon + 1);
                recv[11] = bcd(y2k % 100);
            }
            rt_log("cdvd", "scmd fno=0x01 sceCdReadClock -> %04u-%02u-%02u %02u:%02u:%02u (BCD)",
                year, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
            break;
        }
        case 0x03: /* sceCdGetDiskType */
            wr32(recv, 0, SCECdPS2DVD);
            rt_log("cdvd", "scmd fno=0x03 sceCdGetDiskType -> SCECdPS2DVD (0x%02x)", SCECdPS2DVD);
            break;
        case 0x04: /* sceCdGetError */
            wr32(recv, 0, SCECdErNO);
            rt_log("cdvd", "scmd fno=0x04 sceCdGetError -> SCECdErNO");
            break;
        case 0x05: /* sceCdTrayReq: recv {result, traychk} */
            wr32(recv, 0, 1);
            if (recv_size >= 8) wr32(recv, 4, 0);
            rt_log("cdvd", "scmd fno=0x05 sceCdTrayReq(mode=%u) -> 1, traychk=0",
                send_size >= 4 ? rd32(send, 0) : 0);
            break;
        case 0x0C: /* sceCdStatus */
            wr32(recv, 0, SCECdStatPause);
            rt_log("cdvd", "scmd fno=0x0c sceCdStatus -> SCECdStatPause (0x%02x)", SCECdStatPause);
            break;
        case 0x16: /* sceCdBreak */
            wr32(recv, 0, 1);
            rt_log("cdvd", "scmd fno=0x16 sceCdBreak -> 1");
            break;
        case 0x22: /* sceCdMmode */
            wr32(recv, 0, 1);
            rt_log("cdvd", "scmd fno=0x22 sceCdMmode(media=%u) -> 1",
                send_size >= 4 ? rd32(send, 0) : 0);
            break;
        case 0x23: /* sceCdChangeThreadPriority */
            wr32(recv, 0, 1);
            rt_log("cdvd", "scmd fno=0x23 sceCdChangeThreadPriority(%u) -> 1",
                send_size >= 4 ? rd32(send, 0) : 0);
            break;
        default:
            /* Optimistic success, but loud: an fno outside the modeled set
             * is a signal to extend this switch (ps2sdk scmd.c enum). */
            if (recv_size >= 4) wr32(recv, 0, 1);
            rt_log("cdvd", "WARNING scmd fno=0x%02x NOT MODELED (send_size=%u recv_size=%u): "
                "answered result=1, rest zeroed", fno, send_size, recv_size);
            break;
    }
}

/* ---- 0x80000595: N-commands ---------------------------------------------- */

uint32_t g_stream_lsn = 0;

/* Shared by ncmd fno 0x01 (sceCdRead, destination in EE RAM) and fno 0x0D
 * (this library vintage's read into IOP memory, used by the streaming
 * loader: the destinations observed are the IOP ring buffers the game's
 * sound driver opened). Both carry the same 0x18-byte request block:
 * {lbn, sectors, buf, sceCdRMode bytes at +0xC..+0xE, unaligned-fixup block
 * address at +0x10, read-position word address at +0x14}; the two writeback
 * addresses are EE-side library statics in both cases. Layout confirmed
 * against the vendor libcdvd read entry points in the decomp repo's
 * disassembly (behavioral reference; the fno assignments predate the
 * ps2sdk table, where 0x0C is readiopmem and 0x0D is diskready). */
void do_read(uint32_t fno, const uint8_t* send, uint32_t send_size) {
    if (send_size < 24) {
        rt_fatal("cdvd", rt_sched_current_ctx(), "ncmd read with short send (%u bytes)", send_size);
    }
    const bool to_iop = fno == 0x0D;
    uint32_t lbn = rd32(send, 0);
    uint32_t sectors = rd32(send, 4);
    uint32_t buf = rd32(send, 8) & 0x1FFFFFFFu;
    uint32_t mode = rd32(send, 12);
    uint32_t intr_addr = rd32(send, 16) & 0x1FFFFFFFu;
    uint32_t pos_addr = rd32(send, 20) & 0x1FFFFFFFu;
    uint32_t datapattern = (mode >> 16) & 0xFF;
    rt_log("cdvd", "ncmd fno=0x%02x sceCdRead%s(lbn=%u sectors=%u buf=0x%08x mode=0x%06x) intr=0x%08x pos=0x%08x",
        fno, to_iop ? "IOPm" : "", lbn, sectors, buf, mode, intr_addr, pos_addr);
    if (to_iop && (uint64_t)buf + (uint64_t)sectors * 2048 > RT_IOP_RAM_SIZE) {
        rt_fatal("cdvd", rt_sched_current_ctx(),
            "ncmd fno=0x0d read overruns IOP RAM: buf=0x%08x + %u sectors", buf, sectors);
    }
    if (datapattern != 0) { /* SCECdSecS2048 only; ICO is a DVD title */
        rt_fatal("cdvd", rt_sched_current_ctx(),
            "sceCdRead datapattern 0x%02x not modeled (only 2048-byte sectors)", datapattern);
    }
    /* The streaming loader's prefetch window slides off the end of the
     * disc once it runs through the trailing LDUMMY padding file (observed
     * after the title transition: overlapping fno=0x0D reads advancing
     * ~376 sectors each). On hardware those reads fail in the lead-out and
     * the streamer shrugs it off; here we zero-fill and log so the run
     * stays alive without hiding the event. */
    if ((uint64_t)lbn + sectors > rt_iso_total_sectors()) {
        rt_log("cdvd", "read past end of disc (lbn=%u sectors=%u, disc has %u): zero-filling %u sectors",
            lbn, sectors, rt_iso_total_sectors(),
            (uint32_t)((uint64_t)lbn + sectors - (lbn < rt_iso_total_sectors() ? rt_iso_total_sectors() : lbn)));
    }
    uint8_t sec[2048];
    for (uint32_t i = 0; i < sectors; ++i) {
        if (!rt_iso_read_sector(lbn + i, sec)) {
            std::memset(sec, 0, sizeof(sec));
        }
        if (to_iop) {
            std::memcpy(rt_iop_ptr(buf + i * 2048), sec, 2048);
        } else {
            rt_gwrite_bytes(buf + i * 2048, sec, 2048);
        }
    }
    /* Unaligned-fixup info for the EE end-callback: all zero = the whole
     * transfer already landed in place, copy nothing. Covers both the
     * 16-byte 1300 and 64-byte 1400 layout prefixes. */
    if (intr_addr) {
        uint8_t zero[16] = {0};
        rt_gwrite_bytes(intr_addr, zero, sizeof(zero));
    }
    if (pos_addr) {
        uint32_t done = sectors * 2048;
        rt_gwrite_bytes(pos_addr, &done, 4);
    }
}

void do_stream(const uint8_t* send, uint32_t send_size, uint8_t* recv, uint32_t recv_size) {
    if (send_size < 20) {
        rt_fatal("cdvd", rt_sched_current_ctx(), "ncmd stream with short send (%u bytes)", send_size);
    }
    uint32_t lbn = rd32(send, 0);
    uint32_t nsectors = rd32(send, 4);
    uint32_t buf = rd32(send, 8) & 0x1FFFFFFFu;
    uint32_t cmd = rd32(send, 12);
    /* CdvdStCmd_t: 1 START 2 READ 3 STOP 4 SEEK 5 INIT 6 STAT 7 PAUSE
     * 8 RESUME 9 SEEKF (ps2sdk ncmd.c). */
    static const char* names[] = {"?", "START", "READ", "STOP", "SEEK", "INIT", "STAT", "PAUSE", "RESUME", "SEEKF"};
    rt_log("cdvd", "ncmd fno=0x09 sceCdSt%s(lbn=%u n=%u buf=0x%08x)",
        cmd < 10 ? names[cmd] : "?", lbn, nsectors, buf);
    uint32_t result = 1;
    switch (cmd) {
        case 1: case 4: case 9: /* START / SEEK / SEEKF */
            g_stream_lsn = lbn;
            break;
        case 2: { /* READ nsectors at the stream position */
            uint8_t sec[2048];
            uint32_t i = 0;
            for (; i < nsectors; ++i) {
                if (!rt_iso_read_sector(g_stream_lsn + i, sec)) break;
                rt_gwrite_bytes(buf + i * 2048, sec, 2048);
            }
            g_stream_lsn += i;
            result = i; /* sectors read; high half = error (0) */
            break;
        }
        case 6: /* STAT: 0 = buffer fully available */
            result = 0;
            break;
        default: /* STOP / INIT / PAUSE / RESUME: success */
            break;
    }
    if (recv_size >= 4) wr32(recv, 0, result);
}

void svc_ncmd(uint32_t fno, const uint8_t* send, uint32_t send_size,
              uint8_t* recv, uint32_t recv_size) {
    switch (fno) {
        case 0x01: /* sceCdRead */
            do_read(fno, send, send_size);
            break;
        case 0x05: /* sceCdSeek: async success, callback only */
            rt_log("cdvd", "ncmd fno=0x05 sceCdSeek(lbn=%u) -> ok", send_size >= 4 ? rd32(send, 0) : 0);
            break;
        case 0x06: /* sceCdStandby */
        case 0x07: /* sceCdStop */
        case 0x08: /* sceCdPause */
            rt_log("cdvd", "ncmd fno=0x%02x sceCd%s -> ok", fno,
                fno == 0x06 ? "Standby" : (fno == 0x07 ? "Stop" : "Pause"));
            break;
        case 0x09: /* sceCdStream */
            do_stream(send, send_size, recv, recv_size);
            break;
        case 0x0D:
            /* In this library vintage 0x0D is the read into IOP memory, not
             * diskready: the vendor read entry point sends the standard
             * 0x18-byte read block (async, no recv; completion is the RPC
             * END packet, whose EE-side end callback clears the library's
             * busy flags). A 0x0D with no read block is not a shape this
             * library emits. */
            if (send_size >= 24) {
                do_read(fno, send, send_size);
            } else {
                rt_fatal("cdvd", rt_sched_current_ctx(),
                    "ncmd fno=0x0d with send_size=%u (expected the 0x18-byte read block)", send_size);
            }
            break;
        case 0x0E:
            /* Drive status poll (sync, 4-byte recv). The library's read
             * entry points call this before issuing a read and refuse to
             * start while it returns SCECdStatRead (0x06). Reads complete
             * synchronously inside the RPC call here, so the virtual drive
             * is never mid-read: always paused. */
            if (recv_size >= 4) wr32(recv, 0, SCECdStatPause);
            {
                static uint64_t polls = 0;
                ++polls;
                if (rt_trace() || (polls & (polls - 1)) == 0) {
                    rt_log("cdvd", "ncmd fno=0x0e drive status poll -> SCECdStatPause (0x%02x) [#%" PRIu64 "]",
                        SCECdStatPause, polls);
                }
            }
            break;
        default:
            if (recv_size >= 4) wr32(recv, 0, 1);
            rt_log("cdvd", "WARNING ncmd fno=0x%02x NOT MODELED (send_size=%u recv_size=%u): "
                "answered result=1, rest zeroed", fno, send_size, recv_size);
            break;
    }
}

/* ---- 0x80000597: searchfile ---------------------------------------------- */

void svc_searchfile(uint32_t fno, const uint8_t* send, uint32_t send_size,
                    uint8_t* recv, uint32_t recv_size) {
    /* SearchFilePkt layouts (cdvdfsv wire fact; sizes distinguish library
     * vintages, ps2sdk cdvdfsv.c switch on length):
     *   292 bytes: fp(32) + path[256] at +32 + EE writeback addr at +288
     *   296 bytes: fp(32) + pad(4) + path[256] at +36 + writeback at +292
     *              (this game's libcdvd uses this one)
     * cdvdfsv DMAs the filled sceCdlFILE back to the writeback address and
     * returns the found/not-found result in the 4-byte recv. */
    if (send_size < 292) {
        rt_fatal("cdvd", rt_sched_current_ctx(),
            "searchfile fno=%u send_size=%u, expected >= 292", fno, send_size);
    }
    uint32_t name_off = 32, dest_off = 288;
    if (send_size >= 296) {
        name_off = 36;
        dest_off = 292;
    }
    if (send_size != 292 && send_size != 296) {
        rt_log("cdvd", "WARNING searchfile send_size=%u (not a known layout); "
            "using offsets %u/%u", send_size, name_off, dest_off);
    }
    char path[257];
    std::memcpy(path, send + name_off, 256);
    path[256] = 0;
    uint32_t eedest = rd32(send, dest_off) & 0x1FFFFFFFu;

    RtIsoFile f;
    bool found = rt_iso_search(path, &f);
    rt_log("cdvd", "searchfile fno=%u '%s' -> %s%s LBA=%u size=%u (writeback to 0x%08x)",
        fno, path, found ? "FOUND " : "not found", found ? f.name : "",
        f.lsn, f.size, eedest);

    uint8_t out[32] = {0};
    std::memcpy(out + 0, &f.lsn, 4);
    std::memcpy(out + 4, &f.size, 4);
    std::memcpy(out + 8, f.name, 16);
    std::memcpy(out + 24, f.date, 8);
    if (eedest) rt_gwrite_bytes(eedest, out, sizeof(out));
    if (recv_size >= 4) wr32(recv, 0, found ? 1 : 0);
}

/* ---- 0x8000059A: diskready ----------------------------------------------- */

void svc_diskready(uint32_t fno, const uint8_t* send, uint32_t send_size,
                   uint8_t* recv, uint32_t recv_size) {
    uint32_t mode = send_size >= 4 ? rd32(send, 0) : 0;
    if (recv_size >= 4) wr32(recv, 0, SCECdComplete);
    rt_log("cdvd", "diskready fno=%u sceCdDiskReady(mode=%u) -> SCECdComplete", fno, mode);
}

/* ---- 0x80000003: iopheap ------------------------------------------------- */

/* Bump allocator over a reserved span of the virtual IOP RAM. Real
 * allocations matter here: the EE stages data into these buffers with
 * SifSetDma and later asks IOP-side services to consume them. Protocol per
 * ps2sdk ee/kernel/src/iopheap.c: fno 1 alloc {u32 size}->{u32 addr},
 * fno 2 free {u32 addr}->{u32 result}. */
/* Above the minted RPC staging buffers (rpc.cpp kBufBase 0x1C0000 + up to 32
 * services * 0x4000 = 0x240000). 0x200000 overlapped the sndn2 service's
 * staging buffer with the game's first sound-bank iopheap allocation. */
constexpr uint32_t kIopHeapBase = 0x00240000u;
constexpr uint32_t kIopHeapEnd = RT_IOP_RAM_SIZE - 0x10000u;
uint32_t g_iop_heap_ptr = kIopHeapBase;

void svc_iopheap(uint32_t fno, const uint8_t* send, uint32_t send_size,
                 uint8_t* recv, uint32_t recv_size) {
    switch (fno) {
        case 1: { /* alloc */
            uint32_t size = send_size >= 4 ? rd32(send, 0) : 0;
            uint32_t aligned = (size + 0xFF) & ~0xFFu;
            uint32_t addr = 0;
            if (g_iop_heap_ptr + aligned <= kIopHeapEnd) {
                addr = g_iop_heap_ptr;
                g_iop_heap_ptr += aligned;
            }
            if (recv_size >= 4) wr32(recv, 0, addr);
            rt_log("iopheap", "alloc(%u) -> 0x%06x (%u bytes of virtual IOP heap left)",
                size, addr, kIopHeapEnd - g_iop_heap_ptr);
            if (!addr) {
                rt_log("iopheap", "WARNING: virtual IOP heap exhausted (%u byte request)", size);
            }
            break;
        }
        case 2: /* free: bump allocator never reclaims; report success */
            if (recv_size >= 4) wr32(recv, 0, 0);
            rt_log("iopheap", "free(0x%06x) -> 0 (bump allocator, not reclaimed)",
                send_size >= 4 ? rd32(send, 0) : 0);
            break;
        default:
            if (recv_size >= 4) wr32(recv, 0, (uint32_t)-1);
            rt_log("iopheap", "WARNING fno=%u NOT MODELED (send_size=%u): returned -1",
                fno, send_size);
            break;
    }
}

/* ---- 0x80000006: loadfile (sceSifLoadModule) ----------------------------- */

int g_next_module_id = 1;

void svc_loadfile(uint32_t fno, const uint8_t* send, uint32_t send_size,
                  uint8_t* recv, uint32_t recv_size) {
    /* LF_F_MOD_LOAD (fno 0) argument block (ps2sdk ee/kernel/src/loadfile.c,
     * public wire fact): {u32 arg_len/result, u32 modres, char path[252],
     * char args[252]}. The reply lands in the first 8 bytes: result = module
     * id (>= 0 = success), modres = the module's start return (0 =
     * resident). The IOP is HLE'd, so every load "succeeds" without loading
     * anything; drivers the game expects from these modules are HLE'd at the
     * RPC layer instead (this file, padman/mcserv stubs, ...). */
    if (fno == 0 && send_size >= 8) {
        char path[253] = {0};
        uint32_t plen = send_size > 8 ? (send_size - 8 < 252 ? send_size - 8 : 252) : 0;
        std::memcpy(path, send + 8, plen);
        path[252] = 0;
        int id = g_next_module_id++;
        rt_log("iopmod", "loadfile fno=0 LoadModule('%s' arg_len=%u) -> module id %d (HLE, nothing loaded)",
            path, rd32(send, 0), id);
        if (recv_size >= 4) wr32(recv, 0, (uint32_t)id);
        if (recv_size >= 8) wr32(recv, 4, 0); /* modres: RESIDENT END */
        return;
    }
    if (fno == 0xFF) {
        /* LF_F_GET_VERSION (fno 0xFF, ps2sdk loadfile-common.h): the server
         * answers a 4-char version tag that the EE-side library memcmps
         * against the tag its own SDK generation shipped with; on mismatch
         * every sceSifLoadModule fails with 0xFFFEFFFC and this game's boot
         * retries forever. This SDK generation's tag is "2240" (SDK
         * handshake fact, discovered by running the version check; newer
         * ps2sdk loadfile answers "3100" the same way). */
        if (recv_size >= 4) std::memcpy(recv, "2240", 4);
        rt_log("iopmod", "loadfile fno=0xff GetVersion -> \"2240\"");
        return;
    }
    rt_log("iopmod", "WARNING loadfile fno=0x%x NOT MODELED (send_size=%u recv_size=%u): "
        "answered result=1, rest zeroed", fno, send_size, recv_size);
    if (recv_size >= 4) wr32(recv, 0, 1);
}

} // namespace

void rt_cdvd_register_services() {
    rt_iso_mount();

    rt_rpc_register_service(0x80000592, "cdvd:init", svc_init);
    rt_rpc_register_service(0x80000593, "cdvd:scmd", svc_scmd);
    rt_rpc_register_service(0x80000595, "cdvd:ncmd", svc_ncmd);
    rt_rpc_register_service(0x80000597, "cdvd:searchfile", svc_searchfile);
    rt_rpc_register_service(0x8000059A, "cdvd:diskready", svc_diskready);

    /* Non-cdvd IOP servers the boot path touches. */
    rt_rpc_register_service(0x80000003, "iopheap", svc_iopheap);
    rt_rpc_register_service(0x80000006, "loadfile", svc_loadfile);

    /* Remaining cdvdfsv-family servers this library may bind. 0x80000596 is
     * the extra/poweroff server (CD_SERVER_POFF in ps2sdk libcdvd.c); the
     * others are neighbors registered by various cdvdfsv vintages. Loud
     * stubs until a CALL shows up in the log and earns a real model. */
    rt_rpc_register_service(0x80000594, "cdvd:0594(stub)", nullptr);
    rt_rpc_register_service(0x80000596, "cdvd:poff(stub)", nullptr);
    rt_rpc_register_service(0x80000598, "cdvd:0598(stub)", nullptr);
    rt_rpc_register_service(0x80000599, "cdvd:0599(stub)", nullptr);
    rt_rpc_register_service(0x8000059C, "cdvd:diskready2(stub)", nullptr);

    /* Peripheral services with real models: padman (sif/pad.cpp, old wire
     * protocol + per-field pad data delivery) and mcserv (sif/mc.cpp, old
     * MCSERV protocol backed by the [saves] dir). */
    rt_pad_register_services();
    rt_mc_register_service();

    /* Documented loud stub (nullptr handler): BIND succeeds, CALLs are
     * logged and answered with zeroed receive data, so boot can progress
     * past subsystems that tolerate a dead peripheral. Server ids are
     * public SDK facts (ps2sdk iop module sources). */
    rt_rpc_register_service(0x80000701, "sdrdrv(stub)", nullptr);
    /* The game's own sound driver module (cdrom0:\SNDN2DRV.IRX, loaded via
     * the loadfile HLE above) registers its RPC server as ASCII "sndn".
     * Modeled in sndn2.cpp (ack protocol; no audio). */
    rt_sndn2_register_service();
}
