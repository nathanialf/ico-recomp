/* ui/ui.cpp: RmlUi bring-up, the per-field tick, and visibility.
 *
 * See ui.h for the module's rules. The two that shape this file:
 *   - rt_ui_tick is the only place that may call the rt_pgs_overlay_*
 *     entry points, because it is the only one guaranteed to run between
 *     frames (rt_gs_vsync_hook, before the backend's vsync()).
 *   - a failure anywhere in rt_ui_init disables the UI and returns false.
 *     It is never fatal: the port must still run the game with no menu.
 */
#include "ui.h"

#ifdef ICORECOMP_UI

#include "ui_internal.h"

#include "../host/settings.h"
#include "../host/window.h"
#include "../runtime.h"

#include <RmlUi/Core/Core.h>

#include <algorithm>
#include <filesystem>
#include <string>

namespace rtui {

UiState g_ui;

/* ---- overlay backend wrappers ------------------------------------------- */

#ifdef ICORECOMP_HAVE_PARALLEL_GS

bool backend_window_live() {
    RtPgs* pgs = rt_gs_parallel_handle();
    return pgs && rt_pgs_window_handle(pgs) != nullptr;
}

void* backend_window_handle() {
    RtPgs* pgs = rt_gs_parallel_handle();
    return pgs ? rt_pgs_window_handle(pgs) : nullptr;
}

void backend_surface_size(uint32_t* width, uint32_t* height) {
    *width = 0;
    *height = 0;
    if (RtPgs* pgs = rt_gs_parallel_handle()) rt_pgs_surface_size(pgs, width, height);
}

uint32_t backend_texture_create(const uint8_t* rgba8, uint32_t width, uint32_t height) {
    RtPgs* pgs = rt_gs_parallel_handle();
    return pgs ? rt_pgs_overlay_texture_create(pgs, rgba8, width, height) : 0;
}

void backend_texture_destroy(uint32_t texture) {
    if (RtPgs* pgs = rt_gs_parallel_handle()) rt_pgs_overlay_texture_destroy(pgs, texture);
}

void backend_set_frame(const RtPgsOverlayFrame* frame) {
    if (RtPgs* pgs = rt_gs_parallel_handle()) rt_pgs_overlay_set_frame(pgs, frame);
}

#else /* !ICORECOMP_HAVE_PARALLEL_GS */

/* No library to call: this build has no live backend at all (the CI gate
 * builds the UI sources exactly this way, which is what makes it cheap).
 * backend_window_live() false stops rt_ui_init before anything else here
 * can run. */
bool backend_window_live() { return false; }
void* backend_window_handle() { return nullptr; }
void backend_surface_size(uint32_t* width, uint32_t* height) { *width = 0; *height = 0; }
uint32_t backend_texture_create(const uint8_t*, uint32_t, uint32_t) { return 0; }
void backend_texture_destroy(uint32_t) {}
void backend_set_frame(const RtPgsOverlayFrame*) {}

#endif /* ICORECOMP_HAVE_PARALLEL_GS */

namespace {

/* Density-independent pixel ratio: the documents are authored in dp against
 * a 640x480 surface, the historic window size (gs_parallel_lib.cpp's
 * init_windowed). A larger window scales the menu with it rather than
 * leaving it a postage stamp in a corner. Clamped at 4x, past which the text
 * is larger than anything the layout was written for. */
float density_for(uint32_t surface_height) {
    if (surface_height == 0) return 1.0f;
    const float ratio = float(surface_height) / 480.0f;
    return std::min(std::max(ratio, 1.0f), 4.0f);
}

#ifdef ICORECOMP_PGS_SDL
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
}
#endif

void apply_surface_size(uint32_t width, uint32_t height) {
    g_ui.surface_width = width;
    g_ui.surface_height = height;
    g_ui.context->SetDimensions(Rml::Vector2i(int(width), int(height)));
    g_ui.context->SetDensityIndependentPixelRatio(density_for(height));
}

} // namespace

} // namespace rtui

bool rt_ui_init() {
    using namespace rtui;

    if (g_ui.initialized) return true;

    if (!backend_window_live()) {
        rt_log("ui", "no live windowed backend in this run; the settings UI is disabled");
        return false;
    }

    const std::string base = rt_base_dir();
    const std::string ui_dir = base + "/ui";
    std::error_code ec;
    if (!std::filesystem::is_directory(ui_dir, ec)) {
        rt_log("ui", "no UI assets at %s; the settings UI is disabled (the game runs without it)",
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
        rt_log("ui", "Rml::Initialise() failed; the settings UI is disabled");
        return false;
    }

    /* One variable font, one file. RmlUi's FreeType engine walks the named
     * instances of a variable face (FreeTypeInterface.cpp GetFaceVariations)
     * and registers one face per weight, so "Playfair Display" at
     * font-weight 400 and 700 both resolve out of this single file. */
    const std::string font_path = ui_dir + "/fonts/PlayfairDisplay[wght].ttf";
    if (!Rml::LoadFontFace(font_path)) {
        rt_log("ui", "font %s failed to load; the settings UI is disabled", font_path.c_str());
        return false;
    }

    uint32_t width = 0, height = 0;
    backend_surface_size(&width, &height);
    if (width == 0 || height == 0) {
        rt_log("ui", "surface size reported as %ux%u; the settings UI is disabled", width, height);
        return false;
    }

    g_ui.context = Rml::CreateContext("main", Rml::Vector2i(int(width), int(height)));
    if (!g_ui.context) {
        rt_log("ui", "Rml::CreateContext failed; the settings UI is disabled");
        return false;
    }
    apply_surface_size(width, height);

    /* Before any LoadDocument: a document binds its data views while it is
     * parsed, so the model has to exist first. */
    if (!settings_model_init(g_ui.context)) return false;

    const std::string doc_path = ui_dir + "/menu.rml";
    g_ui.menu = g_ui.context->LoadDocument(doc_path);
    if (!g_ui.menu) {
        rt_log("ui", "document %s failed to load; the settings UI is disabled", doc_path.c_str());
        return false;
    }

    /* The fps readout is its own always-loaded document: it is visible with
     * or without the menu, entirely on display.show_fps. Failing to load it
     * costs the readout, not the menu. */
    const std::string fps_path = ui_dir + "/fps.rml";
    g_ui.fps = g_ui.context->LoadDocument(fps_path);
    if (!g_ui.fps) {
        rt_log("ui", "document %s failed to load; the fps readout is unavailable", fps_path.c_str());
    }

    g_ui.visible = false;
    g_ui.initialized = true;

    rt_log("ui", "RmlUi %s up: font %s, document %s, surface %ux%u, dp ratio %.2f",
        Rml::GetVersion().c_str(), font_path.c_str(), doc_path.c_str(),
        width, height, double(density_for(height)));

#ifdef ICORECOMP_PGS_SDL
    sync_menu_hotkey();
#else
    rt_log("ui", "no SDL in this build: the menu has no hotkey and cannot be opened");
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
        rt_log("ui", "surface now %ux%u, dp ratio %.2f", width, height,
            double(density_for(height)));
    }

    /* Everything that changes settings, writes the settings file or shows
     * and hides a document happens here, at the field boundary, never in the
     * event handler: see the reentrancy rules in ui.h. */
    settings_model_tick();
#ifdef ICORECOMP_PGS_SDL
    /* After the model's own tick: a capture that was accepted this field
     * commits here, and a queued control change has already gone through, so
     * the two never write the settings in the same call. */
    rebind_tick();
    /* A rebind or a reset may have changed input.keyboard.menu or
     * input.gamepad.menu in the commit just above; the hotkey follows the
     * committed settings, not the init-time value. */
    sync_menu_hotkey();
#endif
    if (g_ui.flush_save_pending) {
        g_ui.flush_save_pending = false;
        rt_settings_flush_save();
    }

    /* The tick renders whenever any document is up. The menu is one of them;
     * the fps readout is the other, and it is shown on display.show_fps with
     * the menu closed. */
    if (!g_ui.visible && !g_ui.fps_visible) {
        /* Exactly one clear on the way down, keyed on whether anything was
         * drawn last time; never a set_frame call while nothing is up. */
        if (g_ui.frame_posted) {
            backend_set_frame(nullptr);
            g_ui.frame_posted = false;
        }
        return;
    }

    g_ui.context->Update();
    g_ui.render->begin_frame();
    g_ui.context->Render();

    const RtPgsOverlayFrame& frame = g_ui.render->frame(g_ui.surface_width, g_ui.surface_height);
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
        g_ui.menu->Show();
    } else {
#ifdef ICORECOMP_PGS_SDL
        /* A capture never outlives the menu. That is what keeps
         * rt_ui_wants_input() (and so the neutral pad in host/input.cpp) true
         * for the whole of a capture: the menu is up for all of it. */
        rebind_cancel("the menu closed");
#endif
        g_ui.menu->Hide();
        /* Not rt_settings_flush_save() here. This function runs from the
         * event handler, which can execute from inside WSI::begin_frame; the
         * write belongs at the field boundary, so the next rt_ui_tick does
         * it. */
        g_ui.flush_save_pending = true;
    }
#ifdef ICORECOMP_PGS_SDL
    /* SDL3 gates SDL_EVENT_TEXT_INPUT on this, so the text fields in the
     * menu stay dead without it. */
    menu_set_text_input(visible);
#endif
    rt_log("ui", "menu %s", visible ? "opened" : "closed");
}

bool rt_ui_wants_input() {
    return rt_ui_visible();
}

#endif /* ICORECOMP_UI */
