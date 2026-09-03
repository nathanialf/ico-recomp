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

#include "pcm_stream.h"

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

/* ---- SgStPcm streams (commands 0x46-0x4F) --------------------------------
 *
 * A separate namespace from the 48 SPU2 voices above. The command block
 * addresses channels 0..0xF of its own (func_0025E050 and func_0025E238 both
 * bound the index with `sltiu $2, $x, 0x10`) and the 0x4A/0x4B/0x4C masks are
 * over those channels, not over SPU2 voices, so mapping them onto g_voices
 * would invent a correspondence the game never states and would collide with
 * the ADPCM ambience on voices 0 and 1. Ring geometry lives in
 * pcm_stream.h; this file owns decode, volume and mixing.
 *
 * The attract movie is the only user of the block in this binary. See
 * sif/SNDN2_NOTES.md for the per command derivation. */
RtPcmChannel g_pcm[RT_PCM_CHANNELS];

/* Sample format. INFERRED, not measured: 16 bit signed little endian at the
 * SPU2 native 48 kHz, played 1:1 against this engine's 48 kHz output.
 *
 * The reasoning, so it can be checked rather than trusted: the ADPCM family
 * carries an explicit playback rate (cmd 0x41, capped at 0x2EE00 = 192000 Hz,
 * retail func_0025DE78) because its source material varies. The SgStPcm
 * family has no rate command at all, which only works if the driver plays at
 * one fixed rate, and the only rate that needs no SPU2 pitch programming is
 * the hardware's own 48 kHz. 16 bit is what "PCM" means on this hardware and
 * is what makes 0x200 bytes a round 256 samples per channel per block.
 *
 * rt_snd_command's 0x4B handler logs the first bytes of each channel plus the
 * peak of the first block read as s16, so a Windows log settles the format
 * question without another round of reverse engineering. */
constexpr uint32_t kPcmBytesPerSample = 2;

/* Reads one 16 bit sample of channel c at its own stream offset. Runs never
 * straddle: rt_pcm_regroup rejects an odd chunk, so a two byte sample always
 * sits inside one run. */
int16_t pcm_sample(const RtPcmChannel& c) {
    const uint8_t* p = rt_iop_ptr(rt_pcm_addr(c, c.pos));
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* cmd 0x4A volumes are 0..0x7FFF (func_0025E1E8 rejects anything above), one
 * bit wider than the 0..0x3FFF the voice commands use.
 *
 * The movie sends 0x3FFF, and that number is a constant in the caller, not a
 * measurement: StageOrientInit is called with `addiu $10, $0, 0x3FFF` in the
 * delay slot (asm/nonmatchings/src/stage_orient/func_0019D678.s, 0019D7F4),
 * which is its seventh argument; it keeps that register in $22 (0019D2A0) and
 * passes it as argument 3 to func_0023D8A8 (0019D398 with `daddu $6, $22, $0`
 * in the delay slot); func_0023D8A8 stores argument 3 at self+0x5C (`daddu
 * $20, $6, $0` at 0023D8CC, `sw $20, 0x5C($16)` at 0023D9BC); func_0023E298
 * loads self+0x5C at 0023E2CC and hands it to func_0025E1E8.
 *
 * 0x3FFF is unity on the engine's 0x3FFF scale; a larger value is carried
 * through as the gain above unity it denotes rather than clamped. */
float pcm_gain(uint16_t v) { return (float)v / 16383.0f; }

/* Per channel play-through tracking.
 *
 * The sound service never sees the EE's write pointer: the movie fills the
 * ring with raw SIF DMA (ito/mpeg/mv_sub.c func_0023DB80), not through this
 * service, and the only number that crosses the boundary the other way is the
 * cursor this side reports. The DMA itself does pass through the runtime,
 * though: every EE to IOP transfer entry reaches sif/rpc.cpp
 * rt_rpc_on_dma_entry, which calls rt_snd_pcm_note_iop_write below. That is
 * the write-side witness, and it is what makes starvation detectable without
 * guessing from content.
 *
 * Each interleave block of the shared ring holds the value of a counter that
 * is bumped once per DMA touching the ring. A block counts as stale when the
 * play cursor enters it and finds the same stamp it left there on its previous
 * lap: a whole lap of playback went by and the EE never wrote those bytes.
 * Content is not consulted, so a movie that legitimately sends the same bytes
 * twice (silence, most obviously) is not reported as starving. A block the EE
 * has never written keeps stamp 0 and is reported from the second visit on.
 *
 * The model plays through a stale block rather than stopping. A real IOP hands
 * the ring to the SPU2 core input on the hardware's own 48 kHz clock and plays
 * whatever the memory holds; it has no way to stall for the EE. Stopping would
 * be a divergence invented to hide a host-side timing problem. The counter is
 * how the starvation is made visible instead of silent. */
constexpr int kPcmMaxBlocks = 64;

/* Write stamps for the shared ring, indexed by ring offset / block. The three
 * extent words are the DMA hook's range check, refreshed by pcm_refresh_ring
 * whenever the open set changes, so a transfer anywhere else in IOP RAM costs
 * one compare. */
uint32_t g_pcm_wr_gen[kPcmMaxBlocks] = {};
uint32_t g_pcm_writes = 0;
uint32_t g_pcm_ring_base = 0;
uint32_t g_pcm_ring_size = 0;
uint32_t g_pcm_ring_block = 0;

struct PcmTrack {
    uint32_t gen[kPcmMaxBlocks] = {};  /* stamp found on the last visit */
    bool seen[kPcmMaxBlocks] = {};
    uint32_t last_block = 0xFFFFFFFFu;
    uint32_t stale = 0;    /* blocks re-entered that the EE never rewrote */
    uint32_t entered = 0;  /* blocks entered since the last health report */
    int32_t peak = 0;      /* max |s16| since the last health report */
};
PcmTrack g_pcm_track[RT_PCM_CHANNELS];

/* Health-report state. rt_snd_engine_init is reachable more than once (the
 * sndn2 fno 0x65 remote init, sif/sndn2.cpp), so these live at file scope and
 * are reset there; as function statics a re-init left them holding the old
 * run's numbers and the first line after it reported an impossible rate. */
uint64_t g_health_ticks = 0;
uint32_t g_last_stream_pos[kNumVoices] = {};
uint64_t g_last_clip = 0, g_last_frames = 0;
uint64_t g_last_consumed[RT_PCM_CHANNELS] = {};
bool g_pcm_cursor_warned[RT_PCM_CHANNELS] = {};

/* Keeps the DMA hook's range check in step with the open set, and drops the
 * stamps: a change to base, ring or block moves what a block index means. */
void pcm_refresh_ring() {
    g_pcm_ring_base = 0;
    g_pcm_ring_size = 0;
    g_pcm_ring_block = 0;
    for (const auto& c : g_pcm) {
        if (!c.open || !c.consistent) continue;
        g_pcm_ring_base = c.base;
        g_pcm_ring_size = c.ring;
        g_pcm_ring_block = c.block;
        break;
    }
    for (auto& g : g_pcm_wr_gen) g = 0;
    g_pcm_writes = 0;
    for (auto& t : g_pcm_track) {
        for (auto& g : t.gen) g = 0;
        for (auto& sn : t.seen) sn = false;
        t.last_block = 0xFFFFFFFFu;
    }
}

/* Frames whose mixed left or right sample left [-1,1] and was clipped in
 * render(). Reported by the health block so a "peaking" complaint has a
 * number behind it instead of an ear. */
uint64_t g_clip_frames = 0;

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

/* One output frame of every playing SgStPcm channel. No envelope and no
 * reverb send: the command block has neither, so the driver plays the ring
 * flat through the channel volumes and the core master volume. */
void mix_pcm(float* out_l, float* out_r) {
    for (int i = 0; i < RT_PCM_CHANNELS; ++i) {
        RtPcmChannel& c = g_pcm[i];
        if (!c.open || !c.playing || c.chunk == 0) continue;
        PcmTrack& t = g_pcm_track[i];

        /* Block entry: did the EE write this block during the lap just past?
         * The block index into the channel's own data and the index into the
         * shared ring are the same number, because block bi of the ring holds
         * this channel's bytes bi * chunk .. bi * chunk + chunk - 1. */
        uint32_t bi = c.pos / c.chunk;
        if (bi != t.last_block) {
            t.last_block = bi;
            if (bi < (uint32_t)kPcmMaxBlocks) {
                uint32_t gen = g_pcm_wr_gen[bi];
                ++t.entered;
                if (t.seen[bi] && t.gen[bi] == gen) ++t.stale;
                t.seen[bi] = true;
                t.gen[bi] = gen;
            }
        }

        int16_t raw = pcm_sample(c);
        int32_t mag = raw < 0 ? -(int32_t)raw : (int32_t)raw;
        if (mag > t.peak) t.peak = mag;
        float s = (float)raw / 32768.0f;

        c.pos += kPcmBytesPerSample;
        c.consumed += kPcmBytesPerSample;
        /* One lap of this channel's own data is ring / block runs of chunk
         * bytes, computed once by rt_pcm_regroup. Wrapping keeps pos, and
         * therefore the cursor reported at status +0x180, inside the ring the
         * game allocated. consumed above is the unwrapped count, so a rate
         * check is not aliased by the lap. */
        if (c.lap && c.pos >= c.lap) c.pos -= c.lap;

        *out_l += s * (g_unity_vol ? 0.5f : pcm_gain(c.voll));
        *out_r += s * (g_unity_vol ? 0.5f : pcm_gain(c.volr));
    }
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
            mix_pcm(&l, &r);
            rt_reverb_run(rev, &l, &r);
            l *= mast_l;
            r *= mast_r;
            bool clipped = false;
            if (l > 1.0f) { l = 1.0f; clipped = true; }
            else if (l < -1.0f) { l = -1.0f; clipped = true; }
            if (r > 1.0f) { r = 1.0f; clipped = true; }
            else if (r < -1.0f) { r = -1.0f; clipped = true; }
            if (clipped) ++g_clip_frames;
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
    for (auto& c : g_pcm) c = RtPcmChannel();
    for (auto& t : g_pcm_track) t = PcmTrack();
    pcm_refresh_ring();
    /* Health state. This function runs again on every sndn2 fno 0x65 remote
     * init, so the deltas have to start from the state as it is now rather
     * than from whatever the previous run left. */
    g_health_ticks = 0;
    for (auto& p : g_last_stream_pos) p = 0;
    for (auto& c : g_last_consumed) c = 0;
    for (auto& w : g_pcm_cursor_warned) w = false;
    g_last_clip = g_clip_frames;
    g_last_frames = g_frames_rendered;
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

    /* SgStPcm channel cursors at +0x180 + channel * 4. Retail func_0025E238
     * (asm/matchings/src/cod/vendor_25E1E8/func_0025E238.s) reads exactly
     * that word through the same uncached status pointer func_0025DFB0 uses,
     * for channels 0..0xF, and ito/mpeg/mv_sub.c func_0023DEB0 turns it into
     * the movie's refill size. Leaving it at zero, as this runtime did before
     * the block was modelled, tells the movie the driver has consumed nothing
     * and the whole ring minus one 0x400 block is free, every tick. */
    for (int i = 0; i < RT_PCM_CHANNELS; ++i) {
        const RtPcmChannel& c = g_pcm[i];
        if (!c.open) continue;
        uint32_t off = 0x180 + (uint32_t)i * 4;
        if (off + 4 > recv_size) continue;
        if (!c.consistent) {
            /* rt_pcm_regroup could not place this channel, so there is no
             * cursor to report. A number computed from an undefined slot and
             * chunk would become a refill size in func_0023DEB0; the word is
             * left exactly as the guest last saw it instead. */
            if (!g_pcm_cursor_warned[i]) {
                g_pcm_cursor_warned[i] = true;
                rt_log("snd", "pcm channel %d: no cursor written at status +0x%03x, the open "
                              "set is not self consistent so this channel has no derived "
                              "placement (slot 0x%x chunk 0x%x block 0x%x ring 0x%x)",
                    i, off, c.slot, c.chunk, c.block, c.ring);
            }
            continue;
        }
        uint32_t cur = rt_pcm_cursor(c);
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

void rt_snd_pcm_note_iop_write(uint32_t iop_addr, uint32_t size) {
    /* Called for every EE to IOP DMA entry (sif/rpc.cpp). Anything outside the
     * open SgStPcm ring leaves here on the first compare. */
    if (g_pcm_ring_size == 0 || size == 0) return;
    uint64_t first = iop_addr;
    uint64_t last = (uint64_t)iop_addr + size; /* exclusive */
    uint64_t ring_end = (uint64_t)g_pcm_ring_base + g_pcm_ring_size;
    if (last <= g_pcm_ring_base || first >= ring_end) return;
    if (first < g_pcm_ring_base) first = g_pcm_ring_base;
    if (last > ring_end) last = ring_end;
    ++g_pcm_writes;
    uint32_t b0 = (uint32_t)((first - g_pcm_ring_base) / g_pcm_ring_block);
    uint32_t b1 = (uint32_t)((last - 1 - g_pcm_ring_base) / g_pcm_ring_block);
    if (b1 >= (uint32_t)kPcmMaxBlocks) b1 = kPcmMaxBlocks - 1;
    for (uint32_t b = b0; b <= b1; ++b) g_pcm_wr_gen[b] = g_pcm_writes;
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
    constexpr uint32_t kHealthFields = 600; /* about 10 s of virtual time */
    if (++g_health_ticks % kHealthFields == 0) {
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
            uint32_t advanced = v.st_pos - g_last_stream_pos[i];
            g_last_stream_pos[i] = v.st_pos;
            rt_log("snd", "stream voice %d: pitch=0x%04x (%.0f Hz) pos=0x%x "
                          "(+%u bytes/s = %.0f Hz effective) cursor=+0x%05x nonzero=%u",
                i, v.pitch, (double)v.pitch * RT_AUDIO_RATE / 4096.0, v.st_pos,
                advanced, (double)advanced * 28.0 / 16.0,
                stream_cursor(v), nonzero);
        }
        /* Same health line for the SgStPcm channels. The consumed rate is the
         * one number that settles the assumed 48 kHz: if it is right the EE
         * keeps the ring ahead of the cursor and `nonzero` stays high; if the
         * movie's audio is not 48 kHz the two drift and the channel reads a
         * lap of stale bytes. */
        /* Clipping is a property of the whole mix, so it is reported once
         * rather than per channel, and only when it happened. */
        uint64_t clipped = g_clip_frames - g_last_clip;
        uint64_t framed = g_frames_rendered - g_last_frames;
        g_last_clip = g_clip_frames;
        g_last_frames = g_frames_rendered;
        if (clipped) {
            rt_log("snd", "mix clipped %" PRIu64 " of %" PRIu64 " frames since the last report",
                clipped, framed);
        }

        /* SgStPcm channels. `consumed` is unwrapped, so samples-per-field is
         * the real playback rate: 800.8 is 48 kHz, this engine's output rate
         * and the rate the driver is assumed to play at. `pos` alone cannot
         * carry that, because it wraps every ring/block * chunk bytes and a
         * delta taken across a lap boundary aliases.
         *
         * `stale` counts blocks the play cursor re-entered without the EE
         * having written them in the lap in between, witnessed on the SIF DMA
         * path (rt_snd_pcm_note_iop_write), not guessed from content. A
         * nonzero count during the movie means the guest is producing audio
         * slower than the driver consumes it, and what is heard is the
         * previous lap replayed. */
        for (int i = 0; i < RT_PCM_CHANNELS; ++i) {
            const RtPcmChannel& c = g_pcm[i];
            if (!c.open || !c.playing) continue;
            PcmTrack& t = g_pcm_track[i];
            uint64_t advanced = c.consumed - g_last_consumed[i];
            g_last_consumed[i] = c.consumed;
            uint32_t span = c.chunk;
            uint32_t nonzero = 0;
            for (uint32_t b = 0; b < span; ++b) {
                nonzero += rt_iop_ptr(rt_pcm_addr(c, c.pos + b))[0] != 0;
            }
            rt_log("snd", "pcm channel %d: consumed %" PRIu64 " bytes over %u fields "
                          "(%.1f samples per field, 48 kHz is 800.8) pos=0x%x cursor=+0x%05x "
                          "vol=%u/%u peak=%d stale %u of %u blocks entered nonzero=%u/%u",
                i, advanced, kHealthFields, (double)advanced / 2.0 / (double)kHealthFields,
                c.pos, rt_pcm_cursor(c), c.voll, c.volr, t.peak, t.stale, t.entered,
                nonzero, span);
            t.peak = 0;
            t.stale = 0;
            t.entered = 0;
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

/* cmd 0x4A/0x4B/0x4C carry a channel mask in w1, not a channel number:
 * func_0025E118 and func_0025E158 reject an argument with bits 31:24 set and
 * pass the low 32 bits straight through, and the movie sends 1, 2 and 3 for
 * channel 0, channel 1 and both (ito/mpeg/mv_sub.c func_0023E298). Only 16
 * channels exist, so a bit above 15 is reported rather than acted on. */
template <typename Fn>
void for_pcm_mask(uint32_t mask, const char* what, Fn fn) {
    if (mask >> RT_PCM_CHANNELS) {
        rt_log("snd", "pcm %s: mask 0x%08x names channels above %d, which the driver does "
                      "not have; those bits do nothing here", what, mask, RT_PCM_CHANNELS - 1);
    }
    for (int i = 0; i < RT_PCM_CHANNELS; ++i) {
        if (mask & (1u << i)) fn(g_pcm[i], i);
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
        /* ---- PCM streaming (SgStPcm*, retail func_0025E020 to
         * func_0025E280). The attract movie is the only user of this block in
         * this binary; ito/mpeg/mv_sub.c drives it. Per command derivation and
         * the evidence for each field are in SNDN2_NOTES.md. ---- */
        case 0x46: /* Init (func_0025E020, no operands) */
        case 0x47: /* Quit (func_0025E038, no operands) */
            for (auto& c : g_pcm) c = RtPcmChannel();
            for (auto& t : g_pcm_track) t = PcmTrack();
            for (auto& w : g_pcm_cursor_warned) w = false;
            pcm_refresh_ring();
            rt_log("snd", "pcm %s (cmd 0x%02x)", cmd == 0x46 ? "init" : "quit", cmd);
            break;
        case 0x48: { /* Open (func_0025E050): w1 = channel << 24 | flags,
                      * w2 = this channel's first byte in IOP RAM, w3 = the
                      * shared ring size in bytes. */
            uint32_t vc = w1 >> 24;
            uint32_t flags = w1 & 0xFFFFFF;
            uint32_t blk = w1 & 0xFF00;
            if (vc >= (uint32_t)RT_PCM_CHANNELS) {
                rt_log("snd", "pcm open rejected: channel %u is above the driver's 0x10 "
                              "(w1=0x%08x w2=0x%08x w3=0x%08x)", vc, w1, w2, w3);
                return;
            }
            if (w3 == 0 || w2 >= RT_IOP_RAM_SIZE) {
                rt_log("snd", "pcm open rejected: channel %u buffer 0x%06x size 0x%x is not "
                              "inside IOP RAM", vc, w2, w3);
                return;
            }
            if (blk == 0) {
                rt_log("snd", "pcm open: channel %u flags 0x%06x carry no interleave block in "
                              "bits 15:8. The block is what separates the channels inside the "
                              "shared ring and what the EE quantizes its refills to, so this "
                              "channel cannot be placed; it is left closed", vc, flags);
                return;
            }
            RtPcmChannel& c = g_pcm[vc];
            c = RtPcmChannel();
            g_pcm_track[vc] = PcmTrack();
            c.open = true;
            c.flags = flags;
            c.iop_buf = w2;
            c.ring = w3;
            c.block = blk;
            g_pcm_cursor_warned[vc] = false;
            bool consistent = rt_pcm_regroup(g_pcm, RT_PCM_CHANNELS);
            if ((uint64_t)c.base + c.ring > RT_IOP_RAM_SIZE) {
                rt_log("snd", "pcm open rejected: channel %u ring 0x%06x+0x%x leaves IOP RAM",
                    vc, c.base, c.ring);
                c = RtPcmChannel();
                rt_pcm_regroup(g_pcm, RT_PCM_CHANNELS);
                pcm_refresh_ring();
                return;
            }
            pcm_refresh_ring();
            rt_log("snd", "pcm open: channel %u flags=0x%06x iop=0x%06x ring=0x%x block=0x%x "
                          "-> base=0x%06x slot=0x%x chunk=0x%x%s",
                vc, flags, c.iop_buf, c.ring, c.block, c.base, c.slot, c.chunk,
                consistent ? "" : " (INCONSISTENT: the open channels disagree on ring or block "
                                  "size, or a channel's run is not a whole number of 16 bit "
                                  "samples; the interleave below is not trustworthy)");
            break;
        }
        case 0x49: /* Close(channel) (func_0025E0C0: w1 = channel, w2 = w3 = 0) */
            if (w1 >= (uint32_t)RT_PCM_CHANNELS) {
                rt_log("snd", "pcm close: channel %u is above the driver's 0x10", w1);
                return;
            }
            g_pcm[w1] = RtPcmChannel();
            g_pcm_cursor_warned[w1] = false;
            rt_pcm_regroup(g_pcm, RT_PCM_CHANNELS);
            pcm_refresh_ring();
            rt_log("snd", "pcm close: channel %u", w1);
            break;
        case 0x4A: /* ChannelVolume(mask, w2, w3) (func_0025E1E8, each operand
                    * 0..0x7FFF). func_0025E1E8 has four callers in
                    * ito/mpeg/mv_sub.c. The two play paths, func_0023E298 and
                    * its near copy func_0023E368 (restart/resume), send
                    * (1, 0, vol) then (2, vol, 0), one hard pan per channel,
                    * or (3, vol/2, vol/2) when the byte at self+0x58 is set;
                    * vol is the word at self+0x5C. The two teardown paths,
                    * func_0023E228 (0023E240) and func_0023E330 (0023E348),
                    * send (3, 0, 0) as a mute immediately before Stop(3).
                    *
                    * w2 is taken as left and w3 as right, matching cmd 0x01
                    * (w2 = VOLL, w3 = VOLR) and cmd 0x40's (left << 16) | right
                    * in the same command stream. That puts movie channel 0,
                    * the lower address in the ring, on the right. UNRESOLVED:
                    * nothing in the decomp names the two operands, so the
                    * stereo image may be mirrored. Both readings give a
                    * correct stereo field; only the side differs, and the fix
                    * is swapping these two lines. */
            for_pcm_mask(w1, "volume", [w2, w3](RtPcmChannel& c, int i) {
                if (w2 > 0x3FFF || w3 > 0x3FFF) {
                    rt_log("snd", "pcm volume: channel %d asked for %u/%u, above the 0x3FFF "
                                  "that is unity on this engine's scale; carried through as "
                                  "the gain it denotes", i, w2, w3);
                }
                /* func_0025E1E8 rejects anything above 0x7FFF, so this can
                 * only arrive from a malformed command record. The stored
                 * field is 16 bits: say so rather than truncate quietly. */
                if (w2 > 0xFFFF || w3 > 0xFFFF) {
                    rt_log("snd", "pcm volume: channel %d asked for %u/%u, which the retail "
                                  "emitter could never send and does not fit the 16 bit "
                                  "volume field; TRUNCATED to %u/%u", i, w2, w3,
                        (uint32_t)(uint16_t)w2, (uint32_t)(uint16_t)w3);
                }
                c.voll = (uint16_t)w2;
                c.volr = (uint16_t)w3;
            });
            break;
        case 0x4B: /* Play(mask) (func_0025E118) */
            for_pcm_mask(w1, "play", [](RtPcmChannel& c, int i) {
                if (!c.open) {
                    rt_log("snd", "pcm play: channel %d was never opened", i);
                    return;
                }
                c.playing = true;
                /* One shot format evidence for the next Windows log. If the
                 * ring really holds 16 bit PCM the peak is a musical level and
                 * the bytes look like small alternating values; a full scale
                 * peak with high entropy bytes would mean the assumed format
                 * is wrong. */
                const uint8_t* p = rt_iop_ptr(rt_pcm_addr(c, c.pos));
                int32_t peak = 0;
                for (uint32_t b = 0; b + 1 < c.chunk; b += 2) {
                    const uint8_t* q = rt_iop_ptr(rt_pcm_addr(c, c.pos + b));
                    int32_t v = (int16_t)((uint16_t)q[0] | ((uint16_t)q[1] << 8));
                    if (v < 0) v = -v;
                    if (v > peak) peak = v;
                }
                rt_log("snd", "pcm play: channel %d iop=0x%06x base=0x%06x slot=0x%x "
                              "chunk=0x%x block=0x%x ring=0x%x vol=%u/%u first bytes "
                              "%02x %02x %02x %02x %02x %02x %02x %02x, peak |s16| over the "
                              "first run = %d",
                    i, c.iop_buf, c.base, c.slot, c.chunk, c.block, c.ring, c.voll, c.volr,
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], peak);
            });
            break;
        case 0x4C: /* Stop(mask) (func_0025E158) */
            for_pcm_mask(w1, "stop", [](RtPcmChannel& c, int) { c.playing = false; });
            break;
        case 0x4D: { /* (channel, ring offset) (func_0025E198: w1 = channel
                      * < 0x10, w2 <= 0x1FFFFF, w3 = 0). func_0025E198 has two
                      * callers, both in ito/mpeg/mv_sub.c and both sending
                      * (0, 0) then (1, 0) immediately before Play(3):
                      * func_0023E298 (0023E2AC/0023E2B8) and its near copy
                      * func_0023E368 (0023E37C/0023E388), the restart/resume
                      * path. So the only observed effect is "start this
                      * channel at the bottom of the ring".
                      * INFERRED: that w2 is a ring offset in the same units
                      * as the cursor at status +0x180. A nonzero value has
                      * never been seen and is reported rather than acted on
                      * silently. */
            if (w1 >= (uint32_t)RT_PCM_CHANNELS) {
                rt_log("snd", "pcm set position: channel %u is above the driver's 0x10", w1);
                return;
            }
            RtPcmChannel& c = g_pcm[w1];
            if (!c.open) {
                /* Nothing to rewind, and writing pos would leave a value in a
                 * channel the next open resets anyway. */
                rt_log("snd", "pcm set position: channel %u asked for 0x%x but was never "
                              "opened", w1, w2);
                return;
            }
            if (w2 == 0) {
                rt_log("snd", "pcm set position: channel %u to 0, the bottom of the ring "
                              "(was pos=0x%x)", w1, c.pos);
                c.pos = 0;
                break;
            }
            rt_log("snd", "pcm set position: channel %u asked for 0x%x, and only 0 has ever "
                          "been observed, so the meaning of a nonzero operand is unverified. "
                          "Reading it as a ring byte offset (block 0x%x, chunk 0x%x)",
                w1, w2, c.block, c.chunk);
            if (c.block == 0 || c.chunk == 0) return;
            if (w2 % c.block != 0) {
                rt_log("snd", "pcm set position: 0x%x is not a whole number of 0x%x blocks; "
                              "the remainder has no defined channel", w2, c.block);
            }
            c.pos = (w2 / c.block) * c.chunk;
            break;
        }
        case 0x4E: /* (func_0025E100: w1 forwarded with no validation at all).
                    * The movie sends 8, once, right after opening both
                    * channels (ito/mpeg/mv_sub.c func_0023D8A8), and nothing
                    * else in this binary calls it. Not established. */
            rt_log("snd", "pcm cmd 0x4E(0x%08x) is not modelled: the operand's meaning is not "
                          "established from the decomp. The movie sends it once with 8 after "
                          "opening its two channels", w1);
            break;
        case 0x4F: /* (func_0025E280: w1 = mask, w2 <= 0x1FFFFF, w3 < 2).
                    * Never sent by this binary in any observed run. */
            rt_log("snd", "pcm cmd 0x4F is not modelled: w1=0x%08x w2=0x%08x w3=0x%08x",
                w1, w2, w3);
            break;
        default:
            rt_log("snd", "cmd 0x%02x NOT MODELED: w1=0x%08x w2=0x%08x w3=0x%08x",
                cmd, w1, w2, w3);
            break;
    }
}
