/* host/input.h: host-side controller state for the virtual DUALSHOCK 2.
 *
 * Two providers behind one interface, selected at rt_input_init():
 *
 *   1. Scripted (ICORECOMP_INPUT_SCRIPT=file): deterministic input for
 *      headless verification. Line format, one step per line:
 *
 *          <field_number> [button ...] [lx ly rx ry]
 *
 *      field_number  pad field counter (one per NTSC field, ~59.94/s,
 *                    counted from boot) at which this step takes effect.
 *      button        any of: up down left right cross circle square triangle
 *                    l1 l2 r1 r2 start select l3 r3 none
 *      lx ly rx ry   optional stick axes 0..255 (center 128); all four or
 *                    none. Omitted = centered.
 *
 *      A step holds until the next line's field number. '#' starts a
 *      comment. Lines must be sorted by field number.
 *
 *   2. SDL3 gamepad + keyboard, active only when the paraLLEl-GS window
 *      path already initialized SDL video (the dump/headless path never
 *      touches SDL; without the window there is no key focus anyway).
 *      First detected gamepad maps through the SDL3 gamepad layout
 *      (south=cross east=circle west=square north=triangle). Keyboard map:
 *
 *          arrows            d-pad
 *          W A S D           left stick
 *          I J K L           right stick
 *          X                 cross        C   circle
 *          Z                 square       V   triangle
 *          Q / E             L1 / R1      1 3 L2 / R2
 *          T / Y             L3 / R3
 *          Enter             start        Backspace  select
 *
 * Both providers can be inactive (headless, no script): every port then
 * reports "no controller" and sif/pad.cpp presents an empty port.
 *
 * While the settings menu is up (rt_ui_wants_input(), ui/ui.h) the SDL
 * provider reports a centered pad with no buttons instead of sampling the
 * devices, so keys and pad presses aimed at the menu do not also reach the
 * game. The scripted provider is never gated that way: a scripted run does
 * not bring the UI up at all.
 *
 * Runtime-internal, NOT part of the ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_HOST_INPUT_H
#define ICORECOMP_HOST_INPUT_H

#include <cstdint>

/* Button bits, active-high, matching the DS2 wire order so sif/pad.cpp can
 * invert the mask straight into frame bytes 2/3 (public pad protocol fact):
 * byte 2 = bits 0-7, byte 3 = bits 8-15. */
constexpr uint16_t RT_PAD_SELECT   = 1u << 0;
constexpr uint16_t RT_PAD_L3       = 1u << 1;
constexpr uint16_t RT_PAD_R3       = 1u << 2;
constexpr uint16_t RT_PAD_START    = 1u << 3;
constexpr uint16_t RT_PAD_UP       = 1u << 4;
constexpr uint16_t RT_PAD_RIGHT    = 1u << 5;
constexpr uint16_t RT_PAD_DOWN     = 1u << 6;
constexpr uint16_t RT_PAD_LEFT     = 1u << 7;
constexpr uint16_t RT_PAD_L2       = 1u << 8;
constexpr uint16_t RT_PAD_R2       = 1u << 9;
constexpr uint16_t RT_PAD_L1       = 1u << 10;
constexpr uint16_t RT_PAD_R1       = 1u << 11;
constexpr uint16_t RT_PAD_TRIANGLE = 1u << 12;
constexpr uint16_t RT_PAD_CIRCLE   = 1u << 13;
constexpr uint16_t RT_PAD_CROSS    = 1u << 14;
constexpr uint16_t RT_PAD_SQUARE   = 1u << 15;

struct RtPadState {
    uint16_t buttons = 0;           /* active-high RT_PAD_* mask */
    uint8_t lx = 0x80, ly = 0x80;   /* left stick, 0x80 = centered */
    uint8_t rx = 0x80, ry = 0x80;   /* right stick */
};

/* Selects the provider (script beats SDL; neither = no controller). Safe to
 * call before or after the GS backend exists; the SDL provider re-probes
 * lazily on the first poll. */
void rt_input_init();

/* Advances the provider to `field` (the pad field counter) and samples the
 * host devices. Called once per field by sif/pad.cpp before it builds
 * frames. */
void rt_input_poll(uint64_t field);

/* Current state of the virtual controller on `port`. Returns false when no
 * controller should appear on that port (no provider, or port != 0). */
bool rt_input_get(int port, RtPadState* out);

/* Actuator (rumble) values from the game: small motor 0/1, big motor
 * 0..255. Forwarded to SDL gamepad rumble when that provider is active;
 * always recorded for the log. */
void rt_input_set_actuators(int port, uint8_t small_motor, uint8_t big_motor);

#endif /* ICORECOMP_HOST_INPUT_H */
