/* gs/render/gs_dump_parse.h: our own reader for the raw GS stream
 * gs/gs_dumpwriter.cpp writes.
 *
 * Ours (MIT). The format is ours to read: gs_dumpwriter.cpp documents it in
 * full and this file implements the other half of it, so the pair can be
 * checked against each other without a third party's parser. The existing
 * replay path feeds the same file through paraLLEl-GS's parser, which is a
 * useful second opinion and stays available (icorecomp-gs-replay
 * --backend=parallel).
 *
 *   stream  := packet*
 *   packet  := 0x00 u8:path u32:size_bytes data[size]   GIF transfer
 *            | 0x01 u8:field                            vsync
 *            | 0x02 u32:size                            ReadFIFO
 *            | 0x03 u64 lo[512] u64 hi[512]             priv register snapshot
 *
 * A privileged register at byte offset `off` from 0x12000000 lives at index
 * (off & 0xFFF) >> 4, doubled, of the low bank for off < 0x1000 and of the
 * high bank above it. All values little-endian.
 */
#ifndef ICORECOMP_GS_DUMP_PARSE_H
#define ICORECOMP_GS_DUMP_PARSE_H

#include <cstddef>
#include <cstdint>

namespace gsr {

class DumpSink {
public:
    virtual ~DumpSink() = default;
    virtual void gif(int path, const uint8_t* data, uint32_t qwords) = 0;
    virtual void vsync(unsigned field) = 0;
    /* One snapshot: 512 u64 per bank, in the file's own layout. */
    virtual void priv_snapshot(const uint64_t* lo, const uint64_t* hi) = 0;
    /* ReadFIFO packets carry a size and no payload in this format. */
    virtual void read_fifo(uint32_t /*size*/) {}
};

/* Reads the whole file, calling back in order. Returns true when the stream
 * parsed to its end; false, with `error` naming the byte offset and the
 * reason, when it did not. A truncated file is a failure the caller reports:
 * the dump writer flushes at every vsync so a truncated capture is normal
 * after a crash, and the replay tool says how far it got. */
bool dump_parse(const char* path, DumpSink& sink, char* error, size_t error_bytes,
                uint64_t* packets_read);

} // namespace gsr

#endif /* ICORECOMP_GS_DUMP_PARSE_H */
