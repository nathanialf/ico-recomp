/* ui/ui_internal.h: shared state and interfaces of the ui/ module.
 *
 * Private to the .cpp files in src/runtime/ui. ui.h is the module's public
 * face and pulls in neither RmlUi nor SDL; this header pulls in RmlUi, so
 * nothing outside this directory may include it.
 *
 * Threading and reentrancy rules are stated in ui.h and apply to everything
 * here.
 */
#ifndef ICORECOMP_UI_UI_INTERNAL_H
#define ICORECOMP_UI_UI_INTERNAL_H

#ifdef ICORECOMP_UI

#include "../gs/gs_parallel_api.h"
/* For RtBindDevice, which the binding-table declarations below take.
 * Runtime-internal and SDL-free, like this header. */
#include "../host/settings.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/SystemInterface.h>
#include <RmlUi/Core/Types.h>

#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace Rml { class DataModelHandle; }

#ifdef ICORECOMP_PGS_SDL
/* Declared, not included, for the same reason ui.h declares it: this header
 * is included by files that see no SDL. SDL3/SDL.h's own typedef agrees with
 * it, so either include order works. */
union SDL_Event;
#endif

namespace rtui {

/* ---- overlay backend ----------------------------------------------------
 *
 * The four library entry points this module uses, wrapped so that the rest
 * of the module compiles unchanged when the build has no paraLLEl-GS library
 * to link against (ICORECOMP_PARALLEL_GS=OFF, the CI gate). The wrappers are
 * defined in ui.cpp, the real ones under ICORECOMP_HAVE_PARALLEL_GS and
 * do-nothing ones otherwise. All of them are between-frames-only; only
 * rt_ui_tick may call them.
 */
bool backend_window_live();
void backend_surface_size(uint32_t* width, uint32_t* height);
/* The launcher's three: one presented frame with no guest scanout, and the
 * present-mode pair it uses to force FIFO while it is up and put the user's
 * mode back at hand-off. Between-frames-only like the rest, which for these
 * means the launcher's own loop (ui_launcher.cpp), never the event pump. */
uint32_t backend_present_ui();
void backend_set_present_mode(uint32_t mode);
uint32_t backend_present_mode();
uint32_t backend_texture_create(const uint8_t* rgba8, uint32_t width, uint32_t height);
void backend_texture_destroy(uint32_t texture);
void backend_set_frame(const RtPgsOverlayFrame* frame);
/* SDL_Window* as void*, for the window-to-surface coordinate scale in
 * ui_events.cpp. NULL when there is no window. */
void* backend_window_handle();
/* The window-backbuffer rectangle the last presented scanout was blitted
 * into, plus the backbuffer size it was measured against. All zero before
 * the first present and in a build with no library. The drawn cursor for
 * the game's own menus is placed on this rectangle. */
void backend_present_rect(int32_t* x, int32_t* y, int32_t* w, int32_t* h,
                          int32_t* bb_w, int32_t* bb_h);

/* The context's density-independent pixel ratio, derived from the live
 * surface height against the 640x480 design size and clamped to [1, 4] (see
 * density_for in ui.cpp). Valid before the first rt_ui_tick, which is what
 * the launcher needs: it rasterises the title image at the pixel size this
 * implies before the first frame is presented. */
float ui_density_ratio();

/* ---- the in-memory texture schemes ---------------------------------------
 *
 * Two images are built at run time and live in memory, not in a file: the
 * launcher's title image, built from the user's disc (ui/title_logo.h), and
 * the drawn menu cursor cut out of it (ui/cursor_image.h). A document names
 * one with its scheme in an `src` attribute; UiSystemInterface::JoinPath
 * passes such a source through untouched and UiRenderInterface::LoadTexture
 * answers it from the published bytes. Nothing else is servable: this build
 * has no file image loader.
 *
 * The text after the scheme is free: nothing reads it, and the publisher
 * varies it so that RmlUi, which caches a texture by its source string and
 * never re-asks for one it has, fetches a re-rasterised image.
 */
inline constexpr char kLogoScheme[] = "logo:";
inline constexpr char kCursorScheme[] = "cursor:";

/* Publishes the RGBA8 image a scheme serves. `scheme` must be one of the two
 * above. `rgba` is width * height * 4 bytes with alpha premultiplied, and is
 * copied. Returns false, having logged, for an unknown scheme or an empty or
 * zero-sized image.
 *
 * Call this before the flag that puts the element in the document: RmlUi
 * caches a texture load that returned nothing and never asks again. */
bool ui_render_set_image(const char* scheme, const uint8_t* rgba, uint32_t width, uint32_t height);

/* The title image's spelling of the same call, kept because the launcher is
 * the older caller and reads better this way. */
bool ui_render_set_logo(const uint8_t* rgba, uint32_t width, uint32_t height);

/* True once if LoadTexture was asked for that scheme's published image and
 * could not hand a texture back, which for the backend means the GPU upload
 * failed. Reading it clears it, and publishing a new image clears it too.
 *
 * The raster succeeding says nothing about the upload: the two happen in
 * different places and only this side knows the second answer. Without it a
 * failed upload leaves the document showing an element whose texture is
 * missing, with its fallback switched off, which is a blank box. The
 * launcher polls the logo's and puts its text title back; the drawn cursor
 * polls its own and goes back to the arrow built out of borders. */
bool ui_render_take_image_upload_failure(const char* scheme);
bool ui_render_take_logo_upload_failure();

/* The bytes a scheme currently serves, or null when nothing is published
 * under it. Valid until the next ui_render_set_image() for that scheme. The
 * drawn cursor reads the title image back through this when the window scale
 * moves, so it can re-cut its glyph at the new density without a second copy
 * of the image living anywhere. */
const uint8_t* ui_render_image_bytes(const char* scheme, uint32_t* width, uint32_t* height);

/* ---- render interface ---------------------------------------------------
 *
 * Geometry is pooled on this side of the ABI: CompileGeometry copies the
 * converted vertices and indices into a pool entry, and each RenderGeometry
 * appends that entry into the flat arrays being built for this tick plus one
 * RtPgsOverlayCmd. One RtPgsOverlayFrame leaves per tick; only textures
 * cross the boundary as GPU resources.
 */
class UiRenderInterface final : public Rml::RenderInterface {
public:
    /* Frame construction, driven by rt_ui_tick around context->Render(). */
    void begin_frame();
    const RtPgsOverlayFrame& frame(uint32_t surface_width, uint32_t surface_height);

    /* Required. */
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f* transform) override;

    /* Unsupported in this build: each logs once, naming itself, and does the
     * least destructive thing it can. The stylesheets under ui/ avoid the
     * properties that reach them (border-radius, box-shadow, gradients,
     * filters, container opacity, clip masks), so a line from any of these
     * means a stylesheet grew a property this renderer cannot draw. */
    void EnableClipMask(bool enable) override;
    void RenderToClipMask(Rml::ClipMaskOperation operation, Rml::CompiledGeometryHandle geometry,
                          Rml::Vector2f translation) override;
    Rml::LayerHandle PushLayer() override;
    void CompositeLayers(Rml::LayerHandle source, Rml::LayerHandle destination, Rml::BlendMode blend_mode,
                         Rml::Span<const Rml::CompiledFilterHandle> filters) override;
    void PopLayer() override;
    Rml::TextureHandle SaveLayerAsTexture() override;
    Rml::CompiledFilterHandle SaveLayerAsMaskImage() override;
    Rml::CompiledFilterHandle CompileFilter(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void ReleaseFilter(Rml::CompiledFilterHandle filter) override;
    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                      Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;

private:
    struct PooledGeometry {
        std::vector<RtPgsOverlayVertex> vertices;
        std::vector<uint32_t> indices;
        bool in_use = false;
    };

    std::vector<PooledGeometry> m_pool;
    std::vector<size_t> m_free; /* indices into m_pool, reused by CompileGeometry */

    std::vector<RtPgsOverlayVertex> m_vertices;
    std::vector<uint32_t> m_indices;
    std::vector<RtPgsOverlayCmd> m_cmds;
    RtPgsOverlayFrame m_frame = {};

    bool m_scissor_enabled = false;
    Rml::Rectanglei m_scissor = {};
    bool m_has_transform = false;
    Rml::Matrix4f m_transform;

    /* Sources LoadTexture refused. The "logo:" scheme above is the only one
     * it serves; there is no file loader, so every other source lands here
     * and gets one log line, which keeps a repeated draw quiet. */
    std::set<std::string> m_missing_textures;

    /* Deferred texture destruction. RmlUi releases a texture the moment it
     * drops the last reference, which for a glyph atlas can happen partway
     * through Context::Render() while commands already emitted this tick,
     * and the frame the library still holds from the previous tick, name
     * that id. See ReleaseTexture in ui_render.cpp for the full reasoning
     * and for the two-tick rule begin_frame() applies. */
    struct PendingTextureDestroy {
        uint32_t texture;
        uint64_t tick;
    };
    std::vector<PendingTextureDestroy> m_pending_destroy;
    uint64_t m_tick = 0;
};

/* ---- system interface --------------------------------------------------- */

class UiSystemInterface final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
    void JoinPath(Rml::String& translated_path, const Rml::String& document_path,
                  const Rml::String& path) override;
    void SetMouseCursor(const Rml::String& cursor_name) override;
    void SetClipboardText(const Rml::String& text) override;
    void GetClipboardText(Rml::String& text) override;

private:
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
};

/* ---- module state ------------------------------------------------------- */

struct UiState {
    bool initialized = false;
    bool visible = false;
    /* display.show_fps, mirrored here because rt_ui_tick has to know whether
     * anything at all is on screen before it decides to render. The fps
     * document is shown and hidden by settings_model_refresh(). */
    bool fps_visible = false;
    /* Whether the drawn cursor for the game's own menus is up, mirrored here
     * for the same reason: the tick has to know whether anything at all is
     * on screen before it decides to render. ui_menu_cursor_tick() shows and
     * hides the document. */
    bool cursor_visible = false;
    /* True for the whole of rt_launcher_run(), from the Show() of
     * launcher.rml to the Hide() that hands off to the game. Like the two
     * flags above it makes the tick render; unlike them it also makes
     * rt_ui_wants_input() true and takes the menu hotkey out of service
     * (ui_events.cpp), because the launcher owns the window while it is up. */
    bool launcher_visible = false;
    /* Set by rt_ui_set_visible(false), consumed by the next rt_ui_tick: the
     * menu closes from inside the event pump, and the settings file write
     * belongs at the field boundary. */
    bool flush_save_pending = false;
    /* Set when the menu is shown, consumed by the next rt_ui_tick after
     * Context::Update(): the pane and shell heights the log line reports
     * are only laid out once the context has run. */
    bool menu_metrics_pending = false;
    /* True while the library holds an overlay frame from us. Drives the one
     * rt_pgs_overlay_set_frame(NULL) that clears the overlay on hide. */
    bool frame_posted = false;
    uint32_t surface_width = 0, surface_height = 0;

    Rml::Context* context = nullptr;
    Rml::ElementDocument* menu = nullptr;
    Rml::ElementDocument* fps = nullptr;
    /* Loaded once at init and shown and hidden from ui_launcher.cpp. */
    Rml::ElementDocument* launcher = nullptr;
    /* Loaded once at init and shown and hidden from ui_menu_cursor.cpp. */
    Rml::ElementDocument* cursor = nullptr;
    UiRenderInterface* render = nullptr;
    UiSystemInterface* system = nullptr;
};

extern UiState g_ui;

/* ---- settings data model (ui_settings_model.cpp) -------------------------
 *
 * One Rml data model named "settings", bound to a UI-side mirror of
 * RtSettings plus the per-key "overridden by ICORECOMP_X" flags, the active
 * tab and the fps readout text. The documents under ui/ read and write the
 * mirror; nothing there touches rt_settings() directly.
 */

/* Creates the data model on the context. Must run after the context exists
 * and before any document that carries data-model="settings" is loaded,
 * because a document binds its views at load time. Returns false, having
 * logged, when the model could not be created. */
bool settings_model_init(Rml::Context* context);

/* Copies rt_settings() into the mirror, re-reads the override flags and the
 * settings path, dirties every bound variable, and shows or hides the fps
 * document to match display.show_fps. Called when the menu opens and after
 * every commit: commit-time validation can revert a value, and the menu has
 * to show what was actually kept, not what was typed. */
void settings_model_refresh();

/* The model's half of rt_ui_tick, at the field boundary: applies a control
 * change queued by an event callback (see the file comment in
 * ui_settings_model.cpp for why it is queued rather than applied inline) and
 * refreshes the fps readout text a few times a second. */
void settings_model_tick();

/* The Input pane's label for one binding slot ("Cross", "Left stick up",
 * "Menu key"). `device` selects the table. Out of range returns "?".
 * ui_rebind.cpp names the conflicting slot in its inline reject message
 * with this. */
const char* bind_slot_label(RtBindDevice device, int slot);

/* The device's name as the log lines and the pane spell it: "keyboard",
 * "gamepad", "mouse", or "?" for a value outside the enum. One copy, here,
 * because both this file's tick and ui_rebind.cpp's log lines need it. */
const char* bind_device_name(RtBindDevice device);

/* Puts the Input pane into or out of the "waiting for input" state for one
 * slot and sets the inline status line under the binding tables. `active`
 * false ends the capture; `slot` is then ignored and the tables are read
 * back from the settings. `status` is shown as-is and may be empty. Called
 * only from ui_rebind.cpp. */
void settings_model_set_rebind(bool active, RtBindDevice device, int slot, const std::string& status);

/* Focuses the nav button for whichever tab active_tab currently names
 * ("nav-display" .. "nav-debug" in menu.rml). Called from rt_ui_set_
 * visible(true) after Show(): Show()'s own FocusFlag::Auto default focuses
 * the first tab-index element in document order (always the Display tab),
 * which is wrong whenever the menu was left open on a different tab last
 * time. Also called by settings_model_cycle_tab below, after it moves the
 * tab, so the pad's focus ring always sits on the tab actually showing. */
void settings_model_focus_active_tab();

/* Steps active_tab by +1 or -1 through the five tabs, wrapping, dirties it,
 * and focuses the new tab's nav button. A no-op while the menu is not open
 * (the launcher has no tabs). Called from ui_events.cpp's gamepad shoulder
 * handling (L1/R1), and from its level-1 Left/Right (the two-level pad
 * model below). */
void settings_model_cycle_tab(int direction);

/* ---- two-level pad model (menu.rml's cards column + its pane) ------------
 *
 * ui_events.cpp decides which of the two levels a gamepad direction or
 * South applies to by walking up from whatever has focus (a .nav-button
 * ancestor means level 1, a .pane ancestor means level 2), never from state
 * of its own, so a mouse click can never leave the two disagreeing. These
 * three are what level 1 needs beyond settings_model_cycle_tab (Left/Right)
 * and settings_model_focus_active_tab (East, going back from level 2).
 */

/* Moves the pad's focus ring by +1 or -1 among the five cards, from
 * whichever one currently has focus, wrapping at the ends. Unlike
 * settings_model_cycle_tab, this never touches active_tab or which pane is
 * showing: it is level 1's Up/Down, which only relocates the cursor among
 * the cards. A no-op if focus is not currently on one of the five cards
 * (ui_events.cpp only calls it once it already knows that). */
void settings_model_focus_card(int direction);

/* Level 1's South: makes the focused card's tab the active one, if it
 * differs, and queues the move into the now-visible pane for settings_
 * model_post_update() below. A no-op if focus is not currently on one of
 * the five cards. */
void settings_model_enter_card();

/* The queued half of settings_model_enter_card: focuses the first focusable
 * control in the pane. Called from rt_ui_tick after Rml::Context::Update(),
 * and only from there, because that is the call that re-runs the data-if
 * deciding which .section is displayed. Doing it inside enter_card would
 * walk the pane one field stale. A no-op with nothing queued or with the
 * menu not up. */
void settings_model_post_update();

/* Focuses the first element inside the visible pane (menu.rml's <div
 * id="pane">, the one .section a data-if is currently showing) that has
 * tab-index: auto -- a pre-order walk of the pane's descendants, skipping
 * every subtree whose own display is none (a hidden .section, or anything
 * else data-if or a style rule hid), stopping at the first element that is
 * both visible and focusable by RmlUi's own definition (ElementDocument.cpp
 * CanFocusElement: visible, focus != none, tab-index: auto). Called by
 * settings_model_enter_card above; a no-op if the pane has nothing
 * focusable (should not happen, every section carries at least one
 * control). */
void settings_model_focus_first_in_pane();

/* Rewrites `hint` for the last device used (pad or keyboard) and dirties
 * "nav_hint" on `model` when it changed. `two_level` picks the settings
 * menu's wording (cards and pane, East backs out, Escape closes) over the
 * launcher's, whose navigation is flat and where neither East nor Escape
 * does anything. ui_settings_model.cpp. */
void sync_nav_hint(std::string* hint, Rml::DataModelHandle* model, bool two_level);

/* Clears the "press again to quit" arm and its label immediately, without
 * waiting for the tick's own 3 s timeout. Called from rt_ui_set_visible(
 * false) so every way of closing the menu (Close, Escape, gamepad East)
 * disarms it, not only a reopen (settings_model_refresh() calls this too)
 * or the timeout. A no-op when nothing is armed. */
void settings_model_disarm_quit();

/* ---- launcher (ui_launcher.cpp) -----------------------------------------
 *
 * The "launcher" data model (separate from "settings": it holds the disc
 * state and the precheck result, none of which is a setting) plus the
 * document that reads it. Called from rt_ui_init: the model has to exist
 * before launcher.rml is parsed.
 *
 * Returns false, having logged, when the model or launcher.rml could not be
 * created. That disables the launcher (rt_launcher_run then returns true
 * straight away) without disabling the settings menu. */
bool launcher_init(Rml::Context* context, const std::string& ui_dir);

/* Takes launcher.rml down while the settings menu is open on top of it, and
 * puts it back when the menu closes. The menu's backdrop is translucent
 * because the game's scanout belongs behind it; over the launcher there is
 * no scanout, only another document, and the two read as one jumbled
 * screen.
 *
 * g_ui.launcher_visible stays true throughout: it means "the launcher owns
 * the window", which is still true with the menu over it, and both
 * rt_launcher_run's loop and rt_ui_wants_input() depend on that meaning.
 * Does nothing when the launcher is not up, which is the in-game case. */
void launcher_set_covered(bool covered);

/* ---- drawn menu cursor (ui_menu_cursor.cpp) ------------------------------
 *
 * The "menucursor" data model and the document that reads it: an arrow drawn
 * inside the presented picture while the pointer owns the mouse on one of
 * the game's own menus. Relative mouse mode stays on there, so the OS cursor
 * is hidden and this is the only cursor on screen. Everything it shows comes
 * from guest/menu_nav.cpp, and it takes no events.
 *
 * Called from rt_ui_init, with the same failure contract as the fps
 * readout: false, having logged, costs the drawn cursor and nothing else. */
bool ui_menu_cursor_init(Rml::Context* context, const std::string& ui_dir);

/* Cuts the letter I out of a freshly published title image, turns it into a
 * cursor (ui/cursor_image.h) and publishes it under kCursorScheme. Called
 * from wherever the title image is published (raster_title_logo in
 * ui_launcher.cpp), so the cursor exists from the same moment the logo does,
 * at the same density, and is re-cut whenever the logo is.
 *
 * Never fatal and never partly done: anything that goes wrong logs and
 * leaves the document with the arrow built out of borders, or with the
 * previous image if there was one. Safe before ui_menu_cursor_init(): it
 * only fills the model and publishes bytes. */
void ui_menu_cursor_build_from_logo(const uint8_t* rgba, uint32_t width, uint32_t height);

/* Shows or hides the document to match "the pointer owns the mouse, relative
 * mode is on so the OS cursor is hidden, and there is a position to point
 * with" and, while it is up, refreshes the model. Called from rt_ui_tick
 * at the field boundary and before the tick's "nothing is up" early-out,
 * which counts g_ui.cursor_visible. */
void ui_menu_cursor_tick();

#ifdef ICORECOMP_PGS_SDL
/* Resolves input.keyboard[RT_KB_MENU] / input.gamepad[RT_GP_MENU] into an
 * SDL scancode and gamepad button, once, at init. An unresolvable name logs
 * and keeps the compiled-in default for that binding (never "no binding").
 * Defined in ui_events.cpp, the only file here that sees SDL. */
void resolve_menu_hotkey();
/* SDL's name for the resolved keyboard hotkey, for the menu document's
 * "press X to close" line. Valid after resolve_menu_hotkey(). */
const char* menu_hotkey_name();
/* The resolved gamepad hotkey's name, for the same line and the startup
 * log: one SDL button name ("guide"), or "first+second" when input.gamepad.
 * menu is a chord ("back+start"). Valid after resolve_menu_hotkey(). */
const char* menu_gamepad_name();
/* SDL3 delivers SDL_EVENT_TEXT_INPUT only between SDL_StartTextInput and
 * SDL_StopTextInput, so the menu turns it on while it is up and off again
 * when it closes. Called from rt_ui_set_visible (ui.cpp), which is why it
 * lives behind a wrapper: ui.cpp includes no SDL headers. Both are plain SDL
 * calls and are legal from the event pump. */
void menu_set_text_input(bool enabled);

/* ---- rebind capture (ui_rebind.cpp) -------------------------------------
 *
 * A capture takes over the events of the device being bound while it is up:
 * the pump routes every event here first, ahead of the menu hotkey and
 * RmlUi (rt_ui_handle_sdl_event). A keyboard or gamepad capture consumes
 * only keyboard and gamepad events, so the mouse still drives the menu
 * underneath; a mouse capture also consumes mouse motion, buttons and the
 * wheel, because the button the user presses has to become the binding
 * rather than activating whatever it is hovering.
 *
 * The accepted name is written to the settings and committed from
 * rebind_tick(), at the field boundary, for the same reason every other
 * settings change in this module is (see the file comment in
 * ui_settings_model.cpp): a commit runs the appliers and those touch the
 * window and the GS library.
 */

/* Starts capturing for one slot on one device. Safe from an event callback:
 * it only sets state and updates the data model. */
void rebind_begin(RtBindDevice device, int slot);

/* True between rebind_begin() and the accept, cancel or timeout that ends
 * the capture, and for as long after an accepted mouse button as its
 * release is still owed (ui_rebind.cpp swallows that release). The pump
 * tests this before handing an event to rebind_handle_sdl_event. */
bool rebind_active();

/* Cancels a capture in progress, if any, with `reason` in the log. Called
 * when the menu closes: an armed capture never outlives the menu, which is
 * what keeps rt_ui_wants_input() true for the whole of one.
 *
 * A capture that was already accepted is left alone by default. Its name is
 * parked for rebind_tick(), which rt_ui_tick calls whether the menu is up or
 * not, so it is written at the coming field boundary rather than dropped by
 * a menu that closed in the same field. `drop_accepted` throws it away as
 * well, and is for the callers that are rewriting the same table underneath
 * it (the resets and the mouse unbind in ui_settings_model.cpp), where
 * applying it afterwards would undo what they just did. */
void rebind_cancel(const char* reason, bool drop_accepted = false);

/* Returns true when the capture consumed the event. */
bool rebind_handle_sdl_event(const SDL_Event& e);

/* The field-boundary half: applies an accepted capture and expires one that
 * has been waiting too long. Called from rt_ui_tick. */
void rebind_tick();

/* ---- gamepad navigation (ui_events.cpp) ----------------------------------
 *
 * Held-direction repeat and the select pad-session both need a field
 * boundary: a NavHold's repeat cadence is a clock, not an event, and a
 * select session has to notice its element losing focus by some means
 * other than an event the loss might not raise here at all (a mouse click
 * elsewhere is handled by RmlUi itself, not by this module). Called from
 * rt_ui_tick after rebind_tick, whether or not the menu is up: with nothing
 * visible it only clears whatever a hold or a session still thinks it owns,
 * so neither survives into the next time the menu opens.
 */
void ui_nav_tick();
#endif

} // namespace rtui

#endif /* ICORECOMP_UI */

#endif /* ICORECOMP_UI_UI_INTERNAL_H */
