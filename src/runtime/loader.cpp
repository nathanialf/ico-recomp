/* loader.cpp: config/recomp.toml reader, ELF SHA-1 pin check, and PT_LOAD
 * segment loader.
 *
 * TOML parsing is a hand-rolled, minimal key lookup (line-based, one section
 * tracked at a time): we only need four scalar keys out of
 * config/recomp.toml ([decomp].root, [decomp].elf, [pins].elf_sha1,
 * [target].entry/vram_base/gp). Anything else in the file (including the
 * multi-line vu1_sources array) is silently skipped -- lines without a bare
 * top-level '=' are ignored, so array continuation lines never confuse the
 * section tracker. No TOML library dependency, per CLAUDE.md's "no heavy
 * deps" guidance for this loader.
 *
 * ELF32 header/program-header layout is hand-rolled too (not <elf.h>) to
 * keep this file self-contained and portable; the field layout is the
 * standard public ELF32 ABI, not anything decomp-derived.
 */
#include "runtime.h"

#include "iso/iso9660.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

#pragma pack(push, 1)
struct Elf32Ehdr {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};
struct Elf32Phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
};
#pragma pack(pop)

static_assert(sizeof(Elf32Ehdr) == 52, "Elf32Ehdr must match the on-disk ELF32 header layout");
static_assert(sizeof(Elf32Phdr) == 32, "Elf32Phdr must match the on-disk ELF32 program header layout");

constexpr uint32_t kPtLoad = 1;
constexpr uint16_t kEtExec = 2;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/* Strips a trailing '#' comment (values in this file never contain '#') and
 * surrounding double quotes, if present. */
std::string strip_value(std::string v) {
    size_t hash = v.find('#');
    if (hash != std::string::npos) v = v.substr(0, hash);
    v = trim(v);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
        v = v.substr(1, v.size() - 2);
    }
    return v;
}

void write_guest_bytes(uint32_t vaddr, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        uint32_t addr = vaddr + uint32_t(off);
        uint8_t* page = g_pages[addr >> 16];
        if (!page) rt_fatal("loader", nullptr, "vaddr 0x%08x not mapped (page table gap) while loading PT_LOAD data", addr);
        uint32_t page_off = addr & 0xFFFF;
        size_t chunk = std::min<size_t>(len - off, size_t(0x10000) - page_off);
        std::memcpy(page + page_off, data + off, chunk);
        off += chunk;
    }
}

void zero_guest_bytes(uint32_t vaddr, size_t len) {
    size_t off = 0;
    while (off < len) {
        uint32_t addr = vaddr + uint32_t(off);
        uint8_t* page = g_pages[addr >> 16];
        if (!page) rt_fatal("loader", nullptr, "vaddr 0x%08x not mapped (page table gap) while zeroing bss", addr);
        uint32_t page_off = addr & 0xFFFF;
        size_t chunk = std::min<size_t>(len - off, size_t(0x10000) - page_off);
        std::memset(page + page_off, 0, chunk);
        off += chunk;
    }
}

/* Packaged-run defaults, mirroring the committed config/recomp.toml
 * ([pins].elf_sha1 is one of the two approved SHA-1 pins; the [target]
 * values are committed config facts, not ROM data). In this mode there is
 * no decomp checkout: the boot ELF is read out of the user's disc image
 * (SCUS_971.13, byte-identical to the pinned ELF) by rt_load_elf. */
constexpr char kPinElfSha1[] = "a4d8fc1948fb2da2395f3863a94a3b93de55de14";
constexpr uint32_t kTargetEntry = 0x00100008;
constexpr uint32_t kTargetVramBase = 0x00100000;
constexpr uint32_t kTargetGp = 0x006388F0;

} // namespace

#ifndef ICORECOMP_SOURCE_ROOT
#error "ICORECOMP_SOURCE_ROOT must be defined by the build (see CMakeLists.txt)"
#endif

const char* rt_base_dir() {
    static const std::string base = [] {
        std::string root = ICORECOMP_SOURCE_ROOT;
        std::ifstream probe(root + "/config/recomp.toml");
        if (probe) return root;
        /* Packaged runtime: the build machine's source tree is not here.
         * Resolve everything against the working directory instead. */
        return std::string(".");
    }();
    return base.c_str();
}

bool rt_load_config(LoaderConfig* out) {
    std::string path = std::string(rt_base_dir()) + "/config/recomp.toml";
    std::ifstream f(path);
    if (!f) {
        std::snprintf(out->elf_sha1, sizeof(out->elf_sha1), "%s", kPinElfSha1);
        out->entry = kTargetEntry;
        out->vram_base = kTargetVramBase;
        out->gp = kTargetGp;
        rt_log("loader", "no config/recomp.toml (packaged run): using compiled-in pins/target "
            "(elf_sha1=%s entry=0x%08x gp=0x%08x); boot ELF will come from the disc image",
            out->elf_sha1, out->entry, out->gp);
        return true;
    }

    std::string section;
    std::string line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[') {
            size_t end = t.find(']');
            if (end != std::string::npos) section = trim(t.substr(1, end - 1));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue; /* array continuation line, etc. */
        std::string key = trim(t.substr(0, eq));
        std::string val = strip_value(t.substr(eq + 1));

        if (section == "decomp") {
            if (key == "root") std::snprintf(out->decomp_root, sizeof(out->decomp_root), "%s", val.c_str());
            else if (key == "elf") std::snprintf(out->decomp_elf, sizeof(out->decomp_elf), "%s", val.c_str());
        } else if (section == "pins") {
            if (key == "elf_sha1") std::snprintf(out->elf_sha1, sizeof(out->elf_sha1), "%s", val.c_str());
        } else if (section == "target") {
            if (key == "entry") out->entry = uint32_t(std::strtoul(val.c_str(), nullptr, 0));
            else if (key == "vram_base") out->vram_base = uint32_t(std::strtoul(val.c_str(), nullptr, 0));
            else if (key == "gp") out->gp = uint32_t(std::strtoul(val.c_str(), nullptr, 0));
        }
    }

    if (!out->decomp_root[0] || !out->decomp_elf[0] || !out->elf_sha1[0] || !out->entry) {
        rt_log("loader", "config/recomp.toml missing required keys (root='%s' elf='%s' elf_sha1='%s' entry=0x%x)",
            out->decomp_root, out->decomp_elf, out->elf_sha1, out->entry);
        return false;
    }
    rt_log("loader", "config: decomp.root=%s decomp.elf=%s pins.elf_sha1=%s target.entry=0x%08x target.vram_base=0x%08x target.gp=0x%08x",
        out->decomp_root, out->decomp_elf, out->elf_sha1, out->entry, out->vram_base, out->gp);
    return true;
}

void rt_resolve_elf_path(const LoaderConfig& cfg, char* buf, size_t buf_size) {
    std::snprintf(buf, buf_size, "%s/%s/%s", rt_base_dir(), cfg.decomp_root, cfg.decomp_elf);
}

namespace {

/* Reads a whole file. Returns false (out untouched) when it cannot be
 * opened; fatal on a short read after a successful open. */
bool read_whole_file(const char* path, std::vector<uint8_t>* out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    if (size < 0) {
        std::fclose(f);
        rt_fatal("loader", nullptr, "ftell('%s') failed", path);
    }
    std::fseek(f, 0, SEEK_SET);
    out->resize(size_t(size));
    if (size > 0 && std::fread(out->data(), 1, size_t(size), f) != size_t(size)) {
        std::fclose(f);
        rt_fatal("loader", nullptr, "short read on '%s'", path);
    }
    std::fclose(f);
    return true;
}

/* Reads SCUS_971.13 out of the mounted disc image (mounting it first; the
 * mount is fatal when no image is found, and already verifies the file
 * exists on the disc). */
void read_elf_from_disc(std::vector<uint8_t>* out) {
    rt_iso_mount();
    RtIsoFile f;
    if (!rt_iso_search("\\SCUS_971.13;1", &f)) {
        rt_fatal("loader", nullptr, "mounted disc has no SCUS_971.13 (wrong image?)");
    }
    out->resize(f.size);
    uint8_t sec[2048];
    uint32_t remaining = f.size;
    for (uint32_t i = 0; remaining > 0; ++i) {
        if (!rt_iso_read_sector(f.lsn + i, sec)) {
            rt_fatal("loader", nullptr, "disc read failed at LBA %u while extracting SCUS_971.13", f.lsn + i);
        }
        uint32_t chunk = remaining < 2048 ? remaining : 2048;
        std::memcpy(out->data() + (size_t(i) * 2048), sec, chunk);
        remaining -= chunk;
    }
    rt_log("loader", "boot ELF read from disc: SCUS_971.13, LBA %u, %u bytes", f.lsn, f.size);
}

/* Bounds-checked read out of the in-memory ELF image. */
void image_read(const std::vector<uint8_t>& img, size_t off, void* dst, size_t len,
                const char* what, const char* src) {
    if (off + len > img.size() || off + len < off) {
        rt_fatal("loader", nullptr, "short read on %s of '%s' (offset %zu + %zu > %zu bytes)",
            what, src, off, len, img.size());
    }
    std::memcpy(dst, img.data() + off, len);
}

} // namespace

void rt_load_elf(const LoaderConfig& cfg) {
    std::vector<uint8_t> elf;
    char elf_src[1536]; /* comfortably covers the base dir + two 512-byte config fields + separators */

    if (cfg.decomp_root[0] && cfg.decomp_elf[0]) {
        rt_resolve_elf_path(cfg, elf_src, sizeof(elf_src));
        rt_log("loader", "ELF path: %s", elf_src);
        if (!read_whole_file(elf_src, &elf)) {
            rt_log("loader", "'%s' not readable; falling back to the disc image's SCUS_971.13", elf_src);
        }
    }
    if (elf.empty()) {
        read_elf_from_disc(&elf);
        std::snprintf(elf_src, sizeof(elf_src), "SCUS_971.13 (from the mounted disc image)");
    }

    Sha1Digest digest = rt_sha1_buffer(elf.data(), elf.size());
    char hex[41];
    rt_sha1_to_hex(digest, hex);
    if (!rt_sha1_equals_hex(digest, cfg.elf_sha1)) {
        rt_fatal("loader", nullptr, "SHA-1 mismatch for '%s': got %s, expected %s ([pins].elf_sha1)",
            elf_src, hex, cfg.elf_sha1);
    }
    rt_log("loader", "SHA-1 pin OK: %s", hex);

    Elf32Ehdr eh;
    image_read(elf, 0, &eh, sizeof(eh), "ELF header", elf_src);
    if (std::memcmp(eh.e_ident, "\x7f""ELF", 4) != 0) {
        rt_fatal("loader", nullptr, "'%s' is not an ELF file (bad magic)", elf_src);
    }
    if (eh.e_ident[4] != 1 /* ELFCLASS32 */ || eh.e_ident[5] != 1 /* ELFDATA2LSB */) {
        rt_fatal("loader", nullptr, "'%s' is not a 32-bit little-endian ELF (class=%u data=%u)", elf_src, eh.e_ident[4], eh.e_ident[5]);
    }
    if (eh.e_type != kEtExec) {
        rt_fatal("loader", nullptr, "'%s' e_type=%u, expected ET_EXEC (2)", elf_src, eh.e_type);
    }
    if (eh.e_phnum == 0) {
        rt_fatal("loader", nullptr, "'%s' has no program headers", elf_src);
    }

    rt_log("loader", "ELF header: entry=0x%08x phoff=%u phentsize=%u phnum=%u",
        eh.e_entry, eh.e_phoff, eh.e_phentsize, eh.e_phnum);
    if (eh.e_entry != cfg.entry) {
        rt_log("loader", "note: ELF e_entry 0x%08x differs from config [target].entry 0x%08x; the config value drives the boot call",
            eh.e_entry, cfg.entry);
    }

    uint32_t load_count = 0;
    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        Elf32Phdr ph;
        image_read(elf, size_t(eh.e_phoff) + size_t(i) * eh.e_phentsize, &ph, sizeof(ph),
            "program header", elf_src);
        if (ph.p_type != kPtLoad) continue;

        rt_log("loader", "PT_LOAD[%u]: vaddr=0x%08x offset=0x%x filesz=0x%x memsz=0x%x flags=0x%x",
            load_count, ph.p_vaddr, ph.p_offset, ph.p_filesz, ph.p_memsz, ph.p_flags);

        if (ph.p_filesz > 0) {
            if (size_t(ph.p_offset) + ph.p_filesz > elf.size()) {
                rt_fatal("loader", nullptr, "segment %u data runs past the end of '%s'", load_count, elf_src);
            }
            write_guest_bytes(ph.p_vaddr, elf.data() + ph.p_offset, ph.p_filesz);
        }
        if (ph.p_memsz > ph.p_filesz) {
            uint32_t bss_len = ph.p_memsz - ph.p_filesz;
            zero_guest_bytes(ph.p_vaddr + ph.p_filesz, bss_len);
            rt_log("loader", "zeroed bss: 0x%08x-0x%08x (%u bytes)",
                ph.p_vaddr + ph.p_filesz, ph.p_vaddr + ph.p_memsz, bss_len);
        }
        ++load_count;
    }

    if (load_count == 0) rt_fatal("loader", nullptr, "no PT_LOAD segment found in '%s'", elf_src);
    if (load_count > 1) {
        rt_log("loader", "note: %u PT_LOAD segments found; CLAUDE.md/plan assumed exactly one (ICO's boot ELF has one)", load_count);
    }
    rt_log("loader", "load complete: %u PT_LOAD segment(s)", load_count);
}
