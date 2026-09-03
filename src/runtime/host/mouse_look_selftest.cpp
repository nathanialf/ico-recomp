/* host/mouse_look_selftest.cpp: standalone exercise of the virtual stick and
 * the byte mapping in host/mouse_look.h (the input.mouse_look setting).
 *
 * Header-only subject, so this links nothing: no SDL, no runtime services,
 * no settings model. Run:
 *
 *     ./icorecomp-mouse-look-selftest
 *
 * Exit code 0 = every check passed; 2 on the first failing CHECK.
 *
 * game_response() below is a restatement of the game's stick chain, written
 * from the behaviour of iosPadGetStick as read in the decomp (ios/pad.c,
 * still an INCLUDE_ASM nonmatching function), not copied code: centre 127.5,
 * radial dead zone 48, the octant divisor 1 + 0.2 * t / 45 for t whole
 * truncated degrees off the nearest cardinal, saturation at 120 and response
 * (mag - 48) / 72. It is the same restatement host/stick_shape_selftest.cpp
 * uses for the left stick.
 *
 * There is a second, square dead zone in the camera's stick reader, and it is
 * deliberately not modelled here: that reader computes it and then never
 * reads the result, so the retail build gates on the radial 48 alone
 * (host/mouse_look.h says exactly where). clears_square_gate() below reports
 * which pairs would also have passed it, as a number rather than a claim,
 * because that reading is what lets the radius floor be 49 and isotropic.
 *
 * The decisive cases are check 3 (no dead band and no slow diagonals at any
 * angle) and checks 7 to 11 (the stick is a position: a drag parks it, a
 * pause holds it, and only a long pause lets go).
 */
#include "host/mouse_look.h"

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

/* Restatement of the game's stick chain (see the file comment). */
float game_response(uint8_t bx, uint8_t by) {
    const float fx = (float)bx - 127.5f;
    const float fy = (float)by - 127.5f;

    float mag = std::sqrt(fx * fx + fy * fy);
    if (mag <= 48.0f) return 0.0f;

    const int d = std::abs((int)(std::atan2((double)fy, (double)fx) * 180.0 / kPi));
    const int rem = d % 90;
    const int t = rem < 46 ? rem : 90 - rem;
    mag /= 1.0f + (float)t * 0.2f / 45.0f;
    if (mag < 48.0f) mag = 48.0f;
    return mag >= 120.0f ? 1.0f : (mag - 48.0f) / 72.0f;
}

/* The square dead zone the camera's stick reader computes and discards: it
 * zeroes the pair only when both components are inside 51.2. Reported, never
 * asserted. */
bool clears_square_gate(uint8_t bx, uint8_t by) {
    const float fx = (float)bx - 127.5f;
    const float fy = (float)by - 127.5f;
    return std::fabs(fx) >= 51.2f || std::fabs(fy) >= 51.2f;
}

/* The magnitude the chain above measures after its octant division, which is
 * the number that has to reach 120 for the game to saturate. Reported by the
 * dense sweep so the headroom the ramp top buys is a printed number rather
 * than an assertion in a comment. */
float game_magnitude(uint8_t bx, uint8_t by) {
    const float fx = (float)bx - 127.5f;
    const float fy = (float)by - 127.5f;
    const float mag = std::sqrt(fx * fx + fy * fy);
    const int d = std::abs((int)(std::atan2((double)fy, (double)fx) * 180.0 / kPi));
    const int rem = d % 90;
    const int t = rem < 46 ? rem : 90 - rem;
    return mag / (1.0f + (float)t * 0.2f / 45.0f);
}

/* Distance of a reported pair from the centred byte pair, which is what the
 * ramp moves and what a release has to walk down. */
double byte_radius(const RtMouseLookOut& o) {
    const double bx = (double)o.x - 127.5, by = (double)o.y - 127.5;
    return std::sqrt(bx * bx + by * by);
}

/* The sweep grid: 360 whole degrees by 32 deflections spread
 * logarithmically from a thousandth of full to full, which brackets both the
 * smallest deflection a drag can leave and the gate. */
const int kAngles = 360;
const int kMagnitudes = 32;
const double kMinM = 0.001;
const double kMaxM = 1.0;

double sweep_m_of(int i, int count) {
    return kMinM * std::pow(kMaxM / kMinM, (double)i / (double)(count - 1));
}

double sweep_m(int i) { return sweep_m_of(i, kMagnitudes); }

} // namespace

int main() {
    { /* 1. a centred stick is inactive */
        const RtMouseLookOut o = rt_mouse_look_map(0.0f, 0.0f, false);
        CHECK(o.x == 0x80 && o.y == 0x80 && !o.active);
        const RtMouseLookOut p = rt_mouse_look_map(0.0f, 0.0f, true);
        CHECK(p.x == 0x80 && p.y == 0x80 && !p.active);
        /* And a stick that has never been touched reports the same, however
         * many still fields go by. */
        RtMouseLookStick st;
        for (int i = 0; i < 60; ++i) {
            const RtMouseLookOut q = st.step(0.0f, 0.0f, 1.0f, false);
            CHECK(!q.active && q.x == 0x80 && q.y == 0x80);
        }
        std::printf("[test] 1 centred stick: inactive, and stays inactive while still\n");
    }
    { /* 2. the smallest deflection there is still reads as a deflection */
        const RtMouseLookOut o = rt_mouse_look_map(1e-6f, 0.0f, false);
        CHECK(o.active);
        CHECK(game_response(o.x, o.y) > 0.0f);
        std::printf("[test] 2 smallest deflection: %u,%u -> response %.4f (square gate %s)\n",
            (unsigned)o.x, (unsigned)o.y, (double)game_response(o.x, o.y),
            clears_square_gate(o.x, o.y) ? "also cleared" : "not cleared, as designed");
    }
    { /* 3. the claim: no dead band at any angle, and a stick at the gate
       * saturates at every angle. Monotonicity is checked to within one byte
       * of the game's own response resolution (1/72): the response is a
       * staircase because the output is a byte pair, and the game's divisor
       * is computed from whole truncated degrees of that quantized pair, so
       * a step in |V| can move the truncated angle by a degree and cost 0.44
       * percent of the divisor before the next step recovers it. */
        const float kMonoTolerance = 1.0f / 72.0f;
        float worst_nonzero = 2.0f;
        int worst_nonzero_deg = -1;
        double worst_dip = 0.0;
        for (int a = 0; a < kAngles; ++a) {
            const double th = (double)a * kPi / 180.0;
            float prev = -1.0f;
            for (int i = 0; i < kMagnitudes; ++i) {
                const double m = sweep_m(i);
                const RtMouseLookOut o = rt_mouse_look_map(
                    (float)(std::cos(th) * m), (float)(std::sin(th) * m), false);
                CHECK(o.active);
                const float resp = game_response(o.x, o.y);

                CHECK(resp > 0.0f);
                if (resp < worst_nonzero) { worst_nonzero = resp; worst_nonzero_deg = a; }
                if (m >= 1.0) CHECK(resp == 1.0f);

                if (prev >= 0.0f) {
                    if ((double)(prev - resp) > worst_dip) worst_dip = (double)(prev - resp);
                    CHECK(resp >= prev - kMonoTolerance);
                }
                prev = resp;
            }
        }
        std::printf("[test] 3 sweep: weakest response %.4f at %d degrees, full deflection"
            " saturates at every angle, worst dip %.5f\n",
            (double)worst_nonzero, worst_nonzero_deg, worst_dip);
    }
    { /* 3b. the same properties on a much finer grid (3600 angles by 256
       * deflections), which is where the byte staircase and the whole-degree
       * divisor actually interact. This is the sweep the ramp top in
       * mouse_look.h is justified by: it prints the smallest magnitude the
       * game measures for a fully deflected stick, and 120 is what that has
       * to clear. */
        const float kMonoTolerance = 1.0f / 72.0f;
        const int angles = 3600, mags = 256;
        double worst_dip = 0.0;
        float worst_full_mag = 1e9f;
        double worst_full_deg = -1.0;
        for (int ai = 0; ai < angles; ++ai) {
            const double deg = (double)ai * 360.0 / (double)angles;
            const double th = deg * kPi / 180.0;
            float prev = -1.0f;
            for (int i = 0; i < mags; ++i) {
                const double m = sweep_m_of(i, mags);
                const RtMouseLookOut o = rt_mouse_look_map(
                    (float)(std::cos(th) * m), (float)(std::sin(th) * m), false);
                const float resp = game_response(o.x, o.y);
                CHECK(resp > 0.0f);
                if (prev >= 0.0f) {
                    if ((double)(prev - resp) > worst_dip) worst_dip = (double)(prev - resp);
                    CHECK(resp >= prev - kMonoTolerance);
                }
                prev = resp;
            }
            /* The gate itself, where a drag past full deflection lands. */
            const RtMouseLookOut full = rt_mouse_look_map(
                (float)std::cos(th), (float)std::sin(th), false);
            CHECK(game_response(full.x, full.y) == 1.0f);
            const float mag = game_magnitude(full.x, full.y);
            if (mag < worst_full_mag) { worst_full_mag = mag; worst_full_deg = deg; }
        }
        std::printf("[test] 3b dense sweep: worst dip %.5f (tolerance %.5f), smallest full"
            " deflection magnitude %.3f at %.2f degrees (needs 120)\n",
            worst_dip, (double)kMonoTolerance, (double)worst_full_mag, worst_full_deg);
        CHECK(worst_full_mag >= 120.0f);
    }
    { /* 4. invert_y mirrors the y byte around the centre and leaves x alone.
       * The mirror is exact (ceil(127.5 + c) + floor(127.5 - c) is 255) for
       * every nonzero y component; a deflection with no y component reports
       * the neutral 0x80 either way, which is the one unit of asymmetry the
       * byte grid forces on a centre of 127.5. */
        int worst_asym = 0;
        for (int a = 0; a < kAngles; ++a) {
            const double th = (double)a * kPi / 180.0;
            for (int i = 0; i < kMagnitudes; ++i) {
                const double m = sweep_m(i);
                const float vx = (float)(std::cos(th) * m);
                const float vy = (float)(std::sin(th) * m);
                const RtMouseLookOut n = rt_mouse_look_map(vx, vy, false);
                const RtMouseLookOut v = rt_mouse_look_map(vx, vy, true);
                CHECK(v.x == n.x);
                CHECK(v.active == n.active);
                const int asym = std::abs((int)n.y + (int)v.y - 255);
                if (asym > worst_asym) worst_asym = asym;
                CHECK(asym <= 1);
            }
        }
        /* Through the stick, which is where the setting is actually read: a
         * drag down with invert_y is the same pair as a drag up without. */
        RtMouseLookStick a, b;
        const RtMouseLookOut da = a.step(0.0f, 40.0f, 1.0f, true);
        const RtMouseLookOut db = b.step(0.0f, -40.0f, 1.0f, false);
        CHECK(da.x == db.x && da.y == db.y);
        std::printf("[test] 4 invert_y: x untouched, y mirrored, worst asymmetry %d\n",
            worst_asym);
    }
    { /* 5. the floor is isotropic: the smallest deflection a drag can leave
       * asks the game for the same small camera offset at every angle. This
       * is what the floor of 49, set against the radial dead zone alone,
       * buys; against the discarded square gate it would have been 52 on a
       * cardinal and 73.5 at 45 degrees, a factor of six in response. */
        const int angles = 3600;
        float lo = 2.0f, hi = 0.0f;
        double lo_deg = -1.0, hi_deg = -1.0;
        int square_cleared = 0;
        for (int ai = 0; ai < angles; ++ai) {
            const double deg = (double)ai * 360.0 / (double)angles;
            const double th = deg * kPi / 180.0;
            RtMouseLookStick st;
            /* One pixel of drag at the default sensitivity: |V| = 1/160. */
            const RtMouseLookOut o = st.step((float)std::cos(th), (float)std::sin(th),
                1.0f, false);
            CHECK(o.active);
            const float resp = game_response(o.x, o.y);
            CHECK(resp > 0.0f);
            if (resp < lo) { lo = resp; lo_deg = deg; }
            if (resp > hi) { hi = resp; hi_deg = deg; }
            if (clears_square_gate(o.x, o.y)) ++square_cleared;
        }
        /* What is left is the byte grid, not the floor: the requested radius
         * is exactly RT_MOUSE_LOOK_GATE_FLOOR at every angle, and rounding
         * away from the centre can add up to a whole unit to each of the two
         * components, which is up to sqrt(2) of magnitude before the game's
         * divisor and about 1.18 after it, plus up to 0.44 percent of the
         * divisor from its whole-degree truncation. So about 1.4 units of
         * the 72 the response is measured in. The bound below is that with
         * headroom; the spread that matters is the printed one. For
         * comparison, clearing the discarded square gate would have cost 52
         * against 73.5 of radius, which is 0.3 of response, a factor of six
         * rather than of two. */
        std::printf("[test] 5 one pixel of drag: response %.4f at %.2f to %.4f at %.2f degrees,"
            " spread %.4f of the 1.0 the response spans (%.2f byte units of the 72);"
            " %d of %d angles would also clear the discarded square gate\n",
            (double)lo, lo_deg, (double)hi, hi_deg, (double)(hi - lo),
            (double)(hi - lo) * 72.0, square_cleared, angles);
        CHECK(hi - lo <= 2.0f / 72.0f);
    }
    { /* 6. the two ends of the ramp as numbers rather than properties. */
        const RtMouseLookOut small = rt_mouse_look_map(0.001f, 0.0f, false);
        const RtMouseLookOut full = rt_mouse_look_map(1.0f, 0.0f, false);
        const float d = (float)std::sqrt(0.5);
        const RtMouseLookOut diag = rt_mouse_look_map(d, d, false);
        std::printf("[test] 6 ends: smallest %u,%u -> %.4f; full %u,%u -> %.4f;"
            " full diagonal %u,%u -> %.4f\n",
            (unsigned)small.x, (unsigned)small.y, (double)game_response(small.x, small.y),
            (unsigned)full.x, (unsigned)full.y, (double)game_response(full.x, full.y),
            (unsigned)diag.x, (unsigned)diag.y, (double)game_response(diag.x, diag.y));
        CHECK(game_response(small.x, small.y) > 0.0f);
        CHECK(game_response(full.x, full.y) == 1.0f);
        CHECK(game_response(diag.x, diag.y) == 1.0f);
    }
    { /* 7. the stick is a position: a drag parks it. The same drag split
       * across fields lands on the same pair (the accumulation does not care
       * how the pixels arrived), the deflection does not move while the
       * mouse is still, and a drag of RT_MOUSE_LOOK_DRAG_PIXELS at
       * sensitivity 1 is exactly full deflection. */
        RtMouseLookStick one, many;
        const RtMouseLookOut o1 = one.step(RT_MOUSE_LOOK_DRAG_PIXELS * 0.5f, 0.0f, 1.0f, false);
        RtMouseLookOut om;
        for (int i = 0; i < 16; ++i) om = many.step(RT_MOUSE_LOOK_DRAG_PIXELS / 32.0f,
            0.0f, 1.0f, false);
        CHECK(o1.x == om.x && o1.y == om.y);
        CHECK(o1.active && o1.x > 0x80 && o1.y == 0x80);

        RtMouseLookStick full;
        const RtMouseLookOut of = full.step(RT_MOUSE_LOOK_DRAG_PIXELS, 0.0f, 1.0f, false);
        CHECK(game_response(of.x, of.y) == 1.0f);
        /* Past the gate the pair stops growing. */
        const RtMouseLookOut past = full.step(RT_MOUSE_LOOK_DRAG_PIXELS * 10.0f,
            0.0f, 1.0f, false);
        CHECK(past.x == of.x && past.y == of.y);

        /* Sensitivity is the drag distance: twice the sensitivity is half
         * the drag for the same deflection. */
        RtMouseLookStick fast;
        const RtMouseLookOut os = fast.step(RT_MOUSE_LOOK_DRAG_PIXELS * 0.25f,
            0.0f, 2.0f, false);
        CHECK(os.x == o1.x && os.y == o1.y);
        std::printf("[test] 7 drag: half a drag -> %u,%u (response %.4f), same in 16 pieces,"
            " same at twice the sensitivity over half the distance; a full drag saturates\n",
            (unsigned)o1.x, (unsigned)o1.y, (double)game_response(o1.x, o1.y));
    }
    { /* 8. holding: RT_MOUSE_LOOK_HOLD_FIELDS still fields report the parked
       * pair unchanged, byte for byte. That is the whole point of the hold:
       * the game's camera parks at the offset instead of walking back. */
        RtMouseLookStick h;
        const RtMouseLookOut parked = h.step(60.0f, -20.0f, 1.0f, false);
        CHECK(parked.active);
        for (int i = 0; i < RT_MOUSE_LOOK_HOLD_FIELDS; ++i) {
            const RtMouseLookOut o = h.step(0.0f, 0.0f, 1.0f, false);
            CHECK(o.active);
            CHECK(o.x == parked.x && o.y == parked.y);
            CHECK(game_response(o.x, o.y) > 0.0f);
        }
        std::printf("[test] 8 hold: %u,%u parked unchanged for %d still fields\n",
            (unsigned)parked.x, (unsigned)parked.y, RT_MOUSE_LOOK_HOLD_FIELDS);
    }
    { /* 9. letting go: after the hold the deflection walks to centre over
       * RT_MOUSE_LOOK_RELEASE_FIELDS fields, never growing, never reversing,
       * and the last of those fields is a released stick. */
        RtMouseLookStick h;
        const RtMouseLookOut parked = h.step(120.0f, 0.0f, 1.0f, false);
        for (int i = 0; i < RT_MOUSE_LOOK_HOLD_FIELDS; ++i) {
            CHECK(h.step(0.0f, 0.0f, 1.0f, false).x == parked.x);
        }
        double prev = byte_radius(parked);
        int active_fields = 0;
        for (int i = 0; i < RT_MOUSE_LOOK_RELEASE_FIELDS; ++i) {
            const RtMouseLookOut o = h.step(0.0f, 0.0f, 1.0f, false);
            const bool last = (i == RT_MOUSE_LOOK_RELEASE_FIELDS - 1);
            if (last) {
                CHECK(!o.active && o.x == 0x80 && o.y == 0x80);
            } else {
                CHECK(o.active);
                CHECK(o.x > 0x80 && o.y == 0x80);       /* direction kept */
                CHECK(byte_radius(o) < prev);           /* strictly shrinking */
                CHECK(game_response(o.x, o.y) > 0.0f);
                prev = byte_radius(o);
                ++active_fields;
            }
        }
        /* And it stays released. */
        for (int i = 0; i < 30; ++i) CHECK(!h.step(0.0f, 0.0f, 1.0f, false).active);
        std::printf("[test] 9 release: %d shrinking fields after the hold, then centred and"
            " released; %d fields of deflection in all after the last motion\n",
            active_fields, RT_MOUSE_LOOK_HOLD_FIELDS + active_fields);
    }
    { /* 10. motion during the release picks up from where the release had
       * got to: no jump back to the parked deflection, and no jump to
       * centre either. */
        RtMouseLookStick h;
        const RtMouseLookOut parked = h.step(80.0f, 0.0f, 1.0f, false);
        for (int i = 0; i < RT_MOUSE_LOOK_HOLD_FIELDS; ++i) h.step(0.0f, 0.0f, 1.0f, false);
        RtMouseLookOut mid;
        for (int i = 0; i < 3; ++i) mid = h.step(0.0f, 0.0f, 1.0f, false);
        CHECK(mid.active);
        CHECK(byte_radius(mid) < byte_radius(parked));
        const RtMouseLookOut resumed = h.step(8.0f, 0.0f, 1.0f, false);
        CHECK(resumed.active);
        CHECK(byte_radius(resumed) > byte_radius(mid));
        CHECK(byte_radius(resumed) < byte_radius(parked));
        /* And the hold is whole again from here. */
        for (int i = 0; i < RT_MOUSE_LOOK_HOLD_FIELDS; ++i) {
            const RtMouseLookOut o = h.step(0.0f, 0.0f, 1.0f, false);
            CHECK(o.active && o.x == resumed.x && o.y == resumed.y);
        }
        std::printf("[test] 10 resume: parked %.1f, three fields into the release %.1f,"
            " resumed %.1f byte radius; the hold starts over\n",
            byte_radius(parked), byte_radius(mid), byte_radius(resumed));
    }
    { /* 11. reset() centres the stick at once, from a hold and from the
       * middle of a release, with no ramp: the mouse has stopped being the
       * camera's and the deflection is not the next owner's. */
        RtMouseLookStick h;
        h.step(60.0f, 0.0f, 1.0f, false);
        h.step(0.0f, 0.0f, 1.0f, false);
        h.reset();
        const RtMouseLookOut a = h.step(0.0f, 0.0f, 1.0f, false);
        CHECK(!a.active && a.x == 0x80 && a.y == 0x80);

        RtMouseLookStick r;
        r.step(60.0f, 0.0f, 1.0f, false);
        for (int i = 0; i < RT_MOUSE_LOOK_HOLD_FIELDS + 2; ++i) r.step(0.0f, 0.0f, 1.0f, false);
        r.reset();
        const RtMouseLookOut b = r.step(0.0f, 0.0f, 1.0f, false);
        CHECK(!b.active && b.x == 0x80 && b.y == 0x80);
        std::printf("[test] 11 reset: centred at once from a hold and from a release\n");
    }

    std::printf("mouse-look-selftest: all checks passed\n");
    return 0;
}
