/* ui/ps2_icon_render.h: renders a parsed PS2 memory-card icon (ps2_icon.h)
 * the way the browser draws it: a lit, perspective front view of one shape
 * over the icon.sys background gradient.
 *
 * Runtime-internal, NOT part of the ABI contract.
 */
#ifndef ICORECOMP_UI_PS2_ICON_RENDER_H
#define ICORECOMP_UI_PS2_ICON_RENDER_H

#include "ps2_icon.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/* width * height * 4 bytes, row-major from the top, R first, straight
 * (non-premultiplied) alpha. Always fully opaque: the background fills
 * every pixel the model does not cover. */
struct RtPs2IconImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;

    bool valid() const { return width > 0 && height > 0 && rgba.size() == size_t(width) * height * 4u; }
};

/* Renders `icon`'s shape `shape` into a size_px by size_px image (the .ico
 * format's own sizes are all square).
 *
 * Camera: a perspective view from the front, vertical field of view 30
 * degrees, on the negative-z side of the model looking toward +z (screen
 * x = +x, screen y = +y; the file's own y axis already points down, so
 * this needs no flip), centred on shape `shape`'s own bounding-box centre.
 * Camera distance is chosen so the larger of the bounding box's x and y
 * half-extents fills 88% of the output's height at the box centre's
 * depth (nearer vertices project a little larger); the exact
 * camera the PS2 BIOS browser uses is not documented anywhere this project
 * has found, so this is a deliberate approximation, not a measurement.
 *
 * Lighting is evaluated per vertex from icon_sys's three lights and ambient
 * colour: rgb * (ambient + sum over the three lights of light_colour *
 * max(0, dot(normalised normal, normalised light_dir))), clamped to
 * [0, 1]. Vertex alpha is treated as PS2 0x80 = 1.0, clamped the same way.
 * On this disc every vertex colour is (0, 0, 0, 127) and every light and
 * the ambient colour are all zero, so the model comes out solid black
 * either way -- the lighting stays live rather than special-cased so a
 * disc whose icon carries real vertex colours or lights renders correctly
 * too.
 *
 * Depth: a per-pixel float z-buffer (nearest wins), Gouraud-shaded colour;
 * perspective-correct interpolation is not worth it at these pixel counts,
 * so the colour and depth are interpolated linearly in screen space. No
 * backface culling and no painter's sort -- the z-buffer makes both
 * unnecessary.
 *
 * Background: a bilinear blend of icon_sys's four background corner
 * colours (file order), alpha 255.
 *
 * Supersampled and box-filtered down, so the silhouette's edge is soft
 * rather than jagged: 4x up to 64 px, 2x up to 256 px, 1:1 above that. The
 * tiers exist so the sizes an icon is actually asked for all get the same
 * treatment; the fragment buffer is what caps the factor, not the size.
 *
 * Never fatal: false with a reason in `err` (may be null) on a bad shape
 * index, a size outside 1..4096, a degenerate bounding box, or an
 * allocation failure caught here. A camera that does not clear the front of
 * the model's bounding box is not an error, but it does make the near-plane
 * guard move vertices, so it logs a line under "ui" saying the render is
 * not the shape the file describes.
 */
bool rt_ps2_icon_render(const RtPs2Icon& icon, const RtPs2IconSys& icon_sys, uint32_t size_px,
                        uint32_t shape, RtPs2IconImage& out, char* err, size_t err_len);

#endif /* ICORECOMP_UI_PS2_ICON_RENDER_H */
