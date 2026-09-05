/* gs/render/gs_regs.h: the GS register file, both contexts, plus the
 * privileged block.
 *
 * Ours (MIT). Field positions are the GS User's Manual's register diagrams;
 * the privileged register names match src/runtime/mmio.cpp and hw/gspriv.cpp.
 *
 * Storage is deliberately dumb: every general register 0x00 to 0x63 is kept
 * as the raw 64-bit value the GIF wrote, and the decoded views below are
 * functions of that array rather than a parallel set of fields. Two reasons.
 * The register file is state a dump must be able to reproduce exactly, and a
 * decoded copy is one more thing that can disagree with the raw value. And
 * the two drawing contexts do not need a context index: the manual gives
 * context 1 and context 2 registers separate addresses (TEX0_1 is 0x06 and
 * TEX0_2 is 0x07, and so on up to ZBUF_1 0x4E and ZBUF_2 0x4F), so one flat
 * array already holds both, and PRIM's CTXT bit selects which addresses a
 * primitive reads.
 *
 * The privileged registers are a separate 0x12000000-relative shadow, because
 * they arrive through write_priv and not through the GIF.
 */
#ifndef ICORECOMP_GS_REGS_H
#define ICORECOMP_GS_REGS_H

#include "gs_texture.h"

#include <cstdint>
#include <cstring>

namespace gsr {

/* General register addresses the renderer reads by name. The vertex and
 * attribute addresses (PRIM, RGBAQ, ST, UV, XYZ*, FOG) are named in
 * gif_decode.h, which is the file that has to know the PACKED descriptor
 * numbering; these are the rest. Anything not named here is still stored
 * verbatim by the register file. */
enum : uint32_t {
    GS_REG_TEX1_1     = 0x14,
    GS_REG_TEX1_2     = 0x15,
    GS_REG_TEX2_1     = 0x16,
    GS_REG_TEX2_2     = 0x17,
    GS_REG_XYOFFSET_1 = 0x18,
    GS_REG_XYOFFSET_2 = 0x19,
    GS_REG_PRMODECONT = 0x1A,
    GS_REG_PRMODE     = 0x1B,
    GS_REG_TEXCLUT    = 0x1C,
    GS_REG_SCANMSK    = 0x22,
    GS_REG_MIPTBP1_1  = 0x34,
    GS_REG_MIPTBP1_2  = 0x35,
    GS_REG_MIPTBP2_1  = 0x36,
    GS_REG_MIPTBP2_2  = 0x37,
    GS_REG_TEXA       = 0x3B,
    GS_REG_FOGCOL     = 0x3D,
    GS_REG_TEXFLUSH   = 0x3F,
    GS_REG_SCISSOR_1  = 0x40,
    GS_REG_SCISSOR_2  = 0x41,
    GS_REG_ALPHA_1    = 0x42,
    GS_REG_ALPHA_2    = 0x43,
    GS_REG_DIMX       = 0x44,
    GS_REG_DTHE       = 0x45,
    GS_REG_COLCLAMP   = 0x46,
    GS_REG_TEST_1     = 0x47,
    GS_REG_TEST_2     = 0x48,
    GS_REG_PABE       = 0x49,
    GS_REG_FBA_1      = 0x4A,
    GS_REG_FBA_2      = 0x4B,
    GS_REG_FRAME_1    = 0x4C,
    GS_REG_FRAME_2    = 0x4D,
    GS_REG_ZBUF_1     = 0x4E,
    GS_REG_ZBUF_2     = 0x4F,
    GS_REG_BITBLTBUF  = 0x50,
    GS_REG_TRXPOS     = 0x51,
    GS_REG_TRXREG     = 0x52,
    GS_REG_TRXDIR     = 0x53,
    GS_REG_HWREG_ADDR = 0x54,
    GS_REG_SIGNAL     = 0x60,
    GS_REG_FINISH     = 0x61,
    GS_REG_LABEL      = 0x62,
    GS_REG_COUNT      = 0x64, /* 0x00 to 0x63 inclusive */
};

/* Privileged register byte offsets from 0x12000000. Same values mmio.cpp
 * names. */
enum : uint32_t {
    GS_PRIV_PMODE    = 0x0000,
    GS_PRIV_SMODE1   = 0x0010,
    GS_PRIV_SMODE2   = 0x0020,
    GS_PRIV_SRFSH    = 0x0030,
    GS_PRIV_SYNCH1   = 0x0040,
    GS_PRIV_SYNCH2   = 0x0050,
    GS_PRIV_SYNCV    = 0x0060,
    GS_PRIV_DISPFB1  = 0x0070,
    GS_PRIV_DISPLAY1 = 0x0080,
    GS_PRIV_DISPFB2  = 0x0090,
    GS_PRIV_DISPLAY2 = 0x00A0,
    GS_PRIV_EXTBUF   = 0x00B0,
    GS_PRIV_EXTDATA  = 0x00C0,
    GS_PRIV_EXTWRITE = 0x00D0,
    GS_PRIV_BGCOLOR  = 0x00E0,
    GS_PRIV_CSR      = 0x1000,
    GS_PRIV_IMR      = 0x1010,
    GS_PRIV_BUSDIR   = 0x1040,
    GS_PRIV_SIGLBLID = 0x1080,
};

/* ---- decoded views --------------------------------------------------------
 *
 * Each is a plain function of one register value, so a caller reads the raw
 * value from the file and decodes it where it needs it. */

struct Bitbltbuf {
    uint32_t sbp;  /* source base, 256-byte blocks */
    uint32_t sbw;  /* source width, 64-pixel units */
    uint32_t spsm;
    uint32_t dbp;
    uint32_t dbw;
    uint32_t dpsm;
};

inline Bitbltbuf decode_bitbltbuf(uint64_t v) {
    Bitbltbuf b{};
    b.sbp  = (uint32_t)(v & 0x3FFFu);
    b.sbw  = (uint32_t)((v >> 16) & 0x3Fu);
    b.spsm = (uint32_t)((v >> 24) & 0x3Fu);
    b.dbp  = (uint32_t)((v >> 32) & 0x3FFFu);
    b.dbw  = (uint32_t)((v >> 48) & 0x3Fu);
    b.dpsm = (uint32_t)((v >> 56) & 0x3Fu);
    return b;
}

struct Trxpos {
    uint32_t ssax, ssay, dsax, dsay;
    uint32_t dir; /* 0 upper-left to lower-right, 1 lower-left to upper-right,
                   * 2 upper-right to lower-left, 3 lower-right to upper-left.
                   * gs_vram.cpp reverses x for 2 and 3 and y for 1 and 3,
                   * which is these four corners. */
};

inline Trxpos decode_trxpos(uint64_t v) {
    Trxpos t{};
    t.ssax = (uint32_t)(v & 0x7FFu);
    t.ssay = (uint32_t)((v >> 16) & 0x7FFu);
    t.dsax = (uint32_t)((v >> 32) & 0x7FFu);
    t.dsay = (uint32_t)((v >> 48) & 0x7FFu);
    t.dir  = (uint32_t)((v >> 59) & 3u);
    return t;
}

struct Trxreg {
    uint32_t rrw, rrh; /* transfer area in pixels */
};

inline Trxreg decode_trxreg(uint64_t v) {
    Trxreg t{};
    t.rrw = (uint32_t)(v & 0xFFFu);
    t.rrh = (uint32_t)((v >> 32) & 0xFFFu);
    return t;
}

/* TRXDIR XDIR: 0 host to local, 1 local to host, 2 local to local, 3 stop. */
enum : uint32_t {
    GS_XDIR_HOST_TO_LOCAL  = 0,
    GS_XDIR_LOCAL_TO_HOST  = 1,
    GS_XDIR_LOCAL_TO_LOCAL = 2,
    GS_XDIR_STOP           = 3,
};

struct Pmode {
    uint32_t en1, en2, crtmd, mmod, amod, slbg, alp;
};

inline Pmode decode_pmode(uint64_t v) {
    Pmode p{};
    p.en1   = (uint32_t)(v & 1u);
    p.en2   = (uint32_t)((v >> 1) & 1u);
    p.crtmd = (uint32_t)((v >> 2) & 7u);
    p.mmod  = (uint32_t)((v >> 5) & 1u);
    p.amod  = (uint32_t)((v >> 6) & 1u);
    p.slbg  = (uint32_t)((v >> 7) & 1u);
    p.alp   = (uint32_t)((v >> 8) & 0xFFu);
    return p;
}

struct Smode1 {
    uint32_t lc;   /* PLL loop divider; 32 is the analog video clock */
    uint32_t cmod; /* 0 progressive/VESA, 2 NTSC, 3 PAL */
};

inline Smode1 decode_smode1(uint64_t v) {
    Smode1 s{};
    s.lc   = (uint32_t)((v >> 3) & 0x7Fu);
    s.cmod = (uint32_t)((v >> 13) & 3u);
    return s;
}

struct Smode2 {
    uint32_t interlaced; /* INT */
    uint32_t ffmd;       /* 0 FIELD mode, 1 FRAME mode */
    uint32_t dpms;
};

inline Smode2 decode_smode2(uint64_t v) {
    Smode2 s{};
    s.interlaced = (uint32_t)(v & 1u);
    s.ffmd       = (uint32_t)((v >> 1) & 1u);
    s.dpms       = (uint32_t)((v >> 2) & 3u);
    return s;
}

struct Dispfb {
    uint32_t fbp; /* base, in pages of 2048 words */
    uint32_t fbw; /* width, 64-pixel units */
    uint32_t psm;
    uint32_t dbx, dby;
};

inline Dispfb decode_dispfb(uint64_t v) {
    Dispfb d{};
    d.fbp = (uint32_t)(v & 0x1FFu);
    d.fbw = (uint32_t)((v >> 9) & 0x3Fu);
    d.psm = (uint32_t)((v >> 15) & 0x1Fu);
    d.dbx = (uint32_t)((v >> 32) & 0x7FFu);
    d.dby = (uint32_t)((v >> 43) & 0x7FFu);
    return d;
}

struct Display {
    uint32_t dx, dy;     /* display position, in VCK units and raster lines */
    uint32_t magh, magv; /* magnification minus one */
    uint32_t dw, dh;     /* display area minus one, in VCK units and lines */
};

inline Display decode_display(uint64_t v) {
    Display d{};
    d.dx   = (uint32_t)(v & 0xFFFu);
    d.dy   = (uint32_t)((v >> 12) & 0x7FFu);
    d.magh = (uint32_t)((v >> 23) & 0xFu);
    d.magv = (uint32_t)((v >> 27) & 3u);
    d.dw   = (uint32_t)((v >> 32) & 0xFFFu);
    d.dh   = (uint32_t)((v >> 44) & 0x7FFu);
    return d;
}

/* TEX2 is not a register of its own as far as the texture unit is concerned:
 * the manual defines it as a write that updates only PSM, CBP, CPSM, CSM,
 * CSA and CLD of the TEX0 of the same context and leaves TBP0, TBW, TW, TH,
 * TCC and TFX standing. So a TEX2 write is applied by merging those fields
 * into the TEX0 value, and everything downstream reads TEX0 alone. The two
 * field masks are gs_texture.h's, written once beside the accessors that
 * take them apart. */
inline uint64_t apply_tex2(uint64_t tex0, uint64_t tex2) {
    const uint64_t mask = (uint64_t)GS_TEX2_MASK_LO
                        | ((uint64_t)GS_TEX2_MASK_HI << 32);
    return (tex0 & ~mask) | (tex2 & mask);
}

/* ---- drawing registers ----------------------------------------------------
 *
 * The drawing side of the file: the registers the rasteriser reads once per
 * primitive or once per batch. Same rule as above, one plain function per
 * register, no cached copy.
 */

struct Prim {
    uint32_t prim; /* 0 point, 1 line, 2 line strip, 3 triangle, 4 triangle
                    * strip, 5 triangle fan, 6 sprite, 7 reserved */
    uint32_t iip, tme, fge, abe, aa1, fst, ctxt, fix;
};

/* PRIM and PRMODE hold the same attribute bits in the same places; PRMODE
 * has no PRIM field, so one decoder serves both and the caller says
 * which primitive type applies. That is the whole of PRMODECONT: AC 1 takes
 * the attributes from PRIM, AC 0 takes them from PRMODE, and the primitive
 * type always comes from the last PRIM write either way. */
inline Prim decode_prim(uint64_t v) {
    Prim p{};
    p.prim = (uint32_t)(v & 7u);
    p.iip  = (uint32_t)((v >> 3) & 1u);
    p.tme  = (uint32_t)((v >> 4) & 1u);
    p.fge  = (uint32_t)((v >> 5) & 1u);
    p.abe  = (uint32_t)((v >> 6) & 1u);
    p.aa1  = (uint32_t)((v >> 7) & 1u);
    p.fst  = (uint32_t)((v >> 8) & 1u);
    p.ctxt = (uint32_t)((v >> 9) & 1u);
    p.fix  = (uint32_t)((v >> 10) & 1u);
    return p;
}

struct Frame {
    uint32_t fbp;   /* base, in pages of 2048 words */
    uint32_t fbw;   /* width, 64-pixel units */
    uint32_t psm;
    uint32_t fbmsk; /* set bits are not written */
};

inline Frame decode_frame(uint64_t v) {
    Frame f{};
    f.fbp   = (uint32_t)(v & 0x1FFu);
    f.fbw   = (uint32_t)((v >> 16) & 0x3Fu);
    f.psm   = (uint32_t)((v >> 24) & 0x3Fu);
    f.fbmsk = (uint32_t)((v >> 32) & 0xFFFFFFFFull);
    return f;
}

struct Zbuf {
    uint32_t zbp;  /* base, in pages of 2048 words */
    uint32_t psm;  /* the full PSM code, 0x30 plus the register's 4-bit field */
    uint32_t zmsk; /* 1 means Z is not written */
};

/* ZBUF's PSM field is four bits and names only the Z formats, so the code
 * the swizzle wants is 0x30 plus it: 0 is PSMZ32, 1 PSMZ24, 2 PSMZ16 and
 * 10 PSMZ16S. */
inline Zbuf decode_zbuf(uint64_t v) {
    Zbuf z{};
    z.zbp  = (uint32_t)(v & 0x1FFu);
    z.psm  = 0x30u | (uint32_t)((v >> 24) & 0xFu);
    z.zmsk = (uint32_t)((v >> 32) & 1u);
    return z;
}

struct Scissor {
    uint32_t x0, x1, y0, y1; /* all inclusive */
};

inline Scissor decode_scissor(uint64_t v) {
    Scissor s{};
    s.x0 = (uint32_t)(v & 0x7FFu);
    s.x1 = (uint32_t)((v >> 16) & 0x7FFu);
    s.y0 = (uint32_t)((v >> 32) & 0x7FFu);
    s.y1 = (uint32_t)((v >> 48) & 0x7FFu);
    return s;
}

struct Xyoffset {
    int32_t ofx, ofy; /* 12.4, subtracted from every vertex */
};

inline Xyoffset decode_xyoffset(uint64_t v) {
    Xyoffset o{};
    o.ofx = (int32_t)(uint32_t)(v & 0xFFFFu);
    o.ofy = (int32_t)(uint32_t)((v >> 32) & 0xFFFFu);
    return o;
}

/* ---- the file ------------------------------------------------------------ */

struct RegisterFile {
    /* Every general register the GIF can address, raw. Both contexts live
     * here; see the file comment. */
    uint64_t reg[GS_REG_COUNT] = {};
    /* 0x12000000 privileged block, one entry per 16-byte slot, low 8 bytes
     * only, the same shadow shape gs_dumpwriter.cpp keeps. */
    uint64_t priv_lo[0x100] = {};
    uint64_t priv_hi[0x100] = {};

    void write(uint32_t addr, uint64_t v) {
        if (addr < GS_REG_COUNT) reg[addr] = v;
    }
    uint64_t read(uint32_t addr) const {
        return addr < GS_REG_COUNT ? reg[addr] : 0;
    }

    void write_priv(uint32_t offset, uint64_t v) {
        offset &= 0x1FFF;
        if (offset < 0x1000) priv_lo[offset >> 4] = v;
        else priv_hi[(offset - 0x1000) >> 4] = v;
    }
    uint64_t read_priv(uint32_t offset) const {
        offset &= 0x1FFF;
        if (offset < 0x1000) return priv_lo[offset >> 4];
        return priv_hi[(offset - 0x1000) >> 4];
    }
};

} // namespace gsr

#endif /* ICORECOMP_GS_REGS_H */
