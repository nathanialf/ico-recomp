/* gs/render/gs_dump_parse.cpp: see gs_dump_parse.h for the format.
 *
 * Ours (MIT). Reads with stdio rather than mapping the file, because a dump
 * of a long run is larger than this process wants to map on a 32-bit host and
 * the read is sequential anyway.
 */
#include "gs_dump_parse.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace gsr {

namespace {

constexpr uint32_t kBankEntries = 0x200; /* u64 per 4 KiB bank */
/* Nothing legitimate is larger: a GIF submission is bounded by the DMA
 * transfer that produced it. A size past this is a corrupt file, and reading
 * it would be an allocation driven by file contents. */
constexpr uint32_t kMaxTransferBytes = 64u * 1024u * 1024u;

bool read_exact(FILE* f, void* dst, size_t bytes) {
    return std::fread(dst, 1, bytes, f) == bytes;
}

} // namespace

bool dump_parse(const char* path, DumpSink& sink, char* error, size_t error_bytes,
                uint64_t* packets_read) {
    if (error && error_bytes) error[0] = 0;
    if (packets_read) *packets_read = 0;

    FILE* f = std::fopen(path, "rb");
    if (!f) {
        if (error) std::snprintf(error, error_bytes, "cannot open %s", path);
        return false;
    }

    std::vector<uint8_t> payload;
    std::vector<uint64_t> lo(kBankEntries), hi(kBankEntries);
    uint64_t offset = 0;
    uint64_t packets = 0;
    bool ok = true;

    for (;;) {
        uint8_t type = 0;
        const size_t got = std::fread(&type, 1, 1, f);
        if (got == 0) break; /* clean end of stream */
        const uint64_t packet_at = offset;
        offset += 1;

        if (type == 0) { /* GIF transfer */
            uint8_t p = 0;
            uint32_t size = 0;
            if (!read_exact(f, &p, 1) || !read_exact(f, &size, 4)) {
                if (error) std::snprintf(error, error_bytes,
                    "truncated GIF header at byte %llu", (unsigned long long)packet_at);
                ok = false;
                break;
            }
            offset += 5;
            if (size > kMaxTransferBytes || (size % 16) != 0) {
                if (error) std::snprintf(error, error_bytes,
                    "GIF transfer at byte %llu claims %u bytes, which is not a sane "
                    "quadword count", (unsigned long long)packet_at, size);
                ok = false;
                break;
            }
            payload.resize(size);
            if (size && !read_exact(f, payload.data(), size)) {
                if (error) std::snprintf(error, error_bytes,
                    "truncated GIF payload at byte %llu (wanted %u bytes)",
                    (unsigned long long)packet_at, size);
                ok = false;
                break;
            }
            offset += size;
            if (p > 2) {
                if (error) std::snprintf(error, error_bytes,
                    "GIF transfer at byte %llu names path %u", (unsigned long long)packet_at, p);
                ok = false;
                break;
            }
            sink.gif((int)p, payload.data(), size / 16);
        } else if (type == 1) { /* vsync */
            uint8_t field = 0;
            if (!read_exact(f, &field, 1)) {
                if (error) std::snprintf(error, error_bytes,
                    "truncated vsync at byte %llu", (unsigned long long)packet_at);
                ok = false;
                break;
            }
            offset += 1;
            sink.vsync(field & 1u);
        } else if (type == 2) { /* ReadFIFO */
            uint32_t size = 0;
            if (!read_exact(f, &size, 4)) {
                if (error) std::snprintf(error, error_bytes,
                    "truncated ReadFIFO at byte %llu", (unsigned long long)packet_at);
                ok = false;
                break;
            }
            offset += 4;
            sink.read_fifo(size);
        } else if (type == 3) { /* privileged register snapshot */
            if (!read_exact(f, lo.data(), kBankEntries * sizeof(uint64_t))
                || !read_exact(f, hi.data(), kBankEntries * sizeof(uint64_t))) {
                if (error) std::snprintf(error, error_bytes,
                    "truncated privileged register snapshot at byte %llu",
                    (unsigned long long)packet_at);
                ok = false;
                break;
            }
            offset += 2 * kBankEntries * sizeof(uint64_t);
            sink.priv_snapshot(lo.data(), hi.data());
        } else {
            if (error) std::snprintf(error, error_bytes,
                "unknown packet type 0x%02x at byte %llu", type,
                (unsigned long long)packet_at);
            ok = false;
            break;
        }
        ++packets;
    }

    std::fclose(f);
    if (packets_read) *packets_read = packets;
    return ok;
}

} // namespace gsr
