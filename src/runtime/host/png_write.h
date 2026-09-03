/* host/png_write.h: minimal PNG encoder shared by the diagnostic dumps and
 * the packaged icon tool.
 *
 * Stored (uncompressed) DEFLATE blocks inside the zlib wrapper: legal per
 * RFC 1950/1951 and needs no compressor. These files are diagnostic images
 * or a handful of small icon frames, so coming out a few times larger than
 * a real compressor would produce does not matter.
 *
 * Lifted out of ui/title_logo_selftest.cpp (which now links this file
 * instead of carrying its own copy) so ui/icon_extract.cpp can use the same
 * writer.
 */
#ifndef ICORECOMP_HOST_PNG_WRITE_H
#define ICORECOMP_HOST_PNG_WRITE_H

#include <cstddef>
#include <cstdint>
#include <vector>

/* Encodes `width` by `height` pixels (row-major from the top) to a PNG
 * byte stream. `channels` is 3 (RGB, colour type 2) or 4 (RGBA, colour
 * type 6). Returns an empty vector for any other channel count, for a zero
 * width or height, and for a null `pixels`. */
std::vector<uint8_t> rt_png_encode(uint32_t width, uint32_t height, const uint8_t* pixels, int channels);

/* Encodes and writes to `path`. Returns false with a one-line reason in
 * `err` (may be null) on anything rt_png_encode refuses or an unwritable
 * path. */
bool rt_png_write(const char* path, uint32_t width, uint32_t height, const uint8_t* pixels, int channels,
                  char* err, size_t err_len);

#endif /* ICORECOMP_HOST_PNG_WRITE_H */
