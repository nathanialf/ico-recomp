/* snd/reverb.cpp: send-bus reverb.
 *
 * Standard Schroeder/Moorer topology (parallel combs into serial allpasses,
 * the arrangement popularized by freeverb; implemented from the textbook
 * description, no code copied): 8 comb filters with damped feedback per
 * channel, 4 allpasses, slight L/R delay offsets for stereo spread.
 *
 * This is deliberately NOT a model of the SPU2 reverb DSP. The game selects
 * a hardware reverb preset (mode number + depth) through the sndn2 command
 * stream; we key approximate room parameters off that mode. Correctness of
 * the tail character is a later milestone; presence and level of the wet
 * bus is what matters now.
 */
#include "snd.h"

#include "../runtime.h"

#include <cstring>

namespace {

/* Tunings scaled for 48 kHz (freeverb's classic comb lengths are for
 * 44.1 kHz; these are the same ratios scaled up and rounded to primes-ish
 * values, stereo spread 23 samples). */
constexpr int kNumCombs = 8;
constexpr int kNumAllpass = 4;
constexpr int kCombLen[kNumCombs] = { 1228, 1306, 1387, 1466, 1518, 1584, 1638, 1704 };
constexpr int kAllpassLen[kNumAllpass] = { 605, 480, 371, 244 };
constexpr int kSpread = 23;
constexpr int kMaxComb = 1704 + kSpread;
constexpr int kMaxAllpass = 605 + kSpread;

struct Comb {
    float buf[kMaxComb];
    int len = 1, pos = 0;
    float filt = 0.0f;
};
struct Allpass {
    float buf[kMaxAllpass];
    int len = 1, pos = 0;
};

struct Channel {
    Comb comb[kNumCombs];
    Allpass ap[kNumAllpass];
};

Channel g_ch[2];
float g_feedback = 0.84f;
float g_damp = 0.2f;
float g_wet_l = 0.0f;
float g_wet_r = 0.0f;
uint32_t g_mode = 0;

float comb_run(Comb& c, float in) {
    float out = c.buf[c.pos];
    c.filt = out * (1.0f - g_damp) + c.filt * g_damp;
    c.buf[c.pos] = in + c.filt * g_feedback;
    if (++c.pos >= c.len) c.pos = 0;
    return out;
}

float allpass_run(Allpass& a, float in) {
    float bufout = a.buf[a.pos];
    float out = bufout - in;
    a.buf[a.pos] = in + bufout * 0.5f;
    if (++a.pos >= a.len) a.pos = 0;
    return out;
}

void set_lengths(float scale) {
    if (scale < 0.25f) scale = 0.25f;
    if (scale > 1.0f) scale = 1.0f;
    for (int s = 0; s < 2; ++s) {
        for (int i = 0; i < kNumCombs; ++i) {
            int len = (int)(kCombLen[i] * scale) + (s ? kSpread : 0);
            g_ch[s].comb[i].len = len < 1 ? 1 : len;
            g_ch[s].comb[i].pos = 0;
            g_ch[s].comb[i].filt = 0.0f;
            std::memset(g_ch[s].comb[i].buf, 0, sizeof(g_ch[s].comb[i].buf));
        }
        for (int i = 0; i < kNumAllpass; ++i) {
            int len = (int)(kAllpassLen[i] * scale) + (s ? kSpread : 0);
            g_ch[s].ap[i].len = len < 1 ? 1 : len;
            g_ch[s].ap[i].pos = 0;
            std::memset(g_ch[s].ap[i].buf, 0, sizeof(g_ch[s].ap[i].buf));
        }
    }
}

} // namespace

void rt_reverb_reset() {
    g_wet_l = g_wet_r = 0.0f;
    g_mode = 0;
    set_lengths(1.0f);
}

void rt_reverb_set_params(uint32_t mode, float depth_l, float depth_r) {
    /* SPU2 reverb mode numbers (public libsd fact): 0 off, 1 room,
     * 2 studio small, 3 studio medium, 4 studio large, 5 hall, 6 space,
     * 7 echo, 8 delay, 9 pipe. Mapped to rough size/decay pairs. */
    struct Preset { float scale, feedback, damp; };
    static const Preset presets[10] = {
        { 0.50f, 0.00f, 0.50f },  /* 0: off */
        { 0.45f, 0.75f, 0.35f },  /* 1: room */
        { 0.35f, 0.70f, 0.30f },  /* 2: studio small */
        { 0.50f, 0.78f, 0.28f },  /* 3: studio medium */
        { 0.65f, 0.82f, 0.25f },  /* 4: studio large */
        { 1.00f, 0.86f, 0.20f },  /* 5: hall */
        { 1.00f, 0.90f, 0.10f },  /* 6: space */
        { 1.00f, 0.80f, 0.05f },  /* 7: echo */
        { 1.00f, 0.75f, 0.05f },  /* 8: delay */
        { 0.80f, 0.88f, 0.40f },  /* 9: pipe */
    };
    const Preset& p = presets[mode < 10 ? mode : 5];
    if (mode != g_mode) {
        g_mode = mode;
        set_lengths(p.scale);
        rt_log_info("snd", "reverb mode=%u depth=(%.3f, %.3f)", mode, depth_l, depth_r);
    }
    g_feedback = p.feedback;
    g_damp = p.damp;
    g_wet_l = mode == 0 ? 0.0f : depth_l;
    g_wet_r = mode == 0 ? 0.0f : depth_r;
}

void rt_reverb_run(float in, float* out_l, float* out_r) {
    if (g_wet_l == 0.0f && g_wet_r == 0.0f) return;
    float acc[2];
    for (int s = 0; s < 2; ++s) {
        float sum = 0.0f;
        for (int i = 0; i < kNumCombs; ++i) sum += comb_run(g_ch[s].comb[i], in);
        for (int i = 0; i < kNumAllpass; ++i) sum = allpass_run(g_ch[s].ap[i], sum);
        acc[s] = sum * 0.06f; /* gain-compensate the 8 parallel combs */
    }
    *out_l += acc[0] * g_wet_l;
    *out_r += acc[1] * g_wet_r;
}
