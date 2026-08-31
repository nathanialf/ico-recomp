/* host/audio.h: host audio output layer for the native sound engine.
 *
 * The engine (snd/engine.cpp) renders 48 kHz stereo float on the main
 * thread (guest threads are minicoro coroutines; the sndn2 RPC handler that
 * drives rendering runs there too). This layer fans the rendered audio out
 * to up to two sinks:
 *
 *   - An SDL3 audio stream (when the build has SDL3 and a playback device
 *     opens). SDL_AudioStream does its own cross-thread buffering, so the
 *     main thread just queues samples; the device thread drains them. When
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

/* Idempotent. Called lazily by the engine on the first rendered buffer. */
void rt_audio_init();

/* frames interleaved stereo float in [-1, 1]. Main thread only. */
void rt_audio_submit(const float* lr, uint32_t frames);

/* Flushes and closes the WAV sink; registered via atexit() by rt_audio_init
 * but callable directly. Safe to call more than once. */
void rt_audio_shutdown();

#endif /* ICORECOMP_HOST_AUDIO_H */
