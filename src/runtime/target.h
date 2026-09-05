/* target.h: the retail disc this port is built for.
 *
 * One build serves one target, because the generated C is a translation of
 * one ELF. That target is SCES_507.60, the European retail disc (SCES-50760).
 * Every use of the name goes through the macros below, so no other file
 * spells the boot file's name or its pin in code. Three comments elsewhere
 * name it in prose (runtime.h and iso/iso9660.h), which is description, not
 * a second definition.
 *
 * These are address facts and file names, the class of fact CLAUDE.md
 * allows in the tree (an address paired with what lives at it, the boot
 * file's name, and the boot ELF SHA-1 pin that the hard rules name as an
 * approved exception). No game bytes, no symbol lists, no disassembly, no
 * content hashes beyond the pin.
 *
 * Provenance. Measured on 2026-09-04 from the retail disc: the SHA-1 of the
 * boot ELF as it sits on the disc, e_entry from its ELF header, and its
 * single PT_LOAD's p_vaddr. The gp comes from the ELF's .reginfo
 * ri_gp_value and is cross-checked against the pair of instructions crt0
 * forms it with, at 0x00100034 and 0x00100048, and the register move at
 * 0x0010005C. It is the value crt0 also hands RFU060, and it has to agree
 * with [target].gp in config/recomp.toml; the loader prefers the config file
 * whenever there is one.
 */
#ifndef ICORECOMP_TARGET_H
#define ICORECOMP_TARGET_H

#include <cstdint>

#define RT_TARGET_NAME "pal"
#define RT_TARGET_REGION "PAL (SCES-50760)"
#define RT_TARGET_BOOT_ELF "SCES_507.60"
#define RT_TARGET_ELF_SHA1 "da3644c54c26fe760f3b6a591a5fc2eab396ed2b"
constexpr uint32_t RT_TARGET_ENTRY = 0x00100008;
constexpr uint32_t RT_TARGET_VRAM_BASE = 0x00100000;
constexpr uint32_t RT_TARGET_GP = 0x00640AF0;

/* The committed config file that carries the input paths ([inputs]: the
 * boot ELF and the disc's own objdump listing), the SHA-1 pin and
 * the target words. A packaged run ships no config file and takes the
 * constants above; a source tree has the file and it wins. */
#define RT_TARGET_CONFIG_TOML "recomp.toml"

/* "\\SCES_507.60;1", the ISO 9660 path rt_iso_search takes. Built here so
 * the two callers cannot spell it differently. */
#define RT_TARGET_BOOT_ELF_ISO_PATH "\\" RT_TARGET_BOOT_ELF ";1"

#endif /* ICORECOMP_TARGET_H */
