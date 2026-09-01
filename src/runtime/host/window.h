/* host/window.h: the executable's event pump and window control.
 *
 * Event pump inversion (milestone 3 of the settings plan): the exe owns the
 * only SDL_PollEvent loop in the process. The library's SdlWsiPlatform
 * (gs_parallel_lib.cpp) calls back into rt_window_pump through the nullable
 * RtPgsHost.pump_events (gs_parallel_api.h) instead of polling SDL itself.
 *
 * Reentrancy: pump_events is called from inside Granite's WSI::begin_frame
 * (see gs_parallel_lib.cpp's comment on RtPgs::present_frame), after a
 * swapchain image may already be acquired. rt_window_pump must therefore
 * only queue/translate events and call rt_pgs_notify_quit / rt_pgs_notify_
 * resize; any other rt_pgs_* call from inside it is fatal (the library's
 * m_in_frame guard).
 */
#ifndef ICORECOMP_HOST_WINDOW_H
#define ICORECOMP_HOST_WINDOW_H

#include "settings.h"

/* Opaque handle to the live backend, defined in gs_parallel_api.h (the
 * MIT/LGPL C ABI boundary). Forward-declared here rather than pulling that
 * header in for every window.h includer; window.cpp includes it for the
 * rt_pgs_* calls it makes. */
struct RtPgs;

/* The live backend's RtPgs*, or nullptr when there is none (dump backend,
 * headless, or this build has no paraLLEl-GS backend). Defined in
 * gs_parallel.cpp when built with ICORECOMP_HAVE_PARALLEL_GS, and as a
 * nullptr stub in window.cpp otherwise, so callers here and in
 * settings_apply.cpp never need the ICORECOMP_HAVE_PARALLEL_GS guard
 * themselves. */
RtPgs* rt_gs_parallel_handle();

/* The process's one and only SDL_PollEvent loop. Routes SDL_EVENT_QUIT to
 * rt_pgs_notify_quit and the two resize events to rt_pgs_notify_resize (plus
 * recording the new window size into settings when display.
 * remember_window_size is set and the window is not fullscreen; the commit
 * itself runs from rt_window_flush_pending_save, never from the pump, per
 * the reentrancy rule above), then hands every event to the UI
 * (rt_ui_handle_sdl_event). Rebind capture attaches here in a later
 * milestone. Called once per field from rt_gs_vsync_hook (hw/gspriv.cpp)
 * and also from inside WSI::begin_frame via pump_events; either caller may
 * see any given event first, harmlessly, since SDL_PollEvent drains the
 * queue. No-op, with no SDL calls compiled in, when there is no live
 * windowed backend (headless build, or rt_gs_parallel_handle() has no
 * window). */
void rt_window_pump();

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

#endif /* ICORECOMP_HOST_WINDOW_H */
