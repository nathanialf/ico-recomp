/* guest/menu_nav.cpp: the mouse pointer for the game's own menus.
 *
 * See menu_nav.h for the two halves.
 *
 * ---- how a menu highlight moves -------------------------------------------
 *
 * The behaviour was traced against the decomp as a behavioural reference,
 * the way PCSX2 is read, and then checked word for word against
 * SCES_507.60. The decomp is not an input to this repo. Every address,
 * offset and flag bit in this comment is SCES_507.60's, from
 * guest/ico_syms.h, which says how each one was measured; several of them
 * differ from the decomp's own structures and that header says which. This
 * module names none of them: it reads the constants.
 *
 * The function names below are roles kept from that trace. The disc's own
 * listing names the same functions lt_switch_layout = default_item_select
 * (0x001BF220), lt_link_layout = display_texture (0x001BF960, in
 * layout_texture.c), lt_prev_layout = display_primary_texture_layout
 * (0x001C03E0), lt_next_layout = exec_layout_texture (0x001C06C8),
 * GetRealModelId = lt_default_mask_property (0x001C0F58) and
 * _la_set_preview_info = la_mc_file_select (0x001BAC08). guest/ico_syms.h
 * carries the match of each one against the retail bytes.
 *
 * The layout table is at 0x00533FE8, one 0x38-byte entry per menu screen id,
 * 0x50 entries; the scene objects a screen is built from are at 0x0030CFF8,
 * stride 0x70.
 *
 * The 2D space below was checked rather than assumed: lt_link_layout applies
 * the -320 and -113 biases (0x001BFA30 and 0x001BFA5C) and the sprite
 * emitter at 0x0010FFA0 divides by 640 and 224 while multiplying by the
 * frame buffer width and height words (0x0063A064 and 0x0063A068), so the
 * mapping onto the picture was read off this build's own draw path.
 *
 *   lt_next_layout runs once per frame. It walks the current layout's
 *   parent chain by each entry's +0x30 link, and for every layout in the
 *   chain it copies that entry's current-item field (+0x2C) into the item
 *   mirror at 0x0063B610, calls the entry's action function (+0x20) and then
 *   lt_switch_layout for that layout. The current layout is handled last:
 *   0x0063B610 is loaded from its +0x2C, lt_prev_layout draws it, and then
 *   lt_switch_layout runs on it.
 *
 *   lt_prev_layout calls the layout's action function and then
 *   lt_link_layout once per scene object in the layout's own range (entry
 *   +0x00 up to +0x04). lt_link_layout draws the highlight sprite for the
 *   one object whose index equals the entry's +0x2C, and only while the
 *   fade state at 0x0063B618 is 2. The highlight is re-derived from +0x2C on
 *   every frame; no press handler latches it anywhere else.
 *
 *   lt_switch_layout is where a D-pad press lands. It reads the game's
 *   pressed-this-frame pad word (not in guest/ico_syms.h: this module never
 *   reads it) and follows the current object's neighbour links (+0x30
 *   right, +0x34 left, +0x38 down, +0x3C up), storing the neighbour into
 *   the entry's +0x2C when it is not negative and playing the cursor sound
 *   when the value changed. Cross (0x40) and triangle (0x10) take the
 *   object's +0x2C and +0x28 target layout ids into the layout switch
 *   helper and leave the fade state at 3 or 5. A screen that installed a
 *   custom handler in the game's handler word (also not in ico_syms.h, for
 *   the same reason) has its +0x2C replaced by handler(+0x2C) instead of
 *   the neighbour walk; with no direction bit pressed the load grid's
 *   handler returns the item it was given, except that it moves on from an
 *   empty save slot to the next occupied one. That last behaviour is traced
 *   from the decomp as a reference and is not re-derived here: the handler
 *   is installed through a setter with 92 call sites in this build, so
 *   which function a given screen installs is not something this file can
 *   assert. Nothing here depends on it beyond the one-write rule below,
 *   which is what keeps the pointer from fighting it.
 *
 * One word therefore decides the highlight, and the screen keeps deriving
 * from it: the layout table entry's +0x2C. Writing it is the whole move,
 * and the game continues from it exactly as it would from its own. Three
 * consequences this file depends on:
 *
 *   - The mirror lags. 0x0063B610 is refreshed from +0x2C at the top of
 *     each frame, so between a write and the next frame it still holds the
 *     value from before the move. The selection is read back from +0x2C,
 *     never from the mirror, so a hover writes once instead of once per
 *     field. The mirror is written too, so anything that reads it before
 *     the game refreshes it sees a state that agrees with the entry.
 *
 *   - Cross applies to the item just written. lt_switch_layout resolves the
 *     object pointer from +0x2C on entry, before it applies that frame's
 *     D-pad bits, and the pad tick that writes the selection runs (from
 *     pad_field_tick, sif/pad.cpp) before the pad frame the game will read.
 *     A select and a cross in the same field confirm the new item, which is
 *     why a click's cross has to reach the pad on the field the click
 *     arrived on: rt_guest_menu_on_button starts the pulse itself when none
 *     is in flight, rather than leaving it for the next tick. A field of
 *     delay would put a whole game frame between the write and the cross,
 *     and the load grid moves the selection off an empty slot in exactly
 *     that frame.
 *
 *   - One frame in the game's own hands. 0x0063B620 (RT_ICO_NAV_SWALLOW,
 *     guest/ico_syms.h) is a one-frame flag that swallows navigation: while
 *     it is non-zero lt_switch_layout returns at once, lt_next_layout skips
 *     lt_switch_layout for every layout in the chain, and lt_link_layout
 *     draws no highlight.
 *     lt_next_layout clears it on the way out. Three writers were traced:
 *     lt_current_property_item sets it on the frame a fade completes and the
 *     new screen becomes interactive, _la_set_preview_info sets it on a load
 *     or save page whose preview info is not ready yet, and the title's
 *     kanbanBoot setup sets it when there is no save to continue from. The
 *     pointer's write does not go through lt_switch_layout, so nothing in the
 *     game would stop it landing on such a frame: it defers instead and
 *     retries on the next field, and the one-write rule keys on the write
 *     actually happening rather than on the hover being seen.
 *
 * The screen adjustment screen is the one screen that does not work that
 * way. Its action function is the listing's la_adjust_screen (0x001BE058,
 * layout_action.c), and it carries its own level word at 0x0028F4C0+0x2C
 * (RT_ICO_BRIGHTNESS): it steps that word with LEFT and RIGHT clamped to 0
 * and 14, stores 7 into it on triangle, and calls GetRealModelId for all
 * fifteen bar positions every frame to light the one the word names. The
 * same word is what gsb_controlBrightness reads every frame as the alpha of
 * a white full-frame sprite, so writing it is both the selection and the
 * brightness. There the level word is what the pointer writes and the
 * layout entry is left alone. guest/ico_syms.h carries both derivations.
 *
 * ---- the two boot screens the kanban system draws -------------------------
 *
 * The refresh rate choice (50 Hz or 60 Hz) and the language choice come up
 * before any of the screens above and are not layout-table screens: no
 * layout id word names them, lt_next_layout does not run them, and the fade
 * state at 0x0063B618 is not what gates them. A log from a run of this port
 * says exactly that. While both were on screen the pointer reported "layout
 * 0x0 chain 0x0: 0 items" and never took the mouse, while the player's
 * 50/60 choice arrived as SetGsCrt PAL/NTSC flips, so the game was reading
 * the pad and moving a selection the pointer could not see.
 *
 * They are drawn by the game's kanban system (the disc listing's kanban.c
 * and kanbanBoot.c), which keeps a list of its own, one node per screen, and
 * hands each node a layout table entry to take everything else from. Traced
 * in the decomp and then checked against the retail ELF word for word;
 * guest/ico_syms.h states that check and its result, and carries every
 * address:
 *
 *   kanbanReqAdd(id, priority) takes a free node out of the thirty slots at
 *   0x0071CB10, stores the layout table entry for `id` at the node's +0x00,
 *   copies that entry's default item (+0x28) into its current item (+0x2C)
 *   and, when that default is not -1, makes the node the one the game
 *   navigates: 0x0063C39C.
 *
 *   display_layout runs once a frame per node. For the node whose entry
 *   pointer matches 0x0063C39C's, and only while that node's +0x0C bit 0
 *   (the fading-out flag kanbanReqDelFade sets) is clear, it reads the same
 *   pressed-this-frame pad word the layout path reads and follows the
 *   current object's neighbour links, +0x30 right, +0x34 left, +0x38 down,
 *   +0x3C up, storing the neighbour into the entry's +0x2C when it is
 *   greater than zero. Cross (0x40) stores 1 into the node's +0x08 and
 *   triangle (0x10) stores 2; the boot state machine acts on the 1 and
 *   ignores the 2. Then it calls func_001B8B10 once per object in the
 *   entry's own range (+0x00 up to +0x04), which is lt_link_layout's twin:
 *   it draws the object and scatters the highlight over the one whose index
 *   equals the entry's +0x2C.
 *
 * So the word that decides the highlight on these two screens is the layout
 * table entry's +0x2C, the same word the pointer writes everywhere else, and
 * a click is still a cross on the virtual pad. Three things differ from the
 * layout path, each of them the game's own behaviour and not a
 * simplification:
 *
 *   - Which screen is up comes from 0x0063C39C, not from the layout id word
 *     and not from the fade state. The layout id word read 0 through both
 *     boot screens in that log, which is the language screen's own entry id
 *     by coincidence and not by the game putting it there.
 *
 *   - The item mirror 0x0063B610 is not written. lt_next_layout is what
 *     refreshes that word from a layout's +0x2C, and it does not run these
 *     screens, so writing it here would be a write the game does not make.
 *
 *   - The swallow flag 0x0063B620 is not read. display_layout and
 *     func_001B8B10 never look at it, so a hover on these screens writes on
 *     the field it happens on rather than deferring to the next one.
 *
 * The two entries are layout 0, the language choice, five items in a column,
 * and layout 1, the 50/60 Hz choice, two items in a row. Those ids are used
 * for the log line and nothing else: the screen is found through the node,
 * so the one other kanban screen that has selectable items, the memory card
 * message on layout 5 (the listing's la_boot_confirm_memory_card, two
 * objects and a default item), is pointed at by the same code without being
 * named.
 *
 * ---- where an item is on screen -------------------------------------------
 *
 * Also lt_link_layout, which is handed one scene object per frame and both
 * draws it and, when it is the selected one, scatters the highlight over
 * it. Its whole placement comes out of seven fields of the object, all of
 * them whole numbers:
 *
 *     w = +0x4C, or +0x64 (the texture's width) when +0x4C is zero
 *     h = +0x48, or +0x60 (the texture's height) when +0x48 is zero
 *     x = -w/2 when +0x44 is non-zero, otherwise +0x54 - 320
 *     y = +0x50 - 113
 *
 * There are two boxes built from those numbers, half a unit apart. The box
 * (x, y) to (x + w, y + h/2) is the one gif_SpriteOffset and the sprite
 * emitter at 0x0010FFA0 scatter the highlight over. The quad the object
 * itself is drawn as adds 8 to the Y and subtracts 8 from the H, both in GS
 * 12.4 fixed point, just before gif_StartPacketPath1, so it runs from
 * (x, y + 0.5) to (x + w,
 * y + h/2). This module reports the second, the drawn one; the half unit
 * between them is 0.002 of the picture's height, which is below the
 * resolution of the hit test and of the boxes the mapping was calibrated
 * against. The asymmetry in h is real and not a transcription slip: the
 * width fields are shifted left by 4 (whole pixels into the GS 12.4 fixed
 * point) and the height fields by 3, so a height field counts half units.
 * The centring case is written as (640*16 - w*16)/2 - 320*16, which is
 * algebraically -w*16/2, so the 640 in it says nothing about any screen.
 *
 * func_001B8B10, the kanban system's twin of lt_link_layout, reads the same
 * seven fields with the same shifts and the same -320 on x, and differs in
 * one number: its y bias is -112 (`addiu $2, $2, -0x70` at 0x001B8BE8) where
 * lt_link_layout's is -113 (`addiu $3, $3, -0x71` at 0x001BFA5C). That is
 * one line of 224, 0.004 of the picture's height. Each path is given the
 * bias its own emitter applies rather than one averaged number: the boot
 * screens are placed by the first and every other screen by the second.
 *
 * The space those numbers live in is 640 by 224, with the origin at the
 * centre. The sprite emitter this path calls (0x0010FFA0, the disc
 * listing's gif_PointOffset) scales every 2D coordinate by 0x0063A064/640
 * horizontally and 0x0063A068/224 vertically, where those two are the frame
 * buffer's width and height (RT_ICO_SCREEN_W and RT_ICO_SCREEN_H), and adds
 * the two 2D base words that sit just below them, 0x0063A05C and
 * 0x0063A060. Both bases are 2048.0: the whole of this build's .text has
 * three stores to them and all three are inside gsb_SetVSMatrix
 * (0x00114C04, 0x00114C0C and 0x00114C74), the first two from the constant
 * `lui $at, 0x4500`, which is 2048.0f. The frame's XYOFFSET is
 * (2048 - width/2, 2048 - height/2) (gsb_fade), which the GS subtracts from
 * the vertex. So a coordinate (x, y) lands on the frame buffer at
 *
 *     (width/2 + x * width/640, height/2 + y * height/224)
 *
 * and the frame buffer's own size cancels out of the fraction:
 *
 *     nx = 0.5 + x / 640      ny = 0.5 + y / 224
 *
 * which is why nothing here reads the frame buffer size or the scanout
 * geometry. The third store is the one that can leave a base other than
 * 2048.0: it subtracts a per-frame delta while a flag word is set, and the
 * only writers of that flag and that delta are 0x001C1F58 and the function
 * at 0x001C2340, the first of which is the listing's staffRollStart. The
 * staff roll is not a layout the pointer is ever up on.
 *
 * Calibrated against the retail game. The screen is the title's continue or
 * new game choice, which is layout 0x0C (the listing's
 * la_title_continue_or_new, 0x001BA870) with scene objects 49 and 50; its
 * default item is 49 and object 49's `down` link is 50, so the two are a
 * column of two. The user measured them on the presented picture as
 *
 *     upper item   x 0.399..0.607   y 0.595..0.685
 *     lower item   x 0.400..0.598   y 0.725..0.821
 *
 * The mapping above, run over those two objects as the retail ELF ships
 * them, gives
 *
 *     object 49    x 0.4000..0.6000  y 0.5982..0.6875
 *     object 50    x 0.3656..0.6344  y 0.7321..0.8214
 *
 * Object 49 agrees on both axes, worst case 0.007 of the picture, which is
 * inside the width of the line the boxes were drawn with. Object 50 agrees
 * on y and does NOT agree on x: it is 0.034 wider on each side than the
 * user measured. The reason is in the object as shipped: its width field
 * (+0x4C) is 0, so the mapping falls back to its texture width (+0x64),
 * which is 0xAC = 172 rather than the 128 the upper item has. That
 * fall-back is the game's own rule and was checked instruction for
 * instruction in lt_link_layout (`lw $a1, 76($s0)` at 0x001BF9A8,
 * `sll $a1, $a1, 4` at 0x001BF9C0, `bne $a1, $zero` at 0x001BF9DC with
 * `sw $a1, 24($sp)` in the delay slot and `sw $a3, 24($sp)` on the
 * fall-through, where $a3 is `lw $v0, 100($s0)` from 0x001BF990 shifted).
 *
 * Which of the two is wrong is not resolved. Either the game writes object
 * 50's +0x4C or +0x64 before that screen is drawn, in which case the static
 * object is not what the pointer sees and the earlier claim "as the retail
 * ELF ships them" was the wrong claim to make, or the lower item's x
 * measurement is off. It is left stated rather than papered over, and one
 * measurement settles it: this module already logs every screen's derived
 * rectangles the first time that screen is active, so a run that reaches
 * the title screen prints object 50's rectangle and either 0.3656..0.6344
 * or 0.400..0.598 comes out. guest/menu_nav_selftest.cpp carries the upper
 * item's arithmetic as a synthetic case.
 *
 * ---- a screen is a chain of layouts, not one ------------------------------
 *
 * lt_next_layout does not work on the current layout alone. Read in four
 * phases:
 *
 *   1. Walk the current layout's parent chain by each entry's +0x30 and
 *      push the ancestors onto a stack. On the way, for every object of
 *      every layout in the chain, copy +0x6C bit 5 into bit 4. Bit 4 is the
 *      per-frame "do not draw" flag lt_link_layout returns on and the scene
 *      manager rewrites mid-frame; bit 5 is the persistent one it is
 *      seeded from each frame. Bit 5 is general, not any one screen's:
 *      GetRealModelId (0x001C0F58) sets it to its second argument's low bit
 *      on any object and touches nothing else, and the retail title screen
 *      ships its own two items, scene objects 49 and 50, with it set (their
 *      +0x6C is 0x28). The screen adjustment screen is one caller of it,
 *      below, and not its meaning.
 *   2. For each ancestor, farthest first: load the mirror 0x0063B610 from
 *      its +0x2C, call its action function (+0x20) while the fade state is
 *      1 or 2, then lt_switch_layout for it, skipped when its +0x2C is
 *      negative.
 *   3. The current layout: mirror from its +0x2C, lt_prev_layout (which is
 *      what draws its objects through lt_link_layout), then
 *      lt_switch_layout for it.
 *   4. Every ancestor again, nearest first, running lt_link_layout over its
 *      whole object range. So the ancestors are drawn too, on top of the
 *      current layout.
 *
 * A page's items therefore need not belong to the layout id the state word
 * reports. The load file select page is the case that forced this. Read off
 * the retail layout table: the page is layout 0x13 (the listing's
 * la_mc_load_file_select), whose own range is objects 188..196 with no
 * default item, and its parent chain is 0x13 <- 0x0E <- 0x10 <- 0x0F. The
 * ten save slots belong to 0x0E (the listing's la_mc_file_select), whose
 * range is 52..73 with default item 62: walking the four neighbour links
 * from 62 inside that range reaches exactly ten objects, 62 through 71,
 * which is the grid. That is where their selection word lives and where the
 * screen's own item handler moves it.
 *
 * The save file select page, layout 0x21 (la_mc_save_file_select), has the
 * same chain through 0x0E. Every other page in the memory card flow reaches
 * its items the same way. Thirty-two of the table's layouts chain up to
 * 0x0F: 0x0E, 0x10, every id from 0x13 to 0x2A, then 0x2C, 0x2D, 0x2E,
 * 0x31, 0x33 and 0x34. Several of them have an empty range of their own.
 * All of them looked itemless before the chain was walked.
 *
 * Each layout in the chain keeps its own +0x2C, and the pointer writes the
 * one belonging to the item it is over. The mirror 0x0063B610 is written
 * only for the current layout, because phase 3 is last and leaves that
 * layout's field in it.
 *
 * ---- which items a layout has ---------------------------------------------
 *
 * A layout entry's range (+0x00 up to +0x04) is every object it draws,
 * decoration included. The ones that can be selected are the ones
 * lt_switch_layout can reach: start from the entry's default item (+0x28)
 * and its current item (+0x2C) and follow the four neighbour links, staying
 * inside the range. That set is exactly what the D-pad can walk to, so a
 * background object with no links is not a menu item and is not offered.
 *
 * A layout whose +0x2C is negative contributes nothing even when objects
 * are reachable from its default item. That is the game's own gate: phase 2
 * and phase 3 both skip lt_switch_layout for it, lt_switch_layout returns
 * immediately on it, and lt_link_layout highlights nothing. Writing an item
 * into it would hand it a highlight and a navigable selection the game did
 * not have. The chain log line names any layout skipped this way.
 *
 * An object whose +0x6C has bit 4 set is skipped: lt_link_layout returns
 * without drawing it, so there is nothing on screen to point at. It is
 * still walked through, because a hidden object can sit between two visible
 * ones in the link graph.
 *
 * The screen adjustment screen is keyed differently, for the same reason
 * its selection word is different: its fifteen bar positions are scene
 * objects 0x18B..0x199, reached by la_adjust_screen's own loop rather than
 * by any link, and the item value for one of them is its level 0..14. The
 * screen is recognised by its object range covering all fifteen of them,
 * which in the retail layout table only layout 0x3C does: 0x3C's range is
 * objects 392..420, and no other entry's range contains 395..409. Bit 4 is
 * not tested on those fifteen: la_adjust_screen calls GetRealModelId to set
 * bit 5 on all of them and clear it on the one the level names, so phase 1
 * leaves all but the selected reading as hidden. On this screen the general
 * bit 5 is being used as a lit marker, which says nothing about where the
 * fifteen places are.
 *
 * The "is a menu up" predicate is then: reads valid, fade 2, and at least
 * one selectable item with a rectangle that lands on the picture, anywhere
 * in the chain. Gameplay and the cinematics hold fade 2 as well, and they
 * are excluded structurally rather than by name, which is what keeps this
 * working without a gameplay layout id (guest/ico_syms.h says why that id
 * is still a sentinel): the retail layout table gives every entry whose
 * action function the disc listing names for one of those states an empty
 * object range (+0x00 equal to +0x04), a default item of -1 and no parent,
 * so there is no object to reach and no rectangle to derive. That covers
 * la_title_demo (0x0A), la_game_loading (0x35), la_game_loop (0x36),
 * la_game_demo (0x37) and la_switching_stage (0x3F).
 *
 * ---- the rules this file is built on --------------------------------------
 *
 *   - The pointer writes two guest words, both listed in guest/ico_syms.h,
 *     and nothing else. It never patches guest code. Cross, triangle and
 *     the wheel go through the virtual pad, because the handlers behind
 *     them read a pressed-this-frame word.
 *
 *   - A press is one field of bits followed by one field of zero. Bits held
 *     across fields arrive as one press followed by the game's own
 *     auto-repeat, which starts around 20 held fields later.
 *
 *   - One write per hover. The write happens on the field the cursor enters
 *     an item and not again while it stays there, whatever the game then
 *     does with the selection. The load grid moving on from an empty slot
 *     is exactly that case: it is the game's own rule, and a pointer that
 *     re-wrote every field would fight it forever.
 *
 *   - A write that cannot be shown correct does not happen. An item outside
 *     the layout's own scene object range can never be the highlighted one,
 *     so writing it would leave the screen with no highlight; it is refused
 *     with a log line, and a click that needed it presses nothing rather
 *     than confirming the item the game still has selected.
 *
 *   - Nothing is hovered means no cross. A left click over no item presses
 *     nothing at all: there is no item to confirm, and pressing cross
 *     anyway would confirm whatever the game happens to have selected
 *     somewhere else on the screen. The right button is not held to that
 *     rule and presses triangle wherever the cursor is, because triangle is
 *     back and back does not name an item. The wheel is the same: it moves
 *     the selection with the game's own direction presses, which need no
 *     item under the cursor either.
 *
 *   - The pointer acts for the SDL provider alone, and only with the
 *     settings overlay closed. rt_input_sdl_active() is the first: a run
 *     driven by ICORECOMP_INPUT_SCRIPT must be bit-identical, and a scripted
 *     run with a live window would otherwise have the OS cursor writing
 *     guest memory. rt_ui_wants_input() is the second: the overlay releases
 *     relative mouse mode, so the OS cursor lies over the picture and a drag
 *     across the settings menu would move the selection on the game's menu
 *     underneath it. host/input.cpp shuts the pad down on the same
 *     condition, and this drops the queue there for the same reason it
 *     drops the mouse events. The read and the change log are not gated:
 *     they write nothing.
 *
 *   - The cursor has two sources and one call. Relative mouse mode does not
 *     turn off for these menus, so the OS cursor is hidden and this module
 *     carries its own position, moved by the field's relative motion and
 *     drawn by ui/cursor.rml. With relative mode off (mouse look off, or
 *     the settings menu over the game's menu) the OS cursor is on screen
 *     and its position is mapped instead. rt_guest_menu_cursor() answers
 *     from whichever applies; nothing downstream knows the difference.
 *
 * The one difference from a player's move: lt_switch_layout plays the
 * cursor sound only when it applied the move itself, so a hover is silent.
 * Playing it would mean calling guest code.
 *
 * The change log (the tuple line) and the per-layout rectangle lines are on
 * by default rather than gated on ICORECOMP_VERBOSE, because the log they
 * have to appear in is produced on another machine by a user who sets no
 * environment variables.
 */
#include "menu_nav.h"

#include "gmem.h"
#include "ico_syms.h"

#include "../ee/kernel.h"
#include "../host/input.h"
#include "../runtime.h"
#include "../ui/ui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

/* The live cursor needs a window and the rectangle the last present blitted
 * the scanout into, and the window service owns both (host/window_service.h),
 * whichever renderer is live. Without SDL there is no window and so no
 * pointer: the honest answer is "no cursor", not a guessed one. */
#if defined(ICORECOMP_HAVE_SDL)
#define RT_MENU_NAV_HAVE_CURSOR 1
#include "../host/mouse.h"
#include "../host/window.h"
#include "../host/window_service.h"
#include <SDL3/SDL.h>
#endif

namespace {

/* ---- guest state --------------------------------------------------------- */

RtGuestMenuState g_state;

/* Last tuple that reached the log, so a value held across many fields costs
 * one line and not one per field. A stretch of unmapped fields does not
 * clear it: coming back to the same tuple the log already carries is not a
 * change worth a second line. */
bool g_have_logged = false;
RtGuestMenuState g_logged;

/* Bound on the log, so a game state that oscillates every field cannot turn
 * this into a per-field trace. One closing line says the log stopped rather
 * than letting it look like the state settled. */
constexpr uint32_t kMaxLines = 2000;
uint32_t g_lines = 0;
bool g_capped = false;

/* Bound on the per-action lines, the one a hover that moves the selection
 * writes and the one a click writes. Both are at info, which is a level the
 * user runs at, and a cursor swept across a menu crosses an item every few
 * fields, so without a bound they are a per-item trace. One closing line
 * says the log stopped rather than letting it read as a pointer that
 * stopped. */
constexpr uint32_t kMaxActionLines = 200;
uint32_t g_action_lines = 0;
bool g_action_log_capped = false;

/* One log line each, never repeated: a write this module refuses is retried
 * on every field the cursor stays on the item (see the hover block in
 * rt_guest_menu_tick), so an unlatched line here would be one per field. */
bool g_logged_adjust_range = false;
bool g_logged_adjust_unmapped = false;
bool g_logged_entry_unmapped = false;
bool g_logged_item_range = false;

/* The three accessors this module needs, with the "never fatal, false when
 * the page is unmapped" contract guest/gmem.h states once for the three
 * observer modules that share it. */
using rt_gmem::read_word;
using rt_gmem::read_i32;
using rt_gmem::write_word;

/* True while the per-action log still has room, counting the line it
 * allows. The first call past the bound writes one closing line and every
 * call after that is silent. */
bool take_action_line() {
    if (g_action_log_capped) return false;
    if (g_action_lines < kMaxActionLines) {
        ++g_action_lines;
        return true;
    }
    g_action_log_capped = true;
    rt_log_info("guest", "guest menu: %u pointer actions logged; the rest of this run's selects"
                    " and clicks are not logged",
           (unsigned)kMaxActionLines);
    return false;
}

/* The address of `layout`'s layout table entry. False when the id is past
 * the end of the table, or when the entry is not mapped. */
bool entry_addr(uint32_t layout, uint32_t* out) {
    if (layout >= RT_ICO_LAYOUT_COUNT) return false;
    const uint32_t addr = RT_ICO_LAYOUT_TABLE + layout * RT_ICO_LAYOUT_STRIDE;
    if (!rt_gptr(addr) || !rt_gptr(addr + RT_ICO_LAYOUT_STRIDE - 1)) return false;
    *out = addr;
    return true;
}

/* The address of scene object `index`. False for a negative index, for one
 * far enough out that the entry could not be in RAM at all, and for one
 * whose page is not mapped. */
bool object_addr(int32_t index, uint32_t* out) {
    if (index < 0 || index > 0xFFFF) return false;
    const uint32_t addr = RT_ICO_SCENE_OBJECTS + (uint32_t)index * RT_ICO_SCENE_STRIDE;
    if (!rt_gptr(addr) || !rt_gptr(addr + RT_ICO_SCENE_STRIDE - 1)) return false;
    *out = addr;
    return true;
}

/* ---- the screen mapping -------------------------------------------------- */

/* The game's own 2D layout space: 640 by 224 with the origin at the centre
 * of the picture. The module header derives all four numbers from the draw
 * path; 113 rather than 112 is the bias lt_link_layout itself applies, and
 * it is kept as the game wrote it. */
constexpr float kSpaceW = 640.0f;
constexpr float kSpaceH = 224.0f;
constexpr int32_t kOriginX = 320;
constexpr int32_t kOriginY = 113;
/* The same space, as the kanban system's own emitter biases it: 112 rather
 * than 113. The two functions were read separately and each screen is given
 * the number its own emitter applies; the module header quotes both
 * instructions. */
constexpr int32_t kKanbanOriginY = 112;

/* Bound on how many objects one layout's range may hold before this module
 * refuses to walk it. The longest range in the retail layout table is 101
 * objects; anything past this is a range that did not come from the game. */
constexpr int32_t kMaxRangeLen = 512;

/* ---- the current screen's items ------------------------------------------ */

std::vector<RtGuestMenuItem> g_items;
/* The chain lt_next_layout processed for the current screen, current layout
 * first, then its parents. g_adjust_layouts holds the ones in it that are
 * the screen adjustment screen, whose selection word is that screen's own
 * level rather than the layout entry's current item. */
std::vector<uint32_t> g_chain;
std::vector<uint32_t> g_adjust_layouts;
/* A chain layout that has reachable objects but whose +0x2C is negative, so
 * the game itself will not navigate it this frame. Named in the chain log
 * because "the page looks right but the pointer does nothing" is otherwise
 * indistinguishable from a bad mapping. */
std::vector<uint32_t> g_chain_no_selection;

/* Set while the screen the items came from is one of the kanban system's
 * (the two PAL boot screens and the memory card message; see the module
 * header). It changes four things and nothing else: the screen is found
 * through 0x0063C39C rather than the layout id word, "is a menu up" does not
 * ask for fade 2, the item mirror is not written, and the swallow flag is
 * not read. The selection word written is the same layout entry field. */
bool g_kanban_active = false;
uint32_t g_kanban_layout = 0;

/* Logged once each: a repeat says nothing the first line did not. */
bool g_logged_kanban_node = false;
bool g_logged_kanban_entry = false;
bool g_logged_kanban_no_items = false;
bool g_logged_kanban_no_selection = false;

/* Layouts whose chain and rectangles have already been logged, and the
 * bound on that list: a run that walks a lot of screens still costs a
 * bounded number of lines. */
std::vector<uint32_t> g_logged_layouts;
/* The kanban screens are kept apart from those, because the two name their
 * screens out of the same numbering: the layout id word read 0 through both
 * boot screens, which is also the language screen's own entry id, and one
 * list would have let the first of them spend the other's line. */
std::vector<uint32_t> g_logged_kanban_layouts;
constexpr size_t kMaxLoggedLayouts = 64;
bool g_layout_log_capped = false;
bool g_logged_long_range = false;
bool g_logged_long_chain = false;
bool g_logged_negative_size = false;

/* lt_next_layout keeps the ancestors it walked in one stack frame, which
 * bounds the chain well under this. A longer walk than this is a table that
 * did not come from the game, or a cycle the visited set did not catch. */
constexpr size_t kMaxChain = 16;

bool layout_is_adjust(uint32_t layout) {
    for (uint32_t l : g_adjust_layouts) {
        if (l == layout) return true;
    }
    return false;
}

/* Fills `out`'s rectangle from scene object `index`, in normalized units of
 * the presented picture. False when the object cannot be read, when
 * lt_link_layout would not draw it (the hidden bit, or a zero size), or
 * when the rectangle it places lands entirely off the picture, which is
 * what layout 0x38's own default item does.
 *
 * `lit_flag` says the hidden bit on this object is the game's own lit
 * marker rather than a decision about whether the item is on screen, which
 * is the screen adjustment screen's fifteen bar positions and nothing
 * else: lt_next_layout copies bit 5 into bit 4 for every object in the
 * chain at the top of each frame, and la_adjust_screen sets bit 5 on all
 * fifteen and clears it on the selected one, so all but the selected read
 * as hidden. Their places on the picture do not move with it. */
bool object_rect(uint32_t layout, int32_t index, bool lit_flag, int32_t origin_y,
                 RtGuestMenuItem* out) {
    uint32_t base = 0;
    if (!object_addr(index, &base)) return false;

    int32_t centred = 0, h = 0, w = 0, y = 0, x = 0, tex_h = 0, tex_w = 0, flags = 0;
    if (!read_i32(base + RT_ICO_OBJ_CENTRED, &centred) ||
        !read_i32(base + RT_ICO_OBJ_H, &h) ||
        !read_i32(base + RT_ICO_OBJ_W, &w) ||
        !read_i32(base + RT_ICO_OBJ_Y, &y) ||
        !read_i32(base + RT_ICO_OBJ_X, &x) ||
        !read_i32(base + RT_ICO_OBJ_TEX_H, &tex_h) ||
        !read_i32(base + RT_ICO_OBJ_TEX_W, &tex_w) ||
        !read_i32(base + RT_ICO_OBJ_FLAGS, &flags)) {
        return false;
    }
    if (!lit_flag && (flags & (int32_t)RT_ICO_OBJ_FLAG_HIDDEN)) return false;

    if (w == 0) w = tex_w;
    if (h == 0) h = tex_h;
    if (w < 0 || h < 0) {
        /* A zero size is the game's own "nothing is drawn here" and is
         * silent, but a negative one is a size no sprite path produces: the
         * object is dropped, and the line says which one so a screen that
         * quietly lost an item can be told apart from a bad mapping. */
        if (!g_logged_negative_size) {
            g_logged_negative_size = true;
            rt_log_warn("guest", "guest menu: layout 0x%x object 0x%x has a negative drawn size"
                            " (w %d, h %d); it is not an item (this line is not repeated)",
                   (unsigned)layout, (unsigned)index, (int)w, (int)h);
        }
        return false;
    }
    if (w == 0 || h == 0) return false;

    /* The drawn size in the layout space. The height fields count half
     * units, which is the <<3 against the width fields' <<4. */
    const float sw = (float)w;
    const float sh = (float)h * 0.5f;
    const float sx = centred != 0 ? -0.5f * sw : (float)(x - kOriginX);
    const float sy = (float)(y - origin_y);

    out->x0 = 0.5f + sx / kSpaceW;
    out->x1 = 0.5f + (sx + sw) / kSpaceW;
    out->y0 = 0.5f + sy / kSpaceH;
    out->y1 = 0.5f + (sy + sh) / kSpaceH;

    /* An item drawn entirely off the picture cannot be pointed at. One
     * partly off it can, so the rectangle is not clamped. */
    return out->x1 > 0.0f && out->x0 < 1.0f && out->y1 > 0.0f && out->y0 < 1.0f;
}

/* Whether `index` is already in `order`. The graphs are a dozen nodes at
 * most, so a linear scan is the whole of the visited set. */
bool contains(const std::vector<int32_t>& order, int32_t index) {
    for (int32_t v : order) {
        if (v == index) return true;
    }
    return false;
}

bool contains(const std::vector<uint32_t>& seen, uint32_t layout) {
    for (uint32_t v : seen) {
        if (v == layout) return true;
    }
    return false;
}

/* The chain lt_next_layout processes for the current screen: the current
 * layout, then its parent by +0x30, and so on until the link is negative.
 * Every layout in it gets its action function and lt_switch_layout, and its
 * objects are drawn (the current layout by lt_prev_layout, the ancestors by
 * the second loop at the end of lt_next_layout), so an item may belong to
 * any of them. */
void build_chain() {
    uint32_t layout = g_state.layout;
    for (size_t depth = 0; depth < kMaxChain; ++depth) {
        uint32_t entry = 0;
        if (!entry_addr(layout, &entry)) return;
        g_chain.push_back(layout);
        int32_t parent = 0;
        if (!read_i32(entry + RT_ICO_LAYOUT_PARENT, &parent)) return;
        if (parent < 0 || parent >= (int32_t)RT_ICO_LAYOUT_COUNT) return;
        if (contains(g_chain, (uint32_t)parent)) return;
        layout = (uint32_t)parent;
    }
    if (!g_logged_long_chain) {
        g_logged_long_chain = true;
        rt_log_warn("guest", "guest menu: layout 0x%x's parent chain is longer than %zu; the rest is"
                        " not walked (this line is not repeated)",
               (unsigned)g_state.layout, kMaxChain);
    }
}

/* Appends one chain layout's selectable items to g_items. */
void add_layout_items(uint32_t layout) {
    uint32_t entry = 0;
    if (!entry_addr(layout, &entry)) return;

    int32_t first = 0, end = 0;
    if (!read_i32(entry + RT_ICO_LAYOUT_FIRST_OBJ, &first) ||
        !read_i32(entry + RT_ICO_LAYOUT_END_OBJ, &end)) {
        return;
    }
    if (end <= first) return; /* gameplay and the cinematics land here */

    /* The screen adjustment screen: its fifteen bar positions are the
     * objects from RT_ICO_ADJUST_FIRST_OBJ (0x18B) on, and the item value
     * is the position 0..14 rather than a scene object index. */
    if (first <= (int32_t)RT_ICO_ADJUST_FIRST_OBJ &&
        end >= (int32_t)(RT_ICO_ADJUST_FIRST_OBJ + RT_ICO_ADJUST_COUNT)) {
        g_adjust_layouts.push_back(layout);
        for (uint32_t i = 0; i < RT_ICO_ADJUST_COUNT; ++i) {
            RtGuestMenuItem it;
            it.layout = layout;
            it.item = i;
            if (object_rect(layout, (int32_t)(RT_ICO_ADJUST_FIRST_OBJ + i), true, kOriginY,
                            &it)) {
                g_items.push_back(it);
            }
        }
        return;
    }

    if (end - first > kMaxRangeLen) {
        if (!g_logged_long_range) {
            g_logged_long_range = true;
            rt_log_warn("guest", "guest menu: layout 0x%x claims %d scene objects, past the %d this"
                            " module walks; no items (this line is not repeated)",
                   (unsigned)layout, (int)(end - first), (int)kMaxRangeLen);
        }
        return;
    }

    /* Everything lt_switch_layout can reach, from where the screen starts
     * and from where it is now. */
    std::vector<int32_t> order;
    int32_t seed = 0;
    if (read_i32(entry + RT_ICO_LAYOUT_DEFAULT_ITEM, &seed) && seed >= first && seed < end) {
        order.push_back(seed);
    }
    int32_t cur = -1;
    const bool have_cur = read_i32(entry + RT_ICO_LAYOUT_CUR_ITEM, &cur);
    if (have_cur && cur >= first && cur < end && !contains(order, cur)) {
        order.push_back(cur);
    }

    static const uint32_t kLinks[4] = {RT_ICO_OBJ_RIGHT, RT_ICO_OBJ_LEFT,
                                       RT_ICO_OBJ_DOWN, RT_ICO_OBJ_UP};
    for (size_t i = 0; i < order.size(); ++i) {
        uint32_t base = 0;
        if (!object_addr(order[i], &base)) continue;
        for (uint32_t off : kLinks) {
            int32_t n = 0;
            if (!read_i32(base + off, &n)) continue;
            if (n < first || n >= end || contains(order, n)) continue;
            order.push_back(n);
        }
    }
    if (order.empty()) return;

    /* The game's own gate on whether this layout is navigable at all:
     * lt_next_layout skips lt_switch_layout for a layout whose +0x2C is
     * negative, lt_switch_layout returns immediately on one, and
     * lt_link_layout highlights nothing. Writing an item into it would give
     * it a highlight and a navigable selection the game did not have. */
    if (!have_cur || cur < first || cur >= end) {
        if (!contains(g_chain_no_selection, layout)) g_chain_no_selection.push_back(layout);
        return;
    }

    for (int32_t index : order) {
        RtGuestMenuItem it;
        it.layout = layout;
        it.item = (uint32_t)index;
        if (object_rect(layout, index, false, kOriginY, &it)) g_items.push_back(it);
    }
}

/* ---- the kanban system's screens ------------------------------------------ */

/* The name of one of them, for the log. Both ids are measured
 * (guest/ico_syms.h) and neither decides anything: the screen is found
 * through the node, and an id this does not know is still pointed at. */
const char* kanban_screen_name(uint32_t layout) {
    if (layout == RT_ICO_KANBAN_LAYOUT_LANGUAGE) return "the language screen";
    if (layout == RT_ICO_KANBAN_LAYOUT_VIDEO_MODE) return "the 50/60 Hz screen";
    return "a boot screen";
}

/* The layout entry of the kanban screen the game is navigating, as a layout
 * id, or false when there is none. This is the whole of "which screen is up"
 * on the boot screens, and it is the game's own answer: 0x0063C39C is the
 * node kanbanReqAdd made navigable, its +0x00 is the layout table entry the
 * screen is built from, and its +0x0C bit 0 is the flag display_layout gives
 * a node no input at all through.
 *
 * Every value that is not one of the thirty node slots, and every entry
 * pointer that is not a row of the layout table, is refused with a line
 * naming the value: those are the two ways this could be reading something
 * that is not a kanban node, and acting on either would write a word chosen
 * by an unrelated number. */
bool kanban_screen(uint32_t* out_layout) {
    uint32_t node = 0;
    if (!read_word(RT_ICO_KANBAN_ACTIVE, &node) || node == 0) return false;

    const uint32_t nodes_end =
        RT_ICO_KANBAN_NODES + RT_ICO_KANBAN_NODE_COUNT * RT_ICO_KANBAN_NODE_STRIDE;
    if (node < RT_ICO_KANBAN_NODES || node >= nodes_end ||
        ((node - RT_ICO_KANBAN_NODES) % RT_ICO_KANBAN_NODE_STRIDE) != 0) {
        if (!g_logged_kanban_node) {
            g_logged_kanban_node = true;
            rt_log_warn("guest", "guest menu: the kanban screen word at 0x%08x holds 0x%08x, which"
                            " is not one of the thirty node slots at 0x%08x; the pointer leaves"
                            " these screens alone (this line is not repeated)",
                   (unsigned)RT_ICO_KANBAN_ACTIVE, (unsigned)node,
                   (unsigned)RT_ICO_KANBAN_NODES);
        }
        return false;
    }

    uint32_t entry = 0;
    if (!read_word(node + RT_ICO_KANBAN_NODE_LAYOUT, &entry)) return false;
    const uint32_t table_end = RT_ICO_LAYOUT_TABLE + RT_ICO_LAYOUT_COUNT * RT_ICO_LAYOUT_STRIDE;
    if (entry < RT_ICO_LAYOUT_TABLE || entry >= table_end ||
        ((entry - RT_ICO_LAYOUT_TABLE) % RT_ICO_LAYOUT_STRIDE) != 0) {
        if (!g_logged_kanban_entry) {
            g_logged_kanban_entry = true;
            rt_log_warn("guest", "guest menu: kanban node 0x%08x names layout entry 0x%08x, which is"
                            " not a row of the layout table at 0x%08x; the pointer leaves this"
                            " screen alone (this line is not repeated)",
                   (unsigned)node, (unsigned)entry, (unsigned)RT_ICO_LAYOUT_TABLE);
        }
        return false;
    }

    uint32_t flags = 0;
    if (!read_word(node + RT_ICO_KANBAN_NODE_FLAGS, &flags)) return false;
    /* Fading out. display_layout gives such a node no input, so neither does
     * the pointer; it is still drawn, which is why this is not an error. */
    if (flags & RT_ICO_KANBAN_FLAG_FADING) return false;

    *out_layout = (entry - RT_ICO_LAYOUT_TABLE) / RT_ICO_LAYOUT_STRIDE;
    return true;
}

/* The selectable items of one kanban screen, appended to g_items. The same
 * walk as the layout path, from the entry's default and current item along
 * the four neighbour links inside the entry's own object range, with the two
 * differences display_layout has: a link is followed only when it is greater
 * than zero (`bgtzl`/`blezl`, not the layout path's "not negative"), and the
 * rectangle is placed with the kanban emitter's own y bias. */
void add_kanban_items(uint32_t layout) {
    uint32_t entry = 0;
    if (!entry_addr(layout, &entry)) return;

    int32_t first = 0, end = 0;
    if (!read_i32(entry + RT_ICO_LAYOUT_FIRST_OBJ, &first) ||
        !read_i32(entry + RT_ICO_LAYOUT_END_OBJ, &end)) {
        return;
    }
    if (end <= first) return;
    if (end - first > kMaxRangeLen) {
        if (!g_logged_long_range) {
            g_logged_long_range = true;
            rt_log_warn("guest", "guest menu: layout 0x%x claims %d scene objects, past the %d this"
                            " module walks; no items (this line is not repeated)",
                   (unsigned)layout, (int)(end - first), (int)kMaxRangeLen);
        }
        return;
    }

    int32_t cur = -1;
    if (!read_i32(entry + RT_ICO_LAYOUT_CUR_ITEM, &cur) || cur < first || cur >= end) {
        /* kanbanReqAdd copies the entry's default item into this field when
         * it makes the node navigable, and the default is what said the node
         * was navigable at all, so this is a state the game does not
         * normally reach. Without it there is no highlight to move and
         * nothing says which item the game would confirm. */
        if (!g_logged_kanban_no_selection) {
            g_logged_kanban_no_selection = true;
            rt_log_warn("guest", "guest menu: the kanban screen on layout 0x%x has a current item of"
                            " %d, outside its objects 0x%x..0x%x; the pointer does nothing on it"
                            " (this line is not repeated)",
                   (unsigned)layout, (int)cur, (unsigned)first, (unsigned)(end - 1));
        }
        return;
    }

    std::vector<int32_t> order;
    int32_t seed = 0;
    if (read_i32(entry + RT_ICO_LAYOUT_DEFAULT_ITEM, &seed) && seed >= first && seed < end) {
        order.push_back(seed);
    }
    if (!contains(order, cur)) order.push_back(cur);

    static const uint32_t kLinks[4] = {RT_ICO_OBJ_RIGHT, RT_ICO_OBJ_LEFT,
                                       RT_ICO_OBJ_DOWN, RT_ICO_OBJ_UP};
    for (size_t i = 0; i < order.size(); ++i) {
        uint32_t base = 0;
        if (!object_addr(order[i], &base)) continue;
        for (uint32_t off : kLinks) {
            int32_t n = 0;
            if (!read_i32(base + off, &n)) continue;
            if (n <= 0 || n < first || n >= end || contains(order, n)) continue;
            order.push_back(n);
        }
    }

    for (int32_t index : order) {
        RtGuestMenuItem it;
        it.layout = layout;
        it.item = (uint32_t)index;
        if (object_rect(layout, index, false, kKanbanOriginY, &it)) g_items.push_back(it);
    }
}

/* Rebuilds g_items for whatever screen the tick just read: every layout in
 * the chain, current layout first. That is also the order the game draws
 * them in, since lt_next_layout draws the current layout (phase 3) and then
 * the ancestors nearest first (phase 4), so a later item in the list is
 * drawn on top of an earlier one and hit_test's "last match wins" agrees
 * with the picture. Called once per tick, before anything reads them. */
void rebuild_items() {
    g_items.clear();
    g_adjust_layouts.clear();
    g_chain_no_selection.clear();
    g_chain.clear();
    g_kanban_active = false;
    g_kanban_layout = 0;
    if (!g_state.valid) return;

    /* A kanban screen first, because it is the game's own answer to "which
     * screen is up" whenever there is one: 0x0063C39C is set only while such
     * a screen is navigable. It is not a chain; each node carries one layout
     * entry and display_layout draws that entry's own object range. */
    uint32_t kanban_layout = 0;
    if (kanban_screen(&kanban_layout)) {
        add_kanban_items(kanban_layout);
        if (!g_items.empty()) {
            g_kanban_active = true;
            g_kanban_layout = kanban_layout;
            g_chain.push_back(kanban_layout);
            return;
        }
        /* A screen the game is navigating whose items this module cannot
         * place. Nothing is offered and the layout path is tried, which is
         * what the module did before these screens were handled at all. */
        if (!g_logged_kanban_no_items) {
            g_logged_kanban_no_items = true;
            rt_log_warn("guest", "guest menu: the kanban screen on layout 0x%x offers no item with a"
                            " rectangle on the picture; the pointer does nothing on it"
                            " (this line is not repeated)",
                   (unsigned)kanban_layout);
        }
    }

    build_chain();
    for (uint32_t layout : g_chain) add_layout_items(layout);
}

/* The chain and one line per item, the first time a screen is interactive.
 * This is the measurement the mapping above is checked against on a real
 * machine, and the chain line is what says which layout a page's items
 * actually came from. */
void log_layout_rects() {
    if (g_layout_log_capped || g_chain.empty()) return;
    std::vector<uint32_t>& seen = g_kanban_active ? g_logged_kanban_layouts : g_logged_layouts;
    const uint32_t key = g_kanban_active ? g_kanban_layout : g_state.layout;
    if (contains(seen, key)) return;
    if (g_logged_layouts.size() + g_logged_kanban_layouts.size() >= kMaxLoggedLayouts) {
        g_layout_log_capped = true;
        rt_log_debug("guest", "guest menu: further layout rectangles not logged");
        return;
    }
    seen.push_back(key);

    if (g_kanban_active) {
        /* The layout state word is on this line because it is what the
         * pointer used to go by, and a log that carries both settles at a
         * glance whether the layout system was up at the same time. */
        rt_log_info("guest", "guest menu: kanban layout 0x%x (%s): %zu items; the layout state word"
                        " says layout 0x%x fade %u",
               (unsigned)g_kanban_layout, kanban_screen_name(g_kanban_layout), g_items.size(),
               (unsigned)g_state.layout, (unsigned)g_state.fade);
    } else {
        std::string chain;
        for (size_t i = 0; i < g_chain.size(); ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%x", (unsigned)g_chain[i]);
            if (i) chain += " <- ";
            chain += buf;
        }
        rt_log_info("guest", "guest menu: layout 0x%x chain %s: %zu items",
               (unsigned)g_state.layout, chain.c_str(), g_items.size());
    }
    for (uint32_t layout : g_chain_no_selection) {
        rt_log_info("guest", "guest menu: layout 0x%x is in the chain and has reachable objects, but"
                        " its current item is negative; it contributes none",
               (unsigned)layout);
    }
    for (const RtGuestMenuItem& it : g_items) {
        rt_log_debug("guest", "guest menu: layout 0x%x item 0x%x rect %.4f,%.4f,%.4f,%.4f",
               (unsigned)it.layout, (unsigned)it.item,
               (double)it.x0, (double)it.y0, (double)it.x1, (double)it.y1);
    }
}

/* ---- pulses and the hover ------------------------------------------------ */

/* Pulses waiting for the field boundary: a click's cross, a right click's
 * triangle, the wheel's steps. Bounded so a spun wheel cannot queue a
 * minute of presses; an overflow drops the newest and says so. */
constexpr size_t kMaxQueue = 8;
std::deque<uint16_t> g_queue;
bool g_queue_dropped = false;

/* Pulse cadence. Phase 1 is the held field, phase 2 the released field,
 * phase 0 no pulse. rt_guest_menu_pulse_bits only reads this; the tick is
 * the only thing that advances it. */
int g_pulse_phase = 0;
uint16_t g_pulse_bits = 0;

uint64_t g_field = 0;
bool g_have_field = false;
bool g_logged_field_mismatch = false;

/* The item the pointer has already acted on, as the layout that owns it and
 * its index: the hover that entered it either wrote the selection or found
 * the item already selected. Cleared when the cursor leaves it, when the
 * screen changes and when the menu stops being interactive, so entering it
 * again acts again. This is what holds the pointer to one write per hover
 * when the game moves the selection somewhere else afterwards. */
bool g_hover_acted = false;
uint32_t g_hover_layout = 0;
uint32_t g_hover_item = 0;

/* Logged once each: a repeat says nothing the first line did not. */
bool g_logged_no_selection = false;
bool g_logged_swallowed = false;

/* ---- the drawn cursor ----------------------------------------------------
 *
 * While relative mouse mode is on the OS cursor is hidden and there is no
 * absolute position to map, so the pointer carries its own: a normalized
 * position in the presented rectangle, moved by the same relative motion
 * that would otherwise have gone to the camera stick, and drawn by
 * ui/cursor.rml. It survives a menu change: taking the mouse again puts the
 * cursor back where it was left, which is what a system cursor would do.
 */
bool g_vc_valid = false;                       /* has it ever taken the mouse */
float g_vc_x = 0.5f, g_vc_y = 0.5f;

/* Whether the pointer owned the mouse on the previous tick, for the two
 * transition log lines, and which screen it owned it for: the hand-back line
 * names the screen it is leaving, which is gone from the state words by the
 * time it is printed. */
bool g_had_mouse = false;
bool g_had_mouse_known = false;
bool g_had_kanban = false;
uint32_t g_had_kanban_layout = 0;

/* Bound on those two lines, on the same principle as the tuple log above: a
 * menu state that oscillates must not turn this into a per-field trace. */
constexpr uint32_t kMaxOwnerLines = 200;
uint32_t g_owner_lines = 0;
bool g_owner_capped = false;

/* Logged once each, because a repeat says nothing the first line did not. */
bool g_logged_motion_no_rect = false;
bool g_logged_motion_nonfinite = false;

/* SDL button indices (host/mouse.h names the convention). */
constexpr uint8_t kSdlButtonLeft = 1;
constexpr uint8_t kSdlButtonRight = 3;

/* ---- helpers ------------------------------------------------------------- */

/* One chain layout's selection, read fresh from the word the game derives
 * its highlight from: the screen adjustment screen's own level, or that
 * layout table entry's current item. False when that word is not mapped. */
bool read_selection(uint32_t layout, uint32_t* out) {
    if (layout_is_adjust(layout)) return read_word(RT_ICO_BRIGHTNESS, out);
    uint32_t entry = 0;
    if (!entry_addr(layout, &entry)) return false;
    return read_word(entry + RT_ICO_LAYOUT_CUR_ITEM, out);
}

/* Makes `item` the selected item of `layout`, which is the current layout or
 * one of its ancestors in the chain, as if the game's own navigation had
 * landed there. `current` is what that selection reads now, for the log.
 * False, having said why, when the write cannot be shown correct: the
 * caller then does nothing at all rather than acting on the wrong item. */
bool write_selection(uint32_t owner, uint32_t item, uint32_t current) {
    const unsigned layout = (unsigned)owner;

    if (layout_is_adjust(owner)) {
        if (item >= RT_ICO_ADJUST_COUNT) {
            if (!g_logged_adjust_range) {
                g_logged_adjust_range = true;
                rt_log_warn("guest", "guest menu: layout 0x%x bar position 0x%x is outside 0..%u;"
                                " not written (this line is not repeated)", layout, (unsigned)item,
                       (unsigned)(RT_ICO_ADJUST_COUNT - 1));
            }
            return false;
        }
        if (!write_word(RT_ICO_BRIGHTNESS, item)) {
            if (!g_logged_adjust_unmapped) {
                g_logged_adjust_unmapped = true;
                rt_log_warn("guest", "guest menu: the screen adjustment level at 0x%08x is not"
                                " mapped; item 0x%x not written (this line is not repeated)",
                       (unsigned)RT_ICO_BRIGHTNESS, (unsigned)item);
            }
            return false;
        }
        g_state.adjust_level = item;
    } else {
        uint32_t entry = 0;
        int32_t first = 0, end = 0;
        if (!entry_addr(owner, &entry) ||
            !read_i32(entry + RT_ICO_LAYOUT_FIRST_OBJ, &first) ||
            !read_i32(entry + RT_ICO_LAYOUT_END_OBJ, &end)) {
            if (!g_logged_entry_unmapped) {
                g_logged_entry_unmapped = true;
                rt_log_warn("guest", "guest menu: the layout table entry for 0x%x is not mapped;"
                                " item 0x%x not written (this line is not repeated)", layout,
                       (unsigned)item);
            }
            return false;
        }
        /* The scene object range the layout owns. lt_link_layout draws the
         * highlight only for an object inside it, so an item outside it can
         * never be the selected one: writing it would leave the screen with
         * no highlight. Every item this module offers came out of that
         * range, so reaching this line means the range moved under it. */
        if ((int32_t)item < first || (int32_t)item >= end) {
            if (!g_logged_item_range) {
                g_logged_item_range = true;
                rt_log_warn("guest", "guest menu: item 0x%x is outside layout 0x%x's items"
                                " 0x%x..0x%x; not written (this line is not repeated)",
                       (unsigned)item, layout, (unsigned)first, (unsigned)(end - 1));
            }
            return false;
        }
        if (!write_word(entry + RT_ICO_LAYOUT_CUR_ITEM, item)) {
            if (!g_logged_entry_unmapped) {
                g_logged_entry_unmapped = true;
                rt_log_warn("guest", "guest menu: the layout table entry for 0x%x is not mapped;"
                                " item 0x%x not written (this line is not repeated)", layout,
                       (unsigned)item);
            }
            return false;
        }
        /* The mirror the game refreshes from that field at the top of every
         * frame, written so a reader in between sees the two agree. Only
         * for the current layout: lt_next_layout loads the mirror from each
         * chain layout in turn and the current one is last, so after a
         * frame it holds that one's field and nothing else's. */
        if (owner == g_state.layout && !g_kanban_active) {
            write_word(RT_ICO_ITEM_ID, item);
            g_state.item = item;
        }
    }

    if (take_action_line()) {
        rt_log_info("guest", "guest menu: select item 0x%x on layout 0x%x (was 0x%x)",
               (unsigned)item, layout, (unsigned)current);
    }
    return true;
}

/* The item under (nx, ny), or nullptr. Later items win, which puts an item
 * the link walk reached later on top of one it reached earlier; the game's
 * own items do not overlap, so this only decides a case that should not
 * happen. */
const RtGuestMenuItem* hit_test(float nx, float ny) {
    const RtGuestMenuItem* hit = nullptr;
    for (const RtGuestMenuItem& r : g_items) {
        if (nx >= r.x0 && nx <= r.x1 && ny >= r.y0 && ny <= r.y1) hit = &r;
    }
    return hit;
}

/* The D-pad press one wheel tick asks for on this screen, taken from the
 * links the selected object actually has: a row steps left and right, a
 * column up and down. The screen adjustment screen's handler only reads
 * LEFT and RIGHT. Zero when the selection has no links at all, which is a
 * screen a wheel cannot move.
 *
 * The press itself reaches every layout in the chain, because lt_next_layout
 * runs lt_switch_layout for each of them, and each moves its own selection
 * or refuses. The direction is read off the first layout that contributed
 * items, which is the current layout when it has any and the nearest
 * ancestor that does otherwise: that is the one the player is steering. */
uint16_t wheel_bits(bool up) {
    if (g_items.empty()) return 0;
    const uint32_t owner = g_items.front().layout;
    if (layout_is_adjust(owner)) return up ? RT_PAD_LEFT : RT_PAD_RIGHT;

    uint32_t sel = 0;
    uint32_t base = 0;
    if (!read_selection(owner, &sel) || !object_addr((int32_t)sel, &base)) return 0;

    int32_t left = -1, right = -1, down = -1, obj_up = -1;
    read_i32(base + RT_ICO_OBJ_LEFT, &left);
    read_i32(base + RT_ICO_OBJ_RIGHT, &right);
    read_i32(base + RT_ICO_OBJ_DOWN, &down);
    read_i32(base + RT_ICO_OBJ_UP, &obj_up);

    if (left >= 0 || right >= 0) return up ? RT_PAD_LEFT : RT_PAD_RIGHT;
    if (down >= 0 || obj_up >= 0) return up ? RT_PAD_UP : RT_PAD_DOWN;
    return 0;
}

void queue_pulse(uint16_t bits) {
    if (bits == 0) return;
    if (g_queue.size() >= kMaxQueue) {
        if (!g_queue_dropped) {
            g_queue_dropped = true;
            rt_log_warn("guest", "guest menu: more than %zu presses queued; dropping the newest"
                            " (this line is not repeated)", kMaxQueue);
        }
        return;
    }
    g_queue.push_back(bits);
}

void start_pulse(uint16_t bits) {
    g_pulse_bits = bits;
    g_pulse_phase = 1;
}

void clear_pulses() {
    g_queue.clear();
    g_pulse_phase = 0;
    g_pulse_bits = 0;
}

/* Whether a press asked for now can be its own field's bits. The SDL
 * provider runs the tick, then the button and wheel events, then reads the
 * pulse bits, all for one field, so a press started here is returned for the
 * field it arrived on and its release lands on the next tick: the same two
 * fields the queue would have given, one field earlier. A press asked for
 * while a pulse is in flight has to queue, or the two would share a field
 * and the game would see one press with no release between them. */
void start_or_queue(uint16_t bits) {
    if (bits == 0) return;
    if (g_pulse_phase == 0 && g_queue.empty()) {
        start_pulse(bits);
        return;
    }
    queue_pulse(bits);
}

/* The cursor has left the item the pointer acted on, or the screen has. */
void forget_hover() {
    g_hover_acted = false;
}

/* One layout's selection now, for a decision rather than for a readout:
 * false when the word cannot be read, in which case the caller does
 * nothing, because acting without knowing what is selected is how the wrong
 * item gets confirmed. */
bool selection_now(uint32_t layout, uint32_t* out) {
    if (read_selection(layout, out)) return true;
    if (!g_logged_no_selection) {
        g_logged_no_selection = true;
        rt_log_warn("guest", "guest menu: layout 0x%x's selection word is not mapped; the pointer"
                        " does nothing on this screen (this line is not repeated)",
               (unsigned)layout);
    }
    return false;
}

/* Whether the pointer may act at all this field. Two conditions, both of
 * them host-side and neither of them about the game:
 *
 *   rt_input_sdl_active()   The pointer's presses ride on the SDL provider's
 *                           virtual pad and reach nothing else. A run driven
 *                           by ICORECOMP_INPUT_SCRIPT has to stay
 *                           bit-identical, and if it has a window the OS
 *                           cursor would otherwise be writing the game's
 *                           selection out from under the script. False in a
 *                           build without SDL, which has no cursor either.
 *
 *   !rt_ui_wants_input()    The settings overlay is up. It releases relative
 *                           mouse mode, so the OS cursor maps onto the
 *                           picture and dragging across the overlay would
 *                           move the selection on the game's menu behind it.
 *                           host/input.cpp neutralises the pad on the same
 *                           condition.
 *
 * Only the pointer half is gated. The read and the change log run for every
 * provider, because they write nothing. */
bool pointer_may_act() {
    return rt_input_sdl_active() && !rt_ui_wants_input();
}

/* The game's own one-frame "swallow this frame's navigation" flag
 * (RT_ICO_NAV_SWALLOW, guest/ico_syms.h). lt_switch_layout returns at once
 * while it is set, lt_next_layout skips lt_switch_layout for the whole chain,
 * and lt_link_layout draws no highlight; lt_next_layout clears it on the way
 * out, so it lasts one frame. The pointer's write does not go through
 * lt_switch_layout, so it would land on a frame the game meant to sit still:
 * the hover waits instead and is taken again next field. Reads as "not set"
 * when the word is unmapped, which is the state the rest of the module
 * already refuses to act in.
 *
 * The flag is set on the frame a fade completes into an interactive screen,
 * so this is the ordinary case at every screen change, not an error. The
 * line is printed once because the mechanism engaging at all is worth seeing
 * in a log from another machine. */
bool navigation_swallowed() {
    uint32_t v = 0;
    if (!read_word(RT_ICO_NAV_SWALLOW, &v) || v == 0) return false;
    if (!g_logged_swallowed) {
        g_logged_swallowed = true;
        rt_log_info("guest", "guest menu: the game is swallowing this frame's navigation"
                        " (0x%08x); the hover write waits for the next field"
                        " (this line is not repeated)", (unsigned)RT_ICO_NAV_SWALLOW);
    }
    return true;
}

/* The pointer's field, run from the tick with the guest state already read
 * and the items already derived. Every decision is taken between pulses,
 * never inside one. */
void pointer_tick() {
    /* Inert: no hover state, no write, no press, and the queue goes with it,
     * the same way host/input.cpp drops the mouse events it will not route.
     * A pulse in flight goes too: no field of it will reach the pad. */
    if (!pointer_may_act()) {
        forget_hover();
        clear_pulses();
        return;
    }

    /* The cadence, first: a press occupies its field and the release
     * occupies the next, and nothing else is decided in between. */
    if (g_pulse_phase == 1) {
        g_pulse_phase = 2;
        g_pulse_bits = 0;
        return;
    }
    if (g_pulse_phase == 2) {
        g_pulse_phase = 0;
        g_pulse_bits = 0;
    }

    if (!rt_guest_menu_active()) {
        forget_hover();
        g_queue.clear();
        return;
    }

    float nx = 0.0f, ny = 0.0f;
    const bool have_cursor = rt_guest_menu_cursor(&nx, &ny);
    const RtGuestMenuItem* hovered = have_cursor ? hit_test(nx, ny) : nullptr;

    /* Entering an item is the whole instruction: it is acted on once, on
     * the field it happens, and not again while the cursor stays inside
     * it. The item is identified by the layout it belongs to as well as its
     * index, because two chain layouts number their objects separately. */
    if (!hovered || g_hover_layout != hovered->layout || g_hover_item != hovered->item) {
        forget_hover();
    }
    /* The swallow flag belongs to lt_next_layout, which does not run a
     * kanban screen: display_layout and func_001B8B10 never read it, so a
     * hover there writes on its own field rather than waiting for a frame
     * the game was not going to sit still through anyway. */
    if (hovered && !g_hover_acted && (g_kanban_active || !navigation_swallowed())) {
        /* The hover is consumed by the write actually happening, or by the
         * item already being the selected one, and by nothing else. A write
         * that is refused (an unmapped word, or an item outside the
         * layout's own range) leaves the three fields clear, so the next
         * field tries again while the cursor stays on the item; the refusal
         * itself is latched inside write_selection, so retrying costs no
         * further log lines. Consuming the hover on a refusal would leave
         * the item pointed at and never selected for as long as the cursor
         * did not move. */
        uint32_t current = 0;
        bool acted = false;
        if (selection_now(hovered->layout, &current)) {
            acted = current == hovered->item ||
                    write_selection(hovered->layout, hovered->item, current);
        }
        if (acted) {
            g_hover_acted = true;
            g_hover_layout = hovered->layout;
            g_hover_item = hovered->item;
        }
    }

    /* Queued presses (a click's cross, a right click's triangle, the
     * wheel's steps) go out one per two fields. */
    if (!g_queue.empty()) {
        const uint16_t bits = g_queue.front();
        g_queue.pop_front();
        start_pulse(bits);
    }
}

#ifdef ICORECOMP_MENU_NAV_TEST
bool g_test_cursor_valid = false;
float g_test_cursor_x = 0.0f, g_test_cursor_y = 0.0f;
bool g_test_captured = false;
float g_test_scale_x = 0.0f, g_test_scale_y = 0.0f;
#endif

/* ---- the drawn cursor's arithmetic --------------------------------------- */

float clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Whether relative mouse mode is on. That one fact decides which of the two
 * sources answers rt_guest_menu_cursor(): with the OS cursor hidden and
 * confined there is no absolute position to map, so the drawn cursor is the
 * only position there is. */
bool relative_mode_on() {
#if defined(ICORECOMP_MENU_NAV_TEST)
    return g_test_captured;
#elif defined(RT_MENU_NAV_HAVE_CURSOR)
    return rt_mouse_captured();
#else
    return false;
#endif
}

/* Normalized units of the presented rectangle covered by one window logical
 * pixel of motion, per axis: the window-to-backbuffer scale (the same one
 * the absolute mapping applies) over the rectangle's size, so at a 1:1
 * render scale one mouse pixel is one picture pixel. False when there is no
 * window or nothing has been presented yet, which is the honest report for
 * "there is no picture to move a cursor across". */
bool motion_scale(float* sx, float* sy) {
#if defined(ICORECOMP_MENU_NAV_TEST)
    if (!(g_test_scale_x > 0.0f) || !(g_test_scale_y > 0.0f)) return false;
    *sx = g_test_scale_x;
    *sy = g_test_scale_y;
    return true;
#elif defined(RT_MENU_NAV_HAVE_CURSOR)
    SDL_Window* win = (SDL_Window*)rt_window_handle();
    if (!win) return false;

    int32_t rx = 0, ry = 0, rw = 0, rh = 0, bb_w = 0, bb_h = 0;
    rt_window_present_rect(&rx, &ry, &rw, &rh, &bb_w, &bb_h);
    if (rw <= 0 || rh <= 0 || bb_w <= 0 || bb_h <= 0) return false;

    int win_w = 0, win_h = 0;
    if (!SDL_GetWindowSize(win, &win_w, &win_h) || win_w <= 0 || win_h <= 0) return false;

    *sx = ((float)bb_w / (float)win_w) / (float)rw;
    *sy = ((float)bb_h / (float)win_h) / (float)rh;
    return true;
#else
    (void)sx;
    (void)sy;
    return false;
#endif
}

/* Puts the drawn cursor at the centre of the picture if it has never been
 * anywhere. Returns whether it had to. */
bool start_drawn_cursor() {
    if (g_vc_valid) return false;
    g_vc_valid = true;
    g_vc_x = 0.5f;
    g_vc_y = 0.5f;
    return true;
}

/* The cap on the two ownership lines. False once it has been reached, with
 * one closing line, so a menu state that oscillates cannot fill the log. */
bool owner_line_ok() {
    if (g_owner_capped) return false;
    if (g_owner_lines >= kMaxOwnerLines) {
        g_owner_capped = true;
        rt_log_info("guest", "guest menu: further pointer handovers not logged");
        return false;
    }
    ++g_owner_lines;
    return true;
}

/* Logs the field the pointer takes the mouse from mouse look and the field
 * it hands it back, so the switch is visible in a log from another machine.
 * Called once per tick, after the four words have been read. */
void update_motion_owner() {
    /* The same two gates the writes are under: a field the pointer cannot
     * act on is not a field it owns the mouse for, and saying it took the
     * mouse there would be a line about something that did not happen. */
    const bool owns = pointer_may_act() && rt_guest_menu_active();
    if (g_had_mouse_known && owns == g_had_mouse) return;

    /* Which screen the line is about: the one being taken, or, on the way
     * out, the one that was held. Empty for a layout-table screen, which the
     * chain line already named. */
    char screen[96];
    screen[0] = '\0';
    const bool kanban = owns ? g_kanban_active : g_had_kanban;
    const uint32_t kanban_layout = owns ? g_kanban_layout : g_had_kanban_layout;
    if (kanban) {
        std::snprintf(screen, sizeof(screen), " on kanban layout 0x%x (%s)",
                      (unsigned)kanban_layout, kanban_screen_name(kanban_layout));
    }

    g_had_mouse_known = true;
    g_had_mouse = owns;
    g_had_kanban = owns && g_kanban_active;
    if (g_had_kanban) g_had_kanban_layout = g_kanban_layout;

    if (!owns) {
        if (owner_line_ok()) {
            rt_log_info("guest", "guest menu: pointer hands the mouse back to mouse look%s", screen);
        }
        return;
    }
    if (!relative_mode_on()) {
        /* Mouse look is off, so the pointer was never captured and the OS
         * cursor is on screen already. Nothing is drawn over it. */
        if (owner_line_ok()) {
            rt_log_info("guest", "guest menu: pointer takes the mouse%s, following the system"
                            " cursor", screen);
        }
        return;
    }
    if (start_drawn_cursor()) {
        if (owner_line_ok()) {
            rt_log_info("guest", "guest menu: pointer takes the mouse%s, drawn cursor at centre",
                   screen);
        }
        return;
    }
    if (owner_line_ok()) {
        rt_log_info("guest", "guest menu: pointer takes the mouse%s, drawn cursor at %.3f,%.3f",
               screen, (double)g_vc_x, (double)g_vc_y);
    }
}

} // namespace

/* ---- per-field entry points ---------------------------------------------- */

void rt_guest_menu_tick(uint64_t field) {
    g_field = field;
    g_have_field = true;

    /* This module is nothing but reads and two writes at the addresses in
     * guest/ico_syms.h. Without every one of those addresses read off the
     * retail ELF there is no honest thing to read, so the pointer does
     * nothing at all rather than poking at an address that belongs to
     * something else. */
    if (!RT_ICO_MENU_NAV_KNOWN) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            rt_log_warn("menu", "the mouse pointer on the game's own menus is off: this build"
                " does not have every address it needs (guest/ico_syms.h names which)."
                " The menus still work from the pad and the keyboard.");
        }
        g_state = RtGuestMenuState{};
        return;
    }

    RtGuestMenuState s;
    if (!read_word(RT_ICO_LAYOUT_ID, &s.layout) ||
        !read_word(RT_ICO_ITEM_ID, &s.item) ||
        !read_word(RT_ICO_FADE_STATE, &s.fade) ||
        !read_word(RT_ICO_BRIGHTNESS, &s.adjust_level)) {
        g_state = RtGuestMenuState{};
        rebuild_items();
        update_motion_owner();
        pointer_tick();
        return;
    }
    s.valid = true;
    g_state = s;

    /* Where this screen's items are, from the game's own scene objects. */
    rebuild_items();
    /* The kanban screens are not in the four words above: the state words
     * say nothing about them, so what the tuple reports for one is read
     * here, after the screen has been resolved. */
    g_state.kanban = g_kanban_active;
    g_state.kanban_layout = g_kanban_layout;
    g_state.kanban_item = 0;
    if (g_kanban_active) {
        uint32_t sel = 0;
        if (read_selection(g_kanban_layout, &sel)) g_state.kanban_item = sel;
    }
    /* Only for a screen this module has something to say about: one the
     * pointer could act on, or one it refused to act on because a chain
     * layout the game will not navigate is what stopped it. Gating on fade 2
     * alone would add gameplay and each cinematic, which hold fade 2 for
     * their whole run, burning one of the 64 logged-layout slots each to
     * print a chain with "0 items". */
    if (rt_guest_menu_active() || !g_chain_no_selection.empty()) log_layout_rects();

    /* Who the field's motion belongs to, before anything reads the cursor:
     * the take-over is what starts the drawn cursor at the centre. */
    update_motion_owner();

    pointer_tick();

    /* The tuple the tick leaves behind, which is what a write this field
     * changed, not what the field started with. */
    if (g_capped) return;
    if (g_have_logged &&
        g_logged.layout == g_state.layout && g_logged.item == g_state.item &&
        g_logged.fade == g_state.fade && g_logged.adjust_level == g_state.adjust_level &&
        g_logged.kanban == g_state.kanban &&
        g_logged.kanban_layout == g_state.kanban_layout &&
        g_logged.kanban_item == g_state.kanban_item) {
        return;
    }
    g_have_logged = true;
    g_logged = g_state;

    if (g_lines >= kMaxLines) {
        g_capped = true;
        rt_log_debug("guest", "guest menu: further changes not logged");
        return;
    }
    ++g_lines;
    if (g_state.kanban) {
        rt_log_debug("guest", "guest menu: layout 0x%x item 0x%x fade %u adjust_level %u,"
                        " kanban layout 0x%x item 0x%x",
               (unsigned)g_state.layout, (unsigned)g_state.item, (unsigned)g_state.fade,
               (unsigned)g_state.adjust_level, (unsigned)g_state.kanban_layout,
               (unsigned)g_state.kanban_item);
    } else {
        rt_log_debug("guest", "guest menu: layout 0x%x item 0x%x fade %u adjust_level %u",
               (unsigned)g_state.layout, (unsigned)g_state.item, (unsigned)g_state.fade,
               (unsigned)g_state.adjust_level);
    }
}

const RtGuestMenuState& rt_guest_menu_state() {
    return g_state;
}

const std::vector<RtGuestMenuItem>& rt_guest_menu_items() {
    return g_items;
}

bool rt_guest_menu_active() {
    /* fade 2 is the interactive state; 3 and 5 are the fading ones, and
     * pressing into a fade would land on whichever screen the fade ends on.
     *
     * Fade 2 alone is not enough: gameplay (layout 0x32) holds fade 2 for
     * the whole play session. The second term is what excludes it, and it
     * excludes it structurally: layout 0x32's scene object range is empty
     * and it has no parent, so the chain is one layout with nothing to
     * reach and g_items is empty. The pre-title cinematic (0x33) is empty
     * the same way. */
    if (!g_state.valid) return false;
    /* A kanban screen is gated by the kanban system instead: the node at
     * 0x0063C39C says the game is navigating it, rebuild_items has already
     * refused a node that is fading out, and the layout fade state means
     * nothing on these screens. */
    if (g_kanban_active) return !g_items.empty();
    if (g_state.fade != 2) return false;
    return !g_items.empty();
}

uint16_t rt_guest_menu_pulse_bits(uint64_t field) {
    if (!g_have_field || field != g_field) {
        if (!g_logged_field_mismatch) {
            g_logged_field_mismatch = true;
            rt_log_warn("guest", "guest menu: pulse bits asked for field %llu but the last tick was"
                            " field %llu; no bits (this line is not repeated)",
                   (unsigned long long)field, (unsigned long long)g_field);
        }
        return 0;
    }
    /* A read and nothing else: the tick advances the cadence and the button
     * and wheel entry points start it, so asking again for the same field
     * gives the same bits. */
    return g_pulse_phase == 1 ? g_pulse_bits : 0;
}

bool rt_guest_menu_wants_mouse() {
    return rt_guest_menu_active();
}

void rt_guest_menu_on_button(uint8_t sdl_button, bool down) {
    if (!down || !pointer_may_act() || !rt_guest_menu_active()) return;

    if (sdl_button == kSdlButtonLeft) {
        float nx = 0.0f, ny = 0.0f;
        if (!rt_guest_menu_cursor(&nx, &ny)) return;
        const RtGuestMenuItem* hit = hit_test(nx, ny);
        /* Over nothing presses nothing: there is no item to confirm, and a
         * cross would confirm whatever the game has selected elsewhere. */
        if (!hit) return;

        /* Select first, then confirm, both in this field. The SDL provider
         * runs the tick, then this, then rt_guest_menu_pulse_bits for the
         * same field, so the cross start_or_queue puts in flight below is in
         * the bits the game reads on the field it was clicked, one game frame
         * after the write and not two. The hover has usually done the write
         * already, so what is left here is a click that arrived in the same
         * field as the cursor, or one on a screen whose selection the game
         * moved since. A write that is needed and refused takes the cross
         * with it: pressing it anyway would confirm the item still selected,
         * which is not the item that was clicked. */
        uint32_t current = 0;
        const uint32_t owner = hit->layout;
        const uint32_t item = hit->item;
        if (!selection_now(owner, &current)) return;
        if (current != item && !write_selection(owner, item, current)) return;

        /* A click leaves the item acted on, so the tick does not write the
         * same item again while the cursor stays. */
        g_hover_acted = true;
        g_hover_layout = owner;
        g_hover_item = item;

        start_or_queue(RT_PAD_CROSS);
        if (take_action_line()) {
            rt_log_info("guest", "guest menu: click item 0x%x on layout 0x%x",
                   (unsigned)item, (unsigned)owner);
        }
        return;
    }

    if (sdl_button == kSdlButtonRight) {
        start_or_queue(RT_PAD_TRIANGLE);
    }
}

void rt_guest_menu_on_wheel(int ticks) {
    if (ticks == 0 || !pointer_may_act() || !rt_guest_menu_active()) return;

    const bool up = ticks > 0;
    const int n = ticks > 0 ? ticks : -ticks;
    const uint16_t bits = wheel_bits(up);
    if (bits == 0) return;
    for (int i = 0; i < n; ++i) start_or_queue(bits);
}

void rt_guest_menu_on_motion(float dx, float dy) {
    /* The delta arrives already summed over the field, so a driver that
     * handed the accumulator something that is not finite shows up here.
     * Saying so beats carrying a NaN into the cursor, where it would fail
     * every hit test from then on. */
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        if (!g_logged_motion_nonfinite) {
            g_logged_motion_nonfinite = true;
            rt_log_warn("guest", "guest menu: non-finite pointer motion (%f, %f); the drawn cursor"
                            " did not move (this line is not repeated)",
                   (double)dx, (double)dy);
        }
        return;
    }

    float sx = 0.0f, sy = 0.0f;
    if (!motion_scale(&sx, &sy)) {
        if (!g_logged_motion_no_rect) {
            g_logged_motion_no_rect = true;
            rt_log_warn("guest", "guest menu: pointer motion arrived with nothing presented yet;"
                            " the drawn cursor cannot move (this line is not repeated)");
        }
        return;
    }

    /* Motion can arrive before the tick that would have started the cursor
     * (the capture transition and the pad field are two different hooks).
     * The same centre, one field earlier. */
    start_drawn_cursor();

    /* Clamped, not wrapped: the picture is the whole of what the pointer can
     * reach, and a cursor outside it would hover nothing while the OS cursor
     * it stands in for is still confined to the window. */
    g_vc_x = clamp01(g_vc_x + dx * sx);
    g_vc_y = clamp01(g_vc_y + dy * sy);
}

/* ---- the cursor ---------------------------------------------------------- */

bool rt_guest_menu_cursor(float* nx, float* ny) {
#if defined(ICORECOMP_MENU_NAV_TEST) || defined(RT_MENU_NAV_HAVE_CURSOR)
    float x = 0.0f, y = 0.0f;

    if (relative_mode_on()) {
        /* The OS cursor is hidden and confined to the window, so there is no
         * absolute position to map: the drawn cursor is the only one there
         * is. It is clamped where it is moved, so it is already in range. */
        if (!g_vc_valid) return false;
        x = g_vc_x;
        y = g_vc_y;
    } else {
        /* Relative mode is off, so the OS cursor is on screen and its
         * absolute position is the pointer. */
#ifdef ICORECOMP_MENU_NAV_TEST
        if (!g_test_cursor_valid) return false;
        x = g_test_cursor_x;
        y = g_test_cursor_y;
#else
        SDL_Window* win = (SDL_Window*)rt_window_handle();
        if (!win) return false;
        /* A cursor the window does not own is a cursor somewhere else. */
        if (!rt_mouse_focused()) return false;

        int32_t rx = 0, ry = 0, rw = 0, rh = 0, bb_w = 0, bb_h = 0;
        rt_window_present_rect(&rx, &ry, &rw, &rh, &bb_w, &bb_h);
        if (rw <= 0 || rh <= 0 || bb_w <= 0 || bb_h <= 0) return false;

        int win_w = 0, win_h = 0;
        if (!SDL_GetWindowSize(win, &win_w, &win_h) || win_w <= 0 || win_h <= 0) return false;

        float cx = 0.0f, cy = 0.0f;
        rt_mouse_cursor_window(&cx, &cy);
        /* Window logical coordinates to backbuffer pixels, against the
         * backbuffer size the present rectangle was measured in (the same
         * scale ui/ui_events.cpp's window_to_surface applies), then the
         * rectangle's own origin and size. */
        const float px = cx * (float)bb_w / (float)win_w;
        const float py = cy * (float)bb_h / (float)win_h;
        x = (px - (float)rx) / (float)rw;
        y = (py - (float)ry) / (float)rh;
#endif
    }

    /* Outside the presented scanout is over nothing the game drew, which
     * includes the letterbox bars. */
    if (!(x >= 0.0f && x <= 1.0f && y >= 0.0f && y <= 1.0f)) return false;
    if (nx) *nx = x;
    if (ny) *ny = y;
    return true;
#else
    /* No window or no presented rectangle in this build: there is no
     * pointer, and saying so is the only honest answer. */
    (void)nx;
    (void)ny;
    return false;
#endif
}

#ifdef ICORECOMP_MENU_NAV_TEST

void rt_guest_menu_test_set_cursor(bool valid, float nx, float ny) {
    g_test_cursor_valid = valid;
    g_test_cursor_x = nx;
    g_test_cursor_y = ny;
}

void rt_guest_menu_test_set_captured(bool captured) {
    g_test_captured = captured;
}

void rt_guest_menu_test_set_motion_scale(float per_pixel_x, float per_pixel_y) {
    g_test_scale_x = per_pixel_x;
    g_test_scale_y = per_pixel_y;
}

bool rt_guest_menu_test_drawn_cursor(float* nx, float* ny) {
    if (!g_vc_valid) return false;
    if (nx) *nx = g_vc_x;
    if (ny) *ny = g_vc_y;
    return true;
}

void rt_guest_menu_test_reset() {
    g_state = RtGuestMenuState{};
    g_items.clear();
    g_chain.clear();
    g_adjust_layouts.clear();
    g_chain_no_selection.clear();
    g_kanban_active = false;
    g_kanban_layout = 0;
    g_logged_kanban_node = false;
    g_logged_kanban_entry = false;
    g_logged_kanban_no_items = false;
    g_logged_kanban_no_selection = false;
    g_logged_layouts.clear();
    g_logged_kanban_layouts.clear();
    g_layout_log_capped = false;
    g_logged_long_range = false;
    g_logged_long_chain = false;
    g_have_logged = false;
    g_lines = 0;
    g_capped = false;
    clear_pulses();
    g_queue_dropped = false;
    forget_hover();
    g_hover_layout = 0;
    g_hover_item = 0;
    g_logged_no_selection = false;
    g_logged_swallowed = false;
    g_logged_negative_size = false;
    g_have_field = false;
    g_field = 0;
    g_logged_field_mismatch = false;
    g_test_cursor_valid = false;
    g_test_captured = false;
    g_test_scale_x = 0.0f;
    g_test_scale_y = 0.0f;
    g_vc_valid = false;
    g_vc_x = 0.5f;
    g_vc_y = 0.5f;
    g_had_mouse = false;
    g_had_mouse_known = false;
    g_had_kanban = false;
    g_had_kanban_layout = 0;
    g_owner_lines = 0;
    g_owner_capped = false;
    g_logged_motion_no_rect = false;
    g_logged_motion_nonfinite = false;
}

#endif /* ICORECOMP_MENU_NAV_TEST */
