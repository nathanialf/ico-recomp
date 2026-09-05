/* snd/tests/pcm_stream_selftest.cpp: the SgStPcm ring geometry, checked
 * against the attract movie's own numbers, and the achievement unlock
 * chime.
 *
 * Both subjects (snd/pcm_stream.h, snd/chime.h) are header only, so this
 * links nothing: no SDL, no runtime services, no IOP RAM. Same shape as
 * host/stick_shape_selftest.cpp.
 *
 * The numbers below are the ones the retail game sends, read out of a Windows
 * run's log (dist/windows/icorecomp.log) and confirmed against
 * audioDecCreate (PAL 0x00257550) in the retail ELF:
 *
 *   cmd 0x48  w1=0x00010400  w2=0x001b0100  w3=0x00006000
 *   cmd 0x48  w1=0x01010400  w2=0x001b0300  w3=0x00006000
 *
 * That is two channels 0x200 apart inside one 0x6000 byte ring with a 0x400
 * interleave block, so channel 0 owns [0x000,0x200) of every block and
 * channel 1 owns [0x200,0x400).
 *
 * Besides the movie's own case this covers the geometries the runtime has to
 * report rather than repair (an odd run, no interleave block, a channel a
 * whole block above the base, two channels at one address) and the offset
 * fold the health scan in engine.cpp depends on.
 *
 * The second subject is snd/iop_stage.h, the witness for what the EE staged
 * in virtual IOP RAM before it queued a bank transfer. Its numbers are the
 * boot path's own, read out of dist/logs/handoff-2026-09-04/
 * native-crosscheck.log: the sound heap block is 0x78000 bytes at IOP
 * 0x090000 and the seven boot transfers read 237184, 322688, 209536, 136960,
 * 34176, 434176 and 407552 bytes from its base.
 */
#include "../chime.h"
#include "../iop_stage.h"
#include "../pcm_stream.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL  %s\n", what);
        ++g_failures;
    } else {
        std::printf("ok    %s\n", what);
    }
}

void check_eq(uint32_t got, uint32_t want, const char* what) {
    if (got != want) {
        std::printf("FAIL  %s: got 0x%x, want 0x%x\n", what, got, want);
        ++g_failures;
    } else {
        std::printf("ok    %s = 0x%x\n", what, got);
    }
}

/* ---- the unlock chime (snd/chime.h) --------------------------------------
 *
 * Nothing here is a hardware fact: the chime is the port's own sound, and
 * what a selftest can settle about it is that the buffer host/audio.cpp
 * sums into the output is well formed. Every sample finite, none of them
 * past the headroom the mixer's saturation assumes, a real attack, and a
 * tail that has reached silence before the buffer ends, at two different
 * output rates so the envelope is a function of time and not of the sample
 * index. Whether it is audible over the game, and at what level, only a run
 * can say. */
void test_chime_at(uint32_t rate) {
    char what[128];
    const uint32_t frames = rt_chime::frame_count(rate);
    std::snprintf(what, sizeof what, "chime at %u Hz: nonempty buffer", rate);
    check(frames > 0, what);
    if (frames == 0) return;

    /* The length is a duration, so it tracks the rate: within one frame of
     * kLengthSeconds either way. */
    const double ms = 1000.0 * (double)frames / (double)rate;
    const double want_ms = 1000.0 * (double)rt_chime::kLengthSeconds;
    std::snprintf(what, sizeof what, "chime at %u Hz: %.1f ms (want %.1f)", rate, ms, want_ms);
    check(ms > want_ms - 1.0 && ms <= want_ms, what);

    std::vector<float> buf(frames, 1.0f);
    rt_chime::render(buf.data(), frames, rate);

    bool finite = true;
    bool bounded = true;
    double peak = 0.0, first_half_peak = 0.0, second_half_peak = 0.0, tail_peak = 0.0;
    const uint32_t tail_from = frames - frames / 10;   /* the last tenth */
    for (uint32_t i = 0; i < frames; ++i) {
        const float v = buf[i];
        if (!std::isfinite(v)) finite = false;
        const double a = std::fabs((double)v);
        if (a > (double)rt_chime::kPeak) bounded = false;
        if (a > peak) peak = a;
        if (i < frames / 2) { if (a > first_half_peak) first_half_peak = a; }
        else if (a > second_half_peak) { second_half_peak = a; }
        if (i >= tail_from && a > tail_peak) tail_peak = a;
    }

    std::snprintf(what, sizeof what, "chime at %u Hz: every sample is finite", rate);
    check(finite, what);
    std::snprintf(what, sizeof what, "chime at %u Hz: no sample exceeds the %.2f headroom",
        rate, (double)rt_chime::kPeak);
    check(bounded, what);
    /* A buffer of zeroes would pass both of the above. */
    std::snprintf(what, sizeof what, "chime at %u Hz: peak %.3f is a real attack", rate, peak);
    check(peak > 0.1, what);
    std::snprintf(what, sizeof what, "chime at %u Hz: it decays (%.3f then %.3f)",
        rate, first_half_peak, second_half_peak);
    check(second_half_peak < first_half_peak, what);
    std::snprintf(what, sizeof what,
        "chime at %u Hz: the last tenth is silence (%.5f, under 1%% of the peak)", rate, tail_peak);
    check(tail_peak <= 0.01 * peak, what);
    std::snprintf(what, sizeof what, "chime at %u Hz: the buffer ends on exact zero", rate);
    check(buf[frames - 1] == 0.0f, what);
}

/* Applies the two cmd 0x48 records the movie sends. */
void open_movie_stream(RtPcmChannel* ch) {
    const uint32_t w1[2] = { 0x00010400u, 0x01010400u };
    const uint32_t w2[2] = { 0x001b0100u, 0x001b0300u };
    const uint32_t w3 = 0x00006000u;
    for (int i = 0; i < 2; ++i) {
        uint32_t vc = w1[i] >> 24;
        RtPcmChannel& c = ch[vc];
        c = RtPcmChannel();
        c.open = true;
        c.flags = w1[i] & 0xFFFFFF;
        c.block = w1[i] & 0xFF00;
        c.iop_buf = w2[i];
        c.ring = w3;
    }
}

/* One byte per ring byte, holding the channel that claims it. */
constexpr uint32_t kRingSize = 0x6000;
constexpr uint8_t kUnclaimed = 0xFF;
uint8_t g_cover[kRingSize];

} // namespace

int main() {
    RtPcmChannel ch[RT_PCM_CHANNELS];

    open_movie_stream(ch);
    check(rt_pcm_regroup(ch, RT_PCM_CHANNELS), "the movie's two opens are self consistent");
    check(ch[0].consistent && ch[1].consistent, "both channels are placed");

    check_eq(ch[0].block, 0x400, "block");
    check_eq(ch[0].base, 0x1b0100, "ring base (the lower of the two opens)");
    check_eq(ch[1].base, 0x1b0100, "channel 1 shares that base");
    check_eq(ch[0].slot, 0x000, "channel 0 slot");
    check_eq(ch[1].slot, 0x200, "channel 1 slot");
    check_eq(ch[0].chunk, 0x200, "channel 0 run");
    check_eq(ch[1].chunk, 0x200, "channel 1 run");

    /* Addressing: channel 0's first run is [base, base+0x200), then it skips
     * channel 1's run and resumes at base+0x400. */
    check_eq(rt_pcm_addr(ch[0], 0), 0x1b0100, "ch0 byte 0");
    check_eq(rt_pcm_addr(ch[0], 0x1FF), 0x1b02ff, "ch0 byte 0x1FF (end of its first run)");
    check_eq(rt_pcm_addr(ch[0], 0x200), 0x1b0500, "ch0 byte 0x200 (skips ch1's run)");
    check_eq(rt_pcm_addr(ch[1], 0), 0x1b0300, "ch1 byte 0");
    check_eq(rt_pcm_addr(ch[1], 0x200), 0x1b0700, "ch1 byte 0x200");

    /* Every byte of the ring belongs to exactly one channel, and the two
     * channels together tile it: 0x6000 / 0x400 = 24 blocks of 0x200 per
     * channel, so one lap is 0x3000 bytes each. Walking both laps into a
     * coverage map settles disjointness and the tiling claim together, in one
     * pass over the ring instead of one comparison per pair of offsets. */
    const uint32_t lap = ch[0].lap;
    check_eq(lap, 0x3000, "one lap of a channel's own data");
    check_eq(ch[1].lap, 0x3000, "channel 1's lap is the same");
    check_eq(rt_pcm_addr(ch[0], lap), 0x1b0100, "ch0 wraps to its first byte after one lap");
    check_eq(rt_pcm_addr(ch[1], lap), 0x1b0300, "ch1 wraps to its first byte after one lap");

    for (uint32_t i = 0; i < kRingSize; ++i) g_cover[i] = kUnclaimed;
    bool twice = false, outside = false;
    for (uint32_t c = 0; c < 2; ++c) {
        for (uint32_t i = 0; i < ch[c].lap; ++i) {
            uint32_t off = rt_pcm_addr(ch[c], i) - ch[c].base;
            if (off >= kRingSize) { outside = true; continue; }
            if (g_cover[off] != kUnclaimed) twice = true;
            g_cover[off] = (uint8_t)c;
        }
    }
    check(!outside, "a lap never addresses outside the ring");
    check(!twice, "no ring byte is claimed by two channels");
    uint32_t unclaimed = 0;
    for (uint32_t i = 0; i < kRingSize; ++i) unclaimed += g_cover[i] == kUnclaimed;
    check_eq(unclaimed, 0, "ring bytes claimed by no channel");

    /* The health scan in engine.cpp reads a whole run starting from the
     * current pos, so it asks for offsets from lap to lap + chunk - 1. Those
     * have to fold onto the same bytes the lap started with. */
    bool folds = true;
    for (uint32_t k = 0; k < ch[0].chunk; ++k) {
        if (rt_pcm_addr(ch[0], lap + k) != rt_pcm_addr(ch[0], k)) folds = false;
        if (rt_pcm_addr(ch[1], lap + k) != rt_pcm_addr(ch[1], k)) folds = false;
    }
    check(folds, "an offset one lap past the end folds onto the first run");

    /* The cursor the EE polls at status +0x180 is a ring offset quantized to
     * the interleave block, and it must stay below the ring size so that
     * audioDecSendToIOP's `(cursor + ring - write_ptr - 0x400) mod ring`
     * stays a sane free-space figure. */
    check_eq(rt_pcm_cursor(ch[0]), 0, "cursor at rest");
    ch[0].pos = 0x100;
    check_eq(rt_pcm_cursor(ch[0]), 0, "cursor stays in its block until the block is consumed");
    ch[0].pos = 0x200;
    check_eq(rt_pcm_cursor(ch[0]), 0x400, "cursor advances one block after 0x200 own bytes");
    ch[1].pos = 0x200;
    check_eq(rt_pcm_cursor(ch[1]), 0x400, "both channels report the same block");

    bool in_ring = true;
    for (uint32_t p = 0; p < lap; p += 2) {
        ch[0].pos = p;
        uint32_t cur = rt_pcm_cursor(ch[0]);
        if (cur >= ch[0].ring || cur % ch[0].block != 0) { in_ring = false; break; }
    }
    check(in_ring, "the cursor is block aligned and inside the ring for a whole lap");

    /* A single channel owns its whole block: nothing about the geometry
     * assumes there are two. */
    RtPcmChannel one[RT_PCM_CHANNELS];
    one[0].open = true;
    one[0].block = 0x400;
    one[0].iop_buf = 0x1b0100;
    one[0].ring = 0x6000;
    check(rt_pcm_regroup(one, RT_PCM_CHANNELS), "a lone channel is consistent");
    check_eq(one[0].chunk, 0x400, "a lone channel owns the whole block");
    check_eq(one[0].lap, 0x6000, "its lap is the whole ring");
    check_eq(rt_pcm_addr(one[0], 0x400), 0x1b0500, "a lone channel still steps by block");

    /* Everything below is a geometry the runtime reports rather than repairs.
     * A channel left unplaced (consistent == false) gets no cursor written
     * into the status block, so these are the cases that keep a fabricated
     * number out of audioDecSendToIOP's refill arithmetic. */
    RtPcmChannel bad[RT_PCM_CHANNELS];

    open_movie_stream(bad);
    bad[1].ring = 0x4000;
    check(!rt_pcm_regroup(bad, RT_PCM_CHANNELS), "disagreeing ring sizes are reported");

    open_movie_stream(bad);
    bad[1].iop_buf = 0x1b0301; /* runs of 0x201 and 0x1FF bytes */
    check(!rt_pcm_regroup(bad, RT_PCM_CHANNELS),
        "a run that is not a whole number of 16 bit samples is reported");
    check(!bad[0].consistent && !bad[1].consistent, "neither odd-run channel is placed");

    open_movie_stream(bad);
    bad[0].block = 0;
    bad[1].block = 0; /* flags with nothing in bits 15:8 */
    check(!rt_pcm_regroup(bad, RT_PCM_CHANNELS), "a zero interleave block is reported");
    check(!bad[0].consistent && !bad[1].consistent, "neither channel is placed without a block");
    check_eq(bad[0].lap, 0, "no block means no lap");

    open_movie_stream(bad);
    bad[0].ring = 0x6001; /* a ring the block does not tile */
    bad[1].ring = 0x6001;
    check(!rt_pcm_regroup(bad, RT_PCM_CHANNELS), "a ring the block does not tile is reported");

    open_movie_stream(bad);
    bad[1].iop_buf = 0x1b0500; /* a whole block above the base */
    check(!rt_pcm_regroup(bad, RT_PCM_CHANNELS),
        "a channel a whole block above the base is reported");
    check(!bad[1].consistent, "that channel is not placed");

    open_movie_stream(bad);
    bad[1].iop_buf = bad[0].iop_buf; /* two channels at one address */
    check(!rt_pcm_regroup(bad, RT_PCM_CHANNELS), "two channels at one address are reported");
    check(!bad[0].consistent && !bad[1].consistent, "neither of them is placed");

    /* The staging witness (snd/iop_stage.h). The boot pattern is one
     * SifSetDma of the whole chunk into the heap base, then one cmd 0x20
     * reading it back, repeated; each transfer must see its own bytes and
     * none of the previous transfer's marks. */
    {
        static RtIopStageMap map; /* 16 KB of bitmap: not a stack object */
        const uint32_t heap = 0x090000;
        const uint32_t sizes[] = { 237184, 322688, 209536, 136960, 34176, 434176, 407552 };
        bool every_chunk_seen = true;
        for (uint32_t sz : sizes) {
            map.note_write(heap, sz);
            every_chunk_seen &= map.fully_staged(heap, sz);
            check_eq(map.staged_bytes(heap, sz), sz, "staged bytes match the chunk");
            map.consume(heap, sz);
            every_chunk_seen &= !map.fully_staged(heap, sz);
        }
        check(every_chunk_seen, "each boot chunk reads staged and then reads consumed");

        /* A transfer whose staging never happened: the case the sndn2 warn
         * is there to catch. */
        map.reset();
        check(!map.fully_staged(heap, 4096), "an unstaged range is not staged");
        check_eq(map.staged_bytes(heap, 4096), 0, "an unstaged range stages no bytes");

        /* A short write followed by a longer read: partial staging is
         * reported as partial, not rounded up to the whole request. */
        map.reset();
        map.note_write(heap, 1024);
        check(!map.fully_staged(heap, 4096), "a partly staged range is not fully staged");
        check_eq(map.staged_bytes(heap, 4096), 1024, "only the written part counts");

        /* Consuming one chunk must not clear a neighbour that a different
         * SifSetDma staged: the heap holds one chunk at a time here, but the
         * map is addressed by IOP address, not by chunk. */
        map.reset();
        map.note_write(heap, 0x1000);
        map.note_write(heap + 0x1000, 0x1000);
        map.consume(heap, 0x1000);
        check(!map.fully_staged(heap, 0x1000), "the consumed chunk is gone");
        check(map.fully_staged(heap + 0x1000, 0x1000), "its neighbour survives");

        /* Granule rounding is outward, so a write of one byte marks the
         * 16-byte block it lands in and nothing beyond it. */
        map.reset();
        map.note_write(heap + 5, 1);
        check(map.fully_staged(heap, 16), "a one byte write marks its own granule");
        check(!map.fully_staged(heap, 17), "and not the next one");

        /* Out of range and empty arguments are answered, not trusted. */
        map.reset();
        map.note_write(RT_IOP_STAGE_SIZE, 64);
        check_eq(map.staged_bytes(RT_IOP_STAGE_SIZE, 64), 0, "a write past IOP RAM stages nothing");
        map.note_write(RT_IOP_STAGE_SIZE - 32, 4096); /* clipped at the top */
        check(map.fully_staged(RT_IOP_STAGE_SIZE - 32, 32), "a write clipped at the top still lands");
        check_eq(map.staged_bytes(heap, 0), 0, "a zero length stages nothing");
    }

    /* The unlock chime, at the host output rate and at one other, plus the
     * degenerate arguments the caller is allowed to pass. */
    test_chime_at(48000);
    test_chime_at(44100);
    rt_chime::render(nullptr, 16, 48000);
    {
        float one = 1.0f;
        rt_chime::render(&one, 0, 48000);
        rt_chime::render(&one, 1, 0);
        check(one == 1.0f, "a zero frame count or rate writes nothing");
    }

    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
