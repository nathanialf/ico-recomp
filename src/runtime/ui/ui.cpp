/* ui/ui.cpp: RmlUi bring-up, the per-field tick, and visibility.
 *
 * See ui.h for the module's rules. The two that shape this file:
 *   - rt_ui_tick is the only place that may call the overlay entry points,
 *     because it is the only one guaranteed to run between frames
 *     (rt_gs_vsync_hook, before the backend's vsync()). They now go through
 *     GsBackend (rt_gs_backend()->overlay_*) rather than straight to the
 *     paraLLEl-GS library, so the GS command ring sees them in order with
 *     the guest's GIF traffic; see the wrappers below.
 *   - a failure anywhere in rt_ui_init disables the UI and returns false.
 *     It is never fatal: the port must still run the game with no menu.
 */
#include "ui.h"

#ifdef ICORECOMP_UI

#include "ui_internal.h"

#include "../gs/gs_backend.h"
#include "../host/input.h"
#include "../host/settings.h"
#include "../host/window.h"
#include "../host/window_service.h"
#include "../runtime.h"

#include <RmlUi/Core/Core.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace rtui {

UiState g_ui;

/* ---- overlay backend wrappers ------------------------------------------- */

/* Anything that changes what the GS renders or presents goes through the GS
 * backend (gs/gs_backend.h) rather than calling a particular renderer
 * directly. The backend is a command ring with a worker thread on the far
 * end (gs/gs_threaded.cpp), and a call that skips it would be invisible to
 * the ring and so would land out of order against the guest's own GIF
 * traffic.
 *
 * No ICORECOMP_HAVE_PARALLEL_GS guard is needed for these: GsBackend's
 * defaults are no-ops returning 0, which is exactly what the dump backend
 * and a build with no live backend want. rt_gs_backend() never has to create
 * the backend from here either: rt_hw_init() builds it, and main.cpp runs
 * that before rt_ui_init() on both of its orderings.
 *
 * Four things go to the window service instead, because none of them is a GS
 * command: the SDL window handle, the surface size, the resolved present
 * mode, and the present rectangle the last present blitted the scanout into.
 * The first two are main-thread reads the UI lays itself out from; sending a
 * query through the ring would mean blocking on the consumer for a value
 * only this thread can change. The present rectangle is the one of the four
 * the consumer side writes: whichever backend presented publishes it on the
 * GS command ring's worker thread and it is read back here on the EE thread,
 * both under the service's own mutex, so the six values always describe one
 * present rather than a mix of two (host/window_service.h carries the same
 * note). */

uint32_t backend_texture_create(const uint8_t* rgba8, uint32_t width, uint32_t height) {
    return rt_gs_backend()->overlay_texture_create(rgba8, width, height);
}

void backend_texture_destroy(uint32_t texture) {
    rt_gs_backend()->overlay_texture_destroy(texture);
}

void backend_set_frame(const RtPgsOverlayFrame* frame) {
    rt_gs_backend()->overlay_set_frame(frame);
}

uint32_t backend_present_ui() {
    return rt_gs_backend()->present_ui();
}

void backend_set_present_mode(uint32_t mode) {
    rt_gs_backend()->set_present_mode(mode);
}

/* The window and its present rectangle are the executable's now
 * (host/window_service.h), not a GS backend's, so these five are plain
 * forwards with no ICORECOMP_HAVE_PARALLEL_GS split: a build with no live
 * backend at all has a window service that reports no window, which is
 * exactly what stops rt_ui_init before anything else here can run. */

bool backend_window_live() {
    return rt_window_exists();
}

void* backend_window_handle() {
    return rt_window_handle();
}

void backend_surface_size(uint32_t* width, uint32_t* height) {
    rt_window_surface_size(width, height);
}

void backend_present_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h,
                          int32_t* bb_w, int32_t* bb_h) {
    rt_window_present_rect(x, y, w, h, bb_w, bb_h);
}

uint32_t backend_present_mode() {
    return rt_gs_present_mode();
}

namespace {

/* Density-independent pixel ratio: the documents are authored in dp against
 * a 640x480 surface. That is the layout's design size, not the default
 * window size: display.window_width/height default to 1280x960 (settings.h),
 * which is this baseline at a dp ratio of exactly 2. Every column width and
 * font size in ui/style/base.rcss is chosen to fit at ratio 1, so a window
 * dragged down to the 640x480 minimum still shows the whole layout. A larger
 * window scales the menu with it rather than leaving it a postage stamp in a
 * corner. Clamped at 4x, past which the text is larger than anything the
 * layout was written for. */
float density_for(uint32_t surface_height) {
    if (surface_height == 0) return 1.0f;
    const float ratio = float(surface_height) / 480.0f;
    return std::min(std::max(ratio, 1.0f), 4.0f);
}

#ifdef ICORECOMP_HAVE_SDL
/* Re-resolves the menu hotkey whenever the committed settings have moved
 * (rt_settings_generation), and names the key in force in the document's
 * "press X to close" line, which ships with the compiled-in default. Cheap:
 * one integer compare per field when nothing changed. */
void sync_menu_hotkey() {
    static unsigned resolved_gen = 0;
    const unsigned gen = rt_settings_generation();
    if (gen == resolved_gen) return;
    resolved_gen = gen;
    resolve_menu_hotkey();
    if (Rml::Element* key = g_ui.menu->GetElementById("menu-key")) {
        key->SetInnerRML(menu_hotkey_name());
    }
    if (Rml::Element* pad = g_ui.menu->GetElementById("menu-pad")) {
        pad->SetInnerRML(menu_gamepad_name());
    }
}
#endif

} // namespace

/* The ratio the context is running at, read from the live surface rather than
 * from g_ui, so it is right before the first tick has applied a size. The
 * launcher needs it to rasterise the title image at the exact pixel size the
 * overlay will draw it. */
float ui_density_ratio() {
    uint32_t width = 0, height = 0;
    backend_surface_size(&width, &height);
    if (height == 0) height = g_ui.surface_height;
    return density_for(height);
}

namespace {

void apply_surface_size(uint32_t width, uint32_t height) {
    g_ui.surface_width = width;
    g_ui.surface_height = height;
    g_ui.context->SetDimensions(Rml::Vector2i(int(width), int(height)));
    g_ui.context->SetDensityIndependentPixelRatio(density_for(height));
}

/* The first element carrying a class, or null. menu.rml has one .shell and
 * one .pane, so there is nothing here to pick between. */
Rml::Element* first_by_class(Rml::Element* root, const char* class_name) {
    if (!root) return nullptr;
    Rml::ElementList found;
    root->GetElementsByClassName(found, class_name);
    return found.empty() ? nullptr : found.front();
}

/* One line each time the menu opens. It answers the question the pane's
 * bounded height turns on: whether .pane is the height of the shell that
 * holds it, and, when it is not, which element grew instead. A pane that
 * scrolls reads back with its scroll height above its client height, which
 * is the condition RmlUi's wheel handling looks for
 * (Element.cpp, GetClosestScrollableContainer).
 *
 * The figures are RmlUi's, in surface pixels, so at the default 1280x960
 * window they are twice the dp values in base.rcss. */
void log_menu_metrics() {
    Rml::Element* shell = first_by_class(g_ui.menu, "shell");
    Rml::Element* pane = first_by_class(g_ui.menu, "pane");
    /* g_ui.menu is never null here: rt_ui_init fails without menu.rml, and
     * the flag that brings this here is set right after menu->Show(). */
    if (!shell || !pane) {
        rt_log_warn("ui", "menu heights: no %s in the menu document; this file and menu.rml disagree",
            !shell ? ".shell" : ".pane");
        return;
    }
    rt_log_info("ui", "menu heights: body %.1f, shell %.1f, pane %.1f (client %.1f, scroll %.1f)",
        double(g_ui.menu->GetOffsetHeight()), double(shell->GetOffsetHeight()),
        double(pane->GetOffsetHeight()), double(pane->GetClientHeight()),
        double(pane->GetScrollHeight()));
}

} // namespace

} // namespace rtui

bool rt_ui_init() {
    using namespace rtui;

    if (g_ui.initialized) return true;

    if (!backend_window_live()) {
        rt_log_warn("ui", "no live windowed backend in this run; the settings UI is disabled");
        return false;
    }

    /* SDL video is up now (backend_window_live() just confirmed it), so the
     * launcher, which runs before any guest thread exists and so before
     * rt_input_init()/rt_pad_register_services() ever gets to probe, gets
     * pad focus and button events of its own. A no-op in a build with no
     * SDL, and a no-op after the first successful probe. */
    rt_input_sdl_gamepad_probe();

    const std::string base = rt_base_dir();
    const std::string ui_dir = base + "/ui";
    std::error_code ec;
    if (!std::filesystem::is_directory(ui_dir, ec)) {
        rt_log_warn("ui", "no UI assets at %s; the settings UI is disabled (the game runs without it)",
            ui_dir.c_str());
        return false;
    }

    static UiSystemInterface system_interface;
    static UiRenderInterface render_interface;
    g_ui.system = &system_interface;
    g_ui.render = &render_interface;
    Rml::SetSystemInterface(g_ui.system);
    Rml::SetRenderInterface(g_ui.render);
    /* File access goes through RmlUi's own stdio FileInterface: every path
     * this module hands it is already absolute (built from rt_base_dir()),
     * and documents reference their stylesheets relatively from there. */
    if (!Rml::Initialise()) {
        rt_log_warn("ui", "Rml::Initialise() failed; the settings UI is disabled");
        return false;
    }

    /* Two faces, both required: the documents name "Playfair Display" for
     * headings and "JetBrains Mono" for everything a value is read out of,
     * and a missing family falls back to whatever RmlUi finds, which is
     * nothing here. Either failure disables the UI and the log names which
     * file it was.
     *
     * Playfair is one variable font in one file: RmlUi's FreeType engine
     * walks the named instances of a variable face (FreeTypeInterface.cpp
     * GetFaceVariations) and registers one face per weight, so font-weight
     * 400 and 700 both resolve out of that single file. JetBrains Mono is a
     * static regular; nothing in the documents asks it for a bold. */
    const std::string serif_path = ui_dir + "/fonts/PlayfairDisplay[wght].ttf";
    const std::string mono_path = ui_dir + "/fonts/JetBrainsMono-Regular.ttf";
    if (!Rml::LoadFontFace(serif_path)) {
        rt_log_warn("ui", "font %s failed to load; the settings UI is disabled", serif_path.c_str());
        return false;
    }
    if (!Rml::LoadFontFace(mono_path)) {
        rt_log_warn("ui", "font %s failed to load; the settings UI is disabled", mono_path.c_str());
        return false;
    }

    uint32_t width = 0, height = 0;
    backend_surface_size(&width, &height);
    if (width == 0 || height == 0) {
        rt_log_warn("ui", "surface size reported as %ux%u; the settings UI is disabled", width, height);
        return false;
    }

    g_ui.context = Rml::CreateContext("main", Rml::Vector2i(int(width), int(height)));
    if (!g_ui.context) {
        rt_log_warn("ui", "Rml::CreateContext failed; the settings UI is disabled");
        return false;
    }
    apply_surface_size(width, height);

    /* Before any LoadDocument: a document binds its data views while it is
     * parsed, so the model has to exist first. */
    if (!settings_model_init(g_ui.context)) return false;

    /* The same rule, for the same reason: menu.rml's Achievements tab is a
     * nested data-model="achievements" subtree, so that model has to exist
     * before menu.rml is parsed. This call also loads the unlock toast
     * document. A failure costs the tab and the toast and is logged inside,
     * so it is not checked for here. */
    achievements_model_init(g_ui.context, ui_dir);

    const std::string doc_path = ui_dir + "/menu.rml";
    g_ui.menu = g_ui.context->LoadDocument(doc_path);
    if (!g_ui.menu) {
        rt_log_warn("ui", "document %s failed to load; the settings UI is disabled", doc_path.c_str());
        return false;
    }

    /* The fps readout is its own always-loaded document: it is visible with
     * or without the menu, entirely on display.show_fps. Failing to load it
     * costs the readout, not the menu. */
    const std::string fps_path = ui_dir + "/fps.rml";
    g_ui.fps = g_ui.context->LoadDocument(fps_path);
    if (!g_ui.fps) {
        rt_log_warn("ui", "document %s failed to load; the fps readout is unavailable", fps_path.c_str());
    }

    /* The drawn cursor for the game's own menus, another always-loaded pair.
     * Failing to load it costs the cursor, not the menu, so it is logged
     * inside and not checked for here. */
    ui_menu_cursor_init(g_ui.context, ui_dir);

    /* The launcher's model and its document. A failure here costs the
     * launcher, not the menu, so it is logged inside and not
     * checked for here. */
    launcher_init(g_ui.context, ui_dir);

    g_ui.visible = false;
    g_ui.initialized = true;

    rt_log_info("ui", "RmlUi %s up: fonts %s and %s, document %s, surface %ux%u, dp ratio %.2f",
        Rml::GetVersion().c_str(), serif_path.c_str(), mono_path.c_str(), doc_path.c_str(),
        width, height, double(density_for(height)));

#ifdef ICORECOMP_HAVE_SDL
    sync_menu_hotkey();
#else
    rt_log_warn("ui", "no SDL in this build: the menu has no hotkey and cannot be opened");
#endif

    /* Fills the model from the loaded settings and puts the fps readout in
     * the state display.show_fps asks for, before the first tick. */
    settings_model_refresh();
    return true;
}

void rt_ui_tick() {
    using namespace rtui;

    if (!g_ui.initialized) return;

    /* The surface size is re-read here, never from the event handler: a
     * resize event can arrive from inside WSI::begin_frame, where rt_pgs_*
     * calls are off limits. Reading it every tick is a plain getter across
     * the ABI and costs less than the flag it would replace. */
    uint32_t width = 0, height = 0;
    backend_surface_size(&width, &height);
    if (width != 0 && height != 0 &&
        (width != g_ui.surface_width || height != g_ui.surface_height)) {
        apply_surface_size(width, height);
        rt_log_info("ui", "surface now %ux%u, dp ratio %.2f", width, height,
            double(density_for(height)));
    }

    /* Everything that changes settings, writes the settings file or shows
     * and hides a document happens here, at the field boundary, never in the
     * event handler: see the reentrancy rules in ui.h. */
    settings_model_tick();
#ifdef ICORECOMP_HAVE_SDL
    /* After the model's own tick: a capture that was accepted this field
     * commits here, and a queued control change has already gone through, so
     * the two never write the settings in the same call. */
    rebind_tick();
    /* A rebind or a reset may have changed input.keyboard.menu or
     * input.gamepad.menu in the commit just above; the hotkey follows the
     * committed settings, not the init-time value. */
    sync_menu_hotkey();
    /* Held-direction repeat and the select pad-session's own field-boundary
     * check (ui_internal.h); after rebind_tick so a capture that just ended
     * has already dropped whatever it needed to before nav holds are
     * re-examined. */
    ui_nav_tick();
#endif
    if (g_ui.flush_save_pending) {
        g_ui.flush_save_pending = false;
        rt_settings_flush_save();
    }

    /* Before the early-out below, which counts g_ui.cursor_visible: this
     * call is what sets it. */
    ui_menu_cursor_tick();

    /* Also before the early-out, and for the same reason: this call is what
     * sets g_ui.toast_visible, and polling is what starts an unlock toast's
     * four seconds (guest/achievements.h). */
    achievements_model_tick();

    /* The tick renders whenever any document is up: the launcher (which owns
     * the whole window while it is, and draws over an empty backbuffer), the
     * menu, the fps readout, the drawn cursor on the game's own menus, or an
     * achievement unlock toast. */
    if (!g_ui.visible && !g_ui.fps_visible && !g_ui.launcher_visible &&
        !g_ui.cursor_visible && !g_ui.toast_visible) {
        /* Exactly one clear on the way down, keyed on whether anything was
         * drawn last time; never a set_frame call while nothing is up. */
        if (g_ui.frame_posted) {
            backend_set_frame(nullptr);
            g_ui.frame_posted = false;
        }
        return;
    }

    g_ui.context->Update();

    /* After Update(), which is the call that re-runs the data-if deciding
     * which .section the pane shows and lays the result out: the pad's
     * "enter this card" queues its move into the pane for exactly this
     * point (ui_internal.h). */
    settings_model_post_update();

    /* After Update() and never from rt_ui_set_visible: the boxes are laid
     * out by the context, so heights read at the moment the document is
     * shown are the previous field's. */
    if (g_ui.menu_metrics_pending) {
        g_ui.menu_metrics_pending = false;
        log_menu_metrics();
    }

    g_ui.render->begin_frame();
    g_ui.context->Render();

    const RtPgsOverlayFrame& frame = g_ui.render->frame(g_ui.surface_width, g_ui.surface_height);
    /* One line for the first frame that carries geometry and one for the
     * first that does not while a document is up: the two halves of "the UI
     * drew nothing" (RmlUi produced no geometry, or the library drew what it
     * got and it did not show) are told apart from the log alone. */
    static bool logged_geometry = false, logged_empty = false;
    if (frame.cmd_count != 0 && !logged_geometry) {
        logged_geometry = true;
        rt_log_info("ui", "first overlay frame: %u vertices, %u indices, %u cmds for a %ux%u surface"
                     " (menu %d, fps %d, launcher %d, cursor %d)",
            frame.vertex_count, frame.index_count, frame.cmd_count,
            frame.surface_width, frame.surface_height,
            g_ui.visible ? 1 : 0, g_ui.fps_visible ? 1 : 0, g_ui.launcher_visible ? 1 : 0,
            g_ui.cursor_visible ? 1 : 0);
    } else if (frame.cmd_count == 0 && !logged_empty) {
        logged_empty = true;
        rt_log_warn("ui", "a document is up but RmlUi produced no geometry this field"
                     " (menu %d, fps %d, launcher %d, cursor %d)",
            g_ui.visible ? 1 : 0, g_ui.fps_visible ? 1 : 0, g_ui.launcher_visible ? 1 : 0,
            g_ui.cursor_visible ? 1 : 0);
    }
    if (frame.cmd_count != 0) {
        backend_set_frame(&frame);
        g_ui.frame_posted = true;
    } else if (g_ui.frame_posted) {
        /* Up but with nothing to draw (an empty document). Clearing is the
         * honest result; leaving the previous frame up would show stale
         * geometry. */
        backend_set_frame(nullptr);
        g_ui.frame_posted = false;
    }
}

bool rt_ui_visible() {
    return rtui::g_ui.visible;
}

void rt_ui_set_visible(bool visible) {
    using namespace rtui;
    if (!g_ui.initialized || g_ui.visible == visible) return;
    g_ui.visible = visible;
    if (visible) {
        /* Open on what the settings actually hold: another consumer (a
         * window resize, an env-overridden key) may have moved a value since
         * the menu was last up. */
        settings_model_refresh();
        /* And on what has actually been unlocked, for the same reason: the
         * tab's own change check only runs while the menu is already up. */
        achievements_model_refresh();
        /* Over the launcher, the launcher goes away first: the menu's
         * backdrop is translucent for the game's scanout behind it, and
         * two documents through one another is unreadable. No-op when the
         * launcher is not up, which is every in-game open. */
        launcher_set_covered(true);
        g_ui.menu->Show();
        /* Show()'s own default (FocusFlag::Auto) focuses the document
         * itself when no element carries autofocus, and menu.rml has none;
         * this puts the pad's focus ring on whichever tab is actually
         * showing. */
        settings_model_focus_active_tab();
        /* Read at the next tick, once the context has laid the document
         * out. */
        g_ui.menu_metrics_pending = true;
    } else {
#ifdef ICORECOMP_HAVE_SDL
        /* An armed capture never outlives the menu. That is what keeps
         * rt_ui_wants_input() (and so the neutral pad in host/input.cpp) true
         * for the whole of a capture: the menu is up for all of it. A capture
         * that was already accepted is left alone: rebind_tick applies it at
         * this same field boundary, with the menu closed (ui_rebind.cpp). */
        rebind_cancel("the menu closed");
#endif
        /* Every way of closing the menu disarms the Quit button's "press
         * again" state, not only a later reopen. */
        settings_model_disarm_quit();
        g_ui.menu->Hide();
        g_ui.menu_metrics_pending = false;
        /* Whatever the launcher had up when the menu opened comes back. */
        launcher_set_covered(false);
        /* Not rt_settings_flush_save() here. This function runs from the
         * event handler, which can execute from inside WSI::begin_frame; the
         * write belongs at the field boundary, so the next rt_ui_tick does
         * it. */
        g_ui.flush_save_pending = true;
    }
#ifdef ICORECOMP_HAVE_SDL
    /* SDL3 gates SDL_EVENT_TEXT_INPUT on this, so the text fields in the
     * menu stay dead without it. */
    menu_set_text_input(visible);
#endif
    rt_log_info("ui", "menu %s", visible ? "opened" : "closed");
}

bool rt_ui_wants_input() {
    /* The launcher counts even though no guest code is running yet to read
     * a pad: host/input.cpp's contract is "the UI owns input while this is
     * true", and the launcher owns it for the whole of rt_launcher_run(). */
    return rt_ui_visible() || rtui::g_ui.launcher_visible;
}

#endif /* ICORECOMP_UI */
