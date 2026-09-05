/* host/audio.h: host audio output layer for the native sound engine.
 *
 * The engine (snd/engine.cpp) renders 48 kHz stereo float on the main
 * thread (guest threads are minicoro coroutines; the sndn2 RPC handler that
 * drives rendering runs there too). This layer fans the rendered audio out
 * to up to two sinks:
 *
 *   - An SDL3 audio stream (when the build has SDL3 and a playback device
 *     opens). SDL_AudioStream does its own cross-thread buffering, so the
 *     main thread queues samples; the device thread drains them. When
 *     the runtime outruns real time (headless, no vsync throttle) the queue
 *     is capped and excess audio is dropped with a counter.
 *   - A WAV file (ICORECOMP_WAV_CAPTURE=path): 48 kHz stereo s16, header
 *     kept valid incrementally so even an aborted run leaves a readable
 *     file. This is the headless verification path.
 *
 * Both sinks are optional; with neither, submits are counted and discarded.
 * Weak-init: failure to open SDL audio is a log line, never fatal.
 *
 * Runtime-internal, NOT part of the ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_HOST_AUDIO_H
#define ICORECOMP_HOST_AUDIO_H

#include <cstdint>

constexpr uint32_t RT_AUDIO_RATE = 48000;

/* The audio cushion: frames of sound held ahead of the device, which is
 * both the prime size at device open (host/audio.cpp) and the depth the
 * frame pacer steers the queue back to (hw/gspriv.cpp pace_field). 4800
 * frames is 100 ms at 48 kHz, which is six NTSC fields of mix or five PAL
 * ones. It stays a duration rather than a field count, because what it
 * buys is host scheduling latency, which does not change when the guest
 * changes video mode; a US run therefore primes and steers to exactly the
 * frame count it always did. The two uses have to
 * be the same number: the pacer's target is what the prime fills, and the
 * pacer will run unpaced to repay at most this much missing audio after a
 * stall. Host-side pacing only; no guest-visible value depends on it. */
constexpr uint32_t RT_AUDIO_CUSHION_FRAMES = 4800;

/* Idempotent. Called lazily by the engine on the first rendered buffer. */
void rt_audio_init();

/* frames interleaved stereo float in [-1, 1]. Main thread only.
 *
 * What arrives here already carries the three host category gains
 * (audio.music_volume, audio.effects_volume, audio.movie_volume), because
 * only snd/engine.cpp knows which category a sample belongs to; this layer
 * adds audio.master_volume and audio.mute, and only on the device path. So
 * a WAV capture is the engine's own mix whenever those three are at their
 * default of 100, and rt_audio_submit says so out loud when they are not. */
void rt_audio_submit(const float* lr, uint32_t frames);

/* Queues one achievement unlock chime (snd/chime.h), synthesised on the
 * first call and mixed into the samples handed to the device by the next
 * submits, at audio.chime_volume. Main thread only, like rt_audio_submit.
 *
 * Host-side sound, and deliberately only on the device path: the WAV
 * capture is the headless verification baseline and stays a function of the
 * sound engine alone, so it never carries the chime. The master volume and
 * audio.mute apply to the sum, so a muted run plays no chime either. While
 * nothing is queued the mixer stage is skipped outright and the game's own
 * samples reach the device untouched.
 *
 * A call with a chime already playing queues the new one behind it rather
 * than stacking it on top, so the several derived unlocks that can land in
 * one field are chimes in a row and not one several times as loud. */
void rt_audio_play_chime();

/* Flushes and closes the WAV sink; registered via atexit() by rt_audio_init
 * but callable directly. Safe to call more than once. */
void rt_audio_shutdown();

/* Device queue depth in frames, or -1 when there is no device. The frame
 * limiter uses this to lock the emulation rate to the audio clock: the two
 * clocks are independent crystals, so pacing purely on the host wall clock
 * drifts and eventually starves or overruns the device. */
int rt_audio_queued_frames();

/* Frames handed to the device since the last call. Divided by the window's
 * wall time this is the true playback rate: above the device rate the sound
 * plays fast, below it the device starves. */
uint64_t rt_audio_window_frames();

/* Frames handed to the device since startup, never cleared. The pacer
 * compares two readings a field apart to know whether the sound task fed
 * the device at all in between (hw/gspriv.cpp pace_field). */
uint64_t rt_audio_total_frames();

/* Window statistics for the profile report, cleared on read: minimum,
 * mean and maximum queue depth in frames, plus the number of submits that
 * found the queue empty. */
void rt_audio_queue_stats(uint32_t* min_f, uint32_t* mean_f, uint32_t* max_f,
                          uint64_t* underruns);

#endif /* ICORECOMP_HOST_AUDIO_H */
