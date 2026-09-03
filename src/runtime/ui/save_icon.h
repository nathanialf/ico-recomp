/* ui/save_icon.h: the mounted disc's own memory-card save icon, rendered
 * for use as the window icon.
 *
 * The same render icon_extract.cpp does at package time (icon.sys's view
 * icon, shape 0), but at run time from whatever disc is mounted, and never
 * fatal: this is a window decoration, not a boot requirement.
 *
 * Runtime-internal, NOT part of the ABI contract.
 */
#ifndef ICORECOMP_UI_SAVE_ICON_H
#define ICORECOMP_UI_SAVE_ICON_H

#include "ps2_icon.h"
#include "ps2_icon_render.h"

#include <cstddef>
#include <cstdint>

/* Reads and parses the mounted disc's save icon: DATA.DF's icon.sys, then
 * the view icon it names, falling back to "boy_blk.ico" when icon.sys names
 * a file DATA.DF does not have. Requires rt_iso_mounted().
 *
 * Both entries are size-checked against a cap before being read, the way
 * ui/title_logo.cpp caps its archive: an outer table entry declares its own
 * length, and DATA.DF is large enough that an entry claiming most of it
 * would otherwise be allocated twice on the way in.
 *
 * The resolve-and-parse half of rt_save_icon_build, separate so a caller
 * wanting several sizes pays the disc read and the parse once. Never fatal:
 * false with a reason in `err` (may be null). Logs under "ui". */
bool rt_save_icon_load(RtPs2Icon& icon, RtPs2IconSys& icon_sys, char* err, size_t err_len);

/* rt_save_icon_load followed by a shape 0 render at `size_px` square.
 *
 * Never fatal: false with a reason in `err` (may be null) leaves `out`
 * invalid and the caller keeps the window's default icon. Logs under "ui"
 * with timing. Not reentrant against itself: the ISO reader it goes through
 * is one global mounted image. */
bool rt_save_icon_build(uint32_t size_px, RtPs2IconImage& out, char* err, size_t err_len);

#endif /* ICORECOMP_UI_SAVE_ICON_H */
