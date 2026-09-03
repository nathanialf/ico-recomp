/* host/window.h: the executable's event pump and window control.
 *
 * Event pump inversion (milestone 3 of the settings plan): the exe owns the
 * only SDL_PollEvent loop in the process. The library's SdlWsiPlatform
 * (gs_parallel_impl.h) calls back into rt_window_pump through the nullable
 * RtPgsHost.pump_events (gs_parallel_api.h) instead of polling SDL itself.
 *
 * Reentrancy: pump_events is called from inside Granite's WSI::begin_frame
 * (see gs_parallel_present.cpp's comment on RtPgs::present_frame), after a
 * swapchain image may already be acquired. rt_window_pump must therefore
 * only queue/translate events and call rt_pgs_notify_quit / rt_pgs_notify_
 * resize; any other rt_pgs_* call from inside it is fatal (the library's
 * m_in_frame guard).
 */
#ifndef ICORECOMP_HOST_WINDOW_H
#define ICORECOMP_HOST_WINDOW_H

#include "settings.h"

#include <cstdint>

/* Opaque handle to the live backend, defined in gs_parallel_api.h (the
 * MIT/LGPL C ABI boundary). Forward-declared here rather than pulling that
 * header in for every window.h includer; window.cpp includes it for the
 * rt_pgs_* calls it makes.
 *
 * Everything that changes what the GS renders or presents now goes through
 * GsBackend (gs/gs_backend.h) so the GS command ring in gs/gs_threaded.cpp
 * sees it in order. The four calls window.cpp still makes on this handle
 * are the exceptions, and stay that way: rt_pgs_window_handle is a pointer
 * read; rt_pgs_notify_quit and rt_pgs_notify_resize set a flag the library
 * reads at its next frame; and rt_pgs_sample_window_state copies the
 * window's pixel size and minimized state into the library's cache, which
 * only this thread can read out of SDL and only the GS worker thread needs.
 * None of them is a GS command, none may block, and the reentrancy contract
 * above requires them to work from inside WSI::begin_frame, which is exactly
 * where a ring record could not be drained. */
struct RtPgs;

/* The live backend's RtPgs*, or nullptr when there is none (dump backend,
 * headless, or this build has no paraLLEl-GS backend). Defined in
 * gs_parallel.cpp when built with ICORECOMP_HAVE_PARALLEL_GS, and as a
 * nullptr stub in window.cpp otherwise, so callers here and in
 * settings_apply.cpp never need the ICORECOMP_HAVE_PARALLEL_GS guard
 * themselves. */
RtPgs* rt_gs_parallel_handle();

/* The RT_PGS_PRESENT_* value resolve_create_options() settled on at startup
 * (ICORECOMP_GS_PRESENT if set, otherwise display.present). The launcher
 * forces FIFO while it is up and puts this value back at hand-off, rather
 * than deriving the mapping a second time. RT_PGS_PRESENT_MAILBOX (0) in a
 * build with no paraLLEl-GS backend, where nothing consumes it. */
uint32_t rt_gs_parallel_present_mode();

/* The process's one and only SDL_PollEvent loop. Routes SDL_EVENT_QUIT to
 * rt_pgs_notify_quit and the two resize events to rt_pgs_notify_resize (plus
 * recording the new window size into settings when display.
 * remember_window_size is set and the window is not fullscreen; the commit
 * itself runs from rt_window_flush_pending_save, never from the pump, per
 * the reentrancy rule above), and hands every event to the UI
 * (rt_ui_handle_sdl_event). Rebind capture attaches here in a later
 * milestone. Refreshes the library's window-state cache
 * (rt_pgs_sample_window_state) once the queue is drained, since the GS
 * worker thread reads that cache instead of calling SDL itself.
 * Called once per field from rt_gs_vsync_hook (hw/gspriv.cpp)
 * and also from inside WSI::begin_frame via pump_events; either caller may
 * see any given event first, harmlessly, since SDL_PollEvent drains the
 * queue. No-op, with no SDL calls compiled in, when there is no live
 * windowed backend (headless build, or rt_gs_parallel_handle() has no
 * window). */
void rt_window_pump();

/* Holds (on = true) and releases (false) SDL event dispatch, counted so the
 * regions may nest. While held, rt_window_pump does not call SDL_PollEvent
 * at all: it runs the platform message loop, peeks for the quit and resize
 * events the GS library has to hear about, and refreshes the window-state
 * cache, leaving every event queued for the next unheld pump.
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
 * followed by rt_pgs_notify_resize. No-op when headless. */
void rt_window_apply_mode(const RtSettings& s);

/* Sets the window's icon from straight-alpha RGBA pixel data (width * height
 * * 4 bytes, row-major from the top): SDL_CreateSurfaceFrom + SDL_
 * SetWindowIcon, which copies the pixels, so the buffer need not outlive
 * the call. `rgba2x`/`w2`/`h2` are an optional higher-resolution alternate
 * image (SDL_AddSurfaceAlternateImage) for a compositor that wants one, for
 * example the Windows taskbar at high DPI; pass rgba2x == nullptr (or
 * w2/h2 == 0) to skip it. Between frames only, like rt_window_apply_mode.
 * No-op with no SDL calls compiled in when this build has no live windowed
 * backend, or when there is no window right now (headless). A failure logs
 * once under "window" and leaves whatever icon the window already had. */
void rt_window_set_icon(const uint8_t* rgba, uint32_t w, uint32_t h, const uint8_t* rgba2x, uint32_t w2,
                        uint32_t h2);

/* Requests a clean shutdown through the same path a closed window takes:
 * logs "exit requested: %s" naming `why`, then calls rt_pgs_notify_quit on
 * the live backend, which records the closure. In game, the field boundary
 * (rt_gs_vsync_hook, hw/gspriv.cpp) sees it at the next vsync and exits on
 * the EE thread through the shutdown sequence every run already exercises:
 * backend_atexit stops and joins the GS worker, drains the ring, reports the
 * stats and tears the device down (wait-idle, pipeline cache write), then
 * rt_audio_shutdown and the log drain. In the launcher, rt_launcher_run's
 * own loop (ui_launcher.cpp) ends through its WINDOW_CLOSED branch instead.
 * Neither rt_log nor rt_pgs_notify_quit is an rt_pgs_set_* entry point, so
 * the reentrancy rule at the top of this file does not apply to either
 * call: this is legal from the event pump (SDL_EVENT_QUIT already routes
 * through here) and from a settings-menu or launcher callback queued to the
 * field boundary. When there is no
 * live window (headless, or a build with no paraLLEl-GS backend) there is
 * nothing to notify and no vsync to catch it at, so this calls std::exit(0)
 * itself instead. */
void rt_request_exit(const char* why);

#endif /* ICORECOMP_HOST_WINDOW_H */
