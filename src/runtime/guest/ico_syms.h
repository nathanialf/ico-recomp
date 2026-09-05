/* guest/ico_syms.h: guest RAM addresses of the retail ICO ELF's menu state,
 * of its projection block, and of the vendor sound library's voice state.
 *
 * These are address facts about the retail ELF, the same class of fact as
 * config/entry_hooks.txt (an address paired with what lives at it).
 * Address facts are the one thing this header may contain: no code, no game
 * bytes, no content hashes. See the hard rules in CLAUDE.md.
 *
 * All of these are little-endian values in EE RAM, 32-bit words except where
 * the sound block says otherwise. The runtime reads
 * all of them; the mouse pointer writes two, the current-item field of the
 * layout table entry (and its per-frame mirror) and the screen adjustment
 * screen's own bar position. guest/menu_nav.cpp says why those two words are
 * the whole of the write, and how the scene object fields below turn into
 * a rectangle on the presented picture.
 */
#ifndef ICORECOMP_GUEST_ICO_SYMS_H
#define ICORECOMP_GUEST_ICO_SYMS_H

#include <cstdint>

/* ---- where these addresses come from -------------------------------------
 *
 * Every address below is an address in SCES_507.60, established on
 * 2026-09-04 by five methods, named per constant:
 *
 *   correlation  this build's .text was correlated instruction by
 *                instruction against an earlier reading of the same game's
 *                code, and the address recorded is the one every code site
 *                that materializes the datum agreed on. The count that
 *                follows is that agreement: "4/4 sites" means four sites out
 *                of four. A 1/1 answer is still a measurement, but it rests
 *                on one instruction. The translator subcommand that
 *                performed this correlation was removed together with the
 *                other build, so the counts below are the record of a
 *                measurement that cannot now be re-run from this
 *                repository.
 *   RA           the RetroAchievements set for this disc (game 1319), its
 *                r=patch conditions and r=codenotes2 notes, retrieved
 *                2026-09-04. Those addresses were measured on this build by
 *                that community.
 *   code         the instruction itself, decoded out of SCES_507.60. This
 *                build's .text is searched for the masked instruction
 *                window, with the mask
 *                tools/recomp/crates/ingest/src/correlate.rs uses (j, jal,
 *                lui, addi, addiu, ori and every load and store lose their
 *                immediate; branch displacements are kept), and the
 *                instruction at the matching position is decoded. "11 of
 *                11" means eleven sites form the address and all eleven
 *                agree.
 *   listing      the disc's own objdump listing, SRCFILE.TXT, correlated
 *                onto SCES_507.60. It is a different link of the same
 *                source, so a function is matched by comparing its words
 *                against the retail bytes with the relocatable fields
 *                masked out (jump targets, gp displacements, and the
 *                immediate of every lui, addiu, load and store), and the
 *                match is quoted with its word count. What the listing then
 *                supplies is the function's own name and, through the
 *                source line it prints, which source file it came from. No
 *                address is taken from it: every address below is decoded
 *                out of the retail words.
 *   decomp       a listing in the decomp of this game, read as a
 *                behavioural reference the way PCSX2 is and never as the
 *                deciding source (CLAUDE.md: the binary is the authority).
 *                Every entry that cites it says so and says what the retail
 *                ELF or the disc listing independently gives for the same
 *                fact.
 *
 * Where two of them name the same word they agree, and that is said where it
 * happens: a second independent measurement of the same word is the
 * strongest kind of fact in this header.
 *
 * No address here is another build's address plus a delta, and no sentinel
 * was replaced by a plausible value. A value carried from the decomp's
 * structure rather than measured on this build is an inference, not a
 * measurement, and says so where it is used.
 *
 * Four features read guest memory, and each has its own gate below, because
 * what is resolved differs per feature. A feature whose gate is 0 logs one
 * warn naming what is missing and does nothing for the rest of the run.
 *
 *   RT_ICO_MENU_NAV_KNOWN      guest/menu_nav.cpp, the mouse pointer
 *   RT_ICO_WIDESCREEN_KNOWN    guest/widescreen.cpp, display.widescreen
 *   RT_ICO_ACHIEVEMENTS_KNOWN  guest/achievements.cpp, the observer
 *   RT_ICO_SND_FACTORS_KNOWN   snd/engine.cpp, one diagnostic, read only
 */
/* On since 2026-09-04: RT_ICO_NAV_SWALLOW is measured (11 of 11 sites) and
 * every layout and scene object offset the pointer walks was read off this
 * build's own code rather than carried from the decomp's structure. The
 * scene object is 0x70 bytes here and its fields from +0x24 up sit four
 * bytes higher than the decomp's structure has them, so carrying them would
 * have put the highlight on the wrong item. The two PAL boot screens joined
 * it on the same date, through the kanban block below; they need no address
 * this gate did not already cover except that block's own. */
#define RT_ICO_MENU_NAV_KNOWN 1
#define RT_ICO_WIDESCREEN_KNOWN 1
#define RT_ICO_ACHIEVEMENTS_KNOWN 1

/* An address, offset or id that has not been read off the retail ELF.
 * No address is ever this, so a read of it cannot be mistaken for a real
 * one, and rt_gptr has no page for it, so a read through one fails rather
 * than landing on unrelated memory. */
constexpr uint32_t RT_ICO_SYM_UNKNOWN = 0xFFFFFFFFu;

/* Structure offsets are marked where they are used. Every offset the menu
 * pointer walks was read off this build's own code on 2026-09-04 (the
 * layout entry's fields and count, the scene object's stride and fields,
 * the screen adjustment screen's first object), and several of them differ
 * from the decomp's structure.
 *
 * ---- on the function names in this block ----------------------------------
 *
 * The names below are the disc's own listing's, correlated onto the retail
 * ELF on 2026-09-05 by the `listing` method above. Each pairing is one
 * function of SRCFILE.TXT matched against the retail bytes at the address
 * given, with every relocatable field masked out, and the word count and
 * the mismatch count are quoted so the match can be re-run:
 *
 *   retail      disc        listing name                     words, mismatches
 *   0x001BF220  0x001C23B8  default_item_select              252, 0
 *   0x001BF610  0x001C27A8  texture_fading                   212, 0
 *   0x001BF960  0x001C2AF8  display_texture                  668, 0
 *   0x001C03E0  0x001C3578  display_primary_texture_layout   186, 0
 *   0x001C06C8  0x001C3860  exec_layout_texture              184, 0
 *   0x001C0F58  0x001C40F0  lt_default_mask_property          14, 0
 *   0x001BAC08  0x001BDDA0  la_mc_file_select                160, 0
 *   0x001BE058  0x001C11F0  la_adjust_screen                  78, 0
 *
 * An earlier reading of this file used names of its own for the first six
 * of those (lt_switch_layout, lt_link_layout, lt_prev_layout,
 * lt_next_layout, GetRealModelId, _la_set_preview_info) and named the
 * eighth `_la_memory_card_check`, which it is not. The first six are kept
 * in the prose of guest/menu_nav.cpp, which reads them as roles and names
 * the listing's symbol beside each one where it first uses it; the table
 * above is what a reader should hold a symbol against. The eighth was
 * wrong about the code and not only about the spelling: see
 * RT_ICO_BRIGHTNESS below. */

/* correlation: 4/4 sites. */
constexpr uint32_t RT_ICO_LAYOUT_ID = 0x0063B60C;
/* correlation: 5/5 sites. */
constexpr uint32_t RT_ICO_ITEM_ID = 0x0063B610;
/* correlation: 1/1 site. */
constexpr uint32_t RT_ICO_FADE_STATE = 0x0063B618;
/* code: 11 of 11 sites, 2026-09-04. Correlation reports no instruction
 * materializing this address, for a reason that turned out to be about the
 * method and not about the word: the word is reached $gp-relative, which
 * correlation does handle, but not one of the eleven functions that touch it
 * correlates whole, and only sites inside a correlated function are counted.
 *
 * Read off the code instead. This build's .text has exactly eleven
 * $gp-relative accesses forming 0x0063B620 (gp 0x00640AF0, displacement
 * -0x54D0). Six of them pair instruction for instruction by masked window
 * with the earlier reading of the same code: 0x001BA8E4, 0x001BA908,
 * 0x001BAADC, 0x001C083C, 0x001C088C and 0x001C099C. The other five are
 * read out of this build's own functions, which the fade state pins
 * independently: 0x001BF268 is lt_switch_layout's opening read, with the
 * read of 0x0063B618 two instructions later; 0x001BF8E4 is
 * lt_current_property_item's store, followed by the store of 2 to
 * 0x0063B618; 0x001BFA84 is lt_link_layout's read, again followed by
 * 0x0063B618; 0x001BAC60 is _la_set_preview_info's store of 1 in a branch
 * delay slot, after three instructions of literals; 0x001BAAF4 is
 * kanbanBoot's second store, 0x18 past the first. 0x0063B618 was measured by
 * correlation (1/1 site), so the reading rests on a word that was resolved
 * another way.
 *
 * It also lands where the object's shape says it should: 0x0063B620 is the
 * layout id (0x0063B60C) plus 0x14, which is the displacement between those
 * two words in the decomp's own structure. That was the hypothesis; the
 * eleven sites are the measurement. */
constexpr uint32_t RT_ICO_NAV_SWALLOW = 0x0063B620;
/* 0x0028F4EC, the screen adjustment level. One word with two readings that
 * agree, so it is one constant; it used to be two, RT_ICO_MC_SELECT here
 * and RT_ICO_BRIGHTNESS in the boot trace block, which said different
 * things about the same address.
 *
 * correlation: 2/2 sites for the word itself, and 143/143 sites for the
 * base 0x0028F4C0 it sits 0x2C past. RA independently calls 0x0028F4C0 the
 * NTSC/PAL mode word, which is what RT_ICO_VIDEO_MODE below uses: two
 * sources, one address.
 *
 * code, and what settles what the word is. `gsb_controlBrightness`
 * (retail 0x001136B0; listing 0x001135A0, GsBase.c:1138, 82 words and no
 * masked mismatch) reads it every frame: it builds 0x0028F4C0 at
 * 0x001136C0 (`lui $2, 0x29` + `addiu $16, $2, -2880`), loads +0x2C at
 * 0x001136C4 (`lw $3, 44($16)`), stores 0 back over a negative at
 * 0x001136D0, stores 15 back over anything >= 16 at 0x001136D8..0x001136E8,
 * returns without drawing when it is 0 at 0x001136EC, and otherwise puts
 * the word into the alpha byte of an RGBA whose other three bytes are 255
 * (0x00113620..0x00113638) and draws one untextured full-frame sprite. So
 * the word is a 0..15 white-overlay level.
 *
 * code, the screen that steps it. Layout 0x3C's action function is
 * `la_adjust_screen` (retail 0x001BE058; listing 0x001C11F0,
 * layout_action.c:4390, 78 words and no masked mismatch). It builds the
 * same base at 0x001BE098 and 0x001BE0C4, decrements the word on LEFT
 * (`andi 0x8000` at 0x001BE08C) while it is above 0, increments it on
 * RIGHT (`andi 0x2000` at 0x001BE0BC) while `slti $2, $16, 14` at
 * 0x001BE0CC holds, stores 7 on triangle (`andi 0x10` at 0x001BE0F8, `li
 * $2, 7` and `sw $2, 44($3)` at 0x001BE110 and 0x001BE114), and lights the
 * bar by calling lt_default_mask_property(i + 395, 1) for fifteen
 * positions (0x001BE118..0x001BE130) and then (word + 395, 0) for the one
 * the word names (0x001BE134..0x001BE144).
 *
 * The screen is therefore the game's own screen adjustment screen, and not
 * a memory card screen. This entry and the two below carried the name
 * `_la_memory_card_check` until 2026-09-05; the listing's own
 * `la_boot_memory_card_check` (0x001C1328, layout_action.c:1342) is a
 * different function, is layout 0x07's action function (retail
 * 0x001BE190), and that layout has an empty object range. */
constexpr uint32_t RT_ICO_BRIGHTNESS = 0x0028F4C0 + 0x2C;

/* correlation: 9/9 sites. code: the same base, built twice out of the code
 * that indexes the table, `lui 0x53` + `addiu 16360` at
 * 0x001BF22C/0x001BF234 (lt_switch_layout) and 0x001C071C/0x001C0728
 * (lt_next_layout).
 *
 * The layout entry is the one menu structure whose fields sit where the
 * decomp's structure has them. code, every field below read out of the
 * functions that use it:
 *   stride 0x38   `addiu $v1, $zero, 56` then `mult` by the layout id, at
 *                 0x001BF220/0x001BF228, 0x001C0718/0x001C0724 and
 *                 0x001BF900/0x001BF908.
 *   +0x00, +0x04  the object range, `lw 0($s0)` and `lw 4($s0)` compared by
 *                 `slt` at 0x001C0748..0x001C0750.
 *   +0x10..+0x1C  the fade colour, four `lwc1` at 16, 20, 24 and 28 off the
 *                 entry at 0x001C0428..0x001C0438 (lt_prev_layout), scaled
 *                 by the 0x437F (255.0f) and 0x42FE (127.0f) immediates the
 *                 function materializes.
 *   +0x20, +0x24  the action function and its argument, `lw 32($s0)` and
 *                 `lw 36($s0)` at 0x001C07F8 and 0x001C0818.
 *   +0x28, +0x2C  the default item copied into the current item,
 *                 `lw $v1, 40($v0)` then `sw $v1, 44($v0)` at 0x001BF914 and
 *                 0x001BF91C.
 *   +0x30         the parent id, `lw 48($s0)` at 0x001C0794.
 *
 * The count is a property of the table rather than of the struct, so it was
 * read off the table: a row is 0x38 bytes with a function pointer or zero at
 * +0x20 and a parent id at +0x30, rows keep that shape from 0x00533FE8
 * through index 0x4F, and index 0x50 is not a row (the bytes there are
 * text). The criterion was checked on a known answer before it was used
 * here: run over the earlier reading of the same table it stops at the count
 * that reading carries. */
constexpr uint32_t RT_ICO_LAYOUT_TABLE = 0x00533FE8;
constexpr uint32_t RT_ICO_LAYOUT_STRIDE = 0x38;
constexpr uint32_t RT_ICO_LAYOUT_COUNT = 0x50;
constexpr uint32_t RT_ICO_LAYOUT_FIRST_OBJ = 0x00;
constexpr uint32_t RT_ICO_LAYOUT_END_OBJ = 0x04;
constexpr uint32_t RT_ICO_LAYOUT_DEFAULT_ITEM = 0x28;
constexpr uint32_t RT_ICO_LAYOUT_CUR_ITEM = 0x2C;
constexpr uint32_t RT_ICO_LAYOUT_PARENT = 0x30;

/* correlation: 9/9 sites. code: the same base built out of the code that
 * indexes the objects, `lui 0x31` + `addiu -12296` at
 * 0x001BF244/0x001BF254, 0x001C0720/0x001C072C and 0x001C0F5C/0x001C0F64.
 *
 * This structure is NOT the decomp's. code, 2026-09-04: the functions
 * multiply the item index by 112 (`addiu $v1, $zero, 112` then `mult` at
 * 0x001BF4B4/0x001BF4BC in lt_switch_layout and the same pair at
 * 0x001BF23C/0x001BF25C, `addiu $t2, $zero, 112` at 0x001C0730 in
 * lt_next_layout, `addiu $v0, $zero, 112` at 0x001C0F58 in GetRealModelId),
 * so the object is 0x70 bytes here, four more than the decomp's structure,
 * and every field from +0x24 up sits four bytes higher. The offsets below
 * were each read out of this build's code, not shifted on paper. Carrying
 * the decomp's offsets would have read the wrong word of the right object,
 * which is why the menu pointer was off until they were read. */
constexpr uint32_t RT_ICO_SCENE_OBJECTS = 0x0030CFF8;
constexpr uint32_t RT_ICO_SCENE_STRIDE = 0x70;
/* code: lt_switch_layout takes the same seven paths in the same order as the
 * decomp's function, each pad bit reading its link four bytes higher than
 * the decomp's structure has it: triangle (andi 0x10) `lw 40($s1)` at
 * 0x001BF56C, cross (andi 0x40) `lw 44($s1)` at 0x001BF4D4, right (andi
 * 0x2000) `lw 48($s1)` at 0x001BF408, left (andi 0x8000) `lw 52($s1)` at
 * 0x001BF3E0, down (andi 0x4000) `lw 56($s1)` at 0x001BF350, up (andi
 * 0x1000) `lw 60($s1)` at 0x001BF2C0. */
constexpr uint32_t RT_ICO_OBJ_TRIANGLE_TARGET = 0x28;
constexpr uint32_t RT_ICO_OBJ_CROSS_TARGET = 0x2C;
constexpr uint32_t RT_ICO_OBJ_RIGHT = 0x30;
constexpr uint32_t RT_ICO_OBJ_LEFT = 0x34;
constexpr uint32_t RT_ICO_OBJ_DOWN = 0x38;
constexpr uint32_t RT_ICO_OBJ_UP = 0x3C;
/* code: lt_link_layout (0x001BF95C) runs instruction for instruction against
 * the earlier reading of the same function through the whole sprite setup,
 * so each load pairs with a load of the same value: centred `lw 68($s0)` at
 * 0x001BF9F0 and 0x001BFA00; height `lw 72($s0)` at 0x001BF9B0; width
 * `lw 76($s0)` at 0x001BF9A8; Y `lw 80($s0)` at 0x001BFA50; X `lw 84($s0)`
 * at 0x001BFA2C, both then `addiu -320` and `sll 4`.
 *
 * The two texture sizes are the exception to the four-byte shift: the four
 * words at the end of the decomp's object are reordered here as well as
 * moved. They are identified by their use, not by their position: the
 * height substitute is the word scaled and halved into the same stack slot
 * the height went to when the height is zero (`lw 96($s0)` at 0x001BF9A0),
 * and the width substitute is the word taken when the width is zero
 * (`lw 100($s0)` at 0x001BF990). */
constexpr uint32_t RT_ICO_OBJ_CENTRED = 0x44;
constexpr uint32_t RT_ICO_OBJ_H = 0x48;
constexpr uint32_t RT_ICO_OBJ_W = 0x4C;
constexpr uint32_t RT_ICO_OBJ_Y = 0x50;
constexpr uint32_t RT_ICO_OBJ_X = 0x54;
constexpr uint32_t RT_ICO_OBJ_TEX_H = 0x60;
constexpr uint32_t RT_ICO_OBJ_TEX_W = 0x64;
/* code: the flag word moved with the rest, `lw 108($s0)` at 0x001BFA8C,
 * 0x001BFA9C, 0x001BFBC0 and 0x001BFC90 in lt_link_layout. The BIT moved
 * too, and by more than the offset did: lt_next_layout copies bit 5 into
 * bit 4 for every object in the chain (`srl $v0, $v1, 1`, `andi 0x10`,
 * `and $v1, $v1, -17`, `or` at 0x001C076C..0x001C077C) where the decomp's
 * function copies bit 0 into bit 1, and GetRealModelId (0x001C0F58) sets
 * bit 5 from its flag argument (`andi 1`, `sll $a1, $a1, 5`, `and $v0,
 * $v0, -33`) where the decomp's sets bit 0. So the hidden bit the pointer
 * must not select through is 0x10 on this build and the persistent half of
 * the pair is 0x20. */
constexpr uint32_t RT_ICO_OBJ_FLAGS = 0x6C;
constexpr uint32_t RT_ICO_OBJ_FLAG_HIDDEN = 0x10;
/* The persistent half of the pair, 0x20 on this build; see the paragraph
 * above, which measures both halves. */
constexpr uint32_t RT_ICO_OBJ_FLAG_PERSISTENT = 0x20;
/* The screen adjustment screen's fifteen bar positions. code:
 * `la_adjust_screen` (0x001BE058, above) calls lt_default_mask_property(i +
 * 395, 1) for fifteen of them (`addiu $a0, $s0, 395` at 0x001BE118 and
 * 0x001BE130, `slti $v0, $s0, 15` at 0x001BE128) and then once more for the
 * position the level names (`addiu $a0, $a0, 395` at 0x001BE144). 395 is
 * 0x18B, and fifteen is the loop bound, both decoded out of the retail
 * words. Layout 0x3C's own object range in the retail layout table is
 * 392..420, which contains all fifteen and is the only range in the table
 * that does. */
constexpr uint32_t RT_ICO_ADJUST_FIRST_OBJ = 0x18B;
constexpr uint32_t RT_ICO_ADJUST_COUNT = 15;

/* ---- the two boot screens the kanban system draws -------------------------
 *
 * The refresh rate choice (50 Hz or 60 Hz) and the language choice are the
 * first two screens SCES_507.60 puts up, and neither is a layout-table
 * screen in the sense the block above means: no layout id word names them,
 * `lt_next_layout` does not run them, and the fade state at
 * RT_ICO_FADE_STATE is not what gates them. They are drawn by the game's
 * kanban system (the disc listing's kanban.c and kanbanBoot.c), which puts
 * one node per screen in a list of its own and hands each node a layout
 * table entry to take its objects, its selection word and its item links
 * from. So the word the pointer writes on these two screens is the same
 * layout entry field (+0x2C) it writes everywhere else; what differs is how
 * the host finds out which entry that is, and whether the screen is up at
 * all.
 *
 * Method for every constant in this block: `listing`. The six functions it
 * quotes were each matched against the retail bytes at the address given,
 * with the relocatable fields masked out, and all six match with no
 * mismatch anywhere in 1522 words:
 *
 *   retail      disc        listing name and source file      words
 *   0x001B92D0  0x001BC468  kanbanBootMcCheck, kanbanBoot.c     404
 *   0x001B9920  0x001BCAB8  kanbanBootMain, kanbanBoot.c         86
 *   0x001B8738  0x001BB8D0  kanbanReqAdd, kanban.c               76
 *   0x001B8F28  0x001BC0C0  display_layout, kanban.c            108
 *   0x001B8B10  0x001BBCA8  display_texture, kanban.c           180
 *   0x001BF960  0x001C2AF8  display_texture, layout_texture.c   668
 *
 * The fifth of those is the function this file and guest/menu_nav.cpp call
 * func_001B8B10, and the sixth is the one they call lt_link_layout; the two
 * are different static functions of the same name in two translation units.
 * Every address, offset and immediate below was decoded out of the retail
 * words, not read out of the listing. The $gp-relative ones use gp
 * 0x00640AF0, the value the runtime's own thread log reports for this ELF
 * and the one the block above already rests on.
 *
 * The kanban node the game is taking input on. `display_layout`
 * (0x001B8F28) reads it at 0x001B8F2C and gives the pad to the node in its
 * list whose entry pointer (+0x00, `lw $18, 0x0($17)` at 0x001B8F48)
 * matches this node's, provided that node's flag word (+0x0C, `lw $2,
 * 0xC($17)` and `andi $2, $2, 1` at 0x001B8F60 and 0x001B8F64) does not
 * have bit 0 set. This build's .text has exactly six $gp-relative accesses
 * forming 0x0063C39C (displacement -0x4754), and all six are the whole life
 * of the word: 0x001B8850 is `kanbanReqAdd`'s store, which happens only for
 * a node whose entry has a default item other than -1 (`bnel $7, $2` against
 * -1 at 0x001B884C); 0x001B8AF0 is `kanbanInit`'s clear; 0x001B8F2C is the
 * read above; 0x001B9114 and 0x001B9124 are `kanbanReqDelFade`, which clears
 * it when the screen it names is the one being faded out; and 0x001B9164 is
 * `kanbanReqAllDel`'s clear. So a non-zero value here is a screen that is up
 * and can be navigated, and there is no seventh site that could mean
 * something else by it. */
constexpr uint32_t RT_ICO_KANBAN_ACTIVE = 0x0063C39C;
/* The thirty node slots the value above points into. `kanbanReqAdd`
 * (0x001B8738) forms the base with `lui $3, 0x72` + `addiu $6, $3, -0x34F0`
 * at 0x001B8748 and 0x001B8750, walks it with `addiu $6, $6, 0x20` at
 * 0x001B8774 and stops at `slti $2, $3, 30` at 0x001B876C. The runtime reads
 * this only to say whether the word above holds one of those thirty
 * addresses; a value that is not one of them is a state the pointer refuses
 * to act on and logs. */
constexpr uint32_t RT_ICO_KANBAN_NODES = 0x0071CB10;
constexpr uint32_t RT_ICO_KANBAN_NODE_STRIDE = 0x20;
constexpr uint32_t RT_ICO_KANBAN_NODE_COUNT = 30;
/* The node's own two fields the pointer reads. +0x00 is the layout table
 * entry the screen is built from: `kanbanReqAdd` computes it as
 * `D_00533FE8 + id * 0x38` (`addiu $3, $zero, 56` and `mult` at 0x001B8738
 * and 0x001B8740, the base formed by `lui $2, 0x53` + `addiu $2, $2, 0x3FE8`
 * at 0x001B8744 and 0x001B874C, which is RT_ICO_LAYOUT_TABLE) and stores it
 * at 0x001B879C, and the same call copies that entry's default item (+0x28)
 * into its current item (+0x2C) at 0x001B87A8 and 0x001B87C4. So the screen's
 * layout id is (entry - RT_ICO_LAYOUT_TABLE) / 0x38 and its selection word is
 * the layout entry's own current item, the word the pointer already writes.
 *
 * +0x0C bit 0 is the "this screen is fading out" flag: `kanbanReqDelFade`
 * (0x001B9110) sets it with `lw $2, 12($4)`, `ori $2, $2, 1`, `sw $2,
 * 12($4)` at 0x001B9110, 0x001B9118 and 0x001B9120, and `display_layout`
 * gives a node carrying it no input at all. */
constexpr uint32_t RT_ICO_KANBAN_NODE_LAYOUT = 0x00;
constexpr uint32_t RT_ICO_KANBAN_NODE_FLAGS = 0x0C;
constexpr uint32_t RT_ICO_KANBAN_FLAG_FADING = 0x1;
/* Which layout entry each of the two boot screens is built from, measured in
 * `kanbanBootMcCheck` (0x001B92D0), which is the only source of kanban
 * screens in this ELF: `kanbanReqAdd` has exactly five call sites and all
 * five are inside kanbanBoot (0x001B9678, 0x001B9770, 0x001B9868, 0x001B9888
 * and 0x001B99A8 in kanbanBootMain).
 *
 * Layout 0 is the language screen. The call at 0x001B9678 is
 * `kanbanReqAdd(0, 2)`, and the four instructions before it are
 * `sceScfGetLanguage` (0x001B9614) followed by a store of one of five item
 * indices into 0x00534010 (0x001B9670), which is RT_ICO_LAYOUT_TABLE + 0x28,
 * that is layout 0's default item field. The screen is then read back at
 * 0x001B96A4, `lw $3, 0x2C($2)` off the same entry, and its item minus 0x1A
 * indexes a five-entry jump table.
 *
 * Layout 1 is the 50/60 Hz screen. The call at 0x001B9770 is
 * `kanbanReqAdd(1, 2)`, and the state that waits on it reads the same
 * `lw $5, 0x2C($2)` at 0x001B97A4 and turns it into the video mode word:
 * item 0x21 stores 1 to D_0028F4C0 (0x001B97CC, in a branch delay slot) and
 * item 0x22 stores 0 at 0x001B97D0, where D_0028F4C0 is
 * RT_ICO_VIDEO_MODE below, 0 NTSC and 1
 * PAL. A change there calls `gsResetFunc` (0x001B97E8), which is the
 * SetGsCrt the runtime's log shows when the player toggles the choice.
 *
 * These two ids are read for the log line only. Which screen is up is
 * decided by the node above and not by an id, so a build that put these
 * screens on other entries would still be pointed at correctly. */
constexpr uint32_t RT_ICO_KANBAN_LAYOUT_LANGUAGE = 0;
constexpr uint32_t RT_ICO_KANBAN_LAYOUT_VIDEO_MODE = 1;

/* ---- the boot state machine's two state words (boot trace) ---------------
 *
 * Read only, by guest/boot_trace.cpp, for the first fields of a run: the
 * log names every change of these two words with the field it happened on,
 * next to the presenter's own trace of what each field displayed, so a
 * boot-time picture fault can be placed against the game's own state.
 *
 * Both are $gp-relative words of this build (gp 0x00640AF0, as above).
 * kanbanBootMain (0x001B9920) opens with `lw $6, -22068($gp)`
 * (0x8f86a9cc at 0x001B9920), which is its state word: the value is
 * compared against 8 and jump-tabled, and every state stores its
 * successor back to the same displacement. kanbanBootMcCheck
 * (0x001B92D0) loads its own state word with `lw $7, -22064($gp)`
 * (0x8f87a9d0 at 0x001B92DC), compares it against 102, 90, 2, 97, 95, 96,
 * 100, 101, 200, 190, 191, 194, 300, 201, 202, 301 and 302 in that order,
 * and every branch of it stores the next state to the same displacement.
 * The disc's own listing of the same function (SRCFILE.TXT, kanbanBoot.c)
 * has the same shape at a different displacement, which is why the values
 * were decoded out of the retail words and not copied from the listing.
 * What the states mean is read off the listing: 95 and 96 load the product
 * block (iosMcLoadProductBlock, iosMcSync) and, when it holds a system
 * block, call gsResetFunc and go 97 then 190; 100 to 102 are the language
 * screen (kanbanReqAdd(0, 2)); 200 to 202 the 50/60 Hz screen
 * (kanbanReqAdd(1, 2)); 190 and 191 fade to black and switch to stage 1
 * (stgmgrForceSwitchWithFade(1, 255.0f) with a zero colour, then
 * lt_switch_layout(7)); -1 is done. */
constexpr uint32_t RT_ICO_KANBAN_BOOT_MAIN_STATE = 0x0063B4BC;
constexpr uint32_t RT_ICO_KANBAN_BOOT_MC_STATE = 0x0063B4C0;

/* The stage fade gsb_fade draws (boot trace, read only). Retail gsb_fade
 * is 0x00112F78, matched by its gp-independent words (li $6,2; sra; sd $ra;
 * sra; negu; sll at +0x0C..+0x20 are the same in the disc listing's
 * gsb_fade at 0x112E68); its gp displacements decode to these words:
 * state at +0x48 (`lw $3, -20048($gp)`; 0 off, 1 start, 2 running, 3 held
 * at full), speed at +0x80 (`lwc1 $f1, -20044($gp)`, level units per
 * frame times 0.5, negative fades in), level at +0xB0 (`swc1 $f0,
 * -18892($gp)`, 0..128 = sprite alpha), and the sprite's RGBA bytes at
 * -20032..-20029 (alpha byte stored at +0x198, `sb $2, -20029($gp)`).
 * StageManager (disc 0x1ab5a8..0x1ab5cc) starts a fade by writing state 1,
 * speed and the colour bytes; kanbanBootMcCheck clears the state on its
 * return-1 path. */
constexpr uint32_t RT_ICO_FADE_GSB_STATE = 0x0063BCA0;
constexpr uint32_t RT_ICO_FADE_GSB_SPEED = 0x0063BCA4;
constexpr uint32_t RT_ICO_FADE_GSB_LEVEL = 0x0063C124;
constexpr uint32_t RT_ICO_FADE_GSB_RGBA = 0x0063BCB0;
/* The screen adjustment level guest/boot_trace.cpp also logs is
 * RT_ICO_BRIGHTNESS, defined with its derivation in the menu block above.
 * It is not defined a second time here: it used to be, under a second
 * description, which is the defect this note replaces. */

/* The matrix composer's entry, and the projection block it is handed.
 * Neither came from correlation: the composer's name does not carry over and
 * neither it nor its caller matches anything in this build's .text by
 * fingerprint, so both were found by the code that builds the block.
 * Exactly one window of this build's .text materializes the writer's float
 * constant set (2048.0, 262144.0, 4.0, 3.0, 640.0, 100.0, 1.0, 2.0) and
 * ends in swc1 stores off a base formed by `lui $16, 0x0068` at 0x00114CB0
 * and `addiu $2, $16, -17824` at 0x00114DAC (= 0x0067BA60), and in a tail
 * `j 0x001146F0` at 0x00114E4C. config/entry_hooks.txt carries the full
 * argument and is what the translator keys its one entry hook on; the
 * generated code calls rt_entry_hook(ctx, 0x1146F0) in src_GsBase.c.
 *
 * listing: the composer is `gsb_SetVSMatrixSub` (disc 0x001145E8, 270
 * words and no masked mismatch) and the writer is `gsb_SetVSMatrix` (disc
 * 0x00114AD0, entry 0x00114BD8 here). The two links differ inside the
 * writer, which is why only its entry is given and not a word count: the
 * disc build ends it with `jal gsb_SetVSMatrixSub` and a return, the
 * retail build with the tail jump above.
 *
 * The X scale offset is measured on this build and not carried: the writer
 * takes its first argument into $s2 at 0x00114BF4 (`daddu $s2, $a0,
 * $zero`), converts it and divides it by RT_ICO_SCREEN_W at 0x00114DB8,
 * 0x00114DBC and 0x00114DE8, and stores the quotient at `swc1 $f2, 4($v0)`
 * (0x00114E40) with $v0 the block base. The other eight floats go to
 * +0x00 (0x00114CF0 or 0x00114D20, one per branch) and +0x08..+0x20
 * (0x00114E20..0x00114E48). guest/widescreen.cpp carries the rest of the
 * derivation, including the writer's five call sites. */
constexpr uint32_t RT_ICO_MATRIX_COMPOSER = 0x001146F0;
constexpr uint32_t RT_ICO_PROJ_BLOCK = 0x0067BA60;
constexpr uint32_t RT_ICO_PROJ_X_SCALE = 0x04;

/* correlation: 62/62 sites and 51/51 sites. The two highest site counts in
 * this block. */
constexpr uint32_t RT_ICO_SCREEN_W = 0x0063A064;
constexpr uint32_t RT_ICO_SCREEN_H = 0x0063A068;

/* correlation: 3/3 sites. RA independently places the game's flag array at
 * this address ("[384 Bit] Start of Flags Array") and every one of its 31
 * achievement conditions reads a byte inside it, which is the cross-check
 * that matters most in this header: the observer's whole signal is this
 * array.
 *
 * code: a third source, and the one that fixes the length. The two functions
 * that move the array whole are at 0x001819A0 (the write to the card) and
 * 0x001819F8 (the read back), both matched by masked window against the
 * earlier reading of the same pair. Each forms the array's address and then
 * passes a length of 0x32.
 *
 * listing: the same two are the listing's `gflagSave` (disc 0x00183A90, 22
 * words, no masked mismatch) and `gflagLoad` (disc 0x00183AE8, 20 words,
 * no masked mismatch), which is where the names come from. A third retail
 * site is `gflagInit` (0x00181900; disc 0x001839F0, 40 words, no masked
 * mismatch): it forms 0x002A50C0 at 0x00181914 and 0x0018191C and clears
 * the array for `li $a2, 50` at 0x00181928. So the length is confirmed by
 * three sites of this build, and it is not RA's 384 bits (0x30 bytes); the
 * highest byte any RA condition touches is base+0x2C, inside both. */
constexpr uint32_t RT_ICO_PROGRESS_BITS = 0x002A50C0;
constexpr uint32_t RT_ICO_PROGRESS_BYTES = 0x32;
/* code: 0x0063AA04, 8 of 8 sites. Correlation cannot see it for the same
 * reason it cannot see RT_ICO_NAV_SWALLOW above: the word is reached
 * $gp-relative and none of the eight functions that touch it correlates
 * whole. This build's .text has exactly eight $gp-relative accesses forming
 * 0x0063AA04. The decisive one is the save path itself: 0x001819A0 loads
 * the stage id from 0x00639D10 (which correlation measured at 39/39 sites)
 * and stores it to 0x0063AA04 at 0x001819B8, then hands 0x0063AA04 to the
 * card writer for 4 bytes; 0x001819F8 reads the same word back first.
 *
 * The retail words say the same thing about the record's shape, and are
 * where the following was decoded: gflagSave stores the stage id at
 * 0x001819B8, passes 0x0063AA04 at 0x001819AC (`addiu $a1, $gp, -24812`),
 * then passes a second 4-byte word, 0x0063AA00, at 0x001819C8, and only
 * then the 0x32 bytes of the array at 0x001819E0 and 0x001819E8. gflagLoad
 * reads the same three back at 0x001819FC, 0x00181A18 and 0x00181A30.
 * 0x0063AA00 is RT_ICO_CLEAR_COUNT below, which RA measured
 * independently, so two readings agree on the save record's shape.
 * Nothing in the runtime reads RT_ICO_PROGRESS_WORD; it is here because
 * that shape is an address fact about this build. */
constexpr uint32_t RT_ICO_PROGRESS_WORD = 0x0063AA04;
/* correlation: 39/39 sites. RA's notes call this same word "Map ID 3", a
 * second source for it being a stage id. */
constexpr uint32_t RT_ICO_STAGE_ID = 0x00639D10;

/* correlation: 1/1 site.
 *
 * The header's length is 0x14, measured on the retail ELF. `gflagInit`
 * (0x00181900; the listing's gflagInit, disc 0x001839F0, 40 words and no
 * masked mismatch) forms this address at 0x00181970 and 0x00181978 (`lui
 * $4, 0x2A` + `addiu $4, $4, -17968`) and clears the object for `li $a2,
 * 20` at 0x00181984. Two corroborations from the save and load paths:
 * la_save_processing copies 0x14 bytes out of it into the save record's
 * per-file slot at 0x001BD0C4 through 0x001BD0E8, and la_load_processing
 * copies the same 0x14 bytes back, so the record's file table entry is
 * exactly this header.
 *
 * An earlier reading here said the length was unknown because the next
 * datum, 0x0029B9E8, sits at header + 0x18 rather than header + 0x10. That
 * reasoning does not hold: a 0x14 header followed by four bytes of padding
 * puts the next object at + 0x18. Nothing in the runtime reads the length;
 * it is recorded so the two words below are inside a bounded object rather
 * than an open one. */
constexpr uint32_t RT_ICO_SAVE_HEADER = 0x0029B9D0;
constexpr uint32_t RT_ICO_SAVE_HEADER_BYTES = 0x14;
/* Measured, not inferred, and it is not a save counter. The word at
 * header + 0x04 is written from RT_ICO_CLEAR_COUNT (0x0063AA00) on every
 * save: la_save_processing loads that word $gp-relative at 0x001BD09C and
 * stores it at header + 0x04 at 0x001BD0B0, off the base it forms at
 * 0x001BD050. So it is the completed-playthrough count as of the last save.
 * The other words of the header the disc listing names are the stage id at
 * +0x00, the frame clock at +0x08 (RT_ICO_TIME_FRAMES, incremented once a
 * field by la_playtime_count at 0x001BE9FC) and the sofa layout id at
 * +0x0C.
 *
 * It was named RT_ICO_SAVE_COUNTER and flagged inferred until those two
 * stores were read. Both are corrected here rather than left: a host-side
 * profile keyed on this word alone is keyed on the playthrough count, which
 * two separate playthroughs at the same count share.
 * guest/achievements.cpp therefore keys on the card slot as well; see the
 * store section of that file for what the card slot rests on. */
constexpr uint32_t RT_ICO_SAVE_HEADER_CLEAR_COUNT = RT_ICO_SAVE_HEADER + 0x04;

/* correlation: 1/1 site.
 *
 * The stride was 0x18C here, carried from a reading of a different build's
 * structure rather than from this one, and it is wrong. Measured on the
 * retail ELF: the two functions that move a product record are at
 * 0x0013A428 and 0x0013A5A0 (the listing's `product_write`, disc
 * 0x0013B838, 94 words, and `product_read`, disc 0x0013B9B0, 18 words,
 * neither with a masked mismatch). `product_read` forms the array base at
 * 0x0013A5AC and 0x0013A5B8 (`lui $a1, 0x2A` + `addiu $a1, $a1, -18960` =
 * 0x0029B5F0, which is RT_ICO_SAVE_SLOTS below), indexes it with
 * `li $v1, 496` at 0x0013A5A4 and `mult` at 0x0013A5C4, and passes
 * `li $a2, 496` at 0x0013A5C0 as the transfer length. `product_write`
 * carries the same two literals at 0x0013A43C and 0x0013A48C. So the slot
 * record is 496 bytes on this disc.
 *
 * The array holds two records, one per card slot: 0x0029B9D0 - 0x0029B5F0
 * is 0x3E0, which is two strides. What the fields inside a record are is
 * not reproduced here; CLAUDE.md limits this file to addresses and names.
 *
 * Nothing in the runtime reads either of these two; they are here to
 * document the shape of the save record. The runtime does not need them
 * either way: sif/mc.cpp is byte-transparent, it stores whatever length the
 * game writes and gives it back, so the record size is the game's business
 * and not the card server's. */
constexpr uint32_t RT_ICO_SAVE_SLOTS = 0x0029B5F0;
constexpr uint32_t RT_ICO_SAVE_SLOT_STRIDE = 0x1F0;
/* correlation: 5/5 sites. */
constexpr uint32_t RT_ICO_SAVE_CARD_INDEX = 0x0063B550;
/* correlation: 1/1 site. */
constexpr uint32_t RT_ICO_SAVE_FILE_INDEX = 0x0063B4E4;

/* NOT RESOLVED, both of them, and 0x32 is not the answer.
 *
 * An earlier reading here recorded 0x32 as "very likely" the gameplay id,
 * from the decomp. The retail layout table settles that it is not: entry
 * 0x32's action function is 0x001BD408, which is the listing's
 * `la_delete_processing` (disc 0x001C05A0, 62 words and no masked
 * mismatch). The same table names the two candidates that fit instead:
 * entry 0x36's action function is 0x001BD500, the listing's `la_game_loop`
 * (disc 0x001C0698, 88 words, no masked mismatch), and entry 0x3E's is
 * 0x001BD660, the listing's `la_game_over_continue` (disc 0x001C07F8, 128
 * words, no masked mismatch).
 *
 * Those two are still not carried, because a name is not the predicate the
 * counters need. What a run has to settle is which layout id the id word
 * actually holds while the player is playing (the loop is one of at least
 * four gameplay-adjacent entries: 0x35 la_game_loading, 0x36 la_game_loop,
 * 0x37 la_game_demo and 0x3F la_switching_stage) and which it holds on a
 * death. With the sentinel, playtime_ms stays 0 and the two time trophies
 * use the game's own frame counter instead, which is measured
 * (RT_ICO_TIME_FRAMES below). What would resolve both: the layout id lines
 * the diagnostic log already prints (docs/ACHIEVEMENTS.md section 5), one
 * from a playing field and one from a death. */
constexpr uint32_t RT_ICO_LAYOUT_GAMEPLAY = RT_ICO_SYM_UNKNOWN;
constexpr uint32_t RT_ICO_LAYOUT_GAMEOVER_UNKNOWN = 0xFFFFFFFFu;
constexpr uint32_t RT_ICO_LAYOUT_GAMEOVER = RT_ICO_LAYOUT_GAMEOVER_UNKNOWN;

/* ---- four words measured by RA on this build ------------------------------
 *
 * Correlation resolved none of these four. All four come from the
 * r=codenotes2 notes for game 1319 and are corroborated by the conditions
 * of the set's own achievements, which read them.
 */
/* RA "[32 Bit] Map ID", and the word 30 of the set's 31 conditions gate on.
 * Values from the same note: 1 logo, 2 title, 4 cage, 11 first gate, 12
 * second gate, 23 sluice, 35 jetty, 39 beach (the ending), 50 and 51 the
 * two reflectors. Read and logged, never decided from: the observer's
 * unlock condition is the progress bit alone, because a bit that rises is
 * the event whether or not this port has the same map numbering. */
constexpr uint32_t RT_ICO_MAP_ID = 0x0028FE74;
/* RA "[32 Bit] Internal Time (Frames)", the game's own in-game clock. It is
 * RT_ICO_SAVE_HEADER + 0x08, that is inside the object the game saves,
 * which is what an in-game time that survives a save has to be. The set's
 * "Master of the Castle" reads it: under 432000 with the mode word at 0 and
 * under 360000 with it at 1, which is two hours at 60 and at 50 fields a
 * second. guest/achievements.cpp compares frames against frame thresholds
 * from the same two rates rather than converting to milliseconds. */
constexpr uint32_t RT_ICO_TIME_FRAMES = 0x0029B9D8;
/* RA "[32 Bit] NTSC / PAL Mode", 0 NTSC and 1 PAL. Also measured by
 * correlation at 143/143 sites, the base RT_ICO_BRIGHTNESS above is an
 * offset from. Read only to pick which of the two frame thresholds the time
 * trophies use, which is exactly what the RA condition does with it. */
constexpr uint32_t RT_ICO_VIDEO_MODE = 0x0028F4C0;
/* RA "[8 Bit] Game Clear count", the number of completed playthroughs
 * ("Unlocks Film effect + 2 player mode at > 0"). One byte. It is what
 * separates the two secret weapons: the game records both with the one
 * progress bit 114, and the set's two achievements differ only in
 * 0xH63aa00=0 against 0xH63aa00!=0. code: a second source for the address,
 * and for it being saved state. This build's save path writes this word to
 * the card beside the progress array and reads it back (0x001819C8 and
 * 0x00181A18, see RT_ICO_PROGRESS_WORD above), and lt_switch_layout loads it
 * at 0x001BF2D4 and 0x001BF364 to gate the menu items a completed
 * playthrough unlocks.
 *
 * code: a third reading, of the retail words, and the one that says what
 * the word gates. The film-grain pass is only reached while this word is
 * greater than zero: 0x001142D8 is `lw $2, -24816($gp)` (0x0063AA00),
 * 0x001142DC is `blez $2` over the call, and 0x001142EC is
 * `jal 0x00114068`, which is the listing's `gsb_filmNoise` (disc
 * 0x00113F58, 82 words and no masked mismatch). That is the same address
 * and the same "greater than zero" test RA's note describes as unlocking
 * the film effect, measured independently of RA and of this build's save
 * path, so three readings agree on what this word is.
 *
 * One half of RA's note does not survive, and it is recorded here rather
 * than repeated: the note says a nonzero clear count also unlocks the
 * two-player mode. Layout 0x3A's action function `la_game_option` (retail
 * 0x001BDCB8; the listing's la_game_option, disc 0x001C0E50,
 * layout_action.c) toggles a separate option word, 0x00639EA0, on a
 * LEFT-or-RIGHT press: `lw $2, -27728($gp)`, `sltiu $2, $2, 1`,
 * `sw $2, -27728($gp)` at 0x001BDE10, 0x001BDE14 and 0x001BDE1C, with no
 * clear-count test anywhere in that path. That the toggled word is the
 * two-player row rather than one of the other option rows is inferred: it
 * is read off the decomp and is not settled by anything in the retail
 * words or the listing, neither of which names data. What is measured is
 * that the film-grain gate reads this clear count and that
 * `la_game_option`'s toggle does not. Nothing in this runtime reads either
 * fact; it is here so the note is not carried forward as measured. */
constexpr uint32_t RT_ICO_CLEAR_COUNT = 0x0063AA00;

/* ---- the trophy bit table ------------------------------------------------
 *
 * Which bit of the progress array stands for which trophy. Every entry is
 * an address fact of the same class as the rest of this header: a bit index
 * into a guest array paired with what the game sets it for. Nothing about a
 * trophy's name, tier or text belongs here; that is authored host-side from
 * the public PS3 list and lives in guest/achievements.cpp.
 *
 * The table is filled from the RetroAchievements set for this disc (game
 * 1319, retrieved 2026-09-04), whose conditions were measured on this build.
 * A bit index is a fact about one build, so nothing here was carried from a
 * reading of another.
 *
 * A trophy with no entry stays locked and says why. An entry invented from
 * a plausible range would unlock on the wrong event and there would be
 * nothing to notice it by.
 *
 * `trophy` indexes the trophy table in guest/achievements.h (RtTrophyId).
 * This header deliberately includes nothing but <cstdint>, so it cannot name
 * that enum; the constants below mirror it and guest/achievements.cpp
 * static_asserts every one of them against the enum, so the two cannot
 * drift.
 */
constexpr int RT_ICO_TROPHY_RESCUE = 0;
constexpr int RT_ICO_TROPHY_FAILURE = 1;
constexpr int RT_ICO_TROPHY_ARMED_AND_READY = 2;
constexpr int RT_ICO_TROPHY_EAST_GATE = 3;
constexpr int RT_ICO_TROPHY_WEST_GATE = 4;
constexpr int RT_ICO_TROPHY_FAREWELL = 5;
constexpr int RT_ICO_TROPHY_ROYAL_ARMS = 6;
constexpr int RT_ICO_TROPHY_EMANCIPATION = 7;
constexpr int RT_ICO_TROPHY_SPLIT_THE_WATERMELON = 8;
constexpr int RT_ICO_TROPHY_SPIKED_CLUB = 9;
constexpr int RT_ICO_TROPHY_SHINING_SWORD = 10;

/* `clear_count`, the qualifier a bit needs when the game records two
 * different things with one bit. RT_ICO_CLEAR_COUNT is the byte it is read
 * from; an entry that carries a qualifier can only fire while that address
 * is known. */
constexpr int RT_ICO_BIT_CLEAR_ANY = -1;     /* no qualifier */
constexpr int RT_ICO_BIT_CLEAR_FIRST_RUN = 0;  /* clear count == 0 */
constexpr int RT_ICO_BIT_CLEAR_NEW_GAME_PLUS = 1; /* clear count != 0 */

struct RtIcoTrophyBit {
    int trophy;
    int bit; /* 0 .. RT_ICO_PROGRESS_BYTES * 8 - 1, that is 0..399 here */
    int clear_count = RT_ICO_BIT_CLEAR_ANY;
};

/* Measured on SCES_507.60 by the RetroAchievements community: the set for
 * game 1319, its r=patch achievement conditions retrieved 2026-09-04. Each
 * condition names a byte of the flag array and one of RA's bit prefixes
 * (M = bit 0 through T = bit 7), which is the same numbering the game's own
 * accessors use. code: the three of them are at 0x00181A48, 0x00181A70 and
 * 0x00181AA0 (the listing's gflagChk, gflagOn and gflagOff, disc
 * 0x00183B38, 0x00183B60 and 0x00183B90, 10 and 12 and 12 words, none with
 * a masked mismatch), and all three index the array the same way:
 * `sra $3, $4, 3` for the byte, `andi $4, $4, 7` for the bit, against the
 * base formed by `lui 0x2A` + `addiu 20672` (0x002A50C0). So bit index =
 * (byte address - RT_ICO_PROGRESS_BITS) * 8 + bit, and each index below
 * was recomputed from the JSON rather than copied from anyone's table.
 *
 * The array base RA uses is 0x002A50C0, which is independently what
 * correlation reports for the array (3/3 sites, above). That agreement is
 * what makes these indices usable here at all.
 *
 * What is measured and what is read: the bit index and the event RA's own
 * code note names for it are measured. Which PS3 trophy each event is comes
 * from matching the RA description against the published PS3 wording, which
 * is a reading of two English sentences. Where the two describe the same
 * moment in the game the reading is not in doubt; the pairs are listed with
 * both sentences so a reader can check the reading rather than take it.
 *
 *   trophy                RA achievement       byte  bit  RA code note
 *   Rescue                Moonlit Girl         0x03  3    Cage - Yorda pick up
 *   Failure               End of all Hope      0x11  1    1st Gate - Escape False Hope
 *   Armed and Ready       Riddle of Steel      0x19  0    Crest (Left 1) - Sword obtained
 *   East Gate             Light in the Dark    0x11  2    First Beacon Lit (map 50)
 *   West Gate             The Path is Open     0x11  3    Second Beacon Lit (map 51)
 *   Farewell              Road to Freedom      0x11  4    2nd Gate - Road to Freedom
 *   Royal Arms            The Blade Key        0x29  1    Jetty - Queen Sword Get
 *   Emancipation          You Were There       0x2C  3    Beach - Ending Regular
 *   Split the Watermelon  Watermelon Sharing   0x2C  2    Beach - Ending Watermelon
 *   Spiked Club           Mace-ive Advantage   0x0E  2    Sluice - Secret Weapon Get
 *   Shining Sword         Powered by Love      0x0E  2    the same bit, on a later run
 *
 * Bench Warmer ("Save at all save points") has no RA equivalent: the set
 * has no achievement for the benches, so no bit is named for them anywhere
 * and it stays unresolved. The diagnostic playthrough is what resolves it
 * (docs/ACHIEVEMENTS.md section 5).
 */
constexpr RtIcoTrophyBit RT_ICO_TROPHY_BITS[] = {
    {RT_ICO_TROPHY_RESCUE, 27},
    {RT_ICO_TROPHY_FAILURE, 137},
    {RT_ICO_TROPHY_ARMED_AND_READY, 200},
    {RT_ICO_TROPHY_EAST_GATE, 138},
    {RT_ICO_TROPHY_WEST_GATE, 139},
    {RT_ICO_TROPHY_FAREWELL, 140},
    {RT_ICO_TROPHY_ROYAL_ARMS, 329},
    {RT_ICO_TROPHY_EMANCIPATION, 355},
    {RT_ICO_TROPHY_SPLIT_THE_WATERMELON, 354},
    /* One bit for two weapons. The game sets bit 114 for whichever secret
     * weapon the run gives, and the run's number is what says which: the
     * mace on a first run, the light sword after a completed one. RA's two
     * achievements differ only in 0xH63aa00=0 against 0xH63aa00!=0, which
     * is RT_ICO_CLEAR_COUNT. */
    {RT_ICO_TROPHY_SPIKED_CLUB, 114, RT_ICO_BIT_CLEAR_FIRST_RUN},
    {RT_ICO_TROPHY_SHINING_SWORD, 114, RT_ICO_BIT_CLEAR_NEW_GAME_PLUS},
};
constexpr int RT_ICO_TROPHY_BIT_COUNT =
    (int)(sizeof(RT_ICO_TROPHY_BITS) / sizeof(RT_ICO_TROPHY_BITS[0]));

/* ---- the vendor sound library's voice state, for one diagnostic -----------
 *
 * Read only, and read only when a voice is keyed on with a volume of zero.
 * snd/engine.cpp uses these to say WHICH of the eight factors of
 * `_SgSeqSeVolume`'s product was zero, because the command stream carries
 * only the product. Nothing here is ever written, and no gameplay depends on
 * any of it: with RT_ICO_SND_FACTORS_KNOWN at 0 the runtime keeps the older
 * warn, which names the product but not the factor.
 *
 * Method for every address and offset in this block: the retail ELF. Each
 * word at the address quoted was read out of SCES_507.60 itself before
 * anything was decoded out of it, and matched the disc's own listing of the
 * same function; the reading covered `_SgGetSlotContext` 0x00273228..0x0027323C,
 * `_SgGetSeqContext` 0x00273240..0x00273254, `_SgGetVabContext`
 * 0x00273268..0x0027327C, `SgVabOpenFakeBody` 0x00276EE0 and
 * 0x00276EE8..0x00276F24, `_SgSeMain` 0x002739C8..0x00273A8C and
 * `_SgSeqSeVolume` 0x00275284..0x00275314. The addresses below are the
 * %hi/%lo pairs and the load/store displacements decoded from those words,
 * not values carried from any structure definition.
 *
 * Why the factors need three tables rather than one: the level is a product
 * of six halfwords of the keyed voice's slot context, one pan halfword of
 * the same context, and a word held by the sequence entry that owns the
 * voice; and two of the six are not stored in the slot at all until a
 * key-on copies them out of the opened bank's own SE table, which is
 * reached through the vab table. src/runtime/sif/SNDN2_NOTES.md carries the
 * whole chain with the instruction address for every term. */
#define RT_ICO_SND_FACTORS_KNOWN 1

/* _SgGetSlotContext (0x00273228): `lui $2, 0x0073` at 0x0027322C with
 * `addiu $2, $2, 0x1C00` at 0x00273234, and `addiu $3, $0, 0x58` at
 * 0x00273228 as the stride of the `mult $4, $4, $3` that indexes it. One
 * entry per SPU2 voice, 0x30 of them. */
constexpr uint32_t RT_ICO_SG_SLOT_CTX = 0x00731C00;
constexpr uint32_t RT_ICO_SG_SLOT_STRIDE = 0x58;
/* 0x30 entries: `_SgSeKeyOnSlot` scans that many, `slti $3, $18, 0x30` at
 * 0x00274DC0. */
constexpr uint32_t RT_ICO_SG_SLOT_COUNT = 0x30;

/* _SgGetSeqContext (0x00273240): `lui $2, 0x0073` at 0x00273244 with
 * `addiu $2, $2, 0x2C80` at 0x0027324C, stride 0x54 from 0x00273240. One
 * entry per playing sequence, 0x30 of them; `SgSePlay` (0x00277BB8) takes
 * the first free one for an SE. */
constexpr uint32_t RT_ICO_SG_SEQ_CTX = 0x00732C80;
constexpr uint32_t RT_ICO_SG_SEQ_STRIDE = 0x54;
/* 0x30 entries: `SgSePlay`'s search runs while `slti $2, $19, 0x30`
 * (0x00277D9C). */
constexpr uint32_t RT_ICO_SG_SEQ_COUNT = 0x30;

/* _SgGetVabContext (0x00273268): `lui $2, 0x0073` at 0x0027326C with
 * `addiu $2, $2, 0x1600` at 0x00273274, stride 0x0C from 0x00273268.
 * `SgVabOpenFakeBody` walks it from index 1 to 0x7F (0x00276F28's
 * `slti $2, $3, 0x80`). */
constexpr uint32_t RT_ICO_SG_VAB_CTX = 0x00731600;
constexpr uint32_t RT_ICO_SG_VAB_STRIDE = 0x0C;
constexpr uint32_t RT_ICO_SG_VAB_MAX = 0x80;

/* Vab entry +0x00 is the opened .hd image's EE address: `sw $16, 0x0($6)`
 * at 0x00276EE0, with $16 the .hd pointer `SgVabOpenFakeBody` was called
 * with. */
constexpr uint32_t RT_ICO_SG_VAB_HD = 0x00;

/* The .hd's own header holds file-relative offsets at +0x10, +0x18, +0x1C,
 * +0x20 and +0x24, and bank open rebases each into an absolute pointer at
 * +0x30, +0x38, +0x3C, +0x40 and +0x44 (0x00276EE8..0x00276F24). Only the
 * +0x20 / +0x40 pair is needed here: +0x40 is the SE table this runtime
 * reads, and +0x20 is that table's offset inside the .hd file, which is
 * what names the bytes if one of them is zero. */
constexpr uint32_t RT_ICO_SG_HD_SE_TABLE_OFF = 0x20;
constexpr uint32_t RT_ICO_SG_HD_SE_TABLE_PTR = 0x40;

/* Slot context fields. The six product terms are halfwords, read by
 * `_SgSeqSeVolume` at 0x00275284 (+0x16), 0x00275290 (+0x22), 0x00275294
 * (+0x1C), 0x002752A0 (+0x1A), 0x002752B0 (+0x18) and 0x002752BC (+0x1E);
 * the pan halfword at 0x002752CC (+0x20) supplies the left channel's factor
 * in its high byte and the right channel's in its low byte. */
constexpr uint32_t RT_ICO_SG_SLOT_CHAN_EXPRESSION = 0x16;
constexpr uint32_t RT_ICO_SG_SLOT_CHAN_VOLUME = 0x22;
constexpr uint32_t RT_ICO_SG_SLOT_TONE_VOLUME = 0x1C;
constexpr uint32_t RT_ICO_SG_SLOT_VELOCITY = 0x1A;
constexpr uint32_t RT_ICO_SG_SLOT_PROGRAM_VOLUME = 0x18;
constexpr uint32_t RT_ICO_SG_SLOT_SE_MASTER = 0x1E;
constexpr uint32_t RT_ICO_SG_SLOT_PAN = 0x20;

/* Slot context bookkeeping, all bytes written by `_SgSeMain` at key-on:
 * +0x4F at 0x002739CC (the sequence's own channel index, which for an SE is
 * the sequence slot number, `_SgTableEnvAdd` 0x00274ADC), +0x50 at
 * 0x002739D8 (the sequence entry that owns the voice), +0x54 at 0x00273A0C
 * (the vab id), and +0x51, which 0x002739E4 sets to 2 for an SE voice
 * against 1 for a BGM voice. */
constexpr uint32_t RT_ICO_SG_SLOT_SE_INDEX = 0x4F;
constexpr uint32_t RT_ICO_SG_SLOT_SEQ_INDEX = 0x50;
constexpr uint32_t RT_ICO_SG_SLOT_STATE = 0x51;
constexpr uint32_t RT_ICO_SG_SLOT_VAB_ID = 0x54;
constexpr uint8_t RT_ICO_SG_SLOT_STATE_SE = 2;

/* Sequence entry fields: the caller's own left and right level, written by
 * `SgSetSeVolDirect` (0x00278018 and 0x00278020) and read by
 * `_SgSeqSeVolume` at 0x002752E0 and 0x00275308. */
constexpr uint32_t RT_ICO_SG_SEQ_LEVEL_L = 0x44;
constexpr uint32_t RT_ICO_SG_SEQ_LEVEL_R = 0x48;

/* ---- the game's own SE slot, the source of the caller level ---------------
 *
 * `SgSetSeVolDirect` (PAL 0x00277FB8) is what puts a level in the sequence
 * entry's +0x44/+0x48, and its two callers are both inside
 * `sound3DParamSet` (PAL 0x00144C78): a hard (0, 0) mute, and
 * `soundSeVolSet` (PAL 0x001443F0), which computes the level from one entry
 * of the game's own 0x30-slot SE table. So when the sound block above finds
 * the caller level at zero and every bank byte intact, this table holds the
 * three inputs that produced the zero.
 *
 * Base and shape from `soundReqTickProc` (PAL 0x00146778): `lui $3, 0x006C`
 * at 0x0014677C with `addiu $16, $3, 0xF870` at 0x001467BC, `addiu $16,
 * $16, 0x40` at 0x0014685C for the stride, `slti $2, $17, 0x30` at
 * 0x00146854 for the count. Same method as the block above: every word
 * quoted was matched against SCES_507.60 first. */
constexpr uint32_t RT_ICO_SE_SLOTS = 0x006BF870;
constexpr uint32_t RT_ICO_SE_SLOT_STRIDE = 0x40;
constexpr uint32_t RT_ICO_SE_SLOT_COUNT = 0x30;

/* Slot fields, all written by `_soundSeDefPlay` (PAL 0x00145048) unless
 * said otherwise:
 *   +0x02 s16  the left level last sent, seeded to -1 at 0x0014540C so the
 *              first `soundSeVolSet` skips its 0x100-per-call slew
 *   +0x04 u32  flag word (0x00145414); its low half doubles as the right
 *              level last sent, seeded to -1 at 0x0014539C
 *   +0x08 s32  the second argument of `soundSeDefPlay` (0x00145428)
 *   +0x0C u32  the pad actuator id, or 0 (0x001454D4 / 0x001454D8)
 *   +0x10 s16  the sequence entry `SgSePlay` returned (0x00145454)
 *   +0x12 s16  left 3D scale, and +0x14 s16 right; both seeded to 0x1000 at
 *              0x00145408 / 0x00145430 and rewritten to 0x1000 by
 *              `sound3DParamSet` on entry (0x00144C90 / 0x00144C9C)
 *   +0x18 f32  the volume rate, either the caller's own float or, when that
 *              float is negative, the SE definition's +0x24
 *              (0x0014538C / 0x00145390)
 *   +0x30 u32  the sound data record; nonzero means the slot is in use
 *              (0x001454E0)
 *   +0x34 u32  the third argument, a position or 0 (0x00145404)
 *   +0x38 u32  the SE definition entry (0x0014536C)
 *   +0x3C u32  the seventh argument, a caller structure or 0 (0x00145424) */
constexpr uint32_t RT_ICO_SE_SLOT_LEVEL_L = 0x02;
constexpr uint32_t RT_ICO_SE_SLOT_FLAGS = 0x04;
constexpr uint32_t RT_ICO_SE_SLOT_ARG1 = 0x08;
constexpr uint32_t RT_ICO_SE_SLOT_ACTUATOR = 0x0C;
constexpr uint32_t RT_ICO_SE_SLOT_SEQ = 0x10;
constexpr uint32_t RT_ICO_SE_SLOT_SCALE_L = 0x12;
constexpr uint32_t RT_ICO_SE_SLOT_SCALE_R = 0x14;
constexpr uint32_t RT_ICO_SE_SLOT_RATE = 0x18;
constexpr uint32_t RT_ICO_SE_SLOT_RECORD = 0x30;
constexpr uint32_t RT_ICO_SE_SLOT_POS = 0x34;
constexpr uint32_t RT_ICO_SE_SLOT_DEF = 0x38;
constexpr uint32_t RT_ICO_SE_SLOT_CALLER = 0x3C;

/* Flag word bits that decide the level, all read in `sound3DParamSet` and
 * `soundSeVolSet`:
 *   0x20000000  mute. `soundSeVolSet` sends (0, 0) when it is set
 *               (`lui $3, 0x2000` 0x001443F4, `and` 0x00144410, `beqz`
 *               0x00144414 taking the compute path when it is CLEAR).
 *               `_soundSeDefPlay` clears it at 0x001453C8, and
 *               `soundReqTickProc` sets it per frame only for a slot whose
 *               +0x08 is neither -1 nor -2 (0x00146828, 0x00146830,
 *               0x0014683C) and clears it otherwise (0x00146840).
 *   0x01000000  the 3D distance path, taken from the SE definition's +0x38
 *               bit 7 at 0x00145368.
 *   0x02000000  selects `sound3DParamSet`'s own hard mute at 0x00144D4C. */
constexpr uint32_t RT_ICO_SE_SLOT_FLAG_MUTE = 0x20000000u;
constexpr uint32_t RT_ICO_SE_SLOT_FLAG_3D = 0x01000000u;
constexpr uint32_t RT_ICO_SE_SLOT_FLAG_DIRECT = 0x02000000u;

/* The SE definition table the slot's +0x38 points into, so the report can
 * name the sound effect rather than an address: `addiu $3, $0, 0x3C` at
 * 0x0014504C is the stride and `lui $2, 0x005D` at 0x00145058 with
 * `addiu $2, $2, 0x6DB0` at 0x00145064 is the base. Entry +0x24 is the
 * volume rate `_soundSeDefPlay` copies into the slot when the caller passes
 * a negative one. */
constexpr uint32_t RT_ICO_SE_DEF_TABLE = 0x005D6DB0;
constexpr uint32_t RT_ICO_SE_DEF_STRIDE = 0x3C;
constexpr uint32_t RT_ICO_SE_DEF_RATE = 0x24;

/* The one global level in this path: `soundSeVolSet` multiplies both
 * channels by the float here, but only for a slot whose +0x08 is exactly -1
 * and whose +0x3C is not 0 (0x00144464 and 0x0014446C). The read is
 * `lwc1 $f2, -0x64A4($28)` at 0x00144474 against the boot gp of 0x00640AF0.
 * `ExecIcoMisc` is its only writer and one of its three stores is a plain
 * zero (0x001B7EDC, 0x001B7F14, 0x001B7F2C), so a slot that takes this
 * branch can legitimately be silent. */
constexpr uint32_t RT_ICO_SE_GLOBAL_RATE = 0x0063A64C;

/* Where the two SE-table terms live, relative to the table pointer. The
 * key-on reads them at `_SgSeMain` 0x00273A34 (+0x1E, into slot +0x16) and
 * 0x00273A88 (+0x13, into slot +0x22), both indexed by the SE index above
 * times 0x10. The table's byte 0 is the SE master volume, read at
 * 0x00273A5C into slot +0x1E. */
constexpr uint32_t RT_ICO_SG_SE_ENTRY_STRIDE = 0x10;
constexpr uint32_t RT_ICO_SG_SE_ENTRY_EXPRESSION = 0x1E;
constexpr uint32_t RT_ICO_SG_SE_ENTRY_VOLUME = 0x13;

/* ---- the retail assert sink -----------------------------------------------
 *
 * The function the game's assert macro calls with the source file, the line
 * and the formatted message. The retail body is five `nop`s at
 * 0x001B6230..0x001B6240 and `b 0x001B6230` (0x1000FFFA) at 0x001B6244, so
 * a guest assert is a thread spinning for ever with no syscall, which no
 * inventory line can name. config/entry_hooks.txt asks the translator for
 * rt_entry_hook at this entry, and hooks.cpp logs the three arguments and
 * ends the run.
 *
 * code and listing, 2026-09-05, which is what says the address is the
 * assert sink even though the retail body was stripped: .text holds exactly
 * 44 `jal 0x001B6230` sites, SRCFILE.TXT holds exactly 44
 * `jal 1b8d40 <debug_assertMessage>` sites, and the two lists are in the
 * same order. 22 of the 43 consecutive gaps are equal word for word (the
 * first pair is 0x00102E98 and 0x00103494 in the ELF against 0x001030C8
 * and 0x001036C4 in the listing, both 0x5FC apart), and the 44 per-site
 * deltas take only 22 distinct values, 14 sites sharing one, which is what
 * two links of the same source look like. The listing's own
 * debug_assertMessage (disc 0x001B8D40, debug_exception.c:725) has a real
 * body; this build's is the stripped one.
 *
 * Measured on SCES_507.60, 2026-09-04: the PAL DATA.DF producer reached it
 * from a failed sceCdRead. Its three call sites are 0x0013332C, 0x0013339C
 * and 0x00133424, which are +0xDC, +0x14C and +0x1D4 into the function at
 * 0x00133250; the listing's `iosCdvdStManager` (disc 0x00134518) carries
 * its three at the same three offsets. */
constexpr uint32_t RT_ICO_DEBUG_ASSERT = 0x001B6230;

#endif /* ICORECOMP_GUEST_ICO_SYMS_H */
