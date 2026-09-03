/* host/input.cpp: host input providers for the virtual DUALSHOCK 2.
 * Interface and provider selection are documented in input.h.
 */
#include "input.h"

#include "../guest/menu_nav.h"
#include "../runtime.h"
#include "../ui/ui.h"
#include "mouse.h"
#include "mouse_look.h"
#include "mouse_names.h"
#include "settings.h"
#include "stick_shape.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef ICORECOMP_PGS_SDL
#include <SDL3/SDL.h>
#endif

namespace {

enum class Provider { None, Script, Sdl };
Provider g_provider = Provider::None;
bool g_inited = false;

RtPadState g_state;             /* port 0; port 1 never has a controller */
uint8_t g_act_small = 0, g_act_big = 0;

/* The device the player last used, and the only state behind
 * rt_input_last_device(). It boots as the controller by decision, so a run
 * that never touches a mouse never shows a mouse cursor; the first bound key,
 * mouse button, wheel tick or mouse motion moves it, and the first pad button,
 * pressed axis or pushed stick moves it back.
 *
 * Only the SDL provider writes it. A scripted run has no devices to watch and
 * leaves it where it booted, and no build without SDL can move it either,
 * which is why the reader below is not inside the guard. */
RtInputDevice g_last_device = RT_INPUT_DEVICE_CONTROLLER;

/* ---- scripted provider ---------------------------------------------------- */

struct Step {
    uint64_t field = 0;
    uint16_t buttons = 0;
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
};
std::vector<Step> g_script;
size_t g_script_pos = 0;

bool parse_script(const char* path) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) {
        rt_log("input", "ICORECOMP_INPUT_SCRIPT=%s: fopen failed", path);
        return false;
    }
    char line[512];
    int lineno = 0;
    uint64_t last_field = 0;
    while (std::fgets(line, sizeof(line), f)) {
        ++lineno;
        char* hash = std::strchr(line, '#');
        if (hash) *hash = 0;
        std::vector<std::string> tok;
        for (char* p = std::strtok(line, " \t\r\n"); p; p = std::strtok(nullptr, " \t\r\n")) {
            tok.push_back(p);
        }
        if (tok.empty()) continue;
        Step s;
        char* end = nullptr;
        s.field = std::strtoull(tok[0].c_str(), &end, 10);
        if (end == tok[0].c_str() || *end) {
            rt_fatal("input", nullptr, "%s:%d: bad field number '%s'", path, lineno, tok[0].c_str());
        }
        if (!g_script.empty() && s.field < last_field) {
            rt_fatal("input", nullptr, "%s:%d: field %llu out of order (previous %llu)",
                path, lineno, (unsigned long long)s.field, (unsigned long long)last_field);
        }
        last_field = s.field;
        /* Trailing 4 numeric tokens = axes; everything between = buttons. */
        size_t nbtn = tok.size() - 1;
        uint8_t axes[4] = {0x80, 0x80, 0x80, 0x80};
        if (tok.size() >= 5) {
            bool numeric = true;
            for (size_t i = tok.size() - 4; i < tok.size(); ++i) {
                char* e = nullptr;
                unsigned long v = std::strtoul(tok[i].c_str(), &e, 10);
                if (e == tok[i].c_str() || *e || v > 255) { numeric = false; break; }
            }
            if (numeric) {
                nbtn = tok.size() - 5;
                for (size_t i = 0; i < 4; ++i) {
                    axes[i] = (uint8_t)std::strtoul(tok[tok.size() - 4 + i].c_str(), nullptr, 10);
                }
            }
        }
        s.lx = axes[0]; s.ly = axes[1]; s.rx = axes[2]; s.ry = axes[3];
        for (size_t i = 1; i <= nbtn; ++i) {
            if (tok[i] == "none") continue;
            bool found = false;
            for (const auto& b : RT_PAD_BUTTON_NAMES) {
                if (tok[i] == b.name) { s.buttons |= b.bit; found = true; break; }
            }
            if (!found) {
                rt_fatal("input", nullptr, "%s:%d: unknown button '%s'", path, lineno, tok[i].c_str());
            }
        }
        g_script.push_back(s);
    }
    std::fclose(f);
    rt_log("input", "script provider: %zu steps from %s", g_script.size(), path);
    return !g_script.empty();
}

void script_poll(uint64_t field) {
    bool changed = false;
    while (g_script_pos < g_script.size() && g_script[g_script_pos].field <= field) {
        const Step& s = g_script[g_script_pos];
        g_state.buttons = s.buttons;
        g_state.lx = s.lx; g_state.ly = s.ly; g_state.rx = s.rx; g_state.ry = s.ry;
        changed = true;
        ++g_script_pos;
    }
    if (changed) {
        rt_log("input", "script step -> field=%llu buttons=0x%04x sticks=%u,%u,%u,%u",
            (unsigned long long)field, g_state.buttons,
            g_state.lx, g_state.ly, g_state.rx, g_state.ry);
    }
}

/* ---- SDL provider --------------------------------------------------------- */

#ifdef ICORECOMP_PGS_SDL

SDL_Gamepad* g_gamepad = nullptr;
bool g_sdl_probed = false;

/* SDL video is initialized by the paraLLEl-GS window path on the main
 * thread; everything here runs on the same OS thread (the scheduler and its
 * coroutines never leave it). If video never comes up (headless), this
 * provider stays dormant. */
bool sdl_active() {
    return SDL_WasInit(SDL_INIT_VIDEO) != 0;
}

void sdl_probe() {
    if (g_sdl_probed) return;
    g_sdl_probed = true;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        rt_log("input", "SDL gamepad subsystem init failed: %s (keyboard only)", SDL_GetError());
    } else {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids && count > 0) {
            g_gamepad = SDL_OpenGamepad(ids[0]);
            rt_log("input", "SDL gamepad: %s", g_gamepad ? SDL_GetGamepadName(g_gamepad) : "(open failed)");
        } else {
            rt_log("input", "SDL: no gamepad detected; keyboard map active (see host/input.h)");
        }
        SDL_free(ids);
    }
}

uint8_t axis_to_u8(Sint16 v) {
    int x = ((int)v + 32768) >> 8;
    return (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x));
}

/* ---- tables built from settings ------------------------------------------
 *
 * The first sixteen RtKeyBind slots and the first sixteen RtPadBind slots are
 * the same sixteen DS2 buttons in the same order (host/settings.h), so one
 * bit table serves both. The keyboard's eight stick slots and each device's
 * menu slot are handled separately: the menu key is consumed by the UI pump
 * (ui/ui_events.cpp) and is not a pad binding at all.
 */
constexpr uint16_t kSlotBits[16] = {
    RT_PAD_UP, RT_PAD_DOWN, RT_PAD_LEFT, RT_PAD_RIGHT,
    RT_PAD_CROSS, RT_PAD_CIRCLE, RT_PAD_SQUARE, RT_PAD_TRIANGLE,
    RT_PAD_L1, RT_PAD_R1, RT_PAD_L2, RT_PAD_R2, RT_PAD_L3, RT_PAD_R3,
    RT_PAD_START, RT_PAD_SELECT,
};

struct KeyBind {
    SDL_Scancode sc;
    uint16_t bit;
};

/* A gamepad slot resolves either to a button or to one direction of an axis
 * (the "lefttrigger+" convention, a trailing '+' or '-' on an SDL axis
 * name). Exactly one of `button` and `axis` is valid. */
struct PadBind {
    uint16_t bit;
    SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
    SDL_GamepadAxis axis = SDL_GAMEPAD_AXIS_INVALID;
    int dir = 0;                /* +1 or -1, axis binds only */
};

/* A mouse slot resolves to one RtMouseInput (host/mouse_names.h): a button,
 * which is read as level state, or a wheel direction, which is a pulse. Only
 * bound slots are in the table; an unbound one is simply absent. */
struct MouseBind {
    uint16_t bit;
    RtMouseInput input;
};

std::vector<KeyBind> g_key_buttons;
/* Indexed by slot - RT_KB_LSTICK_UP; SDL_SCANCODE_UNKNOWN means unbound. */
SDL_Scancode g_key_stick[8] = {};
std::vector<PadBind> g_pad_binds;
std::vector<MouseBind> g_mouse_binds;

/* The virtual stick the mouse drives (host/mouse_look.h): the camera stick's
 * mouse source, stepped once per field by sdl_poll and centred whenever the
 * mouse stops being the camera's. */
RtMouseLookStick g_mouse_look;

/* Wheel binds are pulses, not levels: the wheel has no held state to read,
 * so each tick has to become a press the game can see and then a release.
 * One queue per direction, since up and down are separate slots.
 *
 * `pending` is the ticks still owed a press. `pressed` says the slot was
 * pressed on the previous field, which forces the next field to release it:
 * the game samples the pad once per field and needs the released field
 * between two presses, or two ticks read as one long press.
 *
 * The cap is a host limit on a host device and it is logged when it bites: a
 * flicked wheel can queue more ticks than the player could possibly mean, and
 * a queue that outlives the flick by seconds is worse than dropping the tail
 * of it. Nothing the game supplied is touched by this. */
constexpr int kWheelQueueCap = 16;
struct WheelPulse {
    int pending = 0;
    bool pressed = false;
};
WheelPulse g_wheel[2];          /* [0] = wheel up, [1] = wheel down */
bool g_wheel_cap_logged = false;

void wheel_queue_reset() {
    g_wheel[0] = WheelPulse{};
    g_wheel[1] = WheelPulse{};
}

/* The rt_settings_generation() the tables above were built from. Zero is
 * never a live generation (settings.cpp starts at 1), so the first poll
 * always builds. */
unsigned g_tables_gen = 0;

/* Cleared when the toggle goes back off, so a later re-enable says so again;
 * the line is worth one log per enable, not one per field. */
bool g_gate_expand_logged = false;

/* Resolves one stored name for a keyboard slot, falling back to the compiled
 * default with a named log line. Never returns "no binding" quietly: if even
 * the default fails to resolve, that is this build's table and SDL
 * disagreeing, and it says so. */
SDL_Scancode resolve_scancode(int slot, const std::string& name) {
    SDL_Scancode sc = name.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(name.c_str());
    if (sc != SDL_SCANCODE_UNKNOWN) return sc;

    const char* def = rt_settings_default_binding(RT_BIND_KEYBOARD, slot);
    rt_log("input", "input.keyboard.%s = \"%s\" is not an SDL scancode name; using the default \"%s\"",
        rt_settings_binding_key(RT_BIND_KEYBOARD, slot), name.c_str(), def);
    sc = SDL_GetScancodeFromName(def);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        rt_log("input", "input.keyboard.%s: the compiled-in default \"%s\" is not an SDL scancode"
            " name either; this build's default table and SDL disagree, so that slot has no key"
            " this run", rt_settings_binding_key(RT_BIND_KEYBOARD, slot), def);
    }
    return sc;
}

/* Splits "lefttrigger+" into the SDL axis token and the direction. Returns
 * false when `name` carries no direction suffix, which means it has to
 * resolve as a button instead. */
bool split_axis_name(const std::string& name, std::string* token, int* dir) {
    if (name.size() < 2) return false;
    const char last = name[name.size() - 1];
    if (last != '+' && last != '-') return false;
    *token = name.substr(0, name.size() - 1);
    *dir = (last == '+') ? 1 : -1;
    return true;
}

bool resolve_pad_name(const std::string& name, PadBind* out) {
    std::string token;
    int dir = 0;
    if (split_axis_name(name, &token, &dir)) {
        const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(token.c_str());
        if (axis == SDL_GAMEPAD_AXIS_INVALID) return false;
        out->axis = axis;
        out->dir = dir;
        return true;
    }
    const SDL_GamepadButton button = name.empty()
        ? SDL_GAMEPAD_BUTTON_INVALID : SDL_GetGamepadButtonFromString(name.c_str());
    if (button == SDL_GAMEPAD_BUTTON_INVALID) return false;
    out->button = button;
    return true;
}

void rebuild_tables() {
    const RtSettings& cfg = rt_settings();

    g_key_buttons.clear();
    g_key_buttons.reserve(16);
    for (int slot = 0; slot < 16; ++slot) {
        const SDL_Scancode sc = resolve_scancode(slot, cfg.input.keyboard[slot]);
        if (sc != SDL_SCANCODE_UNKNOWN) g_key_buttons.push_back({sc, kSlotBits[slot]});
    }
    for (int i = 0; i < 8; ++i) {
        g_key_stick[i] = resolve_scancode(RT_KB_LSTICK_UP + i, cfg.input.keyboard[RT_KB_LSTICK_UP + i]);
    }

    g_pad_binds.clear();
    g_pad_binds.reserve(16);
    for (int slot = 0; slot < 16; ++slot) {
        PadBind b;
        b.bit = kSlotBits[slot];
        if (!resolve_pad_name(cfg.input.gamepad[slot], &b)) {
            const char* def = rt_settings_default_binding(RT_BIND_GAMEPAD, slot);
            rt_log("input", "input.gamepad.%s = \"%s\" is not an SDL gamepad button or axis name;"
                " using the default \"%s\"",
                rt_settings_binding_key(RT_BIND_GAMEPAD, slot), cfg.input.gamepad[slot].c_str(), def);
            if (!resolve_pad_name(def, &b)) {
                rt_log("input", "input.gamepad.%s: the compiled-in default \"%s\" is not an SDL"
                    " gamepad button or axis name either; this build's default table and SDL"
                    " disagree, so that slot has no pad input this run",
                    rt_settings_binding_key(RT_BIND_GAMEPAD, slot), def);
                continue;
            }
        }
        g_pad_binds.push_back(b);
    }

    /* The mouse is the one device with no compiled-in default to fall back
     * to: most of its slots ship unbound ("" is a real value here, see
     * host/settings.h), so an empty name is a deliberate choice and says
     * nothing. A name that is not empty and does not resolve is a typo in
     * the file, and there is nothing sensible to substitute for it, so the
     * slot stays unbound and the line names it once per rebuild. */
    g_mouse_binds.clear();
    g_mouse_binds.reserve(RT_MB_COUNT);
    for (int slot = 0; slot < RT_MB_COUNT; ++slot) {
        const std::string& name = cfg.input.mouse[slot];
        if (name.empty()) continue;
        RtMouseInput in = RT_MOUSE_LEFT;
        if (!rt_mouse_input_from_name(name, &in)) {
            rt_log("input", "input.mouse.%s = \"%s\" is not a mouse input name (%s, %s, %s, %s,"
                " %s, %s, %s); that slot has no mouse input this run",
                rt_settings_binding_key(RT_BIND_MOUSE, slot), name.c_str(),
                rt_mouse_input_name(RT_MOUSE_LEFT), rt_mouse_input_name(RT_MOUSE_RIGHT),
                rt_mouse_input_name(RT_MOUSE_MIDDLE), rt_mouse_input_name(RT_MOUSE_X1),
                rt_mouse_input_name(RT_MOUSE_X2), rt_mouse_input_name(RT_MOUSE_WHEEL_UP),
                rt_mouse_input_name(RT_MOUSE_WHEEL_DOWN));
            continue;
        }
        g_mouse_binds.push_back({kSlotBits[slot], in});
    }
    /* A rebuild can retire the slot a queued tick was aimed at, so the queue
     * does not survive one. */
    wheel_queue_reset();
}

void sync_tables() {
    const unsigned gen = rt_settings_generation();
    if (gen == g_tables_gen) return;
    g_tables_gen = gen;
    rebuild_tables();
}

/* Radial deadzone with remainder rescale, applied to the raw SDL stick pair
 * before axis_to_u8.
 *
 * dz == 0 (the shipped default) skips the math entirely rather than running
 * it with a zero threshold: the float round trip would not be bit-identical
 * to the pre-settings build, and every existing user is on 0.
 *
 * The clamp on the way out is a type limit, not a value policy: a stick with
 * a square gate reports up to 32767 on both axes at once, r reaches 1.41,
 * and the rescale can push a component past the int16 the SDL axis is. It
 * cannot be reached at all with a round gate. */
Sint16 clamp_axis(float v) {
    if (v <= -32768.0f) return -32768;
    if (v >= 32767.0f) return 32767;
    return (Sint16)v;
}

void apply_deadzone(float dz, Sint16* x, Sint16* y) {
    if (dz <= 0.0f) return;
    const float fx = (float)*x, fy = (float)*y;
    const float r = std::sqrt(fx * fx + fy * fy) / 32767.0f;
    if (r <= dz) {
        *x = 0;
        *y = 0;
        return;
    }
    const float scale = ((r - dz) / (1.0f - dz)) / r;
    *x = clamp_axis(fx * scale);
    *y = clamp_axis(fy * scale);
}

/* RtMouseInput (host/mouse_names.h) to the SDL button index host/mouse.h
 * reports. Zero for the two wheel directions, which are not buttons and have
 * no held state to read. */
int sdl_button_of(RtMouseInput in) {
    switch (in) {
    case RT_MOUSE_LEFT:   return SDL_BUTTON_LEFT;
    case RT_MOUSE_MIDDLE: return SDL_BUTTON_MIDDLE;
    case RT_MOUSE_RIGHT:  return SDL_BUTTON_RIGHT;
    case RT_MOUSE_X1:     return SDL_BUTTON_X1;
    case RT_MOUSE_X2:     return SDL_BUTTON_X2;
    default:              return 0;
    }
}

/* The DS2 bits bound to one wheel direction, or 0 when it is unbound. The
 * commit-time rules in settings.cpp already reject two slots on one device
 * sharing a name, so this is one bit in practice; it ORs rather than
 * assuming that. */
uint16_t wheel_bits(RtMouseInput direction) {
    uint16_t bits = 0;
    for (const MouseBind& b : g_mouse_binds) {
        if (b.input == direction) bits |= b.bit;
    }
    return bits;
}

void wheel_enqueue(int dir, int ticks) {
    int room = kWheelQueueCap - g_wheel[dir].pending;
    if (room < 0) room = 0;
    if (ticks > room) {
        if (!g_wheel_cap_logged) {
            g_wheel_cap_logged = true;
            rt_log("input", "mouse wheel press queue is full at %d pending presses; ticks past"
                " that are dropped this field (a flick queues more presses than it can mean)",
                kWheelQueueCap);
        }
        ticks = room;
    }
    g_wheel[dir].pending += ticks;
}

/* Records which device the field's inputs came from. Called once per field
 * with what each side actually did.
 *
 * A field that shows both, which is a hand on the keyboard and a thumb on the
 * pad at the same time, leaves the answer alone: there is no honest winner
 * between them, and the last unambiguous one is a better answer than an
 * arbitrary rule. A field that shows neither leaves it alone too, so the
 * answer survives a player letting go of everything. */
void note_device(bool kbm, bool pad) {
    if (kbm == pad) return;                 /* both, or neither */
    const RtInputDevice now = kbm ? RT_INPUT_DEVICE_KBM : RT_INPUT_DEVICE_CONTROLLER;
    if (now == g_last_device) return;
    g_last_device = now;
    rt_log("input", "last device is now %s",
        now == RT_INPUT_DEVICE_KBM ? "keyboard and mouse" : "the controller");
}

/* Throws away whatever the mouse recorded since the last field. Used
 * wherever those events are not the game's: the settings menu is up, or the
 * pointer owns them. */
void mouse_events_drop() {
    RtMouseButtonEvent drop[16];
    while (rt_mouse_take_button_events(drop, 16) > 0) {}
    rt_mouse_take_wheel_ticks();
    wheel_queue_reset();
}

void sdl_poll(uint64_t field) {
    sdl_probe();
    sync_tables();

    const RtSettings& cfg = rt_settings();
    RtPadState s;

    /* What this field saw of each device, for rt_input_last_device(). Only
     * deliberate input counts: a held bound key, a mouse button, a wheel
     * tick or real pointer motion on one side; a held pad button, an axis
     * bind past its press point or a stick pushed a quarter of the way on
     * the other. An idle device of either kind says nothing. */
    bool kbm_seen = false;
    bool pad_seen = false;

    /* Keyboard (event pump runs in the WSI present path each field). */
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (keys) {
        for (const KeyBind& b : g_key_buttons) {
            if (keys[b.sc]) { s.buttons |= b.bit; kbm_seen = true; }
        }
        /* Left before right and up before down on each stick, so a user
         * holding both opposing keys gets the same answer as the
         * pre-settings build (the later assignment wins). */
        struct { int slot; uint8_t* axis; uint8_t value; } sticks[] = {
            {RT_KB_LSTICK_LEFT,  &s.lx, 0x00}, {RT_KB_LSTICK_RIGHT, &s.lx, 0xFF},
            {RT_KB_LSTICK_UP,    &s.ly, 0x00}, {RT_KB_LSTICK_DOWN,  &s.ly, 0xFF},
            {RT_KB_RSTICK_LEFT,  &s.rx, 0x00}, {RT_KB_RSTICK_RIGHT, &s.rx, 0xFF},
            {RT_KB_RSTICK_UP,    &s.ry, 0x00}, {RT_KB_RSTICK_DOWN,  &s.ry, 0xFF},
        };
        for (const auto& st : sticks) {
            const SDL_Scancode sc = g_key_stick[st.slot - RT_KB_LSTICK_UP];
            if (sc != SDL_SCANCODE_UNKNOWN && keys[sc]) {
                *st.axis = st.value;
                kbm_seen = true;
            }
        }
    }

    /* Mouse look. This is the first stick write outside the g_gamepad gate,
     * and it is outside it on purpose: the mouse is a device of its own and
     * must not need a pad plugged in to work.
     *
     * The delta is drained whether or not the setting is on, so nothing can
     * pile up behind a disabled mouse look and arrive as one jump when it
     * comes back. An empty accumulator is not a missing sample: it is the
     * correct report for "no motion arrived this field", which is what a
     * still mouse produces.
     *
     * The mouse drives a virtual stick rather than a speed, because ICO's
     * camera stick is a position and not a velocity (the trace is in
     * host/mouse_look.h): a drag deflects the stick and leaves it there, so
     * a field of stillness is a stick still being held, not a stick let go.
     * g_mouse_look.step() is therefore called once per field, motion or not,
     * and it reports a pair on every field the stick is off centre.
     *
     * Every field this function runs is one field of that stick's hold,
     * which is what sif/pad.cpp's rt_pad_run_due catch-up loop needs: after
     * a long frame it polls several fields in a row, each of them a field
     * the mouse really did spend still, and each advances the hold by one.
     *
     * Right-stick precedence is keyboard, then mouse, then gamepad: each of
     * the three writes only an axis still sitting at 0x80.
     *
     * Routing, the same split the buttons and the wheel take below: while
     * the pointer owns the mouse (rt_guest_menu_wants_mouse()) the field's
     * motion moves the drawn cursor on the game's own menu and the camera
     * stick sees none of it. The pointer keeps relative mode on for this
     * (host/mouse.h), so the delta arrives here exactly as it does in
     * gameplay; only its destination changes. The stick is centred on that
     * switch and on every one that ends capture (focus lost, the settings
     * menu, mouse look turned off), so a deflection cannot outlive the mouse
     * being the camera's. */
    float look_dx = 0.0f, look_dy = 0.0f;
    const bool look_moved = rt_mouse_take_look_delta(&look_dx, &look_dy);
    /* Motion counts wherever it is routed: moving the mouse on the game's
     * own menu is exactly the case the drawn cursor exists for. */
    if (look_moved && (look_dx != 0.0f || look_dy != 0.0f)) kbm_seen = true;
    if (rt_guest_menu_wants_mouse()) {
        g_mouse_look.reset();
        if (look_moved) rt_guest_menu_on_motion(look_dx, look_dy);
    } else if (!cfg.input.mouse_look || !rt_mouse_captured()) {
        g_mouse_look.reset();
    } else {
        const RtMouseLookOut look = g_mouse_look.step(
            look_moved ? look_dx : 0.0f, look_moved ? look_dy : 0.0f,
            cfg.input.mouse_look_sensitivity, cfg.input.mouse_look_invert_y);
        if (look.active) {
            if (s.rx == 0x80) s.rx = look.x;
            if (s.ry == 0x80) s.ry = look.y;
        }
    }

    /* Mouse buttons and wheel. Both are drained every field whichever way
     * they are routed, so nothing can accumulate behind the routing
     * decision and arrive late.
     *
     * Routing: while the pointer owns the mouse (rt_guest_menu_wants_mouse(),
     * guest/menu_nav.h, which is true while one of the game's own menus is
     * up) every transition and every tick goes
     * to the pointer and no gameplay mouse bind is pressed this field. The
     * two cannot both act on one click without the click doing two things.
     *
     * Otherwise the bound slots are pressed, gated on window focus: the
     * click that raised another window is not aimed at the game.
     * host/mouse.cpp clears its held mask when the window loses focus,
     * because a button released over another window sends no release event
     * here and the bit would otherwise stay set for the rest of the run. It
     * does not push a release into the transition log for that: the level is
     * forgotten, no event is invented. */
    RtMouseButtonEvent events[16];
    int nevents = rt_mouse_take_button_events(events, 16);
    const int wheel_ticks = rt_mouse_take_wheel_ticks();

    /* Same for the buttons and the wheel, and before the routing below
     * consumes them. A press and a release inside one field leaves nothing
     * held, so the transitions count as well as the held state. */
    if (nevents > 0 || wheel_ticks != 0) kbm_seen = true;
    for (int button = 1; button <= 5 && !kbm_seen; ++button) {
        if (rt_mouse_button_held(button)) kbm_seen = true;
    }

    if (rt_guest_menu_wants_mouse()) {
        while (nevents > 0) {
            for (int i = 0; i < nevents; ++i) {
                rt_guest_menu_on_button(events[i].button, events[i].down);
            }
            nevents = rt_mouse_take_button_events(events, 16);
        }
        if (wheel_ticks != 0) rt_guest_menu_on_wheel(wheel_ticks);
        /* Presses queued before the menu came up are not the menu's. */
        wheel_queue_reset();
    } else if (!rt_mouse_focused()) {
        wheel_queue_reset();
    } else {
        for (const MouseBind& b : g_mouse_binds) {
            const int button = sdl_button_of(b.input);
            if (button != 0 && rt_mouse_button_held(button)) s.buttons |= b.bit;
        }

        /* The wheel arrives as one signed total for the field, so a flick up
         * and back down inside a single field cancels: that is what the
         * accumulator in host/mouse.cpp saw and there is no finer answer to
         * give. Each remaining tick becomes one pressed field followed by
         * one released field, so two ticks are two presses the game can
         * count rather than one press held for two fields. */
        if (wheel_ticks > 0) wheel_enqueue(0, wheel_ticks);
        else if (wheel_ticks < 0) wheel_enqueue(1, -wheel_ticks);

        for (int dir = 0; dir < 2; ++dir) {
            const uint16_t bits = wheel_bits(dir == 0 ? RT_MOUSE_WHEEL_UP : RT_MOUSE_WHEEL_DOWN);
            if (bits == 0) {
                /* Unbound: the ticks have nowhere to go, so they are dropped
                 * rather than kept for a rebind that may never come. */
                g_wheel[dir] = WheelPulse{};
                continue;
            }
            if (g_wheel[dir].pressed) {
                g_wheel[dir].pressed = false;       /* the released field */
            } else if (g_wheel[dir].pending > 0) {
                --g_wheel[dir].pending;
                g_wheel[dir].pressed = true;
                s.buttons |= bits;
            }
        }
    }

    if (g_gamepad) {
        /* Raw axis value past which an axis bind (any axis, not only the
         * lefttrigger+ and righttrigger+ defaults on L2 and R2) counts as
         * pressed in the bound direction. This is the pre-settings build's
         * hardcoded `> 8192` on the two triggers. It is not a setting
         * because RtPadState carries no analog trigger channel: the point
         * only ever chooses where the digital bit flips. */
        constexpr float kAxisPressRaw = 8192.0f;
        for (const PadBind& b : g_pad_binds) {
            if (b.axis != SDL_GAMEPAD_AXIS_INVALID) {
                const float v = (float)SDL_GetGamepadAxis(g_gamepad, b.axis) * (float)b.dir;
                if (v > kAxisPressRaw) { s.buttons |= b.bit; pad_seen = true; }
            } else if (SDL_GetGamepadButton(g_gamepad, b.button)) {
                s.buttons |= b.bit;
                pad_seen = true;
            }
        }

        Sint16 lx = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        Sint16 ly = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        Sint16 rx = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        Sint16 ry = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTY);

        /* A stick a quarter of the way from centre is a thumb on it. Read
         * before the dead zone setting, because this is about the device
         * being used and not about what the game should be told, and a
         * quarter of full travel is far past any resting drift. */
        constexpr float kStickActiveRaw = 32767.0f * 0.25f;
        const float lmag = std::sqrt((float)lx * (float)lx + (float)ly * (float)ly);
        const float rmag = std::sqrt((float)rx * (float)rx + (float)ry * (float)ry);
        if (lmag > kStickActiveRaw || rmag > kStickActiveRaw) pad_seen = true;

        apply_deadzone(cfg.input.left_deadzone, &lx, &ly);
        apply_deadzone(cfg.input.right_deadzone, &rx, &ry);

        /* Left stick only. The camera stick goes through the same
         * gate-divided magnitude in the game, but the toggle is scoped to
         * movement by decision; the right stick stays as retail. */
        if (cfg.gameplay.run_any_direction) {
            if (!g_gate_expand_logged) {
                g_gate_expand_logged = true;
                rt_log("input", "gameplay.run_any_direction is on; the left stick is pre-scaled by"
                    " the game's octagonal-gate divisor so a full tilt runs in every direction");
            }
            rt_stick_gate_expand(&lx, &ly);
        } else {
            g_gate_expand_logged = false;
        }

        /* Keyboard sticks win only while deflected. */
        if (s.lx == 0x80) s.lx = axis_to_u8(lx);
        if (s.ly == 0x80) s.ly = axis_to_u8(ly);
        if (s.rx == 0x80) s.rx = axis_to_u8(rx);
        if (s.ry == 0x80) s.ry = axis_to_u8(ry);
    }

    /* The pointer's presses on the game's own menus (guest/menu_nav.h) ride
     * on top of whatever the devices produced this field, so the bits are
     * ORed in rather than replacing anything. Moving the selection is not
     * among them: hovering an item writes the game's own selection words
     * directly, and only a click (cross), a right click (triangle) and a
     * wheel tick (one D-pad step) arrive here as presses. It returns 0
     * whenever no menu is active, which is every field of gameplay. The
     * script provider never reaches here, so a scripted run's input stays
     * bit-identical. */
    s.buttons |= rt_guest_menu_pulse_bits(field);
    note_device(kbm_seen, pad_seen);
    g_state = s;
}

#endif /* ICORECOMP_PGS_SDL */

} // namespace

void rt_input_init() {
    if (g_inited) return;
    g_inited = true;
    const char* script = std::getenv("ICORECOMP_INPUT_SCRIPT");
    if (script && script[0] && parse_script(script)) {
        g_provider = Provider::Script;
        return;
    }
#ifdef ICORECOMP_PGS_SDL
    /* Deferred: SDL video may not be up yet at init time; the poll re-checks.
     * Marking the provider now keeps the selection log truthful. */
    g_provider = Provider::Sdl;
    rt_log("input", "SDL provider selected (activates when the GS window path brings SDL up; "
        "no window = no controller)");
#else
    g_provider = Provider::None;
    rt_log("input", "no input provider (no script, SDL not built): pads report no controller");
#endif
}

void rt_input_poll(uint64_t field) {
    if (!g_inited) rt_input_init();
    switch (g_provider) {
        case Provider::Script:
            script_poll(field);
            break;
#ifdef ICORECOMP_PGS_SDL
        case Provider::Sdl:
            if (rt_ui_wants_input()) {
                /* The menu owns the keyboard and the pad while it is up, so
                 * the game must not also see them. A default-constructed
                 * RtPadState is a real untouched-controller report (no
                 * buttons, both sticks centered at 0x80), not a fabricated
                 * one: it is exactly what sdl_poll would produce from a
                 * device nobody is touching. The guest keeps running.
                 *
                 * The script provider is deliberately not gated: a scripted
                 * run never brings the UI up (main.cpp skips rt_ui_init when
                 * ICORECOMP_INPUT_SCRIPT is set) and its input must stay
                 * bit-identical. */
                g_state = RtPadState{};
                /* Same reason, for the one device that accumulates instead
                 * of being sampled: without this the motion made while the
                 * menu is up would be waiting for the game the moment the
                 * menu closes. host/mouse.cpp also drops capture while the
                 * menu is up, so this only catches what was already in
                 * flight. */
                rt_mouse_discard_look_delta();
                /* And the virtual stick that motion belongs to
                 * (host/mouse_look.h). sdl_poll does not run while the menu
                 * is up, so nothing else would age it out: without this, the
                 * deflection the hand left behind when it reached for the
                 * menu would be reported into the game on the field the menu
                 * closes. */
                g_mouse_look.reset();
                /* The buttons and the wheel are the menu's for the same
                 * fields, and no pulse bits are applied either: the pad is
                 * neutral, which is the whole point of this branch. */
                mouse_events_drop();
            } else if (sdl_active()) {
                sdl_poll(field);
            }
            break;
#endif
        default:
            (void)field;
            break;
    }
}

RtInputDevice rt_input_last_device() { return g_last_device; }

bool rt_input_sdl_active() {
#ifdef ICORECOMP_PGS_SDL
    return g_inited && g_provider == Provider::Sdl && sdl_active();
#else
    return false;
#endif
}

bool rt_input_get(int port, RtPadState* out) {
    if (port != 0 || !g_inited) return false;
    if (g_provider == Provider::None) return false;
#ifdef ICORECOMP_PGS_SDL
    if (g_provider == Provider::Sdl && !sdl_active()) return false;
#endif
    *out = g_state;
    return true;
}

void rt_input_set_actuators(int port, uint8_t small_motor, uint8_t big_motor) {
    if (port != 0) return;
    if (small_motor != g_act_small || big_motor != g_act_big) {
        rt_log("input", "rumble: small=%u big=%u", small_motor, big_motor);
        g_act_small = small_motor;
        g_act_big = big_motor;
    }
#ifdef ICORECOMP_PGS_SDL
    if (g_provider == Provider::Sdl && g_gamepad) {
        /* Small motor is on/off, big is 0..255; hold until the next update
         * (the game refreshes every frame while rumbling). */
        SDL_RumbleGamepad(g_gamepad,
            (Uint16)(big_motor * 257u),
            small_motor ? 0xFFFFu : 0u,
            100 /* ms; refreshed by the per-frame SET_ACTDIRECT stream */);
    }
#endif
}
