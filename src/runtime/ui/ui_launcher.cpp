/* ui/ui_launcher.cpp: the launcher's data model, its documents and its own
 * frame loop.
 *
 * The launcher runs before the game exists: main.cpp brings up memory and
 * the GS backend, then hands control here, and only after Start does it load
 * the config and the boot ELF (see the sequence comment in main.cpp). So
 * this file owns the only frame loop in the process that is not the
 * scheduler's, and it drives it with the same four calls rt_gs_vsync_hook
 * makes per field: pump, apply pending settings, tick the UI, present.
 *
 * Two rules carried over from the rest of the module (ui.h):
 *   - the rt_pgs_* wrappers (backend_present_ui, backend_set_present_mode)
 *     are between-frames-only. The loop below calls them from its own body,
 *     never from an event callback, because an event callback can run inside
 *     Granite's WSI::begin_frame via the pump_events inversion.
 *   - a data-model callback therefore only sets a flag; the work happens in
 *     launcher_tick(), at the top of the next loop iteration. That is the
 *     same queueing rule, and for the same two reasons, as
 *     ui_settings_model.cpp's file comment spells out.
 *
 * Present mode: the launcher forces FIFO while it is up. Mailbox spins a
 * core presenting frames nothing is producing (there is no pace_field here,
 * the guest clock is not running), and FIFO is what paces this loop. The
 * user's mode goes back at hand-off, taken from
 * rt_gs_parallel_present_mode() rather than re-derived, so
 * ICORECOMP_GS_PRESENT still wins exactly as it did at startup.
 */
#include "ui.h"

#ifdef ICORECOMP_UI

#include "ui_internal.h"

#include "../host/portable.h"
#include "../host/settings.h"
#include "../host/window.h"
#include "../iso/iso9660.h"
#include "../runtime.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#ifdef ICORECOMP_PGS_SDL
#include <SDL3/SDL.h>
#endif

namespace rtui {

namespace {

/* ---- the model ----------------------------------------------------------
 *
 * Its own data model, "launcher", not an extension of "settings": only one
 * of these fields (show_at_startup) is a setting. The rest is the state of
 * this run's disc and the boot precheck, which nothing outside the launcher
 * has any use for.
 */
struct LauncherModel {
    std::string disc_path = "no disc image found";
    std::string disc_source;
    bool disc_ok = false;

    /* The boot precheck's message, and whether there is one. The bool is
     * separate for the reason ui_settings_model.cpp's rebind_status is: an
     * Rml data expression coerces a String to bool through Variant's string
     * parse, so a sentence reads as false. */
    std::string precheck_error;
    bool has_precheck_error = false;
    bool can_start = false;

    bool show_at_startup = true;

    /* Browse is off when this build has no SDL and when the platform has no
     * portal or zenity to show a dialog; the text field is the fallback in
     * both cases. disc_locked is the stronger one: --disc named the image
     * for this run, so nothing in the window may pick another. */
    bool browse_available = false;
    bool disc_locked = false;
    std::string path_entry;

    /* One line of status: what the last action did, or why it could not. */
    std::string status;
    bool has_status = false;

    std::string version_line;
};

LauncherModel g_m;
Rml::DataModelHandle g_model;
bool g_model_valid = false;

/* Queued by the callbacks, drained by launcher_tick(). */
bool g_start_pending = false;
bool g_quit_pending = false;
bool g_startup_flag_pending = false;
bool g_clear_disc_pending = false;
bool g_precheck_pending = false;
/* A path to validate and mount: from the file dialog's callback or from the
 * text field. */
bool g_mount_pending = false;
std::string g_mount_path;

/* Set once the loop is allowed to end, and how. */
bool g_done = false;
bool g_done_started = false;

/* --disc: latched at init, because a run started with an explicit image
 * boots that image or reports why it cannot. */
bool g_disc_forced = false;

/* Set by everything that writes the model, consumed by launcher_tick. The
 * bindings are not dirtied every frame on purpose: re-setting a bound text
 * field from the model while the user is typing in it would take the edit
 * back. */
bool g_model_dirty = true;

void set_status(const std::string& text) {
    g_m.status = text;
    g_m.has_status = !text.empty();
    g_model_dirty = true;
}

/* ---- precheck ----------------------------------------------------------- */

/* Runs the boot precheck (config, disc mount, SCUS_971.13, SHA-1 pins, entry
 * lookup) and fills the whole disc half of the model from the result. Reads
 * and hashes about a megabyte off the disc, so it runs on entry and after a
 * disc change, never per frame. Called only from launcher_tick(), which is
 * the launcher's field boundary. */
void refresh_precheck() {
    char err[1024];
    const bool ok = rt_boot_precheck(err, sizeof(err));

    g_m.can_start = ok;
    g_m.precheck_error = ok ? std::string() : std::string(err);
    g_m.has_precheck_error = !g_m.precheck_error.empty();

    g_m.disc_ok = rt_iso_mounted();
    const char* path = rt_iso_mounted_path();
    const char* source = rt_iso_mounted_source();
    g_m.disc_path = path[0] ? path : "no disc image found";
    if (source[0]) {
        g_m.disc_source = source;
    } else {
        g_m.disc_source = "searched --disc, settings.json, config/local.toml and the folder"
                          " next to the executable";
    }
    g_model_dirty = true;
    rt_log("launcher", "boot precheck: %s (disc '%s', %s)", ok ? "ready" : err,
        g_m.disc_path.c_str(), g_m.disc_source.c_str());
}

/* Validates one path, mounts it, and on success saves it as
 * launcher.disc_path. On failure the previous disc is put back: rt_iso_try_
 * mount unmounts whatever was mounted before it even opens the candidate,
 * so without the re-probe a bad pick would silently lose a working disc. */
void mount_chosen_path(const std::string& path) {
    char err[1024];
    if (!rt_iso_try_mount(path.c_str(), err, sizeof(err))) {
        rt_log("launcher", "disc '%s' rejected: %s", path.c_str(), err);
        set_status(err);
        char probe_err[1024];
        if (!rt_iso_probe_mount(probe_err, sizeof(probe_err))) {
            rt_log("launcher", "the previous disc did not come back either: %s", probe_err);
        }
        refresh_precheck();
        return;
    }

    /* Absolute, because settings.json's disc_path is resolved against
     * rt_base_dir() and the dialog's result is relative to nothing in
     * particular. weakly_canonical keeps the path usable when a component
     * is a symlink; on error the path is stored as given rather than
     * silently altered. */
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    const std::string stored = ec ? path : abs.string();

    rt_settings_mutable().launcher.disc_path = stored;
    rt_settings_commit(false);
    rt_settings_request_save();
    rt_log("launcher", "disc set to '%s'; saved as settings.json launcher.disc_path", stored.c_str());
    set_status("disc image accepted and saved");
    refresh_precheck();
}

/* ---- file dialog -------------------------------------------------------- */

#ifdef ICORECOMP_PGS_SDL

/* SDL calls this from inside SDL_PumpEvents (SDL_dialog.c posts the result
 * back to the thread that runs the event loop), which for this process means
 * from inside rt_window_pump, which itself can run from inside
 * WSI::begin_frame. So it does the least it can: copy the path and set a
 * flag. Everything the choice implies (mounting, committing settings,
 * re-running the precheck) happens in launcher_tick().
 *
 * filelist NULL is the error case, including "there is no portal and no
 * zenity on this machine": SDL_GetError() then says so and the launcher
 * falls back to its text field. filelist[0] NULL is a plain cancel. */
void SDLCALL dialog_callback(void* /*userdata*/, const char* const* filelist, int /*filter*/) {
    if (!filelist) {
        const char* e = SDL_GetError();
        rt_log("launcher", "the file dialog could not be shown: %s", e && e[0] ? e : "unknown reason");
        g_m.browse_available = false;
        set_status(e && e[0] ? std::string("no file dialog on this system: ") + e
                             : std::string("no file dialog on this system"));
        return;
    }
    if (!filelist[0]) return; /* cancelled */
    g_mount_path = filelist[0];
    g_mount_pending = true;
}

void show_open_dialog() {
    SDL_Window* win = (SDL_Window*)backend_window_handle();
    /* One filter, the two container formats iso9660.cpp probes: a plain
     * 2048-byte .iso and the .bin of a bin/cue rip. */
    static const SDL_DialogFileFilter kFilters[] = {{"Disc images", "iso;bin"}};

    std::string location = rt_base_dir();
    if (g_m.disc_ok) {
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(g_m.disc_path).parent_path();
        if (!parent.empty() && std::filesystem::is_directory(parent, ec)) location = parent.string();
    }
    rt_log("launcher", "opening the file dialog at '%s'", location.c_str());
    SDL_ShowOpenFileDialog(dialog_callback, nullptr, win, kFilters, 1, location.c_str(), false);
}

void open_url(const std::string& url) {
    if (!SDL_OpenURL(url.c_str())) {
        rt_log("launcher", "could not open '%s': %s", url.c_str(), SDL_GetError());
        set_status("could not open " + url + " in a browser");
    }
}

#else /* !ICORECOMP_PGS_SDL */

/* No SDL in this build: no dialog to show and no browser to hand a URL to.
 * Both say so rather than doing nothing. */
void show_open_dialog() {
    rt_log("launcher", "this build has no SDL, so there is no file dialog; use the path field");
    set_status("this build has no file dialog; type a path instead");
}

void open_url(const std::string& url) {
    rt_log("launcher", "this build has no SDL, so '%s' cannot be opened", url.c_str());
    set_status("this build cannot open " + url);
}

#endif /* ICORECOMP_PGS_SDL */

/* ---- data model callbacks -----------------------------------------------
 *
 * Flags only; launcher_tick() does the work. open_settings is the one
 * exception, and it is the same call the menu hotkey makes from the same
 * context (ui_events.cpp): showing a document touches nothing the
 * reentrancy rule protects.
 */

void on_start(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { g_start_pending = true; }
void on_quit(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { g_quit_pending = true; }

void on_open_settings(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    rt_ui_set_visible(true);
}

void on_open_credits(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    if (g_ui.credits) g_ui.credits->Show();
}

void on_browse(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    if (!g_m.browse_available) return;
    show_open_dialog();
}

void on_use_path(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    if (g_disc_forced) return;
    if (g_m.path_entry.empty()) {
        set_status("type the path to a disc image first");
        return;
    }
    g_mount_path = g_m.path_entry;
    g_mount_pending = true;
}

void on_clear_disc(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    if (g_disc_forced) return;
    g_clear_disc_pending = true;
}

void on_startup_change(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    g_startup_flag_pending = true;
}

void on_open_url(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments) {
    if (arguments.size() != 1) {
        rt_log("launcher", "open_url() wants exactly one argument; the document passed %zu",
            arguments.size());
        return;
    }
    open_url(arguments[0].Get<Rml::String>());
}

/* ---- plain click listeners ----------------------------------------------
 *
 * credits.rml carries no data model (there is nothing in it to bind), and
 * the menu's About pane is on the "settings" model, which has no business
 * knowing about URLs. Both get their two clickable elements wired here
 * instead.
 */
class ClickAction final : public Rml::EventListener {
public:
    enum class What { OpenUrl, HideCredits };
    ClickAction(What what, const char* argument) : m_what(what), m_argument(argument ? argument : "") {}

    void ProcessEvent(Rml::Event&) override {
        switch (m_what) {
        case What::OpenUrl: open_url(m_argument); break;
        case What::HideCredits:
            if (g_ui.credits) g_ui.credits->Hide();
            break;
        }
    }

private:
    What m_what;
    std::string m_argument;
};

const char kSiteUrl[] = "https://defnf.com";

ClickAction g_open_site(ClickAction::What::OpenUrl, kSiteUrl);
ClickAction g_hide_credits(ClickAction::What::HideCredits, nullptr);

/* Missing ids are logged rather than ignored: they mean the document and
 * this file disagree, which costs the user a dead-looking link. */
void attach_click(Rml::ElementDocument* doc, const char* id, ClickAction* action) {
    if (!doc) return;
    Rml::Element* el = doc->GetElementById(id);
    if (!el) {
        rt_log("ui", "launcher: no element '%s' in %s; that click does nothing",
            id, doc->GetSourceURL().c_str());
        return;
    }
    el->AddEventListener(Rml::EventId::Click, action);
}

} // namespace

/* ---- init --------------------------------------------------------------- */

bool launcher_init(Rml::Context* context, const std::string& ui_dir) {
    Rml::DataModelConstructor c = context->CreateDataModel("launcher");
    if (!c) {
        rt_log("ui", "Context::CreateDataModel(\"launcher\") failed; the launcher is disabled");
        return false;
    }

    c.Bind("disc_path", &g_m.disc_path);
    c.Bind("disc_source", &g_m.disc_source);
    c.Bind("disc_ok", &g_m.disc_ok);
    c.Bind("precheck_error", &g_m.precheck_error);
    c.Bind("has_precheck_error", &g_m.has_precheck_error);
    c.Bind("can_start", &g_m.can_start);
    c.Bind("show_at_startup", &g_m.show_at_startup);
    c.Bind("browse_available", &g_m.browse_available);
    c.Bind("disc_locked", &g_m.disc_locked);
    c.Bind("path_entry", &g_m.path_entry);
    c.Bind("status", &g_m.status);
    c.Bind("has_status", &g_m.has_status);
    c.Bind("version_line", &g_m.version_line);

    c.BindEventCallback("start", on_start);
    c.BindEventCallback("quit", on_quit);
    c.BindEventCallback("open_settings", on_open_settings);
    c.BindEventCallback("open_credits", on_open_credits);
    c.BindEventCallback("browse", on_browse);
    c.BindEventCallback("use_path", on_use_path);
    c.BindEventCallback("clear_disc", on_clear_disc);
    c.BindEventCallback("startup_changed", on_startup_change);
    c.BindEventCallback("open_url", on_open_url);

    g_model = c.GetModelHandle();
    g_model_valid = true;

    g_disc_forced = rt_iso_forced_path()[0] != 0;
    g_m.disc_locked = g_disc_forced;
#ifdef ICORECOMP_PGS_SDL
    /* Whether a dialog can actually be shown is only known when one is
     * asked for: SDL reports "no portal, no zenity" through the callback,
     * not up front. Until then the button is offered. */
    g_m.browse_available = !g_disc_forced;
#else
    g_m.browse_available = false;
#endif
    g_m.show_at_startup = rt_settings().launcher.show_at_startup;
    g_m.version_line = std::string("icorecomp  ") + rt_exe_identity();
    if (g_disc_forced) {
        set_status("disc set on the command line");
    }

    const std::string launcher_path = ui_dir + "/launcher.rml";
    g_ui.launcher = context->LoadDocument(launcher_path);
    if (!g_ui.launcher) {
        rt_log("ui", "document %s failed to load; the launcher is disabled", launcher_path.c_str());
        return false;
    }

    /* Loaded once here, shown and hidden from the launcher and never
     * reloaded: a second LoadDocument would leave two copies in the
     * context. Failing to load it costs the credits screen, not the
     * launcher. */
    const std::string credits_path = ui_dir + "/credits.rml";
    g_ui.credits = context->LoadDocument(credits_path);
    if (!g_ui.credits) {
        rt_log("ui", "document %s failed to load; the credits screen is unavailable",
            credits_path.c_str());
    }

    attach_click(g_ui.credits, "credits-url", &g_open_site);
    attach_click(g_ui.credits, "credits-back", &g_hide_credits);
    /* The same link in the menu's About pane, which is reachable while the
     * game is running and never goes through credits.rml. */
    attach_click(g_ui.menu, "about-url", &g_open_site);

    rt_log("ui", "launcher documents loaded: %s%s", launcher_path.c_str(),
        g_ui.credits ? ", credits.rml" : "");
    return true;
}

namespace {

/* ---- the field-boundary half -------------------------------------------- */

void launcher_tick() {
    if (!g_model_valid) return;

    if (g_startup_flag_pending) {
        g_startup_flag_pending = false;
        rt_settings_mutable().launcher.show_at_startup = g_m.show_at_startup;
        rt_settings_commit(false);
        rt_settings_request_save();
        rt_log("launcher", "launcher.show_at_startup = %s", g_m.show_at_startup ? "true" : "false");
        /* commit_validate may have reverted it; show what was kept. */
        g_m.show_at_startup = rt_settings().launcher.show_at_startup;
        g_model_dirty = true;
    }

    if (g_clear_disc_pending) {
        g_clear_disc_pending = false;
        rt_settings_mutable().launcher.disc_path.clear();
        rt_settings_commit(false);
        rt_settings_request_save();
        rt_log("launcher", "launcher.disc_path cleared");
        set_status("saved disc path forgotten");
        /* The mount stays as it is: clearing the setting is about what the
         * next run looks for. The precheck re-runs so the screen shows the
         * source the disc would now be found through. */
        g_precheck_pending = true;
    }

    if (g_mount_pending) {
        g_mount_pending = false;
        mount_chosen_path(g_mount_path);
        g_mount_path.clear();
        g_precheck_pending = false; /* mount_chosen_path already re-ran it */
    }

    if (g_precheck_pending) {
        g_precheck_pending = false;
        refresh_precheck();
    }

    if (g_start_pending) {
        g_start_pending = false;
        if (g_m.can_start) {
            rt_log("launcher", "Start: booting '%s'", g_m.disc_path.c_str());
            g_done = true;
            g_done_started = true;
        } else {
            /* The state can have moved since the last precheck (a disc
             * unplugged, a file replaced), so this re-checks rather than
             * repeating a stale message. */
            refresh_precheck();
            set_status(g_m.can_start ? std::string("ready") : g_m.precheck_error);
        }
    }

    if (g_quit_pending) {
        g_quit_pending = false;
        rt_log("launcher", "Quit");
        g_done = true;
        g_done_started = false;
    }

    if (g_model_dirty) {
        g_model_dirty = false;
        g_model.DirtyAllVariables();
    }
}

} // namespace

} // namespace rtui

bool rt_launcher_run() {
    using namespace rtui;

    if (!g_ui.initialized || !g_ui.launcher) {
        rt_log("launcher", "no launcher document in this run; booting straight into the game");
        return true;
    }
    if (!backend_window_live()) {
        /* main's gate checks this too. Repeated here because a loop with no
         * window would spin at whatever rate present_ui returns 0 at, which
         * is as fast as the CPU allows. */
        rt_log("launcher", "no live window to draw the launcher into; booting straight into the game");
        return true;
    }

    const uint32_t user_present_mode = backend_present_mode();
    const RtPresentMode present_at_entry = rt_settings().display.present;
    backend_set_present_mode(RT_PGS_PRESENT_FIFO);
    rt_log("launcher", "present mode forced to FIFO while the launcher is up (mode %u restored at hand-off)",
        user_present_mode);

    g_ui.launcher_visible = true;
    g_ui.launcher->Show();
#ifdef ICORECOMP_PGS_SDL
    /* The path field needs SDL_EVENT_TEXT_INPUT, which SDL3 only delivers
     * between StartTextInput and StopTextInput. */
    menu_set_text_input(true);
#endif

    g_done = false;
    g_done_started = false;
    g_precheck_pending = true;

    bool window_closed = false;
#ifdef ICORECOMP_PGS_SDL
    bool menu_was_visible = false;
#endif
    while (!g_done) {
        rt_window_pump();
        rt_settings_apply_pending();
        launcher_tick();
        if (g_done) break;
#ifdef ICORECOMP_PGS_SDL
        /* rt_ui_set_visible(false) turns text input off when the menu that
         * was opened from here closes. The launcher still has a text field,
         * so it goes back on. */
        if (menu_was_visible && !rt_ui_visible()) menu_set_text_input(true);
        menu_was_visible = rt_ui_visible();
#endif
        rt_ui_tick();
        const uint32_t flags = backend_present_ui();
        if (flags & RT_PGS_VSYNC_WINDOW_CLOSED) {
            rt_log("launcher", "window closed; exiting");
            window_closed = true;
            break;
        }
        if (!(flags & RT_PGS_VSYNC_PRESENTED)) {
            /* Nothing was presented, so FIFO did not pace this iteration:
             * a minimized window is not presentable and the library polls
             * and returns instead of parking the thread. Without this the
             * loop would spin a core for as long as the window stays
             * minimized. One field at 60 Hz is the right order of magnitude
             * and nothing here is latency-sensitive. */
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    const bool start = g_done_started && !window_closed;

    g_ui.launcher->Hide();
    g_ui.launcher_visible = false;
    if (g_ui.credits) g_ui.credits->Hide();
#ifdef ICORECOMP_PGS_SDL
    if (!rt_ui_visible()) menu_set_text_input(false);
#endif

    /* The user's present mode goes back, unless they changed display.present
     * in the settings menu while the launcher was up: rt_settings_apply_
     * pending already applied that choice in the loop above, and putting the
     * startup value back would undo it. */
    if (rt_settings().display.present == present_at_entry) {
        backend_set_present_mode(user_present_mode);
        rt_log("launcher", "present mode restored to %u", user_present_mode);
    } else {
        rt_log("launcher", "present mode left as the settings menu set it; not restoring the startup value");
    }

    /* One write before either leaving or handing off: show_at_startup and a
     * new disc path are the two things a user expects to survive a launcher
     * they only visited once. This is the field boundary, so the write is
     * legal here. */
    rt_settings_flush_save();

    rt_log("launcher", "%s", start ? "starting the game" : "exiting without starting");
    return start;
}

#endif /* ICORECOMP_UI */
