/* host/settings.cpp: see settings.h for the model and the API contract.
 *
 * Load precedence (first match wins):
 *   1. ICORECOMP_SETTINGS env var, no fallback. "-" or "0" mean
 *      defaults-only, saving disabled -- this is the same opt-out spelling
 *      ICORECOMP_LOG uses elsewhere in this codebase.
 *   2. <rt_base_dir()>/settings.json, if it exists (the "portable folder"
 *      promise: a copy next to the packaged binary wins over the per-user
 *      copy so a USB-stick install stays self-contained).
 *   3. <rt_user_config_dir()>/settings.json, if it exists.
 *   4. Neither exists: compiled-in defaults, no file yet. The first
 *      successful save picks a target by trying rt_base_dir() then
 *      rt_user_config_dir(), and that choice is sticky for the run.
 *
 * Bad-value policy is "loud failure beats silent wrongness" applied to a
 * config file the user (or a hand-edit) can break in any number of ways,
 * and it should never cost them the rest of a working file:
 *   - a parse error copies the raw file to "<path>.bad" and runs on
 *     defaults; the broken original is preserved and never overwritten
 *     again, which means saving is off for the rest of that run;
 *   - a field with the wrong JSON type or an out-of-range value keeps the
 *     compiled-in default for that one field, logs why, and the rest of
 *     the file still loads;
 *   - unknown keys anywhere in the document are kept in the retained DOM
 *     (see g_dom below) across a load/save round trip and logged once;
 *   - "version" > 1 means a newer build wrote this file: run on defaults
 *     and never touch the file, so downgrading and re-upgrading doesn't
 *     lose anything;
 *   - nothing here ever calls rt_fatal(). A settings file is user data,
 *     not a build invariant.
 */
#include "runtime.h"

#include "host/json.h"
#include "host/mouse_names.h"
#include "host/portable.h"
#include "host/settings.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iterator>

namespace {

/* ---- bind default tables ------------------------------------------------
 *
 * The keyboard and gamepad values are SDL_GetScancodeName / SDL
 * mapping-string tokens; the mouse values are host/mouse_names.h names, SDL
 * having no name lookup for mouse buttons. They are the only copy:
 * host/input.cpp builds its tables from rt_settings() and falls back to
 * these through rt_settings_default_binding() when a stored name does not
 * resolve, so there is no second hardcoded map to keep in sync.
 */
struct BindDef {
    const char* json_key;
    const char* def;
};

/* A trailing '+' or '-' on an axis name (lefttrigger+, righttrigger+) is
 * our convention for direction, not part of the SDL token itself. */
constexpr BindDef kKeyboardBinds[RT_KB_COUNT] = {
    {"up", "Up"}, {"down", "Down"}, {"left", "Left"}, {"right", "Right"},
    {"cross", "X"}, {"circle", "C"}, {"square", "Z"}, {"triangle", "Space"},
    {"l1", "Q"}, {"r1", "E"}, {"l2", "1"}, {"r2", "3"}, {"l3", "T"}, {"r3", "Y"},
    {"start", "Return"}, {"select", "Backspace"},
    {"lstick_up", "W"}, {"lstick_down", "S"}, {"lstick_left", "A"}, {"lstick_right", "D"},
    {"rstick_up", "I"}, {"rstick_down", "K"}, {"rstick_left", "J"}, {"rstick_right", "L"},
    {"menu", "F1"},
    {"screenshot", "F12"},
};

constexpr BindDef kGamepadBinds[RT_GP_COUNT] = {
    {"up", "dpup"}, {"down", "dpdown"}, {"left", "dpleft"}, {"right", "dpright"},
    {"cross", "a"}, {"circle", "b"}, {"square", "x"}, {"triangle", "y"},
    {"l1", "leftshoulder"}, {"r1", "rightshoulder"}, {"l2", "lefttrigger+"}, {"r2", "righttrigger+"},
    {"l3", "leftstick"}, {"r3", "rightstick"}, {"start", "start"}, {"select", "back"},
    {"menu", "guide"},
    /* The screenshot hotkey ships on the keyboard only. A pad button spare
     * enough to give it does not exist on a DS2 layout once the menu chord
     * has taken guide, and binding one by default would take a button the
     * game can see away from every player who never wanted the feature. */
    {"screenshot", ""},
};

/* The mouse ships with two slots bound: square (the attack button, on the
 * left button) and r1 (the call/grab button, on the right button). Every
 * other slot is "", which on this device means unbound and is not an error:
 * a mouse has no key for l2 to fall back to. The two names are spelled by
 * calling rt_mouse_input_name() so this table cannot drift from
 * host/mouse_names.h. */
constexpr BindDef kMouseBinds[RT_MB_COUNT] = {
    {"up", ""}, {"down", ""}, {"left", ""}, {"right", ""},
    {"cross", ""}, {"circle", ""},
    {"square", rt_mouse_input_name(RT_MOUSE_LEFT)},
    {"triangle", ""},
    {"l1", ""}, {"r1", rt_mouse_input_name(RT_MOUSE_RIGHT)},
    {"l2", ""}, {"r2", ""}, {"l3", ""}, {"r3", ""},
    {"start", ""}, {"select", ""},
    /* Same reason as the gamepad row above: unbound by default. */
    {"screenshot", ""},
};

/* One binding table plus the two facts every rule about it needs: how many
 * slots it has, and which of them is the menu key (-1 for the mouse, which
 * has none). Indexed by RtBindDevice, so the loaders, the validators and
 * the public accessors all walk the same list and a fifth device would be
 * one row here. */
struct BindTable {
    const BindDef* defs;
    int count;
    int menu_slot;
    /* The screenshot hotkey's slot. Every device has one, unlike the menu
     * key, because the mouse can carry it: it is consumed in the event pump
     * and does not compete with the pointer the menu itself draws. */
    int screenshot_slot;
    const char* json_key;  /* the object key under "input" */
    const char* section;   /* dotted key of the section, for log lines */
    /* Whether this device's names are ever chord-parsed. Gamepad only: a
     * keyboard name is never split ("Keypad +" is a scancode name) and a
     * mouse slot has no menu key for a chord to stand in for. */
    bool chords;
};

constexpr BindTable kBindTables[RT_BIND_DEVICE_COUNT] = {
    {kKeyboardBinds, RT_KB_COUNT, RT_KB_MENU, RT_KB_SCREENSHOT, "keyboard", "input.keyboard", false},
    {kGamepadBinds, RT_GP_COUNT, RT_GP_MENU, RT_GP_SCREENSHOT, "gamepad", "input.gamepad", true},
    /* Player 2 reads the same default table with a shorter count: the first
     * sixteen entries are the sixteen DS2 buttons and the two past them are
     * the host hotkeys, which this device does not have (host/settings.h
     * RT_GP2_COUNT). Chords stay parsed so rule 3 rejects one anywhere here;
     * with no menu slot there is nowhere a chord is legal. */
    {kGamepadBinds, RT_GP2_COUNT, -1, -1, "gamepad2", "input.gamepad2", true},
    {kMouseBinds, RT_MB_COUNT, -1, RT_MB_SCREENSHOT, "mouse", "input.mouse", false},
};

/* True for the slots the host consumes in the event pump: the menu key
 * (ui/ui_events.cpp) and the screenshot key (host/screenshot.cpp). Neither
 * ever reaches the virtual pad, so neither takes part in the "one host input
 * cannot press two DS2 buttons" rule, and a name shared with an ordinary slot
 * is a button the game can never see. */
bool is_hotkey_slot(const BindTable& t, int slot) {
    return slot >= 0 && (slot == t.menu_slot || slot == t.screenshot_slot);
}

bool valid_device(RtBindDevice d) {
    return d >= 0 && d < RT_BIND_DEVICE_COUNT;
}

/* The slot array in one settings struct for one device. */
std::string* bind_values(RtSettings* s, RtBindDevice d) {
    switch (d) {
    case RT_BIND_KEYBOARD: return s->input.keyboard;
    case RT_BIND_GAMEPAD:  return s->input.gamepad;
    case RT_BIND_GAMEPAD2: return s->input.gamepad2;
    case RT_BIND_MOUSE:    return s->input.mouse;
    default:               return nullptr;
    }
}

const std::string* bind_values(const RtSettings& s, RtBindDevice d) {
    return bind_values(const_cast<RtSettings*>(&s), d);
}

/* ---- environment twins ---------------------------------------------------
 *
 * The environment wins over the file for every one of these so existing
 * scripts and CI invocations keep their exact behavior after this milestone
 * lands. Each consumer reads its own env var at its own read site
 * (pace_period_seconds, rt_prof_init, resolve_create_options, sdl_open,
 * main's verbosity setup); this table only powers rt_settings_overridden()
 * and the startup "overridden by" log.
 *
 * "Set" means present in the environment, even as an empty string: that is
 * what every consumer above tests (getenv() != NULL), so the UI's
 * "overridden by" state and the consumer's actual behavior agree.
 *
 * ICORECOMP_LOG is the one exception, and `nonempty` records it. log.cpp
 * tests `env && *env`, because an empty log path names no file and so
 * overrides nothing; reporting debug.log_file as overridden for an empty
 * value would tell the user the setting was ignored when it is exactly what
 * took effect.
 */
struct EnvTwin {
    const char* dotted_key;
    const char* env_var;
    bool nonempty;
};

constexpr EnvTwin kEnvTwins[] = {
    {"debug.fps_limit_hz", "ICORECOMP_FPS_LIMIT", false},
    {"debug.log_level", "ICORECOMP_LOG_LEVEL", false},
    {"debug.profile_fields", "ICORECOMP_PROFILE", false},
    {"debug.log_file", "ICORECOMP_LOG", true},
    {"audio.mute", "ICORECOMP_NO_AUDIO", false},
};

/* Two environment variables that were twins and are not any more, because
 * the settings key each stood over is gone: ICORECOMP_GS_PRESENT (the
 * swapchain present mode, read in gs/gs_select.cpp) and ICORECOMP_VERBOSE
 * (the log channel spec, read in log.cpp's rt_log_init). Both still do
 * exactly what they did, at those same call sites, so every script and CI
 * invocation keeps behaving as it always has; there is simply no file key
 * left for them to win over, so they are not in the table above and
 * rt_settings_overridden() answers false for them. docs/SETTINGS.md section
 * 3 lists them as environment-only.
 */

/* The value this twin overrides its setting with, or null when it is not
 * set in the sense its consumer means (see `nonempty` above). The startup
 * log and rt_settings_overridden() both go through this so the two can
 * never disagree. */
const char* env_twin_value(const EnvTwin& t) {
    const char* v = std::getenv(t.env_var);
    if (!v) return nullptr;
    if (t.nonempty && !*v) return nullptr;
    return v;
}

/* ---- enum <-> JSON string tables ------------------------------------------ */

struct EnumEntry {
    const char* name;
    int value;
};

constexpr EnumEntry kDisplayModeNames[] = {
    {"windowed", (int)RtDisplayMode::Windowed},
    {"fullscreen_desktop", (int)RtDisplayMode::FullscreenDesktop},
    {"fullscreen_exclusive", (int)RtDisplayMode::FullscreenExclusive},
};
constexpr EnumEntry kFitNames[] = {
    {"letterbox", (int)RtFit::Letterbox},
    {"integer", (int)RtFit::IntegerScale},
    {"stretch", (int)RtFit::Stretch},
};
constexpr EnumEntry kRasterNames[] = {
    {"crt", (int)RtRaster::Crt},
    {"window", (int)RtRaster::Window},
};
/* "16_9" rather than "16:9": a colon in a JSON string is legal but reads as
 * structure, and the same token is the option value in ui/menu.rml. */
constexpr EnumEntry kWidescreenNames[] = {
    {"off", (int)RtWidescreen::Off},
    {"window", (int)RtWidescreen::Window},
    {"16_9", (int)RtWidescreen::SixteenNine},
};
/* display.backend had a name table here until 2026-09-05. The key is
 * retired and the struct no longer carries a field for it, so the only
 * spelling of a backend name left in the tree is gs/gs_select.cpp's, which
 * parses ICORECOMP_GS_BACKEND. */
constexpr EnumEntry kFilterNames[] = {
    {"linear", (int)RtFilter::Linear},
    {"nearest", (int)RtFilter::Nearest},
};
/* Highest first, the way the level ordering reads: a line shows when its
 * own level is at or above the one selected here. */
constexpr EnumEntry kLogLevelNames[] = {
    {"error", (int)RT_LOG_ERROR},
    {"warn", (int)RT_LOG_WARN},
    {"info", (int)RT_LOG_INFO},
    {"debug", (int)RT_LOG_DEBUG},
};

/* 2x is deliberately not offered: SuperSampling::X2 only doubles the
 * vertical sampling rate (parallel-gs gs_interface.cpp), and the renderer
 * drops a high-resolution scanout request when either axis has no extra
 * samples (gs_renderer.cpp), so 2x can never scale the picture. */
constexpr int kRenderScales[] = {1, 4, 8, 16};

template <size_t N>
const char* enum_name(const EnumEntry (&table)[N], int value) {
    for (size_t k = 0; k < N; ++k) {
        if (table[k].value == value) return table[k].name;
    }
    return "?";
}

/* ---- global state ----------------------------------------------------- */

RtSettings g_current;             /* rt_settings_mutable()'s target */
RtSettings g_committed;           /* last value rt_settings_commit() accepted */
RtJson g_dom = RtJson::make_object(); /* retained DOM: carries unknown keys */
std::string g_path;               /* "" until a load or save has picked one */
/* Never zero, so a consumer's zero-initialized cache always differs from it
 * on the first check and rebuilds even if rt_settings_init() never ran. */
unsigned g_generation = 1;
std::string g_last_reject;        /* "" when the last commit rejected nothing */
bool g_save_allowed = true;
std::string g_save_blocked_reason;

/* Save debounce. One mechanism for the whole runtime: every producer of
 * settings changes (the menu's control callbacks, window.cpp's remembered
 * window size) calls rt_settings_request_save() and the write lands from
 * rt_settings_flush_save_if_due() at a later field boundary. A slider drag
 * or a resize drag would otherwise be one atomic file write per field. */
using SaveClock = std::chrono::steady_clock;
bool g_save_dirty = false;
SaveClock::time_point g_save_requested_at;
constexpr auto kSaveDebounce = std::chrono::seconds(1);

void apply_compiled_defaults(RtSettings* s) {
    *s = RtSettings{};
    for (int d = 0; d < RT_BIND_DEVICE_COUNT; ++d) {
        const BindTable& t = kBindTables[d];
        std::string* v = bind_values(s, (RtBindDevice)d);
        for (int i = 0; i < t.count; ++i) v[i] = t.defs[i].def;
    }
}

bool is_one_of(const std::string& k, std::initializer_list<const char*> set) {
    for (const char* s : set) {
        if (k == s) return true;
    }
    return false;
}

/* ---- load-time per-value validation ------------------------------------
 *
 * Every one of these keeps *out (the compiled-in default already sitting
 * there from apply_compiled_defaults) when `v` is missing, the wrong JSON
 * type, or out of range, logging the dotted key, the bad value, and the
 * allowed range/set in the log line. A missing key is normal (an older
 * file predating a field) and stays silent; only a present-but-bad value
 * logs.
 */

void load_bool(const RtJson* v, const char* dotted, bool* out) {
    if (!v) return;
    if (v->type != RtJson::Type::Bool) {
        rt_log_warn("settings", "settings: %s is not a boolean (kept default %s)", dotted, *out ? "true" : "false");
        return;
    }
    *out = v->boolean;
}

void load_string(const RtJson* v, const char* dotted, std::string* out) {
    if (!v) return;
    if (v->type != RtJson::Type::String) {
        rt_log_warn("settings", "settings: %s is not a string (kept default \"%s\")", dotted, out->c_str());
        return;
    }
    *out = v->str;
}

void load_int_range(const RtJson* v, const char* dotted, int lo, int hi, int* out) {
    if (!v) return;
    if (v->type != RtJson::Type::Number) {
        rt_log_warn("settings", "settings: %s is not a number (kept default %d)", dotted, *out);
        return;
    }
    double d = v->number;
    if (d != std::floor(d) || d < lo || d > hi) {
        rt_log_warn("settings", "settings: %s = %.6g is out of range [%d, %d] (kept default %d)", dotted, d, lo, hi, *out);
        return;
    }
    *out = (int)d;
}

void load_double_range(const RtJson* v, const char* dotted, double lo, double hi, bool lo_exclusive, double* out) {
    if (!v) return;
    if (v->type != RtJson::Type::Number) {
        rt_log_warn("settings", "settings: %s is not a number (kept default %.6g)", dotted, *out);
        return;
    }
    double d = v->number;
    bool ok = (lo_exclusive ? d > lo : d >= lo) && d <= hi;
    if (!ok) {
        rt_log_warn("settings", "settings: %s = %.6g is out of range %c%.6g, %.6g] (kept default %.6g)",
            dotted, d, lo_exclusive ? '(' : '[', lo, hi, *out);
        return;
    }
    *out = d;
}

/* The bounds are doubles, and revert_float's are the same doubles, so the
 * load path and the commit path share one predicate. Narrowing them to
 * float would not: 0.95 as a double is above (double)0.95f, so the
 * documented maximum would parse, widen, and fail the comparison the loader
 * makes in double, while the commit path (which compares the stored float)
 * accepted the very same value. */
void load_float_range(const RtJson* v, const char* dotted, double lo, double hi, bool lo_exclusive, float* out) {
    double d = *out;
    load_double_range(v, dotted, lo, hi, lo_exclusive, &d);
    *out = (float)d;
}

/* A rate in Hz: 0 has the key's own "off" meaning, otherwise [1, 1000].
 * debug.fps_limit_hz (0 disables the guest field pacer) is the only key
 * that uses it now. */
void load_hz(const RtJson* v, const char* dotted, double* out) {
    if (!v) return;
    if (v->type != RtJson::Type::Number) {
        rt_log_warn("settings", "settings: %s is not a number (kept default %.6g)", dotted, *out);
        return;
    }
    double d = v->number;
    if (d == 0.0 || (d >= 1.0 && d <= 1000.0)) {
        *out = d;
        return;
    }
    rt_log_warn("settings", "settings: %s = %.6g is out of range (must be 0 or [1, 1000]) (kept default %.6g)", dotted, d, *out);
}

/* debug.fps_limit_hz: load_hz plus RT_FPS_LIMIT_MODE_RATE, which is the
 * default and means "the rate of the video mode the game programmed". A
 * negative number that is not exactly that sentinel is still out of
 * range. */
void load_fps_limit(const RtJson* v, const char* dotted, double* out) {
    if (!v) return;
    if (v->type == RtJson::Type::Number && v->number == RT_FPS_LIMIT_MODE_RATE) {
        *out = RT_FPS_LIMIT_MODE_RATE;
        return;
    }
    load_hz(v, dotted, out);
}

void load_int_set(const RtJson* v, const char* dotted, const int* set, size_t n, int* out) {
    if (!v) return;
    if (v->type != RtJson::Type::Number) {
        rt_log_warn("settings", "settings: %s is not a number (kept default %d)", dotted, *out);
        return;
    }
    double d = v->number;
    if (d == std::floor(d)) {
        int iv = (int)d;
        for (size_t k = 0; k < n; ++k) {
            if (set[k] == iv) {
                *out = iv;
                return;
            }
        }
    }
    std::string allowed;
    for (size_t k = 0; k < n; ++k) {
        if (k) allowed += ", ";
        allowed += std::to_string(set[k]);
    }
    rt_log_warn("settings", "settings: %s = %.6g is not one of {%s} (kept default %d)", dotted, d, allowed.c_str(), *out);
}

template <size_t N>
void load_enum(const RtJson* v, const char* dotted, const EnumEntry (&table)[N], int* out) {
    if (!v) return;
    if (v->type != RtJson::Type::String) {
        rt_log_warn("settings", "settings: %s is not a string (kept default \"%s\")", dotted, enum_name(table, *out));
        return;
    }
    for (size_t k = 0; k < N; ++k) {
        if (v->str == table[k].name) {
            *out = table[k].value;
            return;
        }
    }
    std::string allowed;
    for (size_t k = 0; k < N; ++k) {
        if (k) allowed += "/";
        allowed += table[k].name;
    }
    rt_log_warn("settings", "settings: %s = \"%s\" is not one of %s (kept default \"%s\")",
        dotted, v->str.c_str(), allowed.c_str(), enum_name(table, *out));
}

/* ---- unknown-key logging ------------------------------------------------ */

template <typename Pred>
void log_unknown_keys(const RtJson& obj, const std::string& parent, Pred is_known) {
    if (obj.type != RtJson::Type::Object) return;
    for (const auto& kv : obj.obj) {
        if (is_known(kv.first)) continue;
        std::string dotted = parent.empty() ? kv.first : parent + "." + kv.first;
        rt_log_warn("settings", "settings: unknown key \"%s\" kept as-is", dotted.c_str());
    }
}

/* A key this build no longer reads, present in the file a user already
 * has. One info line names it and says what stands in its place; the value
 * itself is left exactly as it was, because the retained DOM carries
 * unknown and retired keys across a save (write_struct_into_dom sets known
 * keys only). Info, not warn: nothing was refused and nothing was
 * overridden. The key stays in the section's known-key list so this line is
 * the only one it produces.
 *
 * Settings handling is never fatal, and a stale key is the ordinary state
 * of a settings.json written by an older build. */
void load_retired(const RtJson* v, const char* dotted, const char* instead) {
    if (!v) return;
    rt_log_info("settings", "settings: %s is no longer read; %s", dotted, instead);
}

/* ---- DOM -> struct ------------------------------------------------------- */

void map_bind_section(const RtJson* sec, const char* dotted_parent, const BindDef* defs, int count, std::string* out) {
    if (!sec) return;
    if (sec->type != RtJson::Type::Object) {
        rt_log_warn("settings", "settings: %s is not an object (kept defaults)", dotted_parent);
        return;
    }
    log_unknown_keys(*sec, dotted_parent, [&](const std::string& k) {
        for (int i = 0; i < count; ++i) {
            if (k == defs[i].json_key) return true;
        }
        return false;
    });
    for (int i = 0; i < count; ++i) {
        std::string dotted = std::string(dotted_parent) + "." + defs[i].json_key;
        load_string(sec->find(defs[i].json_key), dotted.c_str(), &out[i]);
    }
}

void map_from_dom(const RtJson& dom, RtSettings* out) {
    log_unknown_keys(dom, "", [](const std::string& k) {
        /* Every section write_struct_into_dom writes, "system" included: a
         * section missing from this list is reported as an unknown key on
         * the next load of a file this build itself saved. */
        return is_one_of(k, {"version", "display", "audio", "input", "gameplay", "system",
            "achievements", "debug", "launcher"});
    });

    if (const RtJson* d = dom.find("display")) {
        if (d->type != RtJson::Type::Object) {
            rt_log_warn("settings", "settings: \"display\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*d, "display", [](const std::string& k) {
                return is_one_of(k, {"mode", "window_width", "window_height", "remember_window_size",
                    "present", "present_rate", "fit", "filter", "raster", "deinterlace",
                    "widescreen", "backend", "render_scale", "show_fps", "screenshot_dir"});
            });
            load_enum(d->find("mode"), "display.mode", kDisplayModeNames, (int*)&out->display.mode);
            load_int_range(d->find("window_width"), "display.window_width", 320, 16384, &out->display.window_width);
            load_int_range(d->find("window_height"), "display.window_height", 320, 16384, &out->display.window_height);
            load_bool(d->find("remember_window_size"), "display.remember_window_size", &out->display.remember_window_size);
            load_retired(d->find("present"), "display.present",
                "the present mode is mailbox, and ICORECOMP_GS_PRESENT still selects another"
                " one for this run");
            load_retired(d->find("present_rate"), "display.present_rate",
                "the window is refreshed once per field");
            load_enum(d->find("fit"), "display.fit", kFitNames, (int*)&out->display.fit);
            load_enum(d->find("raster"), "display.raster", kRasterNames, (int*)&out->display.raster);
            load_retired(d->find("deinterlace"), "display.deinterlace",
                "the two fields of an interlaced scanout are always bobbed, which is what shows"
                " the attract movie as the disc holds it");
            load_enum(d->find("widescreen"), "display.widescreen", kWidescreenNames, (int*)&out->display.widescreen);
            load_retired(d->find("backend"), "display.backend",
                "the renderer is paraLLEl-GS (the native renderers were withdrawn from settings on"
                " 2026-09-05; ICORECOMP_GS_BACKEND still selects one for a replay or a CI run)");
            load_enum(d->find("filter"), "display.filter", kFilterNames, (int*)&out->display.filter);
            load_int_set(d->find("render_scale"), "display.render_scale", kRenderScales, std::size(kRenderScales), &out->display.render_scale);
            if (d->find("hires_scanout")) {
                /* Retired key. It is not in the known-key list above, so it is
                 * also reported by log_unknown_keys and kept in the file across
                 * a save; this line says why it no longer does anything. */
                rt_log_warn("settings", "settings: display.hires_scanout is no longer a setting;"
                    " display.render_scale 4 and up now requests high-resolution scanout");
            }
            load_bool(d->find("show_fps"), "display.show_fps", &out->display.show_fps);
            /* Any path is accepted as written, including one that does not
             * exist yet or is not writable: host/screenshot.cpp creates it on
             * the first capture and, when it cannot, logs once and skips.
             * There is no allowed set to validate against here, so there is
             * nothing for the loader or commit_validate to revert. */
            load_string(d->find("screenshot_dir"), "display.screenshot_dir", &out->display.screenshot_dir);
        }
    }

    if (const RtJson* a = dom.find("audio")) {
        if (a->type != RtJson::Type::Object) {
            rt_log_warn("settings", "settings: \"audio\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*a, "audio", [](const std::string& k) {
                return is_one_of(k, {"master_volume", "mute", "music_volume", "effects_volume",
                                     "movie_volume", "chime_volume"});
            });
            load_int_range(a->find("master_volume"), "audio.master_volume", 0, 100, &out->audio.master_volume);
            load_bool(a->find("mute"), "audio.mute", &out->audio.mute);
            load_int_range(a->find("music_volume"), "audio.music_volume", 0, 100, &out->audio.music_volume);
            load_int_range(a->find("effects_volume"), "audio.effects_volume", 0, 100, &out->audio.effects_volume);
            load_int_range(a->find("movie_volume"), "audio.movie_volume", 0, 100, &out->audio.movie_volume);
            load_int_range(a->find("chime_volume"), "audio.chime_volume", 0, 100, &out->audio.chime_volume);
        }
    }

    if (const RtJson* i = dom.find("input")) {
        if (i->type != RtJson::Type::Object) {
            rt_log_warn("settings", "settings: \"input\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*i, "input", [](const std::string& k) {
                return is_one_of(k, {"keyboard", "gamepad", "gamepad2", "mouse",
                    "left_deadzone", "right_deadzone",
                    "mouse_look", "mouse_look_sensitivity", "mouse_look_invert_y"});
            });
            for (int d = 0; d < RT_BIND_DEVICE_COUNT; ++d) {
                const BindTable& t = kBindTables[d];
                map_bind_section(i->find(t.json_key), t.section, t.defs, t.count,
                    bind_values(out, (RtBindDevice)d));
            }
            load_float_range(i->find("left_deadzone"), "input.left_deadzone", 0.0, 0.95, false, &out->input.left_deadzone);
            load_float_range(i->find("right_deadzone"), "input.right_deadzone", 0.0, 0.95, false, &out->input.right_deadzone);
            load_bool(i->find("mouse_look"), "input.mouse_look", &out->input.mouse_look);
            load_float_range(i->find("mouse_look_sensitivity"), "input.mouse_look_sensitivity",
                0.05, 20.0, false, &out->input.mouse_look_sensitivity);
            load_bool(i->find("mouse_look_invert_y"), "input.mouse_look_invert_y", &out->input.mouse_look_invert_y);
            /* Retired keys. Neither is in the known-key list above, so both are
             * also reported by log_unknown_keys and kept in the file across a
             * save; these lines say what happens instead. */
            if (i->find("trigger_threshold")) {
                rt_log_warn("settings", "settings: input.trigger_threshold is no longer a setting;"
                    " an axis bound to a button is pressed past a compiled-in raw value of 8192 of 32767");
            }
            if (i->find("rumble")) {
                rt_log_warn("settings", "settings: input.rumble is no longer a setting;"
                    " the host pad motors follow the game's actuator requests");
            }
        }
    }

    if (const RtJson* g = dom.find("gameplay")) {
        if (g->type != RtJson::Type::Object) {
            rt_log_warn("settings", "settings: \"gameplay\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*g, "gameplay", [](const std::string& k) {
                return is_one_of(k, {"run_any_direction"});
            });
            load_bool(g->find("run_any_direction"), "gameplay.run_any_direction", &out->gameplay.run_any_direction);
        }
    }

    if (const RtJson* sys = dom.find("system")) {
        if (sys->type != RtJson::Type::Object) {
            rt_log_warn("settings", "settings: \"system\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*sys, "system", [](const std::string& k) {
                return is_one_of(k, {"language"});
            });
            load_retired(sys->find("language"), "system.language",
                "the game's own language screen is where this disc's language is chosen");
        }
    }

    if (const RtJson* ac = dom.find("achievements")) {
        if (ac->type != RtJson::Type::Object) {
            rt_log_warn("settings", "settings: \"achievements\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*ac, "achievements", [](const std::string& k) {
                return is_one_of(k, {"enabled", "toast", "sound", "sound_volume",
                                     "log_progress_bits"});
            });
            load_bool(ac->find("enabled"), "achievements.enabled", &out->achievements.enabled);
            load_bool(ac->find("toast"), "achievements.toast", &out->achievements.toast);
            load_bool(ac->find("sound"), "achievements.sound", &out->achievements.sound);
            load_retired(ac->find("sound_volume"), "achievements.sound_volume",
                "the chime's volume is audio.chime_volume, with the other host output gains");
            load_retired(ac->find("log_progress_bits"), "achievements.log_progress_bits",
                "the progress-bit diagnostic is always on, at info level");
        }
    }

    if (const RtJson* dbg = dom.find("debug")) {
        if (dbg->type != RtJson::Type::Object) {
            rt_log_warn("settings", "settings: \"debug\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*dbg, "debug", [](const std::string& k) {
                return is_one_of(k, {"verbose", "log_level", "console", "log_file",
                    "profile_fields", "fps_limit_hz"});
            });
            load_retired(dbg->find("verbose"), "debug.verbose",
                "ICORECOMP_VERBOSE is where log channels are named");
            load_enum(dbg->find("log_level"), "debug.log_level", kLogLevelNames,
                (int*)&out->debug.log_level);
            load_bool(dbg->find("console"), "debug.console", &out->debug.console);
            load_bool(dbg->find("log_file"), "debug.log_file", &out->debug.log_file);
            load_int_range(dbg->find("profile_fields"), "debug.profile_fields", 0, 100000, &out->debug.profile_fields);
            load_fps_limit(dbg->find("fps_limit_hz"), "debug.fps_limit_hz", &out->debug.fps_limit_hz);
            if (dbg->find("menu_hit_editor")) {
                /* Retired key. It is not in the known-key list above, so it is
                 * also reported by log_unknown_keys and kept in the file across
                 * a save; this line says why it no longer does anything. */
                rt_log_warn("settings", "settings: debug.menu_hit_editor is no longer a setting;"
                    " the pointer on the game's menus reads the game's own scene objects,"
                    " so there are no hit boxes to author");
            }
        }
    }

    if (const RtJson* lc = dom.find("launcher")) {
        if (lc->type != RtJson::Type::Object) {
            rt_log_warn("settings", "settings: \"launcher\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*lc, "launcher", [](const std::string& k) {
                return is_one_of(k, {"show_at_startup", "disc_path"});
            });
            load_bool(lc->find("show_at_startup"), "launcher.show_at_startup", &out->launcher.show_at_startup);
            load_string(lc->find("disc_path"), "launcher.disc_path", &out->launcher.disc_path);
        }
    }
}

/* ---- struct -> DOM -------------------------------------------------------
 *
 * Sets known keys over the retained DOM in place so unknown keys (kept from
 * a load, or hand-added by a user between runs) survive a save untouched;
 * see RtJson::set's "keeps position or appends" contract in json.h.
 */

RtJson& get_or_make_object(RtJson* parent, const char* key) {
    RtJson* existing = parent->find(key);
    if (existing) {
        if (existing->type == RtJson::Type::Object) return *existing;
        rt_log_warn("settings", "settings: \"%s\" was not an object; replacing it with one on save", key);
    }
    return parent->set(key, RtJson::make_object());
}

void write_bind_section(RtJson* parent, const char* key, const BindDef* defs, int count, const std::string* values) {
    RtJson& sec = get_or_make_object(parent, key);
    for (int i = 0; i < count; ++i) {
        sec.set(defs[i].json_key, RtJson::make_string(values[i]));
    }
}

void write_struct_into_dom(const RtSettings& s, RtJson* dom) {
    dom->set("version", RtJson::make_number(1));

    RtJson& d = get_or_make_object(dom, "display");
    d.set("mode", RtJson::make_string(enum_name(kDisplayModeNames, (int)s.display.mode)));
    d.set("window_width", RtJson::make_number(s.display.window_width));
    d.set("window_height", RtJson::make_number(s.display.window_height));
    d.set("remember_window_size", RtJson::make_bool(s.display.remember_window_size));
    d.set("fit", RtJson::make_string(enum_name(kFitNames, (int)s.display.fit)));
    d.set("raster", RtJson::make_string(enum_name(kRasterNames, (int)s.display.raster)));
    d.set("widescreen", RtJson::make_string(enum_name(kWidescreenNames, (int)s.display.widescreen)));
    d.set("filter", RtJson::make_string(enum_name(kFilterNames, (int)s.display.filter)));
    d.set("render_scale", RtJson::make_number(s.display.render_scale));
    d.set("show_fps", RtJson::make_bool(s.display.show_fps));
    d.set("screenshot_dir", RtJson::make_string(s.display.screenshot_dir));

    RtJson& a = get_or_make_object(dom, "audio");
    a.set("master_volume", RtJson::make_number(s.audio.master_volume));
    a.set("mute", RtJson::make_bool(s.audio.mute));
    a.set("music_volume", RtJson::make_number(s.audio.music_volume));
    a.set("effects_volume", RtJson::make_number(s.audio.effects_volume));
    a.set("movie_volume", RtJson::make_number(s.audio.movie_volume));
    a.set("chime_volume", RtJson::make_number(s.audio.chime_volume));

    RtJson& in = get_or_make_object(dom, "input");
    for (int d = 0; d < RT_BIND_DEVICE_COUNT; ++d) {
        const BindTable& t = kBindTables[d];
        write_bind_section(&in, t.json_key, t.defs, t.count, bind_values(s, (RtBindDevice)d));
    }
    in.set("left_deadzone", RtJson::make_number(s.input.left_deadzone));
    in.set("right_deadzone", RtJson::make_number(s.input.right_deadzone));
    in.set("mouse_look", RtJson::make_bool(s.input.mouse_look));
    in.set("mouse_look_sensitivity", RtJson::make_number(s.input.mouse_look_sensitivity));
    in.set("mouse_look_invert_y", RtJson::make_bool(s.input.mouse_look_invert_y));

    RtJson& g = get_or_make_object(dom, "gameplay");
    g.set("run_any_direction", RtJson::make_bool(s.gameplay.run_any_direction));

    /* No "system" section is written: system.language was its only key and
     * this build does not read it. A file that already has one keeps it
     * untouched, like every other retired key. */

    RtJson& ac = get_or_make_object(dom, "achievements");
    ac.set("enabled", RtJson::make_bool(s.achievements.enabled));
    ac.set("toast", RtJson::make_bool(s.achievements.toast));
    ac.set("sound", RtJson::make_bool(s.achievements.sound));

    RtJson& dbg = get_or_make_object(dom, "debug");
    dbg.set("log_level", RtJson::make_string(enum_name(kLogLevelNames, (int)s.debug.log_level)));
    dbg.set("console", RtJson::make_bool(s.debug.console));
    dbg.set("log_file", RtJson::make_bool(s.debug.log_file));
    dbg.set("profile_fields", RtJson::make_number(s.debug.profile_fields));
    dbg.set("fps_limit_hz", RtJson::make_number(s.debug.fps_limit_hz));

    RtJson& lc = get_or_make_object(dom, "launcher");
    lc.set("show_at_startup", RtJson::make_bool(s.launcher.show_at_startup));
    lc.set("disc_path", RtJson::make_string(s.launcher.disc_path));
}

/* ---- file I/O ------------------------------------------------------------ */

/* Best-effort copy of the raw text of a broken file to "<path>.bad", so a
 * hand-edit mistake is never silently discarded. Overwrites any previous
 * .bad from an earlier broken attempt. */
void write_bad_copy(const std::string& path, const std::string& text) {
    std::string bad = path + ".bad";
    std::FILE* f = rt_fopen_utf8(bad.c_str(), "wb");
    if (!f) {
        rt_log_warn("settings", "settings: could not write %s: %s", bad.c_str(), std::strerror(errno));
        return;
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

/* Saving is off for the rest of a run whose settings file did not parse.
 * g_path still points at that file and the run is on defaults, so the next
 * save would replace the file the user still has to fix with a defaults
 * document; the .bad copy on its own does not keep the "never overwritten
 * again" promise, this does. Every path that writes a .bad copy calls
 * this. */
void block_saving_for_broken_file(const std::string& path) {
    g_save_allowed = false;
    g_save_blocked_reason = path + " failed to parse and was copied to " + path +
        ".bad; fix or delete it, then restart";
}

/* Opens `path` and reads its entire contents into `*out`. Returns false
 * (leaving `*out` empty) when the file cannot be opened; a file that opens
 * but is empty returns true with `*out` cleared. */
bool read_whole_file(const std::string& path, std::string* out) {
    std::FILE* f = rt_fopen_utf8(path.c_str(), "rb");
    if (!f) return false;
    out->clear();
    if (std::fseek(f, 0, SEEK_END) == 0) {
        long sz = std::ftell(f);
        if (sz > 0) {
            out->resize((size_t)sz);
            std::fseek(f, 0, SEEK_SET);
            size_t n = std::fread(out->data(), 1, (size_t)sz, f);
            out->resize(n);
        }
    }
    std::fclose(f);
    return true;
}

/* Reads and applies one settings file at `path`. On any failure this logs
 * why and leaves *out (already at compiled-in defaults) alone; the caller
 * has already set g_path before calling this. */
void load_file(const std::string& path) {
    std::string text;
    if (!read_whole_file(path, &text)) {
        rt_log_warn("settings", "settings: %s not found yet; running on defaults, will create it on first save", path.c_str());
        return;
    }

    RtJson parsed;
    std::string err;
    if (!rt_json_parse(text, &parsed, &err)) {
        write_bad_copy(path, text);
        rt_log_warn("settings", "settings: %s:%s; running on defaults, file copied to %s.bad", path.c_str(), err.c_str(), path.c_str());
        block_saving_for_broken_file(path);
        return;
    }
    if (parsed.type != RtJson::Type::Object) {
        write_bad_copy(path, text);
        rt_log_warn("settings", "settings: %s: top level is not an object; running on defaults, file copied to %s.bad", path.c_str(), path.c_str());
        block_saving_for_broken_file(path);
        return;
    }
    const RtJson* ver = parsed.find("version");
    if (!ver || ver->type != RtJson::Type::Number) {
        write_bad_copy(path, text);
        rt_log_warn("settings", "settings: %s: missing or non-numeric \"version\"; running on defaults, file copied to %s.bad", path.c_str(), path.c_str());
        block_saving_for_broken_file(path);
        return;
    }
    if (ver->number > 1.0) {
        g_save_allowed = false;
        g_save_blocked_reason = "the loaded file has version " + std::to_string((long long)ver->number) + ", written by a newer build";
        rt_log_warn("settings", "settings: %s was written by a newer build (version %.0f); running on defaults, the file is left untouched",
            path.c_str(), ver->number);
        return;
    }
    if (ver->number != 1.0) {
        write_bad_copy(path, text);
        rt_log_warn("settings", "settings: %s: unsupported \"version\" %.6g (expected 1); running on defaults, file copied to %s.bad",
            path.c_str(), ver->number, path.c_str());
        block_saving_for_broken_file(path);
        return;
    }

    g_dom = std::move(parsed);
    map_from_dom(g_dom, &g_current);
    rt_log_info("settings", "settings: loaded from %s", path.c_str());
}

/* ---- source resolution ---------------------------------------------------- */

enum class SourceKind { EnvPath, EnvDisabled, BaseDir, UserConfig, None };

struct ResolvedSource {
    SourceKind kind = SourceKind::None;
    std::string path;         /* EnvPath / BaseDir / UserConfig */
    std::string raw_env;      /* EnvDisabled: the literal "-" or "0" */
    std::string shadow_path;  /* BaseDir with both candidates present */
};

ResolvedSource resolve_source() {
    if (const char* env = std::getenv("ICORECOMP_SETTINGS"); env && *env) {
        if (std::strcmp(env, "-") == 0 || std::strcmp(env, "0") == 0) {
            return {SourceKind::EnvDisabled, "", env, ""};
        }
        return {SourceKind::EnvPath, env, "", ""};
    }

    std::string base_path = std::string(rt_base_dir()) + "/settings.json";
    std::string user_dir = rt_user_config_dir();
    std::string user_path = user_dir.empty() ? std::string() : user_dir + "/settings.json";

    bool base_exists = std::filesystem::exists(base_path);
    bool user_exists = !user_path.empty() && std::filesystem::exists(user_path);

    if (base_exists) {
        return {SourceKind::BaseDir, base_path, "", user_exists ? user_path : std::string()};
    }
    if (user_exists) {
        return {SourceKind::UserConfig, user_path, "", ""};
    }
    return {SourceKind::None, "", "", ""};
}

/* ---- commit-time validation ------------------------------------------------
 *
 * Same ranges as the loader, but reverting to the previously *committed*
 * value rather than the compiled-in default: a user who set render_scale to
 * 4 and then fat-fingers a UI field should not lose their earlier choice on
 * the field they got right.
 */

void revert_int(int* v, int prev, const char* dotted, int lo, int hi) {
    if (*v >= lo && *v <= hi) return;
    rt_log_warn("settings", "settings: %s = %d is out of range [%d, %d]; reverted to %d", dotted, *v, lo, hi, prev);
    *v = prev;
}

/* Bounds in double, and the value promoted to double before comparing, so
 * this is the same predicate load_float_range applies (see its comment). */
void revert_float(float* v, float prev, const char* dotted, double lo, double hi, bool lo_exclusive) {
    const double d = *v;
    bool ok = (lo_exclusive ? d > lo : d >= lo) && d <= hi;
    if (ok) return;
    rt_log_warn("settings", "settings: %s = %.6g is out of range %c%.6g, %.6g]; reverted to %.6g",
        dotted, d, lo_exclusive ? '(' : '[', lo, hi, (double)prev);
    *v = prev;
}

/* ---- binding rules -------------------------------------------------------
 *
 * Two rules, both about one name being claimed twice on one device:
 *
 *   1. A host hotkey (the menu key, input.<device>.menu, consumed by
 *      ui/ui_events.cpp; and the screenshot key, input.<device>.screenshot,
 *      consumed by host/screenshot.cpp) is taken in the event pump and never
 *      reaches the pad, so a hotkey name that is also a pad binding is a pad
 *      button the game can never see. Rejected. The same rule catches the two
 *      hotkeys sharing a name: the menu key is resolved first in the pump, so
 *      the screenshot would never fire.
 *   2. Two ordinary slots holding the same name means one host key or button
 *      presses two DS2 buttons at once. That is a legal thing to want but
 *      almost never a thing anyone meant, and the menu offers no way to say
 *      "yes, both". Rejected.
 *
 * Both rules revert only the slots this commit changed, and skip a pair
 * where neither slot changed since the last commit: that pair was not
 * introduced here, it came in from the settings file, log_bind_duplicates()
 * reported it at load, and re-rejecting it on every unrelated commit would
 * leave a permanent message in the menu for something the user cannot fix
 * from there. Picking a slot to revert in that case would also drop a name
 * the user wrote by hand.
 *
 * A rejection reverts to the previously committed name, never to the
 * compiled default: the user's earlier choice for that slot is theirs and
 * this commit is not a reason to lose it. Empty names are skipped rather
 * than treated as equal to each other: on the keyboard and the gamepad an
 * empty slot is a name that did not resolve, host/input.cpp already
 * replaces it with the compiled default and says so; on the mouse it is the
 * shipped state of most slots. Either way two of them are not a collision
 * the user made.
 *
 * Rule 1 runs once per hotkey slot the device has. The mouse has no menu
 * slot (`menu_slot` -1) so it gets the screenshot pass only, and player 2's
 * pad has neither, so rule 1 does not run for it at all. Rule 2 applies to
 * every device and skips whichever hotkeys it has.
 *
 * Comparison is case-insensitive because that is how SDL_GetScancodeFromName
 * resolves ("f1" and "F1" are the same key), so a case difference in a
 * hand-edited file must not read as two different bindings.
 */

bool bind_name_equal(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    }
    return true;
}

} // namespace

bool rt_settings_split_chord(const std::string& name, std::string* first, std::string* second) {
    const size_t plus_count = (size_t)std::count(name.begin(), name.end(), '+');
    if (plus_count != 1) return false;
    const size_t pos = name.find('+');
    const std::string a = name.substr(0, pos);
    const std::string b = name.substr(pos + 1);
    /* Empty on either side covers both a leading '+' and a trailing one
     * (the axis-direction convention, "lefttrigger+"); either way this is
     * not a chord. */
    if (a.empty() || b.empty()) return false;
    const char la = a.back(), lb = b.back();
    if (la == '+' || la == '-' || lb == '+' || lb == '-') return false;
    *first = a;
    *second = b;
    return true;
}

namespace {

void note_reject(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    /* warn, not info: a reject is a refusal, which is what runtime.h's
     * table grades warn, and it is what every other revert in this file
     * uses. The menu shows it live through rt_settings_last_reject, but a
     * log built into a bug report has to carry it too, and the shipped
     * level is warn. */
    rt_log_warn("settings", "settings: %s", buf);
    if (!g_last_reject.empty()) g_last_reject += "; ";
    g_last_reject += buf;
}

/* One device's slots, described by its BindTable row. */
void validate_binds(std::string* cur, const std::string* prev, const BindTable& t) {
    const BindDef* defs = t.defs;
    const int count = t.count;
    const int menu_slot = t.menu_slot;
    const char* section = t.section;

    /* Rule 1, first: whatever the menu key ends up as, the ordinary-slot pass
     * below then sees the settled value. Which side moved decides which side
     * reverts: binding the menu key onto an existing pad slot is the menu
     * key's fault, binding a pad slot onto the menu key is the pad slot's,
     * and reverting the menu slot in the second case would leave the
     * collision standing. Skipped whole for a device with no menu slot, and
     * also skipped when the menu slot holds a chord: "back+start" over the
     * bound select/start is the expected setup for a pad whose PS button the
     * OS or Steam intercepts, and the guest sees both parts exactly as
     * hardware would until the menu opens and blanks the pad (ui_events.cpp
     * only consumes the chord as a whole, on the edge that completes it). */
    std::string menu_chord_a, menu_chord_b;
    const bool menu_is_chord = t.chords && menu_slot >= 0 &&
        rt_settings_split_chord(cur[menu_slot], &menu_chord_a, &menu_chord_b);
    /* Menu first, then screenshot: run in that order, a name the two hotkeys
     * share is reported by the menu pass, and that is the honest way round.
     * ui/ui_events.cpp resolves the menu key first in the pump and consumes
     * it, so it is the screenshot that would never fire. */
    for (int h = 0; h < 2; ++h) {
        const int hotkey = (h == 0) ? menu_slot : t.screenshot_slot;
        if (hotkey < 0) continue;
        if (hotkey == menu_slot && menu_is_chord) continue;
        for (int i = 0; i < count; ++i) {
            if (i == hotkey) continue;
            if (!bind_name_equal(cur[hotkey], cur[i])) continue;
            const bool hotkey_changed = cur[hotkey] != prev[hotkey];
            const bool other_changed = cur[i] != prev[i];
            if (!hotkey_changed && !other_changed) continue;
            note_reject("%s.%s and %s.%s are both \"%s\"; the host takes %s.%s in the event"
                " pump, so %s.%s would never fire; %s%s%s reverted",
                section, defs[hotkey].json_key, section, defs[i].json_key, cur[hotkey].c_str(),
                section, defs[hotkey].json_key, section, defs[i].json_key,
                hotkey_changed ? defs[hotkey].json_key : "",
                (hotkey_changed && other_changed) ? " and " : "",
                other_changed ? defs[i].json_key : "");
            if (hotkey_changed) cur[hotkey] = prev[hotkey];
            if (other_changed) cur[i] = prev[i];
        }
    }

    /* Rule 2, the same way: only the slots this commit moved revert. Neither
     * hotkey slot takes part; rule 1 above is what covers those. */
    for (int i = 0; i < count; ++i) {
        if (is_hotkey_slot(t, i)) continue;
        for (int j = i + 1; j < count; ++j) {
            if (is_hotkey_slot(t, j)) continue;
            if (!bind_name_equal(cur[i], cur[j])) continue;
            const bool i_changed = cur[i] != prev[i];
            const bool j_changed = cur[j] != prev[j];
            if (!i_changed && !j_changed) continue;
            note_reject("%s.%s and %s.%s are both \"%s\"; one host input cannot drive two"
                " buttons, so %s%s%s reverted",
                section, defs[i].json_key, section, defs[j].json_key, cur[i].c_str(),
                i_changed ? defs[i].json_key : "",
                (i_changed && j_changed) ? " and " : "",
                j_changed ? defs[j].json_key : "");
            if (i_changed) cur[i] = prev[i];
            if (j_changed) cur[j] = prev[j];
        }
    }

    /* Rule 3: a chord is only legal in the menu slot. Anywhere else it would
     * have to hide its two parts from the virtual pad, which changes what
     * the game sees, so it reverts like any other slot this commit moved. */
    if (t.chords) {
        for (int i = 0; i < count; ++i) {
            if (i == menu_slot) continue;
            std::string a, b;
            if (!rt_settings_split_chord(cur[i], &a, &b)) continue;
            if (cur[i] == prev[i]) continue;
            note_reject("%s.%s = \"%s\" is a chord, which only %s.menu accepts; reverted",
                section, defs[i].json_key, cur[i].c_str(), section);
            cur[i] = prev[i];
        }
    }

    /* Rule 4: a chord whose two parts are the same button is not two
     * buttons at all. Only the menu slot can hold a chord, so this only
     * ever looks at it. */
    if (menu_is_chord && bind_name_equal(menu_chord_a, menu_chord_b) &&
        cur[menu_slot] != prev[menu_slot]) {
        note_reject("%s.menu = \"%s\" chords a button with itself; reverted",
            section, cur[menu_slot].c_str());
        cur[menu_slot] = prev[menu_slot];
    }
}

/* Load-time report only, no value change: a duplicate that came in from the
 * settings file is the user's own file and this layer never rewrites user
 * data on load (see the bad-value policy at the top). Saying so once at
 * startup is what keeps the commit-time rule above from having to guess. */
void log_bind_duplicates(const std::string* v, const BindTable& t) {
    for (int i = 0; i < t.count; ++i) {
        for (int j = i + 1; j < t.count; ++j) {
            if (!bind_name_equal(v[i], v[j])) continue;
            if (is_hotkey_slot(t, i) || is_hotkey_slot(t, j)) {
                /* Which of the pair is the hotkey decides the wording: the
                 * host takes it in the event pump, so it is the other slot
                 * that never fires. Two hotkeys sharing a name land here too,
                 * and the menu is the one the pump resolves first. */
                const int hot = is_hotkey_slot(t, i) ? i : j;
                const int lost = (hot == i) ? j : i;
                rt_log_warn("settings", "settings: %s.%s and %s.%s are both \"%s\"; the host takes"
                    " %s.%s in the event pump, so %s.%s will never fire",
                    t.section, t.defs[hot].json_key, t.section, t.defs[lost].json_key, v[i].c_str(),
                    t.section, t.defs[hot].json_key, t.section, t.defs[lost].json_key);
            } else {
                rt_log_warn("settings", "settings: %s.%s and %s.%s are both \"%s\"; that one input will"
                    " press both buttons", t.section, t.defs[i].json_key, t.section, t.defs[j].json_key,
                    v[i].c_str());
            }
        }
    }

    /* A chord is only legal in the menu slot; one that arrived in the file
     * anywhere else is reported once here and left alone, the same way a
     * duplicate is: this is the user's own file, and the commit-time rule
     * above (rule 3) only reverts a slot the running commit itself moved. */
    if (t.chords) {
        for (int i = 0; i < t.count; ++i) {
            if (i == t.menu_slot) continue;
            std::string a, b;
            if (!rt_settings_split_chord(v[i], &a, &b)) continue;
            rt_log_warn("settings", "settings: %s.%s = \"%s\" is a chord, which only %s.menu accepts;"
                " that slot cannot resolve it and host/input.cpp falls back to its default",
                t.section, t.defs[i].json_key, v[i].c_str(), t.section);
        }

        /* Rule 4's load-time twin, the same shape: the commit-time rule only
         * reverts a slot the running commit itself moved, so a chord that
         * pairs a button with itself and arrived in the file would otherwise
         * pass without a word. It resolves, and ui/ui_events.cpp then reads
         * one press of that button as the whole chord, because SDL has
         * already recorded the button as down by the time the event for it is
         * handled. Reported, not rewritten: this is the user's file. */
        std::string menu_a, menu_b;
        if (t.menu_slot >= 0 && rt_settings_split_chord(v[t.menu_slot], &menu_a, &menu_b) &&
            bind_name_equal(menu_a, menu_b)) {
            rt_log_warn("settings", "settings: %s.menu = \"%s\" chords a button with itself, which is one"
                " button, not two; that one press will open and close the menu",
                t.section, v[t.menu_slot].c_str());
        }
    }
}

/* The commit-time twin of load_hz: 0 or [1, 1000], reverted to the value the
 * running commit started from rather than to the compiled-in default, which
 * is the rule every revert_* here follows. */
void revert_hz(double* cur, double prev, const char* dotted) {
    if (*cur == 0.0 || (*cur >= 1.0 && *cur <= 1000.0)) return;
    rt_log_warn("settings", "settings: %s = %.6g is out of range (must be 0 or [1, 1000]);"
        " reverted to %.6g", dotted, *cur, prev);
    *cur = prev;
}

/* The commit-time twin of load_fps_limit. */
void revert_fps_limit(double* cur, double prev, const char* dotted) {
    if (*cur == RT_FPS_LIMIT_MODE_RATE) return;
    revert_hz(cur, prev, dotted);
}

void commit_validate(RtSettings* cur, const RtSettings& prev) {
    revert_int(&cur->display.window_width, prev.display.window_width, "display.window_width", 320, 16384);
    revert_int(&cur->display.window_height, prev.display.window_height, "display.window_height", 320, 16384);
    revert_int(&cur->audio.master_volume, prev.audio.master_volume, "audio.master_volume", 0, 100);
    revert_int(&cur->audio.music_volume, prev.audio.music_volume, "audio.music_volume", 0, 100);
    revert_int(&cur->audio.effects_volume, prev.audio.effects_volume, "audio.effects_volume", 0, 100);
    revert_int(&cur->audio.movie_volume, prev.audio.movie_volume, "audio.movie_volume", 0, 100);
    revert_int(&cur->audio.chime_volume, prev.audio.chime_volume, "audio.chime_volume", 0, 100);
    revert_int(&cur->debug.profile_fields, prev.debug.profile_fields, "debug.profile_fields", 0, 100000);

    revert_float(&cur->input.left_deadzone, prev.input.left_deadzone, "input.left_deadzone", 0.0, 0.95, false);
    revert_float(&cur->input.right_deadzone, prev.input.right_deadzone, "input.right_deadzone", 0.0, 0.95, false);
    revert_float(&cur->input.mouse_look_sensitivity, prev.input.mouse_look_sensitivity,
        "input.mouse_look_sensitivity", 0.05, 20.0, false);

    revert_fps_limit(&cur->debug.fps_limit_hz, prev.debug.fps_limit_hz, "debug.fps_limit_hz");

    /* Four enums are re-checked below, not all of them: display.mode, fit,
     * raster and filter are not. The asymmetry is deliberate. Both entry
     * points already refuse a value outside the table, load_enum for the
     * file and value_of (ui/ui_settings_model.cpp) for the menu, so no enum
     * can reach a commit wrong and none of these four is the only gate on
     * its value. They are a second refusal on the ones whose consumers sit
     * furthest from this file: the log level the sink reads on every
     * thread, the widescreen mode the translator's entry hook reads, the
     * backend selection gs_select.cpp turns into a library, and
     * render_scale, which is an int with an allowed set rather than a name
     * and so has no enum table behind it at either entry point. Adding the
     * rest would be harmless and would prove nothing more than value_of
     * already does. */
    bool ll_ok = false;
    for (const EnumEntry& e : kLogLevelNames) {
        if ((int)cur->debug.log_level == e.value) ll_ok = true;
    }
    if (!ll_ok) {
        rt_log_warn("settings", "settings: debug.log_level = %d is not one of error/warn/info/debug;"
            " reverted to \"%s\"", (int)cur->debug.log_level,
            enum_name(kLogLevelNames, (int)prev.debug.log_level));
        cur->debug.log_level = prev.debug.log_level;
    }

    bool ws_ok = false;
    for (const EnumEntry& e : kWidescreenNames) {
        if ((int)cur->display.widescreen == e.value) ws_ok = true;
    }
    if (!ws_ok) {
        rt_log_warn("settings", "settings: display.widescreen = %d is not one of off/window/16_9;"
            " reverted to \"%s\"", (int)cur->display.widescreen,
            enum_name(kWidescreenNames, (int)prev.display.widescreen));
        cur->display.widescreen = prev.display.widescreen;
    }

    bool rs_ok = false;
    for (int allowed : kRenderScales) {
        if (cur->display.render_scale == allowed) rs_ok = true;
    }
    if (!rs_ok) {
        rt_log_warn("settings", "settings: display.render_scale = %d is not one of {1, 4, 8, 16}; reverted to %d",
            cur->display.render_scale, prev.display.render_scale);
        cur->display.render_scale = prev.display.render_scale;
    }

    for (int d = 0; d < RT_BIND_DEVICE_COUNT; ++d) {
        validate_binds(bind_values(cur, (RtBindDevice)d), bind_values(prev, (RtBindDevice)d),
            kBindTables[d]);
    }

    /* The two cold keys, once the guest is running (settings.h). Before
     * that, a change to one of them is applied by restarting the program,
     * which is only legal while the launcher owns the window; from the
     * first guest field on there is a save, a memory card and a GS worker
     * in flight, so the change is refused here rather than half-applied.
     * The menu disables those controls (ui/menu.rml), so what this catches
     * is the other writers of rt_settings_mutable(): a hand-edited struct,
     * a script, a future caller that does not know the rule. */
    if (rt_settings_gameplay_active()) {
        if (cur->debug.console != prev.debug.console) {
            rt_log_warn("settings", "settings: debug.console can only be changed before the game"
                " starts, because the program restarts to apply it; reverted to %s",
                prev.debug.console ? "true" : "false");
            cur->debug.console = prev.debug.console;
        }
        if (cur->debug.log_file != prev.debug.log_file) {
            rt_log_warn("settings", "settings: debug.log_file can only be changed before the game"
                " starts, because the program restarts to apply it; reverted to %s",
                prev.debug.log_file ? "true" : "false");
            cur->debug.log_file = prev.debug.log_file;
        }
    }
}

} // namespace

/* The shared half of the two peeks: resolve the file, parse it, and hand
 * back the "debug" object, or null when anything at all is not as
 * expected. Neither peek reports a problem: rt_settings_init reads the
 * same file properly a moment later and logs every one of them. */
static const RtJson* peek_debug_object(RtJson* parsed_out) {
    ResolvedSource src = resolve_source();
    std::string path;
    switch (src.kind) {
    case SourceKind::EnvPath:
    case SourceKind::BaseDir:
    case SourceKind::UserConfig:
        path = src.path;
        break;
    case SourceKind::EnvDisabled:
    case SourceKind::None:
        return nullptr;
    }

    std::string text;
    if (!read_whole_file(path, &text)) return nullptr;

    std::string err;
    if (!rt_json_parse(text, parsed_out, &err)) return nullptr;
    if (parsed_out->type != RtJson::Type::Object) return nullptr;

    const RtJson* ver = parsed_out->find("version");
    if (!ver || ver->type != RtJson::Type::Number || ver->number != 1.0) return nullptr;

    const RtJson* dbg = parsed_out->find("debug");
    if (!dbg || dbg->type != RtJson::Type::Object) return nullptr;
    return dbg;
}

/* One key out of an already-parsed "debug" object, or the fallback. A null
 * `dbg` is every failure the peeks share (no file, no parse, wrong
 * version, no debug object), and they all read as the fallback. */
static bool peek_debug_bool(const RtJson* dbg, const char* key, bool fallback) {
    if (!dbg) return fallback;
    const RtJson* v = dbg->find(key);
    if (!v || v->type != RtJson::Type::Bool) return fallback;
    return v->boolean;
}

bool rt_settings_peek_console() {
    RtJson parsed;
    return peek_debug_bool(peek_debug_object(&parsed), "console", false);
}

bool rt_settings_peek_log_file() {
    RtJson parsed;
    return peek_debug_bool(peek_debug_object(&parsed), "log_file", true);
}

void rt_settings_peek_boot(bool* log_file, bool* console) {
    RtJson parsed;
    const RtJson* dbg = peek_debug_object(&parsed);
    if (log_file) *log_file = peek_debug_bool(dbg, "log_file", true);
    if (console) *console = peek_debug_bool(dbg, "console", false);
}

void rt_settings_init() {
    apply_compiled_defaults(&g_current);
    g_dom = RtJson::make_object();
    g_path.clear();
    g_save_allowed = true;
    g_save_blocked_reason.clear();
    g_save_dirty = false;

    ResolvedSource src = resolve_source();
    switch (src.kind) {
    case SourceKind::EnvDisabled:
        g_save_allowed = false;
        g_save_blocked_reason = "ICORECOMP_SETTINGS=" + src.raw_env + " selects defaults-only";
        rt_log_info("settings", "settings: ICORECOMP_SETTINGS=%s selects defaults-only, saving disabled", src.raw_env.c_str());
        break;
    case SourceKind::EnvPath:
        g_path = src.path;
        load_file(src.path);
        break;
    case SourceKind::BaseDir:
        g_path = src.path;
        if (!src.shadow_path.empty()) {
            rt_log_info("settings", "settings: %s and %s both exist; using %s, the per-user copy is shadowed",
                src.path.c_str(), src.shadow_path.c_str(), src.path.c_str());
        }
        load_file(src.path);
        break;
    case SourceKind::UserConfig:
        g_path = src.path;
        load_file(src.path);
        break;
    case SourceKind::None:
        rt_log_info("settings", "settings: no settings file found; running on defaults (save target chosen on first save)");
        break;
    }

    g_committed = g_current;
    g_last_reject.clear();
    ++g_generation;

    for (int d = 0; d < RT_BIND_DEVICE_COUNT; ++d) {
        log_bind_duplicates(bind_values(g_current, (RtBindDevice)d), kBindTables[d]);
    }

    for (const EnvTwin& t : kEnvTwins) {
        if (const char* v = env_twin_value(t)) {
            rt_log_info("settings", "settings: %s is overridden by %s=%s", t.dotted_key, t.env_var, v);
        }
    }
}

const RtSettings& rt_settings() {
    return g_current;
}

RtSettings& rt_settings_mutable() {
    return g_current;
}

/* settings.h: set once by main.cpp, just before rt_sched_boot. Its own
 * variable here rather than a question asked of main.cpp, so the refusal in
 * commit_validate above and the settings selftest can both reach it without
 * linking the runtime's boot path. */
static bool g_gameplay_active = false;

void rt_settings_set_gameplay_active(bool active) {
    g_gameplay_active = active;
}

bool rt_settings_gameplay_active() {
    return g_gameplay_active;
}

const char* rt_settings_cold_key_changed(const RtSettings& before, const RtSettings& now) {
    if (before.debug.console != now.debug.console) return "debug.console";
    if (before.debug.log_file != now.debug.log_file) return "debug.log_file";
    return nullptr;
}

void rt_settings_commit(bool save) {
    RtSettings before = g_committed;
    g_last_reject.clear();
    commit_validate(&g_current, g_committed);
    g_committed = g_current;
    ++g_generation;
    rt_settings_apply(before, g_committed);
    if (save) rt_settings_save();
}

void rt_settings_request_save() {
    g_save_dirty = true;
    g_save_requested_at = SaveClock::now();
}

void rt_settings_flush_save() {
    if (!g_save_dirty) return;
    g_save_dirty = false;
    rt_settings_save();
}

void rt_settings_flush_save_if_due() {
    if (!g_save_dirty) return;
    if (SaveClock::now() - g_save_requested_at < kSaveDebounce) return;
    g_save_dirty = false;
    rt_settings_save();
}

bool rt_settings_save() {
    if (!g_save_allowed) {
        rt_log_warn("settings", "settings: saving is disabled (%s); ignoring rt_settings_save()", g_save_blocked_reason.c_str());
        return false;
    }

    /* g_committed, not g_current: g_current is the UI's and window.cpp's
     * scratch struct and can be mid-edit (a resize handler writes a size
     * there a second before its debounced commit), and only the committed
     * struct has been through commit_validate. */
    write_struct_into_dom(g_committed, &g_dom);
    std::string text = rt_json_write(g_dom);

    if (!g_path.empty()) {
        return rt_json_write_file(g_path, text);
    }

    std::string base_target = std::string(rt_base_dir()) + "/settings.json";
    if (rt_json_write_file(base_target, text)) {
        g_path = base_target;
        return true;
    }
    std::string user_dir = rt_user_config_dir();
    if (!user_dir.empty()) {
        std::string user_target = user_dir + "/settings.json";
        if (rt_json_write_file(user_target, text)) {
            g_path = user_target;
            return true;
        }
    }
    rt_log_warn("settings", "settings: could not save to %s or the per-user config directory; settings will not persist",
        base_target.c_str());
    return false;
}

void rt_settings_reset_defaults() {
    apply_compiled_defaults(&g_current);
}

const char* rt_settings_path() {
    return g_path.c_str();
}

unsigned rt_settings_generation() {
    return g_generation;
}

const char* rt_settings_default_binding(RtBindDevice device, int slot) {
    if (!valid_device(device)) return "";
    const BindTable& t = kBindTables[device];
    return (slot >= 0 && slot < t.count) ? t.defs[slot].def : "";
}

const char* rt_settings_binding_key(RtBindDevice device, int slot) {
    if (!valid_device(device)) return "";
    const BindTable& t = kBindTables[device];
    return (slot >= 0 && slot < t.count) ? t.defs[slot].json_key : "";
}

int rt_settings_bind_slot_count(RtBindDevice device) {
    return valid_device(device) ? kBindTables[device].count : 0;
}

int rt_settings_bind_menu_slot(RtBindDevice device) {
    return valid_device(device) ? kBindTables[device].menu_slot : -1;
}

int rt_settings_bind_screenshot_slot(RtBindDevice device) {
    return valid_device(device) ? kBindTables[device].screenshot_slot : -1;
}

const char* rt_settings_last_reject() {
    return g_last_reject.c_str();
}

bool rt_settings_overridden(const char* dotted_key) {
    for (const EnvTwin& t : kEnvTwins) {
        if (std::strcmp(t.dotted_key, dotted_key) == 0) {
            return env_twin_value(t) != nullptr;
        }
    }
    return false;
}

const char* rt_settings_env_twin(const char* dotted_key) {
    for (const EnvTwin& t : kEnvTwins) {
        if (std::strcmp(t.dotted_key, dotted_key) == 0) return t.env_var;
    }
    return "";
}
