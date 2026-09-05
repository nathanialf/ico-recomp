/* gs/render/gs_swizzle_reference.h: interface of the independent swizzle
 * reference. Used only by gs_swizzle_selftest.cpp; nothing in the renderer
 * links it. See gs_swizzle_reference.cpp for what it does and does not check.
 */
#ifndef ICORECOMP_GS_SWIZZLE_REFERENCE_H
#define ICORECOMP_GS_SWIZZLE_REFERENCE_H

#include <cstdint>

namespace gsref {

struct Address {
    /* False when this file holds no column table for the format (PSMT4), in
     * which case only block and column are meaningful. */
    bool valid;
    uint32_t byte_addr;    /* byte offset into the 4 MiB store */
    uint32_t bit_in_byte;  /* 0 or 4 for PSMT4, 0 otherwise */
    uint32_t block;        /* 256-byte block index, 0..16383 */
    uint32_t column;       /* 64-byte column inside the block, 0..3 */
};

/* True when address() can answer with a literal column table for this format. */
bool has_column_table(uint32_t psm);

/* base_block is the buffer base in 256-byte blocks; fbw is the buffer width
 * in 64-pixel units, exactly as gs_swizzle.h takes them. */
Address address(uint32_t psm, uint32_t base_block, uint32_t fbw, uint32_t x, uint32_t y);

} // namespace gsref

#endif /* ICORECOMP_GS_SWIZZLE_REFERENCE_H */
