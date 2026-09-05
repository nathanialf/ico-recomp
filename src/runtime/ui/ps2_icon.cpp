/* ui/ps2_icon.cpp: see ps2_icon.h. */
#include "ps2_icon.h"

#include "../runtime.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

void set_err(char* err, size_t err_len, const char* fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t rd_u16(const uint8_t* p) {
    return uint16_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8));
}

int16_t rd_s16(const uint8_t* p) {
    return int16_t(rd_u16(p));
}

float rd_f32(const uint8_t* p) {
    const uint32_t bits = rd_u32(p);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

constexpr uint32_t kIconMagic = 0x00010000u;
constexpr uint32_t kMinShapes = 1, kMaxShapes = 32;
constexpr uint32_t kMinVerts = 3, kMaxVerts = 65535;
constexpr uint32_t kAnimIdTag = 1;

constexpr uint32_t kTexNone = 3;
constexpr uint32_t kTexRaw6 = 6, kTexRaw7 = 7;
constexpr uint32_t kTexRle0xE = 0xE, kTexRle0xF = 0xF;

constexpr size_t kIconSysBytes = 964;

} // namespace

bool rt_ps2_icon_parse(const uint8_t* d, size_t len, RtPs2Icon& out, char* err, size_t err_len) {
    out = RtPs2Icon();
    if (len < 20) {
        set_err(err, err_len, "ps2 icon is %zu bytes, shorter than its 20-byte header", len);
        return false;
    }
    const uint32_t magic = rd_u32(d);
    if (magic != kIconMagic) {
        set_err(err, err_len, "ps2 icon magic is 0x%08x, expected 0x%08x", magic, kIconMagic);
        return false;
    }
    const uint32_t shapes = rd_u32(d + 4);
    const uint32_t attrib = rd_u32(d + 8);
    const uint32_t verts = rd_u32(d + 16);
    if (shapes < kMinShapes || shapes > kMaxShapes) {
        set_err(err, err_len, "ps2 icon declares %u shapes", shapes);
        return false;
    }
    if (verts < kMinVerts || verts > kMaxVerts || verts % 3 != 0) {
        set_err(err, err_len, "ps2 icon declares %u vertices, not a positive multiple of 3 under %u",
            verts, kMaxVerts);
        return false;
    }

    const size_t vertex_stride = size_t(8) * shapes + 16;
    const size_t vertex_bytes = size_t(verts) * vertex_stride;
    if (20 + vertex_bytes > len) {
        set_err(err, err_len, "ps2 icon's %u vertices at %u shapes need %zu bytes, file is %zu", verts,
            shapes, 20 + vertex_bytes, len);
        return false;
    }

    const size_t anim_off = 20 + vertex_bytes;
    if (anim_off + 20 > len) {
        set_err(err, err_len, "ps2 icon's animation header at %zu runs past its %zu bytes", anim_off,
            len);
        return false;
    }
    const uint32_t anim_id = rd_u32(d + anim_off);
    if (anim_id != kAnimIdTag) {
        set_err(err, err_len, "ps2 icon's animation id tag at %zu is %u, expected %u", anim_off,
            anim_id, kAnimIdTag);
        return false;
    }
    const uint32_t framelength = rd_u32(d + anim_off + 4);
    const float speed = rd_f32(d + anim_off + 8);
    const uint32_t playoffset = rd_u32(d + anim_off + 12);
    const uint32_t nbframes = rd_u32(d + anim_off + 16);

    /* The recognised texture types, checked so an unrecognised attrib is a
     * loud failure rather than a silently different file. Nothing beyond
     * the animation header is read: the renderer needs positions, normals
     * and vertex colours only, so a texture, wherever it sits, is never
     * touched.
     *
     * There is deliberately no bound on where a texture would start. The
     * animation frame data between the header and any texture has not been
     * measured on this project (this disc's view icon is attrib 3, and no
     * textured .ico has been read), so any offset computed from the header
     * would be a guess, and a guessed bound can only reject files this
     * reader would otherwise render correctly. */
    switch (attrib) {
    case kTexNone:
    case kTexRaw6:
    case kTexRaw7:
    case kTexRle0xE:
    case kTexRle0xF:
        break;
    default:
        set_err(err, err_len, "ps2 icon has an unrecognised texture type (attrib) %u", attrib);
        return false;
    }

    out.shapes = shapes;
    out.attrib = attrib;
    out.vertex_count = verts;
    out.pos.resize(size_t(shapes) * verts * 3);
    out.normal.resize(size_t(verts) * 3);
    out.uv.resize(size_t(verts) * 2);
    out.rgba.resize(size_t(verts) * 4);

    for (uint32_t v = 0; v < verts; ++v) {
        const uint8_t* vp = d + 20 + size_t(v) * vertex_stride;
        for (uint32_t s = 0; s < shapes; ++s) {
            const uint8_t* sp = vp + size_t(s) * 8;
            const size_t base = (size_t(s) * verts + v) * 3;
            out.pos[base + 0] = rd_s16(sp + 0);
            out.pos[base + 1] = rd_s16(sp + 2);
            out.pos[base + 2] = rd_s16(sp + 4);
        }
        const uint8_t* np = vp + size_t(shapes) * 8;
        out.normal[v * 3 + 0] = rd_s16(np + 0);
        out.normal[v * 3 + 1] = rd_s16(np + 2);
        out.normal[v * 3 + 2] = rd_s16(np + 4);
        const uint8_t* tp = np + 8;
        out.uv[v * 2 + 0] = rd_s16(tp + 0);
        out.uv[v * 2 + 1] = rd_s16(tp + 2);
        const uint8_t* cp = tp + 4;
        out.rgba[v * 4 + 0] = cp[0];
        out.rgba[v * 4 + 1] = cp[1];
        out.rgba[v * 4 + 2] = cp[2];
        out.rgba[v * 4 + 3] = cp[3];
    }
    out.anim_id = anim_id;
    out.anim_framelength = framelength;
    out.anim_speed = speed;
    out.anim_playoffset = playoffset;
    out.anim_nbframes = nbframes;

    rt_log_info("ui", "ps2 icon: %zu bytes, %u shapes, attrib %u (%s), %u vertices, anim framelength %u"
                " speed %g playoffset %u nbframes %u",
        len, shapes, attrib, attrib == kTexNone ? "untextured" : "textured, texture not read", verts,
        framelength, double(speed), playoffset, nbframes);
    return true;
}

bool rt_ps2_icon_sys_parse(const uint8_t* d, size_t len, RtPs2IconSys& out, char* err, size_t err_len) {
    out = RtPs2IconSys();
    if (len < kIconSysBytes) {
        set_err(err, err_len, "icon.sys is %zu bytes, need at least %zu", len, kIconSysBytes);
        return false;
    }
    if (std::memcmp(d, "PS2D", 4) != 0) {
        set_err(err, err_len, "icon.sys does not start with 'PS2D'");
        return false;
    }

    out.transparency = rd_u32(d + 0x0C);
    for (int c = 0; c < 4; ++c) {
        for (int k = 0; k < 4; ++k) out.bg_corner[c][k] = rd_u32(d + 0x10 + c * 16 + k * 4);
    }
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 4; ++k) out.light_dir[i][k] = rd_f32(d + 0x50 + i * 16 + k * 4);
    }
    for (int i = 0; i < 3; ++i) {
        for (int k = 0; k < 4; ++k) out.light_colour[i][k] = rd_f32(d + 0x80 + i * 16 + k * 4);
    }
    for (int k = 0; k < 4; ++k) out.ambient[k] = rd_f32(d + 0xB0 + k * 4);

    auto read_name = [&](size_t off) {
        size_t n = 0;
        while (off + n < len && n < 64 && d[off + n] != 0) ++n;
        return std::string(reinterpret_cast<const char*>(d + off), n);
    };
    out.view_icon = read_name(0x104);
    out.copy_icon = read_name(0x144);
    out.delete_icon = read_name(0x184);

    rt_log_info("ui", "icon.sys: transparency %u, corner 0 (%u,%u,%u,%u), view icon '%s', copy icon '%s',"
                " delete icon '%s'",
        out.transparency, out.bg_corner[0][0], out.bg_corner[0][1], out.bg_corner[0][2],
        out.bg_corner[0][3], out.view_icon.c_str(), out.copy_icon.c_str(), out.delete_icon.c_str());
    return true;
}
