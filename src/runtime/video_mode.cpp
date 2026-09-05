/* video_mode.cpp: the live CRT video mode and the field timeline numbers
 * derived from it. See video_mode.h for where the numbers come from.
 *
 * State is one enum. Everything else is a switch on it, so a mode change is
 * one store plus the hooks the deadline holders registered.
 */
#include "video_mode.h"

#include "runtime.h"

namespace {

RtVideoMode g_mode = RT_VIDEO_MODE_BOOT;
bool g_ever_set = false;

constexpr int kMaxHooks = 8;
RtVideoModeHook g_hooks[kMaxHooks];
int g_hook_count = 0;

} // namespace

RtVideoMode rt_video_mode() { return g_mode; }

const char* rt_video_mode_name() { return g_mode == RT_VIDEO_PAL ? "PAL" : "NTSC"; }

uint64_t rt_cycles_per_field() {
    return g_mode == RT_VIDEO_PAL ? RT_CYCLES_PER_FIELD_PAL : RT_CYCLES_PER_FIELD_NTSC;
}

uint64_t rt_cycles_vblank() {
    return g_mode == RT_VIDEO_PAL ? RT_CYCLES_VBLANK_PAL : RT_CYCLES_VBLANK_NTSC;
}

uint64_t rt_cycles_per_hblank() {
    return g_mode == RT_VIDEO_PAL ? RT_CYCLES_PER_HBLANK_PAL : RT_CYCLES_PER_HBLANK_NTSC;
}

double rt_field_rate_hz() {
    return g_mode == RT_VIDEO_PAL ? RT_FIELD_HZ_PAL : RT_FIELD_HZ_NTSC;
}

uint32_t rt_lines_per_frame() {
    return g_mode == RT_VIDEO_PAL ? RT_LINES_PER_FRAME_PAL : RT_LINES_PER_FRAME_NTSC;
}

void rt_video_add_mode_hook(RtVideoModeHook hook) {
    if (!hook) return;
    if (g_hook_count >= kMaxHooks) {
        rt_log_error("video", "more than %d video mode hooks registered; '%p' will not be called"
            " on a mode change", kMaxHooks, (void*)hook);
        return;
    }
    g_hooks[g_hook_count++] = hook;
}

void rt_video_set_mode(RtVideoMode mode, const char* why) {
    const RtVideoMode prev = g_mode;
    if (mode == prev && g_ever_set) return;
    g_ever_set = true;
    g_mode = mode;
    if (mode == prev) {
        rt_log_info("video", "%s: video mode %s confirmed: field %llu cycles (%.2f Hz),"
            " H-blank %llu cycles, vblank %llu cycles",
            why, rt_video_mode_name(), (unsigned long long)rt_cycles_per_field(),
            rt_field_rate_hz(), (unsigned long long)rt_cycles_per_hblank(),
            (unsigned long long)rt_cycles_vblank());
        return;
    }
    rt_log_info("video", "%s: video mode %s -> %s: field %llu cycles (%.2f Hz),"
        " H-blank %llu cycles, vblank %llu cycles",
        why, prev == RT_VIDEO_PAL ? "PAL" : "NTSC", rt_video_mode_name(),
        (unsigned long long)rt_cycles_per_field(), rt_field_rate_hz(),
        (unsigned long long)rt_cycles_per_hblank(), (unsigned long long)rt_cycles_vblank());
    for (int i = 0; i < g_hook_count; ++i) g_hooks[i](prev, mode);
}
