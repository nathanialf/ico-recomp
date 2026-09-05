/* guest/widescreen.h: the horizontal projection scale, on the guest's own
 * projection block.
 *
 * The game builds a projection block in the writer at 0x00114BD8 (the disc
 * listing's gsb_SetVSMatrix) and then tail-jumps to the matrix composer,
 * handing it the block's address in $8. The translator is asked for a callback at the composer's entry
 * (config/entry_hooks.txt, or config/entry_hooks.<target>.txt for any target
 * but `us`), which is the one point where the block is
 * complete and not yet consumed, and this module scales the block's X scale
 * word there.
 *
 * This is not a host-side setting in the sense the rest of the port uses the
 * word: it changes a value the game itself computed, which CLAUDE.md rules
 * out except by an explicit decision. It is therefore off unless something
 * turns it on, it is the one word it touches, and it writes nothing at all
 * while it is off.
 *
 * Runtime-internal, NOT part of the ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_GUEST_WIDESCREEN_H
#define ICORECOMP_GUEST_WIDESCREEN_H

#include "recomp_context.h"

/* The factor to multiply the X scale by: (4/3) / aspect, so 1.0 at the
 * game's own 4:3 and smaller as the picture gets wider. 1.0 and 0.0 both
 * mean off, and so does any value that is not finite and positive, which is
 * logged and then treated as off rather than clamped to something plausible.
 *
 * `mode` is the display.widescreen value the factor came from ("off",
 * "window" or "16_9"). It is used only to name the mode on the one log line
 * a change produces, so that the log says which setting produced the number
 * as well as what the number is. Set by the settings layer,
 * host/settings_apply.cpp. */
void rt_widescreen_set_factor(double k, const char* mode);

/* The aspect the picture is meant to be presented at while widescreen is on:
 * (4/3) / k, so 16/9 in `16_9` mode and the window's own aspect in `window`
 * mode. 0.0 while widescreen is off, which means "present at whatever the
 * scanout derived".
 *
 * There is no separately stored aspect. The factor is derived from the
 * target aspect, so the target aspect is recoverable from the factor, and
 * one number cannot drift from the other. */
double rt_widescreen_target_aspect();

/* Neither renderer reads this module at present time. Both are handed the
 * number above through GsBackend::set_widescreen_aspect, which the settings
 * layer pushes and the GS ring records, so a present uses the value ordered
 * with the fields it applies to rather than whatever the EE thread had
 * stored at that instant. The paraLLEl-GS shim could not read it in any
 * case: it is a separate shared library and cannot link this.
 *
 * Every scanout is presented at that aspect, a full-screen 2D one included
 * (the attract movie, the game's own menus, a fade): nothing at the scanout
 * can tell a 3D field from a 2D one, and a picture whose aspect changed from
 * field to field would be worse than one presented at a single aspect
 * throughout. See docs/SETTINGS.md section 6. */

/* Bumped every time the factor actually moves. hw/gif.cpp uses it to start a
 * fresh window of per-primitive diagnostic lines after a mode change, so a
 * user who turns widescreen on gets the classification report for the scenes
 * right after the change rather than for the run's first fields. */
uint64_t rt_widescreen_generation();

/* The factor in force. 1.0 while it is off. */
double rt_widescreen_factor();

/* Called from rt_entry_hook (src/runtime/hooks.cpp) when the guest enters
 * the matrix composer, RT_ICO_MATRIX_COMPOSER. Returns at once while the
 * factor is off. */
void rt_widescreen_on_composer_entry(R5900Context* ctx);

/* Blocks scaled since the process started, for a later profiler line. It is
 * cumulative across factor changes: it counts work done, not work done at
 * the current factor. */
uint64_t rt_widescreen_applied_count();

#endif /* ICORECOMP_GUEST_WIDESCREEN_H */
