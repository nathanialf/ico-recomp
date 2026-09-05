/* hw/gif_widescreen_selftest.cpp: the widescreen 2D transform in hw/gif.cpp,
 * on hand-built GIF packets.
 *
 * Ours (MIT). No guest bytes: every packet here is written by this file from
 * the public GIF and GS register layouts (the GS User's Manual's GIFtag and
 * packed-format chapters, and ps2tek), the same sources hw/gif.cpp names.
 *
 * What it pins down is the part of display.widescreen that cannot be checked
 * by reading the picture: that the classification rule keeps its hands off
 * everything it is supposed to, and that the coordinates it does rewrite are
 * exactly the 12.4 values the arithmetic gives, rounding included. The rule
 * itself, which packets in this game are 2D, is a measurement question and
 * is stated as provisional in hw/gif.cpp.
 *
 * It links hw/gif.cpp itself rather than a copy of the transform, so what is
 * tested is the code the runtime runs, driven through rt_gif_submit. The
 * runtime services gif.cpp needs are stubbed below, the arrangement the VIF1
 * and GS ring selftests already use.
 */
#include "hw.h"

#include "../gs/gs_backend.h"
#include "../guest/widescreen.h"
#include "../runtime.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* ---- runtime service stubs ------------------------------------------------ */

namespace {

bool g_quiet = true;

void emit(const char* level, const char* component, const char* fmt, va_list ap) {
    if (g_quiet) return;
    std::fprintf(stderr, "[%s] %s: ", level, component ? component : "?");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
}

/* The factor and the generation the transform reads. Set by each case. */
double g_factor = 1.0;
uint64_t g_generation = 0;

/* Everything rt_gif_submit hands the backend, kept so a case can compare the
 * bytes that were actually drawn against the bytes it built. */
struct CapturedBackend : GsBackend {
    std::vector<uint8_t> last;
    int last_path = -1;

    void submit_gif(int path, const uint8_t* data, uint32_t qwords) override {
        last_path = path;
        last.assign(data, data + (size_t)qwords * 16);
    }
    void write_priv(uint32_t, uint64_t) override {}
    bool vsync(unsigned) override { return false; }
    uint64_t read_priv(uint32_t) override { return 0; }
};

CapturedBackend g_backend;

int g_failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", what);
}

void check_eq(uint32_t got, uint32_t want, const char* what) {
    if (got == want) return;
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s: got %u, want %u\n", what, got, want);
}

} // namespace

void rt_log_error(const char* c, const char* f, ...) { va_list a; va_start(a, f); emit("error", c, f, a); va_end(a); }
void rt_log_warn(const char* c, const char* f, ...) { va_list a; va_start(a, f); emit("warn", c, f, a); va_end(a); }
void rt_log_info(const char* c, const char* f, ...) { va_list a; va_start(a, f); emit("info", c, f, a); va_end(a); }
void rt_log_debug(const char* c, const char* f, ...) { va_list a; va_start(a, f); emit("debug", c, f, a); va_end(a); }
bool rt_verbose(const char*) { return false; }
bool rt_trace() { return false; }

void rt_fatal(const char* component, const R5900Context*, const char* fmt, ...) {
    std::fprintf(stderr, "[fatal] %s: ", component ? component : "?");
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::exit(1);
}

/* The geometry diagnostic and the VU1 runtime are not part of what is being
 * tested. rt_geom_scan is never reached at all: hw/gif.cpp's call to it is
 * inside #ifdef ICORECOMP_GEOM_CHECK, which this target does not define.
 * The stub stays so that a build which does define it still links. */
void rt_geom_scan(int, const uint8_t*, uint32_t, uint32_t) {}

/* The run-state trail (host/run_state.cpp). rt_gif_submit marks each packet
 * so a crash report can say which one was in flight; nothing here reads it,
 * and linking the real one would pull the watchdog thread into a test that
 * has no run to watch. */
void rt_run_note_gif() {}
uint32_t rt_vu1_bound_hash() { return 0; }

GsBackend* rt_gs_backend() { return &g_backend; }

double rt_widescreen_factor() { return g_factor; }
uint64_t rt_widescreen_generation() { return g_generation; }

/* ---- packet construction -------------------------------------------------- */

namespace {

/* GS register addresses, and the vertex geometry the cases are built at.
 * OFX is the game's own arrangement: the picture centred on the GS window
 * centre at 2048, so a 512-wide buffer starts at 2048 - 256. */
constexpr uint64_t kAdPrim      = 0x00;
constexpr uint64_t kAdXyz2      = 0x05;
constexpr uint64_t kAdXyoffset1 = 0x18;
constexpr uint64_t kAdPrmodecont = 0x1A;
constexpr uint64_t kAdScissor1  = 0x40;

constexpr int32_t kCentre = 2048 * 16;
constexpr int32_t kOfx = (2048 - 256) * 16;
constexpr uint32_t kScax0 = 0;
constexpr uint32_t kScax1 = 511;

/* PRIM words. FST is bit 8, CTXT bit 9; context 1 is CTXT 0, which is what
 * these use, so XYOFFSET_1 and SCISSOR_1 are the ones that matter. */
constexpr uint64_t kPrimSprite = 6;
constexpr uint64_t kPrimTriStripFst = 4 | (1u << 8);

struct Packet {
    std::vector<uint8_t> bytes;

    void qword(uint64_t lo, uint64_t hi) {
        const size_t at = bytes.size();
        bytes.resize(at + 16);
        std::memcpy(bytes.data() + at, &lo, 8);
        std::memcpy(bytes.data() + at + 8, &hi, 8);
    }
    uint32_t qwords() const { return (uint32_t)(bytes.size() / 16); }
};

/* A GIFtag. flg 0 is PACKED, nreg 1..16, regs is the descriptor list. */
uint64_t giftag_lo(uint32_t nloop, bool eop, uint32_t flg, uint32_t nreg) {
    return (uint64_t)(nloop & 0x7FFF) | ((uint64_t)(eop ? 1 : 0) << 15)
         | ((uint64_t)flg << 58) | ((uint64_t)(nreg & 15) << 60);
}

/* One A+D write: the value in the low half, the register address in the
 * high half. */
void ad(Packet& p, uint64_t addr, uint64_t value) { p.qword(value, addr); }

/* Opens an A+D tag of `n` writes. */
void ad_tag(Packet& p, uint32_t n, bool eop) {
    p.qword(giftag_lo(n, eop, 0, 1), 0x0E);
}

uint64_t xyz2_value(int32_t x, int32_t y, uint32_t z) {
    return (uint64_t)(uint16_t)x | ((uint64_t)(uint16_t)y << 16) | ((uint64_t)z << 32);
}

uint64_t xyoffset_value(int32_t ofx, int32_t ofy) {
    return (uint64_t)(uint16_t)ofx | ((uint64_t)(uint16_t)ofy << 32);
}

uint64_t scissor_value(uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1) {
    return (uint64_t)x0 | ((uint64_t)x1 << 16) | ((uint64_t)y0 << 32) | ((uint64_t)y1 << 48);
}

/* The register state every case starts from, as one A+D tag with no EOP so
 * the vertices can follow in their own tag. */
void prologue(Packet& p) {
    ad_tag(p, 4, false);
    ad(p, kAdPrmodecont, 1);
    ad(p, kAdXyoffset1, xyoffset_value(kOfx, (2048 - 224) * 16));
    ad(p, kAdScissor1, scissor_value(kScax0, kScax1, 0, 447));
    ad(p, kAdPrim, kPrimSprite);
}

/* Reads back the X field of the vertex at qword index `qw` of what the
 * backend received. X is bits 0..15 of the low half in every layout these
 * cases use. */
int32_t got_x(uint32_t qw) {
    uint16_t x = 0;
    std::memcpy(&x, g_backend.last.data() + (size_t)qw * 16, sizeof(x));
    return (int32_t)x;
}

/* Runs one packet through the real entry point with the factor set. */
void submit(int path, const Packet& p, double k) {
    g_factor = k;
    ++g_generation;   /* a fresh shadow for every case */
    g_backend.last.clear();
    rt_gif_submit(path, p.bytes.data(), p.qwords());
}

/* x scaled about the GS centre by k, as the transform computes it. Written
 * out rather than called, so the test states the arithmetic instead of
 * borrowing it. */
int32_t expect_scaled(int32_t x, double k) {
    const double d = (double)(x - kCentre) * k;
    const long r = d >= 0.0 ? (long)(d + 0.5) : -(long)(-d + 0.5);
    return kCentre + (int32_t)r;
}

} // namespace

int main() {
    const double k = 0.75;   /* 16:9 from 4:3 */

    /* 1. A sprite well inside the scissor, on PATH3, in A+D form: both
     *    corners scale about the centre. */
    {
        Packet p;
        prologue(p);
        const int32_t x0 = kOfx + 100 * 16, x1 = kOfx + 200 * 16;
        ad_tag(p, 2, true);
        ad(p, kAdXyz2, xyz2_value(x0, kOfx + 50 * 16, 0x1000));
        ad(p, kAdXyz2, xyz2_value(x1, kOfx + 80 * 16, 0x1000));
        submit(2, p, k);
        check_eq((uint32_t)got_x(6), (uint32_t)expect_scaled(x0, k), "inside sprite, left edge");
        check_eq((uint32_t)got_x(7), (uint32_t)expect_scaled(x1, k), "inside sprite, right edge");
        check(got_x(6) != x0, "inside sprite was actually transformed");
    }

    /* 2. The rounding case. The two edges are 6/16 of a pixel either side of
     *    the centre, so each scales to a half and each rounds away from
     *    zero: the pair comes out 10/16 apart where the exact answer is
     *    9/16. That 1/16 is real and is not hidden. */
    {
        Packet p;
        prologue(p);
        const int32_t x0 = kCentre - 6, x1 = kCentre + 6;
        ad_tag(p, 2, true);
        ad(p, kAdXyz2, xyz2_value(x0, kOfx + 50 * 16, 0x1000));
        ad(p, kAdXyz2, xyz2_value(x1, kOfx + 80 * 16, 0x1000));
        submit(2, p, k);
        check_eq((uint32_t)got_x(6), (uint32_t)(kCentre - 5), "rounding, left edge");
        check_eq((uint32_t)got_x(7), (uint32_t)(kCentre + 5), "rounding, right edge");
        check_eq((uint32_t)(got_x(7) - got_x(6)), 10, "rounded width, exact answer is 9");
    }

    /* 3. A sprite spanning the whole scissor width is a full-frame pass and
     *    is left exactly as the game wrote it. */
    {
        Packet p;
        prologue(p);
        const int32_t x0 = kOfx + (int32_t)kScax0 * 16;
        const int32_t x1 = kOfx + (int32_t)(kScax1 + 1) * 16;
        ad_tag(p, 2, true);
        ad(p, kAdXyz2, xyz2_value(x0, kOfx, 0x1000));
        ad(p, kAdXyz2, xyz2_value(x1, kOfx + 448 * 16, 0x1000));
        submit(2, p, k);
        check_eq((uint32_t)got_x(6), (uint32_t)x0, "full-frame sprite, left edge untouched");
        check_eq((uint32_t)got_x(7), (uint32_t)x1, "full-frame sprite, right edge untouched");
    }

    /* 4. A flat triangle strip on PATH3 with FST=1 is transformed; the same
     *    strip with one vertex at a different Z is projected geometry and is
     *    not. */
    {
        const int32_t xs[3] = {kOfx + 100 * 16, kOfx + 150 * 16, kOfx + 200 * 16};
        for (int mixed = 0; mixed < 2; ++mixed) {
            Packet p;
            prologue(p);
            ad_tag(p, 4, true);
            ad(p, kAdPrim, kPrimTriStripFst);
            for (int v = 0; v < 3; ++v) {
                ad(p, kAdXyz2, xyz2_value(xs[v], kOfx + 50 * 16, mixed && v == 2 ? 0x2000u : 0x1000u));
            }
            submit(2, p, k);
            for (int v = 0; v < 3; ++v) {
                const int32_t want = mixed ? xs[v] : expect_scaled(xs[v], k);
                check_eq((uint32_t)got_x(7 + (uint32_t)v), (uint32_t)want,
                         mixed ? "mixed-Z strip untouched" : "flat FST strip transformed");
            }
        }
    }

    /* 5. The same flat strip on PATH1 is VU1's own output and is never
     *    touched, whatever its extent. */
    {
        const int32_t xs[3] = {kOfx + 100 * 16, kOfx + 150 * 16, kOfx + 200 * 16};
        Packet p;
        prologue(p);
        ad_tag(p, 4, true);
        ad(p, kAdPrim, kPrimTriStripFst);
        for (int v = 0; v < 3; ++v) ad(p, kAdXyz2, xyz2_value(xs[v], kOfx + 50 * 16, 0x1000));
        submit(0, p, k);
        for (int v = 0; v < 3; ++v) {
            check_eq((uint32_t)got_x(7 + (uint32_t)v), (uint32_t)xs[v], "PATH1 strip untouched");
        }
    }

    /* 6. A sprite on PATH1 is transformed: the game's own menu item quad
     *    reaches the GS through gif_StartPacketPath1 (guest/menu_nav.cpp),
     *    and a sprite is two screen-space corners whatever path carried it. */
    {
        Packet p;
        prologue(p);
        const int32_t x0 = kOfx + 100 * 16, x1 = kOfx + 200 * 16;
        ad_tag(p, 2, true);
        ad(p, kAdXyz2, xyz2_value(x0, kOfx + 50 * 16, 0x1000));
        ad(p, kAdXyz2, xyz2_value(x1, kOfx + 80 * 16, 0x1000));
        submit(0, p, k);
        check_eq((uint32_t)got_x(6), (uint32_t)expect_scaled(x0, k), "PATH1 sprite transformed");
        check_eq((uint32_t)got_x(7), (uint32_t)expect_scaled(x1, k), "PATH1 sprite, right edge");
    }

    /* 7. The same inside sprite in PACKED XYZ2 form rather than A+D, so the
     *    other vertex layout is covered. PACKED XYZ2 carries X in bits 0..15
     *    of the low half, Y in bits 32..47, and Z in the whole low word of
     *    the high half. */
    {
        Packet p;
        prologue(p);
        const int32_t x0 = kOfx + 100 * 16, x1 = kOfx + 200 * 16;
        p.qword(giftag_lo(2, true, 0, 1), 0x05);
        p.qword((uint64_t)(uint16_t)x0 | ((uint64_t)(uint16_t)(kOfx + 50 * 16) << 32), 0x1000);
        p.qword((uint64_t)(uint16_t)x1 | ((uint64_t)(uint16_t)(kOfx + 80 * 16) << 32), 0x1000);
        submit(2, p, k);
        check_eq((uint32_t)got_x(6), (uint32_t)expect_scaled(x0, k), "PACKED sprite, left edge");
        check_eq((uint32_t)got_x(7), (uint32_t)expect_scaled(x1, k), "PACKED sprite, right edge");
    }

    /* 8. With the factor off nothing is walked and nothing is copied: the
     *    backend gets the caller's own buffer back, byte for byte. */
    {
        Packet p;
        prologue(p);
        const int32_t x0 = kOfx + 100 * 16, x1 = kOfx + 200 * 16;
        ad_tag(p, 2, true);
        ad(p, kAdXyz2, xyz2_value(x0, kOfx + 50 * 16, 0x1000));
        ad(p, kAdXyz2, xyz2_value(x1, kOfx + 80 * 16, 0x1000));
        submit(2, p, 1.0);
        check(std::memcmp(g_backend.last.data(), p.bytes.data(), p.bytes.size()) == 0,
              "factor 1.0 leaves the packet byte for byte");
    }

    /* 9. A vertex that scales outside the 16-bit X field abandons the whole
     *    primitive, not just the vertices after it. The transform builds a
     *    patch list as it walks and applies it to a copy afterwards, so an
     *    early return part way through a primitive would write some vertices
     *    scaled and the rest as the game wrote them, which is the tearing
     *    the split branch refuses to do.
     *
     *    A factor above 1 (a window narrower than 4:3) with a wide scissor
     *    is the reachable shape: k = 2 puts the third vertex at
     *    2048*16 + (60000 - 2048*16) * 2 = 87232, past 0xFFFF, while the
     *    first two land in range. Every X must come back exactly as
     *    written. */
    {
        const double wide_k = 2.0;
        const int32_t xs[3] = {30000, 33000, 60000};
        Packet p;
        /* The prologue with a scissor wide enough that all three vertices
         * are inside it: 0..2047 in pixels puts the right edge at
         * kOfx + 2048*16 = 61440. */
        ad_tag(p, 4, false);
        ad(p, kAdPrmodecont, 1);
        ad(p, kAdXyoffset1, xyoffset_value(kOfx, (2048 - 224) * 16));
        ad(p, kAdScissor1, scissor_value(0, 2047, 0, 447));
        ad(p, kAdPrim, kPrimSprite);
        ad_tag(p, 4, true);
        ad(p, kAdPrim, kPrimTriStripFst);
        for (int v = 0; v < 3; ++v) {
            ad(p, kAdXyz2, xyz2_value(xs[v], kOfx + 50 * 16, 0x1000));
        }
        /* The first vertex is in range on its own, so a half-applied walk
         * would show up as a changed X here. */
        check(expect_scaled(xs[0], wide_k) != xs[0], "the first vertex would have moved");
        submit(2, p, wide_k);
        for (int v = 0; v < 3; ++v) {
            check_eq((uint32_t)got_x(7 + (uint32_t)v), (uint32_t)xs[v],
                     "out-of-range vertex leaves the whole primitive alone");
        }
        check(std::memcmp(g_backend.last.data(), p.bytes.data(), p.bytes.size()) == 0,
              "no patch from the abandoned primitive reached the packet");
    }

    if (g_failures == 0) {
        std::printf("gif widescreen selftest: all checks passed\n");
        return 0;
    }
    std::printf("gif widescreen selftest: %d check(s) failed\n", g_failures);
    return 1;
}
