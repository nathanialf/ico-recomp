/* snd/chime.h: the achievement unlock chime, synthesised at run time.
 *
 * No audio asset ships with this port and none may: every byte in the
 * repository that is not source is a licence question, and a struck chime
 * is three sine partials and an envelope. So the sound is generated into a
 * PCM buffer once, at the host output rate, and mixed into the host audio
 * output by host/audio.cpp. It is host-side sound: the game's own mix is
 * not touched, and the WAV capture (the headless verification baseline)
 * never carries it.
 *
 * The shape, stated plainly because there is no hardware to reproduce here
 * and therefore nothing to be accurate to: a fast linear attack, three sine
 * partials in the ratio 1 : 3/2 : 2 at falling amplitude, and an
 * exponential decay that is 60 dB down at kDecaySeconds. The last
 * kFadeSeconds are multiplied by a linear ramp to zero so the buffer ends
 * at exactly 0.0f and the last mixed sample cannot click.
 *
 * Header only, so the selftest (snd/tests/pcm_stream_selftest.cpp) links
 * nothing to exercise it. Runtime-internal, NOT part of the ABI contract
 * (include/recomp_*.h).
 */
#ifndef ICORECOMP_SND_CHIME_H
#define ICORECOMP_SND_CHIME_H

#include <cmath>
#include <cstdint>

namespace rt_chime {

/* Linear rise to full amplitude. Short enough to read as a strike rather
 * than a swell, long enough not to be a step discontinuity. */
constexpr float kAttackSeconds = 0.004f;
/* The exponential decay's reference point: 60 dB down at this age. */
constexpr float kDecaySeconds = 0.300f;
/* The linear ramp at the end that lands the buffer on exact silence. */
constexpr float kFadeSeconds = 0.016f;

/* Partials of one struck note: A5 and the fifth and octave above it. The
 * amplitudes fall by half per partial, which is what makes the sum read as
 * one bell-like note rather than three tones. */
constexpr float kPartialHz[3] = { 880.0f, 1320.0f, 1760.0f };
constexpr float kPartialGain[3] = { 1.0f, 0.5f, 0.25f };

/* Peak amplitude of the rendered buffer, before the volume setting. Held
 * well below full scale because this sound is summed on top of the game's
 * own mix, which is already using the range: the sum saturates in
 * host/audio.cpp, and headroom here is what keeps the saturation from ever
 * being reached in practice. The audible level is then audio.chime_volume
 * (0..100, default 60) times this. */
constexpr float kPeak = 0.5f;

/* Total length of the rendered buffer. */
constexpr float kLengthSeconds = kAttackSeconds + kDecaySeconds;

inline uint32_t frame_count(uint32_t rate) {
    return (uint32_t)((double)kLengthSeconds * (double)rate);
}

/* Renders `frames` mono samples at `rate` into `out`. Deterministic: the
 * same rate always produces the same buffer. */
inline void render(float* out, uint32_t frames, uint32_t rate) {
    if (!out || frames == 0 || rate == 0) return;
    const double sample_period = 1.0 / (double)rate;
    /* 60 dB in nepers is 20 * log10(1000), so the time constant that puts
     * the envelope at 1e-3 after kDecaySeconds is kDecaySeconds / ln(1000). */
    const double tau = (double)kDecaySeconds / std::log(1000.0);
    double gain_sum = 0.0;
    for (int p = 0; p < 3; ++p) gain_sum += (double)kPartialGain[p];

    const double attack = (double)kAttackSeconds;
    const double fade_start = (double)kLengthSeconds - (double)kFadeSeconds;

    for (uint32_t i = 0; i < frames; ++i) {
        const double t = (double)i * sample_period;
        double partials = 0.0;
        for (int p = 0; p < 3; ++p) {
            partials += (double)kPartialGain[p]
                      * std::sin(2.0 * 3.14159265358979323846 * (double)kPartialHz[p] * t);
        }
        partials /= gain_sum;

        double env;
        if (t < attack) {
            env = attack > 0.0 ? t / attack : 1.0;
        } else {
            env = std::exp(-(t - attack) / tau);
        }
        if (t > fade_start) {
            const double left = (double)kLengthSeconds - t;
            const double ramp = left > 0.0 ? left / (double)kFadeSeconds : 0.0;
            env *= ramp < 1.0 ? ramp : 1.0;
        }
        out[i] = (float)(partials * env * (double)kPeak);
    }
    /* The buffer ends on silence by construction, not by hope: the fade
     * ramp above reaches zero at kLengthSeconds, and frame_count rounds
     * down, so the last sample is within one sample period of it. Pinning
     * it is what lets the selftest assert exact silence at the end. */
    out[frames - 1] = 0.0f;
}

} // namespace rt_chime

#endif /* ICORECOMP_SND_CHIME_H */
