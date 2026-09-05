/* host/window.h: the executable's event pump and window control.
 *
 * The window itself lives in host/window_service.h: the exe creates it, owns
 * it and hands it to whichever GS backend presents. This file is the part of
 * that ownership that deals with events and with the window's mode, size and
 * icon; window_service.h is the state and the handles. Neither one talks to
 * a particular GS backend, which is the point: nothing here is
 * paraLLEl-GS-specific any more.
 *
 * Reentrancy: rt_window_pump is called from inside Granite's WSI::begin_frame
 * (see gs_parallel_present.cpp's comment on RtPgs::present_frame) through the
 * nullable RtPgsHost.pump_events, after a swapchain image may already be
 * acquired. From that context this file may only queue/translate events and
 * call the rt_window_notify_* / rt_window_sample_state entry points, whose
 * sink callbacks are flag sets; any GS call that touches a swapchain from
 * inside it is fatal (the paraLLEl-GS library's m_in_frame guard).
 */
#ifndef ICORECOMP_HOST_WINDOW_H
#define ICORECOMP_HOST_WINDOW_H

#include "settings.h"
#include "window_service.h"

#include <cstdint>

/* The process's one and only SDL_PollEvent loop. Routes SDL_EVENT_QUIT to
 * rt_window_notify_quit and the resize events to rt_window_notify_resize
 * (plus recording the new window size into settings when display.
 * remember_window_size is set and the window is not fullscreen; the commit
 * itself runs from rt_window_flush_pending_save, never from the pump, per
 * the reentrancy rule above), and hands every event to the UI
 * (rt_ui_handle_sdl_event). Refreshes the window-state cache
 * (rt_window_sample_state) once the queue is drained, since the GS consumer
 * thread reads that cache instead of calling SDL itself.
 *
 * Called once per field from rt_gs_vsync_hook (hw/gspriv.cpp) and also from
 * inside WSI::begin_frame via pump_events; either caller may see any given
 * event first, harmlessly, since SDL_PollEvent drains the queue. No-op, with
 * no SDL calls compiled in, when there is no window (headless, or a build
 * with no SDL). */
void rt_window_pump();

/* Holds (on = true) and releases (false) SDL event dispatch, counted so the
 * regions may nest. While held, rt_window_pump does not call SDL_PollEvent
 * at all: it runs the platform message loop, peeks for the quit and resize
 * events the presenting backend has to hear about, and refreshes the
 * window-state cache, leaving every event queued for the next unheld pump.
 *
 * It exists for one caller: the GS command ring's producer waits
 * (gs/gs_threaded.cpp) pump so a consumer parked on a minimized window can
 * be restored, and one of those waits -- the reply to an overlay texture
 * upload -- runs from inside RmlUi's Context::Render, where dispatching an
 * event would re-enter RmlUi's own context. Dispatch is held across every
 * one of those waits rather than only that one, because the rule "a wait
 * never dispatches" needs no case analysis to stay true. The cost is that an
 * input event that arrives during a wait is handled at the field boundary
 * instead, which is where it would have been handled anyway. */
void rt_window_hold_event_dispatch(bool on);

/* Commits a recorded remember_window_size change once the resize has been
 * quiet for a second. Called from rt_settings_apply_pending() at the field
 * boundary, the one place guaranteed to be outside WSI::begin_frame,
 * because the commit runs the display applier and that may touch the
 * window. The commit does not write the file itself; it asks the settings
 * layer for a write through rt_settings_request_save(), which is where the
 * runtime's one save debounce lives. */
void rt_window_flush_pending_save();

/* Applies display.mode and window size to the live window: FullscreenDesktop
 * -> borderless desktop fullscreen (the only mode that behaves on Wayland);
 * FullscreenExclusive -> exclusive fullscreen at the desktop mode, falling
 * back to desktop fullscreen (logged) if that fails rather than leaving a
 * broken state; Windowed -> windowed at window_width/window_height. Always
 * followed by rt_window_notify_resize. No-op when there is no window. */
void rt_window_apply_mode(const RtSettings& s);

/* Sets the window's icon from straight-alpha RGBA pixel data (width * height
 * * 4 bytes, row-major from the top): SDL_CreateSurfaceFrom + SDL_
 * SetWindowIcon, which copies the pixels, so the buffer need not outlive
 * the call. `rgba2x`/`w2`/`h2` are an optional higher-resolution alternate
 * image (SDL_AddSurfaceAlternateImage) for a compositor that wants one, for
 * example the Windows taskbar at high DPI; pass rgba2x == nullptr (or
 * w2/h2 == 0) to skip it. Between frames only, like rt_window_apply_mode.
 * No-op with no SDL calls compiled in when this build has no SDL, or when
 * there is no window right now (headless). A failure logs once under
 * "window" and leaves whatever icon the window already had. */
void rt_window_set_icon(const uint8_t* rgba, uint32_t w, uint32_t h, const uint8_t* rgba2x, uint32_t w2,
                        uint32_t h2);

/* Requests a clean shutdown through the same path a closed window takes:
 * logs "exit requested: %s" naming `why`, then calls rt_window_notify_quit,
 * which records the closure and forwards it to the live backend's sink. In
 * game, the field boundary (rt_gs_vsync_hook, hw/gspriv.cpp) sees it at the
 * next vsync and exits on the EE thread through the shutdown sequence every
 * run already exercises: backend_atexit stops and joins the GS worker,
 * drains the ring, reports the stats and tears the device down (wait-idle,
 * pipeline cache write), then rt_audio_shutdown and the log drain. In the
 * launcher, rt_launcher_run's own loop (ui_launcher.cpp) ends through its
 * WINDOW_CLOSED branch instead. Neither rt_log nor rt_window_notify_quit
 * touches a swapchain, so the reentrancy rule at the top of this file does
 * not apply to either call: this is legal from the event pump (SDL_EVENT_QUIT
 * already routes through here) and from a settings-menu or launcher callback
 * queued to the field boundary. When there is no window (headless, or a
 * build with no SDL) there is nothing to notify and no vsync to catch it at,
 * so this calls std::exit(0) itself instead. */
void rt_request_exit(const char* why);

#endif /* ICORECOMP_HOST_WINDOW_H */
