/* host/stick_shape_selftest.cpp: standalone exercise of the left-stick
 * reshaping in host/stick_shape.h (the gameplay.run_any_direction toggle).
 *
 * Header-only subject, so this links nothing: no SDL, no runtime services,
 * no settings model. Run:
 *
 *     ./icorecomp-stick-shape-selftest
 *
 * Exit code 0 = every check passed; 2 on the first failing CHECK.
 *
 * The decisive case is the angle sweep, which feeds both the shaped and the
 * unshaped pair through game_stick_value() below. That function is a
 * restatement, written here from the behaviour of the game's stick routine
 * as read in the decomp (ios/pad.c), not copied code: byte pair, radius from
 * centre 127.5, deadzone 48, radius divided by 1 + 0.2 * t / 45 for t whole
 * degrees off the nearest cardinal, saturating at 120. The player runs while
 * the returned value is 1.0 (the game requires it for four consecutive
 * fields); anything below 0.99 walks.
 */
#include "host/stick_shape.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::printf("[test] FAIL at line %d: %s\n", __LINE__, #expr); \
        std::exit(2); \
    } \
} while (0)

namespace {

const double kPi = 3.14159265358979323846;

/* The same int16 -> byte mapping host/input.cpp's axis_to_u8 does. */
int stick_byte(int16_t v) {
    int x = ((int)v + 32768) >> 8;
    return x < 0 ? 0 : (x > 255 ? 255 : x);
}

/* Restatement of the game's stick routine (see the file comment). */
float game_stick_value(int16_t x, int16_t y) {
    const float fx = (float)stick_byte(x) - 127.5f;
    const float fy = (float)stick_byte(y) - 127.5f;
    float radius = std::sqrt(fx * fx + fy * fy);
    if (radius <= 48.0f) return 0.0f;

    const int d = std::abs((int)(std::atan2((double)fy, (double)fx) * 180.0 / kPi));
    const int r = d % 90;
    const int t = r < 46 ? r : 90 - r;
    radius /= 1.0f + (float)t * 0.2f / 45.0f;
    if (radius < 48.0f) radius = 48.0f;
    return radius >= 120.0f ? 1.0f : (radius - 48.0f) / 72.0f;
}

int16_t clamp16(double v) {
    if (v <= -32768.0) return (int16_t)-32768;
    if (v >= 32767.0) return (int16_t)32767;
    return (int16_t)std::lround(v);
}

bool near_within(int a, int b, int tol) {
    return std::abs(a - b) <= tol;
}

double degrees_of(int x, int y) {
    return std::atan2((double)y, (double)x) * 180.0 / kPi;
}

} // namespace

int main() {
    { /* 1. centre stays centred */
        int16_t x = 0, y = 0;
        rt_stick_gate_expand(&x, &y);
        CHECK(x == 0 && y == 0);
        std::printf("[test] 1 centre: ok\n");
    }
    { /* 2. cardinals are untouched: the divisor there is 1 */
        const int16_t in[4][2] = {
            {32767, 0}, {0, (int16_t)-32768}, {(int16_t)-32768, 0}, {0, 32767},
        };
        for (const auto& p : in) {
            int16_t x = p[0], y = p[1];
            rt_stick_gate_expand(&x, &y);
            CHECK(x == p[0] && y == p[1]);
        }
        std::printf("[test] 2 cardinals unchanged: ok\n");
    }
    { /* 3. full tilt at 45 degrees scales by 1.2, no box cap yet */
        int16_t x = 23170, y = 23170;
        rt_stick_gate_expand(&x, &y);
        CHECK(near_within(x, 27804, 1));
        CHECK(near_within(y, 27804, 1));
        std::printf("[test] 3 full 45 degrees: %d,%d\n", (int)x, (int)y);
    }
    { /* 4. half radius at 45 degrees: exactly 1.2, nowhere near the box */
        int16_t x = 11585, y = 11585;
        rt_stick_gate_expand(&x, &y);
        CHECK(near_within(x, 13902, 1));
        CHECK(near_within(y, 13902, 1));
        std::printf("[test] 4 half radius 45 degrees: %d,%d\n", (int)x, (int)y);
    }
    { /* 5. the claim: a full-radius stick runs at every angle once shaped,
       * in all four quadrants, and does not at 45 degrees unshaped */
        float worst_shaped = 2.0f;
        int worst_deg = -1;
        for (int deg = 0; deg <= 90; ++deg) {
            const double rad = (double)deg * kPi / 180.0;
            const int16_t bx = clamp16(32767.0 * std::cos(rad));
            const int16_t by = clamp16(32767.0 * std::sin(rad));
            const int signs[4][2] = {{1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
            for (const auto& sg : signs) {
                const int16_t ix = clamp16((double)bx * sg[0]);
                const int16_t iy = clamp16((double)by * sg[1]);
                int16_t sx = ix, sy = iy;
                rt_stick_gate_expand(&sx, &sy);
                const float v = game_stick_value(sx, sy);
                if (v < worst_shaped) {
                    worst_shaped = v;
                    worst_deg = deg;
                }
                CHECK(v == 1.0f);
            }
        }
        std::printf("[test] 5 sweep: worst shaped value %.4f at %d degrees\n",
            (double)worst_shaped, worst_deg);

        const int16_t d45x = clamp16(32767.0 * std::cos(45.0 * kPi / 180.0));
        const int16_t d45y = clamp16(32767.0 * std::sin(45.0 * kPi / 180.0));
        const float unshaped45 = game_stick_value(d45x, d45y);
        CHECK(unshaped45 < 0.99f);
        CHECK(game_stick_value(32767, 0) == 1.0f);
        std::printf("[test] 5 unshaped: %.4f at 45 degrees, %.4f at 0\n",
            (double)unshaped45, (double)game_stick_value(32767, 0));
    }
    { /* 6. direction is preserved; a component pinned at the int16 endpoint
       * is the one place the pair can move off its line, and it does not
       * arise here (the sweep never clamps below full scale) */
        double worst = 0.0;
        for (int deg = 0; deg <= 90; ++deg) {
            const double rad = (double)deg * kPi / 180.0;
            const int16_t ix = clamp16(32767.0 * std::cos(rad));
            const int16_t iy = clamp16(32767.0 * std::sin(rad));
            int16_t sx = ix, sy = iy;
            rt_stick_gate_expand(&sx, &sy);
            const bool pinned = sx == 32767 || sy == 32767 ||
                                sx == (int16_t)-32768 || sy == (int16_t)-32768;
            const double err = std::fabs(degrees_of(sx, sy) - degrees_of(ix, iy));
            if (pinned) {
                CHECK(err < 0.1);
                continue;
            }
            if (err > worst) worst = err;
            CHECK(err < 0.01);
        }
        std::printf("[test] 6 direction preserved: worst %.5f degrees\n", worst);
    }

    std::printf("stick-shape-selftest: all checks passed\n");
    return 0;
}
