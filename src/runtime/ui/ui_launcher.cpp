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
#include "title_logo.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
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

    /* True once the title image built from the disc has been published to the
     * renderer. The document swaps its "ICO" text for the image on this, and
     * keeps the text whenever it stays false. */
    bool logo_available = false;
};

LauncherModel g_m;
Rml::DataModelHandle g_model;
bool g_model_valid = false;

/* True while the settings menu is up over the launcher and this document
 * has been hidden for it (launcher_set_covered). */
bool g_covered = false;

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
/* Set by refresh_precheck() when a disc is mounted and no logo has been built
 * yet. Drained by launcher_tick(); the disc that is already resolved at
 * startup is handled before the first present instead, see
 * launcher_prepare_first_frame(). */
bool g_logo_pending = false;
/* The pixel size the title image was last rasterised at, and how many times it
 * has been rasterised. The counter is only there to give each raster its own
 * texture source string: RmlUi caches a texture by that string and will not
 * ask for it again, so a re-raster after a window-scale change needs a name it
 * has not seen. */
uint32_t g_logo_px_w = 0, g_logo_px_h = 0;
unsigned g_logo_generation = 0;

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

    /* The title image can only be built once a disc is mounted, which is
     * here. A disc that changes later re-arms this, so picking a working
     * image after a bad one still gets the logo. */
    if (g_m.disc_ok && !g_m.logo_available) g_logo_pending = true;
}

/* ---- title logo ---------------------------------------------------------
 *
 * Built on this loop rather than on a worker thread. The ISO reader is a
 * single file handle with no locking, and this same loop remounts it from
 * mount_chosen_path(), so a background reader would race the user's own disc
 * picker. The cost is one long call the first time a disc is seen: 82 ms on
 * the machine this was measured on, nearly all of it inflating the archive,
 * and about a millisecond on later runs, which come out of the cache. The log
 * lines carry the timings, so a slow disk shows up rather than being guessed
 * at.
 *
 * When the disc is already resolved at startup this runs before the first
 * present, so the launcher's first painted frame carries the image. Doing it
 * a tick later meant the window drew its text title and swapped a few frames
 * in, which reads as the panel adjusting after load. A disc chosen later,
 * through Browse or the path field, still goes the deferred way, because the
 * window has to be up for the user to choose it at all.
 */

/* The pixel box the overlay will draw the image across: the stylesheet's dp
 * box (kRtTitleLogoDp*, mirrored in .title-logo in ui/style/base.rcss) at the
 * context's current dp ratio. Rasterising at exactly this means one texel a
 * pixel, so the hard polygon edges the GS produces are not blurred by a
 * minifying sampler on the way to the screen. */
void logo_pixel_box(uint32_t* w, uint32_t* h) {
    rt_title_logo_pixel_box(ui_density_ratio(), w, h);
}

/* Rasterises at `w` by `h` and publishes it. `first` distinguishes the initial
 * build from a re-raster after the window scale moved.
 *
 * Nothing below this line may throw out of it. title_logo.h promises the build
 * is never fatal, and the whole feature is a launcher decoration: a disc that
 * makes it fail, in any way, costs the image and nothing else. The build keeps
 * that promise for itself; the catch here covers the publish and anything the
 * two allocate between them. */
bool raster_title_logo(uint32_t w, uint32_t h, bool first) try {
    RtTitleLogo logo;
    char err[512];
    if (!rt_title_logo_build(w, h, logo, err, sizeof(err))) {
        rt_log("ui", "no title logo from this disc: %s; the launcher keeps its text title", err);
        return false;
    }
    if (!ui_render_set_logo(logo.rgba.data(), logo.width, logo.height)) return false;

    g_logo_px_w = logo.width;
    g_logo_px_h = logo.height;
    ++g_logo_generation;
    rt_log("ui", "title logo: %s at %ux%u pixels (%ux%u dp at ratio %.2f)",
        first ? "rasterised for the first paint" : "re-rasterised for the new window scale",
        logo.width, logo.height, kRtTitleLogoDpWidth, kRtTitleLogoDpHeight,
        double(ui_density_ratio()));
    return true;
} catch (const std::exception& e) {
    rt_log("ui", "title logo: building the %ux%u image threw (%s); the launcher keeps its text"
                 " title",
        w, h, e.what());
    return false;
} catch (...) {
    rt_log("ui", "title logo: building the %ux%u image threw a non-standard exception; the launcher"
                 " keeps its text title",
        w, h);
    return false;
}

void build_title_logo() {
    if (g_m.logo_available) return;

    uint32_t w = 0, h = 0;
    logo_pixel_box(&w, &h);
    if (!raster_title_logo(w, h, true)) return;

    g_m.logo_available = true;
    g_model_dirty = true;
}

/* Points the image element at the current raster. RmlUi keys its texture cache
 * on the source string and never re-asks for one it has already loaded, so a
 * new raster needs a name it has not seen; the scheme's suffix is that name
 * and nothing else reads it (ui_render.cpp matches on the "logo:" prefix).
 *
 * The name it is leaving has to be released, or each re-raster strands one
 * full image: the texture database holds an entry per source for the life of
 * the context and nothing else ever drops one, so dragging a window edge
 * would upload a new image per size and free none of them.
 * Rml::ReleaseTexture goes through UiRenderInterface::ReleaseTexture, which
 * defers the backend destroy by two ticks, so a frame still naming that
 * texture stays valid. */
void refresh_logo_source() {
    if (!g_ui.launcher) return;
    Rml::Element* img = g_ui.launcher->GetElementById("title-logo");
    if (!img) {
        rt_log("ui", "launcher: no element 'title-logo' in %s; the image cannot be shown",
            g_ui.launcher->GetSourceURL().c_str());
        return;
    }
    char src[64];
    std::snprintf(src, sizeof(src), "%s%ux%u.%u", kLogoScheme, g_logo_px_w, g_logo_px_h,
        g_logo_generation);
    const Rml::String previous = img->GetAttribute<Rml::String>("src", Rml::String());
    if (!previous.empty() && previous != src) Rml::ReleaseTexture(previous);
    img->SetAttribute("src", Rml::String(src));
}

/* Re-rasterises when the window scale has moved, so the image is never scaled
 * on screen. Cheap: the geometry is already in memory, so this is the raster
 * alone, under a millisecond. */
void sync_logo_scale() {
    if (!g_m.logo_available) return;
    uint32_t w = 0, h = 0;
    logo_pixel_box(&w, &h);
    if (w == g_logo_px_w && h == g_logo_px_h) return;
    /* A re-raster that fails leaves the old size in place, so the next tick
     * asks for the same new size and fails again. Without this the log would
     * carry one failure line per frame for as long as the window stays at
     * that size. One box size, one attempt, one line. */
    static uint32_t failed_w = 0, failed_h = 0;
    if (w == failed_w && h == failed_h) return;
    rt_log("ui", "title logo: window scale moved, %ux%u is no longer the drawn size", g_logo_px_w,
        g_logo_px_h);
    if (raster_title_logo(w, h, false)) {
        failed_w = failed_h = 0;
        refresh_logo_source();
    } else {
        failed_w = w;
        failed_h = h;
    }
}

/* The raster succeeding does not mean the image is on screen: the upload to
 * the backend happens later, inside RmlUi's texture load, and only the
 * renderer sees whether it worked. RmlUi latches a load that returned nothing
 * and never retries it, so a failed upload would leave the document showing
 * the image element, with the text fallback switched off by logo_available,
 * over no texture at all: a blank box where the title should be. Clearing the
 * flag puts the text back. */
void poll_logo_upload() {
    if (!ui_render_take_logo_upload_failure()) return;
    if (!g_m.logo_available) return;
    rt_log("ui", "title logo: the %ux%u image could not be uploaded; the launcher goes back to its"
                 " text title",
        g_logo_px_w, g_logo_px_h);
    g_m.logo_available = false;
    g_model_dirty = true;
}

/* One check that the box the layout actually gave the image matches the box it
 * was rasterised for. A mismatch means ui/style/base.rcss and
 * kRtTitleLogoDpWidth/Height have drifted apart, which costs a resample and
 * the crisp edges with it, so it says so rather than quietly blurring. */
void verify_logo_box() {
    static unsigned checked_generation = 0;
    if (!g_m.logo_available || checked_generation == g_logo_generation) return;
    if (!g_ui.launcher) return;
    Rml::Element* img = g_ui.launcher->GetElementById("title-logo");
    if (!img) return;
    const Rml::Vector2f size = img->GetBox().GetSize();
    if (size.x <= 0.0f || size.y <= 0.0f) return; /* not laid out yet */
    checked_generation = g_logo_generation;
    if (std::fabs(size.x - float(g_logo_px_w)) > 0.5f ||
        std::fabs(size.y - float(g_logo_px_h)) > 0.5f) {
        rt_log("ui", "title logo: the layout gave the image %.1fx%.1f pixels but it was rasterised"
                     " for %ux%u; ui/style/base.rcss and kRtTitleLogoDpWidth/Height disagree, so the"
                     " overlay is resampling it",
            double(size.x), double(size.y), g_logo_px_w, g_logo_px_h);
    }
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
    /* rt_ui_set_visible() hides this document for the time the menu is up
     * (launcher_set_covered below), so the two never overlap. */
    rt_ui_set_visible(true);
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
 * The footer credit in launcher.rml is on the "launcher" data model, which
 * holds the disc state and the precheck result and has no business holding
 * a URL. It gets a plain click listener wired here instead.
 */
class ClickAction final : public Rml::EventListener {
public:
    enum class What { OpenUrl };
    ClickAction(What what, const char* argument) : m_what(what), m_argument(argument ? argument : "") {}

    void ProcessEvent(Rml::Event&) override {
        switch (m_what) {
        case What::OpenUrl: open_url(m_argument); break;
        }
    }

private:
    What m_what;
    std::string m_argument;
};

const char kSiteUrl[] = "https://defnf.com";

ClickAction g_open_site(ClickAction::What::OpenUrl, kSiteUrl);

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
    /* logo_available is bound here, before LoadDocument below, and that
     * ordering is load bearing. launcher.rml gives the image element
     * data-if="logo_available"; an unbound model would leave it visible at
     * the document's first layout, RmlUi would ask for the "logo:" source
     * before any image has been published, and it caches a load that
     * returned nothing and never asks again (see LoadTexture in
     * ui_render.cpp). The title would then stay blank for the run. Bound and
     * false, the element is display:none at first layout and no load is
     * attempted until the flag turns true. */
    c.Bind("logo_available", &g_m.logo_available);

    c.BindEventCallback("start", on_start);
    c.BindEventCallback("quit", on_quit);
    c.BindEventCallback("open_settings", on_open_settings);
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

    /* After every Bind above, never before one: see the note on
     * logo_available. The document's first layout reads the model, and an
     * element whose data-if is unbound is visible for that layout. */
    const std::string launcher_path = ui_dir + "/launcher.rml";
    g_ui.launcher = context->LoadDocument(launcher_path);
    if (!g_ui.launcher) {
        rt_log("ui", "document %s failed to load; the launcher is disabled", launcher_path.c_str());
        return false;
    }

    attach_click(g_ui.launcher, "credit-link", &g_open_site);

    rt_log("ui", "launcher document loaded: %s", launcher_path.c_str());
    return true;
}

void launcher_set_covered(bool covered) {
    /* Not "is the launcher document shown": the in-game menu opens with no
     * launcher at all, and this has to be a no-op there. */
    if (!g_ui.launcher_visible || !g_ui.launcher) return;
    if (covered == g_covered) return;
    g_covered = covered;

    if (covered) {
        g_ui.launcher->Hide();
    } else {
        g_ui.launcher->Show();
    }
    rt_log("ui", "launcher document %s for the settings menu", covered ? "hidden" : "shown again");
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

    /* Ahead of the precheck below, not after it: a precheck arms this, and
     * draining it in the same tick would put the archive read in the same
     * field as the precheck's own megabyte of disc reads and hashing. One
     * tick later the window has already drawn. */
    if (g_logo_pending) {
        g_logo_pending = false;
        build_title_logo();
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

    /* The image must never be drawn at anything but its raster size, so the
     * window scale is followed rather than sampled around. Both are no-ops
     * until the image exists. */
    sync_logo_scale();
    verify_logo_box();
    poll_logo_upload();

    if (g_model_dirty) {
        g_model_dirty = false;
        g_model.DirtyAllVariables();
    }
}

/* Everything that has to be done before the window's first frame is presented.
 *
 * The precheck resolves whatever disc --disc, settings.json, config/local.toml
 * or the folder next to the executable already name, and if one is there the
 * title image is rasterised right here. Nothing has been presented yet at this
 * point, so the first painted frame carries the image and the panel never
 * visibly adjusts. The wait is bounded by the extraction itself: about 82 ms
 * cold and a millisecond out of the cache.
 *
 * When no disc resolves, this leaves g_logo_pending set exactly as before and
 * launcher_tick() picks the image up once one is chosen in the window. */
void launcher_prepare_first_frame() {
    const auto t0 = std::chrono::steady_clock::now();
    g_precheck_pending = false;
    refresh_precheck();

    if (!g_logo_pending) {
        rt_log("ui", "title logo: no disc resolved before the first frame; the launcher opens with"
                     " its text title and picks the image up when a disc is chosen");
        return;
    }
    g_logo_pending = false;
    build_title_logo();

    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    rt_log("ui", "title logo: held the first launcher frame for %.1f ms; it %s", ms,
        g_m.logo_available ? "carries the image" : "falls back to the text title");
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

    g_covered = false;
    g_ui.launcher_visible = true;
    g_ui.launcher->Show();
#ifdef ICORECOMP_PGS_SDL
    /* The path field needs SDL_EVENT_TEXT_INPUT, which SDL3 only delivers
     * between StartTextInput and StopTextInput. */
    menu_set_text_input(true);
#endif

    g_done = false;
    g_done_started = false;
    /* Before the loop, so nothing has been presented yet when it runs. */
    launcher_prepare_first_frame();

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
    g_covered = false;
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
