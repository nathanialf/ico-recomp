/* sif/mc.cpp: the mcserv RPC service (old MCSERV wire protocol) of the
 * virtual IOP, backed by a directory on the host disk.
 *
 * The retail ICO ELF links the old (SDK 2.2, "PsIIlibmc 2240") EE libmc;
 * the wire protocol below was read out of that library's disassembly in the
 * decomp checkout (asm/.../src/cod/vendor_24E9D8, functions identified by
 * their RPC fno and argument stores; sceMcInit is func_0024F240). Server id
 * 0x80000400; recv is always the 4-byte result word except INIT. Send
 * blocks (offsets within the RPC send data):
 *
 *   0xFE Init        0x30 bytes; recv 12: {result, mcserv_ver, mcman_ver}.
 *                    libmc requires mcserv_ver >= 0x20A and mcman_ver >=
 *                    0x20E or it prints "too old release of mcserv.irx".
 *   0x01 GetInfo     port@4 slot@8 wantFormat@0xC wantFree@0x10 wantType
 *                    @0x14, EE writeback addr@0x1C: server writes type@+0,
 *                    free-KB@+4, formatted@+0x90 there (0xC0 block).
 *   0x02 Open        port@0 slot@4 mode@8 name@0x14. mode: 1 rd, 2 wr,
 *                    0x200 create, 0x40 mkdir (sceMcMkdir = Open|0x40).
 *                    result = fd or negative error.
 *   0x03 Close       fd@0.
 *   0x04 Seek        fd@0 offset@0x10 whence@0x14; result = new position.
 *   0x05 Read        fd@0 size@0xC buf@0x18, edge writeback addr@0x1C:
 *                    {head_size, tail_size, head_dst, tail_dst, head data
 *                    @0x10, tail data@0x50}. This server DMAs everything
 *                    straight to buf and zeroes the edge sizes.
 *   0x06 Write       fd@0 tail_size@0xC head_size@0x14 tail_addr@0x18,
 *                    head bytes inline@0x20 (libmc splits at a 16-byte
 *                    boundary); result = bytes written.
 *   0x0A Flush       fd@0.
 *   0x0C ChDir       port@0 slot@4 pwd writeback addr@0x10 name@0x14; the
 *                    server writes the previous working directory string
 *                    (up to 0x400 bytes) to the writeback address.
 *   0x0D GetDir      port@0 slot@4 mode@8 maxent@0xC table@0x10 name@0x14.
 *                    mode 0 starts a new search (name may hold * and ?
 *                    wildcards in its final component), nonzero continues.
 *                    Entries are 0x40-byte sceMcTblGetDir records (public
 *                    SDK layout): create time@0, modify time@8, size@0x10,
 *                    attr@0x14, name@0x20. result = entries written.
 *   0x0E SetFileInfo port@0 slot@4 flags@8 info addr@0x10 name@0x14. flags:
 *                    1 = set create time, 2 = set modify time, 4 = set
 *                    attr, 0x10 = rename to info->EntryName (sceMcRename).
 *   0x0F Delete      port@0 slot@4 name@0x14.
 *   0x10 Format      port@4 slot@8.
 *   0x11 Unformat    port@4 slot@8.
 *   0x12 GetEntSpace port@0 slot@4 name@0x14; result = free dir entries.
 *
 * Timestamps in the wire records are 8-byte sceMcStDateTime: {pad, sec,
 * min, hour, day, month, u16 year}.
 *
 * Host backing: [saves] dir from config/local.toml (default saves/mc0,
 * resolved against the repo root, gitignored). Card directory entries map
 * to host directories/files 1:1. PS2 attributes and creation times that the
 * host filesystem cannot hold live in a JSON sidecar per entry
 * ("<name>.mcmeta.json", hidden from listings). Only port 0 has a card:
 * always present, formatted, 8 MB.
 */
#include "rpc.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace fs = std::filesystem;

namespace {

/* libmc result codes (public SDK facts). */
constexpr int kResOk = 0;
constexpr int kResChangedCard = -1;
constexpr int kResNoEntry = -4;
constexpr int kResDenied = -5;
constexpr int kResNotEmpty = -6;
constexpr int kResNoFreeFd = -7;

constexpr uint32_t kAttrDir = 0x8427;   /* MC_ATTR_norm_folder */
constexpr uint32_t kAttrFile = 0x8497;  /* MC_ATTR_norm_file */

constexpr uint32_t kTotalKb = 8135;     /* formatted 8 MB card, free when empty */
constexpr uint32_t kMaxFds = 16;
constexpr const char* kSidecarSuffix = ".mcmeta.json";

uint32_t rd32(const uint8_t* p, uint32_t off) { uint32_t v; std::memcpy(&v, p + off, 4); return v; }
void wr32(uint8_t* p, uint32_t off, uint32_t v) { if (p) std::memcpy(p + off, &v, 4); }

std::string g_base;                 /* host directory backing the card */
bool g_ready = false;

struct McTime { int y = 2003, mo = 1, d = 1, h = 0, mi = 0, s = 0; };

struct Fd {
    bool used = false;
    std::FILE* f = nullptr;
    std::string card_path;          /* for logs and close-time sidecar touch */
    bool wrote = false;
};
Fd g_fd[kMaxFds];

std::string g_cwd = "/";            /* ChDir state (port 0) */
bool g_first_getinfo = true;

struct DirEnt {
    std::string name;
    uint32_t attr = 0;
    uint32_t size = 0;
    McTime create, modify;
};
std::vector<DirEnt> g_listing;      /* GetDir continuation state */
size_t g_listing_pos = 0;

/* ---- config / paths ------------------------------------------------------- */

std::string toml_lookup_saves() {
    /* Same minimal line-based lookup as iso9660.cpp/loader.cpp. */
    std::string path = std::string(ICORECOMP_SOURCE_ROOT) + "/config/local.toml";
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return "";
    char line[512];
    bool in_saves = false;
    std::string result;
    while (std::fgets(line, sizeof(line), f)) {
        std::string t = line;
        size_t a = t.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        size_t b = t.find_last_not_of(" \t\r\n");
        t = t.substr(a, b - a + 1);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[') { in_saves = (t == "[saves]"); continue; }
        if (!in_saves) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = t.substr(0, eq);
        key.erase(key.find_last_not_of(" \t") + 1);
        if (key != "dir") continue;
        std::string v = t.substr(eq + 1);
        size_t hash = v.find('#');
        if (hash != std::string::npos) v = v.substr(0, hash);
        a = v.find_first_not_of(" \t");
        b = v.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        v = v.substr(a, b - a + 1);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
        result = v;
        break;
    }
    std::fclose(f);
    return result;
}

void mc_init_backing() {
    if (g_ready) return;
    std::string dir;
    const char* env = std::getenv("ICORECOMP_SAVES_DIR"); /* tests / overrides */
    if (env && env[0]) dir = env;
    if (dir.empty()) dir = toml_lookup_saves();
    if (dir.empty()) dir = "saves/mc0";
    if (dir[0] == '/') g_base = dir;
    else g_base = std::string(ICORECOMP_SOURCE_ROOT) + "/" + dir;
    std::error_code ec;
    fs::create_directories(g_base, ec);
    if (ec) {
        rt_fatal("mc", nullptr, "cannot create the saves directory '%s': %s",
            g_base.c_str(), ec.message().c_str());
    }
    g_ready = true;
    rt_log("mc", "virtual memory card (port 0): %s", g_base.c_str());
}

bool is_sidecar(const std::string& name) {
    size_t sl = std::strlen(kSidecarSuffix);
    return name.size() >= sl && name.compare(name.size() - sl, sl, kSidecarSuffix) == 0;
}

/* Guest card path -> normalized "/a/b" form. Empty return = invalid.
 * Handles ".", "..", leading "/" (absolute) vs cwd-relative. */
std::string resolve_card_path(const char* gpath) {
    std::vector<std::string> comps;
    std::string start = (gpath[0] == '/') ? "" : g_cwd;
    std::string full = start + "/" + gpath;
    size_t i = 0;
    while (i < full.size()) {
        while (i < full.size() && full[i] == '/') ++i;
        size_t j = i;
        while (j < full.size() && full[j] != '/') ++j;
        if (j > i) {
            std::string c = full.substr(i, j - i);
            if (c == ".") { /* stay */ }
            else if (c == "..") { if (!comps.empty()) comps.pop_back(); }
            else if (c.size() > 32 || is_sidecar(c)) return "";
            else comps.push_back(c);
        }
        i = j;
    }
    std::string out = "/";
    for (size_t k = 0; k < comps.size(); ++k) {
        if (k) out += "/";
        out += comps[k];
    }
    return out;
}

std::string host_path(const std::string& card_path) {
    return card_path == "/" ? g_base : g_base + card_path;
}

std::string sidecar_path(const std::string& card_path) {
    return host_path(card_path) + kSidecarSuffix;
}

/* ---- sidecar metadata ----------------------------------------------------- */

McTime time_from_host(const std::string& hpath) {
    struct ::stat st {};
    McTime t;
    if (::stat(hpath.c_str(), &st) == 0) {
        struct tm tmv {};
        localtime_r(&st.st_mtime, &tmv);
        t.y = tmv.tm_year + 1900; t.mo = tmv.tm_mon + 1; t.d = tmv.tm_mday;
        t.h = tmv.tm_hour; t.mi = tmv.tm_min; t.s = tmv.tm_sec;
    }
    return t;
}

McTime time_now() {
    std::time_t now = std::time(nullptr);
    struct tm tmv {};
    localtime_r(&now, &tmv);
    McTime t;
    t.y = tmv.tm_year + 1900; t.mo = tmv.tm_mon + 1; t.d = tmv.tm_mday;
    t.h = tmv.tm_hour; t.mi = tmv.tm_min; t.s = tmv.tm_sec;
    return t;
}

struct Meta {
    uint32_t attr = 0;
    McTime create, modify;
    bool have_attr = false, have_create = false, have_modify = false;
};

bool parse_time_array(const char* p, McTime* out) {
    int v[6];
    if (std::sscanf(p, "[%d,%d,%d,%d,%d,%d]", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    out->y = v[0]; out->mo = v[1]; out->d = v[2]; out->h = v[3]; out->mi = v[4]; out->s = v[5];
    return true;
}

Meta read_meta(const std::string& card_path) {
    Meta m;
    std::FILE* f = std::fopen(sidecar_path(card_path).c_str(), "r");
    if (!f) return m;
    char buf[512] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    const char* p;
    if ((p = std::strstr(buf, "\"attr\":"))) {
        m.attr = (uint32_t)std::strtoul(p + 7, nullptr, 10);
        m.have_attr = true;
    }
    if ((p = std::strstr(buf, "\"create\":")) && parse_time_array(p + 9, &m.create)) {
        m.have_create = true;
    }
    if ((p = std::strstr(buf, "\"modify\":")) && parse_time_array(p + 9, &m.modify)) {
        m.have_modify = true;
    }
    return m;
}

void write_meta(const std::string& card_path, const Meta& m) {
    std::FILE* f = std::fopen(sidecar_path(card_path).c_str(), "w");
    if (!f) {
        rt_log("mc", "WARNING: cannot write sidecar for %s (errno %d)", card_path.c_str(), errno);
        return;
    }
    std::fprintf(f, "{\"attr\":%u,\"create\":[%d,%d,%d,%d,%d,%d],\"modify\":[%d,%d,%d,%d,%d,%d]}\n",
        m.attr,
        m.create.y, m.create.mo, m.create.d, m.create.h, m.create.mi, m.create.s,
        m.modify.y, m.modify.mo, m.modify.d, m.modify.h, m.modify.mi, m.modify.s);
    std::fclose(f);
}

/* Effective entry info: sidecar values where present, host stat otherwise. */
DirEnt entry_info(const std::string& card_path, const std::string& name, bool is_dir) {
    DirEnt e;
    e.name = name;
    Meta m = read_meta(card_path);
    e.attr = m.have_attr ? m.attr : (is_dir ? kAttrDir : kAttrFile);
    McTime host = time_from_host(host_path(card_path));
    e.modify = m.have_modify ? m.modify : host;
    e.create = m.have_create ? m.create : e.modify;
    if (!is_dir) {
        std::error_code ec;
        uintmax_t sz = fs::file_size(host_path(card_path), ec);
        e.size = ec ? 0 : (uint32_t)sz;
    }
    return e;
}

void put_time(uint8_t* p, const McTime& t) {
    p[0] = 0;
    p[1] = (uint8_t)t.s; p[2] = (uint8_t)t.mi; p[3] = (uint8_t)t.h;
    p[4] = (uint8_t)t.d; p[5] = (uint8_t)t.mo;
    p[6] = (uint8_t)(t.y & 0xFF); p[7] = (uint8_t)(t.y >> 8);
}

McTime get_time(const uint8_t* p) {
    McTime t;
    t.s = p[1]; t.mi = p[2]; t.h = p[3]; t.d = p[4]; t.mo = p[5];
    t.y = p[6] | (p[7] << 8);
    return t;
}

/* ---- helpers -------------------------------------------------------------- */

bool wild_match(const char* pat, const char* s) {
    if (*pat == 0) return *s == 0;
    if (*pat == '*') return wild_match(pat + 1, s) || (*s && wild_match(pat, s + 1));
    if (*s == 0) return false;
    if (*pat == '?' || *pat == *s) return wild_match(pat + 1, s + 1);
    return false;
}

uint32_t used_kb_recursive(const std::string& hpath) {
    uint32_t kb = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(hpath, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (is_sidecar(it->path().filename().string())) continue;
        if (it->is_regular_file(ec)) {
            uintmax_t sz = it->file_size(ec);
            kb += (uint32_t)((sz + 1023) / 1024);
        } else {
            kb += 1; /* a directory costs a cluster */
        }
    }
    return kb;
}

const char* send_name(const uint8_t* send, uint32_t send_size) {
    /* Path at +0x14, NUL-terminated within the 0x414 block. */
    static char name[0x400];
    if (send_size < 0x15) { name[0] = 0; return name; }
    uint32_t n = send_size - 0x14;
    if (n > sizeof(name) - 1) n = sizeof(name) - 1;
    std::memcpy(name, send + 0x14, n);
    name[n] = 0;
    return name;
}

int alloc_fd() {
    for (uint32_t i = 0; i < kMaxFds; ++i) {
        if (!g_fd[i].used) return (int)i;
    }
    return -1;
}

/* ---- command implementations --------------------------------------------- */

int do_open(uint32_t port, uint32_t mode, const char* gname) {
    if (port != 0) return kResNoEntry;
    std::string cp = resolve_card_path(gname);
    if (cp.empty() || cp == "/") return kResNoEntry;
    std::string hp = host_path(cp);
    std::error_code ec;
    if (mode & 0x40) { /* mkdir */
        if (fs::exists(hp, ec)) return kResDenied;
        if (!fs::create_directory(hp, ec) || ec) {
            rt_log("mc", "mkdir %s failed: %s", cp.c_str(), ec.message().c_str());
            return kResNoEntry;
        }
        Meta m;
        m.attr = kAttrDir;
        m.create = m.modify = time_now();
        m.have_attr = m.have_create = m.have_modify = true;
        write_meta(cp, m);
        rt_log("mc", "Mkdir %s -> 0", cp.c_str());
        return kResOk;
    }
    bool exists = fs::exists(hp, ec);
    if (!exists && !(mode & 0x200)) return kResNoEntry;
    if (exists && fs::is_directory(hp, ec)) return kResDenied;
    int fd = alloc_fd();
    if (fd < 0) return kResNoFreeFd;
    const char* fmode;
    if (mode & 0x200) fmode = "w+b";                 /* create / truncate */
    else if (mode & 2) fmode = "r+b";
    else fmode = "rb";
    std::FILE* f = std::fopen(hp.c_str(), fmode);
    if (!f) {
        rt_log("mc", "Open %s mode=0x%x: fopen(%s) failed (errno %d)", cp.c_str(), mode, fmode, errno);
        return kResNoEntry;
    }
    if (!exists) {
        Meta m;
        m.attr = kAttrFile;
        m.create = m.modify = time_now();
        m.have_attr = m.have_create = m.have_modify = true;
        write_meta(cp, m);
    }
    g_fd[fd].used = true;
    g_fd[fd].f = f;
    g_fd[fd].card_path = cp;
    g_fd[fd].wrote = false;
    return fd;
}

int do_getdir(uint32_t port, uint32_t mode, int maxent, uint32_t table,
              const char* gname) {
    if (port != 0) return kResNoEntry;
    if (mode == 0) {
        g_listing.clear();
        g_listing_pos = 0;
        /* Split the final component off as the match pattern. */
        std::string raw = gname;
        size_t slash = raw.find_last_of('/');
        std::string dir_part = (slash == std::string::npos) ? "" : raw.substr(0, slash);
        std::string pat = (slash == std::string::npos) ? raw : raw.substr(slash + 1);
        if (slash == 0) dir_part = "/";
        if (pat.empty()) pat = "*";
        std::string cp = resolve_card_path(dir_part.empty() ? "." : dir_part.c_str());
        if (cp.empty()) return kResNoEntry;
        std::string hp = host_path(cp);
        std::error_code ec;
        if (!fs::is_directory(hp, ec)) return kResNoEntry;
        if (cp != "/") {
            /* Real cards list "." and ".." first inside a subdirectory. */
            if (wild_match(pat.c_str(), ".")) {
                DirEnt e = entry_info(cp, ".", true);
                g_listing.push_back(e);
            }
            if (wild_match(pat.c_str(), "..")) {
                DirEnt e = entry_info(cp, "..", true);
                g_listing.push_back(e);
            }
        }
        for (auto it = fs::directory_iterator(hp, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::string nm = it->path().filename().string();
            if (is_sidecar(nm) || nm.size() > 32) continue;
            if (!wild_match(pat.c_str(), nm.c_str())) continue;
            std::string child = (cp == "/") ? "/" + nm : cp + "/" + nm;
            std::error_code ec2;
            g_listing.push_back(entry_info(child, nm, fs::is_directory(it->path(), ec2)));
        }
        rt_log("mc", "GetDir '%s' (dir=%s pat=%s): %zu entries", gname, cp.c_str(),
            pat.c_str(), g_listing.size());
    }
    int written = 0;
    while (written < maxent && g_listing_pos < g_listing.size()) {
        const DirEnt& e = g_listing[g_listing_pos++];
        uint8_t rec[0x40] = {0};
        put_time(rec + 0x00, e.create);
        put_time(rec + 0x08, e.modify);
        wr32(rec, 0x10, e.size);
        rec[0x14] = (uint8_t)(e.attr & 0xFF);
        rec[0x15] = (uint8_t)(e.attr >> 8);
        std::memcpy(rec + 0x20, e.name.c_str(),
            e.name.size() < 32 ? e.name.size() : 32);
        rt_gwrite_bytes(table + (uint32_t)written * 0x40u, rec, sizeof(rec));
        ++written;
    }
    return written;
}

int do_delete(uint32_t port, const char* gname) {
    if (port != 0) return kResNoEntry;
    std::string cp = resolve_card_path(gname);
    if (cp.empty() || cp == "/") return kResNoEntry;
    std::string hp = host_path(cp);
    std::error_code ec;
    if (!fs::exists(hp, ec)) return kResNoEntry;
    if (fs::is_directory(hp, ec)) {
        for (auto it = fs::directory_iterator(hp, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (!is_sidecar(it->path().filename().string())) return kResNotEmpty;
        }
        /* Only sidecars left: clear them, then the directory. */
        fs::remove_all(hp, ec);
    } else {
        fs::remove(hp, ec);
    }
    fs::remove(sidecar_path(cp), ec);
    return ec ? kResDenied : kResOk;
}

int do_setfileinfo(uint32_t port, uint32_t flags, const uint8_t info[0x40],
                   const char* gname) {
    if (port != 0) return kResNoEntry;
    std::string cp = resolve_card_path(gname);
    if (cp.empty() || cp == "/") return kResNoEntry;
    std::string hp = host_path(cp);
    std::error_code ec;
    if (!fs::exists(hp, ec)) return kResNoEntry;
    if (flags & 0x10) { /* rename to info->EntryName */
        char newname[33] = {0};
        std::memcpy(newname, info + 0x20, 32);
        if (!newname[0] || std::strchr(newname, '/') || is_sidecar(newname)) {
            return kResDenied;
        }
        /* Renames stay within the entry's own directory. */
        size_t slash = cp.find_last_of('/');
        std::string dest = cp.substr(0, slash + 1) + newname;
        fs::rename(hp, host_path(dest), ec);
        if (ec) return kResDenied;
        std::error_code ec2;
        if (fs::exists(sidecar_path(cp), ec2)) {
            fs::rename(sidecar_path(cp), sidecar_path(dest), ec2);
        }
        rt_log("mc", "Rename %s -> %s", cp.c_str(), dest.c_str());
        return kResOk;
    }
    bool is_dir = fs::is_directory(hp, ec);
    Meta m = read_meta(cp);
    if (!m.have_attr) { m.attr = is_dir ? kAttrDir : kAttrFile; m.have_attr = true; }
    if (!m.have_create) { m.create = time_from_host(hp); m.have_create = true; }
    if (!m.have_modify) { m.modify = time_from_host(hp); m.have_modify = true; }
    if (flags & 1) m.create = get_time(info + 0x00);
    if (flags & 2) m.modify = get_time(info + 0x08);
    if (flags & 4) m.attr = (uint32_t)info[0x14] | ((uint32_t)info[0x15] << 8);
    write_meta(cp, m);
    return kResOk;
}

/* ---- the RPC service ------------------------------------------------------ */

void svc_mcserv(uint32_t fno, const uint8_t* send, uint32_t send_size,
                uint8_t* recv, uint32_t recv_size) {
    mc_init_backing();
    if (!send || send_size < 0x20) {
        rt_log("mc", "WARNING mcserv fno=0x%02x with short send (%u bytes): denied", fno, send_size);
        if (recv_size >= 4) wr32(recv, 0, (uint32_t)kResDenied);
        return;
    }
    uint32_t fd, port;
    int result = kResOk;
    switch (fno) {
        case 0xFE: /* Init: version handshake */
            for (auto& e : g_fd) {
                if (e.used && e.f) std::fclose(e.f);
                e = Fd{};
            }
            g_cwd = "/";
            wr32(recv, 0, 0);
            if (recv_size >= 12) {
                wr32(recv, 4, 0x20A);   /* mcserv version: libmc needs >= 0x20A */
                wr32(recv, 8, 0x20E);   /* mcman version: libmc needs >= 0x20E */
            }
            rt_log("mc", "Init -> 0 (mcserv 0x20A, mcman 0x20E)");
            return;
        case 0x01: { /* GetInfo */
            port = rd32(send, 4);
            uint32_t wb = rd32(send, 0x1C) & 0x1FFFFFFFu;
            uint32_t type = 0, free_kb = 0, formatted = 0;
            if (port == 0) {
                type = 2; /* PS2 card */
                uint32_t used = used_kb_recursive(g_base);
                free_kb = used < kTotalKb ? kTotalKb - used : 0;
                formatted = 1;
                result = g_first_getinfo ? kResChangedCard : kResOk;
                g_first_getinfo = false;
            }
            if (wb) {
                uint8_t blk[0xC0] = {0};
                wr32(blk, 0x00, type);
                wr32(blk, 0x04, free_kb);
                wr32(blk, 0x90, formatted);
                rt_gwrite_bytes(wb, blk, sizeof(blk));
            }
            rt_log("mc", "GetInfo port=%u slot=%u -> %d (type=%u free=%uKB formatted=%u)",
                port, rd32(send, 8), result, type, free_kb, formatted);
            break;
        }
        case 0x02: { /* Open (and Mkdir via mode 0x40) */
            port = rd32(send, 0);
            uint32_t mode = rd32(send, 8);
            const char* nm = send_name(send, send_size);
            result = do_open(port, mode, nm);
            rt_log("mc", "Open port=%u '%s' mode=0x%x -> %d", port, nm, mode, result);
            break;
        }
        case 0x03: /* Close */
            fd = rd32(send, 0);
            if (fd < kMaxFds && g_fd[fd].used) {
                std::fclose(g_fd[fd].f);
                if (g_fd[fd].wrote) {
                    Meta m = read_meta(g_fd[fd].card_path);
                    if (!m.have_attr) { m.attr = kAttrFile; m.have_attr = true; }
                    if (!m.have_create) { m.create = time_now(); m.have_create = true; }
                    m.modify = time_now();
                    m.have_modify = true;
                    write_meta(g_fd[fd].card_path, m);
                }
                rt_log("mc", "Close fd=%u (%s) -> 0", fd, g_fd[fd].card_path.c_str());
                g_fd[fd] = Fd{};
            } else {
                result = kResDenied;
                rt_log("mc", "Close fd=%u: not open -> %d", fd, result);
            }
            break;
        case 0x04: { /* Seek */
            fd = rd32(send, 0);
            int32_t offset = (int32_t)rd32(send, 0x10);
            uint32_t whence = rd32(send, 0x14);
            if (fd >= kMaxFds || !g_fd[fd].used) { result = kResDenied; break; }
            int w = whence == 1 ? SEEK_CUR : (whence == 2 ? SEEK_END : SEEK_SET);
            std::fseek(g_fd[fd].f, offset, w);
            result = (int)std::ftell(g_fd[fd].f);
            rt_log("mc", "Seek fd=%u offset=%d whence=%u -> %d", fd, offset, whence, result);
            break;
        }
        case 0x05: { /* Read */
            fd = rd32(send, 0);
            uint32_t size = rd32(send, 0xC);
            uint32_t buf = rd32(send, 0x18) & 0x1FFFFFFFu;
            uint32_t edge = rd32(send, 0x1C) & 0x1FFFFFFFu;
            if (fd >= kMaxFds || !g_fd[fd].used) { result = kResDenied; break; }
            std::vector<uint8_t> tmp(size);
            size_t got = size ? std::fread(tmp.data(), 1, size, g_fd[fd].f) : 0;
            if (got) rt_gwrite_bytes(buf, tmp.data(), (uint32_t)got);
            if (edge) {
                /* Everything landed in place: zero head/tail sizes so the
                 * EE end callback copies nothing. */
                uint8_t z[16] = {0};
                rt_gwrite_bytes(edge, z, sizeof(z));
            }
            result = (int)got;
            rt_log("mc", "Read fd=%u size=%u buf=0x%08x -> %d", fd, size, buf, result);
            break;
        }
        case 0x06: { /* Write: inline head bytes + remainder from EE RAM */
            fd = rd32(send, 0);
            uint32_t tail_size = rd32(send, 0xC);
            uint32_t head_size = rd32(send, 0x14);
            uint32_t tail_addr = rd32(send, 0x18) & 0x1FFFFFFFu;
            if (fd >= kMaxFds || !g_fd[fd].used) { result = kResDenied; break; }
            if (head_size > 16 || send_size < 0x20 + head_size) {
                rt_fatal("mc", rt_sched_current_ctx(),
                    "Write fd=%u: head_size=%u exceeds the inline area (send_size=%u)",
                    fd, head_size, send_size);
            }
            size_t wrote = 0;
            if (head_size) wrote += std::fwrite(send + 0x20, 1, head_size, g_fd[fd].f);
            if (tail_size) {
                std::vector<uint8_t> tmp(tail_size);
                rt_gread_bytes(tail_addr, tmp.data(), tail_size);
                wrote += std::fwrite(tmp.data(), 1, tail_size, g_fd[fd].f);
            }
            g_fd[fd].wrote = true;
            result = (int)wrote;
            rt_log("mc", "Write fd=%u head=%u tail=%u -> %d", fd, head_size, tail_size, result);
            break;
        }
        case 0x0A: /* Flush */
            fd = rd32(send, 0);
            if (fd < kMaxFds && g_fd[fd].used) std::fflush(g_fd[fd].f);
            rt_log("mc", "Flush fd=%u -> 0", fd);
            break;
        case 0x0C: { /* ChDir */
            port = rd32(send, 0);
            uint32_t pwd_wb = rd32(send, 0x10) & 0x1FFFFFFFu;
            const char* nm = send_name(send, send_size);
            std::string prev = g_cwd;
            if (port != 0) { result = kResNoEntry; break; }
            std::string cp = resolve_card_path(nm);
            std::error_code ec;
            if (cp.empty() || !fs::is_directory(host_path(cp), ec)) {
                result = kResNoEntry;
            } else {
                g_cwd = cp;
            }
            if (pwd_wb) {
                char out[0x400] = {0};
                std::snprintf(out, sizeof(out), "%s", prev.c_str());
                rt_gwrite_bytes(pwd_wb, out, sizeof(out));
            }
            rt_log("mc", "ChDir port=%u '%s' -> %d (cwd=%s)", port, nm, result, g_cwd.c_str());
            break;
        }
        case 0x0D: { /* GetDir */
            port = rd32(send, 0);
            uint32_t mode = rd32(send, 8);
            int maxent = (int)rd32(send, 0xC);
            uint32_t table = rd32(send, 0x10) & 0x1FFFFFFFu;
            result = do_getdir(port, mode, maxent, table, send_name(send, send_size));
            rt_log("mc", "GetDir port=%u mode=%u maxent=%d -> %d", port, mode, maxent, result);
            break;
        }
        case 0x0E: { /* SetFileInfo / Rename */
            port = rd32(send, 0);
            uint32_t flags = rd32(send, 8);
            uint32_t info_addr = rd32(send, 0x10) & 0x1FFFFFFFu;
            uint8_t info[0x40] = {0};
            if (info_addr) rt_gread_bytes(info_addr, info, sizeof(info));
            const char* nm = send_name(send, send_size);
            result = do_setfileinfo(port, flags, info, nm);
            rt_log("mc", "SetFileInfo port=%u '%s' flags=0x%x -> %d", port, nm, flags, result);
            break;
        }
        case 0x0F: { /* Delete */
            port = rd32(send, 0);
            const char* nm = send_name(send, send_size);
            result = do_delete(port, nm);
            rt_log("mc", "Delete port=%u '%s' -> %d", port, nm, result);
            break;
        }
        case 0x10: { /* Format: erase the card */
            port = rd32(send, 4);
            if (port != 0) { result = kResNoEntry; break; }
            std::error_code ec;
            uint32_t removed = 0;
            for (auto it = fs::directory_iterator(g_base, ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec)) {
                std::error_code ec2;
                removed += (uint32_t)fs::remove_all(it->path(), ec2);
            }
            g_cwd = "/";
            rt_log("mc", "Format port=%u: erased %u host entries under %s -> 0",
                port, removed, g_base.c_str());
            break;
        }
        case 0x11: /* Unformat: the virtual card stays formatted */
            rt_log("mc", "Unformat: ignored (virtual card is always formatted) -> 0");
            break;
        case 0x12: { /* GetEntSpace */
            port = rd32(send, 0);
            const char* nm = send_name(send, send_size);
            if (port != 0) { result = kResNoEntry; break; }
            std::string cp = resolve_card_path(nm);
            if (cp.empty()) { result = kResNoEntry; break; }
            std::error_code ec;
            int used = 0;
            for (auto it = fs::directory_iterator(host_path(cp), ec);
                 !ec && it != fs::directory_iterator(); it.increment(ec)) {
                if (!is_sidecar(it->path().filename().string())) ++used;
            }
            result = used < 400 ? 400 - used : 0; /* free entries in this dir */
            rt_log("mc", "GetEntSpace port=%u '%s' -> %d", port, nm, result);
            break;
        }
        case 0x14: case 0x15: /* thread priority housekeeping: accepted */
            rt_log("mc", "fno=0x%02x (housekeeping) -> 0", fno);
            break;
        default:
            result = kResDenied;
            rt_log("mc", "WARNING mcserv fno=0x%02x NOT MODELED (send_size=%u recv_size=%u) -> %d",
                fno, send_size, recv_size, result);
            break;
    }
    if (recv_size >= 4) wr32(recv, 0, (uint32_t)result);
}

} // namespace

void rt_mc_register_service() {
    rt_rpc_register_service(0x80000400, "mcserv", svc_mcserv);
}
