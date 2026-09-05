/* gs/render/shaders/overlay.frag: the one graphics pipeline's fragment stage.
 *
 * Ours (MIT). Texture slot 0 and the linear/clamp sampler, or the vertex
 * colour alone when the command has no texture (texture id 0 in the overlay
 * ABI means solid white).
 *
 * Whether the colours are premultiplied is a blend state decision, made when
 * the pipeline is built (GraphicsPipelineDesc::premultiplied), not something
 * this shader corrects. The UI hands over premultiplied vertex colours and
 * premultiplied texture bytes; see RT_PGS_OVERLAY_PREMULTIPLIED.
 */
#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 2) uniform texture2D g_textures[8];
layout(set = 0, binding = 3) uniform sampler g_samplers[4];

layout(push_constant) uniform Push {
    mat4 transform;
    vec2 surface_size;
    vec2 translate;
    uint use_transform;
    uint use_texture;
} pc;

void main() {
    vec4 c = v_color;
    if (pc.use_texture != 0u) {
        /* Sampler 1 is linear/clamp in the RHI's immutable set. */
        c *= texture(sampler2D(g_textures[0], g_samplers[1]), v_uv);
    }
    o_color = c;
}
