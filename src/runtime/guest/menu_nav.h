/* guest/menu_nav.h: the mouse pointer for the game's own menus.
 *
 * Two halves that share one read of guest memory:
 *
 *   observation  The host reads the game's menu state words once per pad
 *                field (guest/ico_syms.h) and keeps the last values.
 *
 *   the pointer  The game's own scene objects say where each menu item is
 *                on screen: every layout in the current screen's chain owns
 *                a range of them, each carries the position and size the
 *                game draws it at, and menu_nav.cpp turns those into
 *                rectangles on the presented picture. Hovering an item
 *                makes it the selected item at
 *                once: the pointer writes the game's own selection word,
 *                which is the word the game's per-frame drawing re-derives
 *                the highlight from, so the screen lands on that item
 *                exactly as if the player's D-pad had walked there. A left
 *                click selects and then presses cross, a right click
 *                presses triangle, and a wheel tick presses one D-pad step.
 *                Those three go through the virtual pad, because the
 *                handlers behind them read a pressed-this-frame word.
 *
 * Writing the selection is a decision of the user's, taken on 2026-09-03:
 * the pointer intercepts the guest here rather than synthesising a walk of
 * D-pad presses toward the item. menu_nav.cpp carries the traced mechanism,
 * the two words that are written, and the screen mapping.
 *
 * Nothing is authored. There is no rectangle table and no editor: every
 * rectangle comes out of the same guest fields the game draws from, so a
 * screen that was never visited still points correctly.
 *
 * The values read:
 *   layout  current layout (menu screen) id, from the layout state machine
 *   item    the selected item id as the game's accessor returns it, a mirror
 *           of the current layout entry's current-item field refreshed at the
 *           top of every frame
 *   fade    layout fade/transition state; 2 is the interactive state and 3
 *           and 5 are the two fading ones. Gameplay also sits at 2, so the
 *           predicate below needs more than fade.
 *   mcsel   the memory card check screen's selector index (0..14)
 *
 * The word the highlight is actually drawn from, the layout table entry's
 * current-item field, is not kept here: every decision reads it fresh out of
 * guest memory, because it is the word the pointer writes and a copy of it
 * would be one frame stale exactly where that matters.
 *
 * `valid` is false when any of the four read addresses is unmapped, which is
 * the state before the ELF is loaded. Nothing is logged then.
 *
 * Runtime-internal, NOT part of the ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_GUEST_MENU_NAV_H
#define ICORECOMP_GUEST_MENU_NAV_H

#include <cstdint>
#include <vector>

struct RtGuestMenuState {
    uint32_t layout = 0;
    uint32_t item = 0;
    uint32_t fade = 0;
    uint32_t mcsel = 0;
    bool valid = false;
};

/* One selectable item of the current screen, as the game itself places it.
 *
 * `layout` is the layout the item belongs to and whose selection word it is
 * written into. It is not always the current layout id: a screen is built
 * from a chain of layouts (the current one and its parents by +0x30), every
 * one of which lt_next_layout runs and draws, and the items of a page like
 * the load file select (0x10) belong to an ancestor (0xb, the ten-slot
 * grid). menu_nav.cpp carries the trace.
 *
 * `item` is the value that layout's selection word takes for it: a scene
 * object index on every screen but the memory card check, and a selector
 * position 0..14 on that one.
 *
 * The rectangle is in normalized presented-scanout coordinates, so 0..1 is
 * the picture the game drew and the letterbox bars are outside it by
 * construction; it is the same unit rt_guest_menu_cursor() answers in. It
 * is the game's own sprite quad, not a padded hit area, and it is left
 * unclamped: an item the game placed partly off the picture reports the
 * rectangle it placed. */
struct RtGuestMenuItem {
    uint32_t layout = 0;
    uint32_t item = 0;
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

/* ---- per-field entry points ---------------------------------------------- */

/* Reads the game's menu state, derives the current screen's items, logs the
 * tuple whenever it changes, and gives the pointer its field: a hover over
 * an item that is not the selected one writes the selection, and a queued
 * press starts. Called once per pad field from pad_field_tick(), before
 * rt_input_poll; `field` is that field counter.
 *
 * The read and the log run for every input provider. The pointer's half runs
 * only while the SDL provider is the live one (rt_input_sdl_active()) and the
 * settings overlay is closed (!rt_ui_wants_input()): a scripted run must stay
 * bit-identical, and an overlay that has released relative mouse mode has the
 * OS cursor lying over the picture with nothing aimed at the game's menu.
 * With either gate shut the field writes nothing, hovers nothing, starts no
 * press, and drops whatever was queued. */
void rt_guest_menu_tick(uint64_t field);

/* The values read by the most recent tick. */
const RtGuestMenuState& rt_guest_menu_state();

/* The selectable items of the screen the most recent tick found: every
 * layout in the chain, the current one first and then its parents, each
 * layout's items in the order they were reached from its default item.
 * That is also the order the game draws them in, so a later item is on top
 * of an earlier one. Empty when there is no such screen, which includes
 * gameplay and the cinematics. */
const std::vector<RtGuestMenuItem>& rt_guest_menu_items();

/* Whether a menu the host may point at is on screen: the state words read,
 * fade 2 (the interactive state), and at least one selectable item with a
 * usable rectangle on some layout in the current screen's chain. */
bool rt_guest_menu_active();

/* DS2 button bits (host/input.h RT_PAD_* order) the host wants pressed on
 * this field to drive the game's menu: cross on a click, triangle on a
 * right click, one D-pad step per wheel tick. Moving the selection is not
 * one of them: that is a write, not a press. The SDL input provider ORs
 * these into the virtual pad; the script provider never sees them.
 *
 * A pure read of the state the tick and this field's button and wheel
 * events left behind, so asking twice for the same field gives the same
 * answer. `field` must be the field the last rt_guest_menu_tick was given;
 * any other value returns 0 and logs once, because a press that straddles
 * two fields is not a press the game would see the way this module counted
 * it.
 *
 * A press with no pulse already in flight starts inside the call that asked
 * for it (rt_guest_menu_on_button, rt_guest_menu_on_wheel), which the SDL
 * provider runs before this one on the same field, so a click's cross is in
 * the bits for the field the click arrived on. That is what puts the select
 * and the cross in one field. Anything asked for while a pulse is in flight
 * waits in the queue and is started by the next tick that is free.
 *
 * Every press is one field of bits followed by one field of zero. The
 * game's menu handlers read a pressed-this-frame word, which needs the
 * release: bits held across fields would arrive as one press and then the
 * game's own auto-repeat. */
uint16_t rt_guest_menu_pulse_bits(uint64_t field);

/* True while the pointer, not the gameplay bindings, owns the mouse: a menu
 * is active. The SDL input provider routes the field's motion, button and
 * wheel events here through the three calls below instead of mapping them
 * to the camera stick and the DS2 slots. */
bool rt_guest_menu_wants_mouse();
/* One SDL mouse button transition (SDL button index, true = pressed). */
void rt_guest_menu_on_button(uint8_t sdl_button, bool down);
/* Signed wheel ticks this field, positive = wheel up. */
void rt_guest_menu_on_wheel(int ticks);
/* This field's relative motion, in window logical pixels (SDL's convention:
 * x positive right, y positive down). It moves the drawn cursor described
 * below and nothing else; the camera stick sees none of it. */
void rt_guest_menu_on_motion(float dx, float dy);

/* ---- the cursor ----------------------------------------------------------
 *
 * Normalized 0..1 against the presented scanout rectangle, so a coordinate
 * is a fraction of the picture the game drew and the letterbox bars are
 * outside 0..1 by construction. There are two sources, and which one answers
 * depends on whether relative mouse mode is on (host/mouse.h):
 *
 *   relative mode on   The drawn cursor: a position this module keeps,
 *                      started at the centre of the picture the first time
 *                      the pointer takes the mouse, moved by
 *                      rt_guest_menu_on_motion and clamped to 0..1, so it
 *                      cannot leave the picture. ui/cursor.rml draws it,
 *                      because the OS cursor is hidden while relative mode
 *                      is on. False before the pointer has ever taken the
 *                      mouse.
 *
 *   relative mode off  The OS cursor mapped into the presented scanout:
 *                      window logical coordinates scaled to backbuffer
 *                      pixels, the present rectangle's origin subtracted,
 *                      divided by its size. False (leaving the outputs
 *                      alone) when there is no window, nothing has been
 *                      presented yet, the window has no focus, or the
 *                      cursor is outside the scanout rectangle, which is
 *                      the honest report for "the pointer is over nothing
 *                      the game drew".
 *
 * Everything downstream (the hover, the click) reads this one call and does
 * not care which source answered. */
bool rt_guest_menu_cursor(float* nx, float* ny);

#ifdef ICORECOMP_MENU_NAV_TEST
/* Test hooks, compiled only into guest/menu_nav_selftest.cpp's target.
 * There is no windowed backend there, so the cursor has to be injected. */

/* The OS cursor, as the absolute mapping would have produced it. Read only
 * while the injected relative mode is off, which is the default. */
void rt_guest_menu_test_set_cursor(bool valid, float nx, float ny);
/* Stands in for rt_mouse_captured(): true puts the module on the drawn
 * cursor, false on the injected OS cursor above. */
void rt_guest_menu_test_set_captured(bool captured);
/* Stands in for the present rectangle's geometry, as the normalized units
 * of picture one window logical pixel of motion covers on each axis. Zero
 * or negative on either axis means "nothing has been presented yet", which
 * is what the module sees before the first frame. */
void rt_guest_menu_test_set_motion_scale(float per_pixel_x, float per_pixel_y);
/* The drawn cursor's position, or false before the pointer has taken the
 * mouse. Reads the state directly, without the relative-mode gate. */
bool rt_guest_menu_test_drawn_cursor(float* nx, float* ny);
/* Forgets the pointer's state, so the next tick starts clean. */
void rt_guest_menu_test_reset();
#endif

#endif /* ICORECOMP_GUEST_MENU_NAV_H */
