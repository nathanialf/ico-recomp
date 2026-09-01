/* iso/iso9660.h: disc image backend for the cdvdman HLE.
 *
 * Mounts the user's ICO disc image (plain 2048-byte .iso or raw 2352-byte
 * bin/cue; the sector layout is probed by locating the ISO9660 primary
 * volume descriptor) and answers path lookups and sector reads.
 *
 * Runtime-internal, NOT part of the ABI contract.
 */
#ifndef ICORECOMP_ISO_ISO9660_H
#define ICORECOMP_ISO_ISO9660_H

#include <cstddef>
#include <cstdint>

/* Matches the wire layout of sceCdlFILE (ps2sdk libcdvd-common.h, public SDK
 * fact): lsn, size, 8.3 name, date bytes. */
struct RtIsoFile {
    uint32_t lsn = 0;
    uint32_t size = 0;
    char name[16] = {0};
    uint8_t date[8] = {0};
};

/* Explicit disc image path (the runtime's --disc flag). Takes precedence
 * over every probed location; a path set here that does not mount is fatal
 * (the user asked for that file specifically). Call before rt_iso_mount. */
void rt_iso_set_path(const char* path);

/* The path rt_iso_set_path stored, or "" when --disc was not given. The
 * launcher asks so it can disable its disc picker: a run started with an
 * explicit --disc must boot that image or report why it cannot, never a
 * different one chosen in the window. */
const char* rt_iso_forced_path();

/* Mounts the disc image. Path resolution order:
 *   1. rt_iso_set_path (--disc), fatal when unusable
 *   2. rt_settings().launcher.disc_path, when non-empty (relative to
 *      rt_base_dir()); NOT fatal when it fails to mount, unlike --disc: the
 *      failure is logged with its source label and the search continues, so
 *      a stale saved path cannot brick a run. Only compiled in for targets
 *      that link host/settings.cpp (ICORECOMP_HAVE_SETTINGS); the ipu
 *      selftest does not, and skips this step.
 *   3. config/local.toml [disc] path (untracked, gitignored; relative to
 *      rt_base_dir())
 *   4. <decomp root>/baserom/Ico_USA.bin, then .iso; dev checkouts only,
 *      skipped when the config named no decomp root
 *   5. ico.iso / ico.bin / Ico_USA.iso / Ico_USA.bin next to the
 *      executable (the packaged convention: disc beside the exe)
 * Fatal if nothing usable is found (loud failure per CLAUDE.md). Verifies
 * the mount by locating SCUS_971.13 and DFDATAS/DATA.DF. */
void rt_iso_mount();
bool rt_iso_mounted();

/* Same search and the same log lines as rt_iso_mount, but returns false with
 * a one-line reason in `err` instead of calling rt_fatal. Nothing is left
 * mounted on failure. Used by rt_boot_precheck so the launcher can report a
 * missing or wrong disc in its window instead of dying before it can draw.
 * err may be null. */
bool rt_iso_probe_mount(char* err, size_t err_len);

/* Mounts exactly `path`: no probe list, no config or settings lookup. Runs
 * the same PVD read and SCUS_971.13 verification rt_iso_mount does, and
 * never calls rt_fatal. On failure `err` gets one human-readable line
 * naming the step that failed and the path, and nothing is left mounted
 * (the file handle is closed and the mount state reset, so a later
 * rt_iso_try_mount or rt_iso_mount starts clean). A disc already mounted
 * from another path is unmounted first, whether or not this call succeeds.
 * The mounted source label is "explicit path". err may be null. */
bool rt_iso_try_mount(const char* path, char* err, size_t err_len);

/* The path the current image was mounted from, and the label of the step in
 * the resolution order that supplied it ("--disc", "settings.json
 * launcher.disc_path", "config/local.toml [disc].path", "decomp baserom
 * bin/cue", "decomp baserom iso", "next to the executable", "explicit
 * path"). Both are "" when nothing is mounted. The returned pointers are
 * invalidated by the next mount. */
const char* rt_iso_mounted_path();
const char* rt_iso_mounted_source();

/* ISO9660 path lookup ("\\DFDATAS\\DATA.DF;1" style; '/' and '\\' both
 * accepted, case-insensitive, ";version" suffix ignored). */
bool rt_iso_search(const char* path, RtIsoFile* out);

/* Reads one 2048-byte data sector. Returns false past end of disc. */
bool rt_iso_read_sector(uint32_t lsn, uint8_t out[2048]);

/* Reads up to `count` consecutive 2048-byte sectors into `out`, which must
 * hold count * 2048 bytes. Returns how many were read; a short return means
 * the end of the disc or an I/O error. One seek and one read on a plain
 * 2048 image. */
uint32_t rt_iso_read_sectors(uint32_t lsn, uint32_t count, uint8_t* out);

uint32_t rt_iso_sector_size();  /* raw sector size of the mounted image */
uint32_t rt_iso_total_sectors();

#endif /* ICORECOMP_ISO_ISO9660_H */
