/* ui/ps2_icon.h: the PS2 memory-card icon (.ico) format, as read out of
 * DFDATAS/DATA.DF's boy_blk.ico entry on the retail ICO disc, and the
 * icon.sys descriptor that names it.
 *
 * .ico layout (measured against the retail disc's boy_blk.ico, one raw
 * 95624-byte entry of DATA.DF's outer table):
 *
 *   0x00  u32   magic, 0x00010000
 *   0x04  u32   shapes (nbsec) : 8 in this file, one position set a
 *               morph-target frame
 *   0x08  u32   attrib : texture type; 3 = untextured (this file), 6/7 =
 *               raw 128x128 RGB555, 0xE/0xF = RLE. This reader never reads
 *               a texture: the render it feeds (ps2_icon_render.h) uses
 *               positions, normals and vertex colours only. The four
 *               textured values are accepted and logged, not decoded; any
 *               other value fails the parse.
 *   0x0C  f32   1.0 (unused by this reader)
 *   0x10  u32   vertex_count (nbvtx) : 1191 here; a triangle list, so a
 *               multiple of 3
 *   0x14  per-vertex data, 8*shapes+16 bytes a vertex (80 here):
 *           for each shape:  s16 x, y, z; u16 unused  (fixed point / 4096)
 *           then:            s16 nx, ny, nz; u16 unused
 *           then:            s16 u, v
 *           then:            u8 r, g, b, a  (PS2 scale, 0x80 = 1.0; every
 *                             vertex in boy_blk.ico is (0, 0, 0, 127))
 *   ...   animation header, right after the vertex table:
 *           u32 idtag = 1; u32 framelength; f32 speed; u32 playoffset;
 *           u32 nbframes
 *         then animation frame data and, for a textured attrib, the
 *         texture. Neither is used or decoded. Where the frame data ends
 *         and a texture would begin has not been measured, so nothing here
 *         claims to know it: the parse stops at the animation header and
 *         validates only the bytes it actually reads.
 *
 * Runtime-internal, NOT part of the ABI contract.
 */
#ifndef ICORECOMP_UI_PS2_ICON_H
#define ICORECOMP_UI_PS2_ICON_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct RtPs2Icon {
    uint32_t shapes = 0;
    uint32_t attrib = 0;
    uint32_t vertex_count = 0;

    /* Shape-major: pos[(shape * vertex_count + vertex) * 3 + {0=x,1=y,2=z}],
     * raw fixed-point units (divide by 4096.0f for world units). */
    std::vector<int16_t> pos;
    /* One normal a vertex, shared across every shape:
     * normal[vertex * 3 + {0,1,2}], raw units (direction only; this reader
     * normalises it, so the fixed-point scale does not matter). */
    std::vector<int16_t> normal;
    /* One texcoord a vertex: uv[vertex * 2 + {0,1}]. Unused for attrib 3. */
    std::vector<int16_t> uv;
    /* One RGBA colour a vertex, PS2 scale (0x80 = 1.0):
     * rgba[vertex * 4 + {0,1,2,3}]. */
    std::vector<uint8_t> rgba;

    uint32_t anim_id = 0;          /* always 1 when the file parses */
    uint32_t anim_framelength = 0;
    float anim_speed = 0.0f;
    uint32_t anim_playoffset = 0;
    uint32_t anim_nbframes = 0;
};

/* Parses `data` (exactly one DATA.DF entry's bytes, no container). Loud
 * validation throughout: a magic mismatch, an out-of-range shape or vertex
 * count, a vertex count that is not a multiple of 3, a truncated vertex
 * table, a truncated or wrongly tagged animation header and an
 * unrecognised texture type all fail with a reason in `err` (may be null)
 * rather than reading past what the file holds. */
bool rt_ps2_icon_parse(const uint8_t* data, size_t len, RtPs2Icon& out, char* err, size_t err_len);

/* icon.sys: the memory-card browser's per-save descriptor, 964 bytes,
 * measured layout:
 *   0x00  "PS2D"
 *   0x0C  u32   transparency
 *   0x10  4 corners x {u32 r, g, b, a} : background gradient, file order
 *   0x50  3 x {f32 x, y, z, w} : light directions
 *   0x80  3 x {f32 r, g, b, a} : light colours
 *   0xB0  {f32 r, g, b, a}     : ambient colour
 *   0x104 char[64]             : view icon file name, NUL-terminated
 *   0x144 char[64]             : copy icon file name
 *   0x184 char[64]             : delete icon file name
 * (0xC0 carries a Shift-JIS title this reader does not need and does not
 * decode.) */
struct RtPs2IconSys {
    uint32_t transparency = 0;
    uint32_t bg_corner[4][4] = {}; /* [corner][r,g,b,a], file order */
    float light_dir[3][4] = {};
    float light_colour[3][4] = {};
    float ambient[4] = {};

    /* "" when the field held nothing before its terminator. */
    std::string view_icon;
    std::string copy_icon;
    std::string delete_icon;
};

bool rt_ps2_icon_sys_parse(const uint8_t* data, size_t len, RtPs2IconSys& out, char* err, size_t err_len);

#endif /* ICORECOMP_UI_PS2_ICON_H */
