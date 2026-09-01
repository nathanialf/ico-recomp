/* ui/ui_settings_model.cpp: the "settings" Rml data model behind menu.rml
 * and fps.rml.
 *
 * Shape: one UI-side mirror of RtSettings (UiSettingsMirror below) bound
 * variable by variable into a data model named "settings", plus the per-key
 * "overridden by ICORECOMP_X" flags, the active tab, the settings path and
 * the fps readout text. The documents read and write the mirror; nothing
 * under ui/ reaches rt_settings() itself.
 *
 * Why control changes are queued instead of applied in the callback, which
 * is the one non-obvious decision in this file:
 *
 *   1. Reentrancy. A change event is dispatched from
 *      Rml::Context::Process*, which runs in rt_window_pump, which can
 *      execute from inside Granite's WSI::begin_frame. rt_settings_commit
 *      runs rt_settings_apply, and that calls rt_window_apply_mode
 *      (SDL window calls) and rt_pgs_set_presentation (fatal mid-frame,
 *      the library's m_in_frame guard). The commit has to happen at the
 *      field boundary, which is settings_model_tick(), called from
 *      rt_ui_tick.
 *
 *   2. Listener order. Both the data-value controller (which writes the
 *      mirror from the event) and our data-event-change callback listen for
 *      the same "change" event on the same element, and RmlUi instantiates
 *      them in the order it walks the element's attribute map
 *      (ElementUtilities.cpp ApplyDataViewsControllers), which is not
 *      specified. Applying in the callback would therefore sometimes read a
 *      mirror the controller had not written yet, and worse, the
 *      controller's write could land after settings_model_refresh() had
 *      already put the validated value back, leaving the menu showing a
 *      value the settings do not hold. By the next tick both have run.
 *
 * Text fields (window size, verbose, profiler period, fps limit) apply on
 * Enter or blur only, not per keystroke. That falls out of the same one
 * callback: RmlUi's text widget puts "linebreak" in its change event
 * (WidgetTextInput::DispatchChangeEvent), false for an ordinary keystroke
 * and true for Enter, and no other control sends that parameter at all. A
 * change with linebreak=false is the only case the callback ignores; the
 * blur event on those same fields has no parameters and applies.
 */
#include "ui.h"

#ifdef ICORECOMP_UI

#include "ui_internal.h"

#include "../host/settings.h"
#include "../hw/hw.h"
#include "../runtime.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rtui {

namespace {

/* ---- the mirror ---------------------------------------------------------
 *
 * Selects and text fields are strings because that is what an Rml form
 * control's value is; the parse back into the typed settings happens in
 * mirror_to_settings() and reports its own failures. Ranges and checkboxes
 * keep their native type. The *_text members are display-only: a bound int
 * or float renders through Rml's own number formatting, which spells 0.15
 * as 0.150000.
 */
struct UiSettingsMirror {
    /* display */
    std::string display_mode;
    std::string window_width;
    std::string window_height;
    std::string present;
    std::string fit;
    std::string filter;
    std::string render_scale;
    bool hires_scanout = false;
    bool show_fps = false;

    /* audio */
    int master_volume = 100;
    std::string master_volume_text;
    bool mute = false;

    /* input */
    float left_deadzone = 0.0f;
    float right_deadzone = 0.0f;
    float trigger_threshold = 0.25f;
    std::string left_deadzone_text;
    std::string right_deadzone_text;
    std::string trigger_threshold_text;
    bool rumble = true;

    /* debug */
    std::string verbose;
    bool log_file = true;
    std::string profile_fields;
    std::string fps_limit_hz;

    /* Environment twins: the control is disabled and a hint names the
     * variable that owns the value for this run. */
    bool overridden_present = false;
    bool overridden_fps_limit_hz = false;
    bool overridden_verbose = false;
    bool overridden_profile_fields = false;
    bool overridden_log_file = false;
    bool overridden_mute = false;
    std::string override_text_present;
    std::string override_text_fps_limit_hz;
    std::string override_text_verbose;
    std::string override_text_profile_fields;
    std::string override_text_log_file;
    std::string override_text_mute;

    /* chrome */
    std::string settings_path;
    std::string active_tab = "display";
    std::string fps_text;
};

UiSettingsMirror g_m;
Rml::DataModelHandle g_model;
bool g_model_valid = false;

/* Queued by the control callbacks, drained by settings_model_tick(). */
bool g_apply_pending = false;
bool g_reset_pending = false;

using ModelClock = std::chrono::steady_clock;
ModelClock::time_point g_fps_text_at;
bool g_fps_text_started = false;

/* ---- enum name tables ---------------------------------------------------
 *
 * The same spellings settings.cpp reads and writes in the JSON, so a value
 * picked here round-trips through the file unchanged. The option values in
 * menu.rml must match these.
 */
struct EnumName {
    const char* name;
    int value;
};

constexpr EnumName kDisplayModes[] = {
    {"windowed", (int)RtDisplayMode::Windowed},
    {"fullscreen_desktop", (int)RtDisplayMode::FullscreenDesktop},
    {"fullscreen_exclusive", (int)RtDisplayMode::FullscreenExclusive},
};
constexpr EnumName kPresentModes[] = {
    {"mailbox", (int)RtPresentMode::Mailbox},
    {"fifo", (int)RtPresentMode::Fifo},
    {"immediate", (int)RtPresentMode::Immediate},
};
constexpr EnumName kFits[] = {
    {"letterbox", (int)RtFit::Letterbox},
    {"integer", (int)RtFit::IntegerScale},
    {"stretch", (int)RtFit::Stretch},
};
constexpr EnumName kFilters[] = {
    {"linear", (int)RtFilter::Linear},
    {"nearest", (int)RtFilter::Nearest},
};

template <size_t N>
const char* name_of(const EnumName (&table)[N], int value) {
    for (size_t i = 0; i < N; ++i) {
        if (table[i].value == value) return table[i].name;
    }
    return table[0].name;
}

/* Unknown string keeps the current value and says so: the menu can only
 * produce the option values it ships, so this means the document and this
 * file disagree. */
template <size_t N>
bool value_of(const EnumName (&table)[N], const std::string& name, const char* dotted, int* out) {
    for (size_t i = 0; i < N; ++i) {
        if (name == table[i].name) {
            *out = table[i].value;
            return true;
        }
    }
    rt_log("ui", "settings menu: %s = \"%s\" is not one of the options this build knows;"
                 " keeping the current value", dotted, name.c_str());
    return false;
}

/* ---- text field parsing ------------------------------------------------- */

std::string fmt(const char* format, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    return std::string(buf);
}

const char* skip_spaces(const char* p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}

/* Whole-string parse: trailing junk is a failure, not a prefix match. On
 * failure the previous value stays and the reason is logged; the menu shows
 * the kept value again after the refresh that follows the commit. */
bool parse_int_field(const std::string& text, const char* dotted, int* out) {
    const char* begin = skip_spaces(text.c_str());
    char* end = nullptr;
    const long v = std::strtol(begin, &end, 10);
    if (end == begin || *skip_spaces(end) != '\0') {
        rt_log("ui", "settings menu: %s = \"%s\" is not a whole number; keeping %d",
            dotted, text.c_str(), *out);
        return false;
    }
    *out = (int)v;
    return true;
}

bool parse_double_field(const std::string& text, const char* dotted, double* out) {
    const char* begin = skip_spaces(text.c_str());
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin || *skip_spaces(end) != '\0') {
        rt_log("ui", "settings menu: %s = \"%s\" is not a number; keeping %.6g",
            dotted, text.c_str(), *out);
        return false;
    }
    *out = v;
    return true;
}

/* ---- mirror <-> settings ------------------------------------------------ */

void set_override(const char* dotted, bool* flag, std::string* text) {
    *flag = rt_settings_overridden(dotted);
    const char* env = rt_settings_env_twin(dotted);
    *text = *flag && env[0] ? std::string("overridden by ") + env : std::string();
}

void settings_to_mirror() {
    const RtSettings& s = rt_settings();

    g_m.display_mode = name_of(kDisplayModes, (int)s.display.mode);
    g_m.window_width = fmt("%d", s.display.window_width);
    g_m.window_height = fmt("%d", s.display.window_height);
    g_m.present = name_of(kPresentModes, (int)s.display.present);
    g_m.fit = name_of(kFits, (int)s.display.fit);
    g_m.filter = name_of(kFilters, (int)s.display.filter);
    g_m.render_scale = fmt("%d", s.display.render_scale);
    g_m.hires_scanout = s.display.hires_scanout;
    g_m.show_fps = s.display.show_fps;

    g_m.master_volume = s.audio.master_volume;
    g_m.master_volume_text = fmt("%d", s.audio.master_volume);
    g_m.mute = s.audio.mute;

    g_m.left_deadzone = s.input.left_deadzone;
    g_m.right_deadzone = s.input.right_deadzone;
    g_m.trigger_threshold = s.input.trigger_threshold;
    g_m.left_deadzone_text = fmt("%.2f", (double)s.input.left_deadzone);
    g_m.right_deadzone_text = fmt("%.2f", (double)s.input.right_deadzone);
    g_m.trigger_threshold_text = fmt("%.2f", (double)s.input.trigger_threshold);
    g_m.rumble = s.input.rumble;

    g_m.verbose = s.debug.verbose;
    g_m.log_file = s.debug.log_file;
    g_m.profile_fields = fmt("%d", s.debug.profile_fields);
    g_m.fps_limit_hz = fmt("%g", s.debug.fps_limit_hz);

    set_override("display.present", &g_m.overridden_present, &g_m.override_text_present);
    set_override("debug.fps_limit_hz", &g_m.overridden_fps_limit_hz, &g_m.override_text_fps_limit_hz);
    set_override("debug.verbose", &g_m.overridden_verbose, &g_m.override_text_verbose);
    set_override("debug.profile_fields", &g_m.overridden_profile_fields, &g_m.override_text_profile_fields);
    set_override("debug.log_file", &g_m.overridden_log_file, &g_m.override_text_log_file);
    set_override("audio.mute", &g_m.overridden_mute, &g_m.override_text_mute);

    const char* path = rt_settings_path();
    g_m.settings_path = path[0] ? path : "no settings file yet";
}

void mirror_to_settings() {
    RtSettings& s = rt_settings_mutable();
    int e = 0;

    if (value_of(kDisplayModes, g_m.display_mode, "display.mode", &e)) s.display.mode = (RtDisplayMode)e;
    if (value_of(kPresentModes, g_m.present, "display.present", &e)) s.display.present = (RtPresentMode)e;
    if (value_of(kFits, g_m.fit, "display.fit", &e)) s.display.fit = (RtFit)e;
    if (value_of(kFilters, g_m.filter, "display.filter", &e)) s.display.filter = (RtFilter)e;

    parse_int_field(g_m.window_width, "display.window_width", &s.display.window_width);
    parse_int_field(g_m.window_height, "display.window_height", &s.display.window_height);
    parse_int_field(g_m.render_scale, "display.render_scale", &s.display.render_scale);
    s.display.hires_scanout = g_m.hires_scanout;
    s.display.show_fps = g_m.show_fps;

    s.audio.master_volume = g_m.master_volume;
    s.audio.mute = g_m.mute;

    s.input.left_deadzone = g_m.left_deadzone;
    s.input.right_deadzone = g_m.right_deadzone;
    s.input.trigger_threshold = g_m.trigger_threshold;
    s.input.rumble = g_m.rumble;

    s.debug.verbose = g_m.verbose;
    s.debug.log_file = g_m.log_file;
    parse_int_field(g_m.profile_fields, "debug.profile_fields", &s.debug.profile_fields);
    parse_double_field(g_m.fps_limit_hz, "debug.fps_limit_hz", &s.debug.fps_limit_hz);
}

void sync_fps_document() {
    if (!g_ui.fps) return;
    const bool want = rt_settings().display.show_fps;
    if (want == g_ui.fps_visible) return;
    g_ui.fps_visible = want;
    if (want) {
        /* No focus: the readout is not interactive and must not take the
         * keyboard away from the menu. */
        g_ui.fps->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    } else {
        g_ui.fps->Hide();
    }
}

/* ---- event callbacks ---------------------------------------------------- */

void on_control_change(Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&) {
    if (ev == Rml::EventId::Change) {
        /* Only the text widget sends this parameter (see the file comment):
         * false means an ordinary keystroke, which does not apply. */
        const auto& params = ev.GetParameters();
        const auto it = params.find("linebreak");
        if (it != params.end() && !it->second.Get<bool>()) return;
    }
    g_apply_pending = true;
}

void on_reset_defaults(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    /* Queued for the same reason a control change is: the commit that
     * follows runs the appliers. */
    g_reset_pending = true;
}

void on_close(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    rt_ui_set_visible(false);
}

} // namespace

bool settings_model_init(Rml::Context* context) {
    Rml::DataModelConstructor c = context->CreateDataModel("settings");
    if (!c) {
        rt_log("ui", "Context::CreateDataModel(\"settings\") failed; the settings UI is disabled");
        return false;
    }

    c.Bind("display_mode", &g_m.display_mode);
    c.Bind("window_width", &g_m.window_width);
    c.Bind("window_height", &g_m.window_height);
    c.Bind("present", &g_m.present);
    c.Bind("fit", &g_m.fit);
    c.Bind("filter", &g_m.filter);
    c.Bind("render_scale", &g_m.render_scale);
    c.Bind("hires_scanout", &g_m.hires_scanout);
    c.Bind("show_fps", &g_m.show_fps);

    c.Bind("master_volume", &g_m.master_volume);
    c.Bind("master_volume_text", &g_m.master_volume_text);
    c.Bind("mute", &g_m.mute);

    c.Bind("left_deadzone", &g_m.left_deadzone);
    c.Bind("right_deadzone", &g_m.right_deadzone);
    c.Bind("trigger_threshold", &g_m.trigger_threshold);
    c.Bind("left_deadzone_text", &g_m.left_deadzone_text);
    c.Bind("right_deadzone_text", &g_m.right_deadzone_text);
    c.Bind("trigger_threshold_text", &g_m.trigger_threshold_text);
    c.Bind("rumble", &g_m.rumble);

    c.Bind("verbose", &g_m.verbose);
    c.Bind("log_file", &g_m.log_file);
    c.Bind("profile_fields", &g_m.profile_fields);
    c.Bind("fps_limit_hz", &g_m.fps_limit_hz);

    c.Bind("overridden_present", &g_m.overridden_present);
    c.Bind("overridden_fps_limit_hz", &g_m.overridden_fps_limit_hz);
    c.Bind("overridden_verbose", &g_m.overridden_verbose);
    c.Bind("overridden_profile_fields", &g_m.overridden_profile_fields);
    c.Bind("overridden_log_file", &g_m.overridden_log_file);
    c.Bind("overridden_mute", &g_m.overridden_mute);
    c.Bind("override_text_present", &g_m.override_text_present);
    c.Bind("override_text_fps_limit_hz", &g_m.override_text_fps_limit_hz);
    c.Bind("override_text_verbose", &g_m.override_text_verbose);
    c.Bind("override_text_profile_fields", &g_m.override_text_profile_fields);
    c.Bind("override_text_log_file", &g_m.override_text_log_file);
    c.Bind("override_text_mute", &g_m.override_text_mute);

    c.Bind("settings_path", &g_m.settings_path);
    c.Bind("active_tab", &g_m.active_tab);
    c.Bind("fps_text", &g_m.fps_text);

    c.BindEventCallback("apply", on_control_change);
    c.BindEventCallback("reset_defaults", on_reset_defaults);
    c.BindEventCallback("close_menu", on_close);

    g_model = c.GetModelHandle();
    g_model_valid = true;
    return true;
}

void settings_model_refresh() {
    if (!g_model_valid) return;
    settings_to_mirror();
    g_model.DirtyAllVariables();
    sync_fps_document();
}

void settings_model_tick() {
    if (!g_model_valid) return;

    if (g_reset_pending) {
        g_reset_pending = false;
        g_apply_pending = false;
        rt_settings_reset_defaults();
        rt_settings_commit(false);
        rt_settings_request_save();
        settings_model_refresh();
        rt_log("ui", "settings menu: reset to defaults");
    } else if (g_apply_pending) {
        g_apply_pending = false;
        mirror_to_settings();
        rt_settings_commit(false);
        rt_settings_request_save();
        /* commit_validate may have reverted a value; show what was kept. */
        settings_model_refresh();
    }

    if (!g_ui.fps_visible) return;

    /* Four times a second, not per field: the readout is for a person
     * reading it, and the numbers themselves are a one-second average
     * (rt_gs_field_stats). */
    const auto now = ModelClock::now();
    if (g_fps_text_started && now - g_fps_text_at < std::chrono::milliseconds(250)) return;
    g_fps_text_started = true;
    g_fps_text_at = now;

    double fps = 0.0, field_ms = 0.0;
    rt_gs_field_stats(&fps, &field_ms);
    std::string text = fmt("%.1f fields/s  %.1f ms", fps, field_ms);
    if (text != g_m.fps_text) {
        g_m.fps_text = std::move(text);
        g_model.DirtyVariable("fps_text");
    }
}

} // namespace rtui

#endif /* ICORECOMP_UI */
