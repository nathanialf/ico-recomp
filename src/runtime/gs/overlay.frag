#version 450

/* gs/overlay.frag: paired with overlay.vert, see its header comment. */

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

void main() {
    /* Untextured draws bind a 1x1 white image (draw_overlay's fallback), so
     * this one shader covers both textured and solid-color commands. */
    out_color = texture(u_tex, v_uv) * v_color;
}
