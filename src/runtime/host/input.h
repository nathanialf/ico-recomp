/* host/input.h: host-side controller state for the virtual DUALSHOCK 2.
 *
 * Two providers behind one interface, selected at rt_input_init():
 *
 *   1. Scripted (ICORECOMP_INPUT_SCRIPT=file): deterministic input for
 *      headless verification. Line format, one step per line:
 *
 *          <field_number> [button ...] [lx ly rx ry]
 *
 *      field_number  pad field counter (one per field of the programmed
 *                    video mode, 59.94/s on NTSC and 50/s on PAL,
 *                    counted from boot) at which this step takes effect.
 *      button        any of: up down left right cross circle square triangle
 *                    l1 l2 r1 r2 start select l3 r3 none
 *      lx ly rx ry   optional stick axes 0..255 (center 128); all four or
 *                    none. Omitted = centered.
 *
 *      A step holds until the next line's field number. '#' starts a
 *      comment. Lines must be sorted by field number.
 *
 *   2. SDL3 keyboard + mouse + gamepad, active only when the paraLLEl-GS
 *      window path already initialized SDL video (the dump/headless path
 *      never touches SDL; without the window there is no key focus anyway).
 *      The first two detected gamepads are opened, one per pad port.
 *
 *      Two players. Pad port 0 is player 1: the keyboard, the mouse and the
 *      first gamepad all feed it. Pad port 1 is player 2 and is fed by the
 *      second gamepad alone, through its own binding table input.gamepad2
 *      (the same sixteen DS2 slots, and neither host hotkey: the menu key
 *      and the screenshot key are player 1's). Port 1 reports "no
 *      controller" until a second gamepad is open, so the PAL disc's
 *      two-player mode sees an empty port exactly as it would with nothing
 *      plugged in. Unplugging player 1's pad moves player 2's down to port
 *      0 rather than leaving port 0 empty. The deadzones,
 *      gameplay.run_any_direction and the rumble the game sends are per
 *      port; the mouse, mouse look, the wheel queue and the menu pointer
 *      are player 1's only.
 *
 *      Both maps come from rt_settings().input (host/settings.h), rebuilt
 *      whenever rt_settings_generation() moves. The compiled-in defaults
 *      there reproduce the pre-settings map:
 *
 *          arrows            d-pad
 *          W A S D           left stick
 *          I J K L           right stick
 *          X                 cross        C   circle
 *          Z                 square       Space  triangle
 *          Q / E             L1 / R1      1 3 L2 / R2
 *          T / Y             L3 / R3
 *          Enter             start        Backspace  select
 *
 *      and, on the pad, the SDL3 gamepad layout (south=cross east=circle
 *      west=square north=triangle, both triggers as L2/R2, pressed past
 *      a raw axis value of 8192 of 32767).
 *
 *      The mouse is the third device. Its sixteen button slots come from
 *      input.mouse (host/settings.h) written in host/mouse_names.h's names,
 *      and most of them ship unbound: "" is a real value there and means
 *      exactly that, so it is not reported. A name that is not empty and
 *      does not resolve leaves the slot unbound with a log line, because the
 *      mouse is the one device with no compiled-in default worth falling
 *      back to. Buttons are read as held state; the two wheel directions are
 *      pulses (below).
 *
 * On the right stick the three devices have a precedence, because all three
 * can write it: keyboard first, then mouse look, then the gamepad. Each of
 * them fills only an axis still sitting at the centred 0x80, so a deflected
 * key wins over the mouse and the mouse wins over an idle pad stick. Mouse
 * look (input.mouse_look) drives a virtual stick with the pointer: the
 * accumulated motion moves a deflection that stays where the drag left it,
 * because the game's camera stick is a position and not a velocity.
 * host/mouse.cpp collects the motion and host/mouse_look.h holds the stick
 * and does the byte mapping, and neither changes anything the game computes.
 * It is the one device that does not need a pad plugged in, so it is sampled
 * outside the gamepad branch, and it reports a pair on every field the stick
 * is off centre rather than only on fields the mouse moved.
 *
 * Where the mouse motion, buttons and wheel go is decided once per field,
 * and each goes to exactly one place:
 *
 *   - While the pointer owns the mouse (rt_guest_menu_wants_mouse(),
 *     guest/menu_nav.h: one of the game's own menus is up) the field's
 *     motion moves the drawn cursor and the camera stick
 *     sees none of it, and every button transition and every wheel tick is
 *     handed to guest/menu_nav.cpp with no mouse bind pressed. That module
 *     turns the pointer into the D-pad and cross presses the game's menu
 *     already reads, which arrive as rt_guest_menu_pulse_bits() and are
 *     ORed into the same field's buttons.
 *   - Otherwise the motion goes to the camera stick through mouse look and
 *     the bound slots are pressed from the held button state, gated on
 *     window focus.
 *
 * A wheel bind is a pulse rather than a level, because a wheel has no held
 * state: each tick is queued and presses its slot for one field, then
 * releases it for one field before the next queued tick, so two ticks read
 * as two presses instead of one long one. The queue is capped (logged when
 * it bites) and is dropped on a focus loss, on a settings rebuild, and when
 * the pointer takes the mouse.
 *
 * Both providers can be inactive (headless, no script): every port then
 * reports "no controller" and sif/pad.cpp presents an empty port. The
 * scripted provider drives port 0 only, so port 1 is an empty port for
 * every scripted run and a scripted run's input stays bit-identical.
 *
 * While the settings menu is up (rt_ui_wants_input(), ui/ui.h) the SDL
 * provider reports a centered pad with no buttons instead of sampling the
 * devices, so keys and pad presses aimed at the menu do not also reach the
 * game. The mouse motion, the button transitions and the wheel ticks are
 * thrown away for the same fields, since a device that accumulates would
 * otherwise hand the game everything the menu did the moment the menu
 * closes, and no guest-menu pulse bits are applied either: a neutral pad is
 * a neutral pad. The scripted provider is never gated that way: a scripted
 * run does not bring the UI up at all.
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

/* Button names accepted in an input script, paired with the bit each one
 * sets. The script parser (host/input.cpp) is the only consumer today; it
 * lives here because the script format is documented above and the names
 * are part of it. */
constexpr struct { const char* name; uint16_t bit; } RT_PAD_BUTTON_NAMES[16] = {
    {"select", RT_PAD_SELECT}, {"l3", RT_PAD_L3}, {"r3", RT_PAD_R3},
    {"start", RT_PAD_START}, {"up", RT_PAD_UP}, {"right", RT_PAD_RIGHT},
    {"down", RT_PAD_DOWN}, {"left", RT_PAD_LEFT}, {"l2", RT_PAD_L2},
    {"r2", RT_PAD_R2}, {"l1", RT_PAD_L1}, {"r1", RT_PAD_R1},
    {"triangle", RT_PAD_TRIANGLE}, {"circle", RT_PAD_CIRCLE},
    {"cross", RT_PAD_CROSS}, {"square", RT_PAD_SQUARE},
};

struct RtPadState {
    uint16_t buttons = 0;           /* active-high RT_PAD_* mask */
    uint8_t lx = 0x80, ly = 0x80;   /* left stick, 0x80 = centered */
    uint8_t rx = 0x80, ry = 0x80;   /* right stick */
};

/* Pad ports the virtual IOP serves, and the one authority for that count:
 * sif/pad.cpp sizes its port array from this. Two, because the PAL disc's
 * two-player mode opens port 1 for the second player, who drives Yorda.
 * Slot 0 only on each: neither disc opens a multitap. */
constexpr int RT_PAD_PORTS = 2;

/* Selects the provider (script beats SDL; neither = no controller). Safe to
 * call before or after the GS backend exists; the SDL provider re-probes
 * lazily on the first poll. */
void rt_input_init();

/* Advances the provider to `field` (the pad field counter) and samples the
 * host devices. Called once per field by sif/pad.cpp before it builds
 * frames. */
void rt_input_poll(uint64_t field);

/* Current state of the virtual controller on `port`. Returns false when no
 * controller should appear on that port: no provider at all, a port outside
 * RT_PAD_PORTS, or port 1 with no second gamepad open. Port 0 answers true
 * whenever a provider is live, with or without a pad attached, because the
 * keyboard and the mouse are player 1's controller. */
bool rt_input_get(int port, RtPadState* out);

/* Which kind of device the player last used. The drawn cursor on the game's
 * own menus (ui/ui_menu_cursor.cpp) is the consumer: an arrow on screen while
 * the player is on a pad is an arrow nobody can move.
 *
 * Moved by the SDL provider, once per field, from what the devices actually
 * did that field: a held bound key, a mouse button, a wheel tick or real
 * pointer motion say keyboard and mouse; a held pad button, an axis bind past
 * its press point or a stick pushed a quarter of the way from centre say
 * controller. A field that shows both leaves it alone, and so does a field
 * that shows neither, so it names the last device that was unambiguously
 * used rather than the one being used right now.
 *
 * It boots as the controller: a run where nobody touches a mouse never draws
 * a cursor. A scripted run and a build without SDL leave it there for the
 * whole run, since neither has devices to watch. Each change is logged as
 * `last device is now the controller` or `... keyboard and mouse`. */
enum RtInputDevice {
    RT_INPUT_DEVICE_CONTROLLER,
    RT_INPUT_DEVICE_KBM,
};
RtInputDevice rt_input_last_device();

/* True while the SDL provider is the active one: it was selected at init and
 * SDL video is up. host/mouse.cpp asks before it captures the pointer for
 * mouse look, because a scripted run must stay bit-identical and must not
 * have the pointer grabbed for a mouse whose motion nothing will read.
 * Always false in a build without SDL. */
bool rt_input_sdl_active();

/* Probes for a gamepad the moment SDL video exists, even before any guest
 * thread runs: the launcher (ui/ui_launcher.cpp) needs pad focus and button
 * events of its own, and this module's own probe used to happen only from
 * the first poll after rt_input_init()/rt_pad_register_services() (sif/
 * pad.cpp), which is well after the launcher has already been showing a
 * window. A no-op once a gamepad has already been probed for and always a
 * no-op in a build with no SDL. Called from ui/ui.cpp's rt_ui_init(); every
 * later poll re-probes too (host/input.cpp's own sdl_poll), in case no
 * window existed the first time the launcher tried. */
void rt_input_sdl_gamepad_probe();

#ifdef ICORECOMP_HAVE_SDL
/* Forward-declared, not included: this header is included by files that see
 * no SDL. SDL3/SDL.h's own typedef agrees with it, so either include order
 * works (same pattern as ui/ui_internal.h). */
union SDL_Event;

/* SDL_EVENT_GAMEPAD_ADDED / SDL_EVENT_GAMEPAD_REMOVED hot-plug, from the
 * one event pump (host/window.cpp's rt_window_pump), ahead of rt_ui_
 * handle_sdl_event so the pad this module tracks stays current whether or
 * not a document is up. ADDED opens the new pad onto the first free pad
 * port, so the second pad attached becomes player 2; with both ports taken
 * it logs the pad's name and closes it again rather than displacing a
 * player mid-run (SDL3 also reports every pad already attached at init as
 * ADDED, which SDL_GetGamepadFromID answering non-null tells apart from a
 * fresh attach, so this never reopens a pad it is already using). REMOVED
 * closes the pad that was unplugged, moves a pad on a higher port down into
 * the gap, and then fills whatever port is still free from any pad SDL
 * still lists that is not already open. A no-op for the script provider and
 * for every event type but these two. Plain SDL calls, legal from the
 * pump. */
void rt_input_on_sdl_event(const SDL_Event& e);
#endif

/* Actuator (rumble) values from the game: small motor 0/1, big motor
 * 0..255, for one pad port. Forwarded to the gamepad open on that port
 * whenever the SDL provider is active, and recorded per port for the log on
 * every change, so the log shows what the game asked each player's pad
 * for. */
void rt_input_set_actuators(int port, uint8_t small_motor, uint8_t big_motor);

#endif /* ICORECOMP_HOST_INPUT_H */
