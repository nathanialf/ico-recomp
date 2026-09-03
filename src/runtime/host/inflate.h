/* host/inflate.h: raw DEFLATE (RFC 1951) decompression.
 *
 * Written for this project. Nothing in the tree provided one: FreeType's
 * bundled zlib is compiled out (FT_DISABLE_ZLIB in CMakeLists.txt) and every
 * other copy sits inside the paraLLEl-GS submodule, which the CI gate builds
 * with ICORECOMP_PARALLEL_GS=OFF. A decoder here keeps the runtime's own
 * dependency set unchanged.
 *
 * The one caller is ui/title_logo.cpp: the disc's DFDATAS/DATA.DF holds inner
 * archives stored as raw DEFLATE streams with no zlib or gzip wrapper, which
 * is what "raw" means here (zlib's wbits -15).
 *
 * The decoder is a two-level table one; the canonical assignment of codes to
 * symbols is the one in RFC 1951 section 3.2.2, implemented from that
 * description. See the file comment in inflate.cpp.
 *
 * Runtime-internal, NOT part of the ABI contract.
 */
#ifndef ICORECOMP_HOST_INFLATE_H
#define ICORECOMP_HOST_INFLATE_H

#include <cstddef>
#include <cstdint>
#include <vector>

/* Inflates `in` into `out` (cleared first).
 *
 * `out_limit` is how much the caller wants. Decoding stops as soon as `out`
 * holds at least that many bytes, which is how a caller that only wants a
 * prefix of a large archive avoids paying for the rest; the return is still
 * true and `out` may overshoot by up to one copy length (258 bytes). Pass
 * SIZE_MAX to decode the whole stream.
 *
 * `out_ceiling` is how much the caller will tolerate, and it is a hard limit
 * rather than a stopping point: a stream that would expand past it fails with
 * an error instead of being decoded. It is tested inside the literal and copy
 * loop, not only between blocks, so one crafted block cannot allocate without
 * bound before anything notices. The two are separate because a prefix caller
 * wants a small `out_limit` and still needs a ceiling that covers the whole
 * file it is prepared to hold.
 *
 * Returns false with a one-line reason in `err` (may be null) for a
 * malformed stream, a truncated one, a back reference that points before the
 * start of the output, or an output that would pass the ceiling. Never
 * partially succeeds silently: a false return leaves `out` holding whatever
 * had been produced, and the caller must treat it as unusable.
 *
 * Allocation failure is not caught here: this throws std::bad_alloc like any
 * other vector growth. A caller that has promised never to fail (see
 * ui/title_logo.h) catches it itself.
 */
bool rt_inflate_raw(const uint8_t* in, size_t in_len, std::vector<uint8_t>& out,
                    size_t out_limit, size_t out_ceiling, char* err, size_t err_len);

#endif /* ICORECOMP_HOST_INFLATE_H */
