/* host/mouse.h: the host mouse device, collected once per field.
 *
 * This is the SDL side of mouse look. It owns four things and nothing else:
 * the relative motion accumulated since the last field, the last absolute
 * cursor position in window coordinates, whether the window has focus, and
 * whether the pointer is currently captured. host/mouse_look.h turns a
 * drained delta into a stick byte pair and host/input.cpp writes that into
 * the virtual pad.
 *
 * Capture is decided by four conditions and nothing else: the SDL input
 * provider is the one running (a scripted run never reads this module, so it
 * records nothing and captures nothing), mouse look is on, the window has
 * focus, and the overlay is not up. The game's own menus do
 * not release it; while the pointer owns the mouse (guest/menu_nav.h)
 * host/input.cpp hands the drained delta to the drawn cursor instead of the
 * camera stick, so relative mode and the hidden OS cursor stay as they are.
 *
 * Two entry points with different rules, the same split host/window.h and
 * ui/ui.h document:
 *   - rt_mouse_on_event() runs from rt_window_pump (host/window.cpp), which
 *     can execute from inside Granite's WSI::begin_frame. It may only record
 *     values and flip flags. It makes no SDL call and no window-service call.
 *   - rt_mouse_tick() runs from rt_gs_vsync_hook (hw/gspriv.cpp), at the
 *     field boundary. Every SDL call this module makes, which today means
 *     the relative-mouse-mode transitions, happens there.
 *
 * Everything here runs on the main OS thread with no locking, for the same
 * reason host/settings.h states: guest threads are minicoro coroutines on
 * that thread.
 *
 * Without ICORECOMP_HAVE_SDL (no SDL in this build) every function below is a
 * no-op that reports no motion, no focus and no capture, so call sites need
 * no #ifdef of their own.
 *
 * Runtime-internal, NOT part of the ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_HOST_MOUSE_H
#define ICORECOMP_HOST_MOUSE_H

#include <cstdint>

#ifdef ICORECOMP_HAVE_SDL
/* Declared, not included, exactly as ui/ui.h does it: this header stays free
 * of SDL headers, and SDL3/SDL.h's own `typedef union SDL_Event {...}
 * SDL_Event;` agrees with this declaration, so either include order works. */
union SDL_Event;

/* Records one SDL event. Called from rt_window_pump for every event, with
 * `ui_took_it` set to what rt_ui_handle_sdl_event() returned for it: an
 * event the overlay consumed is the menu's, so motion, wheel and button
 * presses are not also collected for the game. Window focus changes are
 * recorded either way, because focus is a property of the window rather than
 * a click anyone can consume. Motion, wheel and buttons are also dropped
 * whole while the SDL input provider is not the one running, for the same
 * reason the capture is refused then: nothing would ever read them. */
void rt_mouse_on_event(const SDL_Event& e, bool ui_took_it);
#endif

/* One field boundary's worth of mouse work: the first tick reads the
 * window's current focus, and every tick applies a capture transition when
 * the conditions for mouse look changed. This is the only function here that
 * calls SDL. */
void rt_mouse_tick();

/* Drains the accumulated relative motion into *dx, *dy (pixels, SDL's
 * convention: x positive right, y positive down) and zeroes it. Returns
 * false, leaving the outputs alone, when no motion was accumulated since the
 * last drain, which is the honest report for "no motion arrived this field".
 * Motion is only ever accumulated while the pointer is captured. */
bool rt_mouse_take_look_delta(float* dx, float* dy);

/* Throws away the accumulated motion. Used wherever the mouse must not reach
 * the game (the overlay is up, mouse look is off) so that a delta cannot
 * pile up and arrive as one jump when it comes back. */
void rt_mouse_discard_look_delta();

/* Whether the pointer is currently captured for mouse look. */
bool rt_mouse_captured();

/* Whether the window currently has input focus. */
bool rt_mouse_focused();

/* The last absolute cursor position seen, in window coordinates. Recorded
 * from every motion event, captured or not, so it is still meaningful the
 * moment capture is released. Both outputs are set to 0 before any motion
 * event has arrived. */
void rt_mouse_cursor_window(float* x, float* y);

/* The signed wheel travel one SDL_MOUSEWHEEL event carries, with SDL's
 * platform flip undone: `flipped` is `e.wheel.direction ==
 * SDL_MOUSEWHEEL_FLIPPED`, which says the platform already inverted the
 * value, so negating it is what leaves "positive is wheel up" true
 * everywhere. Inline here because two files have to agree about it:
 * host/mouse.cpp accumulates the ticks this way, and ui/ui_rebind.cpp reads
 * the sign the same way when it captures wheelup or wheeldown for a
 * binding. A capture that disagreed would store the name of the direction
 * the user did not scroll on a platform that sets the flag. */
inline float rt_mouse_wheel_signed(float y, bool flipped) { return flipped ? -y : y; }

/* Whole wheel ticks accumulated since the last call, positive for wheel up,
 * and zeroed by the call. The fraction a high-resolution wheel reports is
 * kept across calls rather than dropped. host/input.cpp drains this every
 * field and routes it: to guest/menu_nav.cpp while the pointer owns the
 * mouse, and to the bound wheel slots otherwise. */
int rt_mouse_take_wheel_ticks();

/* One button transition. `button` is the SDL button index (1 left, 2 middle,
 * 3 right, 4 and 5 the two side buttons); this header does not name them,
 * because host/mouse_names.h owns the names the settings file is written
 * with and mapping those onto these indices belongs to the consumer. */
struct RtMouseButtonEvent {
    uint8_t button = 0;
    bool down = false;
};

/* Drains up to `max` recorded button transitions into `out` in the order
 * they arrived and returns how many were written. The log is a small ring:
 * an overflow drops the oldest entries and says so in the log rather than
 * silently swallowing them. host/input.cpp drains it every field, on the
 * same routing as the wheel above, so an overflow means a field's worth of
 * transitions did not fit rather than that nobody is reading. */
int rt_mouse_take_button_events(RtMouseButtonEvent* out, int max);

/* Whether a button is held right now, by the same SDL button index. Level
 * state, tracked from the same events as the log above, so a consumer that
 * only wants "is it down" does not have to drain the log. Cleared whole
 * when the window loses focus: a button released over another window sends
 * no release here, and a bit left set would read as a press the moment
 * focus came back. */
bool rt_mouse_button_held(int button);

#endif /* ICORECOMP_HOST_MOUSE_H */
