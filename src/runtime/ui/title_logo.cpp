/* ui/title_logo.cpp: build the launcher's title image from the user's disc.
 *
 * The image is the game's own title wordmark: the three letter meshes drawn
 * as flat white polygons, laid out at the positions the title animation puts
 * them at. Every number this build uses is read from the user's disc at run
 * time and none of them is written down here; the log lines this file prints
 * carry what a run actually read, so a run can be checked against the disc it
 * was given.
 *
 * The glow sprites the title screen draws behind the letters are deliberately
 * not used. They were, up to geometry version 2; on a real window the halo
 * read as misaligned and it did not earn its place in a launcher panel, so
 * the wordmark is the letters alone.
 *
 * Where the data is
 * -----------------
 * DFDATAS/DATA.DF is a two-level container. The outer level is a plain table
 * (u32 entry count, then 40-byte entries: 32-byte name, u32 byte offset
 * within DATA.DF, u32 size). Each inner .DF entry is a raw DEFLATE stream
 * (no zlib or gzip wrapper) whose inflated image is a second table: a 32-byte
 * header starting with a u32 entry count, then 548-byte entries (512-byte
 * name, u32 index at 512, u32 data offset at 516, misc, u32 size of the NEXT
 * entry at 544), with each file's bytes at its entry's offset and sized by
 * the following entry's offset. The format description comes from the decomp
 * repo's tools/parse_data_df.py, which derived it from the EE loader (DfOpen,
 * the TOC parser at 0x1321c8, the lookup at 0x132388).
 *
 * Four files are read out of STGLOG.DF, all under object/sdf/st26a: the three
 * letter meshes model/I.p2o, model/C.p2o and model/O.p2o, and the title
 * animation anim/title_start.bga. Every match is on the exact stored string.
 * The animation is the expensive one: it sits near the end of the archive, so
 * reaching it is what sets the inflate limit.
 *
 * PS2O meshes (.p2o)
 * ------------------
 *   0x00  'PS2O'
 *   0x04  u32 = filesize - 16
 *   0x08  u32 object count
 *   0x20  body: attribute arrays, material blocks, name strings, prim lists
 *   ...   one 0x100-byte OBJH record per object, at the end of the file
 * OBJH holds ten 16-byte slots at +0x10, each {u32 offset, u32 count}: slot 0
 * positions (float4), 1 normals, 2 UVs (float4), 3 colours (16 bytes each), 4
 * material, 5 texture name, 7 a u32 pointing at the primitive block. The
 * primitive block is a list of triangle strips, each a u16 vertex count
 * followed by 14 bytes of 0xFF and then that many 16-byte records {u32 flag,
 * u16 position index, u16 normal index, u16 uv index, u16 colour index, u32
 * zero}. A count of zero ends the list. A 4x4 float matrix sits at OBJH+0xB0
 * and is the identity in all six of these files.
 *
 * Placement, and why there is no scale factor
 * ------------------------------------------
 * anim/title_start.bga carries a node table whose first six records are the
 * six objects above: the three letters I, C and O, then the three glow quads
 * I_f, C_f and O_f. A record is 0xEC bytes; the first name field is at 0xD0,
 * the translation is the float3 at name - 0xA8 and the scale the float3 at
 * name - 0x90. Only the first three records are read, and only their
 * translation and scale; a record whose name is not the expected letter, or
 * whose numbers are outside a plausible range, fails the build rather than
 * being guessed around.
 *
 * The scale the file carries is used as it stands, with no fitted constant
 * anywhere. That was settled while the glow sprites were still being drawn:
 * each _f object is a unit quad spanning [-1, 1] in x and y carrying its
 * letter's glow, and the glow lands exactly on its letter only at the scale
 * the file gives. A search of the decomp for a scale applied to these objects
 * found none: the BGA node-to-matrix routine is asm only
 * (_RotTransCurrentMatrixYXZ, asm/nonmatchings/src/BgAnimation/), it
 * multiplies in no constant, and the one candidate float in the binary is a
 * camera film aperture (D_00631430, used once in bga_setCounter to turn a
 * focal length into a field of view).
 *
 * The y translation is the same for all three and z is unused in an
 * orthographic front view, so both drop out and only x matters.
 *
 * Geometry, then raster
 * ---------------------
 * The placed meshes are reduced to a flat triangle list in a box that is
 * exactly 1.0 tall and `aspect` wide, with a border of 4 percent of the
 * letters' own width already included on all four sides. The span, the box
 * and the aspect are whatever the disc's own meshes and placement give; the
 * log line at the end of extract() names them for the run. That triangle list
 * is what the cache holds; it has no pixel size in it at all, which is the
 * point.
 *
 * The raster then fills that list at whatever pixel size the caller asks for,
 * under one uniform scale, centred. The caller is the launcher, and it asks
 * for exactly the box the overlay will draw across, so there is one texel a
 * pixel and no resampling on the way to the screen.
 *
 * There is no antialiasing anywhere, deliberately. The GS draws these polygons
 * with it off, and up to geometry version 3 this file both supersampled the
 * coverage and then let the overlay minify the result, so the wordmark came
 * out soft where the title screen is hard. The fill rule is now the hardware's:
 * a pixel belongs to the triangle containing its centre, ties on an edge going
 * to whichever triangle has that edge as a top or left one. Coverage is
 * therefore binary and premultiplied alpha is just the colour or nothing. The
 * vertex colour is applied the way the GS does, Cv = Ct * Cf / 128 with
 * saturation, against the white texel these meshes name; all three files carry
 * 0xFFFFFFFF, so the letters come out white.
 */
#include "title_logo.h"

#include "../host/inflate.h"
#include "../host/portable.h"
#include "../iso/iso9660.h"
#include "../runtime.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

/* Bumped whenever the geometry's contents, layout or cache encoding change, so
 * an old cache is rejected instead of drawn. Part of the cache key.
 *   1: the three glow sprites side by side, as an image
 *   2: the letter meshes over the glows, as an image
 *   3: the letter meshes alone, as an image
 *   4: the letter meshes as geometry, rasterised at the on-screen size
 *   5: the same, with a checksum over the payload in the cache header */
constexpr uint32_t kGeometryVersion = 5;

constexpr char kMagic[8] = {'I', 'C', 'O', 'L', 'O', 'G', 'O', '\0'};

constexpr size_t kOuterEntrySize = 40;
constexpr size_t kOuterNameBytes = 32;
constexpr size_t kInnerHeaderBytes = 32;
constexpr size_t kInnerEntrySize = 548;
constexpr size_t kInnerNameBytes = 512;

constexpr const char* kArchiveName = "STGLOG.DF";

/* The three letters, left to right, so the composite reads "ICO". Each has a
 * mesh and a node in the title animation whose name matches it. */
constexpr int kLetterCount = 3;
const char* const kMeshNames[kLetterCount] = {
    "object/sdf/st26a/model/I.p2o",
    "object/sdf/st26a/model/C.p2o",
    "object/sdf/st26a/model/O.p2o",
};
constexpr const char* kAnimName = "object/sdf/st26a/anim/title_start.bga";

/* BGA node table: the three records this file wants are the first three, at a
 * 0xEC stride, with the name field at 0xD0 in the first one. Translation and
 * scale are the float3s at name - 0xA8 and name - 0x90. See the file comment
 * for the measured values. */
constexpr size_t kBgaFirstName = 0xD0;
constexpr size_t kBgaNodeStride = 0xEC;
constexpr size_t kBgaTranslateBack = 0xA8;
constexpr size_t kBgaScaleBack = 0x90;
const char* const kNodeNames[kLetterCount] = {"I", "C", "O"};

/* The border kept around the letters, as a fraction of their own width, on all
 * four sides. It is part of the geometry rather than of the raster, so the
 * stylesheet's dp box and the pixel box always agree about where the letters
 * sit inside it. */
constexpr float kMarginFraction = 0.04f;

/* A malformed table, or a caller with a silly window, must not be able to ask
 * for an unbounded raster. The real one is under a thousand pixels wide. */
constexpr uint32_t kMaxRasterDim = 8192;

/* Bound on the geometry the cache will accept, so a corrupt file cannot ask
 * for an unbounded allocation. The three letters come to 323 triangles. */
constexpr uint32_t kMaxTriangles = 65536;

/* A whole archive has to fit in memory twice over (compressed and inflated),
 * and a malformed table must not be able to ask for an unbounded allocation.
 * STGLOG.DF is 3.6 MB compressed and 8.5 MB inflated on the retail disc. */
constexpr size_t kMaxArchiveBytes = 64u * 1024 * 1024;

std::string g_cache_path;

void set_err(char* err, size_t err_len, const char* fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t rd_u16(const uint8_t* p) {
    return uint16_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8));
}

/* ---- disc reads ---------------------------------------------------------- */

/* Reads [offset, offset + length) of DATA.DF, which the ISO reader only
 * offers in whole 2048-byte sectors. */
bool read_data_df(const RtIsoFile& file, uint64_t offset, size_t length, std::vector<uint8_t>& out,
                  char* err, size_t err_len) {
    if (offset > file.size || length > file.size - offset) {
        set_err(err, err_len, "DATA.DF range [%llu, +%zu) is outside the file's %u bytes",
            (unsigned long long)offset, length, file.size);
        return false;
    }
    const uint32_t first = uint32_t(offset / 2048);
    const uint32_t skip = uint32_t(offset % 2048);
    const uint32_t sectors = uint32_t((skip + length + 2047) / 2048);

    std::vector<uint8_t> raw(size_t(sectors) * 2048);
    const uint32_t got = rt_iso_read_sectors(file.lsn + first, sectors, raw.data());
    if (got != sectors) {
        set_err(err, err_len, "read %u of %u sectors at LSN %u", got, sectors, file.lsn + first);
        return false;
    }
    out.assign(raw.begin() + skip, raw.begin() + skip + length);
    return true;
}

/* ---- container tables ---------------------------------------------------- */

struct InnerEntry {
    std::string name;
    uint32_t offset = 0;
    uint32_t next_size = 0;
};

/* Finds one outer entry by name (exact, the stored name is uppercase ASCII).
 * The outer table is the first bytes of DATA.DF. */
bool find_outer_entry(const RtIsoFile& file, const char* name, uint32_t* offset, uint32_t* size,
                      char* err, size_t err_len) {
    std::vector<uint8_t> head;
    if (!read_data_df(file, 0, 2048, head, err, err_len)) return false;
    const uint32_t count = rd_u32(head.data());
    if (count == 0 || count > 4096) {
        set_err(err, err_len, "DATA.DF outer table declares %u entries", count);
        return false;
    }
    const size_t table_bytes = 4 + size_t(count) * kOuterEntrySize;
    if (!read_data_df(file, 0, table_bytes, head, err, err_len)) return false;

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = head.data() + 4 + size_t(i) * kOuterEntrySize;
        char stored[kOuterNameBytes + 1];
        std::memcpy(stored, e, kOuterNameBytes);
        stored[kOuterNameBytes] = 0;
        if (std::strcmp(stored, name) != 0) continue;
        *offset = rd_u32(e + kOuterNameBytes);
        *size = rd_u32(e + kOuterNameBytes + 4);
        rt_log("ui", "title logo: DATA.DF outer table has %u entries; %s at offset %u, %u bytes",
            count, name, *offset, *size);
        return true;
    }
    set_err(err, err_len, "DATA.DF has no outer entry named %s (%u entries scanned)", name, count);
    return false;
}

/* Parses the inflated archive's own table. `image` may be a prefix of the
 * archive as long as it covers the table. */
bool parse_inner_table(const std::vector<uint8_t>& image, std::vector<InnerEntry>& out, char* err,
                       size_t err_len) {
    if (image.size() < kInnerHeaderBytes) {
        set_err(err, err_len, "inflated archive is %zu bytes, shorter than its 32-byte header",
            image.size());
        return false;
    }
    const uint32_t count = rd_u32(image.data());
    if (count == 0 || count > 65536) {
        set_err(err, err_len, "inflated archive declares %u entries", count);
        return false;
    }
    const size_t table_bytes = kInnerHeaderBytes + size_t(count) * kInnerEntrySize;
    if (image.size() < table_bytes) {
        set_err(err, err_len, "inflated %zu bytes, need %zu for the %u-entry table", image.size(),
            table_bytes, count);
        return false;
    }

    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = image.data() + kInnerHeaderBytes + size_t(i) * kInnerEntrySize;
        /* The name is 0xFF-padded after its terminator, so stop at the first
         * NUL rather than trusting the whole field. */
        size_t n = 0;
        while (n < kInnerNameBytes && e[n] != 0) ++n;
        InnerEntry ent;
        ent.name.assign(reinterpret_cast<const char*>(e), n);
        ent.offset = rd_u32(e + kInnerNameBytes + 4);
        ent.next_size = rd_u32(e + 544);
        out.push_back(std::move(ent));
    }
    return true;
}

/* ---- PS2O meshes --------------------------------------------------------- */

struct P2oMesh {
    /* Positions, x and y only. The wordmark is a front view and the letters
     * are flat, so z is dropped rather than projected. */
    std::vector<float> x, y;
    /* Three position indices a triangle, already unpacked from the strips. */
    std::vector<uint32_t> tri;
    /* Per-object vertex colour, PS2 scale (0x80 is 1.0). */
    uint8_t colour[4] = {0x80, 0x80, 0x80, 0x80};
    std::string texture;
};

/* Guards against a malformed table asking for an unbounded walk. The largest
 * of these six files has 175 positions in 8 strips. */
constexpr uint32_t kMaxMeshVerts = 65536;
constexpr uint32_t kMaxMeshStrips = 16384;

bool parse_p2o(const uint8_t* d, size_t len, const char* label, P2oMesh& out, char* err,
               size_t err_len) {
    if (len < 0x120 || std::memcmp(d, "PS2O", 4) != 0) {
        set_err(err, err_len, "%s is not a PS2O file (%zu bytes)", label, len);
        return false;
    }
    const uint32_t declared = rd_u32(d + 4);
    if (declared != len - 16) {
        set_err(err, err_len, "%s declares a size field of %u, expected %zu", label, declared,
            len - 16);
        return false;
    }
    const uint32_t objects = rd_u32(d + 8);
    if (objects < 1) {
        set_err(err, err_len, "%s holds no objects", label);
        return false;
    }

    /* The OBJH records sit at the end of the file, 16-byte aligned. Only the
     * first object is used: each of these six files has exactly one. */
    size_t hdr = 0;
    bool found = false;
    for (size_t p = 0x20; p + 0x100 <= len; p += 16) {
        if (std::memcmp(d + p, "OBJH", 4) == 0) {
            hdr = p;
            found = true;
            break;
        }
    }
    if (!found) {
        set_err(err, err_len, "%s has no OBJH record", label);
        return false;
    }

    /* Every slot offset is widened here and stays widened. A u32 read out of
     * the file plus a small constant wraps in u32 arithmetic, and a wrapped
     * sum compares as a tiny offset against the file's length, so a slot
     * offset near 2^32 would pass a bounds test and then be read far outside
     * the file. size_t is wide enough that none of the sums below can wrap. */
    size_t slot_off[10];
    uint32_t slot_count[10];
    for (int i = 0; i < 10; ++i) {
        slot_off[i] = size_t(rd_u32(d + hdr + 0x10 + 16 * i));
        slot_count[i] = rd_u32(d + hdr + 0x10 + 16 * i + 4);
    }

    /* Positions: slot 0, one float4 each. */
    const uint32_t verts = slot_count[0];
    if (verts == 0 || verts > kMaxMeshVerts) {
        set_err(err, err_len, "%s declares %u positions", label, verts);
        return false;
    }
    if (slot_off[0] > len || size_t(verts) * 16 > len - slot_off[0]) {
        set_err(err, err_len, "%s position array [%zu, +%zu) is outside its %zu bytes", label,
            slot_off[0], size_t(verts) * 16, len);
        return false;
    }
    out.x.resize(verts);
    out.y.resize(verts);
    for (uint32_t i = 0; i < verts; ++i) {
        const uint8_t* p = d + slot_off[0] + 16 * i;
        uint32_t bits = rd_u32(p);
        std::memcpy(&out.x[i], &bits, 4);
        bits = rd_u32(p + 4);
        std::memcpy(&out.y[i], &bits, 4);
        /* A NaN or an infinity here would survive every arithmetic step below
         * and reach the raster's float-to-int conversion, which is undefined
         * for a value no int can hold. The file is wrong, so say so. */
        if (!std::isfinite(out.x[i]) || !std::isfinite(out.y[i])) {
            set_err(err, err_len, "%s position %u is (%g, %g), which is not a finite point", label,
                i, double(out.x[i]), double(out.y[i]));
            return false;
        }
    }

    /* Colour: slot 3, RGBA in the first four bytes of the first 16-byte entry. */
    if (slot_count[3] > 0 && slot_off[3] <= len && len - slot_off[3] >= 4) {
        std::memcpy(out.colour, d + slot_off[3], 4);
    }

    /* Texture name: slot 5, a NUL-terminated short name. */
    if (slot_count[5] > 0 && slot_off[5] < len) {
        size_t n = 0;
        while (slot_off[5] + n < len && d[slot_off[5] + n] != 0 && n < 64) ++n;
        out.texture.assign(reinterpret_cast<const char*>(d + slot_off[5]), n);
    }

    /* Primitive block: slot 7's offset holds a u32 pointing at a list of
     * triangle strips. */
    if (slot_count[7] == 0) {
        set_err(err, err_len, "%s has no primitive block", label);
        return false;
    }
    if (slot_off[7] > len || len - slot_off[7] < 4) {
        set_err(err, err_len, "%s primitive slot points past its %zu bytes", label, len);
        return false;
    }
    size_t p = size_t(rd_u32(d + slot_off[7]));
    uint32_t strips = 0;
    out.tri.clear();
    for (;;) {
        if (p > len || len - p < 16) {
            set_err(err, err_len, "%s strip header at %zu runs past its %zu bytes", label, p, len);
            return false;
        }
        const uint16_t n = rd_u16(d + p);
        p += 16;
        if (n == 0) break;
        if (++strips > kMaxMeshStrips) {
            set_err(err, err_len, "%s has more than %u strips", label, kMaxMeshStrips);
            return false;
        }
        if (size_t(n) * 16 > len - p) {
            set_err(err, err_len, "%s strip of %u vertices at %zu runs past its %zu bytes", label, n,
                p, len);
            return false;
        }
        for (uint16_t i = 0; i + 2 < n; ++i) {
            uint32_t idx[3];
            for (int k = 0; k < 3; ++k) {
                idx[k] = rd_u16(d + p + 16 * (i + k) + 4);
                if (idx[k] >= verts) {
                    set_err(err, err_len, "%s strip vertex names position %u of %u", label, idx[k],
                        verts);
                    return false;
                }
            }
            out.tri.push_back(idx[0]);
            out.tri.push_back(idx[1]);
            out.tri.push_back(idx[2]);
        }
        p += size_t(n) * 16;
    }
    if (out.tri.empty()) {
        set_err(err, err_len, "%s has %u strips but no triangles", label, strips);
        return false;
    }
    return true;
}

/* ---- title animation placement ------------------------------------------- */

struct TitleNode {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float sx = 1.0f, sy = 1.0f, sz = 1.0f;
};

float rd_f32(const uint8_t* p) {
    const uint32_t bits = rd_u32(p);
    float v;
    std::memcpy(&v, &bits, 4);
    return v;
}

/* Reads the node records the title wordmark uses. Returns false with a reason
 * rather than guessing: the caller then fails the whole build and the launcher
 * keeps its text title. */
bool parse_bga_nodes(const uint8_t* d, size_t len, TitleNode out[kLetterCount], char* err,
                     size_t err_len) {
    if (len < 8 || std::memcmp(d, "BGA", 4) != 0) {
        set_err(err, err_len, "%s is not a BGA file", kAnimName);
        return false;
    }
    for (int i = 0; i < kLetterCount; ++i) {
        const size_t name = kBgaFirstName + size_t(i) * kBgaNodeStride;
        if (name + 16 > len || name < kBgaTranslateBack) {
            set_err(err, err_len, "%s node %d would be at %zu of %zu bytes", kAnimName, i, name, len);
            return false;
        }
        size_t n = 0;
        while (n < 16 && d[name + n] != 0) ++n;
        if (n == 16 || std::strncmp(reinterpret_cast<const char*>(d + name), kNodeNames[i], 16) != 0) {
            char got[17] = {0};
            std::memcpy(got, d + name, 16);
            for (int k = 0; k < 16; ++k) {
                if (got[k] && (got[k] < 0x20 || got[k] > 0x7E)) got[k] = '?';
            }
            set_err(err, err_len, "%s node %d at %#zx is named '%s', expected '%s'", kAnimName, i,
                name, got, kNodeNames[i]);
            return false;
        }
        const uint8_t* t = d + name - kBgaTranslateBack;
        const uint8_t* s = d + name - kBgaScaleBack;
        out[i].x = rd_f32(t);
        out[i].y = rd_f32(t + 4);
        out[i].z = rd_f32(t + 8);
        out[i].sx = rd_f32(s);
        out[i].sy = rd_f32(s + 4);
        out[i].sz = rd_f32(s + 8);
        /* A node whose numbers are not plausible means the stride guess is
         * wrong for this file, which is worth saying rather than drawing. */
        if (!(out[i].x > -1000.0f && out[i].x < 1000.0f) ||
            !(out[i].y > -1000.0f && out[i].y < 1000.0f) ||
            !(out[i].sx > 0.0f && out[i].sx < 1000.0f) ||
            !(out[i].sy > 0.0f && out[i].sy < 1000.0f)) {
            /* These comparisons are false for a NaN too, which is the point:
             * a non-finite translation or scale reaches the raster's
             * float-to-int conversion otherwise. */
            set_err(err, err_len, "%s node '%s' has translate (%g, %g) and scale (%g, %g)",
                kAnimName, kNodeNames[i], double(out[i].x), double(out[i].y), double(out[i].sx),
                double(out[i].sy));
            return false;
        }
    }
    return true;
}

/* ---- rasteriser ---------------------------------------------------------- */

/* Twice the signed area of the triangle (a, b, c), positive for a clockwise
 * winding in the y-down space the raster works in. Doubles because the inputs
 * are pixel coordinates that can reach a few thousand and the sign has to be
 * exact for the fill rule below. */
double orient2d(double ax, double ay, double bx, double by, double cx, double cy) {
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
}

/* The GS's fill rule: a pixel belongs to the triangle whose interior contains
 * its centre, and a centre exactly on an edge belongs to the triangle for
 * which that edge is a top or a left one. For the positive winding above, an
 * edge (a, b) is top when it is horizontal with b to the right of a, and left
 * when it runs upward (b.y < a.y). The two triangles sharing an edge see it in
 * opposite directions, so exactly one of them claims such a centre: no seam
 * and no double coverage. */
bool top_left(double ax, double ay, double bx, double by) {
    return (ay == by && bx > ax) || (by < ay);
}

/* Fills one triangle into a binary mask at the output resolution. No coverage
 * blending anywhere: the hardware draws these polygons with antialiasing off,
 * so a pixel is either the letter's colour or nothing. */
void fill_triangle(std::vector<uint8_t>& mask, uint32_t mw, uint32_t mh, const double vx[3],
                   const double vy[3]) {
    double ax = vx[0], ay = vy[0], bx = vx[1], by = vy[1], cx = vx[2], cy = vy[2];
    const double area = orient2d(ax, ay, bx, by, cx, cy);
    if (area == 0.0) return; /* degenerate: no pixel centre can be inside it */
    if (area < 0.0) {
        std::swap(bx, cx);
        std::swap(by, cy);
    }

    /* Clamped as doubles and only then converted. A triangle far outside the
     * mask, which a cache or a mesh can ask for with coordinates that are
     * finite but enormous, would otherwise convert a value no int can hold,
     * which has no defined answer. Inside the mask the two orders give the
     * same bounds. */
    const double fx_lo = std::floor(std::min(ax, std::min(bx, cx)) - 0.5);
    const double fx_hi = std::ceil(std::max(ax, std::max(bx, cx)) + 0.5);
    const double fy_lo = std::floor(std::min(ay, std::min(by, cy)) - 0.5);
    const double fy_hi = std::ceil(std::max(ay, std::max(by, cy)) + 0.5);
    const double last_x = double(mw) - 1.0, last_y = double(mh) - 1.0;
    if (fx_hi < 0.0 || fx_lo > last_x || fy_hi < 0.0 || fy_lo > last_y) return;
    const int x_lo = int(std::max(fx_lo, 0.0));
    const int x_hi = int(std::min(fx_hi, last_x));
    const int y_lo = int(std::max(fy_lo, 0.0));
    const int y_hi = int(std::min(fy_hi, last_y));

    const bool tl0 = top_left(ax, ay, bx, by);
    const bool tl1 = top_left(bx, by, cx, cy);
    const bool tl2 = top_left(cx, cy, ax, ay);

    for (int y = y_lo; y <= y_hi; ++y) {
        const double py = double(y) + 0.5;
        uint8_t* row = mask.data() + size_t(y) * mw;
        for (int x = x_lo; x <= x_hi; ++x) {
            const double px = double(x) + 0.5;
            const double w0 = orient2d(ax, ay, bx, by, px, py);
            const double w1 = orient2d(bx, by, cx, cy, px, py);
            const double w2 = orient2d(cx, cy, ax, ay, px, py);
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
            if ((w0 == 0.0 && !tl0) || (w1 == 0.0 && !tl1) || (w2 == 0.0 && !tl2)) continue;
            row[x] = 1;
        }
    }
}

/* ---- geometry ------------------------------------------------------------
 *
 * What the cache holds and what the raster consumes: the letters as a flat
 * triangle list in a box that is `aspect` wide and exactly 1.0 tall, with the
 * border already included. Independent of any pixel size, which is the point:
 * a window-scale change re-rasterises this instead of re-reading the disc.
 */
struct TitleGeometry {
    std::vector<float> xy; /* six floats a triangle: x0 y0 x1 y1 x2 y2 */
    float aspect = 0.0f;

    bool valid() const {
        return aspect > 0.0f && !xy.empty() && xy.size() % 6 == 0;
    }
    size_t triangles() const { return xy.size() / 6; }
};

/* Rasterises `geom` into exactly W by H pixels. One uniform scale, centred, so
 * the letters keep their proportions whatever the box is; a box whose aspect
 * does not match the wordmark's leaves a thin empty margin rather than
 * stretching it. */
bool rasterise(const TitleGeometry& geom, uint32_t W, uint32_t H, const uint8_t colour[4],
               RtTitleLogo& out, char* err, size_t err_len) {
    if (W == 0 || H == 0 || W > kMaxRasterDim || H > kMaxRasterDim) {
        set_err(err, err_len, "a %ux%u raster is outside what this builds", W, H);
        return false;
    }
    const double scale = std::min(double(W) / double(geom.aspect), double(H));
    const double ox = (double(W) - double(geom.aspect) * scale) * 0.5;
    const double oy = (double(H) - scale) * 0.5;

    std::vector<uint8_t> mask(size_t(W) * H, 0);
    for (size_t t = 0; t + 5 < geom.xy.size(); t += 6) {
        double vx[3], vy[3];
        for (int i = 0; i < 3; ++i) {
            vx[i] = ox + double(geom.xy[t + i * 2 + 0]) * scale;
            vy[i] = oy + double(geom.xy[t + i * 2 + 1]) * scale;
        }
        fill_triangle(mask, W, H, vx, vy);
    }

    /* The letters are flat fills of their object's vertex colour, applied the
     * way the GS does it (Cv = Ct * Cf / 128 against the white texel these
     * meshes name, saturating). Coverage is binary, so premultiplied alpha is
     * just the colour or nothing. */
    uint8_t rgb[3];
    for (int c = 0; c < 3; ++c) {
        const float v = 255.0f * float(colour[c]) / 128.0f;
        rgb[c] = uint8_t(v > 255.0f ? 255.0f : v);
    }

    out.width = W;
    out.height = H;
    out.rgba.assign(size_t(W) * H * 4, 0);
    size_t lit = 0;
    for (size_t i = 0; i < size_t(W) * H; ++i) {
        if (!mask[i]) continue;
        ++lit;
        uint8_t* dst = &out.rgba[i * 4];
        dst[0] = rgb[0];
        dst[1] = rgb[1];
        dst[2] = rgb[2];
        dst[3] = 255;
    }
    if (lit == 0) {
        set_err(err, err_len, "the %ux%u raster came out empty", W, H);
        return false;
    }
    return true;
}

/* ---- cache --------------------------------------------------------------- */

/* Identity of the disc the geometry came from. Three facts the ISO layer
 * already knows, so a cache hit costs one mount and no archive read: the
 * image's sector count and DATA.DF's location and size. The geometry version
 * is folded in so a change to the layout above invalidates every cache. The
 * raster size is deliberately not in here: the cache holds geometry, which is
 * size independent, and that is what makes a window-scale change cheap. */
uint64_t disc_key(const RtIsoFile& data_df) {
    const uint64_t fields[4] = {rt_iso_total_sectors(), data_df.lsn, data_df.size, kGeometryVersion};
    uint64_t h = 1469598103934665603ull; /* FNV-1a 64 */
    for (uint64_t f : fields) {
        for (int b = 0; b < 8; ++b) {
            h ^= uint8_t(f >> (b * 8));
            h *= 1099511628211ull;
        }
    }
    return h;
}

/* saves/ next to the executable, which .gitignore and tools/check_no_rom.sh
 * both refuse outright, so game-derived geometry can never reach a commit. The
 * per-user state directory is the fallback for an installation whose own
 * folder is read only. */
const char* cache_path() {
    if (!g_cache_path.empty()) return g_cache_path.c_str();

    std::error_code ec;
    const std::string beside = std::string(rt_base_dir()) + "/saves";
    fs::create_directories(beside, ec);
    if (!ec && fs::is_directory(beside, ec)) {
        g_cache_path = beside + "/title_logo.cache";
        return g_cache_path.c_str();
    }

    const std::string user = rt_user_state_dir();
    if (!user.empty()) {
        std::error_code ec2;
        fs::create_directories(user, ec2);
        if (!ec2 && fs::is_directory(user, ec2)) {
            g_cache_path = user + "/title_logo.cache";
            rt_log("ui", "title logo: '%s' is not writable; caching in '%s'", beside.c_str(),
                g_cache_path.c_str());
            return g_cache_path.c_str();
        }
    }
    rt_log("ui", "title logo: no writable cache location ('%s' and the per-user state directory both"
                 " failed); the geometry is rebuilt every run", beside.c_str());
    return "";
}

/* Header: magic[8], u32 version, u32 triangles, u32 aspect bits, u64 key,
 * u32 payload bytes, u32 payload checksum. Then six little-endian floats a
 * triangle, plus the four colour bytes.
 *
 * The checksum is there because everything before it describes the payload
 * rather than covering it: a file of the right length for the right disc,
 * with a torn or rewritten body, would otherwise be read as geometry. It is
 * not a security measure, and it is not the disc key; it only says that the
 * bytes are the bytes that were written. */
constexpr size_t kCacheHeaderBytes = 8 + 4 + 4 + 4 + 8 + 4 + 4;

/* FNV-1a 32 over the payload. Same construction as disc_key's 64-bit one. */
uint32_t payload_checksum(const uint8_t* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

void wr_u32(uint8_t* p, uint32_t v) {
    for (int b = 0; b < 4; ++b) p[b] = uint8_t(v >> (b * 8));
}

uint32_t f32_bits(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

float f32_from(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

bool cache_load(uint64_t key, TitleGeometry& geom, uint8_t colour[4]) {
    const char* path = cache_path();
    if (!path[0]) return false;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    uint8_t head[kCacheHeaderBytes];
    bool ok = std::fread(head, 1, sizeof(head), f) == sizeof(head);
    if (ok && std::memcmp(head, kMagic, sizeof(kMagic)) != 0) {
        rt_log("ui", "title logo: '%s' is not a logo cache; it will be overwritten", path);
        ok = false;
    }
    uint32_t triangles = 0, bytes = 0, checksum = 0;
    if (ok) {
        const uint32_t version = rd_u32(head + 8);
        triangles = rd_u32(head + 12);
        geom.aspect = f32_from(rd_u32(head + 16));
        uint64_t stored_key = 0;
        for (int b = 0; b < 8; ++b) stored_key |= uint64_t(head[20 + b]) << (b * 8);
        bytes = rd_u32(head + 28);
        checksum = rd_u32(head + 32);
        if (version != kGeometryVersion || stored_key != key) {
            rt_log("ui", "title logo: cache '%s' is for another disc or another build"
                         " (version %u, key %016llx); rebuilding",
                path, version, (unsigned long long)stored_key);
            ok = false;
        } else if (triangles == 0 || triangles > kMaxTriangles ||
                   bytes != triangles * 24u + 4u || !(geom.aspect > 0.0f)) {
            rt_log("ui", "title logo: cache '%s' declares %u triangles in %u bytes at aspect %g;"
                         " rebuilding",
                path, triangles, bytes, double(geom.aspect));
            ok = false;
        }
    }
    if (ok) {
        std::vector<uint8_t> payload(bytes);
        ok = std::fread(payload.data(), 1, bytes, f) == bytes;
        if (!ok) {
            rt_log("ui", "title logo: cache '%s' is truncated; rebuilding", path);
        } else if (payload_checksum(payload.data(), payload.size()) != checksum) {
            rt_log("ui", "title logo: cache '%s' has checksum %08x over its %u payload bytes but"
                         " its header says %08x; rebuilding",
                path, payload_checksum(payload.data(), payload.size()), bytes, checksum);
            ok = false;
        } else {
            geom.xy.resize(size_t(triangles) * 6);
            for (size_t i = 0; i < geom.xy.size(); ++i) {
                geom.xy[i] = f32_from(rd_u32(payload.data() + i * 4));
                /* A checksum-clean file can still hold a non-finite float, if
                 * one was ever written; the raster's float-to-int conversion
                 * has no defined answer for it. */
                if (!std::isfinite(geom.xy[i])) {
                    rt_log("ui", "title logo: cache '%s' holds a non-finite coordinate at %zu;"
                                 " rebuilding",
                        path, i);
                    ok = false;
                    break;
                }
            }
            if (ok) std::memcpy(colour, payload.data() + size_t(triangles) * 24, 4);
        }
    }
    std::fclose(f);
    if (!ok) geom = TitleGeometry();
    return ok;
}

void cache_store(uint64_t key, const TitleGeometry& geom, const uint8_t colour[4]) {
    const char* path = cache_path();
    if (!path[0]) return;
    /* Written to a sibling and renamed, so a run interrupted mid-write leaves
     * either the old cache or none, never a half one. */
    const std::string tmp = std::string(path) + ".new";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        rt_log("ui", "title logo: cannot write the cache '%s': %s", tmp.c_str(), std::strerror(errno));
        return;
    }

    const uint32_t triangles = uint32_t(geom.triangles());
    const uint32_t bytes = triangles * 24u + 4u;
    uint8_t head[kCacheHeaderBytes] = {0};
    std::memcpy(head, kMagic, sizeof(kMagic));
    wr_u32(head + 8, kGeometryVersion);
    wr_u32(head + 12, triangles);
    wr_u32(head + 16, f32_bits(geom.aspect));
    for (int b = 0; b < 8; ++b) head[20 + b] = uint8_t(key >> (b * 8));
    wr_u32(head + 28, bytes);

    std::vector<uint8_t> payload(bytes);
    for (size_t i = 0; i < geom.xy.size(); ++i) wr_u32(payload.data() + i * 4, f32_bits(geom.xy[i]));
    std::memcpy(payload.data() + size_t(triangles) * 24, colour, 4);
    const uint32_t checksum = payload_checksum(payload.data(), payload.size());
    wr_u32(head + 32, checksum);

    bool ok = std::fwrite(head, 1, sizeof(head), f) == sizeof(head);
    ok = ok && std::fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        rt_log("ui", "title logo: writing the cache '%s' failed: %s", tmp.c_str(),
            std::strerror(errno));
        std::remove(tmp.c_str());
        return;
    }

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        rt_log("ui", "title logo: cannot move '%s' into place: %s", tmp.c_str(), ec.message().c_str());
        std::remove(tmp.c_str());
        return;
    }
    rt_log("ui", "title logo: cached %u triangles at aspect %.4f (%zu bytes, payload checksum"
                 " %08x) in '%s'",
        triangles, double(geom.aspect), sizeof(head) + payload.size(), checksum, path);
}

/* ---- extraction ---------------------------------------------------------- */

/* The four files this build pulls out of STGLOG.DF, in one table so the
 * lookup, the bounds checks and the inflate limit are all driven from it. */
constexpr int kWantCount = kLetterCount + 1;

struct WantedFile {
    const char* name;
    size_t offset = 0; /* into the inflated archive */
    size_t size = 0;
};

/* Locates one entry, works out its size from the following entry's offset, and
 * cross-checks that against the one-step lookahead the previous entry carries.
 * A disagreement means the table is not what this reader thinks it is, so it is
 * reported rather than worked around. */
bool locate(const std::vector<InnerEntry>& entries, size_t total, WantedFile& w, char* err,
            size_t err_len) {
    size_t at = entries.size();
    for (size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name == w.name) {
            at = i;
            break;
        }
    }
    if (at == entries.size()) {
        set_err(err, err_len, "%s has no entry named '%s' (%zu entries)", kArchiveName, w.name,
            entries.size());
        return false;
    }
    const uint64_t end = at + 1 < entries.size() ? entries[at + 1].offset : total;
    if (end <= entries[at].offset || end > total) {
        set_err(err, err_len, "'%s' spans [%u, %llu) of a %zu-byte archive", w.name,
            entries[at].offset, (unsigned long long)end, total);
        return false;
    }
    w.offset = entries[at].offset;
    w.size = size_t(end - w.offset);
    if (at > 0 && entries[at - 1].next_size != w.size) {
        set_err(err, err_len, "'%s' is %zu bytes by offset but %u by the previous entry's lookahead",
            w.name, w.size, entries[at - 1].next_size);
        return false;
    }
    return true;
}

bool extract(const RtIsoFile& data_df, TitleGeometry& geom, uint8_t colour[4], char* err,
             size_t err_len) {
    uint32_t archive_offset = 0, archive_size = 0;
    if (!find_outer_entry(data_df, kArchiveName, &archive_offset, &archive_size, err, err_len)) {
        return false;
    }
    if (archive_size == 0 || archive_size > kMaxArchiveBytes) {
        set_err(err, err_len, "%s is %u bytes, outside what this reader will load", kArchiveName,
            archive_size);
        return false;
    }

    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> compressed;
    if (!read_data_df(data_df, archive_offset, archive_size, compressed, err, err_len)) return false;
    rt_log("ui", "title logo: read %u compressed bytes of %s in %.1f ms", archive_size, kArchiveName,
        ms_since(t0));

    /* Two inflates rather than one: the table has to be read before the byte
     * range the wanted files occupy is known. Both stop as soon as they have
     * enough, which for the second one is most of the archive, because the
     * title animation is stored near the end of it. The three meshes are all
     * inside the first 3.3 MB; the animation is what costs the rest. */
    t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> image;
    char ierr[256];
    if (!rt_inflate_raw(compressed.data(), compressed.size(), image, 256u * 1024,
                        kMaxArchiveBytes, ierr, sizeof(ierr))) {
        set_err(err, err_len, "%s: %s", kArchiveName, ierr);
        return false;
    }
    std::vector<InnerEntry> entries;
    if (!parse_inner_table(image, entries, err, err_len)) return false;

    /* The wanted files' offsets come from the table, but their sizes need the
     * next entry's offset, so the limit for the second inflate is the highest
     * of those. */
    WantedFile wanted[kWantCount];
    for (int k = 0; k < kLetterCount; ++k) wanted[k].name = kMeshNames[k];
    wanted[kLetterCount].name = kAnimName;

    size_t need_end = 0;
    for (int i = 0; i < kWantCount; ++i) {
        size_t at = entries.size();
        for (size_t j = 0; j < entries.size(); ++j) {
            if (entries[j].name == wanted[i].name) {
                at = j;
                break;
            }
        }
        if (at == entries.size()) {
            set_err(err, err_len, "%s has no entry named '%s' (%zu entries)", kArchiveName,
                wanted[i].name, entries.size());
            return false;
        }
        /* One past the entry, because the size is the distance to the next
         * one's data. A wanted file that is last needs the whole archive,
         * which is the ceiling and not SIZE_MAX: the inflate has to stop
         * somewhere, and this reader will not hold more than the ceiling
         * anyway. */
        if (at + 1 >= entries.size()) {
            need_end = kMaxArchiveBytes;
            break;
        }
        need_end = std::max<size_t>(need_end, entries[at + 1].offset);
    }

    if (!rt_inflate_raw(compressed.data(), compressed.size(), image, need_end, kMaxArchiveBytes,
                        ierr, sizeof(ierr))) {
        set_err(err, err_len, "%s: %s", kArchiveName, ierr);
        return false;
    }
    rt_log("ui", "title logo: inflated %zu of %u bytes of %s in %.1f ms", image.size(), archive_size,
        kArchiveName, ms_since(t0));

    for (int i = 0; i < kWantCount; ++i) {
        if (!locate(entries, image.size(), wanted[i], err, err_len)) return false;
    }

    /* Placement. There is no fallback: the letters are placed where this
     * disc's own animation puts them or the image is not built at all, and
     * the launcher keeps its text title. Substituting numbers written into
     * this file would be putting disc-derived data in the source tree, and
     * would draw a wordmark this disc did not describe. */
    TitleNode nodes[kLetterCount];
    const WantedFile& anim = wanted[kLetterCount];
    if (!parse_bga_nodes(image.data() + anim.offset, anim.size, nodes, err, err_len)) return false;
    rt_log("ui", "title logo: placement from %s: I %g, C %g, O %g (y %g, scale %g)", kAnimName,
        double(nodes[0].x), double(nodes[1].x), double(nodes[2].x), double(nodes[0].y),
        double(nodes[0].sx));

    P2oMesh meshes[kLetterCount];
    unsigned per_letter[kLetterCount] = {0, 0, 0};
    for (int k = 0; k < kLetterCount; ++k) {
        const WantedFile& mesh = wanted[k];
        if (!parse_p2o(image.data() + mesh.offset, mesh.size, mesh.name, meshes[k], err, err_len)) {
            return false;
        }
        per_letter[k] = unsigned(meshes[k].tri.size() / 3);
        rt_log("ui", "title logo: '%s' at archive offset %zu, %zu bytes, %zu positions, %u triangles,"
                     " texture '%s', colour %02x%02x%02x%02x, placed at x %g",
            mesh.name, mesh.offset, mesh.size, meshes[k].x.size(), per_letter[k],
            meshes[k].texture.c_str(), meshes[k].colour[0], meshes[k].colour[1], meshes[k].colour[2],
            meshes[k].colour[3], double(nodes[k].x));
    }
    std::memcpy(colour, meshes[0].colour, 4);

    /* World space, then a box: the union of the placed letters, grown by the
     * border on all four sides, normalised so the box is exactly 1.0 tall.
     * Everything from here on is unitless, which is what lets the raster
     * choose a pixel size later without touching the disc again. */
    float cx0 = 0.0f, cx1 = 0.0f, cy0 = 0.0f, cy1 = 0.0f;
    bool first = true;
    for (int k = 0; k < kLetterCount; ++k) {
        for (size_t i = 0; i < meshes[k].x.size(); ++i) {
            const float x = nodes[k].x + meshes[k].x[i] * nodes[k].sx;
            const float y = nodes[k].y + meshes[k].y[i] * nodes[k].sy;
            if (first) {
                cx0 = cx1 = x;
                cy0 = cy1 = y;
                first = false;
            } else {
                cx0 = std::min(cx0, x);
                cx1 = std::max(cx1, x);
                cy0 = std::min(cy0, y);
                cy1 = std::max(cy1, y);
            }
        }
    }
    const float content_w = cx1 - cx0, content_h = cy1 - cy0;
    if (!(content_w > 0.0f) || !(content_h > 0.0f)) {
        set_err(err, err_len, "the placed letters span %g by %g world units", double(content_w),
            double(content_h));
        return false;
    }
    const float margin = content_w * kMarginFraction;
    const float box_w = content_w + 2.0f * margin, box_h = content_h + 2.0f * margin;
    const float bx0 = cx0 - margin, by0 = cy0 - margin;

    geom.aspect = box_w / box_h;
    geom.xy.clear();
    for (int k = 0; k < kLetterCount; ++k) {
        for (size_t t = 0; t + 2 < meshes[k].tri.size(); t += 3) {
            for (int i = 0; i < 3; ++i) {
                const uint32_t idx = meshes[k].tri[t + i];
                const float x = nodes[k].x + meshes[k].x[idx] * nodes[k].sx;
                const float y = nodes[k].y + meshes[k].y[idx] * nodes[k].sy;
                geom.xy.push_back((x - bx0) / box_h);
                geom.xy.push_back((y - by0) / box_h);
            }
        }
    }
    if (!geom.valid() || geom.triangles() > kMaxTriangles) {
        set_err(err, err_len, "the geometry came out %zu triangles at aspect %g", geom.triangles(),
            double(geom.aspect));
        return false;
    }

    rt_log("ui", "title logo: meshes I.p2o %u, C.p2o %u, O.p2o %u triangles (%zu in all); letters"
                 " span %.4f by %.4f world units, box %.4f by %.4f with a %.4f border, aspect %.4f",
        per_letter[0], per_letter[1], per_letter[2], geom.triangles(), double(content_w),
        double(content_h), double(box_w), double(box_h), double(margin), double(geom.aspect));
    return true;
}

/* The geometry, kept for the life of the process once it has been read. A
 * window-scale change then costs one raster and no disc access at all. */
TitleGeometry g_geometry;
uint8_t g_colour[4] = {0x80, 0x80, 0x80, 0x80};

/* Reads the geometry into g_geometry, from the cache when one matches this
 * disc and by extraction otherwise (writing the cache on the way out). */
bool ensure_geometry(char* err, size_t err_len) {
    if (g_geometry.valid()) return true;

    if (!rt_iso_mounted()) {
        set_err(err, err_len, "no disc is mounted");
        return false;
    }
    RtIsoFile data_df;
    if (!rt_iso_search("/DFDATAS/DATA.DF", &data_df)) {
        set_err(err, err_len, "the mounted disc has no DFDATAS/DATA.DF");
        return false;
    }

    const uint64_t key = disc_key(data_df);
    const auto t0 = std::chrono::steady_clock::now();
    if (cache_load(key, g_geometry, g_colour)) {
        rt_log("ui", "title logo: cache hit, %zu triangles at aspect %.4f from '%s' in %.1f ms",
            g_geometry.triangles(), double(g_geometry.aspect), cache_path(), ms_since(t0));
        return true;
    }

    rt_log("ui", "title logo: building from '%s' (DATA.DF at LSN %u, %u bytes, disc key %016llx)",
        rt_iso_mounted_path(), data_df.lsn, data_df.size, (unsigned long long)key);
    if (!extract(data_df, g_geometry, g_colour, err, err_len)) {
        g_geometry = TitleGeometry();
        return false;
    }
    rt_log("ui", "title logo: geometry built in %.1f ms", ms_since(t0));
    cache_store(key, g_geometry, g_colour);
    return true;
}

} // namespace

/* The one entry point, and the only place the "never fatal" promise in
 * title_logo.h is kept: everything below it allocates from sizes a disc gave
 * it, and an allocation failure or any other exception has to come out as a
 * false return with a reason, not as an unwound stack through a caller that
 * only wanted a launcher decoration. */
bool rt_title_logo_build(uint32_t width_px, uint32_t height_px, RtTitleLogo& out, char* err,
                         size_t err_len) try {
    out = RtTitleLogo();
    if (!ensure_geometry(err, err_len)) return false;

    const auto t0 = std::chrono::steady_clock::now();
    if (!rasterise(g_geometry, width_px, height_px, g_colour, out, err, err_len)) {
        out = RtTitleLogo();
        return false;
    }
    rt_log("ui", "title logo: rasterised %zu triangles into %ux%u in %.2f ms (aspect %.4f against"
                 " the box's %.4f)",
        g_geometry.triangles(), out.width, out.height, ms_since(t0), double(g_geometry.aspect),
        double(width_px) / double(height_px));
    return true;
} catch (const std::exception& e) {
    g_geometry = TitleGeometry();
    out = RtTitleLogo();
    set_err(err, err_len, "the build threw: %s", e.what());
    rt_log("ui", "title logo: the build threw (%s); the launcher keeps its text title", e.what());
    return false;
} catch (...) {
    g_geometry = TitleGeometry();
    out = RtTitleLogo();
    set_err(err, err_len, "the build threw a non-standard exception");
    rt_log("ui", "title logo: the build threw a non-standard exception; the launcher keeps its"
                 " text title");
    return false;
}

float rt_title_logo_aspect() {
    return g_geometry.valid() ? g_geometry.aspect : 0.0f;
}

const char* rt_title_logo_cache_path() {
    return cache_path();
}
