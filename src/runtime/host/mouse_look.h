/* host/mouse_look.h: the mouse-to-right-stick mapping behind
 * input.mouse_look (settings.h), kept as pure float math with no SDL types
 * so host/mouse_look_selftest.cpp can exercise it without a window, a
 * mouse, or the rest of the runtime. host/mouse.cpp collects the deltas and
 * host/input.cpp writes the result into the virtual pad.
 *
 * Nothing here changes a value the game computed. The output is a DS2 right
 * stick byte pair, the same channel a physical pad writes, and the game's
 * own dead zones, octant divisor, saturation and response curve then run
 * unmodified on it.
 *
 *
 * What the game does with the stick byte pair
 * -------------------------------------------
 * Measured by reading the decomp's iosPadGetStick (ios/pad.c, still an
 * INCLUDE_ASM nonmatching function, so the chain below was read out of the
 * disassembly; stated in prose, no addresses and no copied code):
 *
 *   1. Both bytes are converted to float and 127.5 is subtracted, so the
 *      centre of the 0..255 byte range is the origin.
 *   2. mag = hypot of that pair. mag <= 48 returns a response of zero.
 *   3. The pair is normalized; that unit vector is the direction the camera
 *      turns, and it is taken before step 4, so nothing below changes the
 *      direction, only the response.
 *   4. t = the whole degrees off the nearest cardinal direction. The angle
 *      is converted to degrees and truncated to an int, a remainder against
 *      90 takes that into 0..89, and the octant fold then turns a remainder
 *      above 45 into 90 minus it, which leaves 0..45. mag is divided by
 *      1 + 0.2 * t / 45 and is then held at 48 or above.
 *   5. mag >= 120 returns a response of 1. Otherwise the response is
 *      (mag - 48) / 72.
 *
 * A square dead zone sits on the caller's side, in the camera's stick reader
 * (func_00189D68 in src/camera-ico2.c): it loads a right-stick byte pair
 * from the game's pad state, subtracts the centre, scales by 1/128 and
 * zeroes both components when each of them is under a 0.4 literal, which is
 * 51.2 units. Re-read from its disassembly for this file, and the
 * retail build does not act on it: the two zeroed values are never read
 * again. The pair the camera uses is the one iosPadActRequest reads out of
 * the pad buffer a few instructions later, and the zeroing writes registers
 * that the multiply after that call overwrites. Only the radial 48 gates the
 * retail camera. That is a correction to what this comment said before.
 *
 * The radius floor below is therefore set against the radial 48 alone, at
 * RT_MOUSE_LOOK_GATE_FLOOR = 49, the same radius at every angle. Clearing
 * the square gate as well would cost 52 on a cardinal and 73.5 at 45
 * degrees, which the game reads as responses of 0.056 and 0.355: the
 * smallest camera offset a small drag could ask for would then be six times
 * larger on a diagonal than on a cardinal. If the square gate ever turned
 * out to be live after all, the symptom would be unmistakable and this is
 * where to look: small drags would do nothing at all until the deflection
 * grew past 51.2 on one component.
 *
 * The divisor in step 4 was tuned for the DualShock 2's octagonal gate,
 * whose corners reach further than the inscribed circle. A synthesized byte
 * pair has no gate, so the same pre-multiplication host/stick_shape.h
 * applies to the left stick is applied here: it cancels the game's division
 * so a diagonal drag reaches the same deflection as a cardinal one.
 *
 *
 * The mapping, and where each constant comes from
 * -----------------------------------------------
 * The mouse drives a virtual stick, not a speed. The trace below shows why:
 * the game's camera stick is a position, so what the mouse has to produce is
 * a deflection that stays where the hand put it. host/input.cpp keeps one
 * RtMouseLookStick and steps it once per field.
 *
 *   1. The stick vector V is in unit-circle space, |V| <= 1. A field's
 *      accumulated motion (dx, dy in pixels) moves it:
 *
 *          V += (dx, dy) * sens / RT_MOUSE_LOOK_DRAG_PIXELS
 *
 *      and V is scaled back onto the unit circle when that pushes it out,
 *      which is the stick's gate. Motion is integrated, so how the pixels
 *      were spread across fields does not matter: only where the hand has
 *      dragged to.
 *   2. RT_MOUSE_LOOK_DRAG_PIXELS is 160, the drag that reaches full
 *      deflection at sensitivity 1. A host choice, not a measurement: it is
 *      an eighth of the 1280 px default window (display.window_width) and a
 *      fifth of an 800 px one, so full deflection is a short drag that stays
 *      well inside the picture, and at 800 counts per inch it is 0.2 inch of
 *      hand travel for the full 120 degrees of camera offset the game
 *      allows, which is the order of a DualShock stick's own throw.
 *      Sensitivity divides it: 320 px at 0.5, 80 px at 2.
 *   3. V = 0 reports a centred, inactive pair, and host/input.cpp leaves the
 *      stick alone then, so a keyboard or pad still owns it. Every other V
 *      reports a pair, on every field, whether or not the mouse moved during
 *      it: a stick that is not being touched is still deflected.
 *   4. The byte radius r ramps linearly with |V| from
 *      RT_MOUSE_LOOK_GATE_FLOOR to RT_MOUSE_LOOK_RAMP_TOP. The floor is 49
 *      at every angle (the radial dead zone is 48, and 49 is the first whole
 *      unit past it), so the smallest deflection a drag can ask for is the
 *      same small camera offset whichever way it points: the game reads
 *      about 0.02 of response there, which is under two degrees of offset.
 *
 *      The top is 122 rather than the game's saturation radius of 120. That
 *      is a host choice covering quantization, and it is behaviourally
 *      neutral because the game returns the same response of 1 for anything
 *      at or above 120. Two effects eat into an exact 120: the byte grid
 *      puts the reachable magnitudes on half-integers, and the divisor is
 *      computed from whole truncated degrees of the quantized pair, which
 *      can be one degree (0.44 percent) larger than the divisor this file
 *      cancelled. With the top at 120 the measured magnitude falls as low
 *      as 119.46 at some angles and a full deflection then does not
 *      saturate. At 122 the worst angle measures 121.45, about a byte and a
 *      half of headroom. Both numbers come from sweeping angles through the
 *      chain above; the second one is printed by check 3b of
 *      host/mouse_look_selftest.cpp on every run.
 *   5. Octant pre-expansion, the same math as rt_stick_gate_expand: with t
 *      the degrees off the nearest cardinal, g = 1 + 0.2 * t / 45 and
 *      s = max(1, min(g, 127.5 / (r * max(|ux|, |uy|)))). The second term is
 *      a type limit rather than a policy clamp, exactly as in
 *      host/stick_shape.h: the byte pair is a 0..255 square and the scaled
 *      component cannot leave it. The floor of 1 keeps this transform an
 *      expansion only.
 *   6. The two components are turned into bytes around 127.5, rounding away
 *      from the centre. Rounding to nearest instead would let quantization
 *      cut up to half a unit off the magnitude the game measures, which at
 *      the top of the ramp is the difference between saturating and not, and
 *      it makes the response non-monotone in |V| at some angles. Away from
 *      the centre, the magnitude the game measures is never below the one
 *      asked for, and the mapping stays monotone within the byte grid.
 *
 * SDL reports yrel positive when the mouse moves down and the game reads
 * byte 0 as up, so the two conventions already agree and no sign flip is
 * applied. invert_y negates the y component before the byte conversion and
 * nothing else.
 *
 *
 * What the game does when the stick is neutral
 * --------------------------------------------
 * Traced through the decomp for the hold below, because a hand on a mouse
 * produces neutral fields in the middle of a drag that a hand on a stick
 * never produces. All names are the decomp's.
 *
 * The camera's stick reader is func_00189D68 (src/camera-ico2.c). Its tail
 * reads the right stick through iosPadActRequest, which calls iosPadGetStick
 * (ios/pad.c) for the response, normalizes the byte pair and multiplies that
 * unit vector by the response. Nothing there branches on the stick being
 * neutral. The three conditions that route to ClearMailAdditionalData
 * (src/mail-add-data.c) are a mode word (D_00274EC0[5], zero in the shipped
 * data), func_00153FE8 returning zero (src/boyact.c, a state byte) and a
 * local flag that the camera-set read clears for sets which forbid the
 * manual camera. A released stick reaches none of them, so the camera takes
 * exactly the same path on a neutral field as on a deflected one, with a
 * response of zero.
 *
 * That path is func_00194EC0, which calls ActSendMail_WithAdditionalData.
 * The manual camera is two accumulated angles in radians, D_006D35C0[0] and
 * [1]; GetMailAdditionalData turns them into two rotations of the camera
 * about its target, so they are an orbit offset, not a velocity.
 * ActSendMail_WithAdditionalData is proportional control with a step limit:
 *
 *   target   the stick's unit vector times the response, one component
 *            times D_006D35C0[5] degrees (120, set by func_00194E28), the
 *            other times the pitch range func_00194EC0 builds from
 *            D_006D35C0[6] (80 degrees) and the camera's current pitch.
 *   step     at most D_00633DC0 * (pi/360) * D_002924B0[15 or 16], the index
 *            picked by whether the response is at least 0.1. With the
 *            shipped D_00274EC0[0] = 0 and [1] = 2, D_00633DC0 is 2.0 and
 *            the product is exactly the table entry in degrees per camera
 *            update: 2.0 while the response is 0.1 or more, 1.5 below it.
 *            D_00633DC0 cancels the field divider that same data sets, so
 *            the rate is 60 and 45 degrees per second however the game is
 *            paced; at the shipped two fields per update that is 1.0 and
 *            0.75 degrees per field. The data words, the table entries and
 *            the arithmetic are read out of the decomp. That [1] is the
 *            fields-per-camera-update divider is inferred, from the way it
 *            makes the product come out as whole degrees per update and the
 *            rate constant in real time; the per-update figures above do not
 *            depend on it, the per-field ones do.
 *   ease     inside ten steps of the target the move is a tenth of what
 *            remains per update instead, so the last 20 degrees driving and
 *            15 degrees returning are exponential, about a third of a second
 *            of time constant.
 *
 * So, plainly, for the three questions the hold had to answer:
 *
 *   - The return to centre is not immediate. A neutral field only sets the
 *     target to zero; the accumulated pair walks back at up to 0.75 degrees
 *     per field and eases over the last 15 degrees.
 *   - It is rate limited, and there is no timer of any kind on that path.
 *     No hold counter, no release edge, no mode flag: the two accumulated
 *     angles are the whole state.
 *   - A single neutral field between two deflected fields therefore resets
 *     nothing at all. It costs one step of return and nothing else.
 *   - ClearMailAdditionalData is the one immediate snap to centre in the
 *     path, and a neutral stick does not reach it.
 *
 * The consequence for this file is the one that matters. The camera stick is
 * a position, not a velocity: deflection picks where the camera sits around
 * its target, up to 120 degrees of yaw, and the game walks there and stops.
 * A stick held still holds the camera still and off centre. So the honest
 * way to tell this game that the hand paused is to keep reporting the pair
 * the hand last asked for, which parks the camera exactly where it was
 * heading.
 *
 *
 * Letting go of the stick
 * ------------------------
 * A hand on a mouse stops moving all the time in the middle of a drag; a
 * hand on a stick does not let go for a field. RtMouseLookStick below is
 * what tells those two apart, and it is host state only: it changes which
 * pair the virtual pad reports, never a value the game computed.
 *
 *   - Holding. V is not touched by a field with no motion. The stick stays
 *     where the drag left it and the camera parks at that offset, which is
 *     what a held stick does. RT_MOUSE_LOOK_HOLD_FIELDS of that is how long
 *     a pause is read as the hand still holding the stick.
 *   - Letting go. After the hold, V is walked to zero linearly over
 *     RT_MOUSE_LOOK_RELEASE_FIELDS fields, from wherever it stood. That is a
 *     thumb coming off a stick rather than a stick vanishing: the game's own
 *     return then walks the camera back, rate limited and eased as traced
 *     above. Motion during the release restarts from the V of that field, so
 *     catching the drag again picks up where the release had got to instead
 *     of jumping.
 *
 * There is no smoothing of the delta any more, and adding one back would be
 * a mistake: V is an integral of the deltas, so it already does not care how
 * a hand's pixels landed across field boundaries. Two fields of 3 pixels and
 * one of 6 reach the same deflection. The exponential average this file
 * carried while the mapping was a speed mapping had a real job (per-field
 * pixel counts are noisy at 125 to 1000 mouse reports per second into 59.94
 * fields) and no longer has one; under a position mapping it would only add
 * lag and blunt a fast drag.
 *
 * A floor-radius hold was considered while the mapping was still a speed and
 * is now doubly wrong. Deflection is a position, so holding at
 * RT_MOUSE_LOOK_GATE_FLOOR does not hold the camera: it commands its own
 * small offset and the camera walks back to that, which is the reported
 * reset slowed down rather than removed. There is no deflection the game
 * treats as non-neutral with no effect on where the camera sits, because
 * every response maps to an offset; the deflection that leaves the camera
 * where it is, is the one that put it there.
 *
 * The three constants are host choices, documented at their definitions.
 * RT_MOUSE_LOOK_DRAG_PIXELS and RT_MOUSE_LOOK_HOLD_FIELDS are the two a
 * player might want. Neither is a setting: input.mouse_look_drag_pixels
 * (40 to 1000 pixels, default 160) and input.mouse_look_hold_ms (0 to
 * 1000 ms, default 250) are the keys they would be, and nothing reads
 * those names today. docs/SETTINGS.md says the same.
 *
 * Runtime-internal, NOT part of the ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_HOST_MOUSE_LOOK_H
#define ICORECOMP_HOST_MOUSE_LOOK_H

#include <algorithm>
#include <cmath>
#include <cstdint>

/* Pixels of accumulated mouse motion that push the virtual stick from
 * centred to fully deflected at sensitivity 1. Host choice; see the header
 * comment for the window fraction and the hand travel it works out to.
 * There is no setting for it either: input.mouse_look_drag_pixels, 40 to
 * 1000, default 160, is the key it would be and nothing reads that name
 * yet. */
constexpr float RT_MOUSE_LOOK_DRAG_PIXELS = 160.0f;

/* Byte-space radius the smallest deflection reports. The game's radial dead
 * zone is 48 and this is the first whole unit past it, the same at every
 * angle. The square dead zone the camera's stick reader computes and then
 * discards is deliberately not cleared; the header comment says why, and
 * what it would look like if that reading were ever wrong. */
constexpr float RT_MOUSE_LOOK_GATE_FLOOR = 49.0f;

/* Top of the radius ramp. The game saturates at 120; see the header comment
 * on why this sits above it. */
constexpr float RT_MOUSE_LOOK_RAMP_TOP = 122.0f;

/* Fields of no mouse motion that still read as a hand holding the stick
 * where it is. 15 fields is 250 ms at 59.94 fields per second, and about 7
 * camera updates at the game's shipped two fields per update.
 *
 * A host choice: it is how long a pause reads as "still aiming there" rather
 * than "let go". Below about 4 fields it stops being a pause at all, and
 * above about 60 the camera stays parked long after the hand stopped. There
 * is no setting for it: input.mouse_look_hold_ms, 0 to 1000 ms, default 250,
 * is the key it would be and nothing reads that name yet. */
constexpr int RT_MOUSE_LOOK_HOLD_FIELDS = 15;

/* Fields the stick takes to walk back to centre once the hold has run out.
 * 6 fields is 100 ms, a thumb coming off a stick. Host choice, and the
 * smallest of the three: the game's own return is what actually moves the
 * camera afterwards, at 45 degrees per second, and this only decides how
 * abruptly the deflection that pointed it there goes away. One field would
 * be a stick vanishing rather than being released. */
constexpr int RT_MOUSE_LOOK_RELEASE_FIELDS = 6;

struct RtMouseLookOut {
    uint8_t x = 0x80, y = 0x80;
    bool active = false;
};

/* One byte away from the centre, rounding away from 127.5. The clamp is a
 * type limit: the octant scale above already keeps the value inside the
 * square, and 0 and 255 are the ends of the byte an axis is. */
inline uint8_t rt_mouse_look_byte(float centred) {
    const float v = 127.5f + centred;
    const float rounded = (centred >= 0.0f) ? std::ceil(v) : std::floor(v);
    if (rounded <= 0.0f) return 0;
    if (rounded >= 255.0f) return 255;
    return (uint8_t)(int)rounded;
}

/* Maps the virtual stick vector to a right-stick byte pair. (`vx`, `vy`) is
 * in unit-circle space and RtMouseLookStick keeps |V| <= 1; a longer vector
 * would only report the same saturated pair the game reads as a response of
 * 1, so nothing here rejects one. `invert_y` is input.mouse_look_invert_y.
 *
 * active is false only for a stick at dead centre, and the pair is then the
 * centred 0x80, 0x80 an untouched stick reports. A vector that is not finite
 * takes the same path: it cannot be a deflection, and host/mouse.cpp is the
 * layer with a logger, so it rejects and logs a non-finite delta before it
 * can ever reach the accumulation. */
inline RtMouseLookOut rt_mouse_look_map(float vx, float vy, bool invert_y) {
    RtMouseLookOut out;
    const float len = std::sqrt(vx * vx + vy * vy);
    if (!(len > 0.0f)) return out;

    const float ux = vx / len, uy = vy / len;
    const float m = std::min(1.0f, len);
    const float r = RT_MOUSE_LOOK_GATE_FLOOR
        + (RT_MOUSE_LOOK_RAMP_TOP - RT_MOUSE_LOOK_GATE_FLOOR) * m;

    const float ax = std::fabs(ux), ay = std::fabs(uy);
    const float amax = std::max(ax, ay);

    /* Degrees off the nearest cardinal, 0..45, and the game's divisor for
     * them. */
    const float t = std::atan2(std::min(ax, ay), amax)
        * (180.0f / 3.14159265358979323846f);
    const float g = 1.0f + 0.2f * t / 45.0f;
    const float box = 127.5f / (r * amax);
    const float s = std::max(1.0f, std::min(g, box));

    out.x = rt_mouse_look_byte(ux * r * s);
    out.y = rt_mouse_look_byte((invert_y ? -uy : uy) * r * s);
    out.active = true;
    return out;
}

/* The virtual stick: the mouse's position in stick space, plus the hold and
 * the release described in the header comment. One instance lives on the SDL
 * provider side (host/input.cpp) and step() is called once per pad field,
 * including the catch-up fields sif/pad.cpp runs after a long frame: those
 * are fields the mouse really did spend still, and each advances the hold by
 * one.
 *
 * Kept here, next to the mapping and free of SDL, so
 * host/mouse_look_selftest.cpp can drive it field by field. */
struct RtMouseLookStick {
    /* The stick vector, unit-circle space, |V| <= 1. */
    float vx = 0.0f, vy = 0.0f;
    /* Fields of stillness still read as the hand holding the stick. */
    int held = 0;
    /* Fields left in the release ramp, and the vector it started from. Zero
     * `releasing` means no release is running. */
    int releasing = 0;
    float rx = 0.0f, ry = 0.0f;

    /* Centres the stick at once, with no release ramp. The caller does this
     * the moment the mouse stops being the camera's: capture lost (focus,
     * the settings menu, mouse look turned off) or the pointer taking the
     * mouse for the game's own menus. A deflection that survived one of
     * those would be reported into whatever came next. */
    void reset() {
        vx = 0.0f;
        vy = 0.0f;
        held = 0;
        releasing = 0;
        rx = 0.0f;
        ry = 0.0f;
    }

    /* One field. (`fdx`, `fdy`) is the motion accumulated during it, (0, 0)
     * for a field where none arrived. `sens` is
     * input.mouse_look_sensitivity (the settings layer keeps it in
     * [0.05, 20]). Non-finite deltas never reach here: host/mouse.cpp
     * rejects and logs them at the event.
     *
     * Returns the pair to report for this field, with the same meaning as
     * rt_mouse_look_map's: inactive is a stick at rest. */
    RtMouseLookOut step(float fdx, float fdy, float sens, bool invert_y) {
        if (fdx != 0.0f || fdy != 0.0f) {
            const float k = sens / RT_MOUSE_LOOK_DRAG_PIXELS;
            vx += fdx * k;
            vy += fdy * k;
            /* The stick's gate: a drag past full deflection keeps its
             * direction and stops getting longer. */
            const float len = std::sqrt(vx * vx + vy * vy);
            if (len > 1.0f) {
                vx /= len;
                vy /= len;
            }
            held = RT_MOUSE_LOOK_HOLD_FIELDS;
            releasing = 0;
        } else if (held > 0) {
            /* Held: V is not touched at all. */
            if (--held == 0) {
                releasing = RT_MOUSE_LOOK_RELEASE_FIELDS;
                rx = vx;
                ry = vy;
            }
        } else if (releasing > 0) {
            --releasing;
            const float f = (float)releasing / (float)RT_MOUSE_LOOK_RELEASE_FIELDS;
            vx = rx * f;
            vy = ry * f;
        } else {
            vx = 0.0f;
            vy = 0.0f;
        }
        return rt_mouse_look_map(vx, vy, invert_y);
    }
};

#endif /* ICORECOMP_HOST_MOUSE_LOOK_H */
