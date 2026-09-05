/* ui/ui_rebind.cpp: the binding capture state machine behind the Input
 * pane's Rebind buttons.
 *
 * Shape: rebind_begin() arms a capture for one slot on one device; the pump
 * hands every event to rebind_handle_sdl_event() before the menu hotkey and
 * before RmlUi, so the key or button the user presses lands here instead of
 * toggling the menu or activating a widget; rebind_tick() runs at the field
 * boundary and is where an accepted capture is written to the settings and
 * committed.
 *
 * Two rules shape the rest:
 *
 *   - The accept never commits from the event handler. A commit runs
 *     rt_settings_apply, which touches the window and the GS library, and
 *     the pump can execute from inside Granite's WSI::begin_frame. The
 *     accepted name is parked in g_pending and rebind_tick() applies it, the
 *     same queued path the rest of the menu uses (see the file comment in
 *     ui_settings_model.cpp).
 *
 *   - A capture never silently steals a name that is already in use. A
 *     rejected press leaves the capture armed and puts the reason in the
 *     Input pane's status line, so the user presses something else rather
 *     than discovering later that two slots swapped.
 *
 * Which events a capture consumes follows the device being bound. A
 * keyboard or gamepad capture consumes keyboard and gamepad events only:
 * mouse and window events fall through to RmlUi, which keeps the menu
 * underneath alive, so the mouse button that started the capture gets its
 * matching release and clicking a different Rebind button re-arms on that
 * slot instead of being swallowed. A mouse capture consumes those same
 * events and, in addition, mouse motion, mouse buttons and the wheel: the
 * button the user presses has to become the binding rather than activating
 * whatever is under the pointer, and the menu must not hover-highlight
 * along the way.
 *
 * That leaves one ordering question, and RmlUi answers it. The click that
 * presses Rebind is dispatched from Context::ProcessMouseButtonUp
 * (Source/Core/Context.cpp: EventId::Click is fired there, when the active
 * element is still the hovered one), so by the time our data-event-click
 * callback calls rebind_begin() the pump has already handed RmlUi both the
 * button down and the button up of that click. No half of the arming click
 * can arrive after arming, and the first button down a mouse capture sees
 * is always a fresh press. The reverse case is real and handled: the press
 * this file accepts is consumed here, so its release would otherwise reach
 * RmlUi on its own, and the capture usually ends (at the next field
 * boundary) before the user lifts the button. g_swallow_button outlives the
 * capture for exactly that release, and is dropped by rebind_cancel so
 * nothing is swallowed once the menu is gone and the game owns the mouse
 * again.
 */
#include "ui.h"

#if defined(ICORECOMP_UI) && defined(ICORECOMP_HAVE_SDL)

#include "ui_internal.h"

#include "../host/mouse.h"
#include "../host/mouse_names.h"
#include "../host/settings.h"
#include "../runtime.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <string>
#include <vector>

namespace rtui {

namespace {

using RebindClock = std::chrono::steady_clock;

/* A capture that nobody finishes must not sit on the keyboard forever: the
 * pane says "press a key" and the menu would otherwise be unusable until the
 * user guessed that Escape cancels. */
constexpr auto kTimeout = std::chrono::seconds(5);

/* Axis travel that counts as a deliberate deflection during capture. Higher
 * than the raw press point host/input.cpp uses for a bound axis (8192 of
 * 32767, about 25%) on purpose: this is "the user meant this axis", not "the
 * trigger is pressed", and a resting stick on a worn pad can sit well off
 * center. */
constexpr float kCaptureAxis = 0.6f * 32767.0f;

enum class State { Idle, Capturing, Accepted };

State g_state = State::Idle;
RtBindDevice g_device = RT_BIND_KEYBOARD;
int g_slot = -1;
std::string g_pending;              /* the accepted name, applied by rebind_tick */
RebindClock::time_point g_started;
/* The SDL button index whose release is still owed to this file, or -1.
 * Deliberately outside the state machine: a mouse press is accepted the
 * moment it arrives, the capture ends at the next field boundary, and the
 * user's finger is normally still down then. */
int g_swallow_button = -1;

/* Chord capture: gamepad.menu, and only gamepad.menu, accepts on release
 * rather than on the first press, because it is the one slot a chord is
 * legal on. `g_chord_seen` is every distinct button pressed since the
 * capture armed, in press order; `g_chord_held` is how many of them are
 * still down. Accept happens once nothing is held: one distinct button
 * accepted alone, two accepted as "first+second" (order carries no meaning,
 * see settings.cpp's rt_settings_split_chord), a third distinct button
 * invalidates the attempt so the user is never left trying to store a chord
 * bigger than two. */
std::vector<SDL_GamepadButton> g_chord_seen;
int g_chord_held = 0;

bool is_menu_slot_capture() {
    return g_device == RT_BIND_GAMEPAD && g_slot == rt_settings_bind_menu_slot(RT_BIND_GAMEPAD);
}

/* The two devices whose captures read SDL gamepad events. Player 2's table
 * captures from any pad the same way player 1's does: a binding is a button
 * name, not a device, and the name resolves against whichever pad is on that
 * port at the time. Requiring the press to come from the pad currently on
 * port 1 would make the table unbindable whenever the second pad is
 * unplugged, and would say nothing about a pad swapped in later. */
bool is_gamepad_capture() {
    return g_device == RT_BIND_GAMEPAD || g_device == RT_BIND_GAMEPAD2;
}

void chord_reset() {
    g_chord_seen.clear();
    g_chord_held = 0;
}

const std::string& stored_name(RtBindDevice device, int slot) {
    const RtSettings& s = rt_settings();
    switch (device) {
    case RT_BIND_GAMEPAD:  return s.input.gamepad[slot];
    case RT_BIND_GAMEPAD2: return s.input.gamepad2[slot];
    case RT_BIND_MOUSE:    return s.input.mouse[slot];
    default:              return s.input.keyboard[slot];
    }
}

/* SDL resolves names case-insensitively, so a stored "f1" and a captured
 * "F1" are the same key and have to compare equal here too. An empty name
 * is never equal to anything, which is what keeps the unbound mouse slots
 * (most of them, by default) from reading as a table full of duplicates. */
bool name_equal(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (SDL_tolower((unsigned char)a[i]) != SDL_tolower((unsigned char)b[i])) return false;
    }
    return true;
}

void set_status(const std::string& text) {
    settings_model_set_rebind(g_state == State::Capturing, g_device, g_slot, text);
}

void end_capture(const char* log_reason, const std::string& status) {
    g_state = State::Idle;
    g_pending.clear();
    /* Nothing was stored, so no release is owed to this file: a press that
     * got this far was rejected, or the capture ended without one. */
    g_swallow_button = -1;
    chord_reset();
    settings_model_set_rebind(false, g_device, g_slot, status);
    if (log_reason) {
        rt_log_info("ui", "rebind %s.%s: %s",
            bind_device_name(g_device), rt_settings_binding_key(g_device, g_slot), log_reason);
    }
    g_slot = -1;
}

/* Returns false, having set the status line, when `name` cannot be used for
 * the slot being captured. Both cases name the slot that already holds it:
 * "already taken" without saying by what is exactly the message that makes a
 * user hunt through the table.
 *
 * Only the captured device's own slots are looked at. The mouse has no menu
 * slot (rt_settings_bind_menu_slot returns -1 for it), so that branch is
 * unreachable there, and player 2's pad has neither hotkey, so both are
 * unreachable there. A mouse name can never collide with a key or a pad
 * button because the two are never compared, and neither can a name on one
 * pad collide with the same name on the other: they are two devices and one
 * button on each is two buttons. */
bool accept_allowed(const std::string& name) {
    const int menu = rt_settings_bind_menu_slot(g_device);
    const int shot = rt_settings_bind_screenshot_slot(g_device);
    const int count = rt_settings_bind_slot_count(g_device);
    for (int i = 0; i < count; ++i) {
        if (i == g_slot) continue;           /* re-pressing the slot's own name is fine */
        if (!name_equal(name, stored_name(g_device, i))) continue;
        if (i == menu) {
            set_status("\"" + name + "\" is the menu key; the menu eats it before the pad sees it");
        } else if (i == shot) {
            /* The other host hotkey. Same reason as the menu key: the pump
             * takes it and nothing downstream ever sees the press. */
            set_status("\"" + name + "\" is the screenshot key; the host eats it before the pad"
                " sees it");
        } else {
            set_status("\"" + name + "\" is already bound to " + bind_slot_label(g_device, i));
        }
        return false;
    }
    return true;
}

void accept(const std::string& name) {
    if (!accept_allowed(name)) return;       /* still capturing, waiting for another press */
    g_pending = name;
    g_state = State::Accepted;
    settings_model_set_rebind(false, g_device, g_slot, "");
}

/* Keyboard events while a keyboard slot is capturing. */
bool capture_key(const SDL_Event& e) {
    if (e.key.repeat) return true;
    if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
        end_capture("cancelled with Escape", "rebind cancelled");
        return true;
    }
    const char* name = SDL_GetScancodeName(e.key.scancode);
    if (!name || !name[0]) {
        set_status("SDL has no name for that key; it cannot be stored");
        return true;
    }
    accept(name);
    return true;
}

/* Gamepad button events while a gamepad slot is capturing. Every slot but
 * gamepad.menu accepts on the first press, same as always. gamepad.menu
 * accepts on release instead, because it is the one slot a chord is legal
 * on: see the g_chord_seen comment above and capture_gamepad_button_up
 * below, which does the accepting. */
bool capture_gamepad_button(const SDL_Event& e) {
    const SDL_GamepadButton button = SDL_GamepadButton(e.gbutton.button);
    const char* name = SDL_GetGamepadStringForButton(button);
    if (!name || !name[0]) {
        set_status("SDL has no name for that button; it cannot be stored");
        return true;
    }
    if (!is_menu_slot_capture()) {
        accept(name);
        return true;
    }
    bool already_seen = false;
    for (SDL_GamepadButton b : g_chord_seen) {
        if (b == button) already_seen = true;
    }
    if (!already_seen) {
        if (g_chord_seen.size() >= 2) {
            chord_reset();
            set_status("only two buttons make a chord; release and try again");
            return true;
        }
        g_chord_seen.push_back(button);
    }
    ++g_chord_held;
    return true;
}

/* The release half of the menu-slot chord capture above: every other slot's
 * gamepad release is swallowed (nothing to do, the button already
 * accepted on the press). Accepts once nothing captured is still held: one
 * distinct button becomes its own name, two become "first+second" in the
 * order they were pressed. */
bool capture_gamepad_button_up(const SDL_Event& e) {
    if (!is_menu_slot_capture()) return true;
    if (g_chord_held > 0) --g_chord_held;
    (void)e;
    if (g_chord_held > 0 || g_chord_seen.empty()) return true;
    if (g_chord_seen.size() == 1) {
        accept(SDL_GetGamepadStringForButton(g_chord_seen[0]));
    } else {
        const char* a = SDL_GetGamepadStringForButton(g_chord_seen[0]);
        const char* b = SDL_GetGamepadStringForButton(g_chord_seen[1]);
        accept(std::string(a ? a : "") + "+" + (b ? b : ""));
    }
    chord_reset();
    return true;
}

/* Axis motion while a gamepad slot is capturing. Refused outright for
 * gamepad.menu: an axis can never be part of a chord and the grammar (settings.cpp,
 * rt_settings_split_chord) has no way to spell one alongside a button
 * anyway. The stored name for every other slot carries the direction as a
 * trailing '+' or '-', the convention host/input.cpp reads back
 * (settings.cpp's kGamepadBinds ships "lefttrigger+"). */
bool capture_gamepad_axis(const SDL_Event& e) {
    if (is_menu_slot_capture()) {
        set_status("an axis cannot be the menu key; press a button, or hold two together");
        return true;
    }
    const float value = (float)e.gaxis.value;
    if (value > -kCaptureAxis && value < kCaptureAxis) return true;
    const char* name = SDL_GetGamepadStringForAxis(SDL_GamepadAxis(e.gaxis.axis));
    if (!name || !name[0]) {
        set_status("SDL has no name for that axis; it cannot be stored");
        return true;
    }
    accept(std::string(name) + (value > 0.0f ? "+" : "-"));
    return true;
}

/* Mouse button presses while a mouse slot is capturing. The names are ours,
 * not SDL's (host/mouse_names.h explains why), so the mapping from the SDL
 * button index lives here and nowhere else on this side. A button beyond X2
 * has no name to store, which is the same answer the other three capture
 * functions give for an input SDL cannot name. */
bool capture_mouse_button(const SDL_Event& e) {
    RtMouseInput input;
    switch (e.button.button) {
    case SDL_BUTTON_LEFT:   input = RT_MOUSE_LEFT; break;
    case SDL_BUTTON_RIGHT:  input = RT_MOUSE_RIGHT; break;
    case SDL_BUTTON_MIDDLE: input = RT_MOUSE_MIDDLE; break;
    case SDL_BUTTON_X1:     input = RT_MOUSE_X1; break;
    case SDL_BUTTON_X2:     input = RT_MOUSE_X2; break;
    default:
        set_status("this port has no name for mouse button " + std::to_string((int)e.button.button)
            + "; it cannot be stored");
        return true;
    }
    accept(rt_mouse_input_name(input));
    if (g_state == State::Accepted) {
        /* Accepted, so this press is consumed here and its release must be
         * too, whenever it comes. */
        g_swallow_button = (int)e.button.button;
    }
    return true;
}

/* Wheel motion while a mouse slot is capturing. SDL3 reports y positive
 * away from the user, which is the direction the wheelup name means, except
 * on a platform that sets SDL_MOUSEWHEEL_FLIPPED and has already inverted
 * it. rt_mouse_wheel_signed (host/mouse.h) undoes that, which is the same
 * rule host/mouse.cpp accumulates ticks by: without it a capture on such a
 * platform would store wheeldown for the scroll that input.cpp then reads
 * back as wheelup. A purely horizontal scroll has no name in this table, so
 * it leaves the capture armed rather than storing something the user did
 * not ask for. */
bool capture_mouse_wheel(const SDL_Event& e) {
    const float y = rt_mouse_wheel_signed(e.wheel.y,
        e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
    if (y > 0.0f) {
        accept(rt_mouse_input_name(RT_MOUSE_WHEEL_UP));
    } else if (y < 0.0f) {
        accept(rt_mouse_input_name(RT_MOUSE_WHEEL_DOWN));
    }
    return true;
}

} // namespace

void rebind_begin(RtBindDevice device, int slot) {
    if (slot < 0 || slot >= rt_settings_bind_slot_count(device)) return;
    if (g_state == State::Accepted) {
        /* A capture is accepted and waiting for the next rebind_tick to write
         * it to the settings. Re-arming here would clear g_pending and throw
         * that binding away with nothing on screen to say so, which reads as
         * a rebind that silently did not take. Refuse instead: the commit
         * happens at the coming field boundary, and a second click then arms
         * normally. */
        settings_model_set_rebind(false, g_device, g_slot,
            "applying the previous capture, press Rebind again");
        rt_log_warn("ui", "rebind %s.%s: refused, the previous capture is still being applied",
            bind_device_name(device), rt_settings_binding_key(device, slot));
        return;
    }
    if (g_state != State::Idle) {
        /* Capturing, and the user clicked a different Rebind button before
         * pressing anything: drop the old one silently and arm the new slot.
         * Nothing has been captured yet, so nothing is lost. */
        g_state = State::Idle;
        g_pending.clear();
    }
    g_device = device;
    g_slot = slot;
    g_state = State::Capturing;
    g_started = RebindClock::now();
    chord_reset();
    settings_model_set_rebind(true, device, slot, "");
    rt_log_info("ui", "rebind %s.%s: waiting for input (Escape cancels, 5 s timeout)",
        bind_device_name(device), rt_settings_binding_key(device, slot));
}

bool rebind_active() {
    return g_state != State::Idle || g_swallow_button >= 0;
}

void rebind_cancel(const char* reason, bool drop_accepted) {
    /* The menu is closing or the table was rewritten under the capture. An
     * owed release stops being ours at that point: keeping it would swallow
     * a release after the menu has gone, which is the one direction that
     * could leave something stuck down for whoever reads the mouse next. */
    g_swallow_button = -1;
    /* An accepted capture is a binding the user made, parked for the next
     * rebind_tick to write. The menu closing is not a reason to throw it
     * away: the user pressed the button, the pane said so, and closing the
     * menu in the same field would otherwise lose it silently. rt_ui_tick
     * calls rebind_tick whether or not the menu is up, so the commit still
     * happens at the coming field boundary and the state goes idle there.
     * The callers that rewrite the table under it pass drop_accepted, since
     * for them the accepted name is about to be overwritten anyway and
     * applying it afterwards would undo the reset. */
    if (g_state == State::Accepted && !drop_accepted) return;
    if (g_state == State::Idle) return;
    end_capture(reason, "rebind cancelled");
}

bool rebind_handle_sdl_event(const SDL_Event& e) {
    /* Owed first, before the state test: this release belongs to a press
     * this file already consumed, and the capture it belonged to may have
     * finished several fields ago. */
    if (g_swallow_button >= 0 && e.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        (int)e.button.button == g_swallow_button) {
        g_swallow_button = -1;
        return true;
    }
    if (g_state != State::Capturing) return false;

    const bool mouse = g_device == RT_BIND_MOUSE;

    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        /* Escape cancels a gamepad or mouse capture too: it is the only way
         * out from the keyboard when something else is being rebound. */
        if (g_device == RT_BIND_KEYBOARD) return capture_key(e);
        if (!e.key.repeat && e.key.scancode == SDL_SCANCODE_ESCAPE) {
            end_capture("cancelled with Escape", "rebind cancelled");
        }
        return true;
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
        /* Swallowed so the key that was captured does not also type into
         * whatever field had the focus. */
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        return is_gamepad_capture() ? capture_gamepad_button(e) : true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return is_gamepad_capture() ? capture_gamepad_button_up(e) : true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return is_gamepad_capture() ? capture_gamepad_axis(e) : true;
    case SDL_EVENT_GAMEPAD_REMOVED:
        /* The pad this capture was reading is gone. host/input.cpp (called
         * first by the pump) decides whether another pad takes over; either
         * way any partial chord read from the old pad is meaningless, so
         * drop it and tell the user to press again rather than let a stale
         * g_chord_seen combine with buttons from whatever pad shows up
         * next. Only the capture's own device cares: a keyboard or mouse
         * capture ignores it and falls through, same as ui_events.cpp's
         * nav-hold clear does for that event. */
        if (!is_gamepad_capture()) return false;
        chord_reset();
        set_status("controller removed; press the button again");
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return mouse ? capture_mouse_button(e) : false;
    case SDL_EVENT_MOUSE_WHEEL:
        return mouse ? capture_mouse_wheel(e) : false;
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_MOTION:
        /* Consumed only while a mouse slot is capturing: the menu under a
         * keyboard or gamepad capture stays usable, and the menu under a
         * mouse capture must not hover, click or scroll on the way to the
         * button being bound. */
        return mouse;
    default:
        return false;
    }
}

void rebind_tick() {
    if (g_state == State::Capturing) {
        if (RebindClock::now() - g_started < kTimeout) return;
        end_capture("timed out with nothing pressed", "rebind timed out");
        return;
    }
    if (g_state != State::Accepted) return;

    const std::string name = g_pending;
    const RtBindDevice device = g_device;
    const int slot = g_slot;

    RtSettings& m = rt_settings_mutable();
    switch (device) {
    case RT_BIND_GAMEPAD:  m.input.gamepad[slot] = name; break;
    case RT_BIND_GAMEPAD2: m.input.gamepad2[slot] = name; break;
    case RT_BIND_MOUSE:    m.input.mouse[slot] = name; break;
    default:              m.input.keyboard[slot] = name; break;
    }
    rt_settings_commit(false);
    rt_settings_request_save();

    /* commit_validate applies the same collision rules a second time, over
     * the whole struct rather than the one slot this capture looked at. It
     * can still reject (a name the user typed into the file by hand, a slot
     * that changed since the capture began), and when it does the pane shows
     * its words, not ours. */
    const char* reject = rt_settings_last_reject();
    if (reject[0]) {
        g_state = State::Idle;
        g_pending.clear();
        settings_model_set_rebind(false, device, slot, reject);
        g_slot = -1;
        rt_log_warn("ui", "rebind %s.%s: the commit rejected \"%s\"",
            bind_device_name(device), rt_settings_binding_key(device, slot), name.c_str());
        return;
    }

    g_state = State::Idle;
    g_pending.clear();
    settings_model_set_rebind(false, device, slot,
        std::string(bind_slot_label(device, slot)) + " is now \"" + name + "\"");
    g_slot = -1;
    rt_log_info("ui", "rebind %s.%s = \"%s\"",
        bind_device_name(device), rt_settings_binding_key(device, slot), name.c_str());
}

} // namespace rtui

#endif /* ICORECOMP_UI && ICORECOMP_HAVE_SDL */
