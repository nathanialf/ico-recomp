/* host/mouse.cpp: see mouse.h.
 *
 * Reentrancy rule (the same one host/window.cpp states, because this file's
 * event entry point is called from inside that file's pump): rt_mouse_on_
 * event runs from rt_window_pump, which can execute from inside Granite's
 * WSI::begin_frame. It therefore only records values and flips flags. Every
 * SDL call this module makes is in rt_mouse_tick, at the field boundary.
 *
 * Guarded like host/input.cpp and host/window.cpp: SDL is only linked into
 * the executable when the live paraLLEl-GS backend was built with SDL3
 * window support (ICORECOMP_PGS_SDL). Without it every function here is a
 * no-op with no SDL calls compiled in.
 */
#include "mouse.h"

#include "../runtime.h"

#ifdef ICORECOMP_PGS_SDL
#include "../gs/gs_parallel_api.h"
#include "../ui/ui.h"
#include "input.h"
#include "settings.h"
#include "window.h"

#include <cmath>

#include <SDL3/SDL.h>

namespace {

/* Relative motion accumulated since the last drain, in pixels. Only added to
 * while captured. */
float g_dx = 0.0f, g_dy = 0.0f;
bool g_have_delta = false;

/* Last absolute position, window coordinates. Recorded from every motion
 * event, captured or not. */
float g_cursor_x = 0.0f, g_cursor_y = 0.0f;

bool g_focused = false;
bool g_focus_known = false;     /* the first tick reads it from the window */
bool g_captured = false;

/* Wheel: SDL reports fractional scroll on high-resolution wheels, so the
 * fraction is kept and only whole ticks are handed out. */
float g_wheel_accum = 0.0f;

/* Button transitions, oldest first. 32 is well past what one field can
 * produce from a human hand; an overflow is a bug elsewhere, so it is
 * logged rather than absorbed. */
constexpr int kButtonLogSize = 32;
RtMouseButtonEvent g_button_log[kButtonLogSize];
int g_button_log_count = 0;
bool g_button_log_overflowed = false;
uint32_t g_button_held = 0;     /* bit (index - 1) per SDL button index */

/* Names the condition that ended a capture, for the log line. */
const char* g_release_reason = "";

/* A relative-mode call that failed, and the state it was asked for. The
 * retry itself is wanted: a failure can be transient and the next field
 * should try again. The log line is not, once per field for the rest of the
 * run, so it is latched the same way g_button_log_overflowed above is. The
 * line comes back when the wanted state changes, and a call that succeeds
 * clears the latch, so a failure that returns is reported again. */
bool g_capture_error_logged = false;
bool g_capture_error_wanted = false;

SDL_Window* window() {
    RtPgs* pgs = rt_gs_parallel_handle();
    if (!pgs) return nullptr;          /* no live backend (dump mode, or not built) */
    return (SDL_Window*)rt_pgs_window_handle(pgs);
}

/* Whether the pointer should be captured right now, and, when it should not,
 * which condition said so. The order is the order the reason is reported in;
 * it is not a precedence, since any one of them is enough. */
bool capture_wanted() {
    if (!rt_input_sdl_active()) {
        /* A scripted run (host/input.h) feeds the pad from a file and never
         * reads this module, so grabbing the pointer for it would trap the
         * cursor for no one. */
        g_release_reason = "the SDL input provider is not active";
        return false;
    }
    if (!rt_settings().input.mouse_look) { g_release_reason = "mouse look is off"; return false; }
    if (!g_focused) { g_release_reason = "the window lost focus"; return false; }
    if (rt_ui_wants_input()) { g_release_reason = "the settings menu is up"; return false; }
    /* The game's own menu is not a release condition. While the pointer owns
     * the mouse (guest/menu_nav.h) relative mode stays on and this field's
     * motion moves a cursor the overlay draws inside the picture instead of
     * the camera stick, so the OS cursor stays hidden and the pointer cannot
     * walk out of the window. host/input.cpp does that routing. */
    g_release_reason = "";
    return true;
}

void drop_delta() {
    g_dx = 0.0f;
    g_dy = 0.0f;
    g_have_delta = false;
}

void push_button(uint8_t button, bool down) {
    if (button >= 1 && button <= 32) {
        const uint32_t bit = 1u << (button - 1);
        if (down) g_button_held |= bit; else g_button_held &= ~bit;
    }
    if (g_button_log_count >= kButtonLogSize) {
        /* Drop the oldest so the newest transition, which is the one a
         * consumer is about to act on, survives. Loud, once per run. */
        if (!g_button_log_overflowed) {
            g_button_log_overflowed = true;
            rt_log("input", "mouse button log overflowed at %d entries; the oldest are being"
                " dropped, which means nothing is draining it", kButtonLogSize);
        }
        for (int i = 1; i < kButtonLogSize; ++i) g_button_log[i - 1] = g_button_log[i];
        g_button_log_count = kButtonLogSize - 1;
    }
    g_button_log[g_button_log_count].button = button;
    g_button_log[g_button_log_count].down = down;
    ++g_button_log_count;
}

} // namespace

void rt_mouse_on_event(const SDL_Event& e, bool ui_took_it) {
    /* Mouse state is only recorded while the SDL input provider is the one
     * running. A scripted run (host/input.h) feeds the pad from a file and
     * never reads this module, so nothing would drain the button log and it
     * would report an overflow that is not a bug; the same condition already
     * refuses the capture in capture_wanted(). Focus falls through, because
     * it is a property of the window rather than mouse state. */
    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (!rt_input_sdl_active()) return;
        break;
    default:
        break;
    }

    switch (e.type) {
    case SDL_EVENT_MOUSE_MOTION:
        /* The absolute position is recorded even for an event the overlay
         * consumed: it is where the pointer is, and the overlay having drawn
         * something under it does not change that. */
        g_cursor_x = e.motion.x;
        g_cursor_y = e.motion.y;
        if (ui_took_it || !g_captured) break;
        /* A driver that hands us a value that is not finite cannot be turned
         * into a rate. Saying so beats carrying a NaN into the stick bytes,
         * where it would come out as a silently wrong pair. */
        if (!std::isfinite(e.motion.xrel) || !std::isfinite(e.motion.yrel)) {
            rt_log("input", "mouse motion with a non-finite relative delta (%f, %f); ignored",
                (double)e.motion.xrel, (double)e.motion.yrel);
            break;
        }
        g_dx += e.motion.xrel;
        g_dy += e.motion.yrel;
        g_have_delta = true;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        if (ui_took_it) break;
        if (!std::isfinite(e.wheel.y)) {
            rt_log("input", "mouse wheel with a non-finite delta (%f); ignored",
                (double)e.wheel.y);
            break;
        }
        /* The un-flip rule lives in mouse.h, because ui/ui_rebind.cpp has
         * to read the sign the same way when it captures a wheel binding. */
        g_wheel_accum += rt_mouse_wheel_signed(e.wheel.y,
            e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (ui_took_it) break;
        push_button(e.button.button, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        g_focused = true;
        g_focus_known = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        g_focused = false;
        g_focus_known = true;
        /* The level state goes with the focus. A button released over
         * another window produces no BUTTON_UP here, so a held bit would
         * stay set for the rest of the run: the slot bound to it would read
         * as pressed again the moment focus came back, and
         * rt_input_last_device() would sit on keyboard and mouse. Nothing is
         * pushed into the transition log for this, because no button was
         * released in this window; forgetting a level is not the same as
         * inventing an event. */
        g_button_held = 0;
        break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        /* The pointer left the window. Relative mode confines it, so this
         * only arrives while capture is off; the accumulator is dropped so a
         * motion recorded on the way out cannot arrive a field later. */
        drop_delta();
        break;
    default:
        break;
    }
}

void rt_mouse_tick() {
    SDL_Window* win = window();
    if (!win) return;               /* headless: nothing to capture */

    if (!g_focus_known) {
        /* No focus event has arrived yet, which is the state on the first
         * field after the window comes up. Ask the window instead of
         * assuming, so a run that starts focused captures immediately. */
        g_focused = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
        g_focus_known = true;
    }

    const bool wanted = capture_wanted();
    if (wanted == g_captured) return;

    /* Relative mode hides the cursor itself (SDL_mouse.h: it "hides the
     * cursor, grabs mouse input, and ... reports continuous relative mouse
     * motion"), so there is no SDL_HideCursor/SDL_ShowCursor pair here. */
    if (!SDL_SetWindowRelativeMouseMode(win, wanted)) {
        if (!g_capture_error_logged || g_capture_error_wanted != wanted) {
            g_capture_error_logged = true;
            g_capture_error_wanted = wanted;
            rt_log("input", "SDL_SetWindowRelativeMouseMode(%s) failed: %s; mouse look stays %s",
                wanted ? "true" : "false", SDL_GetError(), g_captured ? "captured" : "released");
        }
        return;
    }
    g_capture_error_logged = false;

    g_captured = wanted;
    /* Whatever was accumulated across the edge belongs to the other mode. */
    drop_delta();
    if (wanted) {
        rt_log("input", "mouse look: captured");
    } else {
        rt_log("input", "mouse look: released (%s)", g_release_reason);
    }
}

bool rt_mouse_take_look_delta(float* dx, float* dy) {
    if (!g_have_delta) return false;
    if (dx) *dx = g_dx;
    if (dy) *dy = g_dy;
    drop_delta();
    return true;
}

void rt_mouse_discard_look_delta() {
    drop_delta();
}

bool rt_mouse_captured() { return g_captured; }
bool rt_mouse_focused() { return g_focused; }

void rt_mouse_cursor_window(float* x, float* y) {
    if (x) *x = g_cursor_x;
    if (y) *y = g_cursor_y;
}

int rt_mouse_take_wheel_ticks() {
    const float whole = std::trunc(g_wheel_accum);
    g_wheel_accum -= whole;
    return (int)whole;
}

int rt_mouse_take_button_events(RtMouseButtonEvent* out, int max) {
    int n = g_button_log_count < max ? g_button_log_count : max;
    if (n < 0) n = 0;
    for (int i = 0; i < n; ++i) out[i] = g_button_log[i];
    const int left = g_button_log_count - n;
    for (int i = 0; i < left; ++i) g_button_log[i] = g_button_log[n + i];
    g_button_log_count = left;
    return n;
}

bool rt_mouse_button_held(int button) {
    if (button < 1 || button > 32) return false;
    return (g_button_held & (1u << (button - 1))) != 0;
}

#else /* !ICORECOMP_PGS_SDL */

void rt_mouse_tick() {}
bool rt_mouse_take_look_delta(float*, float*) { return false; }
void rt_mouse_discard_look_delta() {}
bool rt_mouse_captured() { return false; }
bool rt_mouse_focused() { return false; }
void rt_mouse_cursor_window(float* x, float* y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
}
int rt_mouse_take_wheel_ticks() { return 0; }
int rt_mouse_take_button_events(RtMouseButtonEvent*, int) { return 0; }
bool rt_mouse_button_held(int) { return false; }

#endif /* ICORECOMP_PGS_SDL */
