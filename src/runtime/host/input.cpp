/* host/input.cpp: host input providers for the virtual DUALSHOCK 2.
 * Interface and provider selection are documented in input.h.
 */
#include "input.h"

#include "../runtime.h"
#include "../ui/ui.h"
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

std::vector<KeyBind> g_key_buttons;
/* Indexed by slot - RT_KB_LSTICK_UP; SDL_SCANCODE_UNKNOWN means unbound. */
SDL_Scancode g_key_stick[8] = {};
std::vector<PadBind> g_pad_binds;

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

    const char* def = rt_settings_default_binding(false, slot);
    rt_log("input", "input.keyboard.%s = \"%s\" is not an SDL scancode name; using the default \"%s\"",
        rt_settings_binding_key(false, slot), name.c_str(), def);
    sc = SDL_GetScancodeFromName(def);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        rt_log("input", "input.keyboard.%s: the compiled-in default \"%s\" is not an SDL scancode"
            " name either; this build's default table and SDL disagree, so that slot has no key"
            " this run", rt_settings_binding_key(false, slot), def);
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
            const char* def = rt_settings_default_binding(true, slot);
            rt_log("input", "input.gamepad.%s = \"%s\" is not an SDL gamepad button or axis name;"
                " using the default \"%s\"",
                rt_settings_binding_key(true, slot), cfg.input.gamepad[slot].c_str(), def);
            if (!resolve_pad_name(def, &b)) {
                rt_log("input", "input.gamepad.%s: the compiled-in default \"%s\" is not an SDL"
                    " gamepad button or axis name either; this build's default table and SDL"
                    " disagree, so that slot has no pad input this run",
                    rt_settings_binding_key(true, slot), def);
                continue;
            }
        }
        g_pad_binds.push_back(b);
    }
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

void sdl_poll() {
    sdl_probe();
    sync_tables();

    const RtSettings& cfg = rt_settings();
    RtPadState s;

    /* Keyboard (event pump runs in the WSI present path each field). */
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (keys) {
        for (const KeyBind& b : g_key_buttons) {
            if (keys[b.sc]) s.buttons |= b.bit;
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
            if (sc != SDL_SCANCODE_UNKNOWN && keys[sc]) *st.axis = st.value;
        }
    }

    if (g_gamepad) {
        /* Raw axis value past which an axis bind counts as pressed, in the
         * bound direction. This is the pre-settings build's hardcoded
         * `> 8192` on the two triggers. It is not a setting because an axis
         * bound to a pad slot is a button as far as the game is concerned
         * (the L2 and R2 defaults are lefttrigger+ and righttrigger+). */
        constexpr float kTriggerPressRaw = 8192.0f;
        for (const PadBind& b : g_pad_binds) {
            if (b.axis != SDL_GAMEPAD_AXIS_INVALID) {
                const float v = (float)SDL_GetGamepadAxis(g_gamepad, b.axis) * (float)b.dir;
                if (v > kTriggerPressRaw) s.buttons |= b.bit;
            } else if (SDL_GetGamepadButton(g_gamepad, b.button)) {
                s.buttons |= b.bit;
            }
        }

        Sint16 lx = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX);
        Sint16 ly = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY);
        Sint16 rx = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
        Sint16 ry = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
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
            } else if (sdl_active()) {
                sdl_poll();
            }
            break;
#endif
        default:
            (void)field;
            break;
    }
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
