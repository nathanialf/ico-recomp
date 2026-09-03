/* ui/ui_render.cpp: RmlUi's RenderInterface on top of the overlay ABI
 * (gs/gs_parallel_api.h).
 *
 * The whole render path is a data transform: RmlUi's geometry becomes one
 * flat RtPgsOverlayFrame per tick, and the only GPU resources that cross the
 * boundary are overlay textures. Nothing here touches Vulkan; the library
 * owns the pipeline (RtPgs::draw_overlay).
 *
 * Alpha: RmlUi 6 hands out premultiplied vertex colors (Rml::Vertex::colour
 * is ColourbPremultiplied) and premultiplied texture bytes
 * (RenderInterface::GenerateTexture's contract), so every command emitted
 * here sets RT_PGS_OVERLAY_PREMULTIPLIED.
 *
 * Calling rules: RmlUi calls this only from Context::Update() and
 * Context::Render(), both of which run inside rt_ui_tick, between frames.
 * That is what makes the texture create/destroy calls legal here.
 */
#include "ui.h"

#ifdef ICORECOMP_UI

#include "ui_internal.h"

#include "../runtime.h"

#include <RmlUi/Core/Matrix4.h>

#include <cstring>
#include <vector>

namespace rtui {

namespace {

/* The one image this renderer serves through LoadTexture, published by
 * ui_render_set_logo(). Held as bytes rather than as an uploaded texture
 * because RmlUi decides when it wants the upload, and it can release and ask
 * again (a resolution change re-creates the render manager's textures). */
std::vector<uint8_t> g_logo_rgba;
uint32_t g_logo_width = 0;
uint32_t g_logo_height = 0;
/* Set when a load of that image could not produce a texture. See
 * ui_render_take_logo_upload_failure in ui_internal.h. */
bool g_logo_upload_failed = false;

/* R8G8B8A8_UNORM byte order, matching RtPgsOverlayVertex::rgba and
 * gs_parallel.cpp's pack_rgba: red in the low byte. */
uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
}

/* One log line the first time each unimplemented RenderInterface function is
 * reached, naming itself. Loud enough to find in a log, quiet enough not to
 * flood one at 60 fields per second. */
void log_unsupported_once(bool& logged, const char* function) {
    if (logged) return;
    logged = true;
    rt_log("ui", "RenderInterface::%s is not implemented in this build;"
                 " the affected element draws without that effect."
                 " A stylesheet under ui/ asked for something this renderer cannot draw.",
        function);
}

} // namespace

void UiRenderInterface::begin_frame() {
    /* Retire the textures released two or more ticks ago, before anything of
     * this tick is built. ReleaseTexture explains the rule; the short form is
     * that the newest frame the library can still be holding was built in the
     * previous tick, so an id released during tick N is only unreferenced
     * everywhere once tick N+2 starts. */
    ++m_tick;
    size_t kept = 0;
    for (const PendingTextureDestroy& p : m_pending_destroy) {
        if (p.tick + 2 <= m_tick) {
            backend_texture_destroy(p.texture);
        } else {
            m_pending_destroy[kept++] = p;
        }
    }
    m_pending_destroy.resize(kept);

    m_vertices.clear();
    m_indices.clear();
    m_cmds.clear();
    /* Scissor and transform are per-tick render state in RmlUi; it sets both
     * before it needs them, but starting clean means a tick can never
     * inherit the previous one's clip. */
    m_scissor_enabled = false;
    m_has_transform = false;
}

const RtPgsOverlayFrame& UiRenderInterface::frame(uint32_t surface_width, uint32_t surface_height) {
    m_frame.vertices = m_vertices.data();
    m_frame.vertex_count = uint32_t(m_vertices.size());
    m_frame.indices = m_indices.data();
    m_frame.index_count = uint32_t(m_indices.size());
    m_frame.cmds = m_cmds.data();
    m_frame.cmd_count = uint32_t(m_cmds.size());
    m_frame.surface_width = surface_width;
    m_frame.surface_height = surface_height;
    return m_frame;
}

Rml::CompiledGeometryHandle UiRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                              Rml::Span<const int> indices) {
    size_t slot;
    if (!m_free.empty()) {
        slot = m_free.back();
        m_free.pop_back();
    } else {
        m_pool.emplace_back();
        slot = m_pool.size() - 1;
    }

    PooledGeometry& g = m_pool[slot];
    g.in_use = true;
    g.vertices.clear();
    g.vertices.reserve(vertices.size());
    for (const Rml::Vertex& v : vertices) {
        RtPgsOverlayVertex out;
        out.x = v.position.x;
        out.y = v.position.y;
        out.u = v.tex_coord.x;
        out.v = v.tex_coord.y;
        out.rgba = pack_rgba(v.colour.red, v.colour.green, v.colour.blue, v.colour.alpha);
        g.vertices.push_back(out);
    }
    g.indices.clear();
    g.indices.reserve(indices.size());
    for (int i : indices) g.indices.push_back(uint32_t(i));

    /* Handle 0 is RmlUi's "could not compile", so the pool index is offset
     * by one. */
    return Rml::CompiledGeometryHandle(slot + 1);
}

void UiRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                                       Rml::TextureHandle texture) {
    const size_t slot = size_t(geometry) - 1;
    if (geometry == 0 || slot >= m_pool.size() || !m_pool[slot].in_use) {
        rt_log("ui", "RenderGeometry: unknown geometry handle %llu; the draw is dropped",
            (unsigned long long)geometry);
        return;
    }
    const PooledGeometry& g = m_pool[slot];
    if (g.indices.empty()) return;

    RtPgsOverlayCmd cmd = {};
    cmd.texture = uint32_t(texture);
    cmd.index_offset = uint32_t(m_indices.size());
    cmd.index_count = uint32_t(g.indices.size());
    cmd.vertex_offset = int32_t(m_vertices.size());
    cmd.flags = RT_PGS_OVERLAY_PREMULTIPLIED;

    if (m_scissor_enabled) {
        cmd.flags |= RT_PGS_OVERLAY_SCISSOR;
        cmd.scissor_x = m_scissor.Left();
        cmd.scissor_y = m_scissor.Top();
        cmd.scissor_w = m_scissor.Width();
        cmd.scissor_h = m_scissor.Height();
    }

    if (m_has_transform) {
        /* RmlUi's convention is transform * (position + translation): the
         * translation happens in the transformed element's own space. The
         * overlay ABI's convention is the other way round (its mvp adds
         * translate into the transform's translation column, i.e. after the
         * transform), so fold the translation into the matrix here and leave
         * the command's translate at zero. Matrix4f is column-major by
         * default (RMLUI_MATRIX_ROW_MAJOR is off) and data() returns that
         * storage directly, which is the layout RtPgsOverlayCmd::transform
         * documents. */
        cmd.flags |= RT_PGS_OVERLAY_TRANSFORM;
        const Rml::Matrix4f combined =
            m_transform * Rml::Matrix4f::Translate(translation.x, translation.y, 0.0f);
        std::memcpy(cmd.transform, combined.data(), sizeof(cmd.transform));
    } else {
        cmd.translate_x = translation.x;
        cmd.translate_y = translation.y;
    }

    m_vertices.insert(m_vertices.end(), g.vertices.begin(), g.vertices.end());
    m_indices.insert(m_indices.end(), g.indices.begin(), g.indices.end());
    m_cmds.push_back(cmd);
}

void UiRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry) {
    const size_t slot = size_t(geometry) - 1;
    if (geometry == 0 || slot >= m_pool.size() || !m_pool[slot].in_use) {
        rt_log("ui", "ReleaseGeometry: unknown geometry handle %llu",
            (unsigned long long)geometry);
        return;
    }
    PooledGeometry& g = m_pool[slot];
    g.in_use = false;
    /* Keep the vectors' capacity: the slot is about to be handed to the next
     * CompileGeometry, and RmlUi recompiles the same shapes constantly. */
    g.vertices.clear();
    g.indices.clear();
    m_free.push_back(slot);
}

bool ui_render_set_logo(const uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) {
        rt_log("ui", "the title logo image is %ux%u; nothing to publish", width, height);
        return false;
    }
    g_logo_rgba.assign(rgba, rgba + size_t(width) * size_t(height) * 4u);
    g_logo_width = width;
    g_logo_height = height;
    g_logo_upload_failed = false; /* a new image gets a fresh answer */
    rt_log("ui", "title logo published under the \"%s\" scheme: %ux%u, %zu bytes", kLogoScheme,
        width, height, g_logo_rgba.size());
    return true;
}

bool ui_render_take_logo_upload_failure() {
    const bool failed = g_logo_upload_failed;
    g_logo_upload_failed = false;
    return failed;
}

/* The only texture source this build serves. There is no file loader behind
 * it on purpose: reading an arbitrary path off disk because a stylesheet
 * named one is not something the overlay renderer does. Anything else logs
 * once per distinct source and draws untextured. */
Rml::TextureHandle UiRenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) {
    texture_dimensions = Rml::Vector2i(0, 0);

    if (source.compare(0, sizeof(kLogoScheme) - 1, kLogoScheme) == 0) {
        if (g_logo_rgba.empty()) {
            /* RmlUi caches a failed load and never retries it, so the
             * launcher only puts the element in the document once the image
             * exists (the logo_available flag in ui_launcher.cpp). Reaching
             * this means those two went out of step. */
            rt_log("ui", "LoadTexture(%s): no title logo has been published;"
                         " the element draws untextured", source.c_str());
            g_logo_upload_failed = true;
            return Rml::TextureHandle(0);
        }
        const uint32_t id = backend_texture_create(g_logo_rgba.data(), g_logo_width, g_logo_height);
        if (id == 0) {
            rt_log("ui", "LoadTexture(%s): the %ux%u upload failed; the element draws untextured",
                source.c_str(), g_logo_width, g_logo_height);
            g_logo_upload_failed = true;
            return Rml::TextureHandle(0);
        }
        texture_dimensions = Rml::Vector2i(int(g_logo_width), int(g_logo_height));
        rt_log("ui", "LoadTexture(%s): %ux%u uploaded as overlay texture %u", source.c_str(),
            g_logo_width, g_logo_height, id);
        return Rml::TextureHandle(id);
    }

    if (m_missing_textures.insert(std::string(source)).second) {
        rt_log("ui", "LoadTexture(%s): file images are not implemented in this build;"
                     " the element draws untextured", source.c_str());
    }
    return Rml::TextureHandle(0);
}

Rml::TextureHandle UiRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                      Rml::Vector2i source_dimensions) {
    if (source_dimensions.x <= 0 || source_dimensions.y <= 0) {
        rt_log("ui", "GenerateTexture: refusing %dx%d texture", source_dimensions.x, source_dimensions.y);
        return Rml::TextureHandle(0);
    }
    const size_t expected = size_t(source_dimensions.x) * size_t(source_dimensions.y) * 4u;
    if (source.size() < expected) {
        rt_log("ui", "GenerateTexture: %zu bytes for a %dx%d RGBA texture (%zu needed)",
            source.size(), source_dimensions.x, source_dimensions.y, expected);
        return Rml::TextureHandle(0);
    }
    const uint32_t id = backend_texture_create(reinterpret_cast<const uint8_t*>(source.data()),
                                               uint32_t(source_dimensions.x),
                                               uint32_t(source_dimensions.y));
    /* id 0 means the upload failed; the library already logged why. RmlUi
     * treats 0 as "could not generate" and draws untextured. */
    return Rml::TextureHandle(id);
}

void UiRenderInterface::ReleaseTexture(Rml::TextureHandle texture) {
    if (texture == 0) return;
    /* Deferred, not immediate. RmlUi can release a glyph atlas in the middle
     * of Context::Render(): a glyph that is new to a font face forces the
     * layer to regenerate, FontFaceLayer::Generate clears textures_owned
     * (third_party/rmlui/Source/Core/FontEngineDefault/FontFaceLayer.cpp:26)
     * and RenderManager calls straight through to ReleaseTexture
     * (Source/Core/RenderManager.cpp:342). Two things still name the id at
     * that moment: the commands this tick already emitted, and the frame the
     * library retained from the last tick. Destroying it there made
     * rt_pgs_overlay_set_frame reject the whole frame ("references unknown
     * texture"), or left the already-retained frame drawing white boxes.
     *
     * So record the id with the tick that released it and let begin_frame()
     * destroy it once two ticks have started. An id released anywhere in
     * tick N is unreferenced by the frame built in tick N+1, and that frame
     * has replaced the retained one by the time tick N+2 begins. */
    m_pending_destroy.push_back(PendingTextureDestroy{ uint32_t(texture), m_tick });
}

void UiRenderInterface::EnableScissorRegion(bool enable) {
    m_scissor_enabled = enable;
}

void UiRenderInterface::SetScissorRegion(Rml::Rectanglei region) {
    m_scissor = region;
}

void UiRenderInterface::SetTransform(const Rml::Matrix4f* transform) {
    m_has_transform = transform != nullptr;
    if (transform) m_transform = *transform;
}

/* ---- unsupported ---------------------------------------------------------
 *
 * Layers, filters, shaders and the clip mask have no overlay-ABI equivalent
 * in v1. Each logs once and returns the least destructive value it can: a
 * zero handle (RmlUi's "not available") or nothing at all. */

void UiRenderInterface::EnableClipMask(bool /*enable*/) {
    static bool logged = false;
    log_unsupported_once(logged, "EnableClipMask");
}

void UiRenderInterface::RenderToClipMask(Rml::ClipMaskOperation /*operation*/,
                                         Rml::CompiledGeometryHandle /*geometry*/,
                                         Rml::Vector2f /*translation*/) {
    static bool logged = false;
    log_unsupported_once(logged, "RenderToClipMask");
}

Rml::LayerHandle UiRenderInterface::PushLayer() {
    static bool logged = false;
    log_unsupported_once(logged, "PushLayer");
    /* 0 is the base layer, which is the one being rendered to anyway. */
    return Rml::LayerHandle(0);
}

void UiRenderInterface::CompositeLayers(Rml::LayerHandle /*source*/, Rml::LayerHandle /*destination*/,
                                        Rml::BlendMode /*blend_mode*/,
                                        Rml::Span<const Rml::CompiledFilterHandle> /*filters*/) {
    static bool logged = false;
    log_unsupported_once(logged, "CompositeLayers");
}

void UiRenderInterface::PopLayer() {
    static bool logged = false;
    log_unsupported_once(logged, "PopLayer");
}

Rml::TextureHandle UiRenderInterface::SaveLayerAsTexture() {
    static bool logged = false;
    log_unsupported_once(logged, "SaveLayerAsTexture");
    return Rml::TextureHandle(0);
}

Rml::CompiledFilterHandle UiRenderInterface::SaveLayerAsMaskImage() {
    static bool logged = false;
    log_unsupported_once(logged, "SaveLayerAsMaskImage");
    return Rml::CompiledFilterHandle(0);
}

Rml::CompiledFilterHandle UiRenderInterface::CompileFilter(const Rml::String& /*name*/,
                                                           const Rml::Dictionary& /*parameters*/) {
    static bool logged = false;
    log_unsupported_once(logged, "CompileFilter");
    return Rml::CompiledFilterHandle(0);
}

void UiRenderInterface::ReleaseFilter(Rml::CompiledFilterHandle /*filter*/) {
    static bool logged = false;
    log_unsupported_once(logged, "ReleaseFilter");
}

Rml::CompiledShaderHandle UiRenderInterface::CompileShader(const Rml::String& /*name*/,
                                                           const Rml::Dictionary& /*parameters*/) {
    static bool logged = false;
    log_unsupported_once(logged, "CompileShader");
    return Rml::CompiledShaderHandle(0);
}

void UiRenderInterface::RenderShader(Rml::CompiledShaderHandle /*shader*/,
                                     Rml::CompiledGeometryHandle /*geometry*/,
                                     Rml::Vector2f /*translation*/, Rml::TextureHandle /*texture*/) {
    static bool logged = false;
    log_unsupported_once(logged, "RenderShader");
}

void UiRenderInterface::ReleaseShader(Rml::CompiledShaderHandle /*shader*/) {
    static bool logged = false;
    log_unsupported_once(logged, "ReleaseShader");
}

} // namespace rtui

#endif /* ICORECOMP_UI */
