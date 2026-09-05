/* Boot trace, guest half.
 *
 * Why: on 2026-09-05 the user reported two white flashes before the SCE
 * logo on a boot that loads the memory card's system block and so never
 * shows the language and 50/60 Hz screens. Nothing in the log at the time
 * said what the display held on those fields or what the game was doing,
 * so this and the presenter's display trace (gs/gs_parallel_scanout.cpp,
 * gs/gs_parallel_present.cpp) were added: three streams, all keyed on the
 * field number, all bounded to the first RT_BOOT_TRACE_FIELDS fields and to
 * changes only, so a boot log carries a few dozen lines and a long run
 * carries nothing more.
 *
 * This half logs the game's own boot state machine (kanbanBootMain and
 * kanbanBootMcCheck state words, guest/ico_syms.h), the kanban node the
 * game is taking input on, the stage id, the layout id, the stage fade
 * gsb_fade draws (state, speed, level, colour) and the brightness word, each
 * time any of them changes. Read only; nothing is written to guest memory. */
#include "boot_trace.h"

#include "gmem.h"
#include "ico_syms.h"
#include "../runtime.h"

namespace {

struct Snapshot {
    int32_t main_state = 0;
    int32_t mc_state = 0;
    uint32_t kanban = 0;
    int32_t stage = 0;
    int32_t layout = 0;
    int32_t fade_state = 0;
    float fade_speed = 0.f;
    float fade_level = 0.f;
    uint32_t fade_rgba = 0;
    int32_t brightness = 0;
    bool operator==(const Snapshot& o) const {
        return main_state == o.main_state && mc_state == o.mc_state &&
               kanban == o.kanban && stage == o.stage && layout == o.layout &&
               fade_state == o.fade_state && fade_speed == o.fade_speed &&
               fade_level == o.fade_level && fade_rgba == o.fade_rgba &&
               brightness == o.brightness;
    }
};

uint64_t g_field = 0;
bool g_have_last = false;
Snapshot g_last;

} // namespace

void rt_boot_trace_field() {
    if (g_field >= RT_BOOT_TRACE_FIELDS) return;
    const uint64_t field = ++g_field;

    Snapshot s;
    /* A word that is not readable leaves its field at zero and the line
     * says so through `unmapped`; guest RAM is mapped from the first field,
     * so this is a guard, not a path anything takes. */
    bool ok = true;
    ok &= rt_gmem::read_i32(RT_ICO_KANBAN_BOOT_MAIN_STATE, &s.main_state);
    ok &= rt_gmem::read_i32(RT_ICO_KANBAN_BOOT_MC_STATE, &s.mc_state);
    ok &= rt_gmem::read_word(RT_ICO_KANBAN_ACTIVE, &s.kanban);
    ok &= rt_gmem::read_i32(RT_ICO_STAGE_ID, &s.stage);
    ok &= rt_gmem::read_i32(RT_ICO_LAYOUT_ID, &s.layout);
    ok &= rt_gmem::read_i32(RT_ICO_FADE_GSB_STATE, &s.fade_state);
    ok &= rt_gmem::read_f32(RT_ICO_FADE_GSB_SPEED, &s.fade_speed);
    ok &= rt_gmem::read_f32(RT_ICO_FADE_GSB_LEVEL, &s.fade_level);
    ok &= rt_gmem::read_word(RT_ICO_FADE_GSB_RGBA, &s.fade_rgba);
    ok &= rt_gmem::read_i32(RT_ICO_BRIGHTNESS, &s.brightness);

    if (g_have_last && s == g_last) {
        if (field == RT_BOOT_TRACE_FIELDS) {
            rt_log_info("boot", "boot trace: guest state trace ends at field %llu",
                        (unsigned long long)field);
        }
        return;
    }
    g_have_last = true;
    g_last = s;
    rt_log_info("boot", "boot trace: field %llu: kanbanBoot main state %d, mc-check state %d,"
                " kanban node 0x%08x, stage %d, layout 0x%x; gsb fade state %d speed %.1f"
                " level %.1f rgba %u,%u,%u,%u; brightness %d%s",
                (unsigned long long)field, s.main_state, s.mc_state, s.kanban, s.stage,
                (unsigned)s.layout, s.fade_state, (double)s.fade_speed, (double)s.fade_level,
                s.fade_rgba & 0xffu, (s.fade_rgba >> 8) & 0xffu, (s.fade_rgba >> 16) & 0xffu,
                (s.fade_rgba >> 24) & 0xffu, s.brightness,
                ok ? "" : " (a word was unmapped, shown as 0)");
    if (field == RT_BOOT_TRACE_FIELDS) {
        rt_log_info("boot", "boot trace: guest state trace ends at field %llu",
                    (unsigned long long)field);
    }
}
