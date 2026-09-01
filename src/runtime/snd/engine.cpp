/* snd/engine.cpp: the native sound engine.
 *
 * 48 voices (SPU2 hardware count, 2 cores x 24; the EE library's own table
 * has 0x30 entries) rendered at the SPU2 native 48 kHz into stereo float.
 * Per voice: streaming VAG ADPCM decode out of fake SPU RAM (algorithm per
 * the public Sony VAG spec, same as the decomp repo's tools/decode_vag.py),
 * linear-interpolation resampling driven by the SPU2 pitch register unit
 * (0x1000 = one input sample per 48 kHz output tick), SPU2 ADSR envelope
 * from the two ADSR register words, linear left/right volumes, a reverb
 * send, and master volumes from the command stream.
 *
 * Command semantics: see SNDN2_NOTES.md. Unknown command ids are loud
 * (every occurrence logged with full operands), never fatal: a wrong note
 * beats a dead boot, and the log is the discovery tool.
 *
 * Threading: everything runs on the main OS thread (RPC handlers are
 * synchronous; rt_snd_flush_tick is called from the sndn2 fno 0x64 handler
 * once per vblank field). host/audio.cpp owns the handoff to the audio
 * device thread.
 */
#include "snd.h"

#include "../host/audio.h"
#include "../host/portable.h"
#include "../prof.h"
#include "../runtime.h"
#include "../sif/rpc.h" /* rt_iop_ptr: streams decode out of the IOP ring */

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

/* ---- VAG ADPCM (public Sony spec; cf. decomp tools/decode_vag.py) -------- */

constexpr int kF1[5] = { 0, 60, 115, 98, 122 };
constexpr int kF2[5] = { 0, 0, -52, -55, -60 };

constexpr uint8_t kFlagLoopEnd = 0x01;   /* jump to loop start after block */
constexpr uint8_t kFlagLoopRepeat = 0x02; /* with LoopEnd: keep playing */
constexpr uint8_t kFlagLoopStart = 0x04; /* this block is the loop start */

/* ---- voices --------------------------------------------------------------- */

constexpr int kNumVoices = 48;

enum class EnvPhase : uint8_t { Off, Attack, Decay, Sustain, Release };

struct Voice {
    /* VAG stream */
    uint32_t start_addr = 0;   /* byte address in SPU RAM */
    uint32_t cur_addr = 0;     /* current block */
    uint32_t loop_addr = 0;
    int hist1 = 0, hist2 = 0;
    int16_t block[28] = {};
    int block_pos = 28;        /* 28 = need a new block */
    bool ended = false;        /* LoopEnd without Repeat consumed */

    /* resampler */
    uint32_t pitch = 0x1000;   /* SPU2 pitch register value, 0x1000 = 48000 Hz */
    uint32_t frac = 0;         /* 12-bit fixed-point position */
    int16_t s_prev = 0, s_cur = 0;

    /* envelope */
    EnvPhase phase = EnvPhase::Off;
    int32_t env = 0;           /* 0..0x7FFF */
    uint32_t env_div = 0;      /* tick divider counter */
    uint16_t adsr1 = 0, adsr2 = 0;

    /* volumes: SPU2 VOLL/VOLR-style values from cmd 0x01, 0..0x3FFF linear
     * (bit 15 = sweep-mode word; approximated, see rt_snd_command). */
    uint16_t voll = 0, volr = 0;
    bool rev_on = false;       /* effect-send enable, cmd 0x0C mask */

    /* cmd 0x04 note parameters; pitch above is derived from these. */
    uint8_t center = 60, note = 60;
    int8_t fine = 0;           /* 1/16 semitone units */
    uint32_t pitch_scale = 0x1000; /* 12.12 fixed, 0x1000 = x1.0 */

    /* streaming (cmd 0x3E claims a voice for SgStAdpcm playback): the voice
     * decodes VAG blocks straight out of the interleaved IOP ring buffer
     * instead of SPU RAM. st_pos counts this voice's own stream bytes; the
     * chunk/stride mapping de-interleaves the shared ring. */
    bool is_stream = false;
    bool st_playing = false;
    uint32_t st_iop_buf = 0;   /* this voice's first chunk in IOP RAM */
    uint32_t st_ring = 0;      /* total ring size in bytes (shared) */
    uint32_t st_chunk = 0x800; /* contiguous bytes per voice per stride */
    uint32_t st_stride = 0x800;
    uint32_t st_blk = 0;       /* IOP transfer block; cursor granularity */
    uint32_t st_pos = 0;

    uint64_t keyon_count = 0;
};

Voice g_voices[kNumVoices];

/* Master volume per core (cmd 0x28, SgSetMasterVol, 0..0x3FFF). Core 1's
 * output feeds core 0 on hardware; this mono-core model uses core 0's
 * values (the game sets both identically). */
uint16_t g_master_l[2] = { 0, 0 };
uint16_t g_master_r[2] = { 0, 0 };
/* Reverb depth per core (cmd 0x16, SgSetReverbDepth). */
uint16_t g_rev_depth_l[2] = { 0, 0 };
uint16_t g_rev_depth_r[2] = { 0, 0 };
uint32_t g_rev_type = 0;

uint64_t g_keyons = 0;
uint64_t g_frames_rendered = 0;
uint32_t g_frame_frac = 0; /* fractional frames-per-field accumulator, 16.16 */

bool g_selftest_done = false;
bool g_unity_vol = false;  /* ICORECOMP_SND_UNITY_VOL: pipeline debug aid */

/* ---- VAG block decode ----------------------------------------------------- */

void decode_block(Voice& v) {
    const uint8_t* p = rt_spu_ram() + (v.cur_addr & (RT_SPU_RAM_SIZE - 16));
    int shift = p[0] & 0x0F;
    int filt = (p[0] >> 4) & 0x0F;
    if (filt > 4) filt = 0;
    uint8_t flags = p[1];
    int f1 = kF1[filt], f2 = kF2[filt];
    int h1 = v.hist1, h2 = v.hist2;
    for (int i = 0; i < 28; ++i) {
        int nib = (p[2 + (i >> 1)] >> ((i & 1) * 4)) & 0x0F;
        if (nib & 8) nib -= 16;
        int s = (nib << 12) >> shift;
        int val = s + ((f1 * h1 + f2 * h2) >> 6);
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        v.block[i] = (int16_t)val;
        h2 = h1;
        h1 = val;
    }
    v.hist1 = h1;
    v.hist2 = h2;
    v.block_pos = 0;

    if (flags & kFlagLoopStart) v.loop_addr = v.cur_addr;
    /* Consume the block, then honor end flags when it runs out. */
    if (flags & kFlagLoopEnd) {
        if (flags & kFlagLoopRepeat) {
            v.cur_addr = v.loop_addr;
        } else {
            v.ended = true; /* hardware would fetch silence and set ENDX */
        }
    } else {
        v.cur_addr += 16;
    }
}

/* The ring base is recovered from the voice's own buffer: the game hands
 * out base + chunk * slot, and base is stride-aligned. */
uint32_t ring_base(const Voice& v) { return v.st_iop_buf & ~(v.st_stride - 1); }

/* Absolute IOP RAM address of stream byte i for this voice: chunks of
 * st_chunk bytes at st_stride spacing, wrapping at the shared ring size. */
uint32_t stream_addr(const Voice& v, uint32_t i) {
    uint32_t slot = v.st_iop_buf & (v.st_stride - 1);
    uint32_t off = slot + (i / v.st_chunk) * v.st_stride + (i % v.st_chunk);
    return ring_base(v) + (v.st_ring ? off % v.st_ring : off);
}

/* Byte offset within the ring of the next byte this voice will consume,
 * rounded down to the IOP transfer block. This is the number the EE reads
 * back; see rt_snd_fill_status.
 *
 * The rounding is not a convenience. The retail refill converts the byte
 * delta to sectors by truncation (func_00132DC0: `sra $22, 11`, with the
 * +0x7FF only on the negative branch) while ACTSetEnvAllmighty advances its
 * own PREV by the untruncated delta. Any delta that is not a whole number
 * of sectors therefore leaves `delta % 2048` bytes of the ring holding the
 * previous lap's audio, permanently. Reporting a 16-byte-granular decoder
 * position would do exactly that, roughly every 2.3 s. The hardware cursor
 * moves in whole transfer blocks (0x4000 here, and the 0x5C000 ring is
 * exactly 23 of them), which keeps every delta a whole number of sectors. */
uint32_t stream_cursor(const Voice& v) {
    uint32_t off = stream_addr(v, v.st_pos) - ring_base(v);
    return v.st_blk ? off - (off % v.st_blk) : off;
}

void decode_stream_block(Voice& v) {
    /* 16-byte VAG block; chunks are multiples of 16, so a block never
     * spans a stride boundary. Loop/end flags are ignored: a stream is a
     * continuous ring the EE refills. */
    const uint8_t* p = rt_iop_ptr(stream_addr(v, v.st_pos));
    int shift = p[0] & 0x0F;
    int filt = (p[0] >> 4) & 0x0F;
    if (filt > 4) filt = 0;
    int f1 = kF1[filt], f2 = kF2[filt];
    int h1 = v.hist1, h2 = v.hist2;
    for (int i = 0; i < 28; ++i) {
        int nib = (p[2 + (i >> 1)] >> ((i & 1) * 4)) & 0x0F;
        if (nib & 8) nib -= 16;
        int s = (nib << 12) >> shift;
        int val = s + ((f1 * h1 + f2 * h2) >> 6);
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        v.block[i] = (int16_t)val;
        h2 = h1;
        h1 = val;
    }
    v.hist1 = h1;
    v.hist2 = h2;
    v.block_pos = 0;
    v.st_pos += 16;
}

int16_t next_input_sample(Voice& v) {
    if (v.block_pos >= 28) {
        if (v.is_stream) {
            decode_stream_block(v);
            return v.block[v.block_pos++];
        }
        if (v.ended) {
            /* Past the end: hardware plays the all-zero "silence" pattern.
             * Kill the envelope so the voice frees up. */
            v.phase = EnvPhase::Off;
            v.env = 0;
            return 0;
        }
        decode_block(v);
    }
    return v.block[v.block_pos++];
}

/* ---- SPU2 ADSR (public spec: nocash SPU documentation, identical envelope
 * generator on PS1 SPU and SPU2, SPU2 clocked at 48 kHz) ------------------- */

/* Executes one envelope step for the given rate parameters. Returns the new
 * level. mode_exp/decrease select the exponential/direction behavior. */
void env_step(Voice& v, int shift, int step_val, bool mode_exp, bool decrease) {
    /* Wait 1 << max(0, shift - 11) ticks between applications. */
    uint32_t cycles = 1u << (shift > 11 ? shift - 11 : 0);
    int32_t step = step_val << (shift < 11 ? 11 - shift : 0);
    if (mode_exp && !decrease && v.env > 0x6000) cycles *= 4; /* slow attack tail */
    if (mode_exp && decrease) step = (int32_t)(((int64_t)step * v.env) >> 15);
    if (++v.env_div < cycles) return;
    v.env_div = 0;
    v.env += decrease ? -step : step;
    if (v.env < 0) v.env = 0;
    if (v.env > 0x7FFF) v.env = 0x7FFF;
}

void env_tick(Voice& v) {
    switch (v.phase) {
        case EnvPhase::Off:
            return;
        case EnvPhase::Attack: {
            bool exp = (v.adsr1 & 0x8000) != 0;
            int shift = (v.adsr1 >> 10) & 0x1F;
            int step = 7 - ((v.adsr1 >> 8) & 3);
            env_step(v, shift, step, exp, false);
            if (v.env >= 0x7FFF) { v.env = 0x7FFF; v.phase = EnvPhase::Decay; v.env_div = 0; }
            return;
        }
        case EnvPhase::Decay: {
            int shift = (v.adsr1 >> 4) & 0x0F;
            int sustain_level = (((v.adsr1 & 0x0F) + 1) << 11) - 1;
            env_step(v, shift, 8, true, true);
            if (v.env <= sustain_level) { v.phase = EnvPhase::Sustain; v.env_div = 0; }
            return;
        }
        case EnvPhase::Sustain: {
            bool exp = (v.adsr2 & 0x8000) != 0;
            bool dec = (v.adsr2 & 0x4000) != 0;
            int shift = (v.adsr2 >> 8) & 0x1F;
            int step_bits = (v.adsr2 >> 6) & 3;
            int step = dec ? 8 - step_bits : 7 - step_bits;
            env_step(v, shift, step, exp, dec);
            if (v.env <= 0 && dec) v.phase = EnvPhase::Off;
            return;
        }
        case EnvPhase::Release: {
            bool exp = (v.adsr2 & 0x0020) != 0;
            int shift = v.adsr2 & 0x1F;
            env_step(v, shift, 8, exp, true);
            if (v.env <= 0) { v.env = 0; v.phase = EnvPhase::Off; }
            return;
        }
    }
}

/* ---- key events ----------------------------------------------------------- */

/* Derives the SPU2 pitch register value from the cmd 0x04 note parameters.
 * The EE sends {center(tone byte +2), note, fine(1/16 semitone), 12.12
 * pitch scale}; the conversion to an absolute rate happens in the IOP
 * driver, which is not available for reference. Convention chosen here:
 * note == center plays the sample at 48 kHz (pitch 0x1000), each semitone
 * is a factor of 2^(1/12). This is the standard VAB-style mapping and puts
 * the observed boot SEs (note 13-14 semitones below center) at ~21-22 kHz,
 * consistent with typical 22.05 kHz encodings. Revisit against real
 * hardware output if tuning sounds off. */
void update_pitch(Voice& v) {
    float semis = (float)((int)v.note - (int)v.center) + (float)v.fine / 16.0f;
    float step = std::exp2(semis / 12.0f) * ((float)v.pitch_scale / 4096.0f);
    int32_t p = (int32_t)(step * 4096.0f + 0.5f);
    if (p < 0) p = 0;
    if (p > 0x3FFF) p = 0x3FFF;
    v.pitch = (uint32_t)p;
}

void key_on(Voice& v) {
    v.cur_addr = v.start_addr;
    v.loop_addr = v.start_addr;
    v.hist1 = v.hist2 = 0;
    v.block_pos = 28;
    v.ended = false;
    v.frac = 0;
    v.s_prev = v.s_cur = 0;
    v.phase = EnvPhase::Attack;
    v.env = 0;
    v.env_div = 0;
    ++v.keyon_count;
    ++g_keyons;
}

void key_off(Voice& v) {
    if (v.phase != EnvPhase::Off) {
        v.phase = EnvPhase::Release;
        v.env_div = 0;
    }
}

/* ---- selftest dump -------------------------------------------------------- */

/* ICORECOMP_SND_SELFTEST=prefix: at the first key-on, dump the voice's raw
 * VAG bytes (up to the end flag, capped) to <prefix>.vag and our decode of
 * the same bytes to <prefix>.s16 (mono s16le), for external comparison with
 * the decomp repo's tools/decode_vag.py (see snd/tests/vag_compare.py).
 * Output files are ROM-derived: keep them out of the repo (untracked paths
 * only; check_no_rom is the gate). */
void selftest_dump(const Voice& v) {
    const char* prefix = std::getenv("ICORECOMP_SND_SELFTEST");
    if (!prefix || g_selftest_done) return;
    g_selftest_done = true;

    const uint8_t* ram = rt_spu_ram();
    uint32_t max_bytes = 512 * 1024;
    if (v.start_addr >= RT_SPU_RAM_SIZE) return;
    if (max_bytes > RT_SPU_RAM_SIZE - v.start_addr) max_bytes = RT_SPU_RAM_SIZE - v.start_addr;

    /* Find the stream end: first block with the LoopEnd flag. */
    uint32_t len = 0;
    while (len + 16 <= max_bytes) {
        uint8_t flags = ram[v.start_addr + len + 1];
        len += 16;
        if (flags & kFlagLoopEnd) break;
    }

    char path[1024];
    std::snprintf(path, sizeof(path), "%s.vag", prefix);
    std::FILE* f = rt_fopen_utf8(path, "wb");
    if (f) { std::fwrite(ram + v.start_addr, 1, len, f); std::fclose(f); }

    /* Decode the same bytes with a scratch voice (linear stream decode, no
     * loop handling: matches decode_vag.py stop_on_end=False over `len`). */
    std::snprintf(path, sizeof(path), "%s.s16", prefix);
    f = rt_fopen_utf8(path, "wb");
    if (f) {
        Voice scratch;
        scratch.start_addr = scratch.cur_addr = scratch.loop_addr = v.start_addr;
        scratch.block_pos = 28;
        for (uint32_t b = 0; b < len / 16; ++b) {
            /* force linear traversal regardless of loop flags */
            scratch.cur_addr = v.start_addr + b * 16;
            decode_block(scratch);
            std::fwrite(scratch.block, 2, 28, f);
        }
        std::fclose(f);
    }
    rt_log("snd", "selftest: dumped %u VAG bytes at SPU 0x%06x -> %s.{vag,s16}",
        len, v.start_addr, prefix);
}

/* ---- rendering ------------------------------------------------------------ */

float mix_voice(Voice& v, float* out_l, float* out_r, float* out_rev) {
    float e;
    if (v.is_stream) {
        if (!v.st_playing) return 0.0f;
        e = 1.0f; /* streams have no envelope */
    } else {
        if (v.phase == EnvPhase::Off) return 0.0f;
        env_tick(v);
        if (v.phase == EnvPhase::Off) return 0.0f;
        e = (float)v.env / 32767.0f;
    }

    /* Advance the input stream by pitch/0x1000 samples (SPU2 pitch unit:
     * 0x1000 steps one 48 kHz input sample per output tick; capped at
     * 0x3FFF = 4x like the hardware register). */
    uint32_t pitch = v.pitch > 0x3FFF ? 0x3FFF : v.pitch;
    v.frac += pitch;
    while (v.frac >= 0x1000) {
        v.frac -= 0x1000;
        v.s_prev = v.s_cur;
        v.s_cur = next_input_sample(v);
    }
    float t = (float)v.frac / 4096.0f;
    float s = ((float)v.s_prev + ((float)v.s_cur - (float)v.s_prev) * t) / 32768.0f;

    float sample = s * e;
    float vl = g_unity_vol ? 0.5f : (float)v.voll / 16383.0f;
    float vr = g_unity_vol ? 0.5f : (float)v.volr / 16383.0f;
    *out_l += sample * vl;
    *out_r += sample * vr;
    if (v.rev_on) *out_rev += sample * (vl + vr) * 0.5f;
    return sample;
}

void render(uint32_t frames) {
    /* Render in small stack chunks. */
    float buf[256 * 2];
    while (frames) {
        uint32_t n = frames > 256 ? 256 : frames;
        float mast_l = g_unity_vol ? 1.0f : (float)g_master_l[0] / 16383.0f;
        float mast_r = g_unity_vol ? 1.0f : (float)g_master_r[0] / 16383.0f;
        for (uint32_t i = 0; i < n; ++i) {
            float l = 0.0f, r = 0.0f, rev = 0.0f;
            for (auto& v : g_voices) mix_voice(v, &l, &r, &rev);
            rt_reverb_run(rev, &l, &r);
            l *= mast_l;
            r *= mast_r;
            if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
            if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
            buf[i * 2] = l;
            buf[i * 2 + 1] = r;
        }
        rt_audio_submit(buf, n);
        g_frames_rendered += n;
        frames -= n;
    }
}

} // namespace

/* ---- public interface ----------------------------------------------------- */

void rt_snd_engine_init(uint32_t voice_budget) {
    for (auto& v : g_voices) v = Voice();
    for (int c = 0; c < 2; ++c) {
        g_master_l[c] = g_master_r[c] = 0;
        g_rev_depth_l[c] = g_rev_depth_r[c] = 0;
    }
    g_rev_type = 0;
    g_unity_vol = std::getenv("ICORECOMP_SND_UNITY_VOL") != nullptr;
    rt_reverb_reset();
    rt_audio_init();
    rt_log("snd", "engine init: %d voices modeled (EE budget %u)%s", kNumVoices, voice_budget,
        g_unity_vol ? "; ICORECOMP_SND_UNITY_VOL: game volume commands overridden" : "");
}

void rt_snd_fill_status(uint8_t* recv, uint32_t recv_size) {
    /* Per-voice stream read cursors. The value is a byte OFFSET WITHIN THE
     * RING, 0 .. ring - 1, not an IOP address. Reporting an address here is
     * what made the boot ambience play as fragments from all over its file.
     *
     * Ground truth is ACTSetEnvAllmighty, the read callback AdpcmOpen hands
     * to iosCdvdChgFileName. Per tick it does:
     *
     *   CUR      = func_0025DFB0(slot->0x08)      // this word, channel-0 voice
     *   PREV     = slot->0x10                     // 0 at open, kept < slot->0x1C
     *   consumed = CUR >= PREV ? CUR - PREV : slot->0x1C - PREV
     *   if (consumed > 0x1EAAA || CUR < PREV)     // a third of the ring, or wrap
     *       read(slot->0x18 + PREV, consumed)     // 0x18 = ring base
     *   slot->0x10 = PREV + consumed < slot->0x1C ? PREV + consumed : 0
     *
     * slot->0x1C is the ring size (0x5C000), so CUR is compared and
     * subtracted against a ring offset throughout. Feed it an address and
     * every refill asks for `address` bytes and lands back at offset 0. */
    for (int i = 0; i < kNumVoices; ++i) {
        const Voice& v = g_voices[i];
        if (!v.is_stream) continue;
        uint32_t off = 0xC0 + (uint32_t)(i % 24) * 4 + (uint32_t)(i / 24) * 0x60;
        if (off + 4 > recv_size) continue;
        uint32_t cur = stream_cursor(v);
        std::memcpy(recv + off, &cur, 4);
    }
}

bool rt_snd_stream_ring(uint32_t addr, uint32_t* base, uint32_t* ring, uint32_t* cursor) {
    for (int i = 0; i < kNumVoices; ++i) {
        const Voice& v = g_voices[i];
        if (!v.is_stream || v.st_ring == 0) continue;
        uint32_t b = ring_base(v);
        if (addr < b || addr >= b + v.st_ring) continue;
        if (base) *base = b;
        if (ring) *ring = v.st_ring;
        if (cursor) *cursor = stream_cursor(v);
        return true;
    }
    return false;
}

void rt_snd_flush_tick() {
    RT_PROF_ZONE(RT_PROF_AUDIO);
    /* One vblank field of audio: 48000 / 59.94 = 800.80 frames. 16.16
     * fixed-point accumulator carries the fraction. */
    constexpr uint64_t kStep = ((uint64_t)RT_AUDIO_RATE << 16) * 1001 / 60000; /* 59.94 Hz */
    g_frame_frac += (uint32_t)kStep;
    uint32_t frames = g_frame_frac >> 16;
    g_frame_frac &= 0xFFFF;
    render(frames);


    /* Once per ~10 s: stream ring health (fill state is otherwise invisible
     * because raw SIF DMA logging is deduplicated). */
    static uint64_t ticks = 0;
    if (++ticks % 600 == 0) {
        for (int i = 0; i < kNumVoices; ++i) {
            const Voice& v = g_voices[i];
            if (!v.is_stream || !v.st_playing) continue;
            uint32_t base = ring_base(v);
            /* st_iop_buf comes from the guest, so the scan window has to be
             * clipped to IOP RAM rather than trusted to fit. */
            uint32_t span = base < RT_IOP_RAM_SIZE ? RT_IOP_RAM_SIZE - base : 0;
            if (span > 0x1000) span = 0x1000;
            const uint8_t* p = rt_iop_ptr(base);
            uint32_t nonzero = 0;
            for (uint32_t b = 0; b < span; ++b) nonzero += p[b] != 0;
            /* pitch is the whole ballgame for a stream: 0x1000 means the
             * voice is stepping one 48 kHz sample per output sample, so
             * 44.1 kHz source material plays 8.8% fast and outruns the
             * ring. Report it with the rate it implies. */
            static uint32_t last_pos[kNumVoices] = {0};
            uint32_t advanced = v.st_pos - last_pos[i];
            last_pos[i] = v.st_pos;
            rt_log("snd", "stream voice %d: pitch=0x%04x (%.0f Hz) pos=0x%x "
                          "(+%u bytes/s = %.0f Hz effective) cursor=+0x%05x nonzero=%u",
                i, v.pitch, (double)v.pitch * RT_AUDIO_RATE / 4096.0, v.st_pos,
                advanced, (double)advanced * 28.0 / 16.0,
                stream_cursor(v), nonzero);
        }
    }
}

/* Converts a cmd 0x01 volume word to a linear 0..0x3FFF level. Plain values
 * are already linear; bit 15 marks the SPU2 sweep-mode encoding
 * ((mode << 8) | (vol >> 7), retail func_0025AC60 tail), which this model
 * flattens to its 7-bit level without the ramp. Negative (phase-invert)
 * values use their magnitude. */
uint16_t vol_word(uint32_t w) {
    int32_t s = (int16_t)(w & 0xFFFF);
    if (s < 0 && (w & 0x8000)) {
        /* could be sweep encoding or a negative linear volume; sweep is the
         * documented emitter behavior when the tone's sweep byte is set */
        return (uint16_t)((w & 0x7F) << 7);
    }
    if (s < 0) s = -s;
    return (uint16_t)(s > 0x3FFF ? 0x3FFF : s);
}

/* Applies a 48-bit voice mask (w2 = voices 0..23, w3 = voices 24..47,
 * both 24-bit halves) to a per-voice action. */
template <typename Fn>
void for_mask(uint32_t w2, uint32_t w3, Fn fn) {
    uint64_t mask = ((uint64_t)(w3 & 0xFFFFFF) << 24) | (w2 & 0xFFFFFF);
    for (int i = 0; i < kNumVoices; ++i) {
        if (mask & (1ull << i)) fn(g_voices[i]);
    }
}

void rt_snd_command(uint32_t cmd, uint32_t w1, uint32_t w2, uint32_t w3) {
    RT_PROF_ZONE(RT_PROF_AUDIO);
    /* Semantics per SNDN2_NOTES.md (EE emitters in the retail vendor
     * library; aug6 names in parentheses). */
    uint32_t voice = w1 & 0xFF;
    switch (cmd) {
        /* ---- per-voice (w1 = voice number 0..0x2F) ---- */
        case 0x01: /* voice volume left/right (func_0025AC60) */
            if (voice >= kNumVoices) { rt_log("snd", "cmd 0x01 voice %u out of range", voice); return; }
            g_voices[voice].voll = vol_word(w2);
            g_voices[voice].volr = vol_word(w3);
            break;
        case 0x02: /* ADSR1/ADSR2, raw SPU2 register words from the bank */
            if (voice >= kNumVoices) { rt_log("snd", "cmd 0x02 voice %u out of range", voice); return; }
            g_voices[voice].adsr1 = (uint16_t)(w2 & 0xFFFF);
            g_voices[voice].adsr2 = (uint16_t)(w3 & 0xFFFF);
            break;
        case 0x03: /* VAG start address, SPU RAM byte address */
            if (voice >= kNumVoices) { rt_log("snd", "cmd 0x03 voice %u out of range", voice); return; }
            if (w2 >= RT_SPU_RAM_SIZE) { rt_log("snd", "cmd 0x03 addr 0x%08x out of SPU RAM", w2); return; }
            g_voices[voice].start_addr = w2 & ~15u;
            break;
        case 0x04: { /* note params (func_0025AC18): w2 = center<<24 | note<<16
                      * | fine<<8 | bend-center, w3 = moddepth<<24 | scale12.12 */
            if (voice >= kNumVoices) { rt_log("snd", "cmd 0x04 voice %u out of range", voice); return; }
            Voice& v = g_voices[voice];
            v.center = (uint8_t)(w2 >> 24);
            v.note = (uint8_t)(w2 >> 16);
            v.fine = (int8_t)(w2 >> 8);
            v.pitch_scale = w3 & 0xFFFFFF;
            update_pitch(v);
            break;
        }
        /* ---- per-tick voice masks (w1 = 0; w2/w3 = 24-bit halves) ---- */
        case 0x0A: /* KEY ON */
            for_mask(w2, w3, [](Voice& v) { key_on(v); selftest_dump(v); });
            break;
        case 0x0B: /* KEY OFF */
            for_mask(w2, w3, [](Voice& v) { key_off(v); });
            break;
        case 0x0C: /* effect (reverb) send enable, level-triggered full state */
            for (int i = 0; i < kNumVoices; ++i) g_voices[i].rev_on = false;
            for_mask(w2, w3, [](Voice& v) { v.rev_on = true; });
            break;
        case 0x0D: /* second mode mask (noise/pitch-mod class, unresolved on
                    * the EE side); accepted without an audible model */
            break;
        /* ---- reverb / master (w1 = core 0/1) ---- */
        case 0x14: /* SgSetReverbEndAddr(core, addr): work area bound */
            break;
        case 0x15: /* SgSetReverbType(core, type) */
            g_rev_type = w2;
            rt_reverb_set_params(g_rev_type,
                (float)g_rev_depth_l[0] / 16383.0f, (float)g_rev_depth_r[0] / 16383.0f);
            break;
        case 0x16: /* SgSetReverbDepth(core, dL, dR) */
            g_rev_depth_l[w1 & 1] = (uint16_t)(w2 & 0x7FFF);
            g_rev_depth_r[w1 & 1] = (uint16_t)(w3 & 0x7FFF);
            rt_reverb_set_params(g_rev_type,
                (float)g_rev_depth_l[0] / 16383.0f, (float)g_rev_depth_r[0] / 16383.0f);
            break;
        case 0x17: /* SgSetReverbDelaytime */
        case 0x18: /* SgSetReverbFeedback */
            break; /* preset-keyed reverb ignores fine parameters for now */
        case 0x28: /* SgSetMasterVol(core, L, R), 0..0x3FFF */
            g_master_l[w1 & 1] = (uint16_t)(w2 & 0x3FFF);
            g_master_r[w1 & 1] = (uint16_t)(w3 & 0x3FFF);
            break;
        /* ---- misc setup ---- */
        case 0x1F: /* SgQuit terminator */
        case 0x32: /* parameter write (0x0A = digital output mode, 0x08 =
                    * per-voice tone attribute; no audible model) */
        case 0x3C: /* SgStAdpcmInit */
        case 0x3D: /* SgStAdpcmQuit */
            break;
        /* ---- ADPCM streaming (SgStAdpcm*) ---- */
        case 0x3E: { /* Open (func_0025DD20): claims a voice for a stream.
                      * w1 = vc<<24 | mode | blk[15:8] | spu[23:16],
                      * w2 = spu[15:0]<<16 | ring[23:8],
                      * w3 = ring[7:0]<<24 | iopBuf[23:0].
                      * The vendor loads the ring size once and packs it into
                      * both w2 and w3's top byte, so both halves are needed;
                      * blk is a separate field (`lw $3,0x14($16); andi
                      * $3,0xFF00`) and carries only its bits 15:8. mode is
                      * 0x10000/0x20000/0x40000 for 1/2/4 channels, so
                      * reading it as a channel count is exact. spu is the
                      * voice's SPU RAM address, which this model does not
                      * need: playback reads the IOP ring directly. */
            uint32_t vc = w1 >> 24;
            uint32_t nch = (w1 >> 16) & 0xFF;
            uint32_t ring = ((w2 & 0xFFFF) << 8) | (w3 >> 24);
            uint32_t blk = w1 & 0xFF00;
            uint32_t iop = w3 & 0xFFFFFF;
            /* mode carries exactly 1, 2 or 4. Three would give a chunk of
             * 682, which is not a multiple of 16, and decode_stream_block
             * would read VAG blocks across the interleave boundary into the
             * next channel's data. Reject rather than guess. */
            if (vc >= kNumVoices || (nch != 1 && nch != 2 && nch != 4)) {
                rt_log("snd", "stream open rejected: voice=%u nch=%u "
                              "(w1=0x%08x w2=0x%08x w3=0x%08x)", vc, nch, w1, w2, w3);
                return;
            }
            /* iop and ring are guest values. Validate the whole ring here,
             * at the one point they enter, so playback, the health scan and
             * the play-time peek can all index it without re-checking. */
            if ((uint64_t)(iop & ~0x7FFu) + ring > RT_IOP_RAM_SIZE) {
                rt_log("snd", "stream open rejected: voice=%u ring 0x%06x+0x%x leaves "
                              "IOP RAM", vc, iop & ~0x7FFu, ring);
                return;
            }
            /* Quantizing the cursor only keeps refills sector-aligned if the
             * block is a whole number of sectors and tiles the ring exactly.
             * ICO: blk 0x4000, ring 0x5C000 = 23 blocks. */
            if (blk == 0 || blk % 2048 != 0 || ring == 0 || ring % blk != 0) {
                rt_log("snd", "stream open: voice=%u block 0x%x does not tile ring 0x%x in "
                              "whole sectors; reporting an unquantized cursor, so refills "
                              "will leave gaps", vc, blk, ring);
                blk = 0;
            }
            Voice& v = g_voices[vc];
            v = Voice();
            v.is_stream = true;
            v.st_iop_buf = iop;
            v.st_ring = ring;
            v.st_chunk = 0x800 / nch;
            v.st_stride = 0x800;
            v.st_blk = blk;
            rt_log("snd", "stream open: voice=%u nch=%u iop=0x%06x ring=0x%x blk=0x%x "
                          "spu=0x%06x", vc, nch, iop, ring, blk, ((w1 & 0xFF) << 16) | (w2 >> 16));
            break;
        }
        case 0x3F: /* Close(voice) */
            if (voice < kNumVoices && g_voices[voice].is_stream) g_voices[voice] = Voice();
            break;
        case 0x40: /* ChannelVolume(handle, L, R): w1|w2<<24 = 48-bit voice
                    * mask handle, w3 = (L<<16)|R, 0..0x3FFF */
            for_mask(w1, w2, [w3](Voice& v) {
                v.voll = (uint16_t)((w3 >> 16) & 0x3FFF);
                v.volr = (uint16_t)(w3 & 0x3FFF);
            });
            break;
        case 0x41: /* ChannelPitch(handle, rate in Hz, <= 192000) */
            for_mask(w1, w2, [w3](Voice& v) {
                uint64_t p = (uint64_t)w3 * 0x1000 / RT_AUDIO_RATE;
                v.pitch = p > 0x3FFF ? 0x3FFF : (uint32_t)p;
            });
            break;
        case 0x42: /* Play(handle) */
            for_mask(w1, w2, [](Voice& v) {
                if (!v.is_stream) return;
                v.st_playing = true;
                v.st_pos = 0;
                v.block_pos = 28;
                v.hist1 = v.hist2 = 0;
                v.frac = 0;
                v.s_prev = v.s_cur = 0;
                const uint8_t* p = rt_iop_ptr(v.st_iop_buf);
                rt_log("snd", "stream play: iop=0x%06x first bytes %02x %02x %02x %02x %02x %02x %02x %02x",
                    v.st_iop_buf, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
            });
            break;
        case 0x43: /* Stop(handle) */
            for_mask(w1, w2, [](Voice& v) { v.st_playing = false; });
            break;
        case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A:
        case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            rt_log("snd", "STREAM cmd 0x%02x (SgStPcm*) NOT MODELED: w1=0x%08x w2=0x%08x w3=0x%08x",
                cmd, w1, w2, w3);
            break;
        default:
            rt_log("snd", "cmd 0x%02x NOT MODELED: w1=0x%08x w2=0x%08x w3=0x%08x",
                cmd, w1, w2, w3);
            break;
    }
}
