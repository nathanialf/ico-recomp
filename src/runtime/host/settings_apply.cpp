/* host/settings_apply.cpp: the one place a settings diff becomes subsystem
 * calls (settings.h's rt_settings_apply / rt_settings_apply_pending).
 *
 * Appliers live in these two functions rather than in an observer registry
 * (see settings.h). The display keys came first; debug.* and
 * achievements.* have appliers here now as well. Every key with an applier
 * is in the table below, and the table is the classification: docs
 * (docs/SETTINGS.md) repeat it, this file owns it.
 *
 *   display.mode                 hot   rt_window_apply_mode (SDL fullscreen
 *                                       + notify_resize)
 *   display.window_width/height  hot   rt_window_apply_mode, same call
 *   display.fit, display.filter  hot   GsBackend::set_presentation (only
 *                                       stores; safe any time between frames)
 *   display.raster               hot   GsBackend::set_raster (only stores;
 *                                       the next vsync reads it)
 *
 *   The four hot presentation keys above, plus display.render_scale below,
 *   are pushed by push_presentation() against the words the backend was last
 *   given rather than against the previous settings struct. A load runs no
 *   applier, so a diff against `before` only ever carried an edit: a
 *   settings.json holding display.raster = "crt" reached a backend that read
 *   the settings itself when it was made (paraLLEl-GS does) and no other
 *   (gs/render/gs_native.cpp does not include the settings header at all).
 *   Comparing against the last push covers the load and every later edit in
 *   one place, for whichever backend the run built.
 *   display.widescreen           hot   two halves. The projection factor is
 *                                       pushed straight into
 *                                       guest/widescreen.cpp, which only
 *                                       stores a double, so it is safe from
 *                                       anywhere. The matching present
 *                                       aspect goes to the backend from
 *                                       rt_settings_apply_pending() at the
 *                                       field boundary, because it has to
 *                                       reach the GS command ring's consumer
 *                                       in order with the fields it applies
 *                                       to. Both are recomputed every field
 *                                       boundary as well as on a change,
 *                                       because in `window` mode the factor
 *                                       follows the window and a resize is
 *                                       not a settings change
 *   deinterlace                  hot   GsBackend::set_deinterlace (only
 *                                       stores; the next vsync reads it).
 *                                       Not a settings key any more: the
 *                                       value is bob, and push_presentation
 *                                       hands it over once like the rest
 *   display.render_scale         warm  applied via GsBackend::set_render_scale
 *                                       from rt_settings_apply_pending() at
 *                                       the field boundary, because it
 *                                       resizes the renderer's own buffers.
 *                                       Compared against the last push, like
 *                                       the four hot ones above
 *
 *   The present mode and the present rate are not in this table any more.
 *   Neither is a settings key: the swapchain mode is mailbox unless
 *   ICORECOMP_GS_PRESENT names another (gs/gs_select.cpp reads it once, when
 *   the backend is made), and the window is refreshed once per field, which
 *   is the rate the backend is created with. There is nothing left for an
 *   applier to push
 *   display.remember_window_size hot   read fresh by host/window.cpp's
 *                                       resize handler on every resize
 *                                       event; nothing to push here
 *   display.screenshot_dir       hot   read fresh by host/screenshot.cpp,
 *                                       which re-resolves the folder whenever
 *                                       the value differs from the one it
 *                                       last resolved for; nothing to push
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
 *   debug.log_level              hot   rt_log_set_level, unless
 *                                       ICORECOMP_LOG_LEVEL is set: log.cpp
 *                                       holds the level, it does not read
 *                                       the settings struct. The verbose
 *                                       channel spec has no entry here at
 *                                       all: it is ICORECOMP_VERBOSE, parsed
 *                                       once by rt_log_init, and not a
 *                                       settings key
 *   debug.console                cold  rt_console_init runs once, before
 *                                       rt_log_init, off the peeked value
 *                                       (rt_settings_peek_console). A
 *                                       console cannot be attached to a
 *                                       process that has already decided
 *                                       it has none, so this restarts the
 *                                       program, the same as debug.log_file
 *
 *   achievements.enabled         hot   rt_achievements_configure, pushed as
 *   achievements.toast                    one call together with the
 *   achievements.sound                    always-on progress-bit
 *                                       diagnostic. Applied unconditionally
 *                                       rather than diffed: four
 *                                       assignments into
 *                                       guest/achievements.cpp are cheaper
 *                                       than the comparison would be
 *   audio.chime_volume           hot   no applier: the chime mixer in
 *                                       host/audio.cpp reads it fresh on
 *                                       every submit, the same way
 *                                       audio.master_volume is read
 *   audio.music_volume           hot   no applier either: snd/engine.cpp
 *   audio.effects_volume                 reads all three fresh on every
 *   audio.movie_volume                   rendered chunk, where it sums each
 *                                       category
 *
 * Everything else outside display (audio.*, input.*, gameplay.*, the rest
 * of debug.*, launcher.*) needs no applier: those consumers read
 * rt_settings() fresh on every use (pace_period_seconds in gspriv.cpp,
 * sdl_submit in audio.cpp, rt_prof_field in prof.h), and launcher.* is cold
 * for the current run by design. Two keys are cold and restart the program
 * when they are changed from the menu: debug.console above and
 * debug.log_file (the sink is opened once, off rt_settings_peek_boot).
 * Neither has an applier, because there is nothing to push to: the consumer
 * read the value at process start. display.backend was a third until it was
 * retired on 2026-09-05. launcher.* is the one cold family that restarts
 * nothing, because it only decides what the next launch's launcher does
 * (host/settings.h). Milestone 6 (the settings menu) added no
 * applier for that reason: every control it exposes outside display.* was
 * already read fresh.
 *
 * The hot display calls (set_presentation, set_raster, set_deinterlace) and
 * the warm one (set_render_scale) go through the GS backend
 * (gs/gs_backend.h) rather than calling a particular renderer directly,
 * which is what they used to do.
 * The backend is a command ring with a worker thread on the far end
 * (gs/gs_threaded.cpp, shipped in 9bbda6b): a call that skips it would be
 * invisible to the ring and so would arrive out of order against the GIF
 * and priv traffic it has to stay ordered against. The dump backend's
 * defaults are no-ops, so a dump-only run behaves as it did when the guard
 * below excluded it.
 *
 * The guard is rt_gs_backend_if_created() != nullptr rather than
 * rt_gs_backend(): a settings change that lands before rt_hw_init() has
 * nothing to push into, and this file must not be what builds the backend
 * (see gs_backend.h). window.cpp's rt_window_apply_mode already makes the
 * hot window path a safe no-op when there is no window.
 */
#include "settings.h"

#include "window.h"

#include "../gs/gs_backend.h"
#include "../guest/achievements.h"
#include "../guest/widescreen.h"
#include "../runtime.h"

/* The RT_PGS_* presentation constants are the vocabulary GsBackend speaks
 * (gs/gs_backend.h), whichever renderer is behind it, so this header is
 * included whether or not the paraLLEl-GS library is built. It declares no
 * definitions of its own and nothing here calls into the library. */
#include "../gs/gs_parallel_api.h"

namespace {

uint32_t fit_to_pgs(RtFit f) {
    switch (f) {
    case RtFit::IntegerScale: return RT_PGS_FIT_INTEGER;
    case RtFit::Stretch: return RT_PGS_FIT_STRETCH;
    default: return RT_PGS_FIT_LETTERBOX;
    }
}

uint32_t raster_to_pgs(RtRaster r) {
    return r == RtRaster::Window ? RT_PGS_RASTER_WINDOW : RT_PGS_RASTER_CRT;
}

uint32_t deinterlace_to_pgs(RtDeinterlace d) {
    switch (d) {
    case RtDeinterlace::Bob: return RT_PGS_DEINTERLACE_BOB;
    case RtDeinterlace::Weave: return RT_PGS_DEINTERLACE_WEAVE;
    case RtDeinterlace::Adaptive: return RT_PGS_DEINTERLACE_ADAPTIVE;
    default: return RT_PGS_DEINTERLACE_BOB;
    }
}

uint32_t filter_to_pgs(RtFilter f) {
    return f == RtFilter::Nearest ? RT_PGS_FILTER_NEAREST : RT_PGS_FILTER_LINEAR;
}

/* The four presentation words last handed to the backend, and whether any
 * have been handed over at all.
 *
 * A load runs no applier, and rt_settings_apply only pushes what moved, so a value that
 * arrived in settings.json and was never edited reached the backend only if
 * the backend read the settings itself when it was made. paraLLEl-GS does
 * (gs/gs_parallel.cpp resolve_create_options); the native renderer
 * (gs/render/gs_native.cpp) does not include the settings header at all, so
 * a file holding display.raster = "crt" or display.render_scale = 8 was
 * ignored on that backend for the whole run unless the user opened the menu
 * and changed the key.
 *
 * Pushing against what was last pushed, rather than against the previous
 * settings struct, covers the load as well as every later edit and stays one
 * comparison per field once it has settled. `valid` is false until the
 * backend exists, which is what makes the first field after rt_hw_init the
 * one that pushes. */
struct PushedPresentation {
    bool valid = false;
    uint32_t fit = 0;
    uint32_t filter = 0;
    uint32_t raster = 0;
    uint32_t deinterlace = 0;
    uint32_t render_scale = 0;
} g_pushed;

/* The widescreen present aspect last handed to the backend. -1.0 for the
 * same reason: no aspect is, so the first push always happens. */
double g_pushed_widescreen_aspect = -1.0;

/* Hands the backend every presentation word that differs from the one it was
 * last given, and all of them the first time. `warm` also covers
 * display.render_scale, which resizes the renderer's own buffers and so may
 * only run at a field boundary; the four hot ones store a word and are safe
 * from anywhere between frames, which is why rt_settings_apply calls this
 * with warm false and rt_settings_apply_pending with warm true.
 *
 * g_pushed.valid is set only once render_scale has gone over as well, so a
 * hot commit that lands before the first field boundary does not leave the
 * warm half looking already pushed. */
void push_presentation(GsBackend* gs, const RtSettings& s, bool warm) {
    const uint32_t fit = fit_to_pgs(s.display.fit);
    const uint32_t filter = filter_to_pgs(s.display.filter);
    const uint32_t raster = raster_to_pgs(s.display.raster);
    const uint32_t deinterlace = deinterlace_to_pgs(s.display.deinterlace);
    const bool first = !g_pushed.valid;

    if (first || fit != g_pushed.fit || filter != g_pushed.filter) {
        g_pushed.fit = fit;
        g_pushed.filter = filter;
        gs->set_presentation(fit, filter);
    }
    if (first || raster != g_pushed.raster) {
        g_pushed.raster = raster;
        gs->set_raster(raster);
    }
    if (first || deinterlace != g_pushed.deinterlace) {
        g_pushed.deinterlace = deinterlace;
        gs->set_deinterlace(deinterlace);
    }
    if (!warm) return;

    const uint32_t scale = (uint32_t)s.display.render_scale;
    if (first || scale != g_pushed.render_scale) {
        g_pushed.render_scale = scale;
        gs->set_render_scale(scale);
    }
    g_pushed.valid = true;
}

/* ---- display.widescreen ---------------------------------------------------
 *
 * The setting's own value is a mode, not a number: the number the guest
 * projection is scaled by depends on the shape of the window as well, and in
 * `window` mode the window moves without any settings key moving. So the
 * factor is derived here from both, every time this runs, and
 * rt_widescreen_set_factor logs only when the result actually changes.
 *
 * k = (4/3) / target aspect. 4:3 is the aspect the game's own projection was
 * built for, so k is 1 at 4:3, 0.75 at 16:9, and above 1 for a window
 * narrower than 4:3 (which narrows the frustum rather than widening it, and
 * is the honest answer for that window rather than a case to refuse). */
const char* widescreen_mode_name(RtWidescreen w) {
    switch (w) {
    case RtWidescreen::Window: return "window";
    case RtWidescreen::SixteenNine: return "16_9";
    default: return "off";
    }
}

/* The aspect of the surface the picture is presented into. 0 when there is
 * no window to ask (headless, or before the backend exists), in which case
 * the configured window size is the best answer available and is what the
 * window will open at. */
double presentation_aspect(const RtSettings& s) {
    uint32_t w = 0, h = 0;
    rt_window_surface_size(&w, &h);
    if (w == 0 || h == 0) {
        w = (uint32_t)(s.display.window_width > 0 ? s.display.window_width : 0);
        h = (uint32_t)(s.display.window_height > 0 ? s.display.window_height : 0);
    }
    if (w == 0 || h == 0) return 0.0;
    return double(w) / double(h);
}

void refresh_achievements(const RtSettings& s) {
    /* The progress-bit diagnostic is always on and is not a setting: it is
     * one info line per progress-bit transition and per layout id change,
     * which is the log that resolves the trophy bit table and is what a
     * user is asked for when a trophy does not fire. See
     * guest/achievements.cpp and docs/ACHIEVEMENTS.md. */
    rt_achievements_configure(s.achievements.enabled, s.achievements.toast,
        s.achievements.sound, /*log_progress_bits=*/true);
}

void refresh_widescreen(const RtSettings& s) {
    const RtWidescreen mode = s.display.widescreen;
    double k = 1.0;
    if (mode == RtWidescreen::SixteenNine) {
        k = (4.0 / 3.0) / (16.0 / 9.0);
    } else if (mode == RtWidescreen::Window) {
        const double aspect = presentation_aspect(s);
        /* No window and no configured size: nothing to derive a factor from,
         * so the projection is left as the game wrote it until there is. */
        if (aspect > 0.0) k = (4.0 / 3.0) / aspect;
    }
    rt_widescreen_set_factor(k, widescreen_mode_name(mode));
}

} // namespace

void rt_settings_apply_loaded() {
    refresh_achievements(rt_settings());
}

void rt_settings_apply(const RtSettings& before, const RtSettings& now) {
    /* Unconditional, like refresh_widescreen below: four assignments into
     * guest/achievements.cpp, cheaper than the diff would be. */
    refresh_achievements(now);

    /* There is no display.backend applier any more either: the key was
     * retired on 2026-09-05 and RtSettings carries no field for it. What
     * resolves a backend is gs/gs_select.cpp, once, off
     * ICORECOMP_GS_BACKEND. */

    /* There is no debug.verbose applier any more: the channel spec is not a
     * setting, it is ICORECOMP_VERBOSE, which log.cpp parses once in
     * rt_log_init. Nothing here can change it mid-run. */

    if (before.debug.log_level != now.debug.log_level) {
        if (rt_settings_overridden("debug.log_level")) {
            rt_log_warn("settings", "settings: debug.log_level changed but ICORECOMP_LOG_LEVEL"
                                    " overrides it for this run; not applied");
        } else {
            rt_log_set_level(now.debug.log_level);
            /* Logged at the level it just became, so the line is visible
             * exactly when the new level would show it. */
            rt_log_info("settings", "settings: debug.log_level = %s",
                rt_log_level_name(now.debug.log_level));
        }
    }

    /* Unconditional rather than diffed: in `window` mode the factor depends
     * on the window as well as on the key, and rt_widescreen_set_factor is a
     * comparison and an assignment when nothing moved. `before` is still
     * read, for the one line that says the key itself changed. */
    if (before.display.widescreen != now.display.widescreen) {
        rt_log_info("settings", "settings: display.widescreen = %s",
            widescreen_mode_name(now.display.widescreen));
    }
    refresh_widescreen(now);

    const bool window_changed =
        before.display.mode != now.display.mode ||
        before.display.window_width != now.display.window_width ||
        before.display.window_height != now.display.window_height;
    if (window_changed) {
        rt_window_apply_mode(now);
    }

    /* Against what the backend was last given rather than against `before`:
     * the two agree for every edit, and only the first form also covers the
     * value a load left in the struct with no applier behind it. */
    if (GsBackend* gs = rt_gs_backend_if_created()) {
        push_presentation(gs, now, /*warm=*/false);
    }

    /* Nothing is queued here any more. display.render_scale is compared in
     * rt_settings_apply_pending() against the one it last pushed, which
     * covers the startup push as well as every later change, and the
     * present mode is not a settings key: gs/gs_select.cpp resolved it once,
     * from ICORECOMP_GS_PRESENT or the compiled-in mailbox, when the backend
     * was made. */
}

void rt_settings_apply_pending() {
    /* Field boundary, outside WSI::begin_frame: the one context where the
     * remember_window_size commit may run, since committing runs the display
     * applier (see window.h), and the one context where the settings file
     * may be written. */
    rt_window_flush_pending_save();
    rt_settings_flush_save_if_due();
    /* A resize is not a settings change, so `window` mode has to be
     * re-derived here as well as on a commit. Cheap: a surface-size query
     * and a comparison. */
    refresh_widescreen(rt_settings());
    const double want_aspect = rt_widescreen_target_aspect();
    const bool aspect_changed = want_aspect != g_pushed_widescreen_aspect;
    GsBackend* gs = rt_gs_backend_if_created();
    if (!gs) {
        /* Nothing to apply to (the backend does not exist yet). g_pushed
         * keeps its "nothing pushed yet" state, which is what makes the
         * first field after the backend is made hand it everything the
         * file holds. */
        return;
    }
    /* Every field, not only on a change: this is the one call that reaches
     * a backend which never read the settings itself, and it is five
     * comparisons once it has settled. */
    push_presentation(gs, rt_settings(), /*warm=*/true);
    if (aspect_changed) {
        g_pushed_widescreen_aspect = want_aspect;
        gs->set_widescreen_aspect(want_aspect);
    }
    /* No present rate is pushed: the backend is created presenting one
     * finished field at a time, which is what every shipped run does, and
     * nothing can move it. GsBackend::set_present_rate stays on the
     * interface for the command ring's own selftest. */
}
