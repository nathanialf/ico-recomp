/* gs/gs_dumpwriter.cpp: GS backend that records traffic in the paraLLEl-GS
 * raw stream format, replayable with its gs-stream-replayer tool.
 *
 * Format (derived by reading the paraLLEl-GS dump reader/writer sources,
 * https://github.com/Arntzen-Software/parallel-gs, dump/gs_dump_parser.cpp
 * GSDumpParser::open_raw + iterate_until_vsync and
 * dump/gs_dump_generator.cpp. paraLLEl-GS is LGPLv3+; this file is an
 * independent implementation of the on-disk format for interoperability,
 * no code is copied):
 *
 *   stream  := packet*
 *   packet  := 0x00 u8:path u32:size_bytes data[size]   GIF transfer
 *            | 0x01 u8:field                            vsync
 *            | 0x02 u32:size                            ReadFIFO (unused)
 *            | 0x03 PrivRegisterState[8192 bytes]       priv snapshot
 *
 *   PrivRegisterState is two 4 KB banks of u64[0x200]. A privileged
 *   register at byte offset `off` (from 0x12000000) lives at u64 index
 *   ((off & 0xFFF) >> 4) * 2 of the low bank for off < 0x1000, high bank
 *   for 0x1000 <= off < 0x2000 (each register occupies a 16-byte slot,
 *   value in the low 8 bytes). All values little-endian.
 *
 * The parser replays with a zero-initialized GS, so no header or initial
 * state snapshot is needed; the game's own init packets set everything.
 * Following the reference generator, a PrivRegisters snapshot is emitted
 * immediately before every Vsync packet.
 *
 * When ICORECOMP_GS_DUMP is unset this class still runs as the priv
 * register shadow and statistics collector, it writes no file.
 * The dump path must be outside the repository (check_no_rom is a
 * mechanical gate; GS packets are ROM-derived data).
 */
#include "gs_backend.h"

#include "runtime.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kBankQw64 = 0x200; /* u64 entries per 4 KB bank */

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

class DumpBackend final : public GsBackend {
public:
    explicit DumpBackend(const char* path) {
        if (path && path[0]) {
            m_file = std::fopen(path, "wb");
            if (m_file) {
                /* Optional recording window, for capturing a short slice of
                 * a long run without filling the disk: start at field
                 * ICORECOMP_GS_DUMP_FROM (default 0) and record
                 * ICORECOMP_GS_DUMP_FIELDS fields (default 0 = unlimited).
                 * A windowed capture starts with a priv snapshot; VRAM
                 * contents uploaded before the window are absent, so the
                 * first frames may miss long-lived textures. */
                const char* from = std::getenv("ICORECOMP_GS_DUMP_FROM");
                const char* count = std::getenv("ICORECOMP_GS_DUMP_FIELDS");
                if (from && from[0]) m_from = std::strtoull(from, nullptr, 10);
                if (count && count[0]) m_count = std::strtoull(count, nullptr, 10);
                if (m_from || m_count) {
                    rt_log_info("gs", "dump writer: recording paraLLEl-GS raw stream to %s "
                        "(window: fields %" PRIu64 "..%" PRIu64 ")",
                        path, m_from, m_count ? m_from + m_count : UINT64_MAX);
                } else {
                    rt_log_info("gs", "dump writer: recording paraLLEl-GS raw stream to %s", path);
                }
            } else {
                rt_log_warn("gs", "dump writer: FAILED to open %s, recording disabled", path);
            }
        } else {
            rt_log_info("gs", "dump writer: ICORECOMP_GS_DUMP not set, shadow/stats only");
        }
    }

    ~DumpBackend() override {
        if (m_file) std::fclose(m_file);
    }

    void submit_gif(int path, const uint8_t* data, uint32_t qwords) override {
        if (path < 0 || path > 2 || qwords == 0) return;
        ++m_packets[path];
        m_qwords[path] += qwords;
        m_transfer_since_vsync = true;
        if (m_file && in_window()) {
            if (!m_window_open) {
                write_priv_snapshot();
                m_window_open = true;
            }
            const uint8_t type = 0;
            const uint8_t p = (uint8_t)path;
            const uint32_t size = qwords * 16u;
            std::fwrite(&type, 1, 1, m_file);
            std::fwrite(&p, 1, 1, m_file);
            std::fwrite(&size, 4, 1, m_file);
            std::fwrite(data, 1, size, m_file);
        }
        if (is_pow2(m_packets[path])) {
            rt_log_debug("gs", "PATH%d packet #%" PRIu64 ": %u qw (total %" PRIu64 " qw)",
                path + 1, m_packets[path], qwords, m_qwords[path]);
        }
    }

    void write_priv(uint32_t offset, uint64_t v) override {
        offset &= 0x1FFF;
        ++m_priv_writes;
        if (offset < 0x1000) {
            m_lo[(offset >> 4) * 2] = v;
        } else {
            m_hi[((offset - 0x1000) >> 4) * 2] = v;
        }
    }

    uint64_t read_priv(uint32_t offset) override {
        offset &= 0x1FFF;
        if (offset < 0x1000) return m_lo[(offset >> 4) * 2];
        return m_hi[((offset - 0x1000) >> 4) * 2];
    }

    bool vsync(unsigned field) override {
        ++m_vsyncs;
        bool presented = m_transfer_since_vsync;
        if (m_file && in_window()) {
            write_priv_snapshot();
            uint8_t type = 1; /* Vsync */
            const uint8_t f = (uint8_t)(field & 1);
            std::fwrite(&type, 1, 1, m_file);
            std::fwrite(&f, 1, 1, m_file);
            std::fflush(m_file); /* keep truncated runs replayable */
            m_window_open = true;
        } else if (m_file && m_window_open && !in_window()) {
            /* Window just closed: stop touching the file so the capture
             * stays a clean [from, from+count) slice. */
            std::fclose(m_file);
            m_file = nullptr;
            rt_log_info("gs", "dump writer: window complete at field %" PRIu64 ", file closed", m_vsyncs);
        }
        m_transfer_since_vsync = false;
        return presented;
    }

    void report_stats() override {
        rt_log_info("gs", "---- GS backend stats ----");
        rt_log_info("gs", "  packets: PATH1=%" PRIu64 " (%" PRIu64 " qw)  PATH2=%" PRIu64 " (%" PRIu64 " qw)  PATH3=%" PRIu64 " (%" PRIu64 " qw)",
            m_packets[0], m_qwords[0], m_packets[1], m_qwords[1], m_packets[2], m_qwords[2]);
        rt_log_info("gs", "  priv writes=%" PRIu64 " vsyncs=%" PRIu64, m_priv_writes, m_vsyncs);
        static const struct { uint32_t off; const char* name; } kShow[] = {
            {0x0000, "PMODE"}, {0x0020, "SMODE2"}, {0x0070, "DISPFB1"}, {0x0080, "DISPLAY1"},
            {0x0090, "DISPFB2"}, {0x00A0, "DISPLAY2"}, {0x00E0, "BGCOLOR"}, {0x1000, "CSR"},
        };
        for (const auto& s : kShow) {
            uint64_t v = (s.off < 0x1000) ? m_lo[(s.off >> 4) * 2] : m_hi[((s.off - 0x1000) >> 4) * 2];
            rt_log_info("gs", "  %-8s = 0x%016" PRIx64, s.name, v);
        }
    }

private:
    bool in_window() const {
        return m_vsyncs >= m_from && (m_count == 0 || m_vsyncs < m_from + m_count);
    }

    void write_priv_snapshot() {
        uint8_t type = 3; /* PrivRegisters snapshot */
        std::fwrite(&type, 1, 1, m_file);
        std::fwrite(m_lo, sizeof(uint64_t), kBankQw64, m_file);
        std::fwrite(m_hi, sizeof(uint64_t), kBankQw64, m_file);
    }

    FILE* m_file = nullptr;
    uint64_t m_from = 0;
    uint64_t m_count = 0;
    bool m_window_open = false;
    uint64_t m_lo[kBankQw64] = {};
    uint64_t m_hi[kBankQw64] = {};
    uint64_t m_packets[3] = {};
    uint64_t m_qwords[3] = {};
    uint64_t m_priv_writes = 0;
    uint64_t m_vsyncs = 0;
    bool m_transfer_since_vsync = false;
};

} // namespace

GsBackend* rt_gs_make_dump_backend(const char* dump_path) {
    return new DumpBackend(dump_path);
}
