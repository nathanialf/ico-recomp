#version 450

/* gs/overlay.vert: overlay render pass, milestone 4 (see gs_parallel_lib.cpp
 * RtPgs::draw_overlay). Ours (MIT), compiled by tools/gen_overlay_spirv.sh
 * into overlay_shaders.inc; not ROM-derived.
 *
 * mvp is built lib-side per RtPgsOverlayCmd: an orthographic pixel-space
 * projection of the surface, premultiplied with the command's translation
 * and (when RT_PGS_OVERLAY_TRANSFORM is set) its 4x4 transform.
 */

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color; /* R8G8B8A8_UNORM vertex attribute */

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
} pc;

void main() {
    v_uv = in_uv;
    v_color = in_color;
    gl_Position = pc.mvp * vec4(in_pos, 0.0, 1.0);
}
