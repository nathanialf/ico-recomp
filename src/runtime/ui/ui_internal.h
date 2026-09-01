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
uint32_t backend_texture_create(const uint8_t* rgba8, uint32_t width, uint32_t height);
void backend_texture_destroy(uint32_t texture);
void backend_set_frame(const RtPgsOverlayFrame* frame);
/* SDL_Window* as void*, for the window-to-surface coordinate scale in
 * ui_events.cpp. NULL when there is no window. */
void* backend_window_handle();

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

    /* LoadTexture has no implementation in v1 (text and solid boxes only);
     * one log line per distinct source keeps a repeated draw quiet. */
    std::set<std::string> m_missing_textures;
};

/* ---- system interface --------------------------------------------------- */

class UiSystemInterface final : public Rml::SystemInterface {
public:
    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
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
    /* Set by rt_ui_set_visible(false), consumed by the next rt_ui_tick: the
     * menu closes from inside the event pump, and the settings file write
     * belongs at the field boundary. */
    bool flush_save_pending = false;
    /* True while the library holds an overlay frame from us. Drives the one
     * rt_pgs_overlay_set_frame(NULL) that clears the overlay on hide. */
    bool frame_posted = false;
    uint32_t surface_width = 0, surface_height = 0;

    Rml::Context* context = nullptr;
    Rml::ElementDocument* menu = nullptr;
    Rml::ElementDocument* fps = nullptr;
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
 * "Menu key"). `gamepad` selects the RtPadBind slots over the RtKeyBind
 * ones. Out of range returns "?". ui_rebind.cpp names the conflicting slot
 * in its inline reject message with this. */
const char* bind_slot_label(bool gamepad, int slot);

/* Puts the Input pane into or out of the "waiting for input" state for one
 * slot and sets the inline status line under the binding tables. `active`
 * false ends the capture; `slot` is then ignored and the tables are read
 * back from the settings. `status` is shown as-is and may be empty. Called
 * only from ui_rebind.cpp. */
void settings_model_set_rebind(bool active, bool gamepad, int slot, const std::string& status);

#ifdef ICORECOMP_PGS_SDL
/* Resolves input.keyboard[RT_KB_MENU] / input.gamepad[RT_GP_MENU] into an
 * SDL scancode and gamepad button, once, at init. An unresolvable name logs
 * and keeps the compiled-in default for that binding (never "no binding").
 * Defined in ui_events.cpp, the only file here that sees SDL. */
void resolve_menu_hotkey();
/* SDL's name for the resolved keyboard hotkey, for the menu document's
 * "press X to close" line. Valid after resolve_menu_hotkey(). */
const char* menu_hotkey_name();
/* SDL3 delivers SDL_EVENT_TEXT_INPUT only between SDL_StartTextInput and
 * SDL_StopTextInput, so the menu turns it on while it is up and off again
 * when it closes. Called from rt_ui_set_visible (ui.cpp), which is why it
 * lives behind a wrapper: ui.cpp includes no SDL headers. Both are plain SDL
 * calls and are legal from the event pump. */
void menu_set_text_input(bool enabled);

/* ---- rebind capture (ui_rebind.cpp) -------------------------------------
 *
 * A capture takes over the keyboard and gamepad events while it is up: the
 * pump routes every event here first, ahead of the menu hotkey and RmlUi
 * (rt_ui_handle_sdl_event). Mouse and window events still fall through, so
 * the menu underneath keeps working and the click that started the capture
 * gets its matching release.
 *
 * The accepted name is written to the settings and committed from
 * rebind_tick(), at the field boundary, for the same reason every other
 * settings change in this module is (see the file comment in
 * ui_settings_model.cpp): a commit runs the appliers and those touch the
 * window and the GS library.
 */

/* Starts capturing for one slot. Safe from an event callback: it only sets
 * state and updates the data model. */
void rebind_begin(bool gamepad, int slot);

/* True between rebind_begin() and the accept, cancel or timeout that ends
 * the capture. */
bool rebind_active();

/* Cancels a capture in progress, if any, with `reason` in the log. Called
 * when the menu closes: a capture never outlives the menu, which is what
 * keeps rt_ui_wants_input() true for the whole of it. */
void rebind_cancel(const char* reason);

/* Returns true when the capture consumed the event. */
bool rebind_handle_sdl_event(const SDL_Event& e);

/* The field-boundary half: applies an accepted capture and expires one that
 * has been waiting too long. Called from rt_ui_tick. */
void rebind_tick();
#endif

} // namespace rtui

#endif /* ICORECOMP_UI */

#endif /* ICORECOMP_UI_UI_INTERNAL_H */
