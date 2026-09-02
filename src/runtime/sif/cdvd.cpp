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

#include "../host/portable.h"
#include "../iso/iso9660.h"
#include "../snd/snd.h"

#include <cinttypes>
#include <cstring>
#include <vector>
#include <ctime>

namespace {

/* libcdvd-common.h constants (public SDK facts). */
constexpr uint32_t SCECdPS2DVD = 0x14;
constexpr uint32_t SCECdComplete = 2;    /* sceCdDiskReady: ready */
constexpr uint32_t SCECdStatPause = 0x0A;
constexpr uint32_t SCECdStatRead = 0x06; /* drive is mid-transfer */
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
            rt_localtime(now, &tmv);
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

/* Shared by ncmd fno 0x01 (sceCdRead, destination in EE RAM) and fno 0x0D
 * (this library vintage's read into IOP memory, used by the streaming
 * loader: the destinations observed are the IOP ring buffers the game's
 * sound driver opened). Both carry the same 0x18-byte request block:
 * {lbn, sectors, buf, sceCdRMode bytes at +0xC..+0xE, unaligned-fixup block
 * address at +0x10, read-position word address at +0x14}; the two writeback
 * addresses are EE-side library statics in both cases. Confirmed against
 * a raw dump of the block: 3a b3 01 00 | b8 00 00 00 | 00 80 2b 00 |
 * 02 00 00 00 | 00 1a 55 00 | c0 1a 55 00 decodes as lbn=111418
 * sectors=184 buf=0x2b8000 mode=2. 184 sectors is 0x5C000, exactly one
 * stream ring, which is the sound library's initial fill; later refills
 * are a third of a ring and move their destination along the ring. Layout
 * confirmed against the vendor libcdvd read entry points in the decomp
 * repo's disassembly (behavioral reference; the fno assignments predate
 * the ps2sdk table, where 0x0C is readiopmem and 0x0D is diskready). */
/* Virtual clock value until which the drive reports itself as reading. */
uint64_t g_drive_busy_until = 0;

uint32_t iop_alloc_room(uint32_t addr);

/* ---- virtual drive timing ------------------------------------------------
 *
 * One rate and one seek latency serve both the sceCdRead busy model below
 * and the stream ring model further down, so the two cannot drift apart.
 */

/* Sustained transfer rate of the virtual drive in MB/s. 5.4 MB/s is a PS2
 * DVD at its nominal 4x. ICORECOMP_CDVD_MBPS overrides it for development;
 * 0 turns the whole drive timing model off (transfers land instantly and
 * the ring is always full), which is what this runtime did before the
 * drive was paced. */
double drive_mbps() {
    static const double v = [] {
        const char* e = std::getenv("ICORECOMP_CDVD_MBPS");
        double x = e ? std::strtod(e, nullptr) : 5.4; /* PS2 DVD, about 4x */
        return x < 0.0 ? 0.0 : x;
    }();
    return v;
}

/* Bus cycles one 2048-byte sector takes to arrive at drive_mbps(). 0 when
 * the timing model is off. */
uint64_t sector_cycles() {
    const double mbps = drive_mbps();
    if (mbps <= 0.0) return 0;
    return (uint64_t)(2048.0 / (mbps * 1048576.0) * (double)RT_BUSCLK_HZ);
}

/* Bus cycles between a stream start/seek and the first sector reaching the
 * ring. 100 ms is the order of a full-stroke seek plus the drive settling
 * on a PS2 DVD; the retail figure for this drive is not measured here, so
 * ICORECOMP_CDVD_SEEK_MS sits next to ICORECOMP_CDVD_MBPS for sweeping it.
 * A negative value keeps the default with a log rather than being applied. */
uint64_t seek_cycles() {
    static const uint64_t v = [] {
        const char* e = std::getenv("ICORECOMP_CDVD_SEEK_MS");
        double ms = e ? std::strtod(e, nullptr) : 100.0;
        if (ms < 0.0) {
            rt_log("cdvd", "ICORECOMP_CDVD_SEEK_MS=%s is negative; keeping the 100 ms default", e);
            ms = 100.0;
        }
        return (uint64_t)(ms / 1000.0 * (double)RT_BUSCLK_HZ);
    }();
    return drive_mbps() <= 0.0 ? 0 : v;
}

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
     * after the title transition, as overlapping fno=0x0D reads). On
     * hardware those reads fail in the lead-out and
     * the streamer shrugs it off; here we zero-fill and log so the run
     * stays alive without hiding the event. */
    if ((uint64_t)lbn + sectors > rt_iso_total_sectors()) {
        rt_log("cdvd", "read past end of disc (lbn=%u sectors=%u, disc has %u): zero-filling %u sectors",
            lbn, sectors, rt_iso_total_sectors(),
            (uint32_t)((uint64_t)lbn + sectors - (lbn < rt_iso_total_sectors() ? rt_iso_total_sectors() : lbn)));
    }
    /* One bulk read, then one copy into the guest. A stream refill is a
     * third of the ring at a time (0x1EAAA bytes, 62 sectors) and the
     * initial fill is a whole ring (184). The refill lands on a whole
     * number of sectors because the reported cursor moves in 0x4000
     * transfer blocks, so eight blocks clear the threshold: 0x20000 bytes,
     * 64 sectors. A per-sector loop would turn either into that many
     * seek/read pairs. */
    /* sectors is guest supplied. The IOP path has a fatal guard above; the
     * EE path had none, so a wild count sized a multi-gigabyte vector before
     * any I/O happened. Refuse loudly instead. */
    if (sectors > rt_iso_total_sectors() + 4096) {
        rt_fatal("cdvd", rt_sched_current_ctx(),
            "sceCdRead sector count %u is larger than the disc (%u sectors)",
            sectors, rt_iso_total_sectors());
    }
    static std::vector<uint8_t> disc_buf;
    disc_buf.resize((size_t)sectors * 2048);
    uint32_t got = rt_iso_read_sectors(lbn, sectors, disc_buf.data());
    if (got < sectors) {
        std::memset(disc_buf.data() + (size_t)got * 2048, 0,
            ((size_t)sectors - got) * 2048);
    }
    if (to_iop) {
        size_t want = (size_t)sectors * 2048;
        /* A stream refill writes at ring_base + PREV for `consumed` bytes
         * (retail ACTSetEnvAllmighty), which stays inside the ring: the end
         * of the write is either the reported cursor or, on the wrap path,
         * exactly the ring size. Describe every refill, since the ring
         * arithmetic is otherwise invisible and it paces the whole stream,
         * and flag a write that leaves the ring as the anomaly it is. */
        uint32_t ring_base = 0, ring = 0, cursor = 0;
        if (rt_snd_stream_ring(buf, &ring_base, &ring, &cursor)) {
            uint32_t at = buf - ring_base;
            rt_log("cdvd", "stream refill: ring 0x%06x+0x%05x, write +0x%05x..+0x%05x "
                           "(%zu bytes, %u sectors from lbn %u), voice cursor +0x%05x%s",
                ring_base, ring, at, (uint32_t)(at + want), want, sectors, lbn, cursor,
                at + want > ring ? ", past the ring end" : "");
        }
        /* IOP RAM has no allocator protection, so the write lands where the
         * game aimed it even when that leaves the allocation; truncating
         * would drop bytes the game expects to be there. The hard bound is
         * the IOP RAM fatal above. Report a departure rather than hide it. */
        uint32_t room = iop_alloc_room(buf);
        if (room == 0) {
            /* Not inside any tracked allocation: either the game never got
             * this buffer from iopheap, or the allocation table filled. */
            static uint64_t untracked = 0;
            ++untracked;
            if (rt_trace() || (untracked & (untracked - 1)) == 0) {
                rt_log("cdvd", "read into untracked IOP buffer 0x%06x (%zu bytes); "
                               "extent unknown [#%" PRIu64 "]",
                    buf, want, untracked);
            }
        }
        if (room && want > room) {
            /* Never sampled: with the ring model correct this cannot happen,
             * so every occurrence is a real divergence and all of them have
             * to be visible, not one in a power of two. */
            rt_log("cdvd", "read into IOP buffer 0x%06x runs past its allocation: "
                           "%zu bytes requested, allocation has %u left",
                buf, want, room);
        }
        std::memcpy(rt_iop_ptr(buf), disc_buf.data(), want);
    } else {
        rt_gwrite_bytes(buf, disc_buf.data(), (size_t)sectors * 2048);
    }
    /* Unaligned-fixup info for the EE end-callback: all zero = the whole
     * transfer already landed in place, copy nothing. Covers both the
     * 16-byte 1300 and 64-byte 1400 layout prefixes. */
    if (intr_addr) {
        uint8_t zero[16] = {0};
        rt_gwrite_bytes(intr_addr, zero, sizeof(zero));
    }
    if (pos_addr) {
        /* The +0x14 word is the library's read-position word. Writing a byte
         * count here made the streaming scheduler walk backwards: successive
         * reads went lbn 112994 -> 112433 -> 111884 while the requested size
         * crept up ~13 sectors each time, so the ring was refilled from a
         * position that jumped around. The natural reading is the disc
         * position the drive reached. ICORECOMP_CDVD_POS selects the
         * semantic while this is being pinned down:
         *   end    (default) lbn + sectors, the position after the transfer
         *   sectors          count of sectors transferred
         *   bytes            previous behaviour, sectors * 2048 */
        static const int mode = [] {
            const char* e = std::getenv("ICORECOMP_CDVD_POS");
            if (!e) return 0;
            if (std::strcmp(e, "sectors") == 0) return 1;
            if (std::strcmp(e, "bytes") == 0) return 2;
            return 0;
        }();
        uint32_t pos = mode == 2 ? sectors * 2048
                     : mode == 1 ? sectors
                                 : lbn + sectors;
        rt_gwrite_bytes(pos_addr, &pos, 4);
    }

    /* Hold the virtual drive busy for as long as the transfer would take.
     * The data is already in place and the RPC completes immediately, so
     * nothing blocks; what this restores is the back pressure the library
     * already looks for. Its read entry points poll fno 0x0E and refuse to
     * start while the drive reports SCECdStatRead, so answering "idle"
     * unconditionally let the streaming prefetch window slide as fast as
     * the EE loop runs and rewrite the sound ring under the voices, which
     * sounds like fast forward. ICORECOMP_CDVD_MBPS sets the rate, 0
     * restores the unthrottled behaviour. */
    const uint64_t per_sector = sector_cycles();
    if (per_sector) {
        g_drive_busy_until = rt_clock_now() + (uint64_t)sectors * per_sector;
    }
}

/* ---- the CD stream (ncmd fno 0x09) ---------------------------------------
 *
 * cdvdman owns a ring of sectors in IOP RAM which the drive fills in the
 * background. sceCdStRead hands the caller only what has already arrived
 * and returns that count packed as (error << 16) | sectors. Measured in the
 * decomp: the game's own copy of sceCdStRead is func_0024DAB8 (decomp
 * asm/nonmatchings/src/cod/vendor_24AAC8/func_0024DAB8.s), which after the
 * stream RPC does "srl $16, $2, 16" for the error and
 * "andi $19, $2, 0xFFFF" for the sector count. Same split as ps2sdk
 * ee/rpc/cdvd/src/ncmd.c, whose STMNBLK path returns "ret & 0xFFFF" under
 * the comment "read only data currently in stream buffer".
 *
 * A short read is normal and both callers handle it:
 *   - blocking mode (the movie, ito/mpeg readBufBeginPut -> ios/inflate.c
 *     inflate_start -> sceCdStRead(32 sectors, mode 1)) loops inside the EE
 *     library until the whole request is satisfied, sleeping 8 hsyncs
 *     through SetAlarm/WaitSema (func_0024BFD0, decomp
 *     src/cod/vendor_24AAC8.c) whenever a call returns 0 sectors;
 *   - non-blocking mode (the file loader, decomp
 *     asm/nonmatchings/ios/cdvd/iosCdvdManager.s around lines 96-131 and
 *     190-219) commits whatever came back, and on a zero-sector reply parks
 *     the thread in SleepThread until the per-frame handler wakes it.
 * So a latency model only has to get the returned count right.
 *
 * Ring geometry comes from sceCdStInit and this title uses two of them
 * (decomp tough_nuts/func_00132FF0/func_00132FF0.c, the readable form of
 * ios/cdvd.c func_00132FF0 and func_00131480): the movie stream asks for
 * bufmax 0x240 sectors in 0x24 banks over a 0x120010-byte IOP allocation,
 * the file loader for bufmax 0x50 in 5 banks over 0x28010 bytes. Both are
 * bufmax * 2048 plus the 16-byte allocation tag, which is what fixes bufmax
 * as a sector count rather than a byte count.
 *
 * Delivering everything the caller asked for inside one call is what broke
 * the stage loader: 320 KB of stage data landed in the frame that tore the
 * previous stage down, while the display list kicked in that same frame
 * still carried the previous frame's model tags pointing into that memory.
 * On hardware those bytes are still intact when that chain is kicked,
 * because the drive needs ten or more fields to deliver them and the chain
 * cursors are reset before the data arrives. VIF1 then took a stage model
 * word as a VIFcode and raised its unknown-VIFcode fatal.
 *
 * State machine, driven entirely by the virtual clock (rt_clock_now, EE bus
 * cycles): START/SEEK/SEEKF set the position, empty the ring and schedule
 * the first arrival at now + seek_cycles(); each further sector arrives one
 * sector_cycles() later until the ring is full, at which point the drive
 * stalls and resumes when a read frees room; READ hands over
 * min(requested, buffered); STAT reports what is buffered; PAUSE freezes
 * arrival and RESUME shifts the schedule by the time spent paused; STOP
 * discards the ring.
 */
struct StreamState {
    bool inited = false;
    uint32_t bufmax = 0;       /* ring capacity in sectors (sceCdStInit arg 1) */
    uint32_t bankmax = 0;      /* number of banks (sceCdStInit arg 2) */
    uint32_t iop_buf = 0;      /* ring address in IOP RAM (sceCdStInit arg 3) */
    bool running = false;      /* between START/SEEK and STOP */
    bool paused = false;
    uint32_t pos = 0;          /* next sector READ will hand over */
    uint32_t buffered = 0;     /* sectors sitting in the ring */
    uint64_t next_arrival = 0; /* clock at which `buffered` grows by one */
    uint64_t paused_at = 0;
};
StreamState g_st;

/* Ring capacity in sectors. sceCdStInit is the only source for it, so when
 * the game starts a stream without one the capacity is genuinely unknown:
 * say so once and let the ring fill without a ceiling rather than invent a
 * size. Arrival rate and seek latency still apply in that case. */
uint32_t stream_capacity() {
    if (g_st.bufmax) return g_st.bufmax;
    static bool told = false;
    if (!told) {
        told = true;
        rt_log("cdvd", "stream started with no sceCdStInit seen: ring capacity is unknown, "
                       "so the drive is never stalled on a full ring (rate and seek still modeled)");
    }
    return 0xFFFFFFFFu;
}

/* Bring `buffered` up to date with the virtual clock. */
void stream_advance() {
    if (!g_st.running || g_st.paused) return;
    const uint64_t per = sector_cycles();
    if (per == 0) return; /* timing model off; see the READ and STAT cases */
    const uint32_t cap = stream_capacity();
    if (g_st.buffered >= cap) {
        /* Full ring: the drive holds off and picks up again once a read
         * frees room, one sector time later. */
        g_st.next_arrival = rt_clock_now() + per;
        return;
    }
    const uint64_t now = rt_clock_now();
    if (now < g_st.next_arrival) return;
    const uint64_t arrived = (now - g_st.next_arrival) / per + 1;
    const uint64_t room = (uint64_t)cap - g_st.buffered;
    if (arrived >= room) {
        g_st.buffered = cap;
        g_st.next_arrival = now + per;
    } else {
        g_st.buffered += (uint32_t)arrived;
        g_st.next_arrival += arrived * per;
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
    const char* name = cmd < 10 ? names[cmd] : "?";
    /* READ logs its own line below, with what it handed over. Every other
     * command keeps the one-line-per-command form. */
    if (cmd != 2) {
        rt_log("cdvd", "ncmd fno=0x09 sceCdSt%s(lbn=%u n=%u buf=0x%08x)", name, lbn, nsectors, buf);
    }
    uint32_t result = 1;
    switch (cmd) {
        case 5: /* INIT */
            /* sceCdStInit(bufmax, bankmax, iop_buf) is staged as
             * readStreamData[0]=bufmax, [1]=bankmax, [2]=buf (ps2sdk
             * ee/rpc/cdvd/src/ncmd.c sceCdStInit calling sceCdStream), so on
             * this packet the lbn field carries the ring size in sectors and
             * the nsectors field the bank count. Neither is a disc position. */
            g_st = StreamState{};
            g_st.inited = true;
            g_st.bufmax = lbn;
            g_st.bankmax = nsectors;
            g_st.iop_buf = buf;
            rt_log("cdvd", "stream ring: %u sectors (%u KB) in %u banks at IOP 0x%06x; "
                           "first sector %.1f ms after a seek, then one every %.2f ms",
                g_st.bufmax, g_st.bufmax * 2, g_st.bankmax, g_st.iop_buf,
                (double)seek_cycles() * 1000.0 / (double)RT_BUSCLK_HZ,
                (double)sector_cycles() * 1000.0 / (double)RT_BUSCLK_HZ);
            break;
        case 1: case 4: case 9: /* START / SEEK / SEEKF */
            g_st.running = true;
            g_st.paused = false;
            g_st.pos = lbn;
            g_st.buffered = 0;
            g_st.next_arrival = rt_clock_now() + seek_cycles();
            break;
        case 2: { /* READ nsectors at the stream position */
            if (!g_st.running) {
                /* No START/SEEK is live, so there is nothing to hand over.
                 * The EE library short-circuits this case itself (ps2sdk
                 * sceCdStRead returns 0 when streamStatus is 0, before the
                 * RPC), so seeing it here means the stream state machine
                 * here and the game's disagree. Loud, not sampled. */
                rt_log("cdvd", "sceCdStREAD with no stream running (n=%u): returning 0", nsectors);
            }
            stream_advance();
            const bool paced = sector_cycles() != 0;
            uint32_t give = nsectors;
            if (paced && give > g_st.buffered) give = g_st.buffered;
            else if (!paced && !g_st.running) give = 0;
            uint32_t i = 0;
            if (give) {
                static std::vector<uint8_t> stream_buf;
                stream_buf.resize((size_t)give * 2048);
                i = rt_iso_read_sectors(g_st.pos, give, stream_buf.data());
                if (i) rt_gwrite_bytes(buf, stream_buf.data(), (size_t)i * 2048);
                if (i < give) {
                    /* The stream ran off the end of the disc. Loud, not
                     * sampled: with a correct position this cannot happen. */
                    rt_log("cdvd", "stream read short at lbn %u: %u of %u sectors on the disc (%u total)",
                        g_st.pos, i, give, rt_iso_total_sectors());
                }
                g_st.pos += i;
                /* Unpaced (ICORECOMP_CDVD_MBPS=0) the ring is not tracked,
                 * so there is nothing to take out of it. */
                g_st.buffered -= paced ? i : 0;
            }
            result = i; /* sectors read; high half = error (0) */
            /* Not sampled. The loader polls this once a field while it
             * waits, so a stalled load prints one line per field; that line
             * is the only view of why it is waiting and it paces itself. */
            rt_log("cdvd", "ncmd fno=0x09 sceCdStREAD(lbn=%u n=%u buf=0x%08x) "
                           "delivered %u of %u (buffered %u/%u), next lbn %u",
                lbn, nsectors, buf, i, nsectors, g_st.buffered, g_st.bufmax, g_st.pos);
            break;
        }
        case 3: /* STOP: the ring is discarded.
                 * Non-zero is success here: the game latches sceCdGetError
                 * when this returns 0 (decomp ios/cdvd.c func_001331D8 and
                 * func_00131560). Same for START, whose 0 return makes
                 * func_00131480 latch the error. */
            g_st.running = false;
            g_st.paused = false;
            g_st.buffered = 0;
            break;
        case 6: /* STAT */
            /* A sector count, not a flag. ps2sdk
             * common/include/libcdvd-common.h lines 611-615 document it as
             *   "get stream read status
             *    @return number of sectors read if successful, 0 otherwise"
             * and the game settles it: the movie player is the only caller
             * of sceCdStStat in the binary (decomp
             * asm/nonmatchings/src/stage_orient/OtherStagePositionGet.s
             * lines 29-42, a splat file-split artifact holding the ito/mpeg
             * main loop) and it uses the result as a signed threshold: the
             * call is followed immediately by a set-on-less-than against 32,
             * asking whether fewer than 32 sectors are buffered, weighed
             * against the 0x10000 bytes it is about to request, printing
             * "movie pause" and freezing the movie for 30 fields when the
             * ring is that low. Answering a flat 0, which is what this
             * runtime used to do, means that branch is taken on every
             * iteration. It is never compared to zero and never subtracted. */
            stream_advance();
            result = sector_cycles() == 0 ? g_st.bufmax : g_st.buffered;
            break;
        case 7: /* PAUSE: arrival freezes where it is */
            if (g_st.running && !g_st.paused) {
                stream_advance();
                g_st.paused = true;
                g_st.paused_at = rt_clock_now();
            }
            break;
        case 8: /* RESUME: the schedule shifts by the time spent paused */
            if (g_st.paused) {
                const uint64_t now = rt_clock_now();
                if (now > g_st.paused_at) g_st.next_arrival += now - g_st.paused_at;
                g_st.paused = false;
            }
            break;
        default:
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
            {
                const bool busy = rt_clock_now() < g_drive_busy_until;
                const uint32_t st = busy ? SCECdStatRead : SCECdStatPause;
                if (recv_size >= 4) wr32(recv, 0, st);
                static uint64_t polls = 0;
                ++polls;
                if (rt_trace() || (polls & (polls - 1)) == 0) {
                    rt_log("cdvd", "ncmd fno=0x0e drive status poll -> %s (0x%02x) [#%" PRIu64 "]",
                        busy ? "SCECdStatRead" : "SCECdStatPause", st, polls);
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

/* sceSifAllocIopHeap / sceSifFreeIopHeap. Protocol per ps2sdk
 * ee/kernel/src/iopheap.c: fno 1 alloc {u32 size}->{u32 addr}, fno 2 free
 * {u32 addr}->{u32 result}.
 *
 * Real allocations matter here: the EE stages data into these buffers with
 * SifSetDma and later asks IOP-side services to consume them, and the
 * addresses are not opaque to the game (the MPEG helper rejects any IOP
 * address above 0x1FFFFF, decomp src/cod/vendor_258CC0.c func_0025E050).
 *
 * On hardware the IOP side of this is AllocSysMemory(SMEM_LOW, size, 0):
 * lowest-address-first fit, 256-byte granularity, out of the RAM left above
 * the loaded modules, and FreeSysMemory really releases the block. This
 * models the same behaviour over the span below. The base address is a
 * runtime choice, not a measured fact: the hardware base depends on which
 * IOP modules this title loads and that layout is not known here. The 2 MB
 * ceiling is a hardware fact. The full IOP address map is in rpc.h. */
constexpr uint32_t kIopHeapBase = 0x00090000u;
constexpr uint32_t kIopHeapEnd = RT_IOP_RAM_SIZE;
constexpr uint32_t kIopHeapGran = 256u;

static_assert(RT_SIF_IOP_CMDBUF < kIopHeapBase,
    "the sifcmd command buffer sits inside the iopheap heap");

/* Live blocks, kept sorted by address. This is the allocator's whole state:
 * free space is whatever no live block covers, so a free coalesces with its
 * neighbours by construction and the freed space is immediately reusable as
 * part of a larger gap.
 *
 * It is also the record a transfer aimed at one of these buffers is checked
 * against. Without it a disc read that asks for more than the buffer holds
 * walks straight through whatever the game allocated next.
 *
 * `size` is what the game asked for and `span` is that rounded up to the
 * allocation granularity: placement uses span, the overrun checks use the
 * size the game actually asked for. */
struct IopAlloc { uint32_t addr, size, span; };
constexpr int kMaxIopAllocs = 64;
IopAlloc g_iop_allocs[kMaxIopAllocs];
int g_iop_alloc_count = 0;

/* Bytes from `addr` to the end of the allocation containing it, or 0 when
 * `addr` is not inside a tracked allocation. */
uint32_t iop_alloc_room(uint32_t addr) {
    for (int i = 0; i < g_iop_alloc_count; ++i) {
        const IopAlloc& a = g_iop_allocs[i];
        if (addr >= a.addr && addr < a.addr + a.size) {
            return a.addr + a.size - addr;
        }
    }
    return 0;
}

/* Heap the blocks actually occupy, granularity rounding included. */
uint32_t iop_heap_live() {
    uint32_t live = 0;
    for (int i = 0; i < g_iop_alloc_count; ++i) live += g_iop_allocs[i].span;
    return live;
}

uint32_t iop_heap_largest_free() {
    uint32_t best = 0;
    uint32_t cursor = kIopHeapBase;
    for (int i = 0; i < g_iop_alloc_count; ++i) {
        const IopAlloc& a = g_iop_allocs[i];
        if (a.addr - cursor > best) best = a.addr - cursor;
        cursor = a.addr + a.span;
    }
    if (kIopHeapEnd - cursor > best) best = kIopHeapEnd - cursor;
    return best;
}

void iop_heap_dump() {
    rt_log("iopheap", "live block list: %d blocks, %u bytes",
        g_iop_alloc_count, iop_heap_live());
    for (int i = 0; i < g_iop_alloc_count; ++i) {
        rt_log("iopheap", "  block %d: 0x%06x size %u (span %u)",
            i, g_iop_allocs[i].addr, g_iop_allocs[i].size, g_iop_allocs[i].span);
    }
}

/* Lowest-address first fit, the AllocSysMemory(SMEM_LOW) rule. Returns 0
 * (NULL) when nothing fits, which is what the EE library hands the game. */
uint32_t iop_heap_alloc(uint32_t size) {
    if (size == 0) {
        /* What the IOP kernel returns for a zero-byte AllocSysMemory is not
         * verified here, and the game never asks for one. */
        rt_log("iopheap", "WARNING: alloc(0) requested; the IOP kernel's answer to a "
            "zero-byte allocation is unverified. Returning NULL.");
        return 0;
    }
    if (g_iop_alloc_count >= kMaxIopAllocs) {
        iop_heap_dump();
        rt_fatal("iopheap", rt_sched_current_ctx(),
            "live block table full (%d blocks) allocating %u bytes: another block "
            "cannot be tracked, and returning an untracked address would silently "
            "disable the transfer bounds checks that depend on this list",
            kMaxIopAllocs, size);
    }
    uint32_t span = (size + (kIopHeapGran - 1)) & ~(kIopHeapGran - 1);
    uint32_t cursor = kIopHeapBase;
    int slot = -1;
    for (int i = 0; i < g_iop_alloc_count; ++i) {
        const IopAlloc& a = g_iop_allocs[i];
        if (a.addr - cursor >= span) { slot = i; break; }
        cursor = a.addr + a.span;
    }
    if (slot < 0) {
        if (kIopHeapEnd - cursor < span) return 0;
        slot = g_iop_alloc_count;
    }
    for (int i = g_iop_alloc_count; i > slot; --i) g_iop_allocs[i] = g_iop_allocs[i - 1];
    g_iop_allocs[slot] = IopAlloc{cursor, size, span};
    ++g_iop_alloc_count;
    return cursor;
}

/* 0 on success. -1 when `addr` is not the base of a live block: the IOP
 * returns a failure for an invalid block, and iopheap.c passes the IOP
 * result straight back. The exact failure constant is unverified here (no
 * ps2sdk tree to read), so -1 is the reported value. */
int iop_heap_free(uint32_t addr) {
    for (int i = 0; i < g_iop_alloc_count; ++i) {
        if (g_iop_allocs[i].addr != addr) continue;
        for (int j = i; j + 1 < g_iop_alloc_count; ++j) g_iop_allocs[j] = g_iop_allocs[j + 1];
        --g_iop_alloc_count;
        return 0;
    }
    return -1;
}

void svc_iopheap(uint32_t fno, const uint8_t* send, uint32_t send_size,
                 uint8_t* recv, uint32_t recv_size) {
    switch (fno) {
        case 1: { /* alloc */
            uint32_t size = send_size >= 4 ? rd32(send, 0) : 0;
            uint32_t addr = iop_heap_alloc(size);
            if (recv_size >= 4) wr32(recv, 0, addr);
            rt_log("iopheap", "alloc(%u) -> 0x%06x (live %u bytes, largest free %u)",
                size, addr, iop_heap_live(), iop_heap_largest_free());
            if (!addr && size) {
                rt_log("iopheap", "WARNING: virtual IOP heap exhausted (%u byte request)", size);
                /* Once per run: enough to see a leak, not enough to bury the
                 * log if the game retries in a loop. */
                static bool dumped = false;
                if (!dumped) { dumped = true; iop_heap_dump(); }
            }
            break;
        }
        case 2: { /* free */
            uint32_t addr = send_size >= 4 ? rd32(send, 0) : 0;
            int result = iop_heap_free(addr);
            if (recv_size >= 4) wr32(recv, 0, (uint32_t)result);
            if (result == 0) {
                rt_log("iopheap", "free(0x%06x) -> 0 (live %u bytes)", addr, iop_heap_live());
            } else {
                rt_log("iopheap", "WARNING free(0x%06x) -> -1: not the base of a live block. "
                    "The game does not do this on hardware, so either an address was "
                    "corrupted or a block was freed twice.", addr);
                iop_heap_dump();
            }
            break;
        }
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
