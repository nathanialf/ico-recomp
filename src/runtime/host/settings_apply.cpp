/* host/settings_apply.cpp: the one place a settings diff becomes subsystem
 * calls (settings.h's rt_settings_apply / rt_settings_apply_pending).
 *
 * Milestone 3 of the settings plan implements the display appliers only;
 * later milestones add audio/input/debug/launcher appliers to the same two
 * functions rather than growing an observer registry (see settings.h).
 *
 * Per-setting classification (display section; the plan's table extended to
 * every field this milestone touches):
 *
 *   display.mode                 hot   rt_window_apply_mode (SDL fullscreen
 *                                       + notify_resize)
 *   display.window_width/height  hot   rt_window_apply_mode, same call
 *   display.fit, display.filter  hot   rt_pgs_set_presentation (only stores;
 *                                       safe any time between frames)
 *   display.present              warm  queued; rt_pgs_set_present_mode
 *                                       touches the swapchain, so it applies
 *                                       from rt_settings_apply_pending() at
 *                                       the field boundary, never here
 *   display.render_scale         warm  queued the same way, applied via
 *                                       rt_pgs_set_render_scale
 *   display.remember_window_size hot   read fresh by host/window.cpp's
 *                                       resize handler on every resize
 *                                       event; nothing to push here
 *   display.show_fps             hot   UI-side only: ui/ui_settings_model.cpp
 *                                       shows or hides the fps.rml document
 *                                       from its own refresh, at the field
 *                                       boundary. Nothing to push from here
 *
 *   gameplay.run_any_direction   hot   read fresh by sdl_poll
 *                                       (host/input.cpp) every field, which
 *                                       reshapes the left stick pair it is
 *                                       about to report; nothing to push here
 *
 *   debug.verbose                hot   rt_log_set_verbose, unless
 *                                       ICORECOMP_VERBOSE is set (log.cpp
 *                                       parses a spec once; it does not
 *                                       poll the struct)
 *
 * Everything else outside display (audio.*, input.*, gameplay.*, the rest
 * of debug.*, launcher.*) needs no applier: those consumers read
 * rt_settings() fresh on every use (pace_period_seconds in gspriv.cpp,
 * sdl_submit in audio.cpp, rt_prof_field in prof.h), and launcher.* is cold
 * for the current run by design. Milestone 6 (the settings menu) added no
 * applier for that reason: every control it exposes outside display.* was
 * already read fresh.
 *
 * Every rt_pgs_* call is guarded on rt_gs_parallel_handle() != nullptr: a
 * dump-only run, a headless live run, or a build with no paraLLEl-GS backend
 * at all all have no window/renderer to push these into, and window.cpp's
 * rt_window_apply_mode / rt_gs_parallel_handle stub already make the hot
 * window path a safe no-op in exactly those cases.
 */
#include "settings.h"

#include "window.h"

#include "../runtime.h"

#ifdef ICORECOMP_HAVE_PARALLEL_GS
#include "../gs/gs_parallel_api.h"
#endif

namespace {

#ifdef ICORECOMP_HAVE_PARALLEL_GS

uint32_t present_to_pgs(RtPresentMode m) {
    switch (m) {
    case RtPresentMode::Fifo: return RT_PGS_PRESENT_FIFO;
    case RtPresentMode::Immediate: return RT_PGS_PRESENT_IMMEDIATE;
    default: return RT_PGS_PRESENT_MAILBOX;
    }
}

uint32_t fit_to_pgs(RtFit f) {
    switch (f) {
    case RtFit::IntegerScale: return RT_PGS_FIT_INTEGER;
    case RtFit::Stretch: return RT_PGS_FIT_STRETCH;
    default: return RT_PGS_FIT_LETTERBOX;
    }
}

uint32_t filter_to_pgs(RtFilter f) {
    return f == RtFilter::Nearest ? RT_PGS_FILTER_NEAREST : RT_PGS_FILTER_LINEAR;
}

/* Warm queue: display.present and display.render_scale touch
 * the swapchain or the GS interface's in-flight state, so they wait for
 * rt_settings_apply_pending() at the field boundary (hw/gspriv.cpp) instead
 * of applying from inside rt_settings_apply, which can run from UI code at
 * an arbitrary point in a field. */
struct PendingApply {
    bool present_mode = false;
    uint32_t present_mode_value = 0;
    bool render_scale = false;
    uint32_t render_scale_factor = 0;
} g_pending;

#endif /* ICORECOMP_HAVE_PARALLEL_GS */

} // namespace

void rt_settings_apply(const RtSettings& before, const RtSettings& now) {
    if (before.debug.verbose != now.debug.verbose) {
        if (rt_settings_overridden("debug.verbose")) {
            rt_log("settings", "settings: debug.verbose changed but ICORECOMP_VERBOSE overrides it"
                               " for this run; not applied");
        } else {
            /* Same rule as main.cpp at startup: an empty spec means "the
             * compiled-in default channels", which is what rt_log_init
             * selected when the env var was unset; rt_log_set_verbose("")
             * would instead clear every channel. */
            if (now.debug.verbose.empty()) {
                rt_log("settings", "settings: debug.verbose cleared; default channels return at next start");
            } else {
                rt_log_set_verbose(now.debug.verbose.c_str());
            }
        }
    }

    const bool window_changed =
        before.display.mode != now.display.mode ||
        before.display.window_width != now.display.window_width ||
        before.display.window_height != now.display.window_height;
    if (window_changed) {
        rt_window_apply_mode(now);
    }

#ifdef ICORECOMP_HAVE_PARALLEL_GS
    RtPgs* pgs = rt_gs_parallel_handle();
    if (pgs && (before.display.fit != now.display.fit || before.display.filter != now.display.filter)) {
        rt_pgs_set_presentation(pgs, fit_to_pgs(now.display.fit), filter_to_pgs(now.display.filter));
    }

    if (before.display.present != now.display.present) {
        if (rt_settings_overridden("display.present")) {
            /* ICORECOMP_GS_PRESENT already decided this at startup
             * (gs_parallel.cpp's resolve_create_options); a settings-file
             * edit must not silently override the environment for the rest
             * of the run. */
            rt_log("settings", "settings: display.present changed but ICORECOMP_GS_PRESENT overrides it"
                                " for this run; not applied");
        } else {
            g_pending.present_mode = true;
            g_pending.present_mode_value = present_to_pgs(now.display.present);
        }
    }

    if (before.display.render_scale != now.display.render_scale) {
        g_pending.render_scale = true;
        g_pending.render_scale_factor = (uint32_t)now.display.render_scale;
    }
#endif
}

void rt_settings_apply_pending() {
    /* Field boundary, outside WSI::begin_frame: the one context where the
     * remember_window_size commit may run, since committing runs the display
     * applier (see window.h), and the one context where the settings file
     * may be written. */
    rt_window_flush_pending_save();
    rt_settings_flush_save_if_due();
#ifdef ICORECOMP_HAVE_PARALLEL_GS
    if (!g_pending.present_mode && !g_pending.render_scale) return;
    RtPgs* pgs = rt_gs_parallel_handle();
    if (!pgs) {
        /* Nothing to apply to (headless or no live backend); drop rather
         * than carry stale requests forward indefinitely. */
        g_pending = {};
        return;
    }
    if (g_pending.present_mode) {
        g_pending.present_mode = false;
        rt_pgs_set_present_mode(pgs, g_pending.present_mode_value);
    }
    if (g_pending.render_scale) {
        g_pending.render_scale = false;
        rt_pgs_set_render_scale(pgs, g_pending.render_scale_factor);
    }
#endif
}
