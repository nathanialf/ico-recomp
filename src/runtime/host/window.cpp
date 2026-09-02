/* host/window.cpp: see window.h.
 *
 * Reentrancy rule (repeated from window.h because it is the one rule that
 * matters most here): rt_window_pump runs from inside WSI::begin_frame via
 * the pump_events callback (gs_parallel_present.cpp's RtPgs::present_frame),
 * after a swapchain image may already be acquired. From that context this
 * file may only queue/translate events and call rt_pgs_notify_quit /
 * rt_pgs_notify_resize; any rt_pgs_set_* entry point fatals mid-frame (the
 * library's m_in_frame guard). rt_window_apply_mode is a rt_pgs_set_*-shaped
 * operation in spirit (it touches the window) and must only ever be called
 * between frames -- from rt_settings_apply's hot path (settings_apply.cpp)
 * or from gs_parallel.cpp at startup, never from rt_window_pump itself.
 *
 * Guarded like host/input.cpp: SDL is only linked into the executable when
 * the live paraLLEl-GS backend was built with SDL3 window support
 * (ICORECOMP_PGS_SDL; see CMakeLists.txt). Without it every function here
 * is a no-op with no SDL calls compiled in.
 */
#include "window.h"

#include "../runtime.h"
#include "../ui/ui.h"

#ifdef ICORECOMP_PGS_SDL
#include "../gs/gs_parallel_api.h"
#include <SDL3/SDL.h>
#include <chrono>
#endif

#ifndef ICORECOMP_HAVE_PARALLEL_GS
/* No live paraLLEl-GS backend in this build (ICORECOMP_PARALLEL_GS=OFF at
 * configure time): gs_parallel.cpp, which defines the real
 * rt_gs_parallel_handle(), is not even compiled (it is only added to
 * icorecomp-runtime's sources under that option; see CMakeLists.txt). Stub
 * it here -- window.cpp is always built -- so this file and
 * settings_apply.cpp can call it unconditionally instead of every call site
 * needing the ICORECOMP_HAVE_PARALLEL_GS guard. Exactly one of this stub or
 * gs_parallel.cpp's real definition exists in any given build. */
RtPgs* rt_gs_parallel_handle() { return nullptr; }
/* Same reason, same pair: RT_PGS_PRESENT_MAILBOX is 0 (gs_parallel_api.h),
 * and no caller in such a build ever hands it to a library that is not
 * there. */
uint32_t rt_gs_parallel_present_mode() { return 0; }
#endif

#ifdef ICORECOMP_PGS_SDL

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

} // namespace

void rt_window_pump() {
    RtPgs* pgs = rt_gs_parallel_handle();
    if (!pgs) return; /* no live backend (dump mode, or not built) */
    void* raw = rt_pgs_window_handle(pgs);
    SDL_Window* win = (SDL_Window*)raw;
    if (!win) return; /* headless: nothing to pump */

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            rt_pgs_notify_quit(pgs);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            rt_pgs_notify_resize(pgs);
            record_window_size(win);
            break;
        default:
            break;
        }
        /* The UI sees every event, after this file has done its own part of
         * the handling: the menu hotkey has to work whether the menu is up
         * or not, and window/quit events are harmless to hand it. Inside the
         * UI the order is binding capture, then the hotkey, then RmlUi
         * (ui/ui_events.cpp); the bool it returns says whether the UI
         * consumed the event, and nothing downstream of this loop needs it
         * today. It stays inside the reentrancy rule: event translation and
         * flag flips only, no rt_pgs_* calls. No-op (an inline stub in ui.h)
         * when this build has no UI. */
        rt_ui_handle_sdl_event(e);
    }
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
    RtPgs* pgs = rt_gs_parallel_handle();
    if (!pgs) return;
    void* raw = rt_pgs_window_handle(pgs);
    SDL_Window* win = (SDL_Window*)raw;
    if (!win) return; /* headless */

    switch (s.display.mode) {
    case RtDisplayMode::FullscreenDesktop:
        /* Borderless desktop fullscreen, explicitly clearing any exclusive
         * mode a previous run left set: the only fullscreen mode that
         * behaves on Wayland (no compositor-granted exclusive mode there). */
        SDL_SetWindowFullscreenMode(win, nullptr);
        if (!SDL_SetWindowFullscreen(win, true)) {
            rt_log("window", "SDL_SetWindowFullscreen (desktop) failed: %s", SDL_GetError());
        }
        break;
    case RtDisplayMode::FullscreenExclusive: {
        const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(win));
        bool ok = desktop
            && SDL_SetWindowFullscreenMode(win, desktop)
            && SDL_SetWindowFullscreen(win, true);
        if (!ok) {
            rt_log("window", "exclusive fullscreen failed (%s); falling back to desktop fullscreen",
                SDL_GetError());
            SDL_SetWindowFullscreenMode(win, nullptr);
            if (!SDL_SetWindowFullscreen(win, true)) {
                rt_log("window", "SDL_SetWindowFullscreen (desktop fallback) failed: %s", SDL_GetError());
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

    rt_pgs_notify_resize(pgs);
}

#else /* !ICORECOMP_PGS_SDL */

void rt_window_pump() {}
void rt_window_flush_pending_save() {}
void rt_window_apply_mode(const RtSettings&) {}

#endif /* ICORECOMP_PGS_SDL */
