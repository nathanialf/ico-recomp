/* iso/iso9660.cpp: disc image mounting and ISO9660 directory parsing.
 *
 * Formats: plain 2048-byte-sector images (.iso) and raw 2352-byte-sector
 * bin/cue rips. The layout is probed, not assumed: sector 16 must hold the
 * primary volume descriptor (type 1, "CD001") at one of the candidate
 * (sector size, user-data offset) layouts:
 *   (2048, 0)   plain image
 *   (2352, 16)  raw mode 1 (12 sync + 4 header)
 *   (2352, 24)  raw mode 2 form 1 (12 sync + 4 header + 8 subheader)
 * The .cue sheet, when present, is not consulted; the probe is authoritative
 * for a single-data-track disc.
 *
 * ISO9660 structure facts (ECMA-119, public standard): PVD at LBA 16; root
 * directory record at PVD offset 156; directory records are {len u8, ext_len
 * u8, extent u32-LE at +2, size u32-LE at +10, datetime 7 bytes at +18,
 * flags u8 at +25, name_len u8 at +32, name at +33}; records do not cross
 * sector boundaries (a zero len byte skips to the next sector).
 *
 * No game data or content hashes are committed by this file; the disc path
 * itself comes from an untracked local config or the read-only decomp
 * checkout (CLAUDE.md).
 */
#include "iso9660.h"

#include "../host/portable.h"
#include "../runtime.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

std::FILE* g_disc = nullptr;
std::string g_forced_path; /* rt_iso_set_path (--disc) */
uint32_t g_sector_size = 0;
uint32_t g_data_offset = 0;
uint32_t g_total_sectors = 0;
uint32_t g_root_lba = 0;
uint32_t g_root_size = 0;

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

/* Minimal line-based TOML lookup, same approach as loader.cpp: returns the
 * value of `key` inside `[section]`, or "" if absent. */
std::string toml_lookup(const char* path, const char* section, const char* key) {
    std::ifstream f(path);
    if (!f) return "";
    std::string cur, line;
    while (std::getline(f, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t.front() == '[') {
            size_t end = t.find(']');
            if (end != std::string::npos) cur = trim(t.substr(1, end - 1));
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        if (cur != section || trim(t.substr(0, eq)) != key) continue;
        std::string v = t.substr(eq + 1);
        size_t hash = v.find('#');
        if (hash != std::string::npos) v = v.substr(0, hash);
        v = trim(v);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        return v;
    }
    return "";
}

bool read_raw_sector(uint32_t lsn, uint8_t out[2048]) {
    if (!g_disc || lsn >= g_total_sectors) return false;
    long long pos = (long long)lsn * g_sector_size + g_data_offset;
    if (rt_fseek64(g_disc, pos, SEEK_SET) != 0) return false;
    return std::fread(out, 1, 2048, g_disc) == 2048;
}

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool probe_layout(long long file_size) {
    struct Layout { uint32_t ss, off; const char* what; };
    const Layout candidates[] = {
        {2048, 0, "plain 2048 (.iso)"},
        {2352, 16, "raw 2352 mode 1"},
        {2352, 24, "raw 2352 mode 2 form 1"},
    };
    uint8_t buf[8];
    for (const Layout& c : candidates) {
        long long pos = 16ll * c.ss + c.off;
        if (pos + 6 > file_size) continue;
        if (rt_fseek64(g_disc, pos, SEEK_SET) != 0) continue;
        if (std::fread(buf, 1, 6, g_disc) != 6) continue;
        if (buf[0] == 0x01 && std::memcmp(buf + 1, "CD001", 5) == 0) {
            g_sector_size = c.ss;
            g_data_offset = c.off;
            g_total_sectors = (uint32_t)(file_size / c.ss);
            rt_log("iso", "sector layout probed: %s (PVD found at LBA 16), %u sectors",
                c.what, g_total_sectors);
            return true;
        }
    }
    return false;
}

/* Case-insensitive component compare, both sides stripped of a ";version"
 * suffix. */
bool name_matches(const char* rec_name, uint32_t rec_len, const char* want) {
    uint32_t rl = rec_len;
    for (uint32_t i = 0; i < rec_len; ++i) {
        if (rec_name[i] == ';') { rl = i; break; }
    }
    uint32_t wl = 0;
    while (want[wl] && want[wl] != ';') ++wl;
    if (rl != wl) return false;
    for (uint32_t i = 0; i < rl; ++i) {
        if (std::toupper((unsigned char)rec_name[i]) != std::toupper((unsigned char)want[i])) return false;
    }
    return true;
}

/* Scans one directory extent for `component`. Fills extent/size/flags/date
 * and the on-disc name. */
bool scan_dir(uint32_t dir_lba, uint32_t dir_size, const char* component,
              uint32_t* out_lba, uint32_t* out_size, uint8_t* out_flags,
              uint8_t out_date[7], char out_name[16]) {
    uint8_t sec[2048];
    uint32_t sectors = (dir_size + 2047) / 2048;
    for (uint32_t si = 0; si < sectors; ++si) {
        if (!read_raw_sector(dir_lba + si, sec)) return false;
        uint32_t off = 0;
        while (off + 33 <= 2048) {
            uint8_t len = sec[off];
            if (len == 0) break; /* rest of sector is padding */
            if (off + len > 2048) break;
            uint8_t name_len = sec[off + 32];
            const char* nm = (const char*)&sec[off + 33];
            if (name_len > 0 && !(name_len == 1 && (nm[0] == 0 || nm[0] == 1))) {
                if (name_matches(nm, name_len, component)) {
                    *out_lba = le32(&sec[off + 2]);
                    *out_size = le32(&sec[off + 10]);
                    *out_flags = sec[off + 25];
                    std::memcpy(out_date, &sec[off + 18], 7);
                    uint32_t cp = name_len < 15 ? name_len : 15;
                    for (uint32_t i = 0; i < cp; ++i) out_name[i] = nm[i];
                    out_name[cp] = 0;
                    /* Trim the ";version" from the stored name too. */
                    for (uint32_t i = 0; i < cp; ++i) {
                        if (out_name[i] == ';') { out_name[i] = 0; break; }
                    }
                    return true;
                }
            }
            off += len;
        }
    }
    return false;
}

/* Absolute on either OS: leading '/' or '\' (POSIX, UNC/rooted) or a drive
 * letter ("C:..."). */
bool path_is_absolute(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() >= 2 && p[1] == ':';
}

bool try_open(const std::string& path, const char* how) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    if (rt_fseek64(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    long long size = rt_ftell64(f);
    g_disc = f;
    if (!probe_layout(size)) {
        rt_log("iso", "'%s' (%s): no ISO9660 PVD found at any known sector layout; skipping",
            path.c_str(), how);
        std::fclose(f);
        g_disc = nullptr;
        return false;
    }
    rt_log("iso", "mounted '%s' (%s), %lld bytes", path.c_str(), how, size);
    return true;
}

} // namespace

bool rt_iso_mounted() { return g_disc != nullptr; }
uint32_t rt_iso_sector_size() { return g_sector_size; }
uint32_t rt_iso_total_sectors() { return g_total_sectors; }

bool rt_iso_read_sector(uint32_t lsn, uint8_t out[2048]) {
    return read_raw_sector(lsn, out);
}

bool rt_iso_search(const char* path, RtIsoFile* out) {
    if (!g_disc) return false;
    uint32_t lba = g_root_lba, size = g_root_size;
    uint8_t flags = 2; /* directory */
    uint8_t date[7] = {0};
    char name[16] = {0};

    const char* p = path;
    /* Skip a device prefix like "cdrom0:" if present. */
    const char* colon = std::strchr(p, ':');
    if (colon) p = colon + 1;

    char component[64];
    bool found_any = false;
    while (*p) {
        while (*p == '/' || *p == '\\') ++p;
        if (!*p) break;
        uint32_t n = 0;
        while (*p && *p != '/' && *p != '\\' && n < sizeof(component) - 1) component[n++] = *p++;
        component[n] = 0;
        if (n == 0) break;
        if (!(flags & 2)) return false; /* path continues past a file */
        if (!scan_dir(lba, size, component, &lba, &size, &flags, date, name)) return false;
        found_any = true;
    }
    if (!found_any) return false;
    out->lsn = lba;
    out->size = size;
    std::memcpy(out->name, name, sizeof(out->name));
    /* sceCdlFILE date bytes: [0]=iso flags, [1]=sec, [2]=min, [3]=hour,
     * [4]=day, [5]=month, [6..7]=year (libcdvd-common.h comment). ISO record
     * datetime is {years since 1900, month, day, hour, min, sec, tz}. */
    unsigned year = 1900u + date[0];
    out->date[0] = flags;
    out->date[1] = date[5];
    out->date[2] = date[4];
    out->date[3] = date[3];
    out->date[4] = date[2];
    out->date[5] = date[1];
    out->date[6] = (uint8_t)(year & 0xFF);
    out->date[7] = (uint8_t)(year >> 8);
    return true;
}

void rt_iso_set_path(const char* path) {
    g_forced_path = path ? path : "";
}

void rt_iso_mount() {
    if (g_disc) return;

    std::string tried;
    if (!g_forced_path.empty()) {
        if (try_open(g_forced_path, "--disc")) goto mounted;
        rt_fatal("iso", nullptr, "--disc %s: not readable or no ISO9660 filesystem found",
            g_forced_path.c_str());
    }
    {
        std::string local = std::string(rt_base_dir()) + "/config/local.toml";
        std::string cfg_path = toml_lookup(local.c_str(), "disc", "path");
        if (!cfg_path.empty()) {
            std::string p = path_is_absolute(cfg_path) ? cfg_path
                : std::string(rt_base_dir()) + "/" + cfg_path;
            if (try_open(p, "config/local.toml [disc].path")) goto mounted;
            tried += p + " ";
        }
    }
    {
        LoaderConfig cfg;
        std::string root = "../ico";
        if (rt_load_config(&cfg) && cfg.decomp_root[0]) root = cfg.decomp_root;
        std::string base = std::string(rt_base_dir()) + "/" + root + "/baserom/Ico_USA";
        if (try_open(base + ".bin", "decomp baserom bin/cue")) goto mounted;
        tried += base + ".bin ";
        if (try_open(base + ".iso", "decomp baserom iso")) goto mounted;
        tried += base + ".iso ";
    }
    {
        /* Packaged-zip convention: the disc sits next to the exe (which is
         * the working directory when launched by double click or from its
         * own folder). */
        static const char* const kLocal[] = { "ico.iso", "ico.bin", "Ico_USA.iso", "Ico_USA.bin" };
        for (const char* name : kLocal) {
            if (try_open(name, "working directory")) goto mounted;
            tried += std::string(name) + " ";
        }
    }
    rt_fatal("iso", nullptr, "no disc image found (tried: %s). Pass --disc <path>, set [disc] path "
        "in config/local.toml, or put the image next to the exe as ico.iso.",
        tried.c_str());

mounted:
    /* PVD: root directory record at offset 156; volume id at 40. */
    {
        uint8_t pvd[2048];
        if (!read_raw_sector(16, pvd)) rt_fatal("iso", nullptr, "failed to read the PVD sector");
        g_root_lba = le32(&pvd[156 + 2]);
        g_root_size = le32(&pvd[156 + 10]);
        char volid[33];
        std::memcpy(volid, &pvd[40], 32);
        volid[32] = 0;
        for (int i = 31; i >= 0 && volid[i] == ' '; --i) volid[i] = 0;
        rt_log("iso", "PVD: volume id '%s', root dir LBA=%u size=%u", volid, g_root_lba, g_root_size);
    }

    /* Mount verification: the boot ELF and the game's data archive must
     * resolve (paths are public facts from the disc's own filesystem). */
    RtIsoFile f;
    if (rt_iso_search("\\SCUS_971.13;1", &f)) {
        rt_log("iso", "verify: SCUS_971.13 at LBA %u, %u bytes", f.lsn, f.size);
    } else {
        rt_fatal("iso", nullptr, "mounted image has no SCUS_971.13; wrong disc?");
    }
    if (rt_iso_search("\\DFDATAS\\DATA.DF;1", &f)) {
        rt_log("iso", "verify: DFDATAS/DATA.DF at LBA %u, %u bytes", f.lsn, f.size);
    } else {
        rt_log("iso", "WARNING: DFDATAS/DATA.DF not found on the mounted image");
    }
}
