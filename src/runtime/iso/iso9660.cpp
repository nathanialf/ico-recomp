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
 * itself comes from the command line, the user's own settings.json, an
 * untracked local config or the read-only decomp checkout (CLAUDE.md). The
 * order those are consulted in is documented on rt_iso_mount in iso9660.h,
 * and implemented once, in probe_and_mount below.
 */
#include "iso9660.h"

#include "../target.h"

#include "../prof.h"

#include <vector>

#include "../host/portable.h"
#include "../runtime.h"

/* Only targets that link host/settings.cpp may read a setting from here.
 * icorecomp-ipu-selftest links this file but not settings.cpp, so it does
 * not define ICORECOMP_HAVE_SETTINGS and loses the launcher.disc_path step
 * of the resolution order (see iso9660.h). Same gate as prof.h. */
#ifdef ICORECOMP_HAVE_SETTINGS
#include "../host/settings.h"
#endif

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

std::FILE* g_disc = nullptr;
std::string g_forced_path; /* rt_iso_set_path (--disc) */
std::string g_mounted_path;
std::string g_mounted_source;
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

/* Longest single disc read in the profile window, in milliseconds, for the
 * summary's "fields:" line (prof.h). A read that blocks on a cold file or a
 * network share is one of the few things that can stall a field outright,
 * and the "disc" bucket's average hides it.
 *
 * No clock reading of its own: the disc zone already stamps the clock at
 * its edges, so the timer reads the zone's own exclusive nanosecond
 * counter either side of it. Two things have to hold for that difference
 * to be the read and nothing else. Declaration order: the timer is
 * constructed before the zone and therefore destroyed after it, which is
 * the only order in which the zone has already billed the read by the time
 * the difference is taken. And no enclosing RT_PROF_DISC zone: the opening
 * value is read before RT_PROF_ZONE runs, so an outer disc zone would have
 * its own accumulated time billed into the counter by that entry and land
 * in this read's total. Neither function below is called from inside the
 * other, and nothing else opens a disc zone. */
double g_disc_max_ms = 0.0;

struct DiscReadTimer {
    bool on = g_rt_prof_on;
    uint64_t start = 0;
    DiscReadTimer() { if (on) start = rt_prof_zone_ns(RT_PROF_DISC); }
    ~DiscReadTimer() {
        if (!on) return;
        const double ms = (double)(rt_prof_zone_ns(RT_PROF_DISC) - start) / 1e6;
        if (ms > g_disc_max_ms) g_disc_max_ms = ms;
    }
    DiscReadTimer(const DiscReadTimer&) = delete;
    DiscReadTimer& operator=(const DiscReadTimer&) = delete;
};

bool read_raw_sector(uint32_t lsn, uint8_t out[2048]) {
    DiscReadTimer disc_timer;
    RT_PROF_ZONE(RT_PROF_DISC);
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
            rt_log_info("iso", "sector layout probed: %s (PVD found at LBA 16), %u sectors",
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

/* Why a mount attempt did not produce a usable disc. The two "the file is a
 * disc image but not the right one" outcomes are separated from the two
 * "there is no image here" outcomes because rt_iso_mount is fatal on the
 * former and moves on to the next candidate on the latter. */
enum class MountFail {
    None,    /* mounted */
    Open,    /* not openable/seekable: candidate absent */
    Layout,  /* no ISO9660 PVD at any known sector layout */
    Pvd,     /* PVD sector unreadable after a successful layout probe */
    NoBoot,  /* mounted, but the boot ELF is not on it */
};

void unmount() {
    if (g_disc) {
        std::fclose(g_disc);
        g_disc = nullptr;
    }
    g_sector_size = 0;
    g_data_offset = 0;
    g_total_sectors = 0;
    g_root_lba = 0;
    g_root_size = 0;
    g_mounted_path.clear();
    g_mounted_source.clear();
}

/* The one mount implementation: opens `path`, probes the sector layout,
 * reads the PVD and verifies the disc by locating the boot ELF (and warning
 * about a missing DFDATAS/DATA.DF). Unmounts whatever was mounted before,
 * first thing, and leaves nothing mounted on any failure. Never fatal:
 * every caller decides for itself whether a failure is loud. `err` gets one
 * human-readable line on failure. */
MountFail mount_path(const std::string& path, const char* how, std::string* err) {
    unmount();

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        *err = "cannot open '" + path + "': " + std::strerror(errno);
        return MountFail::Open;
    }
    if (rt_fseek64(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        *err = "cannot seek to the end of '" + path + "'";
        return MountFail::Open;
    }
    long long size = rt_ftell64(f);
    g_disc = f;
    if (!probe_layout(size)) {
        rt_log_warn("iso", "'%s' (%s): no ISO9660 PVD found at any known sector layout; skipping",
            path.c_str(), how);
        unmount();
        *err = "'" + path + "' has no ISO9660 volume descriptor at any known sector layout "
               "(plain 2048, raw 2352); not a disc image?";
        return MountFail::Layout;
    }
    rt_log_info("iso", "mounted '%s' (%s), %lld bytes", path.c_str(), how, size);

    /* PVD: root directory record at offset 156; volume id at 40. */
    uint8_t pvd[2048];
    if (!read_raw_sector(16, pvd)) {
        unmount();
        *err = "'" + path + "': failed to read the ISO9660 primary volume descriptor at LBA 16";
        return MountFail::Pvd;
    }
    g_root_lba = le32(&pvd[156 + 2]);
    g_root_size = le32(&pvd[156 + 10]);
    {
        char volid[33];
        std::memcpy(volid, &pvd[40], 32);
        volid[32] = 0;
        for (int i = 31; i >= 0 && volid[i] == ' '; --i) volid[i] = 0;
        rt_log_info("iso", "PVD: volume id '%s', root dir LBA=%u size=%u", volid, g_root_lba, g_root_size);
    }

    /* Mount verification: the boot ELF and the game's data archive must
     * resolve (paths are public facts from the disc's own filesystem). */
    RtIsoFile file;
    if (!rt_iso_search(RT_TARGET_BOOT_ELF_ISO_PATH, &file)) {
        /* One build serves one disc: the generated code is a translation of
         * one ELF, so an image without that ELF cannot be run here whatever
         * else is on it. */
        unmount();
        *err = "'" + path + "' has no " + RT_TARGET_BOOT_ELF + "; it is not an ICO "
             + RT_TARGET_REGION + " disc image";
        return MountFail::NoBoot;
    }
    rt_log_info("iso", "verify: %s at LBA %u, %u bytes", RT_TARGET_BOOT_ELF, file.lsn, file.size);
    if (rt_iso_search("\\DFDATAS\\DATA.DF;1", &file)) {
        rt_log_info("iso", "verify: DFDATAS/DATA.DF at LBA %u, %u bytes", file.lsn, file.size);
    } else {
        rt_log_warn("iso", "WARNING: DFDATAS/DATA.DF not found on the mounted image");
    }

    g_mounted_path = path;
    g_mounted_source = how;
    return MountFail::None;
}

/* The fatal text rt_iso_mount has always used for a candidate that opened
 * and probed as a disc image but is not usable. Kept separate from the
 * richer message mount_path builds so the loud path stays byte-identical
 * for anyone grepping the log. */
const char* candidate_fatal_text(MountFail f) {
    return f == MountFail::Pvd ? "failed to read the PVD sector"
                               : "mounted image has no " RT_TARGET_BOOT_ELF "; wrong disc?";
}

/* Walks the resolution order documented in iso9660.h. `fatal` selects
 * rt_iso_mount's historical behavior (rt_fatal with the same text it has
 * always used); otherwise the same condition returns false with the message
 * in *err. Returns true with a disc mounted. */
bool probe_and_mount(bool fatal, std::string* err) {
    if (g_disc) return true;

    std::string tried;
    std::string e;
    int outcome = 0; /* 0 keep probing, 1 mounted, -1 gave up with *err set */

    auto give_up = [&](const char* fatal_text, const std::string& err_text) {
        if (fatal) rt_fatal("iso", nullptr, "%s", fatal_text);
        *err = err_text;
        outcome = -1;
    };

    /* One candidate. `soft` candidates never stop the search: a saved
     * setting that has gone stale must not be able to brick a run, so its
     * failure is logged with its source label and the search continues. */
    auto attempt = [&](const std::string& path, const char* how, bool soft) {
        if (outcome != 0) return;
        MountFail f = mount_path(path, how, &e);
        if (f == MountFail::None) {
            outcome = 1;
            return;
        }
        if (soft) {
            rt_log_warn("iso", "%s: %s; continuing the disc search", how, e.c_str());
        } else if (f == MountFail::Pvd || f == MountFail::NoBoot) {
            give_up(candidate_fatal_text(f), e);
            return;
        }
        tried += path + " ";
    };

    if (!g_forced_path.empty()) {
        MountFail f = mount_path(g_forced_path, "--disc", &e);
        if (f == MountFail::None) return true;
        if (f == MountFail::Pvd || f == MountFail::NoBoot) {
            give_up(candidate_fatal_text(f), e);
        } else {
            std::string msg = "--disc " + g_forced_path +
                ": not readable or no ISO9660 filesystem found";
            give_up(msg.c_str(), msg);
        }
        return false;
    }

#ifdef ICORECOMP_HAVE_SETTINGS
    /* rt_settings_init() runs in main before anything reaches rt_iso_mount
     * (main.cpp: settings init, then rt_mem_init, then the loader), so
     * rt_settings() is valid here. The launcher writes this key when the
     * user picks a disc. */
    if (outcome == 0) {
        const std::string& saved = rt_settings().launcher.disc_path;
        if (!saved.empty()) {
            std::string p = path_is_absolute(saved) ? saved
                : std::string(rt_base_dir()) + "/" + saved;
            attempt(p, "settings.json launcher.disc_path", true);
        }
    }
#endif

    if (outcome == 0) {
        std::string local = std::string(rt_base_dir()) + "/config/local.toml";
        std::string cfg_path = toml_lookup(local.c_str(), "disc", "path");
        if (!cfg_path.empty()) {
            std::string p = path_is_absolute(cfg_path) ? cfg_path
                : std::string(rt_base_dir()) + "/" + cfg_path;
            attempt(p, "config/local.toml [disc].path", false);
        }
    }

    if (outcome == 0) {
        /* Dev checkout only: the sibling decomp tree's baserom. A packaged
         * run has no decomp_root, and probing one there would put a
         * meaningless path in the "tried:" list below. */
        LoaderConfig cfg;
        if (rt_load_config(&cfg) && cfg.decomp_root[0]) {
            std::string base = std::string(rt_base_dir()) + "/" + cfg.decomp_root + "/baserom/Ico_PAL";
            attempt(base + ".bin", "decomp baserom bin/cue", false);
            attempt(base + ".iso", "decomp baserom iso", false);
        }
    }

    if (outcome == 0) {
        /* Packaged convention: the disc sits in the same folder as the
         * executable. rt_base_dir() is that folder for a packaged run, so
         * this holds however the exe was launched. */
        static const char* const kLocal[] = { "ico.iso", "ico.bin", "Ico_PAL.iso", "Ico_PAL.bin" };
        for (const char* name : kLocal) {
            attempt(std::string(rt_base_dir()) + "/" + name, "next to the executable", false);
        }
    }

    if (outcome == 1) return true;
    if (outcome == -1) return false;

    std::string msg = "no disc image found (tried: " + tried +
        "). Pass --disc <path>, set [disc] path in config/local.toml, "
        "or put the image next to the exe as ico.iso.";
    give_up(msg.c_str(), msg);
    return false;
}

} // namespace

/* See prof.h: reading clears, so each window reports its own worst read. */
extern "C" double rt_disc_prof_max_ms(void) {
    const double ms = g_disc_max_ms;
    g_disc_max_ms = 0.0;
    return ms;
}

bool rt_iso_mounted() { return g_disc != nullptr; }
uint32_t rt_iso_sector_size() { return g_sector_size; }
uint32_t rt_iso_total_sectors() { return g_total_sectors; }

bool rt_iso_read_sector(uint32_t lsn, uint8_t out[2048]) {
    return read_raw_sector(lsn, out);
}

uint32_t rt_iso_read_sectors(uint32_t lsn, uint32_t count, uint8_t* out) {
    DiscReadTimer disc_timer;
    RT_PROF_ZONE(RT_PROF_DISC);
    if (!g_disc || count == 0) return 0;
    if (lsn >= g_total_sectors) return 0;
    if (count > g_total_sectors - lsn) count = g_total_sectors - lsn;

    /* A plain 2048 image stores the user data contiguously, so a run of
     * sectors is one seek and one read. The retail streaming path asks for
     * ~1400 sectors at a time; doing that per sector costs 1400 seek/read
     * pairs, which is ruinous when the image sits on a network share.
     * Raw 2352 layouts interleave headers between sectors and keep the
     * per-sector path. */
    if (g_sector_size == 2048 && g_data_offset == 0) {
        long long pos = (long long)lsn * 2048;
        if (rt_fseek64(g_disc, pos, SEEK_SET) != 0) return 0;
        size_t got = std::fread(out, 2048, count, g_disc);
        return (uint32_t)got;
    }

    /* Raw layouts (2352 mode 1 / mode 2 form 1) wrap each 2048-byte user
     * area in sync, header and ECC, so the user data is strided rather than
     * contiguous. That is still one read: pull the whole span and unpack it,
     * instead of a seek and a read per sector. The retail streaming path
     * asks for ~1400 sectors at a time, and doing that one sector at a time
     * is ruinous when the image is on a network share. */
    static std::vector<uint8_t> raw;
    raw.resize((size_t)count * g_sector_size);
    if (rt_fseek64(g_disc, (long long)lsn * g_sector_size, SEEK_SET) != 0) return 0;
    size_t got = std::fread(raw.data(), g_sector_size, count, g_disc);
    for (size_t i = 0; i < got; ++i) {
        std::memcpy(out + i * 2048,
            raw.data() + i * g_sector_size + g_data_offset, 2048);
    }
    return (uint32_t)got;
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

const char* rt_iso_forced_path() { return g_forced_path.c_str(); }

/* Thin wrapper over the shared probe: same candidate order, same source
 * labels, same log lines and same fatal messages as before this function was
 * factored out. */
void rt_iso_mount() {
    std::string err;
    (void)probe_and_mount(true, &err); /* never returns false: fatal inside */
}

bool rt_iso_probe_mount(char* err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    std::string e;
    if (probe_and_mount(false, &e)) return true;
    if (err && err_len) std::snprintf(err, err_len, "%s", e.c_str());
    return false;
}

bool rt_iso_try_mount(const char* path, char* err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (!path || !path[0]) {
        if (err && err_len) std::snprintf(err, err_len, "no disc image path given");
        unmount();
        return false;
    }
    std::string e;
    if (mount_path(path, "explicit path", &e) == MountFail::None) return true;
    if (err && err_len) std::snprintf(err, err_len, "%s", e.c_str());
    return false;
}

const char* rt_iso_mounted_path() { return g_mounted_path.c_str(); }
const char* rt_iso_mounted_source() { return g_mounted_source.c_str(); }
