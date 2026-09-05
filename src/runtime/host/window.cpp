/* host/window.cpp: see window.h.
 *
 * Reentrancy rule (repeated from window.h because it is the one rule that
 * matters most here): rt_window_pump runs from inside WSI::begin_frame via
 * the pump_events callback (gs_parallel_present.cpp's RtPgs::present_frame),
 * after a swapchain image may already be acquired. From that context this
 * file may only queue/translate events and call rt_window_notify_quit /
 * rt_window_notify_resize / rt_window_sample_state, whose sink callbacks are
 * flag sets. rt_window_apply_mode touches the window itself and must only
 * ever be called between frames -- from rt_settings_apply's hot path
 * (settings_apply.cpp) or from gs/gs_select.cpp at startup, never from
 * rt_window_pump.
 *
 * Guarded like host/input.cpp: SDL is only linked into the executable when
 * this build has it (ICORECOMP_HAVE_SDL; see CMakeLists.txt). Without it
 * every function here is a no-op with no SDL calls compiled in.
 */
#include "window.h"

#include "../runtime.h"
#include "../ui/ui.h"
#include "input.h"
#include "mouse.h"
#include "screenshot.h"
#include "window_service.h"

#include <cstdlib>

#ifdef ICORECOMP_HAVE_SDL

#include <SDL3/SDL.h>
#include <chrono>

namespace {

using WindowClock = std::chrono::steady_clock;

/* display.remember_window_size: a live-resize drag delivers many
 * WINDOW_RESIZED events per second. This flag is not the save debounce (that
 * is rt_settings_request_save(), one mechanism for the whole runtime); it is
 * this file's "when is it safe to commit" gate. record_window_size runs
 * inside the pump, which can execute from inside WSI::begin_frame, and a
 * commit runs the display applier, which touches the window. So the size
 * change is recorded here and committed a second of quiet later from
 * rt_window_flush_pending_save(), which rt_settings_apply_pending() calls at
 * the field boundary. */
bool g_size_dirty = false;
WindowClock::time_point g_size_dirty_since;

void maybe_save_window_size() {
    if (!g_size_dirty) return;
    if (WindowClock::now() - g_size_dirty_since < std::chrono::seconds(1)) return;
    g_size_dirty = false;
    /* Validate and apply, but do not write here: the file write is asked for
     * through the settings layer's own debounce, so a resize drag and a menu
     * edit in the same second produce one write, not two. */
    rt_settings_commit(false);
    rt_settings_request_save();
}

/* Records the window's current size into settings when the user (not
 * rt_window_apply_mode itself) resized it. Idempotent: rt_window_apply_mode
 * setting the window to the size settings already holds reads back the same
 * size here and never marks dirty, so applying settings never re-triggers a
 * save. */
void record_window_size(SDL_Window* win) {
    if (!rt_settings().display.remember_window_size) return;
    if (SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    if (w <= 0 || h <= 0) return;
    RtSettings& m = rt_settings_mutable();
    if (m.display.window_width == w && m.display.window_height == h) return;
    m.display.window_width = w;
    m.display.window_height = h;
    g_size_dirty = true;
    g_size_dirty_since = WindowClock::now();
}

/* rt_window_hold_event_dispatch's depth, and the once-only line that says a
 * restricted pump happened. Counted rather than a flag: the GS ring's waits
 * nest (a wait runs a pump, and a settings change from that pump can enqueue
 * and wait again). */
int g_dispatch_held = 0;
bool g_restricted_pump_logged = false;

} // namespace

void rt_window_hold_event_dispatch(bool on) {
    if (on) {
        ++g_dispatch_held;
    } else if (g_dispatch_held > 0) {
        --g_dispatch_held;
    }
}

namespace {

/* The pump a wait runs while dispatch is held: no SDL_PollEvent, so nothing
 * is taken out of the queue and nothing is handed to the UI, the input layer
 * or the mouse. What it still does is everything the waiting side actually
 * needs:
 *
 *   - SDL_PumpEvents runs the platform's message loop, which is what keeps
 *     the window responsive to the compositor and what updates SDL's own
 *     window state, so the minimized flag rt_window_sample_state reads below
 *     is fresh. That is how a restore reaches a consumer parked on an
 *     unpresentable swapchain.
 *   - a peek (SDL_PeepEvents with SDL_PEEKEVENT, which leaves the events
 *     queued) for the quit and resize events the presenting backend has to
 *     hear about, so a close during a wait still ends the run and a resize
 *     during one still rebuilds the swapchain. Both notifications are
 *     idempotent flag sets, so the real pump seeing the same events
 *     afterwards costs nothing.
 *
 * Every input event stays in the queue and is delivered in order by the next
 * unheld pump, which is the field boundary. Nothing is dropped and nothing
 * is delivered out of order. */
void restricted_pump() {
    if (!g_restricted_pump_logged) {
        g_restricted_pump_logged = true;
        rt_log_info("window", "event dispatch held: a GS ring wait pumped the window without "
                         "dispatching (events stay queued for the field boundary)");
    }
    SDL_PumpEvents();
    /* SDL_HasEvent scans the queue without copying, so a quit is found
     * however much input is queued ahead of it. The close request is looked
     * for first because it is the one that says the player asked: SDL
     * generates SDL_EVENT_QUIT itself when the last window goes away, and a
     * quit with no close behind it is a different event with a different
     * cause and a different log level (window_service.h). */
    if (SDL_HasEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) {
        rt_window_notify_quit("the window's close button (SDL_EVENT_WINDOW_CLOSE_REQUESTED)", true);
    }
    if (SDL_HasEvent(SDL_EVENT_QUIT)) {
        rt_window_notify_quit("SDL_EVENT_QUIT with no window close behind it. SDL carries no"
            " source on this event, so what raised it (a session logout, a signal the SDL"
            " backend translates, a window server that went away) is not knowable from here",
            false);
    }
    /* Window events only: the mouse-motion flood a wait can accumulate is
     * outside this range, so sixteen is far more than the queue ever holds
     * of these, and anything past it is seen by the next pump 2 ms later or
     * by the full one at the field boundary. */
    SDL_Event win_events[16];
    const int n = SDL_PeepEvents(win_events, 16, SDL_PEEKEVENT,
                                 SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST);
    for (int i = 0; i < n; ++i) {
        switch (win_events[i].type) {
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            rt_window_notify_resize();
            break;
        default:
            break;
        }
    }
    rt_window_sample_state();
}

} // namespace

void rt_window_pump() {
    SDL_Window* win = (SDL_Window*)rt_window_handle();
    if (!win) return; /* headless: nothing to pump */

    if (g_dispatch_held > 0) {
        restricted_pump();
        return;
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            /* The player pressed the window's close button. Handled by name
             * rather than left to the SDL_EVENT_QUIT that SDL raises after
             * it, so the run can tell "the user closed the window" from "a
             * quit turned up and nothing explains it" (window_service.h).
             * The first cause wins there, so this arriving before SDL's own
             * quit is what makes the good one stick. */
            rt_window_notify_quit("the window's close button (SDL_EVENT_WINDOW_CLOSE_REQUESTED)", true);
            break;
        case SDL_EVENT_QUIT:
            rt_window_notify_quit("SDL_EVENT_QUIT with no window close behind it. SDL carries no"
                " source on this event, so what raised it (a session logout, a signal the SDL"
                " backend translates, a window server that went away) is not knowable from here",
                false);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            rt_window_notify_resize();
            record_window_size(win);
            break;
        /* Restoring a minimized window on Windows usually keeps the pixel
         * size, so neither event above fires and nothing would tell the
         * backend the window can be presented to again. The notification is
         * a flag set plus a state resample, so raising it on these two costs
         * a swapchain rebuild that was going to happen anyway. */
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            rt_window_notify_resize();
            break;
        default:
            break;
        }
        /* Gamepad hot-plug, ahead of the UI: rt_input_on_sdl_event only
         * opens/closes a pad and logs, which is legal from the pump the same
         * way the QUIT/RESIZED handling above is, and the UI's own gamepad
         * navigation (ui/ui_events.cpp) reads whichever pad this leaves
         * open. A no-op for every event but GAMEPAD_ADDED/REMOVED. */
        rt_input_on_sdl_event(e);
        /* The UI sees every event, after this file has done its own part of
         * the handling: the menu hotkey has to work whether the menu is up
         * or not, and window/quit events are harmless to hand it. Inside the
         * UI the order is binding capture, then the hotkey, then RmlUi
         * (ui/ui_events.cpp); the bool it returns says whether the UI
         * consumed the event, and the mouse is the consumer of that answer:
         * a click or a motion the overlay took is the menu's and must not
         * also reach the game. Both calls stay inside the reentrancy rule:
         * event translation and flag flips only, no GS calls, and
         * host/mouse.cpp keeps its own SDL calls in rt_mouse_tick at the
         * field boundary. rt_ui_handle_sdl_event is a no-op (an inline stub
         * in ui.h) when this build has no UI. */
        const bool ui_took_it = rt_ui_handle_sdl_event(e);
        /* The screenshot hotkey, after the UI and only for events the UI did
         * not take: a rebind capture in the menu has to win, so that binding
         * the screenshot key to something else does not fire a capture on the
         * press being bound. Before the mouse, and folded into what the mouse
         * is told was consumed, so a mouse button bound to the screenshot does
         * not also reach the game. It records a flag and nothing else, which
         * is all this context allows (see the reentrancy rule above); the
         * capture itself happens in rt_screenshot_tick at the field boundary.
         * Not inside the UI: with ICORECOMP_UI=OFF rt_ui_handle_sdl_event is
         * an inline stub and the hotkey still has to work. */
        const bool shot_took_it = ui_took_it ? false : rt_screenshot_on_sdl_event(e);
        rt_mouse_on_event(e, ui_took_it || shot_took_it);
    }
    /* Refreshes the window's cached pixel size and minimized state, and
     * forwards the sample to the presenting backend's sink. This thread is
     * the only one that may ask SDL for either (the event queue is per thread
     * on Windows), and the GS command ring's worker thread is what needs the
     * answers: Granite asks the WSI platform for a surface size from inside
     * begin_frame, on the worker. Once per pump, so once per field plus every
     * EE-side wait that pumps; two SDL queries at that rate cost nothing.
     * Legal from inside begin_frame like the notify_* calls above, and for
     * the same reason: it touches host-side state, not the swapchain. See
     * the threads section of host/window_service.h. */
    rt_window_sample_state();
    /* The size commit happens in rt_window_flush_pending_save, not here:
     * rt_settings_commit runs the display applier, and this function can
     * execute from inside WSI::begin_frame via pump_events, where window and
     * swapchain calls are off limits (the reentrancy rule at the top of this
     * file). */
}

void rt_window_flush_pending_save() {
    maybe_save_window_size();
}

void rt_window_apply_mode(const RtSettings& s) {
    SDL_Window* win = (SDL_Window*)rt_window_handle();
    if (!win) return; /* headless */

    switch (s.display.mode) {
    case RtDisplayMode::FullscreenDesktop:
        /* Borderless desktop fullscreen, explicitly clearing any exclusive
         * mode a previous run left set: the only fullscreen mode that
         * behaves on Wayland (no compositor-granted exclusive mode there). */
        SDL_SetWindowFullscreenMode(win, nullptr);
        if (!SDL_SetWindowFullscreen(win, true)) {
            rt_log_warn("window", "SDL_SetWindowFullscreen (desktop) failed: %s", SDL_GetError());
        }
        break;
    case RtDisplayMode::FullscreenExclusive: {
        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(win));
        bool ok = desktop
            && SDL_SetWindowFullscreenMode(win, desktop)
            && SDL_SetWindowFullscreen(win, true);
        if (!ok) {
            rt_log_warn("window", "exclusive fullscreen failed (%s); falling back to desktop fullscreen",
                SDL_GetError());
            SDL_SetWindowFullscreenMode(win, nullptr);
            if (!SDL_SetWindowFullscreen(win, true)) {
                rt_log_warn("window", "SDL_SetWindowFullscreen (desktop fallback) failed: %s", SDL_GetError());
            }
        }
        break;
    }
    case RtDisplayMode::Windowed:
    default:
        SDL_SetWindowFullscreenMode(win, nullptr);
        SDL_SetWindowFullscreen(win, false);
        SDL_SetWindowSize(win, s.display.window_width, s.display.window_height);
        break;
    }

    rt_window_notify_resize();
}

void rt_window_set_icon(const uint8_t* rgba, uint32_t w, uint32_t h, const uint8_t* rgba2x, uint32_t w2,
                        uint32_t h2) {
    SDL_Window* win = (SDL_Window*)rt_window_handle();
    if (!win) return; /* headless */
    if (!rgba || w == 0 || h == 0) return;

    /* SDL_CreateSurfaceFrom aliases the caller's pixels rather than copying
     * them, so `rgba` and `rgba2x` must outlive SDL_SetWindowIcon below,
     * which is where SDL makes its own copy; both surfaces are destroyed
     * before this returns. */
    SDL_Surface* base = SDL_CreateSurfaceFrom(int(w), int(h), SDL_PIXELFORMAT_RGBA32, (void*)rgba,
        int(w) * 4);
    if (!base) {
        rt_log_warn("window", "SDL_CreateSurfaceFrom (window icon) failed: %s", SDL_GetError());
        return;
    }
    SDL_Surface* alt = nullptr;
    if (rgba2x && w2 > 0 && h2 > 0) {
        alt = SDL_CreateSurfaceFrom(int(w2), int(h2), SDL_PIXELFORMAT_RGBA32, (void*)rgba2x, int(w2) * 4);
        if (!alt) {
            rt_log_warn("window", "SDL_CreateSurfaceFrom (window icon alternate) failed: %s", SDL_GetError());
        } else if (!SDL_AddSurfaceAlternateImage(base, alt)) {
            rt_log_warn("window", "SDL_AddSurfaceAlternateImage (window icon) failed: %s", SDL_GetError());
        }
    }
    if (!SDL_SetWindowIcon(win, base)) {
        rt_log_warn("window", "SDL_SetWindowIcon failed: %s", SDL_GetError());
    }
    if (alt) SDL_DestroySurface(alt);
    SDL_DestroySurface(base);
}

void rt_request_exit(const char* why) {
    rt_log_info("window", "exit requested: %s", why);
    /* A deliberate end, so the end-of-run summary goes out at info. Named
     * here rather than at the exit itself because this is the only place
     * that knows which of the several deliberate ends this is: Quit from
     * the menu, Quit from the launcher, the window's own close button, or a
     * restart to apply a cold key. */
    rt_run_set_exit_reason(true, "exit requested: %s", why);
    if (rt_window_exists()) {
        /* A deliberate request by construction: every caller is Quit from
         * the menu, Quit from the launcher, or the restart that applies a
         * cold key. */
        rt_window_notify_quit(why, true);
    } else {
        /* No window to catch this at its next vsync (dump mode, or
         * headless): there is nothing left to wait for. */
        std::exit(0);
    }
}

#else /* !ICORECOMP_HAVE_SDL */

void rt_window_pump() {}
void rt_window_hold_event_dispatch(bool) {}
void rt_window_flush_pending_save() {}
void rt_window_apply_mode(const RtSettings&) {}
void rt_window_set_icon(const uint8_t*, uint32_t, uint32_t, const uint8_t*, uint32_t, uint32_t) {}

void rt_request_exit(const char* why) {
    rt_log_info("window", "exit requested: %s", why);
    rt_run_set_exit_reason(true, "exit requested: %s", why);
    std::exit(0);
}

#endif /* ICORECOMP_HAVE_SDL */
