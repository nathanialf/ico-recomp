/* gs/render/shaders/overlay.vert: the one graphics pipeline's vertex stage.
 *
 * Ours (MIT). Draws the UI's overlay geometry (RtPgsOverlayFrame in
 * gs/gs_parallel_api.h) and anything else that wants textured triangles in
 * surface pixel coordinates.
 *
 * Vertex layout is the RHI's fixed one (rhi.h): float2 position in surface
 * pixels with the origin at the top left, float2 texture coordinate, one
 * R8G8B8A8_UNORM colour.
 *
 * The transform is applied in pixel space and the result is divided into
 * clip space with its own w kept, so a perspective transform from the UI
 * behaves the way the layout intended rather than being flattened.
 */
#version 450

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

layout(push_constant) uniform Push {
    mat4 transform;      /* column-major, column vectors, as the UI sends it */
    vec2 surface_size;   /* the size the geometry was laid out for */
    vec2 translate;
    uint use_transform;
    uint use_texture;
} pc;

void main() {
    vec4 p = vec4(a_position + pc.translate, 0.0, 1.0);
    if (pc.use_transform != 0u) p = pc.transform * p;
    /* Pixels to clip space. The y axis points down in the source geometry and
     * down in Vulkan's framebuffer, so neither is flipped. */
    gl_Position = vec4(2.0 * p.x / pc.surface_size.x - p.w,
                       2.0 * p.y / pc.surface_size.y - p.w,
                       0.0, p.w);
    v_uv = a_uv;
    v_color = a_color;
}
