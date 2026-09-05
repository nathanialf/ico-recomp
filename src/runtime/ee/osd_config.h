/* ee/osd_config.h: the console OSD configuration word this kernel reports,
 * and the readers the game's vendor library applies to it.
 *
 * PAL-only surface. Measured on the two retail ELFs: the PAL build
 * (SCES_507.60) calls the GetOsdConfigParam stub at 0x001005D0 from eight
 * sites inside its libscf object, one of which is reachable; the US build
 * (SCUS_971.13) calls it from none. So the console language only reaches
 * this game on the PAL disc.
 *
 * Every accessor below is the vendor library's own arithmetic, so the
 * selftest can check the word this runtime hands back against the code that
 * reads it rather than against an SDK header. Each address below was read
 * out of the retail ELF (SCES_507.60) and holds the instruction named;
 * sceScfGetLanguage itself begins at 0x00272958, the target of
 * kanbanBootMcCheck's jal at 0x001B9614:
 *
 *   version    (word >> 13) & 7    sceScfGetLanguage 0x0027298C srl $v0,$v1,13
 *   language   (word >> 16) & 0x1F sceScfGetLanguage 0x002729A8 andi $v0,$v0,0x1f
 *                                                    (version != 0)
 *   language0  (word >> 4) & 1     sceScfGetLanguage 0x0027299C srl $v0,$v1,4
 *                                                    (version == 0)
 *   aspect     (word >> 1) & 3     sceScfGetAspect   0x00272A04 srl $v0,$v0,1
 *
 * This is runtime-internal, NOT part of the ABI contract.
 */
#ifndef ICORECOMP_EE_OSD_CONFIG_H
#define ICORECOMP_EE_OSD_CONFIG_H

#include <cstdint>

/* The OSD word GetOsdConfigParam writes. Version 1 because the language
 * field the game wants only exists for a nonzero version: at version 0 the
 * library reads a one-bit Japanese/English flag that cannot express the four
 * other languages this disc carries. Every other field is zero; nothing in
 * this build reads one. */
static inline uint32_t rt_osd_config_word(int language) {
    return (1u << 13) | ((uint32_t)(language & 0x1F) << 16);
}

static inline uint32_t rt_osd_cfg_version(uint32_t word) { return (word >> 13) & 7u; }
static inline uint32_t rt_osd_cfg_language(uint32_t word) { return (word >> 16) & 0x1Fu; }
static inline uint32_t rt_osd_cfg_language_v0(uint32_t word) { return (word >> 4) & 1u; }
static inline uint32_t rt_osd_cfg_aspect(uint32_t word) { return (word >> 1) & 3u; }

/* kanbanBootMcCheck's own test on the value sceScfGetLanguage returned:
 * `addiu $a0, $v0, -1` at 0x001B961C then `sltiu $v1, $a0, 5` at
 * 0x001B9624, so a language of 1..5 indexes the five-entry jump table at
 * 0x0061D6F0 and anything else leaves the compiled-in default in place.
 * Both words read out of the retail ELF (2444ffff, 2c830005); the earlier
 * comment put the addiu at 0x001B9618, which is the sw in the jal's shadow.
 * The jump table's own address is INFERRED, carried from the note this
 * comment replaces and not re-read out of the ELF here; only the 1..5 test
 * this function implements is measured. */
static inline bool rt_osd_language_selects_subtitles(uint32_t language) {
    return (uint32_t)(language - 1u) < 5u;
}

#endif /* ICORECOMP_EE_OSD_CONFIG_H */
