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
 * Text fields (screenshot folder, profiler period, fps limit) apply on Enter or blur
 * only, not per keystroke. That falls out of the same one
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
#include "../host/input.h"
#include "../host/screenshot.h"
#include "../host/window.h"
#include "../hw/hw.h"
#include "../runtime.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rtui {

namespace {

/* One row of a binding table. The Input pane renders all four tables with
 * a data-for loop over an array of these rather than binding one scalar per
 * slot: that would mean a Bind() call here and a hand-written row in
 * menu.rml for each of the 73, and adding a slot would mean touching both.
 *
 * `binding` is the name to show, which is the stored name except while that
 * row is capturing, when it is the prompt. `capturing` drives the row's
 * style, not its text. */
struct UiBindRow {
    std::string label;
    std::string binding;
    bool capturing = false;
};

/* One entry of a select built by a data-for loop rather than by fixed
 * <option> elements in the document. `value` is what the option's value
 * attribute carries, which is what the bound string ends up holding;
 * `label` is what the list shows. Only the window size select needs this:
 * its list is fixed except for the leading "custom" entry, which exists
 * only when the stored size is not one of the offered ones. */
struct UiOptionRow {
    std::string value;
    std::string label;
};

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
    /* "WIDTHxHEIGHT", the value of one of window_sizes below. */
    std::string window_size;
    std::vector<UiOptionRow> window_sizes;
    bool remember_window_size = true;
    std::string fit;
    std::string raster;
    std::string widescreen;
    /* What the active backend's device turned out to be, refreshed with the
     * rest of the mirror rather than per frame. Nothing creates a device to
     * answer: the live backend publishes this once, just after it made its
     * own (hw/hw.h). */
    std::string probe_renderer;
    std::string probe_features;
    std::string filter;
    std::string render_scale;
    std::string screenshot_dir;
    bool show_fps = false;

    /* audio. The four category gains sit beside the master one; each is
     * 0..100 and each is a host output gain (host/settings.h audio). */
    int master_volume = 100;
    std::string master_volume_text;
    bool mute = false;
    int music_volume = 100;
    std::string music_volume_text;
    int effects_volume = 100;
    std::string effects_volume_text;
    int movie_volume = 100;
    std::string movie_volume_text;
    int chime_volume = 60;
    std::string chime_volume_text;

    /* input */
    float left_deadzone = 0.0f;
    float right_deadzone = 0.0f;
    std::string left_deadzone_text;
    std::string right_deadzone_text;
    std::vector<UiBindRow> keyboard_binds;
    std::vector<UiBindRow> gamepad_binds;
    /* Player 2's pad: the same sixteen labels, without the two hotkey rows
     * (host/settings.h RT_GP2_COUNT). */
    std::vector<UiBindRow> gamepad2_binds;
    std::vector<UiBindRow> mouse_binds;
    bool mouse_look = true;
    float mouse_look_sensitivity = 1.0f;
    std::string mouse_look_sensitivity_text;
    bool mouse_look_invert_y = false;
    /* Inline result of the last capture or commit: a reject reason, a
     * timeout, or "" when there is nothing to say. The bool is what the
     * document tests: an Rml data expression coerces a String to bool
     * through Variant's string parse, which reads "rebind cancelled" as
     * false, so "is there a message" has to be its own variable. */
    std::string rebind_status;
    bool has_rebind_status = false;

    /* gameplay */
    bool run_any_direction = false;

    /* achievements */
    bool ach_enabled = true;
    bool ach_toast = true;
    bool ach_sound = false;

    /* debug */
    std::string log_level;
    bool console = false;
    bool log_file = true;
    std::string profile_fields;
    std::string fps_limit_hz;

    /* The cold keys (host/settings.h): debug.console and debug.log_file are
     * applied by restarting the program, which is only offered while the
     * launcher still owns the window. gameplay_active is what the console
     * checkbox is disabled on; debug.log_file has an environment twin that
     * can disable it as well, so it gets its own flag rather than an
     * expression in the document. cold_note is the one sentence both
     * controls show, in red (.note-restart), because it says what a change
     * does rather than what the control is. display.backend was a third
     * cold key until it was retired on 2026-09-05; nothing here mirrors it
     * any more. */
    bool gameplay_active = false;
    bool log_file_locked = false;
    std::string cold_note;

    /* Environment twins: the control is disabled and a hint names the
     * variable that owns the value for this run. */
    bool overridden_fps_limit_hz = false;
    bool overridden_log_level = false;
    bool overridden_profile_fields = false;
    bool overridden_log_file = false;
    bool overridden_mute = false;
    std::string override_text_fps_limit_hz;
    std::string override_text_log_level;
    std::string override_text_profile_fields;
    std::string override_text_log_file;
    std::string override_text_mute;

    /* chrome */
    std::string settings_path;
    std::string active_tab = "achievements";
    std::string fps_text;
    /* The Quit button's own label: "Quit" normally, "Press again to quit"
     * for the 3 s window after the first press. No settings key -- this is
     * UI state, not something that persists across a run. */
    std::string quit_label = "Quit";
    std::string nav_hint;
};

UiSettingsMirror g_m;
Rml::DataModelHandle g_model;
bool g_model_valid = false;

/* Queued by the control callbacks, drained by settings_model_tick(). */
bool g_apply_pending = false;
bool g_reset_pending = false;
bool g_reset_keyboard_binds_pending = false;
bool g_reset_gamepad_binds_pending = false;
bool g_reset_gamepad2_binds_pending = false;
bool g_reset_mouse_binds_pending = false;
/* One bit per mouse slot an Unbind button has asked to clear, drained
 * whole by settings_model_tick(). A set rather than a single slot so that
 * two Unbind clicks in one field both land; a bitmask rather than a vector
 * so the event callback allocates nothing. */
static_assert(RT_MB_COUNT <= 32, "the unbind queue is one bit per mouse slot");
unsigned g_unbind_mouse_pending = 0;

using ModelClock = std::chrono::steady_clock;
ModelClock::time_point g_fps_text_at;
bool g_fps_text_started = false;

/* Quit-button press-again arming. Queued the same way every other control
 * change here is (quit_game() only sets g_quit_pressed; the work happens in
 * settings_model_tick(), at the field boundary, because the second press
 * ends in rt_request_exit -> rt_window_notify_quit, which is not legal from an
 * event callback). */
bool g_quit_pressed = false;
bool g_quit_armed = false;
/* settings_model_enter_card() sets this; settings_model_post_update() acts
 * on it. See the comment on that pair below. */
bool g_focus_pane_pending = false;
ModelClock::time_point g_quit_armed_at;
constexpr auto kQuitArmWindow = std::chrono::seconds(3);

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
constexpr EnumName kFits[] = {
    {"letterbox", (int)RtFit::Letterbox},
    {"integer", (int)RtFit::IntegerScale},
    {"stretch", (int)RtFit::Stretch},
};
constexpr EnumName kRasters[] = {
    {"crt", (int)RtRaster::Crt},
    {"window", (int)RtRaster::Window},
};
/* Matches kWidescreenNames in host/settings.cpp; the option values in
 * ui/menu.rml are the same tokens. */
constexpr EnumName kWidescreens[] = {
    {"off", (int)RtWidescreen::Off},
    {"window", (int)RtWidescreen::Window},
    {"16_9", (int)RtWidescreen::SixteenNine},
};
constexpr EnumName kFilters[] = {
    {"linear", (int)RtFilter::Linear},
    {"nearest", (int)RtFilter::Nearest},
};
constexpr EnumName kLogLevels[] = {
    {"error", (int)RT_LOG_ERROR},
    {"warn", (int)RT_LOG_WARN},
    {"info", (int)RT_LOG_INFO},
    {"debug", (int)RT_LOG_DEBUG},
};

/* Slot labels, in the RtKeyBind and RtPadBind orders. These are what the
 * user reads; the JSON keys settings.cpp holds ("lstick_up") are what the
 * file and the log lines use. */
const char* const kKeyboardLabels[RT_KB_COUNT] = {
    "Up", "Down", "Left", "Right",
    "Cross", "Circle", "Square", "Triangle",
    "L1", "R1", "L2", "R2", "L3", "R3",
    "Start", "Select",
    "Left stick up", "Left stick down", "Left stick left", "Left stick right",
    "Right stick up", "Right stick down", "Right stick left", "Right stick right",
    "Menu key",
    "Screenshot key",
};

/* The window sizes the Display tab offers: the integer multiples of the
 * 640x480 4:3 baseline up to 3840x2880, which is 4K wide. The baseline is
 * also the size the UI documents are laid out against (ui.cpp density_for),
 * so every entry here is a whole number of dp ratios. A size settings.json
 * holds that is not in this table is still honored: settings.cpp validates
 * the range, and the select grows a leading "custom" entry for it
 * (refresh_window_sizes below), so the control never shows a size the
 * settings do not hold. */
struct WindowSizeOption {
    int width, height;
    const char* label;
};

const WindowSizeOption kWindowSizes[] = {
    { 640, 480, "640 x 480 (1x)" },
    { 1280, 960, "1280 x 960 (2x)" },
    { 1920, 1440, "1920 x 1440 (3x)" },
    { 2560, 1920, "2560 x 1920 (4x)" },
    { 3200, 2400, "3200 x 2400 (5x)" },
    { 3840, 2880, "3840 x 2880 (6x)" },
};

/* The first sixteen RtKeyBind labels, which is exactly the RtMouseBind
 * order: the mouse has the DS2 buttons and no stick or menu slots. */
const char* const kMouseLabels[RT_MB_COUNT] = {
    "Up", "Down", "Left", "Right",
    "Cross", "Circle", "Square", "Triangle",
    "L1", "R1", "L2", "R2", "L3", "R3",
    "Start", "Select",
    "Screenshot button",
};

/* What the table shows for a mouse slot holding "". The stored value stays
 * empty; this is display only, because a blank cell reads as a rendering
 * fault rather than as a slot nobody bound. */
constexpr const char* kUnboundLabel = "unbound";

const char* const kGamepadLabels[RT_GP_COUNT] = {
    "Up", "Down", "Left", "Right",
    "Cross", "Circle", "Square", "Triangle",
    "L1", "R1", "L2", "R2", "L3", "R3",
    "Start", "Select",
    "Menu button",
    "Screenshot button",
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
    rt_log_warn("ui", "menu: %s = \"%s\" is not one of the options this build knows;"
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
        rt_log_warn("ui", "menu: %s = \"%s\" is not a whole number; keeping %d",
            dotted, text.c_str(), *out);
        return false;
    }
    *out = (int)v;
    return true;
}

/* "WIDTHxHEIGHT", whole string, both parts positive. The select can only
 * produce a value this parses, so a failure here means the document and
 * this file disagree; it is logged and both dimensions are kept. */
bool parse_size_field(const std::string& text, int* width, int* height) {
    const char* begin = skip_spaces(text.c_str());
    char* end = nullptr;
    const long w = std::strtol(begin, &end, 10);
    if (end == begin || *end != 'x') {
        rt_log_warn("ui", "menu: window size \"%s\" is not WIDTHxHEIGHT; keeping %dx%d",
            text.c_str(), *width, *height);
        return false;
    }
    const char* second = end + 1;
    char* end2 = nullptr;
    const long h = std::strtol(second, &end2, 10);
    if (end2 == second || *skip_spaces(end2) != '\0' || w <= 0 || h <= 0) {
        rt_log_warn("ui", "menu: window size \"%s\" is not WIDTHxHEIGHT; keeping %dx%d",
            text.c_str(), *width, *height);
        return false;
    }
    *width = (int)w;
    *height = (int)h;
    return true;
}

/* True when the field holds exactly `word`, ignoring surrounding spaces and
 * letter case. For the one settings field whose value can be a word rather
 * than a number. */
bool field_is_word(const std::string& text, const char* word) {
    const char* p = skip_spaces(text.c_str());
    size_t i = 0;
    for (; word[i]; ++i) {
        if (std::tolower((unsigned char)p[i]) != (unsigned char)word[i]) return false;
    }
    return *skip_spaces(p + i) == '\0';
}

bool parse_double_field(const std::string& text, const char* dotted, double* out) {
    const char* begin = skip_spaces(text.c_str());
    char* end = nullptr;
    const double v = std::strtod(begin, &end);
    if (end == begin || *skip_spaces(end) != '\0') {
        rt_log_warn("ui", "menu: %s = \"%s\" is not a number; keeping %.6g",
            dotted, text.c_str(), *out);
        return false;
    }
    *out = v;
    return true;
}

/* ---- mirror <-> settings ------------------------------------------------ */

/* The stored name for one slot, as a pointer into the struct being edited.
 * The reset and unbind paths below are the only writers here; ui_rebind.cpp
 * has its own read-only twin over rt_settings(). NULL for a device or slot
 * outside the tables, which the callers do not produce: the counts come
 * from rt_settings_bind_slot_count(). */
std::string* bind_slot_storage(RtSettings& s, RtBindDevice device, int slot) {
    switch (device) {
    case RT_BIND_KEYBOARD: return (slot >= 0 && slot < RT_KB_COUNT) ? &s.input.keyboard[slot] : nullptr;
    case RT_BIND_GAMEPAD:  return (slot >= 0 && slot < RT_GP_COUNT) ? &s.input.gamepad[slot] : nullptr;
    case RT_BIND_GAMEPAD2: return (slot >= 0 && slot < RT_GP2_COUNT) ? &s.input.gamepad2[slot] : nullptr;
    case RT_BIND_MOUSE:    return (slot >= 0 && slot < RT_MB_COUNT) ? &s.input.mouse[slot] : nullptr;
    default: return nullptr;
    }
}

void set_override(const char* dotted, bool* flag, std::string* text) {
    *flag = rt_settings_overridden(dotted);
    const char* env = rt_settings_env_twin(dotted);
    *text = *flag && env[0] ? std::string("overridden by ") + env : std::string();
}

/* Rebuilds the window size list for the size the settings currently hold.
 * A size that is one of kWindowSizes gives the plain six-entry list; any
 * other size (a hand edit, or a size remembered from a resize drag with
 * display.remember_window_size on) gets a seventh entry at the top naming
 * itself, whose value is that size, so selecting it is a no-op rather than
 * a silent move to some other size. */
void refresh_window_sizes() {
    const RtSettings& s = rt_settings();
    bool offered = false;
    for (const WindowSizeOption& o : kWindowSizes) {
        if (o.width == s.display.window_width && o.height == s.display.window_height) offered = true;
    }

    g_m.window_sizes.clear();
    if (!offered) {
        UiOptionRow row;
        row.value = g_m.window_size;
        row.label = fmt("custom (%dx%d)", s.display.window_width, s.display.window_height);
        g_m.window_sizes.push_back(row);
    }
    for (const WindowSizeOption& o : kWindowSizes) {
        UiOptionRow row;
        row.value = fmt("%dx%d", o.width, o.height);
        row.label = o.label;
        g_m.window_sizes.push_back(row);
    }
}

/* The Display tab's two read-only lines. See rt_gs_probe_renderer_line in
 * hw/hw.h: they are a read of what the live backend published, not a probe
 * that makes a device. */
void refresh_device_lines() {
    g_m.probe_renderer = rt_gs_probe_renderer_line();
    g_m.probe_features = rt_gs_probe_features_line();
}

void settings_to_mirror() {
    const RtSettings& s = rt_settings();
    refresh_device_lines();

    g_m.display_mode = name_of(kDisplayModes, (int)s.display.mode);
    g_m.window_size = fmt("%dx%d", s.display.window_width, s.display.window_height);
    refresh_window_sizes();
    g_m.remember_window_size = s.display.remember_window_size;
    g_m.fit = name_of(kFits, (int)s.display.fit);
    g_m.raster = name_of(kRasters, (int)s.display.raster);
    g_m.widescreen = name_of(kWidescreens, (int)s.display.widescreen);
    g_m.filter = name_of(kFilters, (int)s.display.filter);
    /* Unlike window_size, this select needs no entry for a value the file
     * holds: settings.cpp accepts only kRenderScales ({1, 4, 8, 16}), which
     * is exactly the option list in menu.rml, so a retired 2 in the file is
     * rejected at load and never reaches the mirror. */
    g_m.render_scale = fmt("%d", s.display.render_scale);
    g_m.show_fps = s.display.show_fps;
    g_m.screenshot_dir = s.display.screenshot_dir;

    g_m.master_volume = s.audio.master_volume;
    g_m.master_volume_text = fmt("%d", s.audio.master_volume);
    g_m.mute = s.audio.mute;
    g_m.music_volume = s.audio.music_volume;
    g_m.music_volume_text = fmt("%d", s.audio.music_volume);
    g_m.effects_volume = s.audio.effects_volume;
    g_m.effects_volume_text = fmt("%d", s.audio.effects_volume);
    g_m.movie_volume = s.audio.movie_volume;
    g_m.movie_volume_text = fmt("%d", s.audio.movie_volume);
    g_m.chime_volume = s.audio.chime_volume;
    g_m.chime_volume_text = fmt("%d", s.audio.chime_volume);

    g_m.left_deadzone = s.input.left_deadzone;
    g_m.right_deadzone = s.input.right_deadzone;
    g_m.left_deadzone_text = fmt("%.2f", (double)s.input.left_deadzone);
    g_m.right_deadzone_text = fmt("%.2f", (double)s.input.right_deadzone);
    g_m.mouse_look = s.input.mouse_look;
    g_m.mouse_look_sensitivity = s.input.mouse_look_sensitivity;
    g_m.mouse_look_sensitivity_text = fmt("%.2f", (double)s.input.mouse_look_sensitivity);
    g_m.mouse_look_invert_y = s.input.mouse_look_invert_y;

    /* Rebuilt rather than patched: a commit can revert a binding (the menu
     * key colliding with a pad slot, a duplicate), and the pane has to show
     * what was kept. A capture in progress re-marks its own row afterwards
     * through settings_model_set_rebind(). */
    g_m.keyboard_binds.resize(RT_KB_COUNT);
    for (int i = 0; i < RT_KB_COUNT; ++i) {
        g_m.keyboard_binds[i].label = kKeyboardLabels[i];
        g_m.keyboard_binds[i].binding = s.input.keyboard[i];
        g_m.keyboard_binds[i].capturing = false;
    }
    g_m.gamepad_binds.resize(RT_GP_COUNT);
    for (int i = 0; i < RT_GP_COUNT; ++i) {
        g_m.gamepad_binds[i].label = kGamepadLabels[i];
        g_m.gamepad_binds[i].binding = s.input.gamepad[i];
        g_m.gamepad_binds[i].capturing = false;
    }
    /* Player 2 reads the first sixteen gamepad labels: the same DS2
     * buttons in the same order, with no hotkey rows past them. */
    g_m.gamepad2_binds.resize(RT_GP2_COUNT);
    for (int i = 0; i < RT_GP2_COUNT; ++i) {
        g_m.gamepad2_binds[i].label = kGamepadLabels[i];
        g_m.gamepad2_binds[i].binding = s.input.gamepad2[i];
        g_m.gamepad2_binds[i].capturing = false;
    }
    g_m.mouse_binds.resize(RT_MB_COUNT);
    for (int i = 0; i < RT_MB_COUNT; ++i) {
        g_m.mouse_binds[i].label = kMouseLabels[i];
        /* "" is a legal committed value on this device and means the slot
         * is unbound; the table says so in words. */
        g_m.mouse_binds[i].binding = s.input.mouse[i].empty() ? kUnboundLabel : s.input.mouse[i];
        g_m.mouse_binds[i].capturing = false;
    }

    g_m.run_any_direction = s.gameplay.run_any_direction;

    g_m.ach_enabled = s.achievements.enabled;
    g_m.ach_toast = s.achievements.toast;
    g_m.ach_sound = s.achievements.sound;

    g_m.log_level = name_of(kLogLevels, (int)s.debug.log_level);
    g_m.console = s.debug.console;
    g_m.log_file = s.debug.log_file;
    g_m.profile_fields = fmt("%d", s.debug.profile_fields);
    /* The default is not a rate, so it is not shown as one: the text box
     * reads "mode", and the parse below takes that word back. "%g" of -1
     * would look like a rate the user had typed. */
    g_m.fps_limit_hz = s.debug.fps_limit_hz == RT_FPS_LIMIT_MODE_RATE
        ? std::string("mode") : fmt("%g", s.debug.fps_limit_hz);

    set_override("debug.fps_limit_hz", &g_m.overridden_fps_limit_hz, &g_m.override_text_fps_limit_hz);
    set_override("debug.log_level", &g_m.overridden_log_level, &g_m.override_text_log_level);
    set_override("debug.profile_fields", &g_m.overridden_profile_fields, &g_m.override_text_profile_fields);
    set_override("debug.log_file", &g_m.overridden_log_file, &g_m.override_text_log_file);
    set_override("audio.mute", &g_m.overridden_mute, &g_m.override_text_mute);

    g_m.gameplay_active = rt_settings_gameplay_active();
    g_m.log_file_locked = g_m.overridden_log_file || g_m.gameplay_active;
    g_m.cold_note = g_m.gameplay_active
        ? "Change this from the launcher; the program restarts to apply it"
        : "Changing this restarts the program";

    const char* path = rt_settings_path();
    g_m.settings_path = path[0] ? path : "no settings file yet";
}

void mirror_to_settings() {
    RtSettings& s = rt_settings_mutable();
    int e = 0;

    if (value_of(kDisplayModes, g_m.display_mode, "display.mode", &e)) s.display.mode = (RtDisplayMode)e;
    if (value_of(kFits, g_m.fit, "display.fit", &e)) s.display.fit = (RtFit)e;
    if (value_of(kRasters, g_m.raster, "display.raster", &e)) s.display.raster = (RtRaster)e;
    if (value_of(kWidescreens, g_m.widescreen, "display.widescreen", &e)) s.display.widescreen = (RtWidescreen)e;
    /* A key whose environment twin is set is never applied (settings.h),
     * and neither is a cold key once the guest is running: the select is
     * disabled then, but the mirror is written by the refresh, not only by
     * the control, so the guard belongs here as well. commit_validate()
     * refuses the same change a second time, for the writers that never
     * come through this file. */
    if (value_of(kFilters, g_m.filter, "display.filter", &e)) s.display.filter = (RtFilter)e;

    parse_size_field(g_m.window_size, &s.display.window_width, &s.display.window_height);
    s.display.remember_window_size = g_m.remember_window_size;
    parse_int_field(g_m.render_scale, "display.render_scale", &s.display.render_scale);
    s.display.show_fps = g_m.show_fps;
    /* Written back as typed: any path is accepted, and host/screenshot.cpp
     * creates it on the first capture (or logs once and skips). */
    s.display.screenshot_dir = g_m.screenshot_dir;

    s.audio.master_volume = g_m.master_volume;
    s.audio.music_volume = g_m.music_volume;
    s.audio.effects_volume = g_m.effects_volume;
    s.audio.movie_volume = g_m.movie_volume;
    s.audio.chime_volume = g_m.chime_volume;
    /* A key whose environment twin is set is never applied (settings.h);
     * the row's click expression toggles the mirror regardless of the
     * disabled box inside it, so the guard lives here, and the refresh
     * below puts the mirror back to the effective value. */
    if (!g_m.overridden_mute) s.audio.mute = g_m.mute;

    s.input.left_deadzone = g_m.left_deadzone;
    s.input.right_deadzone = g_m.right_deadzone;
    s.input.mouse_look = g_m.mouse_look;
    s.input.mouse_look_sensitivity = g_m.mouse_look_sensitivity;
    s.input.mouse_look_invert_y = g_m.mouse_look_invert_y;
    /* The four binding tables are not written back here. They are display
     * rows: ui_rebind.cpp and the unbind queue below are the only writers,
     * and both go through rt_settings_mutable() themselves. */

    s.gameplay.run_any_direction = g_m.run_any_direction;

    s.achievements.enabled = g_m.ach_enabled;
    s.achievements.toast = g_m.ach_toast;
    s.achievements.sound = g_m.ach_sound;

    if (!g_m.overridden_log_level && value_of(kLogLevels, g_m.log_level, "debug.log_level", &e)) {
        s.debug.log_level = (RtLogLevel)e;
    }
    /* Same rule as audio.mute below it in this file: the check row's click
     * expression toggles the mirror regardless of the disabled box inside
     * it, so the guard lives here and the refresh puts the mirror back to
     * the value that was kept. */
    if (!g_m.gameplay_active) s.debug.console = g_m.console;
    /* Cold as well, and with an environment twin: log_file_locked is both
     * reasons in one flag. */
    if (!g_m.log_file_locked) s.debug.log_file = g_m.log_file;
    parse_int_field(g_m.profile_fields, "debug.profile_fields", &s.debug.profile_fields);
    /* "mode" (and "auto", which is what a user is as likely to type) is
     * the sentinel: pace to the video mode the game programmed. */
    if (field_is_word(g_m.fps_limit_hz, "mode") || field_is_word(g_m.fps_limit_hz, "auto")) {
        s.debug.fps_limit_hz = RT_FPS_LIMIT_MODE_RATE;
    } else {
        parse_double_field(g_m.fps_limit_hz, "debug.fps_limit_hz", &s.debug.fps_limit_hz);
    }
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

/* Queued like every other control change: the work (arming, or the second
 * press's flush-and-exit) happens in settings_model_tick(). */
void on_quit_game(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    g_quit_pressed = true;
}

/* The Display tab's "Take screenshot" button. Straight through to the same
 * request the hotkey makes: the capture is taken before the overlay pass, so
 * the menu the button lives in is not in the file, and the user does not have
 * to close the menu and find the key to get one. */
void on_take_screenshot(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    rt_screenshot_request();
}

/* data-for gives the row index as it_index; the document passes it through.
 * A missing or out-of-range argument means the document and this file
 * disagree about the tables, which is worth a line rather than a silent
 * no-op. Outside the SDL guard because the unbind callback below needs it
 * in every build. */
int slot_argument(const Rml::VariantList& arguments, int count) {
    const int slot = arguments.size() == 1 ? arguments[0].Get<int>(-1) : -1;
    if (slot < 0 || slot >= count) {
        rt_log_warn("ui", "menu: a binding button was pressed for slot %d, which is not one"
            " of this device's 0..%d; the document and ui_settings_model.cpp disagree",
            slot, count - 1);
        return -1;
    }
    return slot;
}

#ifdef ICORECOMP_HAVE_SDL
void on_rebind_keyboard(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments) {
    const int slot = slot_argument(arguments, RT_KB_COUNT);
    if (slot >= 0) rebind_begin(RT_BIND_KEYBOARD, slot);
}

void on_rebind_gamepad(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments) {
    const int slot = slot_argument(arguments, RT_GP_COUNT);
    if (slot >= 0) rebind_begin(RT_BIND_GAMEPAD, slot);
}

void on_rebind_gamepad2(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments) {
    const int slot = slot_argument(arguments, RT_GP2_COUNT);
    if (slot >= 0) rebind_begin(RT_BIND_GAMEPAD2, slot);
}

void on_rebind_mouse(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments) {
    const int slot = slot_argument(arguments, RT_MB_COUNT);
    if (slot >= 0) rebind_begin(RT_BIND_MOUSE, slot);
}
#else
/* No SDL means no capture: there are no events to capture and no names to
 * resolve. The buttons stay in the document and say so when pressed.
 * Unbind is not among them: clearing a slot is a settings write, not a
 * capture, and works in a build with no SDL. */
void on_rebind_keyboard(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    rt_log_warn("ui", "menu: this build has no SDL, so a binding cannot be captured");
}
void on_rebind_gamepad(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    rt_log_warn("ui", "menu: this build has no SDL, so a binding cannot be captured");
}
void on_rebind_gamepad2(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    rt_log_warn("ui", "menu: this build has no SDL, so a binding cannot be captured");
}
void on_rebind_mouse(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    rt_log_warn("ui", "menu: this build has no SDL, so a binding cannot be captured");
}
#endif

/* Queued, not applied: the commit that follows runs the appliers, which is
 * the same reason every other control change in this file waits for the
 * field boundary. */
void on_unbind_mouse(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& arguments) {
    const int slot = slot_argument(arguments, RT_MB_COUNT);
    if (slot >= 0) g_unbind_mouse_pending |= 1u << slot;
}

void on_reset_keyboard_binds(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    g_reset_keyboard_binds_pending = true;
}

void on_reset_gamepad_binds(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    g_reset_gamepad_binds_pending = true;
}

void on_reset_gamepad2_binds(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    g_reset_gamepad2_binds_pending = true;
}

void on_reset_mouse_binds(Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
    g_reset_mouse_binds_pending = true;
}

} // namespace

bool settings_model_init(Rml::Context* context) {
    Rml::DataModelConstructor c = context->CreateDataModel("settings");
    if (!c) {
        rt_log_warn("ui", "Context::CreateDataModel(\"settings\") failed; the settings UI is disabled");
        return false;
    }

    c.Bind("display_mode", &g_m.display_mode);
    c.Bind("window_size", &g_m.window_size);
    c.Bind("remember_window_size", &g_m.remember_window_size);
    c.Bind("fit", &g_m.fit);
    c.Bind("raster", &g_m.raster);
    c.Bind("widescreen", &g_m.widescreen);
    c.Bind("probe_renderer", &g_m.probe_renderer);
    c.Bind("probe_features", &g_m.probe_features);
    c.Bind("filter", &g_m.filter);
    c.Bind("render_scale", &g_m.render_scale);
    c.Bind("show_fps", &g_m.show_fps);
    c.Bind("screenshot_dir", &g_m.screenshot_dir);

    c.Bind("master_volume", &g_m.master_volume);
    c.Bind("master_volume_text", &g_m.master_volume_text);
    c.Bind("mute", &g_m.mute);
    c.Bind("music_volume", &g_m.music_volume);
    c.Bind("music_volume_text", &g_m.music_volume_text);
    c.Bind("effects_volume", &g_m.effects_volume);
    c.Bind("effects_volume_text", &g_m.effects_volume_text);
    c.Bind("movie_volume", &g_m.movie_volume);
    c.Bind("movie_volume_text", &g_m.movie_volume_text);
    c.Bind("chime_volume", &g_m.chime_volume);
    c.Bind("chime_volume_text", &g_m.chime_volume_text);

    c.Bind("left_deadzone", &g_m.left_deadzone);
    c.Bind("right_deadzone", &g_m.right_deadzone);
    c.Bind("left_deadzone_text", &g_m.left_deadzone_text);
    c.Bind("right_deadzone_text", &g_m.right_deadzone_text);
    c.Bind("mouse_look", &g_m.mouse_look);
    c.Bind("mouse_look_sensitivity", &g_m.mouse_look_sensitivity);
    c.Bind("mouse_look_sensitivity_text", &g_m.mouse_look_sensitivity_text);
    c.Bind("mouse_look_invert_y", &g_m.mouse_look_invert_y);

    /* Same registration order as the binding rows below: the struct, then
     * the array of it, then the Bind. The window size select is a data-for
     * over this array rather than fixed <option> elements because its list
     * is not fixed: it carries a "custom" entry when the stored size is not
     * one of the offered ones. */
    if (Rml::StructHandle<UiOptionRow> opt = c.RegisterStruct<UiOptionRow>()) {
        opt.RegisterMember("value", &UiOptionRow::value);
        opt.RegisterMember("label", &UiOptionRow::label);
    } else {
        rt_log_warn("ui", "RegisterStruct<UiOptionRow> failed; the window size select is disabled");
    }
    if (!c.RegisterArray<std::vector<UiOptionRow>>()) {
        rt_log_warn("ui", "RegisterArray<vector<UiOptionRow>> failed; the window size select is disabled");
    }
    c.Bind("window_sizes", &g_m.window_sizes);

    /* The struct has to be registered before the array whose value type it
     * is, and both before the Bind of a vector of them. */
    if (Rml::StructHandle<UiBindRow> row = c.RegisterStruct<UiBindRow>()) {
        row.RegisterMember("label", &UiBindRow::label);
        row.RegisterMember("binding", &UiBindRow::binding);
        row.RegisterMember("capturing", &UiBindRow::capturing);
    } else {
        rt_log_warn("ui", "RegisterStruct<UiBindRow> failed; the binding tables are disabled");
    }
    if (!c.RegisterArray<std::vector<UiBindRow>>()) {
        rt_log_warn("ui", "RegisterArray<vector<UiBindRow>> failed; the binding tables are disabled");
    }
    c.Bind("keyboard_binds", &g_m.keyboard_binds);
    c.Bind("gamepad_binds", &g_m.gamepad_binds);
    c.Bind("gamepad2_binds", &g_m.gamepad2_binds);
    c.Bind("mouse_binds", &g_m.mouse_binds);
    c.Bind("rebind_status", &g_m.rebind_status);
    c.Bind("has_rebind_status", &g_m.has_rebind_status);

    c.Bind("run_any_direction", &g_m.run_any_direction);

    c.Bind("ach_enabled", &g_m.ach_enabled);
    c.Bind("ach_toast", &g_m.ach_toast);
    c.Bind("ach_sound", &g_m.ach_sound);

    c.Bind("log_level", &g_m.log_level);
    c.Bind("console", &g_m.console);
    c.Bind("log_file", &g_m.log_file);
    c.Bind("profile_fields", &g_m.profile_fields);
    c.Bind("fps_limit_hz", &g_m.fps_limit_hz);

    /* The cold keys' two controls read these (see the mirror). */
    c.Bind("gameplay_active", &g_m.gameplay_active);
    c.Bind("log_file_locked", &g_m.log_file_locked);
    c.Bind("cold_note", &g_m.cold_note);

    c.Bind("overridden_fps_limit_hz", &g_m.overridden_fps_limit_hz);
    c.Bind("overridden_log_level", &g_m.overridden_log_level);
    c.Bind("overridden_profile_fields", &g_m.overridden_profile_fields);
    c.Bind("overridden_log_file", &g_m.overridden_log_file);
    c.Bind("overridden_mute", &g_m.overridden_mute);
    c.Bind("override_text_fps_limit_hz", &g_m.override_text_fps_limit_hz);
    c.Bind("override_text_log_level", &g_m.override_text_log_level);
    c.Bind("override_text_profile_fields", &g_m.override_text_profile_fields);
    c.Bind("override_text_log_file", &g_m.override_text_log_file);
    c.Bind("override_text_mute", &g_m.override_text_mute);

    c.Bind("settings_path", &g_m.settings_path);
    c.Bind("active_tab", &g_m.active_tab);
    c.Bind("fps_text", &g_m.fps_text);
    c.Bind("quit_label", &g_m.quit_label);
    c.Bind("nav_hint", &g_m.nav_hint);

    c.BindEventCallback("apply", on_control_change);
    c.BindEventCallback("reset_defaults", on_reset_defaults);
    c.BindEventCallback("close_menu", on_close);
    c.BindEventCallback("quit_game", on_quit_game);
    c.BindEventCallback("take_screenshot", on_take_screenshot);
    c.BindEventCallback("rebind_keyboard", on_rebind_keyboard);
    c.BindEventCallback("rebind_gamepad", on_rebind_gamepad);
    c.BindEventCallback("rebind_gamepad2", on_rebind_gamepad2);
    c.BindEventCallback("rebind_mouse", on_rebind_mouse);
    c.BindEventCallback("unbind_mouse", on_unbind_mouse);
    c.BindEventCallback("reset_keyboard_binds", on_reset_keyboard_binds);
    c.BindEventCallback("reset_gamepad_binds", on_reset_gamepad_binds);
    c.BindEventCallback("reset_gamepad2_binds", on_reset_gamepad2_binds);
    c.BindEventCallback("reset_mouse_binds", on_reset_mouse_binds);

    /* Read here and refreshed with the rest of the mirror on every menu
     * open (settings_to_mirror). No device is created to answer: the two
     * lines describe the device the active backend already made, published
     * once into the window service (host/window_service.h). Reading them at
     * model init alone would show "no renderer device created yet" on the
     * boot-first ordering, where the model exists before the backend. */
    refresh_device_lines();

    g_model = c.GetModelHandle();
    g_model_valid = true;
    return true;
}

const char* bind_slot_label(RtBindDevice device, int slot) {
    switch (device) {
    case RT_BIND_GAMEPAD: return (slot >= 0 && slot < RT_GP_COUNT) ? kGamepadLabels[slot] : "?";
    case RT_BIND_GAMEPAD2: return (slot >= 0 && slot < RT_GP2_COUNT) ? kGamepadLabels[slot] : "?";
    case RT_BIND_MOUSE:   return (slot >= 0 && slot < RT_MB_COUNT) ? kMouseLabels[slot] : "?";
    case RT_BIND_KEYBOARD: return (slot >= 0 && slot < RT_KB_COUNT) ? kKeyboardLabels[slot] : "?";
    default: return "?";
    }
}

const char* bind_device_name(RtBindDevice device) {
    switch (device) {
    case RT_BIND_KEYBOARD: return "keyboard";
    case RT_BIND_GAMEPAD:  return "gamepad";
    case RT_BIND_GAMEPAD2: return "gamepad2";
    case RT_BIND_MOUSE:    return "mouse";
    default: return "?";
    }
}

namespace {
/* active_tab's values (menu.rml's nav buttons set one of these) and the id
 * of the button that selects each, in the same order the tabs are laid out
 * (menu.rml section-numbers 01..06). One place, so the shoulder-cycle below
 * and the initial-focus fix agree with the document. */
constexpr const char* kTabOrder[] = {"achievements", "display", "audio", "input", "gameplay", "debug"};
constexpr const char* kTabButtonIds[] = {"nav-achievements", "nav-display", "nav-audio", "nav-input",
                                         "nav-gameplay", "nav-debug"};
constexpr size_t kTabCount = sizeof(kTabOrder) / sizeof(kTabOrder[0]);
} // namespace

void settings_model_focus_active_tab() {
    if (!g_ui.menu) return;
    for (size_t i = 0; i < kTabCount; ++i) {
        if (g_m.active_tab != kTabOrder[i]) continue;
        if (Rml::Element* button = g_ui.menu->GetElementById(kTabButtonIds[i])) button->Focus(true);
        return;
    }
}

void settings_model_cycle_tab(int direction) {
    if (!g_model_valid || !g_ui.visible) return;
    size_t idx = 0;
    for (size_t i = 0; i < kTabCount; ++i) {
        if (g_m.active_tab == kTabOrder[i]) {
            idx = i;
            break;
        }
    }
    idx = (size_t)(((int)idx + direction + (int)kTabCount) % (int)kTabCount);
    g_m.active_tab = kTabOrder[idx];
    g_model.DirtyVariable("active_tab");
    settings_model_focus_active_tab();
}

void settings_model_focus_card(int direction) {
    if (!g_ui.menu || !g_ui.context) return;
    Rml::Element* focus = g_ui.context->GetFocusElement();
    if (!focus) return;
    const Rml::String id = focus->GetId();
    for (size_t i = 0; i < kTabCount; ++i) {
        if (id != kTabButtonIds[i]) continue;
        const size_t next = (size_t)(((int)i + direction + (int)kTabCount) % (int)kTabCount);
        if (Rml::Element* button = g_ui.menu->GetElementById(kTabButtonIds[next])) button->Focus(true);
        return;
    }
}

namespace {

/* RmlUi's own definition of focusable (ElementDocument.cpp, the anonymous
 * CanFocusElement): visible, focus not turned off by style, tab-index:
 * auto. Mirrored here rather than called, since that function is private to
 * ElementDocument.cpp; kept to the same three checks so this walk agrees
 * with what Tab already does. */
bool element_focusable(Rml::Element* el) {
    if (!el->IsVisible()) return false;
    const Rml::ComputedValues& computed = el->GetComputedValues();
    if (computed.focus() == Rml::Style::Focus::None) return false;
    return computed.tab_index() == Rml::Style::TabIndex::Auto;
}

/* Pre-order walk, i.e. document order: the first descendant of `node` for
 * which element_focusable() is true, or null. `node` itself is never
 * tested, only its descendants -- the one caller below starts at the pane,
 * which is never itself a focus target. Every node visited is checked for
 * its own visibility before recursing into it, so a .section a data-if has
 * set display: none on is skipped whole rather than walked and rejected
 * control by control. */
Rml::Element* first_focusable_descendant(Rml::Element* node) {
    const int n = node->GetNumChildren();
    for (int i = 0; i < n; ++i) {
        Rml::Element* child = node->GetChild(i);
        if (!child->IsVisible()) continue;
        if (element_focusable(child)) return child;
        if (Rml::Element* found = first_focusable_descendant(child)) return found;
    }
    return nullptr;
}

} // namespace

void settings_model_focus_first_in_pane() {
    if (!g_ui.menu) return;
    Rml::Element* pane = g_ui.menu->GetElementById("pane");
    if (!pane) return;
    Rml::Element* found = first_focusable_descendant(pane);
    if (!found) return;
    found->Focus(true);
    /* The pane scrolls (`overflow-y: auto`), and a plain Focus() does not
     * scroll the way RmlUi's own Tab and arrow handling does (ElementDocument
     * ::ProcessDefaultAction pairs every Focus with a ScrollIntoView). Without
     * this, entering a pane that was left scrolled down puts the focus ring
     * somewhere the user cannot see. */
    found->ScrollIntoView(true);
}

void settings_model_enter_card() {
    if (!g_ui.context) return;
    Rml::Element* focus = g_ui.context->GetFocusElement();
    if (!focus) return;
    const Rml::String id = focus->GetId();
    for (size_t i = 0; i < kTabCount; ++i) {
        if (id != kTabButtonIds[i]) continue;
        if (g_m.active_tab != kTabOrder[i]) {
            g_m.active_tab = kTabOrder[i];
            g_model.DirtyVariable("active_tab");
        }
        /* Queued, not done here. DirtyVariable only marks the variable; the
         * data-if that decides which .section is displayed is re-run by
         * Rml::Context::Update(), which rt_ui_tick calls after this whole
         * tick. Focusing now would walk the pane as it still is, land on a
         * control in the section that is about to be hidden, and RmlUi then
         * blurs an element whose display goes none (Element.cpp, the
         * visibility branch of OnPropertyChange) onto its parent -- a plain
         * div inside the hidden section, which current_nav_level still reads
         * as level 2 and which no direction can move off. */
        g_focus_pane_pending = true;
        return;
    }
}

void settings_model_post_update() {
    if (!g_focus_pane_pending) return;
    g_focus_pane_pending = false;
    if (!g_ui.visible) return;
    settings_model_focus_first_in_pane();
}

void settings_model_disarm_quit() {
    if (!g_quit_armed) return;
    g_quit_armed = false;
    g_m.quit_label = "Quit";
    if (g_model_valid) g_model.DirtyVariable("quit_label");
}

void settings_model_set_rebind(bool active, RtBindDevice device, int slot, const std::string& status) {
    if (!g_model_valid) return;

    /* Start from the stored names either way: ending a capture has to put
     * the row's real binding back, and starting one has to clear a row an
     * earlier capture was on. */
    settings_to_mirror();
    if (active) {
        std::vector<UiBindRow>* rows = nullptr;
        const char* prompt = "";
        switch (device) {
        case RT_BIND_KEYBOARD:
            rows = &g_m.keyboard_binds;
            prompt = "press a key";
            break;
        case RT_BIND_GAMEPAD:
            rows = &g_m.gamepad_binds;
            prompt = (slot == rt_settings_bind_menu_slot(RT_BIND_GAMEPAD))
                ? "press a button, or hold two together"
                : "press a button or move an axis";
            break;
        case RT_BIND_GAMEPAD2:
            rows = &g_m.gamepad2_binds;
            /* No chord prompt: player 2's table has no menu slot for one to
             * be legal in. */
            prompt = "press a button or move an axis on player 2's pad";
            break;
        case RT_BIND_MOUSE:
            rows = &g_m.mouse_binds;
            prompt = "press a mouse button or turn the wheel";
            break;
        default:
            break;
        }
        if (rows && slot >= 0 && slot < (int)rows->size()) {
            (*rows)[slot].capturing = true;
            (*rows)[slot].binding = prompt;
        }
    }
    g_m.rebind_status = status;
    g_m.has_rebind_status = !status.empty();
    g_model.DirtyAllVariables();
}

void settings_model_refresh() {
    if (!g_model_valid) return;
    settings_to_mirror();
    /* The rebind status line is about one capture, not about the settings.
     * Clearing it here retires it when the menu is reopened or any other
     * control is changed; ui_rebind.cpp sets it through
     * settings_model_set_rebind() after this runs, so a message a capture
     * just produced is not the one being cleared. */
    g_m.rebind_status.clear();
    g_m.has_rebind_status = false;
    /* Same reasoning as the rebind status: a reopen (or any other commit)
     * is not a second press, so it must not read as one. */
    settings_model_disarm_quit();
    /* A queued pane focus belongs to the card press that queued it. A reopen
     * has already put the focus on the active tab's card and must not then be
     * dragged into the pane by a press from the last time the menu was up. */
    g_focus_pane_pending = false;
    g_model.DirtyAllVariables();
    sync_fps_document();
}

/* The footer's control hint follows the last device used, so a pad user
 * sees the pad line without touching anything else. One string per device
 * per document; rewritten only when the device flips.
 *
 * `two_level` picks the menu's pair over the launcher's: the
 * launcher has no cards and no pane, its navigation is flat, and East does
 * nothing there at all (ui_events.cpp's close_menu returns false with the
 * menu not up, so a stray press cannot quit out of the launcher). Escape is
 * the same: it belongs to the menu, not to the launcher. */
void sync_nav_hint(std::string* hint, Rml::DataModelHandle* model, bool two_level) {
    const bool pad = rt_input_last_device() == RT_INPUT_DEVICE_CONTROLLER;
    const char* text;
    if (two_level) {
        text = pad ? "Up Down choose a card, Left Right switch, cross opens it, circle backs out"
                   : "Arrows or Tab move, Enter selects, Escape closes";
    } else {
        text = pad ? "Up Down Left Right move, cross selects"
                   : "Arrows or Tab move, Enter selects";
    }
    if (*hint == text) return;
    *hint = text;
    model->DirtyVariable("nav_hint");
}

namespace {

/* The commit half of the cold-key rule (host/settings.h). Called with the
 * committed settings as they were before the edit; when a cold key moved,
 * the run restarts to apply it.
 *
 * The order matters. The file is written first, because the successor reads
 * it at startup and the whole point is that it comes up on the new value.
 * rt_restart_now() then prepares the new process but tears nothing down
 * itself: it asks for the ordinary exit, so the GS backend writes its
 * pipeline cache, the window is destroyed and the log is drained exactly as
 * they are on Quit, and the successor is only started once all of that has
 * happened.
 *
 * A restart that cannot be prepared is not fatal and reverts nothing: the
 * new value is in the file and will be used at the next launch, which is
 * what a cold key did before this existed. */
void restart_if_cold_key_changed(const RtSettings& before) {
    const char* key = rt_settings_cold_key_changed(before, rt_settings());
    if (!key) return;

    rt_log_info("settings", "restarting to apply %s", key);
    rt_settings_flush_save();

    char why[128];
    std::snprintf(why, sizeof why, "restarting to apply %s", key);
    char err[512];
    /* True means the exit is under way, and with no window it does not
     * return at all. */
    if (rt_restart_now(why, err, sizeof err)) return;

    rt_log_error("settings", "could not restart to apply %s: %s; the value is saved and takes effect"
        " at the next launch, this run keeps the one it started with", key, err);
    char msg[1024];
    std::snprintf(msg, sizeof msg,
        "ICO could not restart itself to apply %s.\n\n%s\n\n"
        "The new value is saved and will be used the next time you start ICO."
        " This run keeps the value it started with.\n", key, err);
    rt_show_message("ICO", msg);
}

} // namespace

void settings_model_tick() {
    if (!g_model_valid) return;
    sync_nav_hint(&g_m.nav_hint, &g_model, /*two_level=*/true);

    if (g_reset_pending) {
        g_reset_pending = false;
        g_apply_pending = false;
#ifdef ICORECOMP_HAVE_SDL
        rebind_cancel("the settings were reset to defaults", /*drop_accepted=*/true);
#endif
        const RtSettings before = rt_settings();
        rt_settings_reset_defaults();
        rt_settings_commit(false);
        rt_settings_request_save();
        settings_model_refresh();
        rt_log_info("ui", "menu: reset to defaults");
        /* Reset is a commit like any other, so a default that differs from
         * the value this run started on is a cold-key change and restarts
         * the program, exactly as changing the control itself would. */
        restart_if_cold_key_changed(before);
    } else if (g_reset_keyboard_binds_pending || g_reset_gamepad_binds_pending ||
               g_reset_gamepad2_binds_pending || g_reset_mouse_binds_pending) {
        /* One device per tick, in device order. Clicking more than one of
         * the four buttons before the next field is unlikely but the later
         * clicks must not be dropped: the other flags stay set and land on
         * the ticks after this one. */
        RtBindDevice device = RT_BIND_KEYBOARD;
        if (g_reset_keyboard_binds_pending) {
            g_reset_keyboard_binds_pending = false;
        } else if (g_reset_gamepad_binds_pending) {
            device = RT_BIND_GAMEPAD;
            g_reset_gamepad_binds_pending = false;
        } else if (g_reset_gamepad2_binds_pending) {
            device = RT_BIND_GAMEPAD2;
            g_reset_gamepad2_binds_pending = false;
        } else {
            device = RT_BIND_MOUSE;
            g_reset_mouse_binds_pending = false;
        }
#ifdef ICORECOMP_HAVE_SDL
        rebind_cancel("the bindings for that device were reset", /*drop_accepted=*/true);
#endif
        RtSettings& m = rt_settings_mutable();
        const int count = rt_settings_bind_slot_count(device);
        for (int i = 0; i < count; ++i) {
            std::string* stored = bind_slot_storage(m, device, i);
            if (stored) *stored = rt_settings_default_binding(device, i);
        }
        rt_settings_commit(false);
        rt_settings_request_save();
        settings_model_refresh();
        rt_log_info("ui", "menu: %s bindings reset to defaults", bind_device_name(device));
    } else if (g_unbind_mouse_pending) {
        /* Every slot the Unbind buttons queued, in one commit. "" is what
         * an unbound mouse slot holds; the table renders it as "unbound".
         * The capture goes with it: the refresh below rebuilds every row,
         * which would otherwise leave a capture armed with nothing on
         * screen saying so. */
        const unsigned slots = g_unbind_mouse_pending;
        g_unbind_mouse_pending = 0;
#ifdef ICORECOMP_HAVE_SDL
        rebind_cancel("a mouse binding was cleared", /*drop_accepted=*/true);
#endif
        RtSettings& m = rt_settings_mutable();
        for (int i = 0; i < RT_MB_COUNT; ++i) {
            if (!(slots & (1u << i))) continue;
            m.input.mouse[i].clear();
            rt_log_info("ui", "menu: input.mouse.%s unbound",
                rt_settings_binding_key(RT_BIND_MOUSE, i));
        }
        rt_settings_commit(false);
        rt_settings_request_save();
        settings_model_refresh();
    } else if (g_apply_pending) {
        g_apply_pending = false;
        /* A copy, taken before mirror_to_settings() writes the same struct:
         * it is what a cold-key change is measured against below. */
        const RtSettings before = rt_settings();
        mirror_to_settings();
        rt_settings_commit(false);
        rt_settings_request_save();
        /* commit_validate may have reverted a value; show what was kept. */
        settings_model_refresh();
        /* Last, so the menu is showing the committed values if the restart
         * cannot be prepared and the run carries on. */
        restart_if_cold_key_changed(before);
    } else if (g_quit_pressed) {
        g_quit_pressed = false;
        if (g_quit_armed) {
            settings_model_disarm_quit();
            rt_settings_flush_save();
            rt_request_exit("Quit from the menu");
        } else {
            g_quit_armed = true;
            g_quit_armed_at = ModelClock::now();
            g_m.quit_label = "Press again to quit";
            g_model.DirtyVariable("quit_label");
        }
    }

    /* The arm window, checked every tick regardless of which branch above
     * ran (or none did): a press that is never repeated has to fall back to
     * "Quit" on its own, not only when some other control change happens to
     * run settings_model_refresh() first. */
    if (g_quit_armed && ModelClock::now() - g_quit_armed_at >= kQuitArmWindow) {
        settings_model_disarm_quit();
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
