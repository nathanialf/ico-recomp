/* host/audio.cpp: host audio output (SDL3 stream + WAV capture). See audio.h. */
#include "audio.h"

#include "portable.h"

#include "../runtime.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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
        rt_log("audio", "WARNING: ICORECOMP_WAV_CAPTURE=%s: fopen failed; capture disabled", path);
        return;
    }
    wav_write_header();
    rt_log("audio", "WAV capture -> %s (48000 Hz stereo s16)", path);
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

/* ---- SDL3 sink ----------------------------------------------------------- */

#ifdef ICORECOMP_SND_SDL
SDL_AudioStream* g_sdl_stream = nullptr;
uint64_t g_sdl_dropped = 0;

/* Cap the device queue at ~1 s. Headless runs outrun real time; without a
 * cap the stream queue grows without bound. */
constexpr uint32_t kMaxQueuedBytes = RT_AUDIO_RATE * 2 * sizeof(float);

void sdl_open() {
    if (std::getenv("ICORECOMP_NO_AUDIO")) {
        rt_log("audio", "ICORECOMP_NO_AUDIO set: SDL audio disabled");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log("audio", "SDL audio init failed (%s); playback disabled", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec;
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = (int)RT_AUDIO_RATE;
    g_sdl_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                             &spec, nullptr, nullptr);
    if (!g_sdl_stream) {
        rt_log("audio", "SDL_OpenAudioDeviceStream failed (%s); playback disabled", SDL_GetError());
        return;
    }
    /* Prime with a few fields of silence. The device starts consuming the
     * moment it is resumed, and the first mix does not arrive until the
     * game issues its first sndn2 flush; without a cushion that gap is an
     * immediate underrun, and every later hitch has nothing to absorb it. */
    {
        constexpr uint32_t kPrimeFrames = RT_AUDIO_RATE / 20; /* 50 ms */
        static float silence[kPrimeFrames * 2] = {0.0f};
        SDL_PutAudioStreamData(g_sdl_stream, silence, (int)sizeof silence);
    }
    SDL_ResumeAudioStreamDevice(g_sdl_stream);
    rt_log("audio", "SDL3 audio stream open (driver=%s, 48000 Hz stereo f32, "
                    "50 ms primed)", SDL_GetCurrentAudioDriver());
}

/* Queue-depth telemetry. The sink previously reported only the overrun
 * case, so a starving queue (the audible one: the device runs out and the
 * gap is a click) was invisible. */
uint32_t g_q_min = UINT32_MAX;
uint32_t g_q_max = 0;
uint64_t g_q_sum = 0;
uint64_t g_q_n = 0;
uint64_t g_underruns = 0;

void sdl_submit(const float* lr, uint32_t frames) {
    if (!g_sdl_stream) return;
    int queued = SDL_GetAudioStreamQueued(g_sdl_stream);
    if (queued >= 0) {
        uint32_t q = (uint32_t)queued;
        if (q < g_q_min) g_q_min = q;
        if (q > g_q_max) g_q_max = q;
        g_q_sum += q;
        ++g_q_n;
        if (q == 0) ++g_underruns;
    }
    if (queued >= 0 && (uint32_t)queued > kMaxQueuedBytes) {
        g_sdl_dropped += frames;
        if ((g_sdl_dropped & (g_sdl_dropped - 1)) == 0) { /* power-of-two repeats */
            rt_log("audio", "SDL queue full (running ahead of real time); %" PRIu64
                " frames dropped so far", g_sdl_dropped);
        }
        return;
    }
    SDL_PutAudioStreamData(g_sdl_stream, lr, (int)(frames * 2 * sizeof(float)));
}
#endif /* ICORECOMP_SND_SDL */

uint64_t g_total_frames = 0;

} // namespace

uint64_t g_window_frames_submitted = 0;

uint64_t rt_audio_window_frames() {
    uint64_t v = g_window_frames_submitted;
    g_window_frames_submitted = 0;
    return v;
}

int rt_audio_queued_frames() {
#ifdef ICORECOMP_SND_SDL
    if (!g_sdl_stream) return -1;
    int queued = SDL_GetAudioStreamQueued(g_sdl_stream);
    if (queued < 0) return -1;
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
    rt_log("audio", "built without SDL audio; %s",
        g_wav ? "WAV capture only" : "output discarded (set ICORECOMP_WAV_CAPTURE=path)");
#endif
    std::atexit(rt_audio_shutdown);
}

void rt_audio_submit(const float* lr, uint32_t frames) {
    if (!g_inited || frames == 0) return;
    g_total_frames += frames;
    g_window_frames_submitted += frames;
    wav_submit(lr, frames);
#ifdef ICORECOMP_SND_SDL
    sdl_submit(lr, frames);
#endif
}

void rt_audio_shutdown() {
    if (g_wav) {
        wav_write_header();
        std::fclose(g_wav);
        g_wav = nullptr;
        rt_log("audio", "WAV capture closed: %" PRIu64 " frames (%.2f s)",
            g_wav_frames, (double)g_wav_frames / RT_AUDIO_RATE);
    }
#ifdef ICORECOMP_SND_SDL
    if (g_sdl_stream) {
        SDL_DestroyAudioStream(g_sdl_stream);
        g_sdl_stream = nullptr;
    }
#endif
}
