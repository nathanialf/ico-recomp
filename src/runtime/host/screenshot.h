/* host/screenshot.h: the user-facing screenshot.
 *
 * F12 (input.keyboard.screenshot, rebindable, with unbound gamepad and mouse
 * twins) writes a PNG of the presented picture into a screenshots folder
 * beside saves/. What lands in the file is the window backbuffer over exactly
 * the rectangle rt_window_present_rect reports: the picture at presented size,
 * with the display aspect already applied by the blit and the letterbox bars
 * excluded, taken before the overlay render pass so the settings menu, the
 * launcher, the drawn pointer and the fps readout are never in it. See
 * gs_parallel_api.h's screenshot section for the library half.
 *
 * The split between the two halves is deliberate: only raw pixels cross the
 * MIT/LGPL C ABI. The folder, the file name, the timestamp, the collision
 * rule and the PNG encoder (host/png_write.h) are all on this side.
 *
 * Everything here runs on the main OS thread, like the rest of host/: the
 * event entry point from rt_window_pump and the tick from rt_gs_vsync_hook.
 */
#ifndef ICORECOMP_HOST_SCREENSHOT_H
#define ICORECOMP_HOST_SCREENSHOT_H

/* Declared, not included: this header is pulled in by files that have no SDL
 * headers, the same arrangement host/mouse.h and ui/ui.h use, and SDL3/SDL.h's
 * own `typedef union SDL_Event {...} SDL_Event;` agrees with it, so either
 * include order works. */
union SDL_Event;

/* Called from rt_window_pump (host/window.cpp) for every event the UI did not
 * take, so a rebind capture in the menu wins over the hotkey and so the
 * feature still works with ICORECOMP_UI=OFF. Returns true when this file
 * consumed the event, which is what keeps a mouse button bound to the
 * screenshot from also reaching the game's pointer handling. Records the
 * request only; nothing is captured or written from the pump (see the
 * reentrancy rule in host/window.h). */
bool rt_screenshot_on_sdl_event(const SDL_Event& e);

/* Called once a field from rt_gs_vsync_hook (hw/gspriv.cpp), next to
 * rt_settings_apply_pending, which is the one point in a field guaranteed to
 * be between frames. Arms a pending request through the GS backend and, once
 * the backend reports an image ready, pulls the pixels and writes the PNG. */
void rt_screenshot_tick();

/* The same request the hotkey makes, for the menu's "Take screenshot" button
 * (ui/ui_settings_model.cpp). The capture is still taken before the overlay
 * pass, so the menu the user pressed the button in is not in the file. */
void rt_screenshot_request();

#endif /* ICORECOMP_HOST_SCREENSHOT_H */
