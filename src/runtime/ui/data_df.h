/* ui/data_df.h: DFDATAS/DATA.DF, the outer container both the launcher's
 * title wordmark (STGLOG.DF) and the packaged save icon (icon.sys,
 * boy_blk.ico) are read out of.
 *
 * DATA.DF is a plain table at the start of the file: a u32 entry count,
 * then that many 40-byte entries {char name[32]; u32 offset; u32 size},
 * each naming a byte range of DATA.DF itself. Names are compared exactly
 * (case-sensitive strcmp against the stored, NUL/pad-terminated field); the
 * outer table on the retail disc carries both "STGLOG.DF" and "icon.sys",
 * so no case convention is assumed here.
 *
 * Lifted out of ui/title_logo.cpp (which now calls these instead of its own
 * copies) so ui/icon_extract.cpp and ui/save_icon.cpp can read the same
 * container. No inflate here: both boy_blk.ico and icon.sys are stored raw,
 * unlike STGLOG.DF, which is a further DEFLATE stream inside its own entry
 * (see title_logo.cpp's file comment) -- that inner layer stays private to
 * title_logo.cpp.
 *
 * Runtime-internal, NOT part of the ABI contract.
 */
#ifndef ICORECOMP_UI_DATA_DF_H
#define ICORECOMP_UI_DATA_DF_H

#include "../iso/iso9660.h"

#include <cstddef>
#include <cstdint>
#include <vector>

constexpr size_t kDataDfOuterEntrySize = 40;
constexpr size_t kDataDfOuterNameBytes = 32;

/* Locates DFDATAS/DATA.DF on the mounted disc and fills `out`. Requires
 * rt_iso_mounted(); false with a reason in `err` (may be null) otherwise or
 * when the disc has no such file. */
bool rt_data_df_open(RtIsoFile* out, char* err, size_t err_len);

/* Reads [offset, offset + length) of DATA.DF into `out`. The ISO reader
 * only offers whole 2048-byte sectors, so this reads the covering sectors
 * and trims to the exact range. */
bool rt_data_df_read(const RtIsoFile& file, uint64_t offset, size_t length, std::vector<uint8_t>& out,
                     char* err, size_t err_len);

/* Finds one outer entry by exact name. Reads the outer table itself (a
 * bounded amount: the count plus that many 40-byte entries), so a caller
 * needs no separate step to load it. */
bool rt_data_df_find(const RtIsoFile& file, const char* name, uint32_t* offset, uint32_t* size,
                     char* err, size_t err_len);

#endif /* ICORECOMP_UI_DATA_DF_H */
