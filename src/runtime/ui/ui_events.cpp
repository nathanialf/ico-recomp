/* ui/ui_events.cpp: SDL3 events translated into RmlUi input, plus the menu
 * hotkey.
 *
 * Reentrancy: rt_ui_handle_sdl_event runs from rt_window_pump
 * (host/window.cpp), which can execute from inside Granite's
 * WSI::begin_frame. Nothing here may call an rt_pgs_* function. It only
 * translates events into Rml::Context::Process* calls, flips the visibility
 * flag and makes plain SDL calls (SDL_CaptureMouse, SDL_StartTextInput);
 * the surface size it needs for coordinate scaling is the one rt_ui_tick
 * read at the last field boundary.
 *
 * The key/mouse/modifier mapping below is ported from RmlUi 6.3's
 * Backends/RmlUi_Platform_SDL.cpp (RmlSDL::InputEventHandler, ConvertKey,
 * ConvertMouseButton, GetKeyModifierState), keeping only its SDL3 branches.
 * RmlUi is MIT: Copyright (c) 2008-2014 CodePoint Ltd, Shift Technology Ltd,
 * and contributors; Copyright (c) 2019-2026 The RmlUi Team, and
 * contributors. That file is not included directly because it also defines
 * SystemInterface_SDL and its SDL2 branches, neither of which this build
 * wants.
 */
#include "ui.h"

#if defined(ICORECOMP_UI) && defined(ICORECOMP_PGS_SDL)

#include "ui_internal.h"

#include "../host/settings.h"
#include "../runtime.h"

#include <RmlUi/Core/Input.h>

#include <SDL3/SDL.h>

#include <string>

namespace rtui {

namespace {

/* Resolved once at init from settings; the compiled-in values here are the
 * same defaults settings.cpp ships ("F1" and "guide") so an unresolvable
 * name can fall back without inventing a different binding. */
SDL_Scancode g_menu_scancode = SDL_SCANCODE_F1;
SDL_GamepadButton g_menu_button = SDL_GAMEPAD_BUTTON_GUIDE;

Rml::Input::KeyIdentifier convert_key(SDL_Keycode key) {
    // clang-format off
    switch (key) {
    case SDLK_UNKNOWN:      return Rml::Input::KI_UNKNOWN;
    case SDLK_ESCAPE:       return Rml::Input::KI_ESCAPE;
    case SDLK_SPACE:        return Rml::Input::KI_SPACE;
    case SDLK_0:            return Rml::Input::KI_0;
    case SDLK_1:            return Rml::Input::KI_1;
    case SDLK_2:            return Rml::Input::KI_2;
    case SDLK_3:            return Rml::Input::KI_3;
    case SDLK_4:            return Rml::Input::KI_4;
    case SDLK_5:            return Rml::Input::KI_5;
    case SDLK_6:            return Rml::Input::KI_6;
    case SDLK_7:            return Rml::Input::KI_7;
    case SDLK_8:            return Rml::Input::KI_8;
    case SDLK_9:            return Rml::Input::KI_9;
    case SDLK_A:            return Rml::Input::KI_A;
    case SDLK_B:            return Rml::Input::KI_B;
    case SDLK_C:            return Rml::Input::KI_C;
    case SDLK_D:            return Rml::Input::KI_D;
    case SDLK_E:            return Rml::Input::KI_E;
    case SDLK_F:            return Rml::Input::KI_F;
    case SDLK_G:            return Rml::Input::KI_G;
    case SDLK_H:            return Rml::Input::KI_H;
    case SDLK_I:            return Rml::Input::KI_I;
    case SDLK_J:            return Rml::Input::KI_J;
    case SDLK_K:            return Rml::Input::KI_K;
    case SDLK_L:            return Rml::Input::KI_L;
    case SDLK_M:            return Rml::Input::KI_M;
    case SDLK_N:            return Rml::Input::KI_N;
    case SDLK_O:            return Rml::Input::KI_O;
    case SDLK_P:            return Rml::Input::KI_P;
    case SDLK_Q:            return Rml::Input::KI_Q;
    case SDLK_R:            return Rml::Input::KI_R;
    case SDLK_S:            return Rml::Input::KI_S;
    case SDLK_T:            return Rml::Input::KI_T;
    case SDLK_U:            return Rml::Input::KI_U;
    case SDLK_V:            return Rml::Input::KI_V;
    case SDLK_W:            return Rml::Input::KI_W;
    case SDLK_X:            return Rml::Input::KI_X;
    case SDLK_Y:            return Rml::Input::KI_Y;
    case SDLK_Z:            return Rml::Input::KI_Z;
    case SDLK_SEMICOLON:    return Rml::Input::KI_OEM_1;
    case SDLK_PLUS:         return Rml::Input::KI_OEM_PLUS;
    case SDLK_COMMA:        return Rml::Input::KI_OEM_COMMA;
    case SDLK_MINUS:        return Rml::Input::KI_OEM_MINUS;
    case SDLK_PERIOD:       return Rml::Input::KI_OEM_PERIOD;
    case SDLK_SLASH:        return Rml::Input::KI_OEM_2;
    case SDLK_GRAVE:        return Rml::Input::KI_OEM_3;
    case SDLK_LEFTBRACKET:  return Rml::Input::KI_OEM_4;
    case SDLK_BACKSLASH:    return Rml::Input::KI_OEM_5;
    case SDLK_RIGHTBRACKET: return Rml::Input::KI_OEM_6;
    case SDLK_DBLAPOSTROPHE: return Rml::Input::KI_OEM_7;
    case SDLK_KP_0:         return Rml::Input::KI_NUMPAD0;
    case SDLK_KP_1:         return Rml::Input::KI_NUMPAD1;
    case SDLK_KP_2:         return Rml::Input::KI_NUMPAD2;
    case SDLK_KP_3:         return Rml::Input::KI_NUMPAD3;
    case SDLK_KP_4:         return Rml::Input::KI_NUMPAD4;
    case SDLK_KP_5:         return Rml::Input::KI_NUMPAD5;
    case SDLK_KP_6:         return Rml::Input::KI_NUMPAD6;
    case SDLK_KP_7:         return Rml::Input::KI_NUMPAD7;
    case SDLK_KP_8:         return Rml::Input::KI_NUMPAD8;
    case SDLK_KP_9:         return Rml::Input::KI_NUMPAD9;
    case SDLK_KP_ENTER:     return Rml::Input::KI_NUMPADENTER;
    case SDLK_KP_MULTIPLY:  return Rml::Input::KI_MULTIPLY;
    case SDLK_KP_PLUS:      return Rml::Input::KI_ADD;
    case SDLK_KP_MINUS:     return Rml::Input::KI_SUBTRACT;
    case SDLK_KP_PERIOD:    return Rml::Input::KI_DECIMAL;
    case SDLK_KP_DIVIDE:    return Rml::Input::KI_DIVIDE;
    case SDLK_KP_EQUALS:    return Rml::Input::KI_OEM_NEC_EQUAL;
    case SDLK_BACKSPACE:    return Rml::Input::KI_BACK;
    case SDLK_TAB:          return Rml::Input::KI_TAB;
    case SDLK_CLEAR:        return Rml::Input::KI_CLEAR;
    case SDLK_RETURN:       return Rml::Input::KI_RETURN;
    case SDLK_PAUSE:        return Rml::Input::KI_PAUSE;
    case SDLK_CAPSLOCK:     return Rml::Input::KI_CAPITAL;
    case SDLK_PAGEUP:       return Rml::Input::KI_PRIOR;
    case SDLK_PAGEDOWN:     return Rml::Input::KI_NEXT;
    case SDLK_END:          return Rml::Input::KI_END;
    case SDLK_HOME:         return Rml::Input::KI_HOME;
    case SDLK_LEFT:         return Rml::Input::KI_LEFT;
    case SDLK_UP:           return Rml::Input::KI_UP;
    case SDLK_RIGHT:        return Rml::Input::KI_RIGHT;
    case SDLK_DOWN:         return Rml::Input::KI_DOWN;
    case SDLK_INSERT:       return Rml::Input::KI_INSERT;
    case SDLK_DELETE:       return Rml::Input::KI_DELETE;
    case SDLK_HELP:         return Rml::Input::KI_HELP;
    case SDLK_F1:           return Rml::Input::KI_F1;
    case SDLK_F2:           return Rml::Input::KI_F2;
    case SDLK_F3:           return Rml::Input::KI_F3;
    case SDLK_F4:           return Rml::Input::KI_F4;
    case SDLK_F5:           return Rml::Input::KI_F5;
    case SDLK_F6:           return Rml::Input::KI_F6;
    case SDLK_F7:           return Rml::Input::KI_F7;
    case SDLK_F8:           return Rml::Input::KI_F8;
    case SDLK_F9:           return Rml::Input::KI_F9;
    case SDLK_F10:          return Rml::Input::KI_F10;
    case SDLK_F11:          return Rml::Input::KI_F11;
    case SDLK_F12:          return Rml::Input::KI_F12;
    case SDLK_F13:          return Rml::Input::KI_F13;
    case SDLK_F14:          return Rml::Input::KI_F14;
    case SDLK_F15:          return Rml::Input::KI_F15;
    case SDLK_NUMLOCKCLEAR: return Rml::Input::KI_NUMLOCK;
    case SDLK_SCROLLLOCK:   return Rml::Input::KI_SCROLL;
    case SDLK_LSHIFT:       return Rml::Input::KI_LSHIFT;
    case SDLK_RSHIFT:       return Rml::Input::KI_RSHIFT;
    case SDLK_LCTRL:        return Rml::Input::KI_LCONTROL;
    case SDLK_RCTRL:        return Rml::Input::KI_RCONTROL;
    case SDLK_LALT:         return Rml::Input::KI_LMENU;
    case SDLK_RALT:         return Rml::Input::KI_RMENU;
    case SDLK_LGUI:         return Rml::Input::KI_LMETA;
    case SDLK_RGUI:         return Rml::Input::KI_RMETA;
    default: break;
    }
    // clang-format on
    return Rml::Input::KI_UNKNOWN;
}

int convert_mouse_button(int button) {
    switch (button) {
    case SDL_BUTTON_LEFT:   return 0;
    case SDL_BUTTON_RIGHT:  return 1;
    case SDL_BUTTON_MIDDLE: return 2;
    default:                return 3;
    }
}

int key_modifier_state() {
    const SDL_Keymod mods = SDL_GetModState();
    int state = 0;
    if (mods & SDL_KMOD_CTRL)  state |= Rml::Input::KM_CTRL;
    if (mods & SDL_KMOD_SHIFT) state |= Rml::Input::KM_SHIFT;
    if (mods & SDL_KMOD_ALT)   state |= Rml::Input::KM_ALT;
    if (mods & SDL_KMOD_NUM)   state |= Rml::Input::KM_NUMLOCK;
    if (mods & SDL_KMOD_CAPS)  state |= Rml::Input::KM_CAPSLOCK;
    return state;
}

/* SDL reports mouse positions in window (logical) coordinates; the RmlUi
 * context is sized in surface pixels (rt_pgs_surface_size), which differ on
 * a high-DPI display. Scale with the surface size the last tick read, never
 * by querying the backend from here. */
void window_to_surface(float x, float y, int* out_x, int* out_y) {
    float scale_x = 1.0f, scale_y = 1.0f;
    SDL_Window* win = (SDL_Window*)backend_window_handle();
    int win_w = 0, win_h = 0;
    if (win && SDL_GetWindowSize(win, &win_w, &win_h) && win_w > 0 && win_h > 0 &&
        g_ui.surface_width != 0 && g_ui.surface_height != 0) {
        scale_x = float(g_ui.surface_width) / float(win_w);
        scale_y = float(g_ui.surface_height) / float(win_h);
    }
    *out_x = int(x * scale_x);
    *out_y = int(y * scale_y);
}

bool toggle_menu() {
    /* The launcher owns the window while it is up: it has its own Settings
     * button, and the menu shows on top of it. The hotkey is still consumed
     * here so it reaches neither RmlUi nor the pad, it just does nothing. */
    if (g_ui.launcher_visible) return true;
    rt_ui_set_visible(!rt_ui_visible());
    return true;
}

/* Escape (and its gamepad twin) closes the menu, except while a text field
 * has the focus: there Escape belongs to the field, which uses it to revert
 * the edit. Anything else focused, or nothing focused, closes. */
bool text_input_focused() {
    Rml::Element* focus = g_ui.context->GetFocusElement();
    if (!focus || focus->GetTagName() != "input") return false;
    const Rml::String type = focus->GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "password";
}

bool close_menu() {
    /* Nothing to close: the launcher is up on its own. Escape does nothing
     * there on purpose, so a stray press cannot quit out of it. Returning
     * false lets the key reach RmlUi, where it is only a focus-level key. */
    if (!g_ui.visible) return false;
    if (text_input_focused()) return false;
    rt_ui_set_visible(false);
    return true;
}

/* Gamepad navigation while the menu is up. RmlUi drives focus from the
 * arrow keys and Enter (ElementDocument::ProcessDefaultAction), so the
 * translation is a d-pad-to-arrow-keys mapping plus south for Enter; the
 * documents carry `nav: auto` and `tab-index: auto` on every control, which
 * is what makes the arrow keys move the focus at all. KI_UNKNOWN means
 * "this button has no menu meaning", and the event falls through unconsumed.
 */
Rml::Input::KeyIdentifier convert_gamepad_button(SDL_GamepadButton button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:    return Rml::Input::KI_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  return Rml::Input::KI_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  return Rml::Input::KI_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Rml::Input::KI_RIGHT;
    case SDL_GAMEPAD_BUTTON_SOUTH:      return Rml::Input::KI_RETURN;
    default:                            return Rml::Input::KI_UNKNOWN;
    }
}

} // namespace

void menu_set_text_input(bool enabled) {
    SDL_Window* win = (SDL_Window*)backend_window_handle();
    if (!win) return;
    if (enabled) {
        SDL_StartTextInput(win);
    } else {
        SDL_StopTextInput(win);
    }
}

void resolve_menu_hotkey() {
    const std::string& kb = rt_settings().input.keyboard[RT_KB_MENU];
    SDL_Scancode scancode = kb.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(kb.c_str());
    if (scancode == SDL_SCANCODE_UNKNOWN) {
        rt_log("ui", "input.keyboard.menu \"%s\" is not an SDL scancode name; keeping the default %s",
            kb.c_str(), SDL_GetScancodeName(g_menu_scancode));
    } else {
        g_menu_scancode = scancode;
    }

    const std::string& gp = rt_settings().input.gamepad[RT_GP_MENU];
    SDL_GamepadButton button =
        gp.empty() ? SDL_GAMEPAD_BUTTON_INVALID : SDL_GetGamepadButtonFromString(gp.c_str());
    if (button == SDL_GAMEPAD_BUTTON_INVALID) {
        rt_log("ui", "input.gamepad.menu \"%s\" is not an SDL gamepad button name; keeping the default %s",
            gp.c_str(), SDL_GetGamepadStringForButton(g_menu_button));
    } else {
        g_menu_button = button;
    }

    rt_log("ui", "menu hotkey: keyboard %s, gamepad %s",
        SDL_GetScancodeName(g_menu_scancode), SDL_GetGamepadStringForButton(g_menu_button));
}

const char* menu_hotkey_name() {
    const char* name = SDL_GetScancodeName(g_menu_scancode);
    return name ? name : "";
}

} // namespace rtui

bool rt_ui_handle_sdl_event(const SDL_Event& e) {
    using namespace rtui;

    if (!g_ui.initialized) return false;

    /* A capture in progress gets first refusal on everything, ahead of the
     * hotkey and RmlUi: the whole point is that the next key or button the
     * user presses becomes the binding rather than doing what it normally
     * does. Which events it takes follows the device being bound
     * (ui_rebind.cpp): a keyboard or gamepad capture consumes keyboard and
     * gamepad events only, so the mouse still drives the menu underneath,
     * while a mouse capture also consumes motion, buttons and the wheel so
     * that the press being bound does not click whatever is under the
     * pointer. rebind_active() also covers the moment after a mouse capture
     * has ended while the release of the press it accepted is still owed. */
    if (rebind_active() && rebind_handle_sdl_event(e)) return true;

    /* The hotkey is looked at whether the menu is up or not, and is consumed
     * here: it never reaches RmlUi and never reaches the pad (host/input.cpp
     * polls key state, and the commit-time binding rules in settings.cpp keep
     * the menu key out of a pad slot). Gamepad button events only arrive for
     * gamepads somebody opened; host/input.cpp does that, this file never
     * does. */
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (!e.key.repeat && e.key.scancode == g_menu_scancode) return toggle_menu();
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (SDL_GamepadButton(e.gbutton.button) == g_menu_button) return toggle_menu();
        break;
    default:
        break;
    }

    /* While nothing is up the UI looks at nothing else: the game owns the
     * input. The launcher counts as up, and while it is, the events below
     * drive it exactly as they drive the menu. */
    if (!g_ui.visible && !g_ui.launcher_visible) return false;

    Rml::Context* context = g_ui.context;
    switch (e.type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const SDL_GamepadButton button = SDL_GamepadButton(e.gbutton.button);
        if (button == SDL_GAMEPAD_BUTTON_EAST) return close_menu();
        const Rml::Input::KeyIdentifier key = convert_gamepad_button(button);
        if (key == Rml::Input::KI_UNKNOWN) return false;
        /* Every Rml::Context::Process* below is negated. RmlUi's convention
         * is inverted from this function's: Process* returns true when the
         * event was NOT consumed (Include/RmlUi/Core/Context.h, the @return
         * on each of them), while rt_ui_handle_sdl_event returns true to
         * mean the UI took the event and the game must not see it. Passing
         * the return value through unchanged handed the game exactly the
         * keys and clicks the menu had just acted on. */
        bool consumed = !context->ProcessKeyDown(key, 0);
        if (key == Rml::Input::KI_RETURN) {
            /* The keyboard path pairs Enter with a newline text input; the
             * text widget needs both to commit a field, so the call is made
             * unconditionally rather than short-circuited away. Either half
             * being consumed means the UI took the press. */
            const bool text_consumed = !context->ProcessTextInput('\n');
            consumed = consumed || text_consumed;
        }
        return consumed;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const Rml::Input::KeyIdentifier key = convert_gamepad_button(SDL_GamepadButton(e.gbutton.button));
        if (key == Rml::Input::KI_UNKNOWN) return false;
        return !context->ProcessKeyUp(key, 0);
    }
    case SDL_EVENT_MOUSE_MOTION: {
        int x = 0, y = 0;
        window_to_surface(e.motion.x, e.motion.y, &x, &y);
        return !context->ProcessMouseMove(x, y, key_modifier_state());
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const bool consumed = !context->ProcessMouseButtonDown(convert_mouse_button(e.button.button),
                                                               key_modifier_state());
        SDL_CaptureMouse(true);
        return consumed;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        SDL_CaptureMouse(false);
        return !context->ProcessMouseButtonUp(convert_mouse_button(e.button.button), key_modifier_state());
    }
    case SDL_EVENT_MOUSE_WHEEL:
        return !context->ProcessMouseWheel(float(-e.wheel.y), key_modifier_state());
    case SDL_EVENT_KEY_DOWN: {
        if (e.key.key == SDLK_ESCAPE && close_menu()) return true;
        bool consumed = !context->ProcessKeyDown(convert_key(e.key.key), key_modifier_state());
        if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) {
            /* Unconditional, not short-circuited: the text widget needs the
             * newline to commit the field whether or not the key down was
             * already consumed. */
            const bool text_consumed = !context->ProcessTextInput('\n');
            consumed = consumed || text_consumed;
        }
        return consumed;
    }
    case SDL_EVENT_KEY_UP:
        return !context->ProcessKeyUp(convert_key(e.key.key), key_modifier_state());
    case SDL_EVENT_TEXT_INPUT:
        return !context->ProcessTextInput(Rml::String(e.text.text));
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        return !context->ProcessMouseLeave();
    default:
        break;
    }
    return false;
}

#endif /* ICORECOMP_UI && ICORECOMP_PGS_SDL */
