/* loader.cpp: config reader, ELF SHA-1 pin check, and PT_LOAD
 * segment loader.
 *
 * TOML parsing is a hand-rolled, minimal key lookup (line-based, one section
 * tracked at a time): we only need a few scalar keys out of
 * config/recomp.toml (see target.h): [decomp].root, [decomp].elf,
 * [pins].elf_sha1 and [target].entry/vram_base/gp. Anything else in the file (including the
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

#include "target.h"

#include "host/portable.h"
#include "iso/iso9660.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifdef ICORECOMP_HAVE_GENERATED
/* Same declaration main.cpp carries; see check_entry_fn below for why the
 * precheck may have to call it. */
extern "C" void g_functab_init(void);
#endif

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

/* Packaged-run defaults, mirroring the committed config/recomp.toml. The
 * pin is the approved boot ELF SHA-1 pin and the [target] values are
 * committed config facts, not ROM data; ../target.h says where each one was
 * measured. In this mode there is no decomp checkout: the boot ELF is read
 * out of the user's disc image (byte-identical to the pinned ELF) by
 * rt_load_elf. */
constexpr char kPinElfSha1[] = RT_TARGET_ELF_SHA1;
constexpr uint32_t kTargetEntry = RT_TARGET_ENTRY;
constexpr uint32_t kTargetVramBase = RT_TARGET_VRAM_BASE;
constexpr uint32_t kTargetGp = RT_TARGET_GP;

} // namespace

#ifndef ICORECOMP_SOURCE_ROOT
#error "ICORECOMP_SOURCE_ROOT must be defined by the build (see CMakeLists.txt)"
#endif

const char* rt_base_dir() {
    static const std::string base = [] {
        std::string root = ICORECOMP_SOURCE_ROOT;
        std::ifstream probe(std::string(root) + "/config/" + RT_TARGET_CONFIG_TOML);
        if (probe) return root;
        /* Packaged runtime: the build machine's source tree is not here.
         * Resolve everything against the executable's own directory, not
         * the working directory, so the package stays self-contained no
         * matter what launched it (a shortcut with its own "Start in"
         * folder, a shell somewhere else, a file manager). Everything a
         * packaged run touches goes through here: the log, config/,
         * saves/, and the disc probe. */
        return rt_exe_dir();
    }();
    return base.c_str();
}

namespace {

/* Parses this target's config file (or fills in the compiled-in pins when
 * there is none). Reached only through rt_load_config, which memoizes
 * it. */
bool load_config_uncached(LoaderConfig* out) {
    std::string path = std::string(rt_base_dir()) + "/config/" + RT_TARGET_CONFIG_TOML;
    std::ifstream f(path);
    if (!f) {
        std::snprintf(out->elf_sha1, sizeof(out->elf_sha1), "%s", kPinElfSha1);
        out->entry = kTargetEntry;
        out->vram_base = kTargetVramBase;
        out->gp = kTargetGp;
        rt_log_info("loader", "no config/%s (packaged run): using the compiled-in pin and "
            "target words (elf_sha1=%s entry=0x%08x gp=0x%08x); boot ELF will come from the "
            "disc image",
            RT_TARGET_CONFIG_TOML, out->elf_sha1, out->entry, out->gp);
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

    /* The pin and the entry are what this loader cannot do without. The
     * [decomp] pair is optional: a config that names no checkout leaves the
     * boot ELF to the user's disc image, which is the same path a packaged
     * run takes. Both halves still go through the same SHA-1 pin check. */
    if (!out->elf_sha1[0] || !out->entry) {
        rt_log_warn("loader", "config/%s missing required keys (elf_sha1='%s' entry=0x%x)",
            RT_TARGET_CONFIG_TOML, out->elf_sha1, out->entry);
        return false;
    }
    const bool have_checkout = out->decomp_root[0] && out->decomp_elf[0];
    if (!have_checkout) {
        /* Half a pair is a typo, not a choice, so it is named. */
        if (out->decomp_root[0] || out->decomp_elf[0]) {
            rt_log_warn("loader", "config/%s names decomp.root='%s' and decomp.elf='%s';"
                " one without the other is not usable, so the boot ELF comes from the disc image",
                RT_TARGET_CONFIG_TOML, out->decomp_root, out->decomp_elf);
        }
        out->decomp_root[0] = 0;
        out->decomp_elf[0] = 0;
    }
    rt_log_info("loader", "config: decomp.root=%s decomp.elf=%s pins.elf_sha1=%s target.entry=0x%08x target.vram_base=0x%08x target.gp=0x%08x",
        have_checkout ? out->decomp_root : "(none, the boot ELF comes from the disc image)",
        have_checkout ? out->decomp_elf : "(none)",
        out->elf_sha1, out->entry, out->vram_base, out->gp);
    return true;
}

} // namespace

bool rt_load_config(LoaderConfig* out) {
    /* Parsed and logged exactly once. rt_iso_mount calls this again to find
     * the decomp root, and sif/cdvd.cpp plus the ipu selftest reach it the
     * same way; without the cache every repeat re-read the file and printed
     * the notice again. */
    static LoaderConfig cached;
    static const bool ok = load_config_uncached(&cached);
    *out = cached;
    return ok;
}

void rt_resolve_elf_path(const LoaderConfig& cfg, char* buf, size_t buf_size) {
    std::snprintf(buf, buf_size, "%s/%s/%s", rt_base_dir(), cfg.decomp_root, cfg.decomp_elf);
}

namespace {

/* Every check below exists once, as a bool + message helper. rt_load_elf
 * calls the helpers and turns a false into rt_fatal with the same message
 * text it has always printed; rt_boot_precheck calls the same helpers and
 * hands the message to the launcher instead. */

/* Formats into a std::string, for message helpers whose text used to be a
 * printf-style rt_fatal argument list. */
RT_PRINTF_FORMAT(1, 2)
std::string fmt(const char* f, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

/* Reads a whole file. On failure *err says why; `opened` distinguishes "no
 * such file" (the caller's cue to fall back to the disc image) from a read
 * failure after a successful open, which is a hard error. */
bool read_whole_file(const char* path, std::vector<uint8_t>* out, bool* opened, std::string* err) {
    *opened = false;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        *err = fmt("cannot open '%s': %s", path, std::strerror(errno));
        return false;
    }
    *opened = true;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    if (size < 0) {
        std::fclose(f);
        *err = fmt("ftell('%s') failed", path);
        return false;
    }
    std::fseek(f, 0, SEEK_SET);
    out->resize(size_t(size));
    if (size > 0 && std::fread(out->data(), 1, size_t(size), f) != size_t(size)) {
        std::fclose(f);
        *err = fmt("short read on '%s'", path);
        return false;
    }
    std::fclose(f);
    return true;
}

/* Reads this build's boot ELF out of the already mounted disc image (the
 * mount verified the file is there). */
bool read_elf_from_disc(std::vector<uint8_t>* out, std::string* err) {
    RtIsoFile f;
    if (!rt_iso_search(RT_TARGET_BOOT_ELF_ISO_PATH, &f)) {
        *err = std::string("mounted disc has no ") + RT_TARGET_BOOT_ELF + " (wrong image?)";
        return false;
    }
    out->resize(f.size);
    uint8_t sec[2048];
    uint32_t remaining = f.size;
    for (uint32_t i = 0; remaining > 0; ++i) {
        if (!rt_iso_read_sector(f.lsn + i, sec)) {
            *err = fmt("disc read failed at LBA %u while extracting %s", f.lsn + i, RT_TARGET_BOOT_ELF);
            out->clear();
            return false;
        }
        uint32_t chunk = remaining < 2048 ? remaining : 2048;
        std::memcpy(out->data() + (size_t(i) * 2048), sec, chunk);
        remaining -= chunk;
    }
    rt_log_info("loader", "boot ELF read from disc: %s, LBA %u, %u bytes",
        RT_TARGET_BOOT_ELF, f.lsn, f.size);
    return true;
}

/* Source label rt_load_elf has always used for the disc-supplied ELF. Also
 * the cache key for that case (see g_elf_cache). */
constexpr char kDiscElfSrc[] = RT_TARGET_BOOT_ELF " (from the mounted disc image)";

/* The ELF image rt_boot_precheck already read and pin-checked, kept so the
 * rt_load_elf that follows a successful precheck does not read the file (or
 * ~1 MB off the disc) a second time. Only used when the key rt_load_elf
 * resolves to is the one the precheck cached. */
struct ElfImageCache {
    bool valid = false;
    std::string key;
    std::vector<uint8_t> bytes;
};
ElfImageCache g_elf_cache;

/* Cache key for one boot ELF image.
 *
 * The decomp-checkout source string is the resolved path, so it names the
 * image on its own. The disc source string is the constant kDiscElfSrc,
 * identical for every image, so the mounted image path is appended: the
 * launcher can mount a different image between the precheck and
 * rt_load_elf, and keying on the label alone would hand back the previous
 * image's bytes together with the pin result that was measured against
 * them. An unmounted disc produces a key no mount can match, which is the
 * wanted answer too: there are no bytes to reuse. */
std::string elf_cache_key(const char* src) {
    if (std::strcmp(src, kDiscElfSrc) == 0) {
        return std::string(src) + "|" + rt_iso_mounted_path();
    }
    return std::string(src);
}

/* Produces the boot ELF bytes and the source string used in messages: the
 * decomp checkout's ELF when the config names one and it is readable,
 * otherwise this build's boot ELF off the disc image. mount_if_needed selects what
 * happens when the disc is needed and nothing is mounted yet:
 *   true  -> rt_iso_mount(), which is fatal when no image is found. This is
 *            rt_load_elf's historical behavior, and it mounts only on the
 *            fallback path, never when the decomp ELF was used.
 *   false -> fails with a message. rt_boot_precheck mounts the disc itself,
 *            non-fatally, before calling this. */
bool acquire_elf(const LoaderConfig& cfg, bool mount_if_needed, std::vector<uint8_t>* out,
                 char* src, size_t src_len, std::string* err) {
    out->clear();

    if (cfg.decomp_root[0] && cfg.decomp_elf[0]) {
        rt_resolve_elf_path(cfg, src, src_len);
        rt_log_info("loader", "ELF path: %s", src);
        if (g_elf_cache.valid && g_elf_cache.key == elf_cache_key(src)) {
            *out = g_elf_cache.bytes;
            rt_log_info("loader", "reusing the %zu-byte ELF image the boot precheck already read", out->size());
            return true;
        }
        bool opened = false;
        std::string e;
        if (read_whole_file(src, out, &opened, &e)) {
            /* A zero-byte file falls through to the disc, matching the
             * `if (elf.empty())` fallback this replaced. */
            if (!out->empty()) return true;
        } else if (opened) {
            *err = e;
            return false;
        } else {
            rt_log_warn("loader", "'%s' not readable; falling back to the disc image's %s",
                src, RT_TARGET_BOOT_ELF);
        }
        out->clear();
    }

    std::snprintf(src, src_len, "%s", kDiscElfSrc);
    if (g_elf_cache.valid && g_elf_cache.key == elf_cache_key(src)) {
        *out = g_elf_cache.bytes;
        rt_log_info("loader", "reusing the %zu-byte ELF image the boot precheck already read", out->size());
        return true;
    }
    if (!rt_iso_mounted()) {
        if (!mount_if_needed) {
            *err = std::string("no disc image is mounted, so ") + RT_TARGET_BOOT_ELF
                 + " cannot be read";
            return false;
        }
        rt_iso_mount(); /* fatal when no image is found, as before */
    }
    return read_elf_from_disc(out, err);
}

/* SHA-1 pin check. Logs the OK line; the message on failure is the text
 * rt_load_elf has always fataled with. */
bool check_elf_pin(const std::vector<uint8_t>& elf, const LoaderConfig& cfg, const char* src,
                   std::string* err) {
    Sha1Digest digest = rt_sha1_buffer(elf.data(), elf.size());
    char hex[41];
    rt_sha1_to_hex(digest, hex);
    if (!rt_sha1_equals_hex(digest, cfg.elf_sha1)) {
        *err = fmt("SHA-1 mismatch for '%s': got %s, expected %s ([pins].elf_sha1)",
            src, hex, cfg.elf_sha1);
        return false;
    }
    rt_log_info("loader", "SHA-1 pin OK: %s", hex);
    return true;
}

/* Bounds-checked read out of the in-memory ELF image. */
bool image_read_checked(const std::vector<uint8_t>& img, size_t off, void* dst, size_t len,
                        const char* what, const char* src, std::string* err) {
    if (off + len > img.size() || off + len < off) {
        *err = fmt("short read on %s of '%s' (offset %zu + %zu > %zu bytes)",
            what, src, off, len, img.size());
        return false;
    }
    std::memcpy(dst, img.data() + off, len);
    return true;
}

void image_read(const std::vector<uint8_t>& img, size_t off, void* dst, size_t len,
                const char* what, const char* src) {
    std::string err;
    if (!image_read_checked(img, off, dst, len, what, src, &err)) {
        rt_fatal("loader", nullptr, "%s", err.c_str());
    }
}

/* ELF header sanity: readable header, magic, 32-bit little-endian, ET_EXEC,
 * at least one program header. */
bool validate_elf_header(const std::vector<uint8_t>& elf, const char* src, Elf32Ehdr* out,
                         std::string* err) {
    if (!image_read_checked(elf, 0, out, sizeof(*out), "ELF header", src, err)) return false;
    if (std::memcmp(out->e_ident, "\x7f""ELF", 4) != 0) {
        *err = fmt("'%s' is not an ELF file (bad magic)", src);
        return false;
    }
    if (out->e_ident[4] != 1 /* ELFCLASS32 */ || out->e_ident[5] != 1 /* ELFDATA2LSB */) {
        *err = fmt("'%s' is not a 32-bit little-endian ELF (class=%u data=%u)",
            src, unsigned(out->e_ident[4]), unsigned(out->e_ident[5]));
        return false;
    }
    if (out->e_type != kEtExec) {
        *err = fmt("'%s' e_type=%u, expected ET_EXEC (2)", src, unsigned(out->e_type));
        return false;
    }
    if (out->e_phnum == 0) {
        *err = fmt("'%s' has no program headers", src);
        return false;
    }
    return true;
}

} // namespace

/* The function table (g_functab, mem.cpp) and the boot precheck that reads
 * it are compiled only into the full runtime executable.
 * icorecomp-ipu-selftest links loader.cpp for rt_base_dir/rt_load_config
 * and links neither mem.cpp nor host/settings.cpp, so a reference to the
 * table from here breaks its link. ICORECOMP_HAVE_SETTINGS is the existing
 * "this target is the full runtime" gate (CMakeLists.txt, prof.h).
 * rt_boot_precheck stays declared unconditionally in runtime.h: a call from
 * a target that does not link the table fails to link naming
 * rt_boot_precheck, which is the right diagnosis to get. */
#ifdef ICORECOMP_HAVE_SETTINGS

namespace {

/* The entry lookup main.cpp does after g_functab_init(). The two messages
 * are the ones main.cpp logs, without its "FATAL: " log prefix: they are
 * what users grep for, so they stay byte-identical on both sides. */
bool check_entry_fn(const LoaderConfig& cfg, std::string* err) {
    if (cfg.entry < RECOMP_TEXT_BASE || cfg.entry >= RECOMP_TEXT_LIMIT) {
        *err = fmt("config [target].entry 0x%08x is outside the function table range [0x%08x, 0x%08x)",
            cfg.entry, RECOMP_TEXT_BASE, RECOMP_TEXT_LIMIT);
        return false;
    }
#ifdef ICORECOMP_HAVE_GENERATED
    /* The precheck can run before main's g_functab_init(), and then every
     * slot is still null and this check would fail for any address. The
     * generated g_functab_init is a memset, a fixed list of assignments and
     * a memcpy, so running it early (and again in main) is idempotent, and
     * nothing installs a g_functab override before main's call. */
    if (!g_functab[RECOMP_FUNC_IDX(cfg.entry)]) g_functab_init();
#endif
    if (!g_functab[RECOMP_FUNC_IDX(cfg.entry)]) {
#ifdef ICORECOMP_HAVE_GENERATED
        *err = fmt("entry function at vram 0x%08x not found in g_functab after g_functab_init()"
                   " -- generated code does not cover this address", cfg.entry);
#else
        *err = fmt("no generated code linked (stub build); entry function at vram 0x%08x cannot be"
                   " resolved. Configure with generated/ee present (or -DGENERATED_DIR=...) to run the game.",
            cfg.entry);
#endif
        return false;
    }
    return true;
}

} // namespace

bool rt_boot_precheck(char* err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    auto fail = [&](const std::string& msg) {
        if (err && err_len) std::snprintf(err, err_len, "%s", msg.c_str());
        return false;
    };

    LoaderConfig cfg;
    if (!rt_load_config(&cfg))
        return fail(std::string("could not load config/") + RT_TARGET_CONFIG_TOML);

    char disc_err[1024];
    if (!rt_iso_probe_mount(disc_err, sizeof(disc_err))) return fail(disc_err);

    std::vector<uint8_t> elf;
    char src[1536];
    std::string e;
    if (!acquire_elf(cfg, /*mount_if_needed=*/false, &elf, src, sizeof(src), &e)) return fail(e);
    if (!check_elf_pin(elf, cfg, src, &e)) return fail(e);
    Elf32Ehdr eh;
    if (!validate_elf_header(elf, src, &eh, &e)) return fail(e);
    if (!check_entry_fn(cfg, &e)) return fail(e);

    /* Hand the bytes to the rt_load_elf that follows, so the disc read or
     * file read happens once per run. The disc stays mounted. */
    g_elf_cache.key = elf_cache_key(src);
    g_elf_cache.bytes = std::move(elf);
    g_elf_cache.valid = true;
    return true;
}

#endif /* ICORECOMP_HAVE_SETTINGS */

void rt_load_elf(const LoaderConfig& cfg) {
    std::vector<uint8_t> elf;
    char elf_src[1536]; /* comfortably covers the base dir + two 512-byte config fields + separators */
    std::string err;

    if (!acquire_elf(cfg, /*mount_if_needed=*/true, &elf, elf_src, sizeof(elf_src), &err)) {
        rt_fatal("loader", nullptr, "%s", err.c_str());
    }
    if (!check_elf_pin(elf, cfg, elf_src, &err)) {
        rt_fatal("loader", nullptr, "%s", err.c_str());
    }

    Elf32Ehdr eh;
    if (!validate_elf_header(elf, elf_src, &eh, &err)) {
        rt_fatal("loader", nullptr, "%s", err.c_str());
    }

    rt_log_info("loader", "ELF header: entry=0x%08x phoff=%u phentsize=%u phnum=%u",
        eh.e_entry, eh.e_phoff, eh.e_phentsize, eh.e_phnum);
    if (eh.e_entry != cfg.entry) {
        rt_log_warn("loader", "note: ELF e_entry 0x%08x differs from config [target].entry 0x%08x; the config value drives the boot call",
            eh.e_entry, cfg.entry);
    }

    uint32_t load_count = 0;
    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        Elf32Phdr ph;
        image_read(elf, size_t(eh.e_phoff) + size_t(i) * eh.e_phentsize, &ph, sizeof(ph),
            "program header", elf_src);
        if (ph.p_type != kPtLoad) continue;

        rt_log_info("loader", "PT_LOAD[%u]: vaddr=0x%08x offset=0x%x filesz=0x%x memsz=0x%x flags=0x%x",
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
            rt_log_info("loader", "zeroed bss: 0x%08x-0x%08x (%u bytes)",
                ph.p_vaddr + ph.p_filesz, ph.p_vaddr + ph.p_memsz, bss_len);
        }
        ++load_count;
    }

    if (load_count == 0) rt_fatal("loader", nullptr, "no PT_LOAD segment found in '%s'", elf_src);
    if (load_count > 1) {
        rt_log_warn("loader", "note: %u PT_LOAD segments found; CLAUDE.md/plan assumed exactly one (ICO's boot ELF has one)", load_count);
    }
    rt_log_info("loader", "load complete: %u PT_LOAD segment(s)", load_count);
    /* The one ABI constant that follows the target rather than the machine.
     * SCES_507.60's .text ends at 0x00289BC4 and its .vutext at 0x0028ECB0,
     * where SCUS_971.13's .vutext ended at 0x002746C0, so the window this
     * runtime was built with was raised from 0x00280000 to 0x00290000 for
     * the PAL switch. Named here because a mismatch is otherwise invisible:
     * generated/ee/funcs_table.c indexes g_functab_orig with no bound test,
     * so a window that is too small is an out-of-bounds write. The
     * translator emits a #error against the highest translated entry, which
     * catches it at build time; this line is how a run says which window
     * the binary in hand actually carries. */
    rt_log_info("loader", "function table window: [0x%08x, 0x%08x), %u slots",
        (unsigned)RECOMP_TEXT_BASE, (unsigned)RECOMP_TEXT_LIMIT, (unsigned)RECOMP_FUNCTAB_SLOTS);
}
