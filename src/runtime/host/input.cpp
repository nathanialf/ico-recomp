/* host/input.cpp: host input providers for the virtual DUALSHOCK 2.
 * Interface and provider selection are documented in input.h.
 */
#include "input.h"

#include "../runtime.h"
#include "../ui/ui.h"

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

const struct { const char* name; uint16_t bit; } kButtonNames[] = {
    {"select", RT_PAD_SELECT}, {"l3", RT_PAD_L3}, {"r3", RT_PAD_R3},
    {"start", RT_PAD_START}, {"up", RT_PAD_UP}, {"right", RT_PAD_RIGHT},
    {"down", RT_PAD_DOWN}, {"left", RT_PAD_LEFT}, {"l2", RT_PAD_L2},
    {"r2", RT_PAD_R2}, {"l1", RT_PAD_L1}, {"r1", RT_PAD_R1},
    {"triangle", RT_PAD_TRIANGLE}, {"circle", RT_PAD_CIRCLE},
    {"cross", RT_PAD_CROSS}, {"square", RT_PAD_SQUARE},
};

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
            for (const auto& b : kButtonNames) {
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

void sdl_poll() {
    sdl_probe();
    RtPadState s;
    /* Keyboard (event pump runs in the WSI present path each field). */
    const bool* keys = SDL_GetKeyboardState(nullptr);
    if (keys) {
        struct { SDL_Scancode sc; uint16_t bit; } map[] = {
            {SDL_SCANCODE_UP, RT_PAD_UP}, {SDL_SCANCODE_DOWN, RT_PAD_DOWN},
            {SDL_SCANCODE_LEFT, RT_PAD_LEFT}, {SDL_SCANCODE_RIGHT, RT_PAD_RIGHT},
            {SDL_SCANCODE_X, RT_PAD_CROSS}, {SDL_SCANCODE_C, RT_PAD_CIRCLE},
            {SDL_SCANCODE_Z, RT_PAD_SQUARE}, {SDL_SCANCODE_V, RT_PAD_TRIANGLE},
            {SDL_SCANCODE_Q, RT_PAD_L1}, {SDL_SCANCODE_E, RT_PAD_R1},
            {SDL_SCANCODE_1, RT_PAD_L2}, {SDL_SCANCODE_3, RT_PAD_R2},
            {SDL_SCANCODE_T, RT_PAD_L3}, {SDL_SCANCODE_Y, RT_PAD_R3},
            {SDL_SCANCODE_RETURN, RT_PAD_START}, {SDL_SCANCODE_BACKSPACE, RT_PAD_SELECT},
        };
        for (const auto& m : map) {
            if (keys[m.sc]) s.buttons |= m.bit;
        }
        if (keys[SDL_SCANCODE_A]) s.lx = 0x00;
        if (keys[SDL_SCANCODE_D]) s.lx = 0xFF;
        if (keys[SDL_SCANCODE_W]) s.ly = 0x00;
        if (keys[SDL_SCANCODE_S]) s.ly = 0xFF;
        if (keys[SDL_SCANCODE_J]) s.rx = 0x00;
        if (keys[SDL_SCANCODE_L]) s.rx = 0xFF;
        if (keys[SDL_SCANCODE_I]) s.ry = 0x00;
        if (keys[SDL_SCANCODE_K]) s.ry = 0xFF;
    }
    if (g_gamepad) {
        struct { SDL_GamepadButton b; uint16_t bit; } map[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, RT_PAD_CROSS}, {SDL_GAMEPAD_BUTTON_EAST, RT_PAD_CIRCLE},
            {SDL_GAMEPAD_BUTTON_WEST, RT_PAD_SQUARE}, {SDL_GAMEPAD_BUTTON_NORTH, RT_PAD_TRIANGLE},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, RT_PAD_UP}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, RT_PAD_DOWN},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, RT_PAD_LEFT}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, RT_PAD_RIGHT},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, RT_PAD_L1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, RT_PAD_R1},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, RT_PAD_L3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, RT_PAD_R3},
            {SDL_GAMEPAD_BUTTON_START, RT_PAD_START}, {SDL_GAMEPAD_BUTTON_BACK, RT_PAD_SELECT},
        };
        for (const auto& m : map) {
            if (SDL_GetGamepadButton(g_gamepad, m.b)) s.buttons |= m.bit;
        }
        if (SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 8192) s.buttons |= RT_PAD_L2;
        if (SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192) s.buttons |= RT_PAD_R2;
        /* Keyboard sticks win only while deflected. */
        if (s.lx == 0x80) s.lx = axis_to_u8(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTX));
        if (s.ly == 0x80) s.ly = axis_to_u8(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFTY));
        if (s.rx == 0x80) s.rx = axis_to_u8(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTX));
        if (s.ry == 0x80) s.ry = axis_to_u8(SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
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
