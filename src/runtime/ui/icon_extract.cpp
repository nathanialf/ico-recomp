/* ui/icon_extract.cpp: renders the PS2 memory-card save icon on the user's
 * disc into a Windows .ico, for the build to embed as icorecomp-runtime's
 * icon (see the `icon` CMake target in CMakeLists.txt).
 *
 * A standalone host tool: no SDL, no RmlUi, no GS. Built and run at package
 * time only, never shipped and never run by a player. Nothing this writes
 * is ever committed: tools/check_no_rom.sh blocks *.ico and *.png outright,
 * and this tool also refuses an output path that resolves under the source
 * root itself, the same rule ui/title_logo_selftest.cpp's file comment
 * documents for its own PNG.
 *
 *   icorecomp-icon-extract <disc.iso|.bin> <out.ico> [--png <dir>] [--size N]
 *
 * --size N renders that one size instead of the default set; N is 1..256.
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
 */
#include "ps2_icon.h"
#include "ps2_icon_render.h"
#include "save_icon.h"

#include "../host/png_write.h"
#include "../iso/iso9660.h"
#include "../runtime.h"
#include "recomp_api.h"

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
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <disc.iso|.bin> <out.ico> [--png <dir>] [--size 1..256]\n"
            "the .ico path is required and must be outside the repository: it is\n"
            "game-derived pixels\n",
            argv[0] ? argv[0] : "icorecomp-icon-extract");
        return 2;
    }
    const std::string disc_path = argv[1];
    const std::string out_path = argv[2];
    std::string png_dir;
    uint32_t only_size = 0;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--png") == 0 && i + 1 < argc) {
            png_dir = argv[++i];
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

    const std::vector<uint8_t> ico = build_ico(images);
    std::FILE* f = std::fopen(out_path.c_str(), "wb");
    const bool wrote = f && std::fwrite(ico.data(), 1, ico.size(), f) == ico.size();
    const bool closed = f && std::fclose(f) == 0;
    if (!wrote || !closed) {
        std::fprintf(stderr, "icon-extract: cannot write '%s'\n", out_path.c_str());
        return 1;
    }
    std::printf("wrote %s (%zu bytes, %zu entries)\n", out_path.c_str(), ico.size(), images.size());
    return 0;
}
