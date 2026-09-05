/* gs/render/gs_vram.cpp: the TRXDIR transfer engine.
 *
 * Ours (MIT). See gs_vram.h for what the class holds and why the host copy
 * is the authority in this milestone.
 *
 * The three directions, as the GS User's Manual describes them:
 *
 *   HOST to LOCAL   the GIF's HWREG image stream fills a RRW x RRH rectangle
 *                   at (DSAX, DSAY) in DBP/DBW/DPSM, in raster order. The
 *                   stream is a bit stream in DPSM's own transfer width,
 *                   least significant bits first inside each byte and bytes
 *                   in order, so a PSMCT24 pixel takes three bytes and one
 *                   PSMT4 byte carries two pixels with the left one in the
 *                   low nibble. Pixels straddle qword boundaries for the
 *                   widths that do not divide 128, which is why the engine
 *                   carries leftover bits between packets.
 *
 *   LOCAL to LOCAL  the same rectangle copied from (SSAX, SSAY) in
 *                   SBP/SBW/SPSM. TRXPOS DIR gives the scan order, which
 *                   only matters when source and destination overlap, and
 *                   they do overlap in real content, so all four orders are
 *                   implemented rather than assumed to be order 0.
 *
 *   LOCAL to HOST   the same rectangle packed back into a byte stream in
 *                   SPSM's transfer width.
 */
#include "gs_vram.h"

#include "../../runtime.h"

#include <cinttypes>

namespace gsr {

namespace {

/* One "say it once" gate per condition, so a transfer the renderer cannot do
 * is loud the first time and does not flood a log at 60 fields a second. */
bool g_logged_unsupported_psm = false;
bool g_logged_host_dir = false;
bool g_logged_stop = false;
bool g_logged_overrun = false;
bool g_logged_psm_mismatch = false;
bool g_logged_local_to_host = false;

} // namespace

void TransferEngine::trxdir(uint64_t value, const RegisterFile& regs, LocalMemory& mem) {
    const uint32_t xdir = (uint32_t)(value & 3u);

    if (xdir == GS_XDIR_STOP) {
        /* A stop while a host stream is still running means the game
         * abandoned the transfer. Nothing is written; say so, because the
         * picture that follows will be missing whatever it was uploading. */
        if (m_active && !g_logged_stop) {
            g_logged_stop = true;
            rt_log_warn("gsr", "TRXDIR stop with %" PRIu64 " of %u pixels transferred; "
                               "the rest of that upload never arrives",
                        m_pixel, m_reg.rrw * m_reg.rrh);
        }
        m_active = false;
        m_mode = GS_XDIR_STOP;
        m_bit_carry_bits = 0;
        m_bit_carry_value = 0;
        return;
    }

    m_buf = decode_bitbltbuf(regs.read(GS_REG_BITBLTBUF));
    m_pos = decode_trxpos(regs.read(GS_REG_TRXPOS));
    m_reg = decode_trxreg(regs.read(GS_REG_TRXREG));
    m_mode = xdir;
    m_pixel = 0;
    m_bit_carry_bits = 0;
    m_bit_carry_value = 0;
    m_active = m_reg.rrw != 0 && m_reg.rrh != 0;

    if (!m_active) {
        /* A zero-area transfer is legal and moves nothing. */
        return;
    }

    if (xdir == GS_XDIR_LOCAL_TO_LOCAL) {
        local_to_local(mem);
        m_active = false;
    } else if (xdir == GS_XDIR_LOCAL_TO_HOST) {
        local_to_host(mem);
        m_active = false;
    } else {
        if (m_pos.dir != 0 && !g_logged_host_dir) {
            g_logged_host_dir = true;
            rt_log_warn("gsr", "host-to-local transfer with TRXPOS DIR %u; the manual "
                               "defines only upper-left to lower-right for this "
                               "direction, and that is the order used",
                        m_pos.dir);
        }
    }
}

uint32_t TransferEngine::host_to_local_data(const uint8_t* data, uint32_t qwords,
                                            LocalMemory& mem) {
    if (!armed_host_to_local()) {
        overrun_qwords += qwords;
        if (!g_logged_overrun) {
            g_logged_overrun = true;
            rt_log_warn("gsr", "GIF image data with no host-to-local transfer armed "
                               "(%u qwords); it is discarded", qwords);
        }
        return qwords;
    }

    const uint32_t bits = gs_transfer_bits(m_buf.dpsm);
    if (bits == 0) {
        ++unsupported_psm;
        if (!g_logged_unsupported_psm) {
            g_logged_unsupported_psm = true;
            rt_log_warn("gsr", "host-to-local transfer to PSM 0x%02x, which has no "
                               "defined pixel width; the upload is discarded",
                        m_buf.dpsm);
        }
        m_active = false;
        return qwords;
    }

    const uint64_t total = (uint64_t)m_reg.rrw * m_reg.rrh;
    const uint32_t mask = bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    const uint32_t base_block = m_buf.dbp;

    for (uint32_t qw = 0; qw < qwords; ++qw) {
        const uint8_t* p = data + (size_t)qw * 16;
        for (uint32_t b = 0; b < 16; ++b) {
            m_bit_carry_value |= (uint64_t)p[b] << m_bit_carry_bits;
            m_bit_carry_bits += 8;
            while (m_bit_carry_bits >= bits && m_pixel < total) {
                const uint32_t v = (uint32_t)(m_bit_carry_value & mask);
                m_bit_carry_value >>= bits;
                m_bit_carry_bits -= bits;
                const uint32_t x = m_pos.dsax + (uint32_t)(m_pixel % m_reg.rrw);
                const uint32_t y = m_pos.dsay + (uint32_t)(m_pixel / m_reg.rrw);
                mem.write_pixel(m_buf.dpsm, base_block, m_buf.dbw, x, y, v);
                ++m_pixel;
                ++host_to_local_pixels;
            }
            if (m_pixel >= total) break;
        }
        if (m_pixel >= total) {
            /* The rest of this qword is padding: the GIF moves whole qwords,
             * so a transfer whose pixel count does not fill one ends with
             * bits nothing reads. */
            m_active = false;
            m_bit_carry_bits = 0;
            m_bit_carry_value = 0;
            return qw + 1;
        }
    }
    return qwords;
}

void TransferEngine::local_to_local(LocalMemory& mem) {
    if (gs_transfer_bits(m_buf.spsm) == 0 || gs_transfer_bits(m_buf.dpsm) == 0) {
        ++unsupported_psm;
        if (!g_logged_unsupported_psm) {
            g_logged_unsupported_psm = true;
            rt_log_warn("gsr", "local-to-local transfer between PSM 0x%02x and 0x%02x, "
                               "one of which has no defined pixel width; nothing is copied",
                        m_buf.spsm, m_buf.dpsm);
        }
        return;
    }
    if (m_buf.spsm != m_buf.dpsm && !g_logged_psm_mismatch) {
        /* The manual requires the two formats to match for a local-to-local
         * transfer. The copy below moves stored values, which is the right
         * answer when they do match and is the honest thing to report when
         * they do not, rather than inventing a conversion the hardware has
         * no circuit for. */
        g_logged_psm_mismatch = true;
        rt_log_warn("gsr", "local-to-local transfer with SPSM 0x%02x != DPSM 0x%02x; "
                           "the stored values are copied without conversion",
                    m_buf.spsm, m_buf.dpsm);
    }

    const bool rev_x = m_pos.dir == 2 || m_pos.dir == 3;
    const bool rev_y = m_pos.dir == 1 || m_pos.dir == 3;
    for (uint32_t j = 0; j < m_reg.rrh; ++j) {
        const uint32_t dy = rev_y ? (m_reg.rrh - 1 - j) : j;
        for (uint32_t i = 0; i < m_reg.rrw; ++i) {
            const uint32_t dx = rev_x ? (m_reg.rrw - 1 - i) : i;
            const uint32_t v = mem.read_pixel(m_buf.spsm, m_buf.sbp, m_buf.sbw,
                                              m_pos.ssax + dx, m_pos.ssay + dy);
            mem.write_pixel(m_buf.dpsm, m_buf.dbp, m_buf.dbw,
                            m_pos.dsax + dx, m_pos.dsay + dy, v);
            ++local_to_local_pixels;
        }
    }
}

void TransferEngine::local_to_host(LocalMemory& mem) {
    /* Nothing is read out of local memory while the direction has no reader.
     * The parameter stays because a GS FIFO reader is what would use it. */
    (void)mem;
    const uint32_t bits = gs_transfer_bits(m_buf.spsm);
    if (bits == 0) {
        ++unsupported_psm;
        if (!g_logged_unsupported_psm) {
            g_logged_unsupported_psm = true;
            rt_log_warn("gsr", "local-to-host transfer from PSM 0x%02x, which has no "
                               "defined pixel width; nothing is read back", m_buf.spsm);
        }
        return;
    }

    /* The pixels are counted and nothing is kept. The bytes would leave the
     * GS through the FIFO at 0x12001000 and this runtime has no reader for
     * it, so there is nowhere to deliver them to; accumulating them against a
     * caller that does not exist grew without bound and still answered the
     * guest's read with nothing. gs_vram.h says the whole of it.
     *
     * The transfer is still a wrong picture waiting to happen, so it is said
     * out loud once rather than only appearing as a counter in the
     * end-of-run report. */
    local_to_host_pixels += (uint64_t)m_reg.rrw * m_reg.rrh;
    if (!g_logged_local_to_host) {
        g_logged_local_to_host = true;
        rt_log_warn("gsr", "LOCAL to HOST transfer of %ux%u from PSM 0x%02x: this runtime "
                           "has no GS FIFO reader, so the pixels are counted and not "
                           "delivered to the guest",
                    m_reg.rrw, m_reg.rrh, m_buf.spsm);
    }
}

} // namespace gsr
