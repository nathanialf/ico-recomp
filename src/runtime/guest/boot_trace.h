/* Boot trace: the game's boot state words, logged on change for the first
 * fields of a run. See boot_trace.cpp. Runtime-internal, not part of the ABI
 * contract (include/recomp_*.h). */
#ifndef ICORECOMP_GUEST_BOOT_TRACE_H
#define ICORECOMP_GUEST_BOOT_TRACE_H

#include <cstdint>

/* Once per field from the field hook (hw/gspriv.cpp rt_gs_vsync_hook), on
 * the EE thread, where guest memory is quiescent. Bounded: it reads ten
 * words a field for the first RT_BOOT_TRACE_FIELDS fields and never again. */
void rt_boot_trace_field();

/* Fields the trace covers; shared with the presenter's display trace
 * (gs/gs_parallel_present.cpp) so the two logs cover the same span. 600
 * fields is twelve seconds at 50 Hz, past the SCE logo on a boot that skips
 * the two PAL screens. */
constexpr uint64_t RT_BOOT_TRACE_FIELDS = 600;

#endif
