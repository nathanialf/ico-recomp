/* gs/render/gif_decode_selftest.cpp: icorecomp-gif-decode-selftest.
 *
 * Exercises gif_decode.h against packets this file builds, so it needs no
 * GPU, no disc and no ROM-derived data. The packets are the shapes the game
 * actually sends (an A+D register block, a PACKED vertex loop, a REGLIST
 * vertex loop, and an IMAGE upload), plus the cases that are easy to get
 * wrong and impossible to see in a picture: the Q latch, a packet split
 * across submissions, and the register shapes hw/geomcheck.cpp relies on now
 * that it decodes through this header rather than through a copy of it.
 *
 * Exit status 0 when every case passed, 1 otherwise.
 */
#include "gif_decode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void fail(const char* what, const std::string& got, const std::string& want) {
    std::printf("FAIL %s\n  got:  %s\n  want: %s\n", what, got.c_str(), want.c_str());
    ++g_failures;
}

/* Records what the decoder produced, as text, so a mismatch prints as one
 * readable line rather than as two vectors of integers. */
struct Recorder {
    std::string out;
    uint32_t image_qwords = 0;
    uint32_t notes = 0;

    void reg(uint32_t addr, uint64_t value) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "r%02x=%016llx ", addr, (unsigned long long)value);
        out += buf;
    }
    void image(const uint8_t* qw, uint32_t qwords) {
        image_qwords += qwords;
        char buf[64];
        /* The first byte of the payload is enough to prove the right bytes
         * arrived in the right order across a split. */
        std::snprintf(buf, sizeof(buf), "img%u:%02x ", qwords, qw[0]);
        out += buf;
    }
    void note(const char*) { ++notes; }
};

/* Packet builder. Everything is little-endian qwords, as the GIF sees them. */
struct Packet {
    std::vector<uint8_t> bytes;

    void qword(uint64_t lo, uint64_t hi) {
        const size_t at = bytes.size();
        bytes.resize(at + 16);
        std::memcpy(bytes.data() + at, &lo, 8);
        std::memcpy(bytes.data() + at + 8, &hi, 8);
    }
    void tag(uint32_t nloop, bool eop, bool pre, uint32_t prim, uint32_t flg,
             uint32_t nreg, uint64_t regs) {
        const uint64_t lo = (uint64_t)(nloop & 0x7FFF)
                          | ((uint64_t)(eop ? 1 : 0) << 15)
                          | ((uint64_t)(pre ? 1 : 0) << 46)
                          | ((uint64_t)(prim & 0x7FF) << 47)
                          | ((uint64_t)(flg & 3) << 58)
                          | ((uint64_t)(nreg & 15) << 60);
        qword(lo, regs);
    }
    uint32_t qwords() const { return (uint32_t)(bytes.size() / 16); }
};

std::string decode_all(const Packet& p, uint32_t chunk_qwords) {
    gsr::GifDecodeState st;
    Recorder rec;
    uint32_t i = 0;
    while (i < p.qwords()) {
        const uint32_t take = std::min(chunk_qwords, p.qwords() - i);
        gsr::gif_decode(st, p.bytes.data() + (size_t)i * 16, take, rec);
        i += take;
    }
    return rec.out;
}

/* A+D: one register per loop, REGS[0] = 0xE. This is the shape the SDK's
 * register-setting helpers emit and the bulk of what the game sends. */
void test_ad() {
    Packet p;
    p.tag(3, true, false, 0, 0, 1, 0xEull);
    p.qword(0x0000000000000042ull, 0x50); /* BITBLTBUF (0x50) */
    p.qword(0x0000000100000002ull, 0x51); /* TRXPOS (0x51) */
    p.qword(0x00000000000000FFull, 0x00); /* PRIM (0x00) through A+D */
    const std::string want =
        "r50=0000000000000042 r51=0000000100000002 r00=00000000000000ff ";
    const std::string got = decode_all(p, 64);
    if (got != want) fail("A+D block", got, want);
}

/* PACKED with PRE, ST latching Q into the following RGBAQ, and a vertex with
 * the ADC bit set, which must land on XYZ3 rather than XYZ2. */
void test_packed() {
    Packet p;
    /* REGS: ST(2), RGBAQ(1), XYZ2(5), XYZ2(5) -> 0x5512 read from the low
     * nibble upwards. */
    p.tag(1, true, true, 0x0004, 0, 4, 0x5512ull);
    /* ST: S and T in the low qword, Q in the low word of the high qword. */
    p.qword(0x3F0000003E800000ull, 0x40000000ull);
    /* RGBAQ: R=0x11 G=0x22 B=0x33 A=0x44, Q from the ST above. */
    p.qword(0x0000002200000011ull, 0x0000004400000033ull);
    /* XYZ2 at (0x0100, 0x0200), Z = 0x30, no ADC. */
    p.qword(0x0000020000000100ull, 0x0000000000000030ull);
    /* The same vertex with ADC (bit 111) set. */
    p.qword(0x0000020000000100ull, 0x0000800000000030ull);
    const std::string want =
        "r00=0000000000000004 "                 /* PRE */
        "r02=3f0000003e800000 "                 /* ST */
        "r01=4000000044332211 "                 /* RGBAQ with the latched Q */
        "r05=0000003002000100 "                 /* XYZ2 */
        "r0d=0000003002000100 ";                /* XYZ3, from the ADC bit */
    const std::string got = decode_all(p, 64);
    if (got != want) fail("PACKED vertex loop", got, want);
}

/* REGLIST: two raw register values per qword, and an odd total leaving the
 * high half of the last qword as padding. */
void test_reglist() {
    Packet p;
    /* REGS: XYZ2(5), RGBAQ(1), NOP(0xF) -> 0xF15. NLOOP 1 gives 3 values,
     * so the third qword's high half is padding. */
    p.tag(1, true, false, 0, 1, 3, 0xF15ull);
    p.qword(0x1111111111111111ull, 0x2222222222222222ull);
    p.qword(0x3333333333333333ull, 0xDEADBEEFDEADBEEFull); /* high half is padding */
    const std::string want =
        "r05=1111111111111111 r01=2222222222222222 ";
    const std::string got = decode_all(p, 64);
    if (got != want) fail("REGLIST with padding", got, want);
}

/* IMAGE: NLOOP counts payload qwords, not register values. */
void test_image() {
    Packet p;
    p.tag(2, true, false, 0, 2, 1, 0);
    p.qword(0x00000000000000AAull, 0);
    p.qword(0x00000000000000BBull, 0);
    const std::string want = "img2:aa ";
    const std::string got = decode_all(p, 64);
    if (got != want) fail("IMAGE payload", got, want);
}

/* The decoder's state has to survive a submission boundary anywhere in the
 * packet, because PATH3 splits packets across DMA kicks. Decoding the same
 * bytes one qword at a time must produce exactly the stream a single call
 * does, except that the image payload arrives in more pieces. */
void test_split() {
    Packet p;
    p.tag(2, false, false, 0, 0, 2, 0x0Eull | (0x0Eull << 4));
    p.qword(0x0000000000001111ull, 0x50);
    p.qword(0x0000000000002222ull, 0x51);
    p.qword(0x0000000000003333ull, 0x52);
    p.qword(0x0000000000004444ull, 0x53);
    p.tag(1, true, false, 0, 1, 2, 0x51ull);
    p.qword(0x5555555555555555ull, 0x6666666666666666ull);

    const std::string whole = decode_all(p, 64);
    for (uint32_t chunk = 1; chunk <= 4; ++chunk) {
        const std::string split = decode_all(p, chunk);
        if (split != whole) {
            char what[64];
            std::snprintf(what, sizeof(what), "split at %u qwords", chunk);
            fail(what, split, whole);
        }
    }
    if (whole.empty()) fail("split baseline", whole, "(non-empty)");
}

/* An IMAGE payload split across submissions has to arrive in order and in
 * full. Counted rather than compared as text, because the number of image()
 * calls legitimately differs with the chunk size. */
void test_image_split() {
    Packet p;
    p.tag(5, true, false, 0, 2, 1, 0);
    for (uint32_t i = 0; i < 5; ++i) p.qword(0xA0 + i, 0);

    gsr::GifDecodeState st;
    Recorder rec;
    uint32_t at = 0;
    while (at < p.qwords()) {
        const uint32_t take = std::min<uint32_t>(2, p.qwords() - at);
        gsr::gif_decode(st, p.bytes.data() + (size_t)at * 16, take, rec);
        at += take;
    }
    if (rec.image_qwords != 5) {
        char got[64];
        std::snprintf(got, sizeof(got), "%u", rec.image_qwords);
        fail("IMAGE split payload qwords", got, "5");
    }
}

/* The shapes hw/geomcheck.cpp used to decode for itself, now that it is a
 * sink over this header. Each one is a place its private walker and this
 * decoder could have disagreed, and none of the cases above covered them:
 *
 *   the ADC bit exists only in the PACKED layout, so an A+D or REGLIST XYZ2
 *   whose bit 47 is set is a Z bit and still kicks;
 *   a PACKED XYZF2 with ADC lands on XYZF3, not just XYZ2 on XYZ3;
 *   XYOFFSET_1 and XYOFFSET_2 arrive through A+D and nothing else;
 *   a PACKED PRIM descriptor delivers eleven bits.
 */
void test_geomcheck_shapes() {
    Packet p;

    /* PACKED: PRIM(0), XYZF2(4) with ADC. REGS 0x40. */
    p.tag(1, false, false, 0, 0, 2, 0x40ull);
    p.qword(0xFFFFFFFFFFFFFFFFull, 0);   /* PRIM, eleven bits of it */
    /* XYZF2 at (0x0100, 0x0200), Z 0x30, F 0x77, ADC set at bit 111. */
    p.qword(0x0000020000000100ull, 0x0000877000000300ull);

    /* A+D: an XYZ2 register value whose bit 47 is set, which is a Z bit in
     * this layout and not ADC, then the two XYOFFSETs. */
    p.tag(3, false, false, 0, 0, 1, 0xEull);
    p.qword(0x0000800002000100ull, 0x05);
    p.qword(0x0000005000000040ull, 0x18);
    p.qword(0x0000006000000050ull, 0x19);

    /* REGLIST: XYZ2(5) then XYZ3(0xD), REGS 0xD5. The first value has bit 47
     * set for the same reason. */
    p.tag(1, true, false, 0, 1, 2, 0xD5ull);
    p.qword(0x0000800002000100ull, 0x0000000002000100ull);

    const std::string want =
        "r00=00000000000007ff "                 /* PRIM, masked to 11 bits */
        "r0c=7700003002000100 "                 /* XYZF3, from the ADC bit */
        "r05=0000800002000100 "                 /* A+D XYZ2: bit 47 is Z */
        "r18=0000005000000040 "                 /* XYOFFSET_1 */
        "r19=0000006000000050 "                 /* XYOFFSET_2 */
        "r05=0000800002000100 "                 /* REGLIST XYZ2: bit 47 is Z */
        "r0d=0000000002000100 ";                /* REGLIST XYZ3 */
    const std::string got = decode_all(p, 64);
    if (got != want) fail("geomcheck register shapes", got, want);
}

} // namespace

int main() {
    test_ad();
    test_packed();
    test_reglist();
    test_image();
    test_split();
    test_image_split();
    test_geomcheck_shapes();
    if (g_failures) {
        std::printf("gif-decode-selftest: %d failures\n", g_failures);
        return 1;
    }
    std::printf("gif-decode-selftest: pass.\n");
    return 0;
}
