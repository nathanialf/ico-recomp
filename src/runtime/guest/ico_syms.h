/* guest/ico_syms.h: guest RAM addresses of the retail ICO ELF's menu state.
 *
 * These are address facts taken from the decomp's symbol list, the same
 * class of fact as config/vendor_names.txt (an address paired with what
 * lives at it). Address facts are the one thing this header may contain:
 * no code, no game bytes, no content hashes. See the hard rules in
 * CLAUDE.md.
 *
 * All of these are 32-bit little-endian words in EE RAM. The runtime reads
 * all of them; the mouse pointer writes two, the current-item field of the
 * layout table entry (and its per-frame mirror) and the memory card check
 * screen's selector index. guest/menu_nav.cpp says why those two words are
 * the whole of the write, and how the scene object fields below turn into
 * a rectangle on the presented picture.
 */
#ifndef ICORECOMP_GUEST_ICO_SYMS_H
#define ICORECOMP_GUEST_ICO_SYMS_H

#include <cstdint>

/* Current layout (menu screen) id, read by the layout state machine. */
constexpr uint32_t RT_ICO_LAYOUT_ID = 0x0063314C;
/* Current selected item id; the game's item-select accessor returns it.
 * This word is a mirror: lt_next_layout copies the current layout table
 * entry's current-item field into it at the top of every frame, before any
 * handler reads it, so it lags the entry by one frame after a move. */
constexpr uint32_t RT_ICO_ITEM_ID = 0x00633150;
/* Layout fade/transition state. 2 is the interactive state: lt_link_layout
 * draws a highlight only at 2, and lt_switch_layout applies cross and
 * triangle only at 2. 3 and 5 are the two fading states lt_fade_status
 * sets. Gameplay also sits at 2, which is why fade alone is not the
 * predicate. */
constexpr uint32_t RT_ICO_FADE_STATE = 0x00633158;
/* The one-frame "swallow this frame's navigation" flag. lt_switch_layout
 * returns at once while it is non-zero, lt_next_layout skips lt_switch_layout
 * for every layout in the chain, and lt_link_layout draws no highlight;
 * lt_next_layout clears it on the way out, so it lasts exactly one frame.
 * Three writers were traced: lt_current_property_item stores 1 on the frame a
 * fade completes and the new screen becomes interactive (it sets the fade
 * state to 2 in the same breath), _la_set_preview_info stores 1 on a load or
 * save page whose preview info is not ready, and the title's kanbanBoot
 * setup (func_001B1800, func_001B19E8) stores 1 when there is no save to
 * continue from and 0 when there is. The pointer reads it and defers its
 * write for that frame, because a write the game will not navigate from is a
 * highlight moved behind the game's own back. */
constexpr uint32_t RT_ICO_NAV_SWALLOW = 0x00633160;

/* The memory card check screen's 15-position selector index (0..14). The
 * screen's own action function (_la_memory_card_check in the decomp's
 * src/layout_action.c) steps it with LEFT and RIGHT, clamped to 0 and 14,
 * and re-derives the highlight from it every frame through GetRealModelId,
 * so this word alone decides which position is lit. */
constexpr uint32_t RT_ICO_MC_SELECT = 0x00274EC0 + 0x2C;

/* ---- the layout table ----------------------------------------------------
 *
 * One entry per menu screen, indexed by the layout id in RT_ICO_LAYOUT_ID.
 * The fields below are the ones the pointer needs; the rest of the 0x38
 * bytes hold the screen's fade colour (+0x10..+0x1C, four floats
 * lt_prev_layout scales by 255 and 127) and the screen's action function
 * (+0x20) and that function's argument (+0x24).
 *
 * The current-item field is the selection: lt_link_layout draws the
 * highlight sprite for the one scene object whose index equals it, on every
 * frame, and lt_switch_layout is what a D-pad press updates it through.
 */
constexpr uint32_t RT_ICO_LAYOUT_TABLE = 0x0053C020;
constexpr uint32_t RT_ICO_LAYOUT_STRIDE = 0x38;
/* Entries 0x00..0x4A. What follows the last entry is unrelated data, so a
 * layout id at or past this is not a layout and nothing is read for it. */
constexpr uint32_t RT_ICO_LAYOUT_COUNT = 0x4B;
/* +0x00 and +0x04: the half-open range of scene object indices this layout
 * owns. lt_prev_layout walks exactly this range, so an item outside it can
 * never be the highlighted one. */
constexpr uint32_t RT_ICO_LAYOUT_FIRST_OBJ = 0x00;
constexpr uint32_t RT_ICO_LAYOUT_END_OBJ = 0x04;
/* +0x28: the item the screen starts on, and the seed the pointer walks the
 * neighbour graph from. -1 on a screen with nothing to select. */
constexpr uint32_t RT_ICO_LAYOUT_DEFAULT_ITEM = 0x28;
/* +0x2C: the current item, a scene object index in the range above. */
constexpr uint32_t RT_ICO_LAYOUT_CUR_ITEM = 0x2C;
/* +0x30: the parent layout id, or -1. lt_next_layout walks this chain. */
constexpr uint32_t RT_ICO_LAYOUT_PARENT = 0x30;

/* ---- the scene objects ---------------------------------------------------
 *
 * One entry per menu item, indexed by the scene object indices in the
 * layout entry's range. lt_link_layout is passed one of these per frame and
 * both draws it and, when its index equals the layout entry's current item,
 * draws the highlight over it. The position fields below are the ones it
 * turns into a sprite; guest/menu_nav.cpp carries the arithmetic and the
 * units.
 */
constexpr uint32_t RT_ICO_SCENE_OBJECTS = 0x002E81F0;
constexpr uint32_t RT_ICO_SCENE_STRIDE = 0x6C;

/* Navigation. lt_switch_layout stores the neighbour named by the D-pad bit
 * pressed this frame into the layout entry's current item, when it is not
 * negative; cross and triangle take the two target layout ids into
 * func_001B5C38. -1 in any of them means "no link that way". */
constexpr uint32_t RT_ICO_OBJ_TRIANGLE_TARGET = 0x24;
constexpr uint32_t RT_ICO_OBJ_CROSS_TARGET = 0x28;
constexpr uint32_t RT_ICO_OBJ_RIGHT = 0x2C;
constexpr uint32_t RT_ICO_OBJ_LEFT = 0x30;
constexpr uint32_t RT_ICO_OBJ_DOWN = 0x34;
constexpr uint32_t RT_ICO_OBJ_UP = 0x38;

/* Placement, all whole numbers in the game's own 2D layout space (see
 * guest/menu_nav.cpp for the space and the mapping onto the picture).
 * +0x40 non-zero centres the sprite horizontally and +0x50 is then unused.
 * +0x44 and +0x48 are the drawn size, and when either is zero the texture's
 * own size at +0x54 and +0x58 stands in for it. */
constexpr uint32_t RT_ICO_OBJ_CENTRED = 0x40;
constexpr uint32_t RT_ICO_OBJ_H = 0x44;
constexpr uint32_t RT_ICO_OBJ_W = 0x48;
constexpr uint32_t RT_ICO_OBJ_Y = 0x4C;
constexpr uint32_t RT_ICO_OBJ_X = 0x50;
constexpr uint32_t RT_ICO_OBJ_TEX_H = 0x54;
constexpr uint32_t RT_ICO_OBJ_TEX_W = 0x58;

/* +0x68, a bit field. Bit 1 set means lt_link_layout returns without
 * drawing the object at all, which is the game's own "this item is not on
 * screen": lt_next_layout copies bit 0 into bit 1 for every object in the
 * chain at the top of each frame, and func_001B7218 may then overwrite bit 1
 * for one object mid-frame. Bit 0 is the persistent per-object half of that
 * pair and nothing more: GetRealModelId(index, flag) sets it to flag & 1 and
 * leaves every other bit alone, and the retail title ships items 0xe and 0xf
 * with it set. The memory card check screen is one use of it and not its
 * meaning: _la_memory_card_check sets it on all fifteen card positions and
 * clears it on the one its selector names, which is how that screen lights a
 * position. */
constexpr uint32_t RT_ICO_OBJ_FLAGS = 0x68;
constexpr uint32_t RT_ICO_OBJ_FLAG_HIDDEN = 0x2;

/* The memory card check screen's fifteen card positions are scene objects
 * 0x158..0x166: _la_memory_card_check calls GetRealModelId(i + 0x158) for
 * all fifteen every frame and lights the one its selector names. They carry
 * the same placement fields as every other object, but they are not reached
 * through the neighbour links, so the pointer keys them by selector
 * position instead. */
constexpr uint32_t RT_ICO_MC_FIRST_OBJ = 0x158;
constexpr uint32_t RT_ICO_MC_COUNT = 15;

#endif /* ICORECOMP_GUEST_ICO_SYMS_H */
