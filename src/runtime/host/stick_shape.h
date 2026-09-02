/* host/stick_shape.h: the left-stick reshaping behind
 * gameplay.run_any_direction (settings.h), kept as pure int16 math with no
 * SDL types so host/stick_shape_selftest.cpp can exercise it without a
 * window, a pad, or the rest of the runtime.
 *
 * What the game does with the stick bytes (read from the decomp's stick
 * routine in ios/pad.c, stated in prose; no addresses, no copied code):
 * it takes the byte pair, forms a radius from centre 127.5, applies a
 * deadzone of 48, divides that radius by 1 + 0.2 * t / 45 where t is the
 * whole degrees off the nearest cardinal direction, and saturates the
 * result at 120. The player runs only while that saturated result holds for
 * four consecutive fields; anything short of it walks.
 *
 * That divisor was tuned for the DualShock 2's octagonal gate, whose corners
 * reach further than the inscribed circle. SDL reports the pad's raw axes
 * with no gate compensation, and a circular gate never reaches the
 * assumed corner overshoot, so off-cardinal tilts lose the division's
 * worth of radius and never saturate. Pre-multiplying the pair by the same factor cancels the game's
 * division, so a circular stick reads the same radius at every angle and a
 * full tilt runs in every direction. Nothing the game computes is changed.
 *
 * The box cap is not a policy choice: the stick bytes are a 0..255 square,
 * so the scaled pair cannot leave it and the largest usable scale is
 * whatever keeps the longer component at full scale. The worst case is near
 * 13 degrees off a cardinal, where a full-radius stick still lands at about
 * 123.8 after the game's division, above the 119.28 the run threshold needs.
 * That ceiling is the same one a physical pad hits at that angle.
 *
 * The game truncates its degrees to a whole number, so its divisor is never
 * larger than the one computed here; the mismatch can only leave the
 * corrected radius slightly high, never short.
 *
 * The scale is held at or above 1: an axis already at the int16 endpoint
 * -32768 is a hair outside the +32767 the box is measured against, and this
 * transform only ever expands. Without the floor, a stick pushed fully left
 * or fully up would come back one unit short of where it started.
 */
#ifndef ICORECOMP_HOST_STICK_SHAPE_H
#define ICORECOMP_HOST_STICK_SHAPE_H

#include <algorithm>
#include <cmath>
#include <cstdint>

/* Type limit, not a value policy: the scaled component has to land back in
 * the int16 an SDL axis is. host/input.cpp has its own equivalent in an
 * anonymous namespace; this header does not depend on it. */
static inline int16_t rt_stick_clamp_axis(float v) {
    if (v <= -32768.0f) return (int16_t)-32768;
    if (v >= 32767.0f) return (int16_t)32767;
    return (int16_t)v;
}

inline void rt_stick_gate_expand(int16_t* x, int16_t* y) {
    const float fx = (float)*x, fy = (float)*y;
    if (fx == 0.0f && fy == 0.0f) return;

    const float ax = std::fabs(fx), ay = std::fabs(fy);
    /* Degrees off the nearest cardinal, 0..45. */
    const float a = std::atan2(std::min(ax, ay), std::max(ax, ay)) * (180.0f / 3.14159265358979323846f);
    const float g = 1.0f + 0.2f * a / 45.0f;
    const float box = 32767.0f / std::max(ax, ay);
    const float s = std::max(1.0f, std::min(g, box));

    *x = rt_stick_clamp_axis(fx * s);
    *y = rt_stick_clamp_axis(fy * s);
}

#endif /* ICORECOMP_HOST_STICK_SHAPE_H */
