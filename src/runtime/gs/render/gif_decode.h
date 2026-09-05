/* gs/render/gif_decode.h: GIF packet decoding, A+D, PACKED and REGLIST.
 *
 * Ours (MIT). Register layouts here are public PS2 hardware documentation
 * (the GS User's Manual's GIFtag and "packed format" chapters, and ps2tek),
 * the same sources hw/gif.cpp and hw/geomcheck.cpp name.
 *
 * Relationship to the two existing readers:
 *
 *   hw/gif.cpp        frames packets for the transport. It tracks tag
 *                     boundaries so a malformed submission is loud, and
 *                     forwards the bytes verbatim. It decodes no registers.
 *   hw/geomcheck.cpp  a diagnostic that reads the few registers a vertex
 *                     check needs, and steps over everything else.
 *
 * This one is the renderer's own decoder: it turns every packet into the
 * sequence of GS register writes the hardware would perform, plus the HWREG
 * image stream, and nothing else. It is a header so the decode inlines into
 * the backend's hot path and so the selftest links no renderer objects.
 *
 * State persists across calls because PATH3 may legally split a packet
 * across DMA kicks; see hw/gif.cpp's PathState for the same reason.
 *
 * The sink is a template parameter rather than an interface, so the compiler
 * sees through it:
 *
 *   void reg(uint32_t addr, uint64_t value)   one GS register write
 *   void image(const uint8_t* qw, uint32_t qwords)
 *                                             HWREG payload, still packed
 *   void note(const char* what)               something the decoder wants
 *                                             said out loud once; the caller
 *                                             owns the rate limiting
 */
#ifndef ICORECOMP_GIF_DECODE_H
#define ICORECOMP_GIF_DECODE_H

#include <cstdint>
#include <cstring>

namespace gsr {

/* GS register addresses the PACKED descriptors name directly. The descriptor
 * value and the register address are the same number for 0x00 to 0x0D, which
 * is what makes the PACKED table a straight mapping. */
enum : uint32_t {
    GS_REG_PRIM    = 0x00,
    GS_REG_RGBAQ   = 0x01,
    GS_REG_ST      = 0x02,
    GS_REG_UV      = 0x03,
    GS_REG_XYZF2   = 0x04,
    GS_REG_XYZ2    = 0x05,
    GS_REG_TEX0_1  = 0x06,
    GS_REG_TEX0_2  = 0x07,
    GS_REG_CLAMP_1 = 0x08,
    GS_REG_CLAMP_2 = 0x09,
    GS_REG_FOG     = 0x0A,
    GS_REG_XYZF3   = 0x0C,
    GS_REG_XYZ3    = 0x0D,
    GS_REG_AD      = 0x0E,
    GS_REG_NOP     = 0x0F,
    GS_REG_HWREG   = 0x54,
};

struct GifDecodeState {
    uint64_t regs = 0;        /* REGS descriptor list of the open tag */
    uint32_t nreg = 0;        /* descriptors per loop, 1..16 */
    uint32_t flg = 0;         /* 0 PACKED, 1 REGLIST, 2 IMAGE, 3 reserved */
    uint32_t values_left = 0; /* PACKED/REGLIST register values still to read */
    uint32_t reg_index = 0;   /* next descriptor inside the loop */
    uint32_t image_qw = 0;    /* IMAGE payload qwords still to read */
    bool in_tag = false;      /* a tag is open and its payload is unfinished */
    /* GIFtag's EOP bit is decoded and dropped. Framing is hw/gif.cpp's job
     * and it tracks EOP itself over the whole submission; a copy kept here
     * that nothing read was a second place for the same fact to live. */
    /* Q latched by a PACKED ST and applied to the next RGBAQ, as the
     * hardware's internal Q register does. Float bits, 1.0 at reset. */
    uint32_t q_bits = 0x3F800000u;

    /* Byte offset, inside the `data` buffer of the call that is running, of
     * the 64-bit little-endian word the register value now being delivered
     * to the sink was read from. kNoValueOffset when the write has no such
     * word, which is the PRE bit's synthesized PRIM.
     *
     * It exists for a sink that has to write back into the packet rather
     * than only read it: hw/gif.cpp's widescreen 2D transform patches the X
     * field of a vertex, and X is bits 0..15 of that same word in all three
     * layouts a vertex can arrive in (PACKED XYZ*, REGLIST, and A+D, where
     * the value is the low half of the qword and the register address is the
     * high half). Every other sink ignores it. */
    static constexpr uint32_t kNoValueOffset = 0xFFFFFFFFu;
    uint32_t value_off = kNoValueOffset;
};

namespace detail {

inline uint64_t load64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

/* One PACKED qword to (register address, register value).
 *
 * Returns the register address, or GS_REG_NOP when the descriptor writes
 * nothing. The ADC bit of XYZF2/XYZ2 lives at bit 111 of the qword and
 * redirects the write to the XYZF3/XYZ3 address, which is the "queue the
 * vertex, do not kick" form. */
inline uint32_t unpack_packed(uint32_t desc, uint64_t d0, uint64_t d1,
                              uint64_t& value, GifDecodeState& st) {
    switch (desc) {
        case GS_REG_PRIM:
            value = d0 & 0x7FFull;
            return GS_REG_PRIM;
        case GS_REG_RGBAQ: {
            /* R, G, B, A each occupy the low byte of their own 32-bit lane;
             * Q comes from the latched ST, not from this qword. */
            const uint64_t r = d0 & 0xFFull;
            const uint64_t g = (d0 >> 32) & 0xFFull;
            const uint64_t b = d1 & 0xFFull;
            const uint64_t a = (d1 >> 32) & 0xFFull;
            value = r | (g << 8) | (b << 16) | (a << 24) | ((uint64_t)st.q_bits << 32);
            return GS_REG_RGBAQ;
        }
        case GS_REG_ST:
            /* S and T are the register value; Q is latched for the next
             * RGBAQ. */
            st.q_bits = (uint32_t)(d1 & 0xFFFFFFFFull);
            value = d0;
            return GS_REG_ST;
        case GS_REG_UV:
            value = (d0 & 0x3FFFull) | (((d0 >> 32) & 0x3FFFull) << 32);
            return GS_REG_UV;
        case GS_REG_XYZF2: {
            const uint64_t x = d0 & 0xFFFFull;
            const uint64_t y = (d0 >> 32) & 0xFFFFull;
            const uint64_t z = (d1 >> 4) & 0xFFFFFFull;
            const uint64_t f = (d1 >> 36) & 0xFFull;
            value = x | (y << 16) | (z << 32) | (f << 56);
            return ((d1 >> 47) & 1ull) ? GS_REG_XYZF3 : GS_REG_XYZF2;
        }
        case GS_REG_XYZ2: {
            const uint64_t x = d0 & 0xFFFFull;
            const uint64_t y = (d0 >> 32) & 0xFFFFull;
            const uint64_t z = d1 & 0xFFFFFFFFull;
            value = x | (y << 16) | (z << 32);
            return ((d1 >> 47) & 1ull) ? GS_REG_XYZ3 : GS_REG_XYZ2;
        }
        case GS_REG_TEX0_1:
        case GS_REG_TEX0_2:
        case GS_REG_CLAMP_1:
        case GS_REG_CLAMP_2:
            value = d0;
            return desc;
        case GS_REG_FOG:
            /* PACKED FOG carries F at bits 100..107, the same place XYZF2
             * carries it, and the FOG register holds it at bits 56..63. */
            value = ((d1 >> 36) & 0xFFull) << 56;
            return GS_REG_FOG;
        case GS_REG_XYZF3: {
            const uint64_t x = d0 & 0xFFFFull;
            const uint64_t y = (d0 >> 32) & 0xFFFFull;
            const uint64_t z = (d1 >> 4) & 0xFFFFFFull;
            const uint64_t f = (d1 >> 36) & 0xFFull;
            value = x | (y << 16) | (z << 32) | (f << 56);
            return GS_REG_XYZF3;
        }
        case GS_REG_XYZ3: {
            const uint64_t x = d0 & 0xFFFFull;
            const uint64_t y = (d0 >> 32) & 0xFFFFull;
            const uint64_t z = d1 & 0xFFFFFFFFull;
            value = x | (y << 16) | (z << 32);
            return GS_REG_XYZ3;
        }
        case GS_REG_AD:
            /* Address and data: the register is named by the qword itself. */
            value = d0;
            return (uint32_t)(d1 & 0xFFull);
        default:
            /* 0x0B is reserved and 0x0F is NOP; both write nothing. */
            value = 0;
            return GS_REG_NOP;
    }
}

} // namespace detail

/* Decodes one submission. `data` is qwords*16 bytes of GIF-tagged packet
 * data, already framed by hw/gif.cpp. Returns when the submission is
 * exhausted; whatever tag is still open stays open in `st`.
 *
 * A submission that ends inside a tag's payload is normal for PATH3 and an
 * error for the other two paths, which is hw/gif.cpp's judgment to make, not
 * this decoder's. */
template <typename Sink>
void gif_decode(GifDecodeState& st, const uint8_t* data, uint32_t qwords, Sink& sink) {
    uint32_t i = 0;
    while (i < qwords) {
        if (!st.in_tag) {
            const uint64_t lo = detail::load64(data + (size_t)i * 16);
            const uint64_t hi = detail::load64(data + (size_t)i * 16 + 8);
            ++i;
            const uint32_t nloop = (uint32_t)(lo & 0x7FFFull);
            const bool pre = ((lo >> 46) & 1ull) != 0;
            const uint32_t prim = (uint32_t)((lo >> 47) & 0x7FFull);
            st.flg = (uint32_t)((lo >> 58) & 3ull);
            st.nreg = (uint32_t)((lo >> 60) & 15ull);
            if (st.nreg == 0) st.nreg = 16;
            st.regs = hi;
            st.reg_index = 0;
            st.values_left = 0;
            st.image_qw = 0;

            /* PRE applies to PACKED and REGLIST only; the manual states it is
             * ignored for an IMAGE tag. */
            if (pre && st.flg <= 1) {
                /* Synthesized from the tag, not read from a payload word. */
                st.value_off = GifDecodeState::kNoValueOffset;
                sink.reg(GS_REG_PRIM, (uint64_t)prim);
            }

            if (st.flg == 0 || st.flg == 1) {
                st.values_left = nloop * st.nreg;
            } else {
                if (st.flg == 3) {
                    /* FLG 3 is reserved. The hardware moves the payload the
                     * same way an IMAGE tag does, so that is what happens
                     * here, said out loud rather than silently. */
                    sink.note("GIFtag FLG=3 (reserved) treated as IMAGE");
                }
                st.image_qw = nloop;
            }
            st.in_tag = st.values_left != 0 || st.image_qw != 0;
            if (!st.in_tag) continue; /* NLOOP 0: a tag with no payload */
        }

        if (st.flg == 0) { /* PACKED: one qword per register value */
            while (st.values_left != 0 && i < qwords) {
                const uint64_t d0 = detail::load64(data + (size_t)i * 16);
                const uint64_t d1 = detail::load64(data + (size_t)i * 16 + 8);
                ++i;
                const uint32_t desc = (uint32_t)((st.regs >> (4 * st.reg_index)) & 15ull);
                uint64_t value = 0;
                const uint32_t addr = detail::unpack_packed(desc, d0, d1, value, st);
                /* i was advanced past this qword above; d0 is its first
                 * eight bytes. */
                st.value_off = (i - 1) * 16u;
                if (addr != GS_REG_NOP) sink.reg(addr, value);
                --st.values_left;
                if (++st.reg_index == st.nreg) st.reg_index = 0;
            }
        } else if (st.flg == 1) { /* REGLIST: two raw 64-bit values per qword */
            while (st.values_left != 0 && i < qwords) {
                const uint8_t* qw = data + (size_t)i * 16;
                for (int half = 0; half < 2 && st.values_left != 0; ++half) {
                    const uint32_t desc = (uint32_t)((st.regs >> (4 * st.reg_index)) & 15ull);
                    const uint64_t value = detail::load64(qw + half * 8);
                    st.value_off = i * 16u + (uint32_t)half * 8u;
                    /* REGLIST carries register values already in register
                     * layout, so nothing is unpacked. NOP still consumes its
                     * slot. */
                    if (desc != GS_REG_NOP) sink.reg(desc, value);
                    --st.values_left;
                    if (++st.reg_index == st.nreg) st.reg_index = 0;
                }
                ++i; /* an odd value count leaves the high half as padding */
            }
        } else { /* IMAGE / reserved: raw qwords to HWREG */
            const uint32_t take = st.image_qw < (qwords - i) ? st.image_qw : (qwords - i);
            if (take != 0) sink.image(data + (size_t)i * 16, take);
            i += take;
            st.image_qw -= take;
        }

        if (st.values_left == 0 && st.image_qw == 0) st.in_tag = false;
    }
}

} // namespace gsr

#endif /* ICORECOMP_GIF_DECODE_H */
