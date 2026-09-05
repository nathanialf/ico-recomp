/* ui/data_df.cpp: see data_df.h. */
#include "data_df.h"

#include "../runtime.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

void set_err(char* err, size_t err_len, const char* fmt, ...) {
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

} // namespace

bool rt_data_df_open(RtIsoFile* out, char* err, size_t err_len) {
    if (!rt_iso_mounted()) {
        set_err(err, err_len, "no disc is mounted");
        return false;
    }
    if (!rt_iso_search("/DFDATAS/DATA.DF", out)) {
        set_err(err, err_len, "the mounted disc has no DFDATAS/DATA.DF");
        return false;
    }
    return true;
}

bool rt_data_df_read(const RtIsoFile& file, uint64_t offset, size_t length, std::vector<uint8_t>& out,
                     char* err, size_t err_len) {
    if (offset > file.size || length > file.size - offset) {
        set_err(err, err_len, "DATA.DF range [%llu, +%zu) is outside the file's %u bytes",
            (unsigned long long)offset, length, file.size);
        return false;
    }
    const uint32_t first = uint32_t(offset / 2048);
    const uint32_t skip = uint32_t(offset % 2048);
    const uint32_t sectors = uint32_t((skip + length + 2047) / 2048);

    std::vector<uint8_t> raw(size_t(sectors) * 2048);
    const uint32_t got = rt_iso_read_sectors(file.lsn + first, sectors, raw.data());
    if (got != sectors) {
        set_err(err, err_len, "read %u of %u sectors at LSN %u", got, sectors, file.lsn + first);
        return false;
    }
    out.assign(raw.begin() + skip, raw.begin() + skip + length);
    return true;
}

bool rt_data_df_find(const RtIsoFile& file, const char* name, uint32_t* offset, uint32_t* size,
                     char* err, size_t err_len) {
    std::vector<uint8_t> head;
    if (!rt_data_df_read(file, 0, 2048, head, err, err_len)) return false;
    const uint32_t count = rd_u32(head.data());
    if (count == 0 || count > 4096) {
        set_err(err, err_len, "DATA.DF outer table declares %u entries", count);
        return false;
    }
    const size_t table_bytes = 4 + size_t(count) * kDataDfOuterEntrySize;
    if (!rt_data_df_read(file, 0, table_bytes, head, err, err_len)) return false;

    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = head.data() + 4 + size_t(i) * kDataDfOuterEntrySize;
        char stored[kDataDfOuterNameBytes + 1];
        std::memcpy(stored, e, kDataDfOuterNameBytes);
        stored[kDataDfOuterNameBytes] = 0;
        if (std::strcmp(stored, name) != 0) continue;
        *offset = rd_u32(e + kDataDfOuterNameBytes);
        *size = rd_u32(e + kDataDfOuterNameBytes + 4);
        rt_log_info("ui", "DATA.DF: outer table has %u entries; %s at offset %u, %u bytes", count, name,
            *offset, *size);
        return true;
    }
    set_err(err, err_len, "DATA.DF has no outer entry named %s (%u entries scanned)", name, count);
    return false;
}
