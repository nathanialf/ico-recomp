/* hw/vif1_selftest.cpp: differential test of the VIF1 UNPACK rewrite
 * (icorecomp-vif1-selftest).
 *
 * hw/vif1.cpp carries two implementations of UNPACK. exec_unpack is the
 * one the runtime uses; exec_unpack_reference is the implementation it
 * replaced, kept verbatim and compiled in only when
 * ICORECOMP_VIF1_SELFTEST is defined. This binary runs randomised UNPACK
 * commands through both, from identical VIF1 register state and identical
 * VU1 data memory, and compares data memory byte for byte and the STROW
 * register afterwards (STMOD 2 writes back into STROW, so memory alone
 * would not catch a difference there).
 *
 * The space covered per case:
 *   VN 0-3 and VL 0-3, so every S/V2/V3/V4 format at 32, 16 and 8 bits
 *     plus the V4-5 expansion, signed and unsigned
 *   CL and WL 0-255, so skipping (WL <= CL), filling (WL > CL), WL = 0
 *     meaning 256 and the CL = 0 clamp
 *   masked and unmasked, with a random 32-bit STMASK so every 2-bit
 *     selector appears in every cycle and field position
 *   STMOD 0-3, with random STROW and STCOL
 *   NUM 1-256, with the 0-means-256 encoding
 *   ADDR anywhere in 0-0x3FF, weighted towards the top of data memory so
 *     runs cross the 1024-quadword wrap, and FLG on and off with random
 *     TOPS
 *   payloads exactly the size unpack_words_needed asks for, and short
 *     payloads whose missing fields must read as zero
 *
 * A quarter of the cases go through rt_vif1_feed instead of calling
 * exec_unpack directly: the register writes and the UNPACK are assembled
 * into one VIFcode stream and fed in randomly sized chunks, so both the
 * span path (a command whole inside one feed call) and the straddle path
 * (a command split across feed calls) are exercised against the same
 * oracle.
 *
 * Exit code 0 = every case matched. On the first mismatch the case is
 * printed (code word, CL, WL, mask, mode and the rest) and the exit code
 * is 1.
 *
 * Usage: icorecomp-vif1-selftest [cases] [seed]
 */
#include "hw.h"

#include "../ee/kernel.h"

#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/* ---- stubs for the symbols vif1.cpp pulls in ---------------------------- */

extern "C" {
uint8_t* g_pages[0x10000]; /* unused here: no command reaches a hexdump */
}

static bool g_verbose = false;
static uint64_t g_log_lines = 0;

void rt_log(const char* component, const char* fmt, ...) {
    ++g_log_lines;
    if (!g_verbose) return;
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

bool rt_trace() { return false; }
void rt_intc_raise(int) {}
void rt_dmac_dump_recent_tags(int) {}
bool rt_dmac_current_tag(int, uint32_t*, uint32_t*, uint32_t*) { return false; }
void rt_gif_submit(int, const uint8_t*, uint32_t) {}
void rt_ipu_fifo_feed(const uint8_t*) {}

/* Two VU1 images, one per implementation, swapped under rt_vu1_state so a
 * case can be run twice without either run seeing the other's writes. */
static Vu1State g_vu[2];
static int g_vu_sel = 0;
Vu1State* rt_vu1_state() { return &g_vu[g_vu_sel]; }

/* Micro memory, plus the upload bookkeeping vu1rt.cpp does with it. The
 * MPG phase below needs to see exactly what vu1rt would have seen, so
 * these stubs carry vu1rt's rule (its rt_vu1_micro_written and the
 * g_upload_dirty check at the top of rt_vu1_mscal) rather than doing
 * nothing. */
static uint8_t g_micro_shadow[16384];
static uint32_t g_upload_len = 0;
static bool g_upload_dirty = false;
static uint64_t g_bound_len = 0;   /* extent of the last bind */
static uint64_t g_bound_hash = 0;  /* hash the last bind resolved */
static uint64_t g_notify_calls = 0;

uint8_t* rt_vu1_micro() { return g_micro_shadow; }

/* FNV-1a over the uploaded extent. Stands in for rc_vu1_hash: all the test
 * needs is that two different byte ranges hash differently. */
static uint64_t micro_hash(const uint8_t* p, uint32_t len) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < len; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

void rt_vu1_micro_written(uint32_t offset, uint32_t bytes) {
    ++g_notify_calls;
    if (offset == 0) g_upload_len = bytes;
    else if (offset == g_upload_len) g_upload_len = offset + bytes;
    else if (offset + bytes > g_upload_len) g_upload_len = offset + bytes;
    g_upload_dirty = true;
}

void rt_vu1_mscal(uint32_t, uint32_t, uint32_t, const char*) {
    if (!g_upload_dirty) return;
    g_bound_len = g_upload_len;
    g_bound_hash = micro_hash(g_micro_shadow, g_upload_len);
    g_upload_dirty = false;
}

/* ---- hooks into vif1.cpp (ICORECOMP_VIF1_SELFTEST) ---------------------- */

void rt_vif1_selftest_reset();
void rt_vif1_selftest_set_regs(uint32_t cl, uint32_t wl, uint32_t mask, uint32_t mode,
                               const uint32_t row[4], const uint32_t col[4], uint32_t tops);
void rt_vif1_selftest_get_row(uint32_t row[4]);
int rt_vif1_selftest_pending();
uint32_t rt_vif1_selftest_words_needed(uint32_t code);
void rt_vif1_selftest_unpack(uint32_t code, const uint32_t* pay, uint32_t words, int reference);

/* ---- case generation ---------------------------------------------------- */

static uint64_t g_rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng64() {
    uint64_t z = (g_rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static uint32_t rng32() { return (uint32_t)(rng64() >> 32); }

/* Uniform in [0, n). */
static uint32_t rnd(uint32_t n) { return n ? rng32() % n : 0; }

struct Case {
    uint32_t code;
    uint32_t cl, wl, mask, mode, tops;
    uint32_t row[4], col[4];
    uint32_t payload[1024 + 8];
    uint32_t words;      /* words handed to the unpack */
    uint32_t exact;      /* words unpack_words_needed asks for */
    bool     via_feed;
};

static void fill_random(void* dst, size_t bytes) {
    uint8_t* p = static_cast<uint8_t*>(dst);
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        const uint64_t v = rng64();
        std::memcpy(p + i, &v, 8);
    }
    for (; i < bytes; ++i) p[i] = (uint8_t)rng32();
}

/* CL and WL, weighted so the interesting shapes come up often: CL = 0
 * (clamped to 1), WL = 0 (meaning 256), WL > CL (filling), WL == CL (one
 * contiguous run) and WL < CL (skipping). */
static void pick_cycle(uint32_t* cl, uint32_t* wl) {
    switch (rnd(8)) {
        case 0: *cl = 0; *wl = rnd(8); break;             /* CL = 0 clamp */
        case 1: *cl = 1 + rnd(4); *wl = 0; break;         /* WL = 0 => 256 */
        case 2: *cl = 1 + rnd(4); *wl = *cl; break;       /* contiguous */
        case 3: *cl = 1 + rnd(4); *wl = *cl + 1 + rnd(6); break;  /* filling */
        case 4: *cl = 2 + rnd(6); *wl = 1 + rnd(*cl); break;      /* skipping */
        case 5: *cl = rnd(256); *wl = rnd(256); break;    /* anything */
        case 6: *cl = 1; *wl = 1; break;                  /* ICO's usual */
        default: *cl = rnd(16); *wl = rnd(16); break;
    }
    *cl &= 0xFF;
    *wl &= 0xFF;
}

static void pick_case(Case& c) {
    pick_cycle(&c.cl, &c.wl);

    /* STMASK. A uniformly random word already puts all four selectors in
     * every cycle and field slot; the fixed patterns make sure the
     * all-data, all-row, all-column and all-protected cycles are hit
     * often rather than once in 4^16 draws. */
    switch (rnd(6)) {
        case 0: c.mask = 0x00000000u; break;
        case 1: c.mask = 0x55555555u; break;
        case 2: c.mask = 0xAAAAAAAAu; break;
        case 3: c.mask = 0xFFFFFFFFu; break;
        default: c.mask = rng32(); break;
    }
    c.mode = rnd(4); /* 0 offset-none, 1 offset, 2 difference, 3 undefined */
    c.tops = rng32() & 0x3FF;
    for (int i = 0; i < 4; ++i) {
        c.row[i] = rng32();
        c.col[i] = rng32();
    }

    const uint32_t vn = rnd(4);
    const uint32_t vl = rnd(4);
    const uint32_t masked = rnd(2);
    const uint32_t usn = rnd(2);
    const uint32_t flg = rnd(2);
    uint32_t num = rnd(2) ? (1 + rnd(256)) : (1 + rnd(8)); /* small NUM often */
    if (num == 256) num = 0;                               /* 0 encodes 256 */
    uint32_t addr;
    switch (rnd(4)) {
        case 0: addr = 0x3FF - rnd(8); break;   /* against the wrap */
        case 1: addr = 0x400 - rnd(300); break; /* runs that cross it */
        case 2: addr = rnd(8); break;
        default: addr = rng32() & 0x3FF; break;
    }
    c.code = (0x60u | (masked << 4) | (vn << 2) | vl) << 24;
    c.code |= (num & 0xFF) << 16;
    c.code |= flg << 15;
    c.code |= usn << 14;
    c.code |= addr & 0x3FF;

    /* words_needed reads CL/WL out of the register file, so set it first. */
    rt_vif1_selftest_set_regs(c.cl, c.wl, c.mask, c.mode, c.row, c.col, c.tops);
    c.exact = rt_vif1_selftest_words_needed(c.code);
    if (c.exact > 1024) c.exact = 1024; /* cannot happen: 256 * 16 bytes */

    c.via_feed = rnd(4) == 0;
    c.words = c.exact;
    if (!c.via_feed && c.exact > 0 && rnd(8) == 0) {
        /* Short payload: the fields past the end must read as zero. The
         * feed path always delivers exactly words_needed words, so this
         * shape only exists for a direct call. */
        const uint32_t drop = 1 + rnd(c.exact < 4 ? c.exact : 4);
        c.words = c.exact - drop;
    }
    fill_random(c.payload, (size_t)c.exact * 4);
}

/* ---- one case ----------------------------------------------------------- */

/* Assembles the register writes and the UNPACK into one VIFcode stream and
 * feeds it in chunks of 1 to `chunk` words. */
static void run_via_feed(const Case& c, uint32_t chunk) {
    uint32_t stream[1024 + 32];
    uint32_t n = 0;
    stream[n++] = 0x01000000u | ((c.wl & 0xFF) << 8) | (c.cl & 0xFF); /* STCYCL */
    stream[n++] = 0x05000000u | c.mode;                               /* STMOD */
    stream[n++] = 0x20000000u;                                        /* STMASK */
    stream[n++] = c.mask;
    stream[n++] = 0x30000000u;                                        /* STROW */
    for (int i = 0; i < 4; ++i) stream[n++] = c.row[i];
    stream[n++] = 0x31000000u;                                        /* STCOL */
    for (int i = 0; i < 4; ++i) stream[n++] = c.col[i];
    stream[n++] = c.code;
    for (uint32_t i = 0; i < c.words; ++i) stream[n++] = c.payload[i];

    rt_vif1_selftest_reset();
    /* TOPS comes from BASE/OFFSET and the MSCAL buffer flip, not from any
     * VIFcode in this stream, so it is planted directly. */
    const uint32_t zero[4] = {0, 0, 0, 0};
    rt_vif1_selftest_set_regs(0, 0, 0, 0, zero, zero, c.tops);

    uint32_t i = 0;
    while (i < n) {
        uint32_t take = 1 + rnd(chunk);
        if (take > n - i) take = n - i;
        rt_vif1_feed(stream + i, take, 0x00100000u + i * 4);
        i += take;
    }
}

static void print_case(const Case& c, const char* what) {
    const uint32_t cmd = (c.code >> 24) & 0x7F;
    uint32_t num = (c.code >> 16) & 0xFF;
    if (num == 0) num = 256;
    std::printf("MISMATCH (%s)\n", what);
    std::printf("  code=0x%08x cmd=0x%02x vn=%u vl=%u masked=%u usn=%u flg=%u num=%u addr=0x%03x\n",
        c.code, cmd, (cmd >> 2) & 3, cmd & 3, (cmd >> 4) & 1,
        (c.code >> 14) & 1, (c.code >> 15) & 1, num, c.code & 0x3FF);
    std::printf("  cl=%u wl=%u mask=0x%08x mode=%u tops=0x%03x\n",
        c.cl, c.wl, c.mask, c.mode, c.tops);
    std::printf("  row=%08x %08x %08x %08x  col=%08x %08x %08x %08x\n",
        c.row[0], c.row[1], c.row[2], c.row[3],
        c.col[0], c.col[1], c.col[2], c.col[3]);
    std::printf("  payload words=%u (exact=%u)%s\n",
        c.words, c.exact, c.words < c.exact ? " SHORT" : "");
}

/* Returns true when the two implementations agreed. */
static bool run_case(const Case& c) {
    /* Same starting data memory for both runs. */
    fill_random(g_vu[0].mem, sizeof(g_vu[0].mem));
    std::memcpy(g_vu[1].mem, g_vu[0].mem, sizeof(g_vu[0].mem));

    uint32_t row_ref[4], row_new[4];

    /* Reference. */
    g_vu_sel = 0;
    rt_vif1_selftest_reset();
    rt_vif1_selftest_set_regs(c.cl, c.wl, c.mask, c.mode, c.row, c.col, c.tops);
    rt_vif1_selftest_unpack(c.code, c.words ? c.payload : nullptr, c.words, 1);
    rt_vif1_selftest_get_row(row_ref);

    /* Subject. */
    g_vu_sel = 1;
    if (c.via_feed) {
        run_via_feed(c, 1 + rnd(64));
        if (rt_vif1_selftest_pending()) {
            print_case(c, "feed left a command pending");
            return false;
        }
    } else {
        rt_vif1_selftest_reset();
        rt_vif1_selftest_set_regs(c.cl, c.wl, c.mask, c.mode, c.row, c.col, c.tops);
        rt_vif1_selftest_unpack(c.code, c.words ? c.payload : nullptr, c.words, 0);
    }
    rt_vif1_selftest_get_row(row_new);

    if (std::memcmp(g_vu[0].mem, g_vu[1].mem, sizeof(g_vu[0].mem)) != 0) {
        print_case(c, c.via_feed ? "VU1 data memory, via rt_vif1_feed" : "VU1 data memory");
        for (uint32_t qw = 0, shown = 0; qw < 1024 && shown < 8; ++qw) {
            const uint32_t* a = reinterpret_cast<const uint32_t*>(g_vu[0].mem) + qw * 4;
            const uint32_t* b = reinterpret_cast<const uint32_t*>(g_vu[1].mem) + qw * 4;
            if (std::memcmp(a, b, 16) == 0) continue;
            ++shown;
            std::printf("  qw 0x%03x ref %08x %08x %08x %08x  new %08x %08x %08x %08x\n",
                qw, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
        }
        return false;
    }
    if (std::memcmp(row_ref, row_new, sizeof(row_ref)) != 0) {
        print_case(c, "STROW after the unpack");
        std::printf("  ref %08x %08x %08x %08x  new %08x %08x %08x %08x\n",
            row_ref[0], row_ref[1], row_ref[2], row_ref[3],
            row_new[0], row_new[1], row_new[2], row_new[3]);
        return false;
    }
    return true;
}



/* ---- throughput ---------------------------------------------------------
 *
 * Not a check, a measurement: the same UNPACK run through both
 * implementations so the report carries a number rather than a claim. The
 * shapes are the ones a field is made of, per-vector cost being what the
 * vif1 profiler bucket divides by.
 */
static double time_unpack(uint32_t code, const uint32_t* pay, uint32_t words,
                          uint32_t reps, int reference) {
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < reps; ++i) {
        rt_vif1_selftest_unpack(code, pay, words, reference);
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
}

static void bench_phase() {
    static uint32_t pay[1024];
    fill_random(pay, sizeof(pay));
    const uint32_t row[4] = {1, 2, 3, 4};
    const uint32_t col[4] = {5, 6, 7, 8};
    g_vu_sel = 0;

    struct Shape {
        const char* name;
        uint32_t cmd;   /* 0x60 | masked | vn << 2 | vl */
        uint32_t cl, wl, mask, mode;
    };
    static const Shape shapes[] = {
        {"V4-32 unmasked STMOD 0 CL=WL=1", 0x6F, 1, 1, 0, 0},
        {"V4-16 unmasked STMOD 0 CL=WL=1", 0x6D, 1, 1, 0, 0},
        {"V3-32 unmasked STMOD 0 CL=WL=1", 0x6B, 1, 1, 0, 0},
        {"V4-32 masked   STMOD 1 CL=WL=1", 0x7F, 1, 1, 0x3F0FC300u, 1},
        {"V4-32 unmasked STMOD 0 CL=2 WL=1", 0x6F, 2, 1, 0, 0},
    };
    const uint32_t num = 64;
    const uint32_t reps = 20000;
    std::printf("throughput (%u vectors per unpack, %u unpacks each):\n", num, reps);
    for (const Shape& sh : shapes) {
        const uint32_t code = (sh.cmd << 24) | (num << 16);
        rt_vif1_selftest_reset();
        rt_vif1_selftest_set_regs(sh.cl, sh.wl, sh.mask, sh.mode, row, col, 0);
        const uint32_t words = rt_vif1_selftest_words_needed(code);
        const double ref = time_unpack(code, pay, words, reps, 1);
        const double now = time_unpack(code, pay, words, reps, 0);
        const double vectors = (double)reps * num;
        std::printf("  %-34s reference %6.2f ns/vector  now %6.2f ns/vector  (%.2fx)\n",
            sh.name, ref / vectors, now / vectors, now > 0.0 ? ref / now : 0.0);
    }
}

/* ---- MPG phase ----------------------------------------------------------
 *
 * vif1.cpp holds back the rt_vu1_micro_written notification for MPG
 * segments whose bytes are already resident, and settles the held-back run
 * at the next MSCAL. The claim that makes it safe is that the program
 * bound after any MSCAL is the same one the unconditional notification
 * would have bound. This phase checks that claim directly: it runs random
 * MPG and MSCAL sequences through rt_vif1_feed while a second, independent
 * model of the old behaviour (copy and notify on every MPG) runs beside
 * it, and after every MSCAL compares the binding the two resolve, plus
 * micro memory itself.
 */
struct MicroRef {
    uint8_t mem[16384];
    uint32_t len = 0;
    bool dirty = false;
    uint64_t bound_len = 0;
    uint64_t bound_hash = 0;
};

static bool mpg_phase(uint32_t rounds) {
    MicroRef ref;
    std::memset(ref.mem, 0, sizeof(ref.mem));
    std::memset(g_micro_shadow, 0, sizeof(g_micro_shadow));
    g_upload_len = 0;
    g_upload_dirty = false;
    g_bound_len = g_bound_hash = 0;
    g_notify_calls = 0;
    rt_vif1_selftest_reset();

    /* A handful of "microprograms" the sequence re-uploads, the way ICO
     * cycles the same five every field. Lengths are multiples of 8 (one
     * VU instruction pair) and at most micro memory. */
    static uint8_t prog[5][8192];
    uint32_t prog_len[5];
    for (int i = 0; i < 5; ++i) {
        fill_random(prog[i], sizeof(prog[i]));
        prog_len[i] = 8 * (1 + rnd(1024));
    }
    /* Two of them share a prefix, which is the case where holding back the
     * notification could keep a stale extent if the settle rule were
     * wrong. */
    std::memcpy(prog[1], prog[0], sizeof(prog[0]) / 2);
    prog_len[1] = prog_len[0] / 16 * 8;
    if (prog_len[1] == 0) prog_len[1] = 8;

    uint32_t stream[8 + 2048];
    uint64_t mpgs = 0, mscals = 0;

    uint32_t prev_pi = 0;
    for (uint32_t r = 0; r < rounds; ++r) {
        /* Half the rounds re-upload the program that is already resident,
         * which is the shape the deferral is for; the rest switch program,
         * which is the shape where the bytes differ and the notification
         * has to go through. */
        const uint32_t pi = (r != 0 && rnd(2)) ? prev_pi : rnd(5);
        prev_pi = pi;
        const uint32_t total = prog_len[pi];
        /* Upload it in 1 to 4 segments; MPG carries at most 256
         * instructions, so 2048 bytes, per command. */
        uint32_t off = 0;
        const bool from_zero = rnd(8) != 0; /* sometimes start off-origin */
        if (!from_zero) off = 8 * rnd(1 + total / 8);
        while (off < total) {
            uint32_t bytes = 8 * (1 + rnd(256));
            if (bytes > total - off) bytes = total - off;
            uint32_t dst = off & 0x3FFF;
            uint32_t clamped = bytes;
            if (dst + clamped > 16384) clamped = 16384 - dst;

            uint32_t n = 0;
            stream[n++] = 0x4A000000u | ((bytes / 8 == 256 ? 0u : bytes / 8) << 16) | (dst / 8);
            std::memcpy(stream + n, prog[pi] + off, bytes);
            n += bytes / 4;

            /* Feed in random chunks so MPG payloads straddle too. */
            uint32_t i = 0;
            const uint32_t chunk = 1 + rnd(96);
            while (i < n) {
                uint32_t take = 1 + rnd(chunk);
                if (take > n - i) take = n - i;
                rt_vif1_feed(stream + i, take, 0x00200000u + i * 4);
                i += take;
            }

            /* The old behaviour, applied to the same segment. */
            std::memcpy(ref.mem + dst, prog[pi] + off, clamped);
            if (dst == 0) ref.len = clamped;
            else if (dst == ref.len) ref.len = dst + clamped;
            else if (dst + clamped > ref.len) ref.len = dst + clamped;
            ref.dirty = true;
            ++mpgs;

            off += bytes;
            if (rnd(6) == 0) break; /* abandon the upload part way */
        }

        /* MSCAL, MSCALF or MSCNT: all three settle the deferred run. */
        static const uint32_t kStart[3] = {0x14000000u, 0x15000000u, 0x17000000u};
        const uint32_t start = kStart[rnd(3)] | rnd(2048);
        rt_vif1_feed(&start, 1, 0x00300000u);
        if (ref.dirty) {
            ref.bound_len = ref.len;
            ref.bound_hash = micro_hash(ref.mem, ref.len);
            ref.dirty = false;
        }
        ++mscals;

        if (std::memcmp(ref.mem, g_micro_shadow, sizeof(ref.mem)) != 0) {
            std::printf("MISMATCH (micro memory) after round %u, program %u\n", r, pi);
            return false;
        }
        if (ref.bound_len != g_bound_len || ref.bound_hash != g_bound_hash) {
            std::printf("MISMATCH (bound program) after round %u, program %u\n", r, pi);
            std::printf("  old: len=%" PRIu64 " hash=0x%016" PRIx64 "\n", ref.bound_len, ref.bound_hash);
            std::printf("  new: len=%" PRIu64 " hash=0x%016" PRIx64 "\n", g_bound_len, g_bound_hash);
            return false;
        }
    }

    std::printf("MPG: %" PRIu64 " uploads and %" PRIu64 " MSCALs bound identically; "
                "%" PRIu64 " of the uploads still needed a rehash notification\n",
        mpgs, mscals, g_notify_calls);
    return true;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char** argv) {
    uint32_t cases = 200000;
    if (argc > 1) cases = (uint32_t)std::strtoul(argv[1], nullptr, 0);
    if (argc > 2) g_rng_state = std::strtoull(argv[2], nullptr, 0);
    g_verbose = std::getenv("ICORECOMP_VIF1_SELFTEST_VERBOSE") != nullptr;

    /* Coverage tallies, so a green run says what it actually covered. */
    uint64_t by_fmt[16] = {0};
    uint64_t n_masked = 0, n_filling = 0, n_skipping = 0, n_contig = 0;
    uint64_t n_short = 0, n_feed = 0, n_wrap = 0, n_flg = 0, n_usn = 0;
    uint64_t by_mode[4] = {0};
    uint64_t vectors = 0;

    std::printf("vif1 UNPACK differential: %u cases, seed 0x%016" PRIx64 "\n",
        cases, g_rng_state);

    for (uint32_t i = 0; i < cases; ++i) {
        Case c;
        pick_case(c);
        if (!run_case(c)) {
            std::printf("failed at case %u\n", i);
            return 1;
        }

        const uint32_t cmd = (c.code >> 24) & 0x7F;
        const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3;
        uint32_t num = (c.code >> 16) & 0xFF;
        if (num == 0) num = 256;
        uint32_t cl = c.cl, wl = c.wl ? c.wl : 256;
        if (cl == 0) cl = 1;
        ++by_fmt[vn * 4 + vl];
        ++by_mode[c.mode];
        if (cmd & 0x10) ++n_masked;
        if (wl > cl) ++n_filling;
        else if (wl < cl) ++n_skipping;
        else ++n_contig;
        if (c.words < c.exact) ++n_short;
        if (c.via_feed) ++n_feed;
        if ((c.code & 0x3FF) + num > 1024) ++n_wrap;
        if ((c.code >> 15) & 1) ++n_flg;
        if ((c.code >> 14) & 1) ++n_usn;
        vectors += num;
    }

    static const char* const fmt_name[16] = {
        "S-32", "S-16", "S-8", "S-5", "V2-32", "V2-16", "V2-8", "V2-5",
        "V3-32", "V3-16", "V3-8", "V3-5", "V4-32", "V4-16", "V4-8", "V4-5",
    };
    std::printf("all %u cases matched (%" PRIu64 " vectors unpacked)\n", cases, vectors);
    std::printf("formats:");
    for (int i = 0; i < 16; ++i) std::printf(" %s=%" PRIu64, fmt_name[i], by_fmt[i]);
    std::printf("\n");
    std::printf("modes: 0=%" PRIu64 " 1=%" PRIu64 " 2=%" PRIu64 " 3=%" PRIu64 "\n",
        by_mode[0], by_mode[1], by_mode[2], by_mode[3]);
    std::printf("cycle: contiguous=%" PRIu64 " skipping=%" PRIu64 " filling=%" PRIu64 "\n",
        n_contig, n_skipping, n_filling);
    std::printf("masked=%" PRIu64 " flg=%" PRIu64 " usn=%" PRIu64
                " short-payload=%" PRIu64 " crosses-wrap=%" PRIu64 " via-feed=%" PRIu64 "\n",
        n_masked, n_flg, n_usn, n_short, n_wrap, n_feed);
    bench_phase();
    if (!mpg_phase(4000)) {
        std::printf("MPG phase failed\n");
        return 1;
    }
    std::printf("(%" PRIu64 " log lines suppressed; set ICORECOMP_VIF1_SELFTEST_VERBOSE to see them)\n",
        g_log_lines);
    return 0;
}
