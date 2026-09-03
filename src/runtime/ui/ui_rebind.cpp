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
 * Only keyboard and gamepad events are consumed. Mouse and window events
 * fall through to RmlUi, which keeps the menu underneath alive: the mouse
 * button that started the capture gets its matching release, and clicking a
 * different Rebind button re-arms on that slot instead of being swallowed.
 */
#include "ui.h"

#if defined(ICORECOMP_UI) && defined(ICORECOMP_PGS_SDL)

#include "ui_internal.h"

#include "../host/settings.h"
#include "../runtime.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <string>

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
bool g_gamepad = false;
int g_slot = -1;
std::string g_pending;              /* the accepted name, applied by rebind_tick */
RebindClock::time_point g_started;

int menu_slot(bool gamepad) {
    return gamepad ? (int)RT_GP_MENU : (int)RT_KB_MENU;
}

int slot_count(bool gamepad) {
    return gamepad ? (int)RT_GP_COUNT : (int)RT_KB_COUNT;
}

const std::string& stored_name(bool gamepad, int slot) {
    return gamepad ? rt_settings().input.gamepad[slot] : rt_settings().input.keyboard[slot];
}

/* SDL resolves names case-insensitively, so a stored "f1" and a captured
 * "F1" are the same key and have to compare equal here too. */
bool name_equal(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (SDL_tolower((unsigned char)a[i]) != SDL_tolower((unsigned char)b[i])) return false;
    }
    return true;
}

void set_status(const std::string& text) {
    settings_model_set_rebind(g_state == State::Capturing, g_gamepad, g_slot, text);
}

void end_capture(const char* log_reason, const std::string& status) {
    g_state = State::Idle;
    g_pending.clear();
    settings_model_set_rebind(false, g_gamepad, g_slot, status);
    if (log_reason) {
        rt_log("ui", "rebind %s.%s: %s",
            g_gamepad ? "gamepad" : "keyboard", rt_settings_binding_key(g_gamepad, g_slot), log_reason);
    }
    g_slot = -1;
}

/* Returns false, having set the status line, when `name` cannot be used for
 * the slot being captured. Both cases name the slot that already holds it:
 * "already taken" without saying by what is exactly the message that makes a
 * user hunt through the table. */
bool accept_allowed(const std::string& name) {
    const int menu = menu_slot(g_gamepad);
    const int count = slot_count(g_gamepad);
    for (int i = 0; i < count; ++i) {
        if (i == g_slot) continue;           /* re-pressing the slot's own name is fine */
        if (!name_equal(name, stored_name(g_gamepad, i))) continue;
        if (i == menu) {
            set_status("\"" + name + "\" is the menu key; the menu eats it before the pad sees it");
        } else {
            set_status("\"" + name + "\" is already bound to " + bind_slot_label(g_gamepad, i));
        }
        return false;
    }
    return true;
}

void accept(const std::string& name) {
    if (!accept_allowed(name)) return;       /* still capturing, waiting for another press */
    g_pending = name;
    g_state = State::Accepted;
    settings_model_set_rebind(false, g_gamepad, g_slot, "");
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

/* Gamepad button events while a gamepad slot is capturing. */
bool capture_gamepad_button(const SDL_Event& e) {
    const SDL_GamepadButton button = SDL_GamepadButton(e.gbutton.button);
    const char* name = SDL_GetGamepadStringForButton(button);
    if (!name || !name[0]) {
        set_status("SDL has no name for that button; it cannot be stored");
        return true;
    }
    accept(name);
    return true;
}

/* Axis motion while a gamepad slot is capturing. The stored name carries the
 * direction as a trailing '+' or '-', the convention host/input.cpp reads
 * back (settings.cpp's kGamepadBinds ships "lefttrigger+"). */
bool capture_gamepad_axis(const SDL_Event& e) {
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

} // namespace

void rebind_begin(bool gamepad, int slot) {
    if (slot < 0 || slot >= slot_count(gamepad)) return;
    if (g_state == State::Accepted) {
        /* A capture is accepted and waiting for the next rebind_tick to write
         * it to the settings. Re-arming here would clear g_pending and throw
         * that binding away with nothing on screen to say so, which reads as
         * a rebind that silently did not take. Refuse instead: the commit
         * happens at the coming field boundary, and a second click then arms
         * normally. */
        settings_model_set_rebind(false, g_gamepad, g_slot,
            "applying the previous capture, press Rebind again");
        rt_log("ui", "rebind %s.%s: refused, the previous capture is still being applied",
            gamepad ? "gamepad" : "keyboard", rt_settings_binding_key(gamepad, slot));
        return;
    }
    if (g_state != State::Idle) {
        /* Capturing, and the user clicked a different Rebind button before
         * pressing anything: drop the old one silently and arm the new slot.
         * Nothing has been captured yet, so nothing is lost. */
        g_state = State::Idle;
        g_pending.clear();
    }
    g_gamepad = gamepad;
    g_slot = slot;
    g_state = State::Capturing;
    g_started = RebindClock::now();
    settings_model_set_rebind(true, gamepad, slot, "");
    rt_log("ui", "rebind %s.%s: waiting for input (Escape cancels, 5 s timeout)",
        gamepad ? "gamepad" : "keyboard", rt_settings_binding_key(gamepad, slot));
}

bool rebind_active() {
    return g_state != State::Idle;
}

void rebind_cancel(const char* reason) {
    if (g_state == State::Idle) return;
    end_capture(reason, "rebind cancelled");
}

bool rebind_handle_sdl_event(const SDL_Event& e) {
    if (g_state != State::Capturing) return false;

    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        /* Escape cancels a gamepad capture too: it is the only way out from
         * the keyboard when the pad is what is being rebound. */
        if (!g_gamepad) return capture_key(e);
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
        return g_gamepad ? capture_gamepad_button(e) : true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        return g_gamepad ? capture_gamepad_axis(e) : true;
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
    const bool gamepad = g_gamepad;
    const int slot = g_slot;

    RtSettings& m = rt_settings_mutable();
    if (gamepad) {
        m.input.gamepad[slot] = name;
    } else {
        m.input.keyboard[slot] = name;
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
        settings_model_set_rebind(false, gamepad, slot, reject);
        g_slot = -1;
        rt_log("ui", "rebind %s.%s: the commit rejected \"%s\"",
            gamepad ? "gamepad" : "keyboard", rt_settings_binding_key(gamepad, slot), name.c_str());
        return;
    }

    g_state = State::Idle;
    g_pending.clear();
    settings_model_set_rebind(false, gamepad, slot,
        std::string(bind_slot_label(gamepad, slot)) + " is now \"" + name + "\"");
    g_slot = -1;
    rt_log("ui", "rebind %s.%s = \"%s\"",
        gamepad ? "gamepad" : "keyboard", rt_settings_binding_key(gamepad, slot), name.c_str());
}

} // namespace rtui

#endif /* ICORECOMP_UI && ICORECOMP_PGS_SDL */
