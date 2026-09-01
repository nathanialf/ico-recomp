/* ui/ui.h: the runtime's RmlUi overlay (settings menu, later the launcher).
 *
 * Everything in this module runs on the main OS thread, at the field
 * boundary, with no locking: guest threads are minicoro coroutines on that
 * same thread (see host/settings.h for the same statement about settings).
 *
 * Two entry points with very different rules:
 *   - rt_ui_tick() runs from rt_gs_vsync_hook (hw/gspriv.cpp), which is
 *     outside Granite's WSI::begin_frame. RmlUi's Update()/Render() call
 *     GenerateTexture/ReleaseTexture and the tick ends with
 *     rt_pgs_overlay_set_frame, all of which are between-frames-only entry
 *     points (the library's m_in_frame fatal guard). This is the only place
 *     they may be called from.
 *   - rt_ui_handle_sdl_event() runs from rt_window_pump (host/window.cpp),
 *     which can execute from inside WSI::begin_frame. It may only translate
 *     events into Rml::Context::Process* calls and flip flags. It must never
 *     call any rt_pgs_* function.
 *
 * This header deliberately includes no RmlUi headers; only the .cpp files in
 * this directory see them. Callers may include it in any build: without
 * ICORECOMP_UI the functions below are inline no-ops, so no call site needs
 * its own #ifdef.
 */
#ifndef ICORECOMP_UI_UI_H
#define ICORECOMP_UI_UI_H

#ifdef ICORECOMP_PGS_SDL
/* Declared, not included: ui.h stays free of SDL and RmlUi headers.
 * SDL3/SDL.h's own `typedef union SDL_Event {...} SDL_Event;` agrees with
 * this declaration, so either include order works. */
union SDL_Event;
#endif

#ifdef ICORECOMP_UI

/* Brings up RmlUi: interfaces, font, context and the menu document. Call
 * after rt_hw_init(), so the window (and therefore the surface size) exists.
 * Returns false, having logged the reason, when this build has no live
 * windowed backend, when rt_base_dir()/ui is missing, or when RmlUi itself
 * fails to initialize. A false return is not fatal: the game runs without a
 * settings menu. */
bool rt_ui_init();

/* One UI tick, from rt_gs_vsync_hook after rt_settings_apply_pending():
 * re-reads the surface size, applies whatever the menu's controls queued
 * since the last field (see ui_settings_model.cpp on why that is queued),
 * writes a pending settings save, and, whenever any document is up (the
 * menu, the fps readout, or both), updates and renders the RmlUi context
 * and hands the resulting draw list to the library as one overlay frame.
 * With nothing up it clears the overlay once and does no RmlUi work at all.
 * No-op until rt_ui_init() has succeeded. */
void rt_ui_tick();

bool rt_ui_visible();
void rt_ui_set_visible(bool visible);

/* The launcher: its own frame loop, run from main() before the config and
 * ELF are loaded, when main's launcher gate passes (see the sequence comment
 * in main.cpp). Shows launcher.rml, pumps events, applies pending settings,
 * ticks the UI and presents through rt_pgs_present_ui until the user starts
 * or quits.
 *
 * Returns true when the boot precheck passed and Start was pressed: the disc
 * is mounted and the boot ELF is already read and pin-checked, so the
 * rt_load_elf that follows reuses it. Returns false when the user chose Quit
 * or closed the window, and main then exits 0 without booting anything.
 *
 * Returns true immediately, with a log, when there is nothing to draw into
 * (no live window) or the launcher document failed to load: the launcher is
 * a front end, never a gate on running the game. No-op requirement on the
 * caller: rt_ui_init() must have run first. */
bool rt_launcher_run();

/* True while the menu owns input, which is exactly while it is visible.
 * host/input.cpp's SDL provider reports a centered pad with no buttons
 * instead of sampling the devices while this is true, so a keypress aimed at
 * the menu does not also reach the game. */
bool rt_ui_wants_input();

#ifdef ICORECOMP_PGS_SDL
/* Called from rt_window_pump for every event window.cpp does not fully own.
 * Inside, the order is binding capture (ui/ui_rebind.cpp), then the menu
 * hotkey, then RmlUi; the returned bool says whether the UI consumed the
 * event. The menu hotkey is consumed here and never reaches RmlUi or the
 * pad. */
bool rt_ui_handle_sdl_event(const SDL_Event& e);
#endif

#else /* !ICORECOMP_UI */

inline bool rt_ui_init() { return false; }
inline void rt_ui_tick() {}
inline bool rt_ui_visible() { return false; }
inline void rt_ui_set_visible(bool) {}
/* No UI means no launcher: main's gate tests ICORECOMP_UI first and never
 * reaches this, but the stub keeps the call site free of its own #ifdef. */
inline bool rt_launcher_run() { return true; }
inline bool rt_ui_wants_input() { return false; }
#ifdef ICORECOMP_PGS_SDL
inline bool rt_ui_handle_sdl_event(const SDL_Event&) { return false; }
#endif

#endif /* ICORECOMP_UI */

#endif /* ICORECOMP_UI_UI_H */
