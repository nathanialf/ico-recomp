/* host/audio.cpp: host audio output (SDL3 stream + WAV capture). See audio.h. */
#include "audio.h"

#include "portable.h"
#include "settings.h"

#include "../runtime.h"
#include "../snd/chime.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef ICORECOMP_SND_SDL
#include <SDL3/SDL.h>
#endif

namespace {

bool g_inited = false;

/* ---- WAV capture sink ---------------------------------------------------- */

std::FILE* g_wav = nullptr;
uint64_t g_wav_frames = 0;      /* stereo frames written */
uint64_t g_wav_frames_flushed = 0;

void wav_write_header() {
    /* 44-byte canonical PCM header, sizes patched as data grows. */
    uint32_t data_bytes32 = (uint32_t)(g_wav_frames * 4u); /* 2ch * s16 */
    uint32_t riff = 36u + data_bytes32;
    uint8_t h[44];
    std::memcpy(h + 0, "RIFF", 4);
    std::memcpy(h + 4, &riff, 4);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmt_size = 16;
    std::memcpy(h + 16, &fmt_size, 4);
    uint16_t fmt_tag = 1, channels = 2, block_align = 4, bits = 16;
    uint32_t rate = RT_AUDIO_RATE, byte_rate = RT_AUDIO_RATE * 4u;
    std::memcpy(h + 20, &fmt_tag, 2);
    std::memcpy(h + 22, &channels, 2);
    std::memcpy(h + 24, &rate, 4);
    std::memcpy(h + 28, &byte_rate, 4);
    std::memcpy(h + 32, &block_align, 2);
    std::memcpy(h + 34, &bits, 2);
    std::memcpy(h + 36, "data", 4);
    std::memcpy(h + 40, &data_bytes32, 4);
    int64_t pos = rt_ftell64(g_wav);
    rt_fseek64(g_wav, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), g_wav);
    if (pos > (int64_t)sizeof(h)) rt_fseek64(g_wav, pos, SEEK_SET);
}

void wav_open() {
    /* Opt in: ICORECOMP_WAV_CAPTURE=path captures the mixer's output before
     * the device sees it, which is what separates a bad mix from a bad
     * handoff. 192 KB per second of run. */
    const char* path = std::getenv("ICORECOMP_WAV_CAPTURE");
    if (!path || !path[0]) return;
    g_wav = rt_fopen_utf8(path, "wb");
    if (!g_wav) {
        rt_log_warn("audio", "WARNING: ICORECOMP_WAV_CAPTURE=%s: fopen failed; capture disabled", path);
        return;
    }
    wav_write_header();
    rt_log_info("audio", "WAV capture -> %s (48000 Hz stereo s16)", path);
}

void wav_submit(const float* lr, uint32_t frames) {
    if (!g_wav) return;
    /* Convert in small stack chunks to keep this allocation-free. */
    int16_t buf[512 * 2];
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > 512) n = 512;
        for (uint32_t i = 0; i < n * 2; ++i) {
            float v = lr[(done + i / 2) * 2 + (i & 1)] * 32767.0f;
            /* explicit index math above keeps L/R order; clamp to s16 */
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            buf[i] = (int16_t)v;
        }
        std::fwrite(buf, 4, n, g_wav);
        done += n;
        g_wav_frames += n;
    }
    /* Keep the header valid about once a second so an abort mid-run still
     * leaves a parseable WAV. */
    if (g_wav_frames - g_wav_frames_flushed >= RT_AUDIO_RATE) {
        wav_write_header();
        std::fflush(g_wav);
        g_wav_frames_flushed = g_wav_frames;
    }
}

/* Said once when a WAV capture is running with a category gain away from
 * 100. The capture is the headless verification baseline and is supposed to
 * be a function of the sound engine alone; audio.master_volume and
 * audio.mute keep that true by applying at the SDL sink below, but the three
 * category gains cannot, because only the engine knows which category a
 * sample belongs to. So the capture does carry them, and this line says so
 * with the numbers in it rather than leaving a quiet difference in a
 * baseline comparison. */
bool g_capture_gain_said = false;

void warn_capture_gains() {
    if (!g_wav || g_capture_gain_said) return;
    const RtSettings& s = rt_settings();
    if (s.audio.music_volume == 100 && s.audio.effects_volume == 100 &&
        s.audio.movie_volume == 100) {
        return;
    }
    g_capture_gain_said = true;
    rt_log_warn("audio", "WAV capture: audio.music_volume=%d effects_volume=%d movie_volume=%d."
                         " These gains are applied where the engine sums each category, so this"
                         " capture is not the engine's own mix. Set all three to 100 for a"
                         " baseline capture. Said once.",
        s.audio.music_volume, s.audio.effects_volume, s.audio.movie_volume);
}

/* ---- SDL3 sink ----------------------------------------------------------- */

#ifdef ICORECOMP_SND_SDL
SDL_AudioStream* g_sdl_stream = nullptr;
uint64_t g_sdl_dropped = 0;

/* Cap the device queue at ~1 s. Headless runs outrun real time; without a
 * cap the stream queue grows without bound. */
constexpr uint32_t kMaxQueuedBytes = RT_AUDIO_RATE * 2 * sizeof(float);

/* Frames of silence held ahead of the device. The device starts consuming
 * the moment it is resumed and a mix only arrives when the game issues its
 * sndn2 flush, so without a cushion that gap is an immediate underrun and
 * every later hitch has nothing to absorb it.
 *
 * The size is RT_AUDIO_CUSHION_FRAMES, the same constant hw/gspriv.cpp's
 * pace_field uses as its target queue depth, so the prime lands the queue
 * exactly where the pacer wants to hold it. This is a host-side pacing
 * decision: it changes when the host produces a field, never a value the
 * game supplied or computed. */
constexpr uint32_t kPrimeFrames = RT_AUDIO_CUSHION_FRAMES;

/* Called once, at device open, and never again while the run continues.
 * Re-priming a queue that has run empty would be worse than the gap it
 * tries to cover: hw/gspriv.cpp's pace_field makes this queue the master
 * clock and steers the field period by its depth against a
 * RT_AUDIO_CUSHION_FRAMES target, so pushing a full cushion of silence in
 * behind a starving queue puts the depth back on target and the limiter
 * stops correcting, while the silence itself is spliced ahead of the real
 * mix as an audible gap and a permanent 100 ms shift that stacks on every
 * repeat. A submit that finds the queue empty is counted in the underrun
 * stat instead, and the profile block's queue-depth line reports it. */
void prime_cushion() {
    if (!g_sdl_stream) return;
    static float silence[kPrimeFrames * 2] = {0.0f};
    SDL_PutAudioStreamData(g_sdl_stream, silence, (int)sizeof silence);
}

void sdl_open() {
    if (const char* e = std::getenv("ICORECOMP_NO_AUDIO")) {
        /* audio.mute's env twin. The env var's actual effect predates
         * settings.json and is stronger than a plain mute (the stream never
         * opens at all); it still wins over the file per the env-precedence
         * rule, so only the "value ignored" log is new here. */
        if (!rt_settings().audio.mute) {
            rt_log_warn("audio", "audio.mute: using ICORECOMP_NO_AUDIO=%s, settings.json value ignored", e);
        }
        rt_log_info("audio", "ICORECOMP_NO_AUDIO set: SDL audio disabled");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log_warn("audio", "SDL audio init failed (%s); playback disabled", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = (int)RT_AUDIO_RATE;
    g_sdl_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                             &spec, nullptr, nullptr);
    if (!g_sdl_stream) {
        rt_log_warn("audio", "SDL_OpenAudioDeviceStream failed (%s); playback disabled", SDL_GetError());
        return;
    }
    /* Prime with a few fields of silence. The device starts consuming the
     * moment it is resumed, and the first mix does not arrive until the
     * game issues its first sndn2 flush; without a cushion that gap is an
     * immediate underrun, and every later hitch has nothing to absorb it. */
    prime_cushion();
    SDL_ResumeAudioStreamDevice(g_sdl_stream);
    rt_log_info("audio", "SDL3 audio stream open (driver=%s, 48000 Hz stereo f32, "
                    "%.0f ms primed)", SDL_GetCurrentAudioDriver(),
        1000.0 * (double)kPrimeFrames / (double)RT_AUDIO_RATE);
}

/* Queue-depth telemetry. The sink previously reported only the overrun
 * case, so a starving queue (the audible one: the device runs out and the
 * gap is a click) was invisible. */
uint32_t g_q_min = UINT32_MAX;
uint32_t g_q_max = 0;
uint64_t g_q_sum = 0;
uint64_t g_q_n = 0;
uint64_t g_underruns = 0;
/* The same count for the whole run. rt_audio_queue_stats clears g_underruns
 * on read, and prof.h calls it once per profile window, so g_underruns is
 * only the residue since the last window and the shutdown line has to
 * report this one instead. */
uint64_t g_underruns_total = 0;

/* One-shot latches for the SDL failures below. A sound device that starts
 * refusing data does it every field, so each of these says it once with the
 * reason and then counts; a line per field would bury the reason it was
 * worth saying at all. */
bool g_logged_queue_query_failed = false;
bool g_logged_put_failed = false;
uint64_t g_put_failures = 0;

/* Every SDL_PutAudioStreamData in this file goes through here. The return
 * value used to be dropped at three call sites, which meant a device that
 * had stopped accepting data produced silence and not one word in the log:
 * the player hears nothing and the file says the mix was submitted. */
void put_stream(const void* data, int bytes) {
    if (SDL_PutAudioStreamData(g_sdl_stream, data, bytes)) return;
    ++g_put_failures;
    if (!g_logged_put_failed) {
        g_logged_put_failed = true;
        rt_log_warn("audio", "SDL_PutAudioStreamData refused %d bytes (%s). The mix for this"
            " field is lost and the run is playing silence for as long as this lasts. Said"
            " once; the total is in the audio summary at exit.", bytes, SDL_GetError());
    }
}

void sdl_submit(const float* lr, uint32_t frames) {
    if (!g_sdl_stream) return;
    int queued = SDL_GetAudioStreamQueued(g_sdl_stream);
    if (queued < 0 && !g_logged_queue_query_failed) {
        /* Not cosmetic: hw/gspriv.cpp's frame limiter locks the field period
         * to this queue depth, and a query that fails takes the audio device
         * out of the pacing loop for the rest of the run. That is a change
         * in how the whole port is paced, and it must not happen quietly. */
        g_logged_queue_query_failed = true;
        rt_log_warn("audio", "SDL_GetAudioStreamQueued failed (%s). The frame limiter locks the"
            " field period to this queue, so from here it paces on the host clock alone and the"
            " audio device is no longer the master clock. Said once.", SDL_GetError());
    }
    if (queued >= 0) {
        uint32_t q = (uint32_t)queued;
        if (q < g_q_min) g_q_min = q;
        if (q > g_q_max) g_q_max = q;
        g_q_sum += q;
        ++g_q_n;
        if (q == 0) {
            /* The cushion is gone: the device has played everything and is
             * about to run dry between this submit and the next. Counted,
             * not refilled (see prime_cushion): silence pushed in here
             * would hide the starvation from the limiter that paces the
             * port off this queue. */
            ++g_underruns;
            ++g_underruns_total;
        }
    }
    if (queued >= 0 && (uint32_t)queued > kMaxQueuedBytes) {
        g_sdl_dropped += frames;
        if ((g_sdl_dropped & (g_sdl_dropped - 1)) == 0) { /* power-of-two repeats */
            rt_log_warn("audio", "SDL queue full (running ahead of real time); %" PRIu64
                " frames dropped so far", g_sdl_dropped);
        }
        return;
    }

    /* Host master gain applies HERE ONLY, to the samples handed to this SDL
     * sink. wav_submit (above, in rt_audio_submit, called before this with
     * the same unscaled `lr`) must never see it: the WAV capture is the
     * headless verification baseline and has to stay a function of the
     * sound engine alone, not of a host output setting. At 100/unmuted this
     * skips the multiply, so the stream is bit-identical to a build with no
     * volume control. */
    const RtSettings& s = rt_settings();
    const float gain = s.audio.mute ? 0.0f : (float)s.audio.master_volume / 100.0f;
    if (gain == 1.0f) {
        put_stream(lr, (int)(frames * 2 * sizeof(float)));
        return;
    }
    float scaled[512 * 2];
    uint32_t done = 0;
    while (done < frames) {
        uint32_t n = frames - done;
        if (n > 512) n = 512;
        for (uint32_t i = 0; i < n * 2; ++i) scaled[i] = lr[done * 2 + i] * gain;
        put_stream(scaled, (int)(n * 2 * sizeof(float)));
        done += n;
    }
}

/* ---- the achievement unlock chime ---------------------------------------
 *
 * A host-side mixer stage, and the only one: the game's own samples are
 * summed with a synthesised chime (snd/chime.h) on the way to the device
 * and nowhere else. When nothing is queued, chime_mix returns the caller's
 * own pointer and not one sample is read, written or copied, so a run with
 * no unlock is sample-identical to a build without any of this.
 *
 * Everything here is main-thread state, like the rest of this file:
 * rt_audio_play_chime is called from the achievement observer, which runs
 * on the guest field tick, and rt_audio_submit from the sound engine's
 * flush. Both are the main OS thread. */

std::vector<float> g_chime;         /* one voice, mono, at RT_AUDIO_RATE */
std::vector<float> g_chime_scratch; /* the summed field handed to the sink */
uint32_t g_chime_pos = 0;           /* frames of the playing voice already mixed */
uint32_t g_chime_queued = 0;        /* voices waiting behind it */
uint32_t g_chime_gap = 0;           /* frames of silence before the next one */

/* Silence between queued chimes, so two unlocks in the same field are heard
 * as two. */
constexpr uint32_t kChimeGapFrames = RT_AUDIO_RATE / 10;
/* A ceiling on the queue. The largest real burst is the derived unlocks at
 * the ending, a handful in one field; anything past this is dropped rather
 * than played minutes later. */
constexpr uint32_t kChimeMaxQueued = 8;

/* Counted so the clamp above is not silent. Said once at warn, then only
 * kept, because a chime that clips clips for its whole 304 ms and a line a
 * buffer would bury the log. */
uint64_t g_chime_clipped = 0;
bool g_chime_clip_said = false;

bool chime_active() {
    return g_chime_pos < (uint32_t)g_chime.size() || g_chime_queued != 0;
}

/* Note on ordering, because it is a real divergence and not a bug worth
 * reordering the pipeline for: this advances the chime's play position
 * before sdl_submit decides whether to drop the buffer (queue full) or
 * whether there is a device at all. A chime queued during a fast-forward,
 * or on a build with no playback device, is therefore consumed without
 * being heard. Nothing the game supplied is affected either way; only the
 * host chime is.
 *
 * Sums the queued chime into `lr` and returns the buffer to submit. Returns
 * `lr` itself, untouched, whenever the queue is empty or the chime gain
 * rounds to zero: at gain 0 the sum is the game's own mix and copying it
 * through the scratch buffer would still saturate it, which is a change to
 * a value the game supplied for no audible gain at all. */
const float* chime_mix(const float* lr, uint32_t frames) {
    if (!chime_active()) return lr;
    const RtSettings& s = rt_settings();
    const float gain = (float)s.audio.chime_volume / 100.0f;
    if (!(gain > 0.0f)) return lr;
    const uint32_t len = (uint32_t)g_chime.size();
    if (g_chime_scratch.size() < (size_t)frames * 2) g_chime_scratch.resize((size_t)frames * 2);
    uint32_t clipped = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        float c = 0.0f;
        if (g_chime_pos < len) {
            c = g_chime[g_chime_pos++];
            if (g_chime_pos == len && g_chime_queued != 0) g_chime_gap = kChimeGapFrames;
        } else if (g_chime_queued != 0) {
            if (g_chime_gap != 0) {
                --g_chime_gap;
            } else {
                --g_chime_queued;
                g_chime_pos = 0;
                c = g_chime[g_chime_pos++];
            }
        }
        c *= gain;
        for (uint32_t ch = 0; ch < 2; ++ch) {
            /* Saturating sum. The chime carries 6 dB of headroom of its own
             * (rt_chime::kPeak), so this clamp is the guard on a game mix
             * that is already at full scale, not the normal path. It does
             * change a value the game supplied, so it is counted and said
             * out loud once rather than being a silent safety net. */
            float v = lr[i * 2 + ch] + c;
            if (v > 1.0f) { v = 1.0f; ++clipped; }
            if (v < -1.0f) { v = -1.0f; ++clipped; }
            g_chime_scratch[(size_t)i * 2 + ch] = v;
        }
    }
    if (clipped != 0) {
        g_chime_clipped += clipped;
        if (!g_chime_clip_said) {
            g_chime_clip_said = true;
            rt_log_warn("audio", "the achievement chime clipped the game's own mix: %u sample(s)"
                                 " in this buffer saturated at full scale. The game's samples are"
                                 " altered for as long as a chime plays; turn achievements.sound"
                                 " off, or audio.chime_volume down, to leave them alone.",
                (unsigned)clipped);
        }
    }
    return g_chime_scratch.data();
}
#endif /* ICORECOMP_SND_SDL */

uint64_t g_total_frames = 0;

} // namespace

uint64_t g_window_frames_submitted = 0;

uint64_t rt_audio_total_frames() { return g_total_frames; }

uint64_t rt_audio_window_frames() {
    uint64_t v = g_window_frames_submitted;
    g_window_frames_submitted = 0;
    return v;
}

int rt_audio_queued_frames() {
#ifdef ICORECOMP_SND_SDL
    if (!g_sdl_stream) return -1;
    int queued = SDL_GetAudioStreamQueued(g_sdl_stream);
    if (queued < 0) {
        /* Same failure sdl_submit reports, reached from the frame limiter
         * instead. One latch covers both, so whichever caller sees it first
         * is the one that says it. */
        if (!g_logged_queue_query_failed) {
            g_logged_queue_query_failed = true;
            rt_log_warn("audio", "SDL_GetAudioStreamQueued failed (%s); the frame limiter is"
                " pacing on the host clock alone from here. Said once.", SDL_GetError());
        }
        return -1;
    }
    return queued / (int)(2 * sizeof(float));
#else
    return -1;
#endif
}

void rt_audio_queue_stats(uint32_t* min_f, uint32_t* mean_f, uint32_t* max_f,
                          uint64_t* underruns) {
#ifdef ICORECOMP_SND_SDL
    const uint32_t bpf = 2 * (uint32_t)sizeof(float);
    *min_f = g_q_min == UINT32_MAX ? 0 : g_q_min / bpf;
    *max_f = g_q_max / bpf;
    *mean_f = g_q_n ? (uint32_t)(g_q_sum / g_q_n) / bpf : 0;
    *underruns = g_underruns;
    g_q_min = UINT32_MAX;
    g_q_max = 0;
    g_q_sum = 0;
    g_q_n = 0;
    g_underruns = 0;
#else
    *min_f = *mean_f = *max_f = 0;
    *underruns = 0;
#endif
}

void rt_audio_init() {
    if (g_inited) return;
    g_inited = true;
    wav_open();
#ifdef ICORECOMP_SND_SDL
    sdl_open();
#else
    rt_log_warn("audio", "built without SDL audio; %s",
        g_wav ? "WAV capture only" : "output discarded (set ICORECOMP_WAV_CAPTURE=path)");
#endif
    std::atexit(rt_audio_shutdown);
}

void rt_audio_play_chime() {
#ifdef ICORECOMP_SND_SDL
    if (g_chime.empty()) {
        /* Rendered once, at the output rate, on the first unlock of the run:
         * a few hundred sines, and nothing at all in a run that unlocks
         * nothing or has the key off. */
        g_chime.resize(rt_chime::frame_count(RT_AUDIO_RATE));
        rt_chime::render(g_chime.data(), (uint32_t)g_chime.size(), RT_AUDIO_RATE);
        /* Idle, not playing: the cursor is only ever inside the buffer
         * while a voice is sounding, which is what chime_active reads. */
        g_chime_pos = (uint32_t)g_chime.size();
        rt_log_info("audio", "unlock chime synthesised: %zu frames (%.0f ms) at %u Hz, peak %.2f",
            g_chime.size(), 1000.0 * (double)g_chime.size() / (double)RT_AUDIO_RATE,
            (unsigned)RT_AUDIO_RATE, (double)rt_chime::kPeak);
    }
    if (chime_active()) {
        if (g_chime_queued < kChimeMaxQueued) ++g_chime_queued;
        return;
    }
    g_chime_pos = 0;
    g_chime_gap = 0;
#endif
}

void rt_audio_submit(const float* lr, uint32_t frames) {
    if (!g_inited || frames == 0) return;
    g_total_frames += frames;
    g_window_frames_submitted += frames;
    /* Without the chime, always: the WAV capture is the verification
     * baseline (audio.h) and the chime is a host sound.
     *
     * The three category gains are not host-side at this point, though.
     * They are the one host output gain the engine has to apply itself,
     * because it is the only place that knows which category a sample came
     * from (snd/engine.cpp), so a capture taken while one of them is away
     * from 100 carries it. Said once, loudly, rather than left to be
     * discovered in a diff against an older baseline. */
    warn_capture_gains();
    wav_submit(lr, frames);
#ifdef ICORECOMP_SND_SDL
    sdl_submit(chime_mix(lr, frames), frames);
#endif
}

void rt_audio_shutdown() {
    if (g_wav) {
        wav_write_header();
        std::fclose(g_wav);
        g_wav = nullptr;
        rt_log_info("audio", "WAV capture closed: %" PRIu64 " frames (%.2f s)",
            g_wav_frames, (double)g_wav_frames / RT_AUDIO_RATE);
    }
#ifdef ICORECOMP_SND_SDL
    if (g_chime_clipped != 0) {
        rt_log_info("audio", "the achievement chime saturated %" PRIu64 " sample(s) of the game's"
                             " own mix over this run", g_chime_clipped);
    }
    /* warn, not info: a run that lost mix to a device that would not take it
     * played silence for that long, and the count is the size of what the
     * player did not hear. Only when it happened, so a healthy run says
     * nothing. */
    if (g_put_failures != 0) {
        rt_log_warn("audio", "the sound device refused %" PRIu64 " submissions over this run;"
                             " that much of the mix was never played", g_put_failures);
    }
    if (g_sdl_dropped != 0) {
        rt_log_warn("audio", "%" PRIu64 " frames were dropped because the queue was full (the"
                             " port ran ahead of real time)", g_sdl_dropped);
    }
    if (g_underruns_total != 0) {
        rt_log_warn("audio", "the sound device ran dry %" PRIu64 " time(s): the queue was empty"
                             " at a submit, which is a gap the player hears", g_underruns_total);
    }
    if (g_sdl_stream) {
        SDL_DestroyAudioStream(g_sdl_stream);
        g_sdl_stream = nullptr;
    }
#endif
}
