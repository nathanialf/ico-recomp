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

#ifdef ICORECOMP_HAVE_SDL
#include <SDL3/SDL.h>
#endif

namespace {

enum class Provider { None, Script, Sdl };
Provider g_provider = Provider::None;
bool g_inited = false;

/* One virtual pad per port (host/input.h RT_PAD_PORTS). Port 0 is player 1
 * and is fed by the keyboard, the mouse and the first gamepad; port 1 is
 * player 2 and is fed by the second gamepad alone, which is the pad the PAL
 * disc's two-player mode reads for Yorda. Port 1 reports "no controller"
 * until a second gamepad is actually open (rt_input_get below), so the game
 * sees an empty port exactly as it would with nothing plugged into it.
 * The scripted provider drives port 0 only. */
RtPadState g_state[RT_PAD_PORTS];
uint8_t g_act_small[RT_PAD_PORTS] = {};
uint8_t g_act_big[RT_PAD_PORTS] = {};

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
        rt_log_warn("input", "ICORECOMP_INPUT_SCRIPT=%s: fopen failed", path);
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
    rt_log_info("input", "script provider: %zu steps from %s", g_script.size(), path);
    return !g_script.empty();
}

void script_poll(uint64_t field) {
    /* Port 0 only: the script format has one pad per line and a scripted run
     * must stay bit-identical to what it was before port 1 existed. Port 1
     * therefore stays a disconnected port for every scripted run. */
    RtPadState& st = g_state[0];
    bool changed = false;
    while (g_script_pos < g_script.size() && g_script[g_script_pos].field <= field) {
        const Step& s = g_script[g_script_pos];
        st.buttons = s.buttons;
        st.lx = s.lx; st.ly = s.ly; st.rx = s.rx; st.ry = s.ry;
        changed = true;
        ++g_script_pos;
    }
    if (changed) {
        rt_log_debug("input", "script step -> field=%llu buttons=0x%04x sticks=%u,%u,%u,%u",
            (unsigned long long)field, st.buttons, st.lx, st.ly, st.rx, st.ry);
    }
}

/* ---- SDL provider --------------------------------------------------------- */

#ifdef ICORECOMP_HAVE_SDL

/* One open SDL gamepad per pad port. [0] is player 1 and [1] is player 2;
 * a null is a port with no pad, which rt_input_get reports as "no
 * controller". The first pad SDL lists takes [0] and stays there: a pad
 * attached later fills the first free entry, and a pad removed from [0] is
 * replaced by whatever was in [1] (player 2's pad becomes player 1's rather
 * than leaving the game with nobody on port 0). */
SDL_Gamepad* g_gamepad[RT_PAD_PORTS] = {};
bool g_sdl_probed = false;

/* True while a pad is open on `port`. */
bool gamepad_open(int port) { return port >= 0 && port < RT_PAD_PORTS && g_gamepad[port]; }

/* The first port with no pad open, or -1 when both are taken. */
int first_free_pad_port() {
    for (int i = 0; i < RT_PAD_PORTS; ++i) {
        if (!g_gamepad[i]) return i;
    }
    return -1;
}

/* SDL video is initialized by the paraLLEl-GS window path on the main
 * thread; everything here runs on the same OS thread (the scheduler and its
 * coroutines never leave it). If video never comes up (headless), this
 * provider stays dormant. */
bool sdl_active() {
    return SDL_WasInit(SDL_INIT_VIDEO) != 0;
}

/* Opens `id` onto `port`, replacing whatever that entry already holds (the
 * caller has either checked that it is null or is deliberately replacing
 * it). Logs the name, or "(open failed)" the same way the pre-hot-plug probe
 * did; the port is named because which player a pad drives is the fact the
 * player needs from this line. */
void open_gamepad(int port, SDL_JoystickID id) {
    g_gamepad[port] = SDL_OpenGamepad(id);
    /* Two outcomes, two levels: which pad was opened is a device identity
     * fact, and a pad that would not open is a pad the player will find
     * dead. Same text either way, so the line reads as it always did. */
    if (g_gamepad[port]) {
        rt_log_info("input", "SDL gamepad on pad port %d (player %d): %s",
            port, port + 1, SDL_GetGamepadName(g_gamepad[port]));
    } else {
        rt_log_warn("input", "SDL gamepad on pad port %d (player %d): %s",
            port, port + 1, "(open failed)");
    }
}

/* SDL_INIT_GAMEPAD plus the first-attached open, once. Split out of the old
 * sdl_probe() so rt_input_sdl_gamepad_probe() (below) can call it from
 * rt_ui_init(), ahead of rt_input_init()/rt_pad_register_services(): the
 * launcher runs before the pad HLE registers, and it needs pad focus and
 * button events of its own. */
void gamepad_subsystem_init() {
    if (g_sdl_probed) return;
    g_sdl_probed = true;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        rt_log_warn("input", "SDL gamepad subsystem init failed: %s (keyboard only)", SDL_GetError());
        return;
    }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (ids && count > 0) {
        /* Up to one pad per port, in the order SDL lists them: the first is
         * player 1 and the second is player 2. A third and beyond are named
         * once and left closed; there are two pad ports and no multitap. */
        for (int i = 0; i < count; ++i) {
            const int port = first_free_pad_port();
            if (port < 0) {
                rt_log_info("input", "SDL: %d gamepads attached; the %d past the two pad ports"
                    " are not opened", count, count - RT_PAD_PORTS);
                break;
            }
            open_gamepad(port, ids[i]);
        }
    } else {
        rt_log_info("input", "SDL: no gamepad detected; keyboard map active (see host/input.h)");
    }
    SDL_free(ids);
}

uint8_t axis_to_u8(Sint16 v) {
    int x = ((int)v + 32768) >> 8;
    return (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x));
}

/* ---- tables built from settings ------------------------------------------
 *
 * The first sixteen RtKeyBind slots and the first sixteen RtPadBind slots are
 * the same sixteen DS2 buttons in the same order (host/settings.h), so one
 * bit table serves both, and so does the mouse's first sixteen. The
 * keyboard's eight stick slots and each device's two hotkey slots are handled
 * separately: the menu key is consumed by the UI pump (ui/ui_events.cpp) and
 * the screenshot key by host/screenshot.cpp, and neither is a pad binding at
 * all. Every loop that indexes kSlotBits therefore stops at sixteen; a loop
 * to a device's slot count would read past the end of it.
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
 * bound slots are in the table; an unbound one is absent. */
struct MouseBind {
    uint16_t bit;
    RtMouseInput input;
};

std::vector<KeyBind> g_key_buttons;
/* Indexed by slot - RT_KB_LSTICK_UP; SDL_SCANCODE_UNKNOWN means unbound. */
SDL_Scancode g_key_stick[8] = {};
/* One resolved bind table per pad port: [0] from input.gamepad, [1] from
 * input.gamepad2. Both hold at most the sixteen DS2 slots; the two hotkey
 * slots past them are never pad binds (see kSlotBits above), and player 2's
 * device has neither. */
std::vector<PadBind> g_pad_binds[RT_PAD_PORTS];
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
    rt_log_warn("input", "input.keyboard.%s = \"%s\" is not an SDL scancode name; using the default \"%s\"",
        rt_settings_binding_key(RT_BIND_KEYBOARD, slot), name.c_str(), def);
    sc = SDL_GetScancodeFromName(def);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        rt_log_warn("input", "input.keyboard.%s: the compiled-in default \"%s\" is not an SDL scancode"
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
    /* A chord ("back+start") is only meaningful for input.gamepad.menu,
     * which ui_events.cpp resolves on its own; this table serves only the
     * sixteen ordinary DS2 bits, so a chord-shaped name here can never be
     * valid input. settings.cpp's validate_binds rule 3 already reverts one
     * that reaches a pad-bit slot through the menu, so this is the
     * defensive twin for a file that was hand-edited (and so never
     * committed) or loaded before the very first commit runs. */
    std::string chord_a, chord_b;
    if (rt_settings_split_chord(name, &chord_a, &chord_b)) return false;

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

    /* The two pads resolve the same way from two different sections, so the
     * loop is one loop over the ports. RT_BIND_GAMEPAD2 shares the first
     * pad's default table (host/settings.cpp), so a slot that falls back
     * falls back to the same name on either port. */
    for (int port = 0; port < RT_PAD_PORTS; ++port) {
        const RtBindDevice device = port == 0 ? RT_BIND_GAMEPAD : RT_BIND_GAMEPAD2;
        const char* section = port == 0 ? "input.gamepad" : "input.gamepad2";
        const std::string* names = port == 0 ? cfg.input.gamepad : cfg.input.gamepad2;
        g_pad_binds[port].clear();
        g_pad_binds[port].reserve(16);
        for (int slot = 0; slot < 16; ++slot) {
            PadBind b;
            b.bit = kSlotBits[slot];
            if (!resolve_pad_name(names[slot], &b)) {
                const char* def = rt_settings_default_binding(device, slot);
                rt_log_warn("input", "%s.%s = \"%s\" is not an SDL gamepad button or axis name;"
                    " using the default \"%s\"",
                    section, rt_settings_binding_key(device, slot), names[slot].c_str(), def);
                if (!resolve_pad_name(def, &b)) {
                    rt_log_warn("input", "%s.%s: the compiled-in default \"%s\" is not an SDL"
                        " gamepad button or axis name either; this build's default table and SDL"
                        " disagree, so that slot has no pad input this run",
                        section, rt_settings_binding_key(device, slot), def);
                    continue;
                }
            }
            g_pad_binds[port].push_back(b);
        }
    }

    /* The mouse is the one device with no compiled-in default to fall back
     * to: most of its slots ship unbound ("" is a real value here, see
     * host/settings.h), so an empty name is a deliberate choice and says
     * nothing. A name that is not empty and does not resolve is a typo in
     * the file, and there is nothing sensible to substitute for it, so the
     * slot stays unbound and the line names it once per rebuild. */
    g_mouse_binds.clear();
    g_mouse_binds.reserve(16);
    /* Sixteen, not RT_MB_COUNT: the slots past the sixteenth are host hotkeys
     * (RT_MB_SCREENSHOT) with no pad bit at all, so kSlotBits has no entry
     * for them. Same bound as the keyboard and gamepad loops above, and for
     * the same reason. host/screenshot.cpp reads the screenshot slot. */
    for (int slot = 0; slot < 16; ++slot) {
        const std::string& name = cfg.input.mouse[slot];
        if (name.empty()) continue;
        RtMouseInput in = RT_MOUSE_LEFT;
        if (!rt_mouse_input_from_name(name, &in)) {
            rt_log_warn("input", "input.mouse.%s = \"%s\" is not a mouse input name (%s, %s, %s, %s,"
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
            rt_log_warn("input", "mouse wheel press queue is full at %d pending presses; ticks past"
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
    rt_log_info("input", "last device is now %s",
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

/* Samples the gamepad open on `port` into `st`, and answers whether the
 * player was doing anything with it this field (a held bound button, an axis
 * bind past its press point, or a stick a quarter of the way from centre).
 *
 * Every axis is written only where `st` is still centred at 0x80, so on port
 * 0 the keyboard and mouse look, which ran first, keep the precedence
 * host/input.h documents. On port 1 nothing ran first, so the pad fills all
 * four.
 *
 * Both ports read the same input.left_deadzone, input.right_deadzone and
 * gameplay.run_any_direction: those describe how a stick is reported, not
 * which player holds it, and a second player on the same settings gets the
 * same feel as the first. False, touching nothing, when that port has no pad
 * open. */
bool sample_gamepad(int port, RtPadState* st) {
    SDL_Gamepad* pad = gamepad_open(port) ? g_gamepad[port] : nullptr;
    if (!pad) return false;
    const RtSettings& cfg = rt_settings();
    bool seen = false;

    /* Raw axis value past which an axis bind (any axis, not only the
     * lefttrigger+ and righttrigger+ defaults on L2 and R2) counts as
     * pressed in the bound direction. This is the pre-settings build's
     * hardcoded `> 8192` on the two triggers. It is not a setting
     * because RtPadState carries no analog trigger channel: the point
     * only ever chooses where the digital bit flips. */
    constexpr float kAxisPressRaw = 8192.0f;
    for (const PadBind& b : g_pad_binds[port]) {
        if (b.axis != SDL_GAMEPAD_AXIS_INVALID) {
            const float v = (float)SDL_GetGamepadAxis(pad, b.axis) * (float)b.dir;
            if (v > kAxisPressRaw) { st->buttons |= b.bit; seen = true; }
        } else if (SDL_GetGamepadButton(pad, b.button)) {
            st->buttons |= b.bit;
            seen = true;
        }
    }

    Sint16 lx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    Sint16 ly = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    Sint16 rx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
    Sint16 ry = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);

    /* A stick a quarter of the way from centre is a thumb on it. Read
     * before the dead zone setting, because this is about the device
     * being used and not about what the game should be told, and a
     * quarter of full travel is far past any resting drift. */
    constexpr float kStickActiveRaw = 32767.0f * 0.25f;
    const float lmag = std::sqrt((float)lx * (float)lx + (float)ly * (float)ly);
    const float rmag = std::sqrt((float)rx * (float)rx + (float)ry * (float)ry);
    if (lmag > kStickActiveRaw || rmag > kStickActiveRaw) seen = true;

    apply_deadzone(cfg.input.left_deadzone, &lx, &ly);
    apply_deadzone(cfg.input.right_deadzone, &rx, &ry);

    /* Left stick only. The camera stick goes through the same
     * gate-divided magnitude in the game, but the toggle is scoped to
     * movement by decision; the right stick stays as retail. */
    if (cfg.gameplay.run_any_direction) {
        if (!g_gate_expand_logged) {
            g_gate_expand_logged = true;
            rt_log_info("input", "gameplay.run_any_direction is on; the left stick is pre-scaled by"
                " the game's octagonal-gate divisor so a full tilt runs in every direction");
        }
        rt_stick_gate_expand(&lx, &ly);
    } else {
        g_gate_expand_logged = false;
    }

    /* Keyboard sticks win only while deflected. */
    if (st->lx == 0x80) st->lx = axis_to_u8(lx);
    if (st->ly == 0x80) st->ly = axis_to_u8(ly);
    if (st->rx == 0x80) st->rx = axis_to_u8(rx);
    if (st->ry == 0x80) st->ry = axis_to_u8(ry);
    return seen;
}

void sdl_poll(uint64_t field) {
    gamepad_subsystem_init();
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

    /* Mouse look. This is the first stick write outside the gamepad sampling
     * below, and it is outside it on purpose: the mouse is a device of its
     * own and must not need a pad plugged in to work.
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

    if (sample_gamepad(0, &s)) pad_seen = true;

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
    g_state[0] = s;

    /* Player 2's port. Nothing but the second gamepad writes it: the
     * keyboard, the mouse, mouse look, the wheel queue and the pointer's
     * menu pulses are all player 1's, so port 1 starts from a fresh
     * untouched-controller report and the pad fills it. When no second pad
     * is open this leaves a centred, button-free state that rt_input_get
     * never hands out, because it answers false for a port with no pad. */
    RtPadState p2;
    const bool pad2_seen = sample_gamepad(1, &p2);
    g_state[1] = p2;

    note_device(kbm_seen, pad_seen || pad2_seen);
}

#endif /* ICORECOMP_HAVE_SDL */

} // namespace

void rt_input_init() {
    if (g_inited) return;
    g_inited = true;
    const char* script = std::getenv("ICORECOMP_INPUT_SCRIPT");
    if (script && script[0] && parse_script(script)) {
        g_provider = Provider::Script;
        return;
    }
#ifdef ICORECOMP_HAVE_SDL
    /* Deferred: SDL video may not be up yet at init time; the poll re-checks.
     * Marking the provider now keeps the selection log truthful. */
    g_provider = Provider::Sdl;
    rt_log_info("input", "SDL provider selected (activates when the GS window path brings SDL up; "
        "no window = no controller)");
#else
    g_provider = Provider::None;
    rt_log_info("input", "no input provider (no script, SDL not built): pads report no controller");
#endif
}

void rt_input_poll(uint64_t field) {
    if (!g_inited) rt_input_init();
    switch (g_provider) {
        case Provider::Script:
            script_poll(field);
            break;
#ifdef ICORECOMP_HAVE_SDL
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
                for (RtPadState& st : g_state) st = RtPadState{};
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

void rt_input_sdl_gamepad_probe() {
#ifdef ICORECOMP_HAVE_SDL
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
    gamepad_subsystem_init();
#endif
}

#ifdef ICORECOMP_HAVE_SDL
void rt_input_on_sdl_event(const SDL_Event& e) {
    /* A scripted run has no devices to watch (see host/input.h) and must
     * stay bit-identical; it also never brings the UI up, so this would
     * never fire in practice, but the guard is cheap and makes the
     * contract explicit rather than accidental. */
    if (g_provider == Provider::Script) return;

    switch (e.type) {
    case SDL_EVENT_GAMEPAD_ADDED: {
        const SDL_JoystickID id = e.gdevice.which;
        /* SDL3 also reports every pad already attached at init as ADDED;
         * SDL_GetGamepadFromID answering non-null is what tells that apart
         * from a fresh attach, so this does not reopen one already open. */
        if (SDL_GetGamepadFromID(id)) return;
        const int port = first_free_pad_port();
        if (port < 0) {
            /* Both pad ports are taken. The pad is named and closed again
             * rather than displacing a player mid-run; there is no multitap
             * on either disc, so there is no third port to put it on. */
            SDL_Gamepad* candidate = SDL_OpenGamepad(id);
            const char* name = candidate ? SDL_GetGamepadName(candidate) : nullptr;
            std::string taken;
            for (int p = 0; p < RT_PAD_PORTS; ++p) {
                if (!taken.empty()) taken += ", ";
                const char* held = g_gamepad[p] ? SDL_GetGamepadName(g_gamepad[p]) : nullptr;
                taken += held ? held : "(unnamed)";
            }
            rt_log_warn("input", "SDL gamepad attached: %s; all %d pad ports are taken (%s),"
                " so it is not opened",
                name ? name : "(open failed)", RT_PAD_PORTS, taken.c_str());
            if (candidate) SDL_CloseGamepad(candidate);
            return;
        }
        open_gamepad(port, id);
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        int port = -1;
        for (int i = 0; i < RT_PAD_PORTS; ++i) {
            if (g_gamepad[i] && SDL_GetGamepadID(g_gamepad[i]) == e.gdevice.which) port = i;
        }
        if (port < 0) return;
        rt_log_info("input", "SDL gamepad removed from pad port %d (player %d): %s",
            port, port + 1, SDL_GetGamepadName(g_gamepad[port]));
        SDL_CloseGamepad(g_gamepad[port]);
        g_gamepad[port] = nullptr;
        /* Close the gap: a pad on a higher port moves down, so unplugging
         * player 1's pad leaves player 2's driving player 1 rather than
         * leaving the game with nobody on port 0. The pad the game sees on
         * the vacated port disconnects on the next field, which is what
         * hardware does. */
        for (int i = port + 1; i < RT_PAD_PORTS; ++i) {
            if (!g_gamepad[i]) continue;
            rt_log_info("input", "SDL gamepad %s moves from pad port %d to pad port %d (player %d)",
                SDL_GetGamepadName(g_gamepad[i]), i, i - 1, i);
            g_gamepad[i - 1] = g_gamepad[i];
            g_gamepad[i] = nullptr;
        }
        /* A pad that was attached while both ports were full was never
         * opened (the ADDED case above), so a freed port is filled from
         * whatever SDL still lists and is not already open. */
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; ++i) {
            if (SDL_GetGamepadFromID(ids[i])) continue;
            const int free_port = first_free_pad_port();
            if (free_port < 0) break;
            open_gamepad(free_port, ids[i]);
        }
        SDL_free(ids);
        break;
    }
    default:
        break;
    }
}
#endif

bool rt_input_sdl_active() {
#ifdef ICORECOMP_HAVE_SDL
    return g_inited && g_provider == Provider::Sdl && sdl_active();
#else
    return false;
#endif
}

bool rt_input_get(int port, RtPadState* out) {
    if (port < 0 || port >= RT_PAD_PORTS || !g_inited) return false;
    if (g_provider == Provider::None) return false;
#ifdef ICORECOMP_HAVE_SDL
    if (g_provider == Provider::Sdl && !sdl_active()) return false;
    /* Port 1 exists only while a second gamepad is open. Reporting a
     * centred pad there instead would tell the game a controller is plugged
     * in when none is, and the PAL disc's two-player mode reads port 1 to
     * decide whether a second player is present; sif/pad.cpp turns this
     * false into the disconnected frame real hardware sends. Port 0 keeps
     * answering true with no pad attached, because the keyboard and the
     * mouse are player 1's controller. */
    if (g_provider == Provider::Sdl && port > 0 && !gamepad_open(port)) return false;
#else
    /* No SDL: there is no device that could be on port 1. */
    if (port > 0) return false;
#endif
    /* The scripted provider drives port 0 only (script_poll), so port 1 is
     * a disconnected port for every scripted run. */
    if (g_provider == Provider::Script && port > 0) return false;
    *out = g_state[port];
    return true;
}

void rt_input_set_actuators(int port, uint8_t small_motor, uint8_t big_motor) {
    if (port < 0 || port >= RT_PAD_PORTS) return;
    if (small_motor != g_act_small[port] || big_motor != g_act_big[port]) {
        /* Changes only: the game re-sends the same motor state every field
         * while a rumble is active, and an unchanged state says nothing.
         * A count-based cap used to sit on top of this, from when the
         * verbose channel was the only gate. rt_log_debug now carries its
         * own gate (level debug, or "input" named in the ICORECOMP_VERBOSE
         * channel spec that log.cpp parses in rt_log_init; debug.verbose was
         * retired), which subsumes it: the cap could not admit a line the
         * inner gate would refuse, so it was dead and only suppressed the
         * first few. */
        rt_log_debug("input", "rumble: port %d small=%u big=%u", port, small_motor, big_motor);
        g_act_small[port] = small_motor;
        g_act_big[port] = big_motor;
    }
#ifdef ICORECOMP_HAVE_SDL
    if (g_provider == Provider::Sdl && gamepad_open(port)) {
        /* Each port rumbles its own pad: a jolt the game sends to port 1 is
         * player 2's, and a port with no pad open has nothing to rumble. */
        /* Small motor is on/off, big is 0..255; hold until the next update
         * (the game refreshes every frame while rumbling). */
        SDL_RumbleGamepad(g_gamepad[port],
            (Uint16)(big_motor * 257u),
            small_motor ? 0xFFFFu : 0u,
            100 /* ms; refreshed by the per-frame SET_ACTDIRECT stream */);
    }
#endif
}
