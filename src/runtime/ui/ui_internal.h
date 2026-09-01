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
    /* True while the library holds an overlay frame from us. Drives the one
     * rt_pgs_overlay_set_frame(NULL) that clears the overlay on hide. */
    bool frame_posted = false;
    uint32_t surface_width = 0, surface_height = 0;

    Rml::Context* context = nullptr;
    Rml::ElementDocument* menu = nullptr;
    UiRenderInterface* render = nullptr;
    UiSystemInterface* system = nullptr;
};

extern UiState g_ui;

#ifdef ICORECOMP_PGS_SDL
/* Resolves input.keyboard[RT_KB_MENU] / input.gamepad[RT_GP_MENU] into an
 * SDL scancode and gamepad button, once, at init. An unresolvable name logs
 * and keeps the compiled-in default for that binding (never "no binding").
 * Defined in ui_events.cpp, the only file here that sees SDL. */
void resolve_menu_hotkey();
/* SDL's name for the resolved keyboard hotkey, for the menu document's
 * "press X to close" line. Valid after resolve_menu_hotkey(). */
const char* menu_hotkey_name();
#endif

} // namespace rtui

#endif /* ICORECOMP_UI */

#endif /* ICORECOMP_UI_UI_INTERNAL_H */
