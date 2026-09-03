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

#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Traits.h>

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace rtui {

namespace {

/* Resolved once at init from settings; the compiled-in values here are the
 * same defaults settings.cpp ships ("F1" and "guide") so an unresolvable
 * name can fall back without inventing a different binding. */
SDL_Scancode g_menu_scancode = SDL_SCANCODE_F1;
/* A single button (g_menu_chord[1] == INVALID) or a chord of two
 * (input.gamepad.menu = "back+start"). Order carries no meaning: dispatch
 * below checks either half as the one just pressed with the other held. */
SDL_GamepadButton g_menu_chord[2] = {SDL_GAMEPAD_BUTTON_GUIDE, SDL_GAMEPAD_BUTTON_INVALID};

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

/* `force` skips the text-field exception: the keyboard's Escape belongs to a
 * focused text field there (it reverts the edit), but the gamepad's East
 * closes the menu regardless of what has focus, because East has no other
 * meaning a text field could claim. */
bool close_menu(bool force = false) {
    /* Nothing to close: the launcher is up on its own. Escape does nothing
     * there on purpose, so a stray press cannot quit out of it. Returning
     * false lets the key reach RmlUi, where it is only a focus-level key. */
    if (!g_ui.visible) return false;
    if (!force && text_input_focused()) return false;
    rt_ui_set_visible(false);
    return true;
}

/* Gamepad navigation while the menu is up. South maps to Enter, which
 * ElementDocument::ProcessDefaultAction turns into a Click() on whatever
 * has tab-index: auto focus (every control in these documents): that is
 * how South starts a button, toggles a checkbox and opens a closed select.
 * The d-pad directions are not mapped here: they and the left stick share
 * one held/repeat state machine below (NavHold), because a direction has to
 * behave the same way whichever one produced it. KI_UNKNOWN means "this
 * button has no menu meaning", and the event falls through unconsumed. */
Rml::Input::KeyIdentifier convert_gamepad_button(SDL_GamepadButton button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return Rml::Input::KI_RETURN;
    default:                       return Rml::Input::KI_UNKNOWN;
    }
}

/* ---- gamepad directional navigation ---------------------------------------
 *
 * The left stick is read as four synthetic d-pad edges (hysteresis, fixed
 * and independent of input.left_deadzone: this is about the menu noticing a
 * deliberate push, not about what the game is told the stick reports), and
 * both the real d-pad and the stick share one held/repeat state per
 * direction: press dispatches once and starts a hold, ui_nav_tick() (called
 * every field from rt_ui_tick, after rebind_tick) re-dispatches it after
 * 400 ms and then every 100 ms, and the hold clears on release, on the menu
 * closing, or on the pad being unplugged.
 *
 * A direction that reaches RmlUi and is not consumed (ElementDocument's own
 * arrow handling found nothing to move to -- the last item in a column, or
 * nothing focused at all) is re-dispatched once with the document itself
 * focused; `body { nav: auto }` (base.rcss) then answers it with the first
 * or last focusable element in document order, which is this module's one
 * wrap mechanism and also its recovery when a data-if removed whatever had
 * the focus.
 */

constexpr Sint16 kNavAxisPress = 16384;
constexpr Sint16 kNavAxisRelease = 9830;
constexpr auto kNavRepeatDelay = std::chrono::milliseconds(400);
constexpr auto kNavRepeatInterval = std::chrono::milliseconds(100);

enum class NavDir { Left, Right, Up, Down, Count };

struct NavHold {
    bool held = false;
    std::chrono::steady_clock::time_point next_repeat_at;
};
NavHold g_nav_hold[(size_t)NavDir::Count];

Rml::Input::KeyIdentifier nav_dir_key(NavDir dir) {
    switch (dir) {
    case NavDir::Left:  return Rml::Input::KI_LEFT;
    case NavDir::Right: return Rml::Input::KI_RIGHT;
    case NavDir::Up:    return Rml::Input::KI_UP;
    case NavDir::Down:  return Rml::Input::KI_DOWN;
    default:            return Rml::Input::KI_UNKNOWN;
    }
}

/* ---- select pad session ----------------------------------------------------
 *
 * A closed <select> is reached like any other focusable control: native
 * tab-index, native South -> Click(), which opens it. An open one is not
 * driven through RmlUi's own arrow handling, because WidgetDropDown answers
 * an Up/Down there by moving the selection immediately (WidgetDropDown.cpp,
 * SeekSelection -> SetSelection), which sets the <select>'s value and fires
 * "change" on every step -- exactly the event menu.rml's data-event-
 * change="apply()" commits, so an intermediate option the thumb passed
 * through on the way to the one it wanted would be saved. A pad session
 * therefore owns Up/Down itself while a select is open: it moves a
 * highlighted option (the "padhl" pseudo-class, ui/style/base.rcss) without
 * touching the control's value, and only South commits, by clicking the
 * highlighted option once, the same way a mouse click on it always has.
 */
Rml::Element* g_nav_select = nullptr;        /* the open <select>, or null */
Rml::Element* g_nav_select_option = nullptr; /* the highlighted option */

void select_session_clear_highlight() {
    if (g_nav_select_option) g_nav_select_option->SetPseudoClass("padhl", false);
    g_nav_select_option = nullptr;
}

void select_session_end() {
    select_session_clear_highlight();
    g_nav_select = nullptr;
}

/* WidgetDropDown moves every <option> out of the <select>'s own children and
 * under an internal "selectbox" element it owns (ElementFormControlSelect.cpp
 * MoveChildren, WidgetDropDown.cpp AddOption), and that wrapper is appended as
 * a non-DOM child (WidgetDropDown's constructor passes false). A subtree query
 * from the select therefore never reaches the options: Element::
 * GetElementsByTagName walks GetNumChildren(), which counts DOM children only.
 * ElementFormControlSelect's own GetNumOptions/GetOption is the one way in from
 * outside the widget. A data-for template option stays a child of the box with
 * display: none and is not a real option; WidgetDropDown::OnChildAdd tells it
 * apart by the same attribute, and skipping it here keeps the two agreeing. */
void select_session_options(Rml::Element* select, Rml::ElementList* out) {
    out->clear();
    Rml::ElementFormControlSelect* control = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(select);
    if (!control) return;
    const int count = control->GetNumOptions();
    for (int i = 0; i < count; ++i) {
        Rml::Element* option = control->GetOption(i);
        if (option && !option->HasAttribute("data-for")) out->push_back(option);
    }
}

void select_session_begin(Rml::Element* select) {
    Rml::ElementList options;
    select_session_options(select, &options);
    if (options.empty()) return;
    g_nav_select = select;
    Rml::Element* current = nullptr;
    for (Rml::Element* opt : options) {
        if (opt->IsPseudoClassSet("checked")) {
            current = opt;
            break;
        }
    }
    g_nav_select_option = current ? current : options.front();
    g_nav_select_option->SetPseudoClass("padhl", true);
}

void select_session_move(bool forward) {
    if (!g_nav_select) return;
    Rml::ElementList options;
    select_session_options(g_nav_select, &options);
    if (options.empty()) return;
    size_t idx = 0;
    for (size_t i = 0; i < options.size(); ++i) {
        if (options[i] == g_nav_select_option) {
            idx = i;
            break;
        }
    }
    const size_t n = options.size();
    idx = forward ? (idx + 1) % n : (idx + n - 1) % n;
    select_session_clear_highlight();
    g_nav_select_option = options[idx];
    g_nav_select_option->SetPseudoClass("padhl", true);
}

/* Commits the highlighted option by clicking it: WidgetDropDown's own Click
 * handler for an option (ProcessEvent, EventId::Click) sets the value,
 * fires exactly one "change", hides the box and returns focus to the
 * <select> -- the same sequence a mouse click on it produces. */
void select_session_commit() {
    if (g_nav_select_option) g_nav_select_option->Click();
    select_session_end();
}

/* If nothing is focused, or focus has drifted outside the document that is
 * actually up (a data-if removed the focused element, or the menu just
 * opened over the launcher), focus the document itself so the first arrow
 * key answers with the first or last focusable element (`nav: auto` on
 * body.menu / body.launcher) instead of doing nothing. Called before any
 * pad key that means something to this module. */
void ensure_document_focus(Rml::Context* context) {
    Rml::ElementDocument* target = g_ui.visible ? g_ui.menu : g_ui.launcher;
    if (!target) return;
    Rml::Element* focus = context->GetFocusElement();
    if (!focus || focus->GetOwnerDocument() != target) target->Focus();
}

/* One arrow key: dispatch, and once, if nothing consumed it, retry with the
 * document itself focused (the wrap/recovery mechanism the file comment
 * above describes). */
bool dispatch_nav_key(Rml::Context* context, Rml::Input::KeyIdentifier key) {
    bool consumed = !context->ProcessKeyDown(key, 0);
    if (!consumed) {
        Rml::ElementDocument* doc = g_ui.visible ? g_ui.menu : g_ui.launcher;
        if (doc) {
            doc->Focus();
            consumed = !context->ProcessKeyDown(key, 0);
        }
    }
    return consumed;
}

/* ---- the settings menu's two-level pad model ------------------------------
 *
 * menu.rml is two regions: the cards column on the left (five .nav-button
 * elements) and the pane on the right, which shows the active tab's
 * section. Native RmlUi spatial nav, asked to move Up/Down/Left/Right from
 * an arbitrary point, does not know about that split -- it just finds the
 * nearest focusable element in the requested screen direction, which can
 * and does cross from a card into the pane or back, and would also open a
 * <select>'s own arrow-key handling to a stray Left/Right. This section
 * layers a level on top of the native handling rather than replacing it:
 * level 1 (a card has focus) answers Up/Down by moving among the five
 * cards and Left/Right by changing the active tab; level 2 (something in
 * the pane has focus) answers Up/Down with the native search, vetoed if it
 * would land on a card, and Left/Right with the native search alone (a
 * range slider or a select's own handling claims it; nothing else does,
 * and nothing here falls back to a document-wide search the way
 * dispatch_nav_key's Up/Down does).
 *
 * The level is never stored: it is read from wherever the focus actually
 * is, every time, by current_nav_level() below. That is what keeps a mouse
 * click from ever disagreeing with the pad -- clicking a card or a pane
 * control changes what has focus, which is the only thing the level is
 * decided from.
 */
enum class NavLevel { Cards, Pane };

NavLevel current_nav_level(Rml::Context* context) {
    for (Rml::Element* e = context->GetFocusElement(); e; e = e->GetParentNode()) {
        if (e->IsClassSet("nav-button")) return NavLevel::Cards;
        if (e->IsClassSet("pane")) return NavLevel::Pane;
    }
    return NavLevel::Cards;
}

/* Level 2's guard against escaping the pane, shared by Up/Down and
 * Left/Right: one ProcessKeyDown call can relocate focus onto a card or a
 * footer button, and this puts it back. The escape is the document-wrap
 * fallback, not the native search: the native
 * search itself is confined to the focused element's nearest scroll
 * container (ElementDocument.cpp, FindNextNavigationElement's
 * GetNearestScrollContainer), which for anything in the pane is the pane
 * (`overflow-y: auto`), but dispatch_nav_key's second try focuses the
 * document, and from the document `nav: auto` answers with the next
 * focusable element in the whole document (FindNextTabElement) -- a card
 * going Down, the footer's last button going Up. Up/Down goes through
 * dispatch_nav_key and so has that fallback, which for a control at the
 * top or bottom of a section is common; Left/Right goes through a single
 * plain ProcessKeyDown with no fallback and so should never escape, and
 * runs through here anyway rather than trusting that. Landing outside the
 * pane is the one outcome this function reverts; whatever else the key did
 * (a slider's own adjustment, a text field's own cursor move) stands. */
void nav_fire_pane_key(Rml::Context* context, Rml::Input::KeyIdentifier key, bool with_wrap) {
    Rml::Element* before = context->GetFocusElement();
    if (with_wrap) {
        dispatch_nav_key(context, key);
    } else {
        context->ProcessKeyDown(key, 0);
    }
    Rml::Element* after = context->GetFocusElement();
    /* Anything outside the pane is an escape, not only a card: the
     * document-wrap fallback lands Down on the first focusable element
     * (a card) and Up on the last (the footer's Quit button). */
    if (before && after && after != before && current_nav_level(context) != NavLevel::Pane) {
        before->Focus(true);
    }
}

/* The settings menu's own direction handling (g_ui.visible), level decided
 * fresh each call. Left to nav_fire below when only the launcher is up
 * (g_ui.launcher_visible without g_ui.visible): the launcher has no cards
 * and no pane, and keeps the flat navigation it always had. */
void nav_fire_menu(Rml::Context* context, NavDir dir) {
    if (current_nav_level(context) == NavLevel::Cards) {
        /* Level 1 with the focus somewhere that is not one of the five cards:
         * a footer button, or the document itself, which is where RmlUi puts
         * the focus after a click on empty panel (Context::ProcessMouse
         * ButtonDown walks up to the first element that accepts focus, and
         * the body does). settings_model_focus_card and _enter_card both
         * match the focused element against the five card ids, so without
         * this every direction would be a no-op and the pad would have no way
         * back to a card. The active tab's card is where it goes. */
        Rml::Element* focus = context->GetFocusElement();
        if (!focus || !focus->IsClassSet("nav-button")) {
            settings_model_focus_active_tab();
            return;
        }
        switch (dir) {
        case NavDir::Up:    settings_model_focus_card(-1); return;
        case NavDir::Down:  settings_model_focus_card(1);  return;
        case NavDir::Left:  settings_model_cycle_tab(-1);  return;
        case NavDir::Right: settings_model_cycle_tab(1);   return;
        default: return;
        }
    }
    switch (dir) {
    case NavDir::Up:
    case NavDir::Down:
        nav_fire_pane_key(context, nav_dir_key(dir), /*with_wrap=*/true);
        return;
    case NavDir::Left:
    case NavDir::Right:
        nav_fire_pane_key(context, nav_dir_key(dir), /*with_wrap=*/false);
        return;
    default:
        return;
    }
}

/* What a single press of `dir` does: moves the select session's highlight
 * if one is open, otherwise routes to the settings menu's two-level model
 * while it is up, or dispatches the plain arrow key everywhere else (the
 * launcher, whose navigation stays flat). Shared by the press edge
 * (BUTTON_DOWN / an axis crossing its press threshold) and by ui_nav_
 * tick()'s repeat, so the two can never disagree about what a direction
 * means. */
void nav_fire(NavDir dir) {
    Rml::Context* context = g_ui.context;
    if (!context) return;
    if (g_nav_select) {
        /* An open box owns all four directions, not only the two it acts on:
         * a Left or Right forwarded to RmlUi would be answered by the pane's
         * own spatial search, which moves the focus off the <select>, and the
         * blur that follows closes the box behind the session's back. */
        if (dir == NavDir::Up || dir == NavDir::Down) select_session_move(dir == NavDir::Down);
        return;
    }
    if (g_ui.visible) {
        nav_fire_menu(context, dir);
        return;
    }
    dispatch_nav_key(context, nav_dir_key(dir));
}

void nav_hold_set(NavDir dir, bool held) {
    NavHold& hold = g_nav_hold[(size_t)dir];
    if (held && !hold.held) {
        hold.held = true;
        hold.next_repeat_at = std::chrono::steady_clock::now() + kNavRepeatDelay;
        nav_fire(dir);
    } else if (!held) {
        hold.held = false;
    }
}

void nav_hold_clear_all() {
    for (NavHold& hold : g_nav_hold) hold.held = false;
}

bool button_to_navdir(SDL_GamepadButton button, NavDir* out) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  *out = NavDir::Left;  return true;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: *out = NavDir::Right; return true;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:    *out = NavDir::Up;    return true;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  *out = NavDir::Down;  return true;
    default: return false;
    }
}

/* Left-stick axis motion as the same two press/release edges a d-pad button
 * would produce, with hysteresis: press above kNavAxisPress, release below
 * kNavAxisRelease, so a stick resting between the two (as one that never
 * quite recenters will) does not chatter the hold. */
void axis_edge(NavDir dir, bool press, bool release) {
    if (press) {
        nav_hold_set(dir, true);
    } else if (release) {
        nav_hold_set(dir, false);
    }
    /* Neither: the axis sits in the hysteresis band, and whatever the hold
     * already was stands. */
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
    std::string chord_a, chord_b;
    if (rt_settings_split_chord(gp, &chord_a, &chord_b)) {
        const SDL_GamepadButton a = SDL_GetGamepadButtonFromString(chord_a.c_str());
        const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(chord_b.c_str());
        if (a == SDL_GAMEPAD_BUTTON_INVALID || b == SDL_GAMEPAD_BUTTON_INVALID) {
            rt_log("ui", "input.gamepad.menu \"%s\" does not resolve as a chord; keeping the default %s",
                gp.c_str(), menu_gamepad_name());
        } else {
            g_menu_chord[0] = a;
            g_menu_chord[1] = b;
        }
    } else {
        const SDL_GamepadButton button =
            gp.empty() ? SDL_GAMEPAD_BUTTON_INVALID : SDL_GetGamepadButtonFromString(gp.c_str());
        if (button == SDL_GAMEPAD_BUTTON_INVALID) {
            rt_log("ui", "input.gamepad.menu \"%s\" is not an SDL gamepad button name; keeping the default %s",
                gp.c_str(), menu_gamepad_name());
        } else {
            g_menu_chord[0] = button;
            g_menu_chord[1] = SDL_GAMEPAD_BUTTON_INVALID;
        }
    }

    rt_log("ui", "menu hotkey: keyboard %s, gamepad %s",
        SDL_GetScancodeName(g_menu_scancode), menu_gamepad_name());
}

const char* menu_hotkey_name() {
    const char* name = SDL_GetScancodeName(g_menu_scancode);
    return name ? name : "";
}

const char* menu_gamepad_name() {
    /* SDL's longest button name ("rightpaddle2") is well under half of
     * this, even doubled with a '+' between the two. */
    static char buf[64];
    if (g_menu_chord[1] == SDL_GAMEPAD_BUTTON_INVALID) {
        const char* name = SDL_GetGamepadStringForButton(g_menu_chord[0]);
        return name ? name : "";
    }
    const char* a = SDL_GetGamepadStringForButton(g_menu_chord[0]);
    const char* b = SDL_GetGamepadStringForButton(g_menu_chord[1]);
    std::snprintf(buf, sizeof(buf), "%s+%s", a ? a : "", b ? b : "");
    return buf;
}

void ui_nav_tick() {
    /* Which document the pad is driving: the menu while it is up, the
     * launcher otherwise, nothing when neither is. Keyed on that rather
     * than on "anything is up" so a hold or a select session is dropped
     * when the menu opens over the launcher or closes back onto it too,
     * not only when everything goes away: a direction still held across
     * that boundary would otherwise carry on repeating into whichever
     * document comes next, and a session belongs to the document its
     * <select> lives in. */
    static const void* prev_document = nullptr;
    const void* document = g_ui.visible ? (const void*)g_ui.menu
                                        : (g_ui.launcher_visible ? (const void*)g_ui.launcher : nullptr);
    if (document != prev_document) {
        prev_document = document;
        nav_hold_clear_all();
        select_session_end();
    }
    if (!document) return;

    if (g_nav_select && g_ui.context->GetFocusElement() != g_nav_select) {
        /* The select lost focus by some means other than the East/South
         * handling below (a mouse click elsewhere, Tab, the document itself
         * losing focus): the session no longer means anything. */
        select_session_end();
    }

    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < (size_t)NavDir::Count; ++i) {
        NavHold& hold = g_nav_hold[i];
        if (!hold.held || now < hold.next_repeat_at) continue;
        hold.next_repeat_at = now + kNavRepeatInterval;
        nav_fire((NavDir)i);
    }
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
    if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
        /* Nothing here consumes this: host/input.cpp's own rt_input_on_sdl_
         * event (called first by the pump) decides whether another pad
         * takes over. This side effect only keeps a hold or a select session
         * from surviving the pad it was reading. Ahead of the capture below,
         * which does consume this event for a gamepad capture: after it, a
         * direction held when the pad was pulled would keep repeating. */
        nav_hold_clear_all();
        select_session_end();
    }

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
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        const SDL_GamepadButton button = SDL_GamepadButton(e.gbutton.button);
        if (g_menu_chord[1] == SDL_GAMEPAD_BUTTON_INVALID) {
            if (button == g_menu_chord[0]) return toggle_menu();
        } else if (button == g_menu_chord[0] || button == g_menu_chord[1]) {
            /* Chord: the other half has to already be held, so the toggle
             * fires exactly once, on whichever half completes the pair, in
             * either order. */
            const SDL_GamepadButton other = (button == g_menu_chord[0]) ? g_menu_chord[1] : g_menu_chord[0];
            if (SDL_Gamepad* gp = SDL_GetGamepadFromID(e.gbutton.which)) {
                if (SDL_GetGamepadButton(gp, other)) return toggle_menu();
            }
        }
        break;
    }
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
        ensure_document_focus(context);
        const SDL_GamepadButton button = SDL_GamepadButton(e.gbutton.button);

        if (button == SDL_GAMEPAD_BUTTON_EAST) {
            if (g_nav_select) {
                g_nav_select->Click(); /* closes the open box, no value change */
                select_session_end();
                return true;
            }
            /* The settings menu's level 2: East backs out to level 1 (the
             * card for the tab that is still showing) rather than closing
             * the menu. A second East from there falls through to the
             * close below, since current_nav_level() now reads Cards. */
            if (g_ui.visible && current_nav_level(context) == NavLevel::Pane) {
                settings_model_focus_active_tab();
                return true;
            }
            /* Forced: East closes the menu even out of a focused text field,
             * unlike the keyboard's Escape (see close_menu's comment). */
            return close_menu(/*force=*/true);
        }
        if (button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER || button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) {
            settings_model_cycle_tab(button == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER ? 1 : -1);
            return true;
        }

        NavDir dir = NavDir::Count;
        if (button_to_navdir(button, &dir)) {
            nav_hold_set(dir, true);
            return true;
        }

        if (button == SDL_GAMEPAD_BUTTON_SOUTH) {
            Rml::Element* focus = context->GetFocusElement();
            /* The settings menu's level 1: South enters the focused card,
             * making its tab active (if it was not already) and moving
             * focus into the now-visible pane (level 2). */
            if (focus && focus->IsClassSet("nav-button")) {
                settings_model_enter_card();
                return true;
            }
            if (focus && focus->GetTagName() == "select" && !focus->IsPseudoClassSet("disabled")) {
                if (g_nav_select == focus) {
                    select_session_commit();
                } else {
                    select_session_begin(focus);
                    focus->Click(); /* opens the box */
                }
                return true;
            }
        }

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
        const SDL_GamepadButton button = SDL_GamepadButton(e.gbutton.button);
        NavDir dir = NavDir::Count;
        if (button_to_navdir(button, &dir)) {
            nav_hold_set(dir, false);
            return true;
        }
        const Rml::Input::KeyIdentifier key = convert_gamepad_button(button);
        if (key == Rml::Input::KI_UNKNOWN) return false;
        return !context->ProcessKeyUp(key, 0);
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && e.gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) return false;
        ensure_document_focus(context);
        const bool vertical = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY;
        const Sint16 v = e.gaxis.value;
        axis_edge(vertical ? NavDir::Up : NavDir::Left, v <= -kNavAxisPress, v > -kNavAxisRelease);
        axis_edge(vertical ? NavDir::Down : NavDir::Right, v >= kNavAxisPress, v < kNavAxisRelease);
        return false; /* the game does not read gamepad axis events either way */
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
