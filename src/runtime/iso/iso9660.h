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

/* Mounts the disc image. Path resolution order:
 *   1. rt_iso_set_path (--disc), fatal when unusable
 *   2. config/local.toml [disc] path (untracked, gitignored; relative to
 *      rt_base_dir())
 *   3. <decomp root>/baserom/Ico_USA.bin, then .iso
 *   4. ico.iso / ico.bin / Ico_USA.iso / Ico_USA.bin in the working
 *      directory (the packaged-zip convention: disc next to the exe)
 * Fatal if nothing usable is found (loud failure per CLAUDE.md). Verifies
 * the mount by locating SCUS_971.13 and DFDATAS/DATA.DF. */
void rt_iso_mount();
bool rt_iso_mounted();

/* ISO9660 path lookup ("\\DFDATAS\\DATA.DF;1" style; '/' and '\\' both
 * accepted, case-insensitive, ";version" suffix ignored). */
bool rt_iso_search(const char* path, RtIsoFile* out);

/* Reads one 2048-byte data sector. Returns false past end of disc. */
bool rt_iso_read_sector(uint32_t lsn, uint8_t out[2048]);

uint32_t rt_iso_sector_size();  /* raw sector size of the mounted image */
uint32_t rt_iso_total_sectors();

#endif /* ICORECOMP_ISO_ISO9660_H */
