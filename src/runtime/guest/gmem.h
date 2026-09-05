/* guest/gmem.h: the guest-memory accessors the three observer modules share.
 *
 * guest/menu_nav.cpp, guest/achievements.cpp and guest/widescreen.cpp each
 * read (and two of them write) a handful of words in the game's own memory.
 * All three want the same contract, and it is not the one the rest of the
 * runtime uses:
 *
 *   - never rt_gread32 / rt_gwrite32. Those are fatal on an unmapped
 *     address, and nothing these three modules do is worth ending a run
 *     over: an unmapped page is the ordinary state before the ELF is loaded,
 *     and the right answer to it is "no reading this field".
 *   - false, not a substituted value, when the address is not mapped. A
 *     caller that cannot read a word must know it, not receive a zero.
 *   - both ends of a multi-byte read are checked and then the copy runs off
 *     the first pointer. rt_gptr resolves 64 KB pages, so a read whose first
 *     and last byte are both mapped is one contiguous range.
 *
 * Header-only and inline: these are on the per-field path of all three
 * modules, and the alternative to inlining them is the three private copies
 * this file replaced.
 *
 * Ours (MIT).
 */
#ifndef ICORECOMP_GUEST_GMEM_H
#define ICORECOMP_GUEST_GMEM_H

#include "../ee/kernel.h"

#include <cstdint>
#include <cstring>

namespace rt_gmem {

/* `len` bytes out of guest memory. False when either end is unmapped. */
inline bool read_bytes(uint32_t addr, void* out, uint32_t len) {
    const uint8_t* p = rt_gptr(addr);
    if (!p || len == 0 || !rt_gptr(addr + len - 1)) return false;
    std::memcpy(out, p, len);
    return true;
}

/* `len` bytes into guest memory. False when either end is unmapped. */
inline bool write_bytes(uint32_t addr, const void* in, uint32_t len) {
    uint8_t* p = rt_gptr(addr);
    if (!p || len == 0 || !rt_gptr(addr + len - 1)) return false;
    std::memcpy(p, in, len);
    return true;
}

inline bool read_word(uint32_t addr, uint32_t* out) {
    return read_bytes(addr, out, sizeof(uint32_t));
}

/* The same word read as the signed value the game stores there: every index
 * and link field in the menu tables is signed, with -1 for "none". */
inline bool read_i32(uint32_t addr, int32_t* out) {
    uint32_t v = 0;
    if (!read_word(addr, &v)) return false;
    *out = (int32_t)v;
    return true;
}

inline bool read_f32(uint32_t addr, float* out) {
    return read_bytes(addr, out, sizeof(float));
}

inline bool write_word(uint32_t addr, uint32_t v) {
    return write_bytes(addr, &v, sizeof(v));
}

inline bool write_f32(uint32_t addr, float v) {
    return write_bytes(addr, &v, sizeof(v));
}

} // namespace rt_gmem

#endif /* ICORECOMP_GUEST_GMEM_H */
