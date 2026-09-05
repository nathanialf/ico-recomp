/* ui/icon_extract.cpp: renders the PS2 memory-card save icon on the user's
 * disc into a Windows .ico, and on request a macOS .icns, for the build to
 * embed as icorecomp-runtime's icon and as the app bundle's icon (see the
 * `icon` CMake target in CMakeLists.txt).
 *
 * A standalone host tool: no SDL, no RmlUi, no GS. Built and run at package
 * time only, never shipped and never run by a player. Nothing this writes
 * is ever committed: tools/check_no_rom.sh blocks *.png, *.ico and *.icns
 * outright, and this tool also refuses an output path that resolves under
 * the source root itself, the same rule ui/title_logo_selftest.cpp's file
 * comment documents for its own PNG. The two refusals are independent on
 * purpose: the gate catches a file that reaches `git add`, the path check
 * catches it a step earlier, before the tool writes it at all.
 *
 *   icorecomp-icon-extract <disc.iso|.bin> <out.ico> [--png <dir>] [--size N]
 *                          [--icns <out.icns>]
 *
 * --size N renders that one size instead of the default set; N is 1..256.
 * It does not change the .icns, which always carries its own four sizes.
 *
 * Reads and parses the disc's save icon through ui/save_icon.cpp, the same
 * function the runtime uses for its window icon, so the packaged icon and
 * the window icon cannot drift apart in which file they pick or how they
 * validate it. Renders shape 0 at the sizes a Windows .ico conventionally
 * carries (16, 24, 32, 48, 64, 128, 256, or one size when --size is given,
 * up to the 256 an ICONDIRENTRY can name). The result is written as an
 * ICONDIR of
 * PNG-compressed entries, the format Windows Vista and later accept for an
 * .ico (see build_ico below); --png also writes one PNG a size into the
 * given directory, for looking at without a Windows machine.
 *
 * --icns writes the same renders as a macOS icon container (see build_icns
 * below), at the four sizes an Apple Silicon build wants: 128, 256, 512 and
 * 1024. It is written here rather than by iconutil so the packaged bundle's
 * icon can be produced on any host, including the Linux one this port is
 * developed on, where iconutil does not exist.
 */
#include "ps2_icon.h"
#include "ps2_icon_render.h"
#include "save_icon.h"

#include "../host/png_write.h"
#include "../iso/iso9660.h"
#include "../runtime.h"
#include "recomp_api.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

/* loader.cpp and log.cpp reference the guest page table; nothing here loads
 * an ELF or dumps registers, so an empty one satisfies the link. The
 * recomp_api.h include above matters: it declares g_pages inside
 * extern "C", and without it this definition gets plain C++ linkage, which
 * MSVC mangles and GCC does not (see guest/menu_nav_selftest.cpp's file
 * comment, which hit the same thing). */
uint8_t* g_pages[0x10000];

namespace {

constexpr uint32_t kDefaultSizes[] = {16, 24, 32, 48, 64, 128, 256};

/* The .icns entries, and the four-character type code each size goes in
 * under. These four are the PNG-backed types every macOS that this port
 * targets (14 and later) reads: ic07 128x128, ic08 256x256, ic09 512x512,
 * ic10 1024x1024. Finder picks whichever it needs. The renderer accepts any
 * size up to 4096 (ps2_icon_render.cpp's kMaxIconDim), so all four are
 * always produced; the "at minimum ic07 and ic08" fallback in the plan
 * would only matter for a source that could not be rendered large, which
 * this vector icon is not. */
struct IcnsEntry {
    const char* type;
    uint32_t size_px;
};
constexpr IcnsEntry kIcnsEntries[] = {
    {"ic07", 128}, {"ic08", 256}, {"ic09", 512}, {"ic10", 1024},
};

/* An ICONDIRENTRY stores each dimension in one byte with 0 meaning 256, so
 * 256 is the largest size the format can name and --size may not go past
 * it. */
constexpr uint32_t kMaxIcoSize = 256;

void push_le16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
}

void push_le32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 24));
}

/* Windows .ico, PNG-compressed entries (accepted since Vista):
 *   ICONDIR       {u16 0, u16 type=1, u16 count}
 *   ICONDIRENTRY  {u8 width (0 means 256), u8 height (0 means 256), u8 0,
 *                  u8 0, u16 planes=1, u16 bpp=32, u32 bytes, u32 offset}
 *                 one a size, back to back
 *   payload       each entry's PNG, back to back, at its declared offset
 */
std::vector<uint8_t> build_ico(const std::vector<RtPs2IconImage>& images) {
    std::vector<std::vector<uint8_t>> pngs;
    pngs.reserve(images.size());
    for (const auto& img : images) pngs.push_back(rt_png_encode(img.width, img.height, img.rgba.data(), 4));

    std::vector<uint8_t> out;
    push_le16(out, 0);
    push_le16(out, 1);
    push_le16(out, uint16_t(images.size()));

    uint32_t offset = uint32_t(6 + images.size() * 16);
    for (size_t i = 0; i < images.size(); ++i) {
        /* 1..255 go in as themselves, 256 as 0. Nothing larger reaches here:
         * main() bounds every size to kMaxIcoSize first, because writing 0
         * for a 300 px image would tell Windows it is a 256 px one. */
        const uint32_t w = images[i].width, h = images[i].height;
        out.push_back(uint8_t(w == 256 ? 0 : w));
        out.push_back(uint8_t(h == 256 ? 0 : h));
        out.push_back(0);
        out.push_back(0);
        push_le16(out, 1);
        push_le16(out, 32);
        push_le32(out, uint32_t(pngs[i].size()));
        push_le32(out, offset);
        offset += uint32_t(pngs[i].size());
    }
    for (const auto& p : pngs) out.insert(out.end(), p.begin(), p.end());
    return out;
}

void push_be32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}

/* macOS .icns, PNG-backed entries:
 *   header  'icns' then a big-endian u32 holding the length of the WHOLE
 *           file, the 8 header bytes included
 *   entry   four-character type ('ic07'..'ic10') then a big-endian u32
 *           holding that entry's length, its own 8-byte header included,
 *           then the payload, back to back
 * Everything in this container is big-endian, unlike the .ico above, which
 * is why the two builders do not share a push helper.
 *
 * `images` is looked up by size, not by position: it holds every size the
 * run rendered, of which the .icns takes four. A size that is missing is
 * skipped rather than faked, so a container is always internally
 * consistent, and its size is appended to `missing` so the caller can say
 * so. An .icns with no entries at all is a valid 8-byte container and a
 * useless icon, which is the kind of quiet wrong answer this codebase
 * refuses, so the caller treats an empty `body` as a failure. */
std::vector<uint8_t> build_icns(const std::vector<RtPs2IconImage>& images,
    std::vector<uint32_t>* missing, bool* any_entry) {
    std::vector<uint8_t> body;
    for (const IcnsEntry& e : kIcnsEntries) {
        const RtPs2IconImage* img = nullptr;
        for (const auto& candidate : images) {
            if (candidate.width == e.size_px && candidate.height == e.size_px) {
                img = &candidate;
                break;
            }
        }
        if (!img) {
            missing->push_back(e.size_px);
            continue;
        }
        const std::vector<uint8_t> png =
            rt_png_encode(img->width, img->height, img->rgba.data(), 4);
        body.insert(body.end(), e.type, e.type + 4);
        push_be32(body, uint32_t(png.size() + 8));
        body.insert(body.end(), png.begin(), png.end());
    }

    *any_entry = !body.empty();

    std::vector<uint8_t> out;
    const char magic[4] = {'i', 'c', 'n', 's'};
    out.insert(out.end(), magic, magic + 4);
    push_be32(out, uint32_t(body.size() + 8));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/* Both containers are written the same way, so the error text and the
 * close-failure check stay in one place. */
bool write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    const bool wrote = f && std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    const bool closed = f && std::fclose(f) == 0;
    if (!wrote || !closed) {
        std::fprintf(stderr, "icon-extract: cannot write '%s'\n", path.c_str());
        return false;
    }
    return true;
}

/* True when `path` resolves inside ICORECOMP_SOURCE_ROOT but outside its
 * top-level build/ directory. weakly_canonical so a path that does not
 * exist yet (the usual case for an output file) still resolves through any
 * existing parent symlinks; falls back to the path as given if
 * canonicalisation itself fails.
 *
 * build/ (gitignored, see .gitignore's "/build/") is the CMake `icon`
 * target's own output location (${CMAKE_BINARY_DIR}, ordinarily
 * <root>/build/<preset>), so it is not "the repository" in the sense that
 * matters here: nothing under it can ever be committed. Anywhere else
 * inside the source tree is refused, matching tools/check_no_rom.sh's *.ico
 * and *.png extension gate and ui/title_logo_selftest.cpp's file comment,
 * which states the same rule for its own PNG output. */
bool resolves_under_repo(const std::string& path) {
    std::error_code ec;
    std::filesystem::path cand = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
    if (ec) cand = std::filesystem::absolute(std::filesystem::path(path), ec);
    std::filesystem::path root =
        std::filesystem::weakly_canonical(std::filesystem::path(ICORECOMP_SOURCE_ROOT), ec);
    if (ec) root = std::filesystem::path(ICORECOMP_SOURCE_ROOT);

    const std::filesystem::path rel = cand.lexically_relative(root);
    if (rel.empty()) return false;
    const std::string first = rel.begin()->string();
    if (first == "..") return false;
    if (first == "build") return false;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    /* Info, not the runtime's warn default: this tool's output is its
     * progress lines, and they are all info. */
    rt_log_set_initial_level(RT_LOG_INFO);

    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <disc.iso|.bin> <out.ico> [--png <dir>] [--size 1..256]\n"
            "                                   [--icns <out.icns>]\n"
            "the .ico path is required and must be outside the repository: it is\n"
            "game-derived pixels, and so is the .icns\n",
            argv[0] ? argv[0] : "icorecomp-icon-extract");
        return 2;
    }
    const std::string disc_path = argv[1];
    const std::string out_path = argv[2];
    std::string png_dir;
    std::string icns_path;
    uint32_t only_size = 0;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) {
            png_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--icns") == 0 && i + 1 < argc) {
            icns_path = argv[++i];
        } else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            const char* text = argv[++i];
            char* end = nullptr;
            const unsigned long n = std::strtoul(text, &end, 10);
            if (end == text || *end != 0 || n < 1 || n > kMaxIcoSize) {
                std::fprintf(stderr, "icon-extract: --size '%s' is not a whole number in 1..%u\n",
                    text, kMaxIcoSize);
                return 2;
            }
            only_size = uint32_t(n);
        } else {
            std::fprintf(stderr, "icon-extract: unrecognised argument '%s'\n", argv[i]);
            return 2;
        }
    }

    if (resolves_under_repo(out_path)) {
        std::fprintf(stderr,
            "icon-extract: refusing to write '%s': it resolves under the source root (%s); the .ico\n"
            "is game-derived pixels and must never land in the repository\n",
            out_path.c_str(), ICORECOMP_SOURCE_ROOT);
        return 1;
    }
    if (!png_dir.empty() && resolves_under_repo(png_dir)) {
        std::fprintf(stderr,
            "icon-extract: refusing to write PNGs under '%s': it resolves under the source root"
            " (%s)\n",
            png_dir.c_str(), ICORECOMP_SOURCE_ROOT);
        return 1;
    }
    /* The .icns gets the same pair of refusals as the .ico and the PNGs:
     * check_no_rom.sh's extension list carries it, and this check stops it
     * being written under the source root in the first place. */
    if (!icns_path.empty() && resolves_under_repo(icns_path)) {
        std::fprintf(stderr,
            "icon-extract: refusing to write '%s': it resolves under the source root (%s); the\n"
            ".icns is game-derived pixels and must never land in the repository\n",
            icns_path.c_str(), ICORECOMP_SOURCE_ROOT);
        return 1;
    }

    char err[1024];
    if (!rt_iso_try_mount(disc_path.c_str(), err, sizeof(err))) {
        std::fprintf(stderr, "icon-extract: cannot mount '%s': %s\n", disc_path.c_str(), err);
        return 1;
    }
    std::printf("disc: %s, %u sectors of %u bytes\n", disc_path.c_str(), rt_iso_total_sectors(),
        rt_iso_sector_size());

    RtPs2Icon icon;
    RtPs2IconSys icon_sys;
    if (!rt_save_icon_load(icon, icon_sys, err, sizeof(err))) {
        std::fprintf(stderr, "icon-extract: %s\n", err);
        return 1;
    }
    std::printf("icon.sys: view icon '%s', copy icon '%s', delete icon '%s', background corner 0"
                " (%u,%u,%u,%u)\n",
        icon_sys.view_icon.c_str(), icon_sys.copy_icon.c_str(), icon_sys.delete_icon.c_str(),
        icon_sys.bg_corner[0][0], icon_sys.bg_corner[0][1], icon_sys.bg_corner[0][2],
        icon_sys.bg_corner[0][3]);
    std::printf("save icon: %u shapes, attrib %u, %u vertices\n", icon.shapes, icon.attrib,
        icon.vertex_count);

    std::vector<uint32_t> sizes;
    if (only_size) {
        sizes.push_back(only_size);
    } else {
        for (uint32_t s : kDefaultSizes) sizes.push_back(s);
    }
    /* The .ico set stops at 256, the size an ICONDIRENTRY can name, so the
     * .icns sizes above that are appended here rather than folded into the
     * set above. A size already in the list is not rendered twice; the
     * container builders find what they need by size. */
    const size_t ico_size_count = sizes.size();
    if (!icns_path.empty()) {
        for (const IcnsEntry& e : kIcnsEntries) {
            bool have = false;
            for (uint32_t s : sizes) have = have || s == e.size_px;
            if (!have) sizes.push_back(e.size_px);
        }
    }

    /* The .ico's own directory, not only the PNG one: the CMake `icon`
     * target writes both into a build subdirectory that does not exist
     * before the first run, and without this the .ico write would only
     * work because --png happened to create the same directory first. */
    auto ensure_dir = [](const std::filesystem::path& dir) {
        if (dir.empty()) return true;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec && !std::filesystem::is_directory(dir)) {
            std::fprintf(stderr, "icon-extract: cannot create '%s': %s\n", dir.string().c_str(),
                ec.message().c_str());
            return false;
        }
        return true;
    };
    if (!ensure_dir(std::filesystem::path(out_path).parent_path())) return 1;
    if (!png_dir.empty() && !ensure_dir(std::filesystem::path(png_dir))) return 1;
    if (!icns_path.empty() && !ensure_dir(std::filesystem::path(icns_path).parent_path())) return 1;

    std::vector<RtPs2IconImage> images;
    images.reserve(sizes.size());
    for (uint32_t size_px : sizes) {
        RtPs2IconImage img;
        if (!rt_ps2_icon_render(icon, icon_sys, size_px, 0, img, err, sizeof(err))) {
            std::fprintf(stderr, "icon-extract: rendering %u px: %s\n", size_px, err);
            return 1;
        }
        std::printf("rendered %ux%u\n", img.width, img.height);
        if (!png_dir.empty()) {
            char name[32];
            std::snprintf(name, sizeof(name), "%u.png", size_px);
            const std::string p = (std::filesystem::path(png_dir) / name).string();
            if (!rt_png_write(p.c_str(), img.width, img.height, img.rgba.data(), 4, err, sizeof(err))) {
                std::fprintf(stderr, "icon-extract: %s\n", err);
                return 1;
            }
            std::printf("wrote %s\n", p.c_str());
        }
        images.push_back(std::move(img));
    }

    /* The .ico carries only the sizes it can name; anything appended for
     * the .icns above stays out of it. */
    const std::vector<RtPs2IconImage> ico_images(
        images.begin(), images.begin() + std::ptrdiff_t(ico_size_count));
    const std::vector<uint8_t> ico = build_ico(ico_images);
    if (!write_file(out_path, ico)) return 1;
    std::printf("wrote %s (%zu bytes, %zu entries)\n", out_path.c_str(), ico.size(),
        ico_images.size());

    if (!icns_path.empty()) {
        std::vector<uint32_t> missing;
        bool any_entry = false;
        const std::vector<uint8_t> icns = build_icns(images, &missing, &any_entry);
        if (!missing.empty()) {
            std::string list;
            for (uint32_t s : missing) {
                if (!list.empty()) list += ", ";
                list += std::to_string(s);
            }
            std::fprintf(stderr, "icon-extract: no render at %s for the .icns\n", list.c_str());
        }
        if (!any_entry) {
            std::fprintf(stderr,
                "icon-extract: refusing to write '%s': not one of the four .icns sizes was"
                " rendered, so the container would carry no image\n", icns_path.c_str());
            return 1;
        }
        if (!write_file(icns_path, icns)) return 1;
        std::printf("wrote %s (%zu bytes, %zu of %zu entries)\n", icns_path.c_str(), icns.size(),
            sizeof(kIcnsEntries) / sizeof(kIcnsEntries[0]) - missing.size(),
            sizeof(kIcnsEntries) / sizeof(kIcnsEntries[0]));
    }
    return 0;
}
