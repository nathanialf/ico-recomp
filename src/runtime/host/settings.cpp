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
#include "host/portable.h"
#include "host/settings.h"

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

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

/* ---- bind default tables ------------------------------------------------
 *
 * These are SDL_GetScancodeName / SDL mapping-string tokens. They are the
 * only copy: host/input.cpp builds its SDL tables from rt_settings() and
 * falls back to these through rt_settings_default_binding() when a stored
 * name does not resolve, so there is no second hardcoded map to keep in
 * sync. The values reproduce the pre-settings map exactly.
 */
struct BindDef {
    const char* json_key;
    const char* def;
};

/* A trailing '+' or '-' on an axis name (lefttrigger+, righttrigger+) is
 * our convention for direction, not part of the SDL token itself. */
constexpr BindDef kKeyboardBinds[RT_KB_COUNT] = {
    {"up", "Up"}, {"down", "Down"}, {"left", "Left"}, {"right", "Right"},
    {"cross", "X"}, {"circle", "C"}, {"square", "Z"}, {"triangle", "V"},
    {"l1", "Q"}, {"r1", "E"}, {"l2", "1"}, {"r2", "3"}, {"l3", "T"}, {"r3", "Y"},
    {"start", "Return"}, {"select", "Backspace"},
    {"lstick_up", "W"}, {"lstick_down", "S"}, {"lstick_left", "A"}, {"lstick_right", "D"},
    {"rstick_up", "I"}, {"rstick_down", "K"}, {"rstick_left", "J"}, {"rstick_right", "L"},
    {"menu", "F1"},
};

constexpr BindDef kGamepadBinds[RT_GP_COUNT] = {
    {"up", "dpup"}, {"down", "dpdown"}, {"left", "dpleft"}, {"right", "dpright"},
    {"cross", "a"}, {"circle", "b"}, {"square", "x"}, {"triangle", "y"},
    {"l1", "leftshoulder"}, {"r1", "rightshoulder"}, {"l2", "lefttrigger+"}, {"r2", "righttrigger+"},
    {"l3", "leftstick"}, {"r3", "rightstick"}, {"start", "start"}, {"select", "back"},
    {"menu", "guide"},
};

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
    {"display.present", "ICORECOMP_GS_PRESENT", false},
    {"debug.fps_limit_hz", "ICORECOMP_FPS_LIMIT", false},
    {"debug.verbose", "ICORECOMP_VERBOSE", false},
    {"debug.profile_fields", "ICORECOMP_PROFILE", false},
    {"debug.log_file", "ICORECOMP_LOG", true},
    {"audio.mute", "ICORECOMP_NO_AUDIO", false},
};

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
constexpr EnumEntry kPresentNames[] = {
    {"mailbox", (int)RtPresentMode::Mailbox},
    {"fifo", (int)RtPresentMode::Fifo},
    {"immediate", (int)RtPresentMode::Immediate},
};
constexpr EnumEntry kFitNames[] = {
    {"letterbox", (int)RtFit::Letterbox},
    {"integer", (int)RtFit::IntegerScale},
    {"stretch", (int)RtFit::Stretch},
};
constexpr EnumEntry kFilterNames[] = {
    {"linear", (int)RtFilter::Linear},
    {"nearest", (int)RtFilter::Nearest},
};

constexpr int kRenderScales[] = {1, 2, 4, 8, 16};

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
    for (int i = 0; i < RT_KB_COUNT; ++i) s->input.keyboard[i] = kKeyboardBinds[i].def;
    for (int i = 0; i < RT_GP_COUNT; ++i) s->input.gamepad[i] = kGamepadBinds[i].def;
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
        rt_log("settings", "settings: %s is not a boolean (kept default %s)", dotted, *out ? "true" : "false");
        return;
    }
    *out = v->boolean;
}

void load_string(const RtJson* v, const char* dotted, std::string* out) {
    if (!v) return;
    if (v->type != RtJson::Type::String) {
        rt_log("settings", "settings: %s is not a string (kept default \"%s\")", dotted, out->c_str());
        return;
    }
    *out = v->str;
}

void load_int_range(const RtJson* v, const char* dotted, int lo, int hi, int* out) {
    if (!v) return;
    if (v->type != RtJson::Type::Number) {
        rt_log("settings", "settings: %s is not a number (kept default %d)", dotted, *out);
        return;
    }
    double d = v->number;
    if (d != std::floor(d) || d < lo || d > hi) {
        rt_log("settings", "settings: %s = %.6g is out of range [%d, %d] (kept default %d)", dotted, d, lo, hi, *out);
        return;
    }
    *out = (int)d;
}

void load_double_range(const RtJson* v, const char* dotted, double lo, double hi, bool lo_exclusive, double* out) {
    if (!v) return;
    if (v->type != RtJson::Type::Number) {
        rt_log("settings", "settings: %s is not a number (kept default %.6g)", dotted, *out);
        return;
    }
    double d = v->number;
    bool ok = (lo_exclusive ? d > lo : d >= lo) && d <= hi;
    if (!ok) {
        rt_log("settings", "settings: %s = %.6g is out of range %c%.6g, %.6g] (kept default %.6g)",
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

/* fps_limit_hz: 0 disables pacing, otherwise [1, 1000]. */
void load_fps_limit(const RtJson* v, const char* dotted, double* out) {
    if (!v) return;
    if (v->type != RtJson::Type::Number) {
        rt_log("settings", "settings: %s is not a number (kept default %.6g)", dotted, *out);
        return;
    }
    double d = v->number;
    if (d == 0.0 || (d >= 1.0 && d <= 1000.0)) {
        *out = d;
        return;
    }
    rt_log("settings", "settings: %s = %.6g is out of range (must be 0 or [1, 1000]) (kept default %.6g)", dotted, d, *out);
}

void load_int_set(const RtJson* v, const char* dotted, const int* set, size_t n, int* out) {
    if (!v) return;
    if (v->type != RtJson::Type::Number) {
        rt_log("settings", "settings: %s is not a number (kept default %d)", dotted, *out);
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
    rt_log("settings", "settings: %s = %.6g is not one of {%s} (kept default %d)", dotted, d, allowed.c_str(), *out);
}

template <size_t N>
void load_enum(const RtJson* v, const char* dotted, const EnumEntry (&table)[N], int* out) {
    if (!v) return;
    if (v->type != RtJson::Type::String) {
        rt_log("settings", "settings: %s is not a string (kept default \"%s\")", dotted, enum_name(table, *out));
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
    rt_log("settings", "settings: %s = \"%s\" is not one of %s (kept default \"%s\")",
        dotted, v->str.c_str(), allowed.c_str(), enum_name(table, *out));
}

/* ---- unknown-key logging ------------------------------------------------ */

template <typename Pred>
void log_unknown_keys(const RtJson& obj, const std::string& parent, Pred is_known) {
    if (obj.type != RtJson::Type::Object) return;
    for (const auto& kv : obj.obj) {
        if (is_known(kv.first)) continue;
        std::string dotted = parent.empty() ? kv.first : parent + "." + kv.first;
        rt_log("settings", "settings: unknown key \"%s\" kept as-is", dotted.c_str());
    }
}

/* ---- DOM -> struct ------------------------------------------------------- */

void map_bind_section(const RtJson* sec, const char* dotted_parent, const BindDef* defs, int count, std::string* out) {
    if (!sec) return;
    if (sec->type != RtJson::Type::Object) {
        rt_log("settings", "settings: %s is not an object (kept defaults)", dotted_parent);
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
        return is_one_of(k, {"version", "display", "audio", "input", "debug", "launcher"});
    });

    if (const RtJson* d = dom.find("display")) {
        if (d->type != RtJson::Type::Object) {
            rt_log("settings", "settings: \"display\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*d, "display", [](const std::string& k) {
                return is_one_of(k, {"mode", "window_width", "window_height", "remember_window_size",
                    "present", "fit", "filter", "render_scale", "hires_scanout", "show_fps"});
            });
            load_enum(d->find("mode"), "display.mode", kDisplayModeNames, (int*)&out->display.mode);
            load_int_range(d->find("window_width"), "display.window_width", 320, 16384, &out->display.window_width);
            load_int_range(d->find("window_height"), "display.window_height", 320, 16384, &out->display.window_height);
            load_bool(d->find("remember_window_size"), "display.remember_window_size", &out->display.remember_window_size);
            load_enum(d->find("present"), "display.present", kPresentNames, (int*)&out->display.present);
            load_enum(d->find("fit"), "display.fit", kFitNames, (int*)&out->display.fit);
            load_enum(d->find("filter"), "display.filter", kFilterNames, (int*)&out->display.filter);
            load_int_set(d->find("render_scale"), "display.render_scale", kRenderScales, std::size(kRenderScales), &out->display.render_scale);
            load_bool(d->find("hires_scanout"), "display.hires_scanout", &out->display.hires_scanout);
            if (out->display.hires_scanout && out->display.render_scale < 4) {
                rt_log("settings", "settings: display.hires_scanout is set but display.render_scale is %d;"
                    " hires scanout stays inert below 4x (value kept)", out->display.render_scale);
            }
            load_bool(d->find("show_fps"), "display.show_fps", &out->display.show_fps);
        }
    }

    if (const RtJson* a = dom.find("audio")) {
        if (a->type != RtJson::Type::Object) {
            rt_log("settings", "settings: \"audio\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*a, "audio", [](const std::string& k) {
                return is_one_of(k, {"master_volume", "mute"});
            });
            load_int_range(a->find("master_volume"), "audio.master_volume", 0, 100, &out->audio.master_volume);
            load_bool(a->find("mute"), "audio.mute", &out->audio.mute);
        }
    }

    if (const RtJson* i = dom.find("input")) {
        if (i->type != RtJson::Type::Object) {
            rt_log("settings", "settings: \"input\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*i, "input", [](const std::string& k) {
                return is_one_of(k, {"keyboard", "gamepad", "left_deadzone", "right_deadzone",
                    "trigger_threshold", "rumble"});
            });
            map_bind_section(i->find("keyboard"), "input.keyboard", kKeyboardBinds, RT_KB_COUNT, out->input.keyboard);
            map_bind_section(i->find("gamepad"), "input.gamepad", kGamepadBinds, RT_GP_COUNT, out->input.gamepad);
            load_float_range(i->find("left_deadzone"), "input.left_deadzone", 0.0, 0.95, false, &out->input.left_deadzone);
            load_float_range(i->find("right_deadzone"), "input.right_deadzone", 0.0, 0.95, false, &out->input.right_deadzone);
            load_float_range(i->find("trigger_threshold"), "input.trigger_threshold", 0.0, 1.0, true, &out->input.trigger_threshold);
            load_bool(i->find("rumble"), "input.rumble", &out->input.rumble);
        }
    }

    if (const RtJson* dbg = dom.find("debug")) {
        if (dbg->type != RtJson::Type::Object) {
            rt_log("settings", "settings: \"debug\" is not an object (section kept as defaults)");
        } else {
            log_unknown_keys(*dbg, "debug", [](const std::string& k) {
                return is_one_of(k, {"verbose", "log_file", "profile_fields", "fps_limit_hz"});
            });
            load_string(dbg->find("verbose"), "debug.verbose", &out->debug.verbose);
            load_bool(dbg->find("log_file"), "debug.log_file", &out->debug.log_file);
            load_int_range(dbg->find("profile_fields"), "debug.profile_fields", 0, 100000, &out->debug.profile_fields);
            load_fps_limit(dbg->find("fps_limit_hz"), "debug.fps_limit_hz", &out->debug.fps_limit_hz);
        }
    }

    if (const RtJson* lc = dom.find("launcher")) {
        if (lc->type != RtJson::Type::Object) {
            rt_log("settings", "settings: \"launcher\" is not an object (section kept as defaults)");
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
        rt_log("settings", "settings: \"%s\" was not an object; replacing it with one on save", key);
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
    d.set("present", RtJson::make_string(enum_name(kPresentNames, (int)s.display.present)));
    d.set("fit", RtJson::make_string(enum_name(kFitNames, (int)s.display.fit)));
    d.set("filter", RtJson::make_string(enum_name(kFilterNames, (int)s.display.filter)));
    d.set("render_scale", RtJson::make_number(s.display.render_scale));
    d.set("hires_scanout", RtJson::make_bool(s.display.hires_scanout));
    d.set("show_fps", RtJson::make_bool(s.display.show_fps));

    RtJson& a = get_or_make_object(dom, "audio");
    a.set("master_volume", RtJson::make_number(s.audio.master_volume));
    a.set("mute", RtJson::make_bool(s.audio.mute));

    RtJson& in = get_or_make_object(dom, "input");
    write_bind_section(&in, "keyboard", kKeyboardBinds, RT_KB_COUNT, s.input.keyboard);
    write_bind_section(&in, "gamepad", kGamepadBinds, RT_GP_COUNT, s.input.gamepad);
    in.set("left_deadzone", RtJson::make_number(s.input.left_deadzone));
    in.set("right_deadzone", RtJson::make_number(s.input.right_deadzone));
    in.set("trigger_threshold", RtJson::make_number(s.input.trigger_threshold));
    in.set("rumble", RtJson::make_bool(s.input.rumble));

    RtJson& dbg = get_or_make_object(dom, "debug");
    dbg.set("verbose", RtJson::make_string(s.debug.verbose));
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
        rt_log("settings", "settings: could not write %s: %s", bad.c_str(), std::strerror(errno));
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
        rt_log("settings", "settings: %s not found yet; running on defaults, will create it on first save", path.c_str());
        return;
    }

    RtJson parsed;
    std::string err;
    if (!rt_json_parse(text, &parsed, &err)) {
        write_bad_copy(path, text);
        rt_log("settings", "settings: %s:%s; running on defaults, file copied to %s.bad", path.c_str(), err.c_str(), path.c_str());
        block_saving_for_broken_file(path);
        return;
    }
    if (parsed.type != RtJson::Type::Object) {
        write_bad_copy(path, text);
        rt_log("settings", "settings: %s: top level is not an object; running on defaults, file copied to %s.bad", path.c_str(), path.c_str());
        block_saving_for_broken_file(path);
        return;
    }
    const RtJson* ver = parsed.find("version");
    if (!ver || ver->type != RtJson::Type::Number) {
        write_bad_copy(path, text);
        rt_log("settings", "settings: %s: missing or non-numeric \"version\"; running on defaults, file copied to %s.bad", path.c_str(), path.c_str());
        block_saving_for_broken_file(path);
        return;
    }
    if (ver->number > 1.0) {
        g_save_allowed = false;
        g_save_blocked_reason = "the loaded file has version " + std::to_string((long long)ver->number) + ", written by a newer build";
        rt_log("settings", "settings: %s was written by a newer build (version %.0f); running on defaults, the file is left untouched",
            path.c_str(), ver->number);
        return;
    }
    if (ver->number != 1.0) {
        write_bad_copy(path, text);
        rt_log("settings", "settings: %s: unsupported \"version\" %.6g (expected 1); running on defaults, file copied to %s.bad",
            path.c_str(), ver->number, path.c_str());
        block_saving_for_broken_file(path);
        return;
    }

    g_dom = std::move(parsed);
    map_from_dom(g_dom, &g_current);
    rt_log("settings", "settings: loaded from %s", path.c_str());
}

/* Atomic write of `text` to `target`: "<target>.tmp" then fsync then
 * rename over the target. Any failure logs with strerror, removes the .tmp
 * best-effort, and leaves whatever was at `target` alone. */
bool write_atomic(const std::string& target, const std::string& text) {
    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(target).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    std::string tmp = target + ".tmp";
    std::FILE* f = rt_fopen_utf8(tmp.c_str(), "wb");
    if (!f) {
        rt_log("settings", "settings: could not open %s for writing: %s", tmp.c_str(), std::strerror(errno));
        return false;
    }
    size_t written = std::fwrite(text.data(), 1, text.size(), f);
    if (written != text.size()) {
        rt_log("settings", "settings: short write to %s: %s", tmp.c_str(), std::strerror(errno));
        std::fclose(f);
        std::remove(tmp.c_str());
        return false;
    }
    std::fflush(f);
#ifdef _WIN32
    _commit(rt_fileno(f));
#else
    fsync(rt_fileno(f));
#endif
    std::fclose(f);

    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        rt_log("settings", "settings: rename %s -> %s failed: %s", tmp.c_str(), target.c_str(), ec.message().c_str());
        std::remove(tmp.c_str());
        return false;
    }
    return true;
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
    rt_log("settings", "settings: %s = %d is out of range [%d, %d]; reverted to %d", dotted, *v, lo, hi, prev);
    *v = prev;
}

/* Bounds in double, and the value promoted to double before comparing, so
 * this is the same predicate load_float_range applies (see its comment). */
void revert_float(float* v, float prev, const char* dotted, double lo, double hi, bool lo_exclusive) {
    const double d = *v;
    bool ok = (lo_exclusive ? d > lo : d >= lo) && d <= hi;
    if (ok) return;
    rt_log("settings", "settings: %s = %.6g is out of range %c%.6g, %.6g]; reverted to %.6g",
        dotted, d, lo_exclusive ? '(' : '[', lo, hi, (double)prev);
    *v = prev;
}

/* ---- binding rules -------------------------------------------------------
 *
 * Two rules, both about one name being claimed twice on one device:
 *
 *   1. The menu key (input.keyboard.menu / input.gamepad.menu) is consumed
 *      in the event pump and never reaches the pad (ui/ui_events.cpp), so a
 *      menu key that is also a pad binding is a pad button the game can
 *      never see. Rejected.
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
 * than treated as equal to each other: an empty slot is a name that did not
 * resolve, host/input.cpp already replaces it with the compiled default and
 * says so, and two of them are not a collision the user made.
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

void note_reject(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rt_log("settings", "settings: %s", buf);
    if (!g_last_reject.empty()) g_last_reject += "; ";
    g_last_reject += buf;
}

/* One device's slots. `menu_slot` is the last entry in both enums, which is
 * why `count` and `menu_slot` are passed rather than derived. */
void validate_binds(std::string* cur, const std::string* prev, const BindDef* defs,
                    int count, int menu_slot, const char* section) {
    /* Rule 1, first: whatever the menu key ends up as, the ordinary-slot pass
     * below then sees the settled value. Which side moved decides which side
     * reverts: binding the menu key onto an existing pad slot is the menu
     * key's fault, binding a pad slot onto the menu key is the pad slot's,
     * and reverting the menu slot in the second case would leave the
     * collision standing. */
    for (int i = 0; i < count; ++i) {
        if (i == menu_slot) continue;
        if (!bind_name_equal(cur[menu_slot], cur[i])) continue;
        const bool menu_changed = cur[menu_slot] != prev[menu_slot];
        const bool other_changed = cur[i] != prev[i];
        if (!menu_changed && !other_changed) continue;
        note_reject("%s.menu and %s.%s are both \"%s\"; the menu key never reaches the pad,"
            " so %s%s%s reverted",
            section, section, defs[i].json_key, cur[menu_slot].c_str(),
            menu_changed ? "menu" : "",
            (menu_changed && other_changed) ? " and " : "",
            other_changed ? defs[i].json_key : "");
        if (menu_changed) cur[menu_slot] = prev[menu_slot];
        if (other_changed) cur[i] = prev[i];
    }

    /* Rule 2, the same way: only the slots this commit moved revert. */
    for (int i = 0; i < count; ++i) {
        if (i == menu_slot) continue;
        for (int j = i + 1; j < count; ++j) {
            if (j == menu_slot) continue;
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
}

/* Load-time report only, no value change: a duplicate that came in from the
 * settings file is the user's own file and this layer never rewrites user
 * data on load (see the bad-value policy at the top). Saying so once at
 * startup is what keeps the commit-time rule above from having to guess. */
void log_bind_duplicates(const std::string* v, const BindDef* defs, int count,
                         int menu_slot, const char* section) {
    for (int i = 0; i < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (!bind_name_equal(v[i], v[j])) continue;
            if (i == menu_slot || j == menu_slot) {
                rt_log("settings", "settings: %s.menu and %s.%s are both \"%s\"; the menu key is"
                    " consumed by the menu, so that pad binding will never fire",
                    section, section, defs[i == menu_slot ? j : i].json_key, v[i].c_str());
            } else {
                rt_log("settings", "settings: %s.%s and %s.%s are both \"%s\"; that one input will"
                    " press both buttons", section, defs[i].json_key, section, defs[j].json_key,
                    v[i].c_str());
            }
        }
    }
}

void commit_validate(RtSettings* cur, const RtSettings& prev) {
    revert_int(&cur->display.window_width, prev.display.window_width, "display.window_width", 320, 16384);
    revert_int(&cur->display.window_height, prev.display.window_height, "display.window_height", 320, 16384);
    revert_int(&cur->audio.master_volume, prev.audio.master_volume, "audio.master_volume", 0, 100);
    revert_int(&cur->debug.profile_fields, prev.debug.profile_fields, "debug.profile_fields", 0, 100000);

    revert_float(&cur->input.left_deadzone, prev.input.left_deadzone, "input.left_deadzone", 0.0, 0.95, false);
    revert_float(&cur->input.right_deadzone, prev.input.right_deadzone, "input.right_deadzone", 0.0, 0.95, false);
    revert_float(&cur->input.trigger_threshold, prev.input.trigger_threshold, "input.trigger_threshold", 0.0, 1.0, true);

    if (!(cur->debug.fps_limit_hz == 0.0 || (cur->debug.fps_limit_hz >= 1.0 && cur->debug.fps_limit_hz <= 1000.0))) {
        rt_log("settings", "settings: debug.fps_limit_hz = %.6g is out of range (must be 0 or [1, 1000]); reverted to %.6g",
            cur->debug.fps_limit_hz, prev.debug.fps_limit_hz);
        cur->debug.fps_limit_hz = prev.debug.fps_limit_hz;
    }

    bool rs_ok = false;
    for (int allowed : kRenderScales) {
        if (cur->display.render_scale == allowed) rs_ok = true;
    }
    if (!rs_ok) {
        rt_log("settings", "settings: display.render_scale = %d is not one of {1, 2, 4, 8, 16}; reverted to %d",
            cur->display.render_scale, prev.display.render_scale);
        cur->display.render_scale = prev.display.render_scale;
    }

    if (cur->display.hires_scanout && cur->display.render_scale < 4) {
        rt_log("settings", "settings: display.hires_scanout is set but display.render_scale is %d;"
            " hires scanout stays inert below 4x", cur->display.render_scale);
    }

    validate_binds(cur->input.keyboard, prev.input.keyboard, kKeyboardBinds, RT_KB_COUNT,
        RT_KB_MENU, "input.keyboard");
    validate_binds(cur->input.gamepad, prev.input.gamepad, kGamepadBinds, RT_GP_COUNT,
        RT_GP_MENU, "input.gamepad");
}

} // namespace

bool rt_settings_peek_log_file() {
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
        return true;
    }

    std::string text;
    if (!read_whole_file(path, &text)) return true;

    RtJson parsed;
    std::string err;
    if (!rt_json_parse(text, &parsed, &err)) return true;
    if (parsed.type != RtJson::Type::Object) return true;

    const RtJson* ver = parsed.find("version");
    if (!ver || ver->type != RtJson::Type::Number || ver->number != 1.0) return true;

    const RtJson* dbg = parsed.find("debug");
    if (!dbg || dbg->type != RtJson::Type::Object) return true;

    const RtJson* lf = dbg->find("log_file");
    if (!lf || lf->type != RtJson::Type::Bool) return true;

    return lf->boolean;
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
        rt_log("settings", "settings: ICORECOMP_SETTINGS=%s selects defaults-only, saving disabled", src.raw_env.c_str());
        break;
    case SourceKind::EnvPath:
        g_path = src.path;
        load_file(src.path);
        break;
    case SourceKind::BaseDir:
        g_path = src.path;
        if (!src.shadow_path.empty()) {
            rt_log("settings", "settings: %s and %s both exist; using %s, the per-user copy is shadowed",
                src.path.c_str(), src.shadow_path.c_str(), src.path.c_str());
        }
        load_file(src.path);
        break;
    case SourceKind::UserConfig:
        g_path = src.path;
        load_file(src.path);
        break;
    case SourceKind::None:
        rt_log("settings", "settings: no settings file found; running on defaults (save target chosen on first save)");
        break;
    }

    g_committed = g_current;
    g_last_reject.clear();
    ++g_generation;

    log_bind_duplicates(g_current.input.keyboard, kKeyboardBinds, RT_KB_COUNT, RT_KB_MENU, "input.keyboard");
    log_bind_duplicates(g_current.input.gamepad, kGamepadBinds, RT_GP_COUNT, RT_GP_MENU, "input.gamepad");

    for (const EnvTwin& t : kEnvTwins) {
        if (const char* v = env_twin_value(t)) {
            rt_log("settings", "settings: %s is overridden by %s=%s", t.dotted_key, t.env_var, v);
        }
    }
}

const RtSettings& rt_settings() {
    return g_current;
}

RtSettings& rt_settings_mutable() {
    return g_current;
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
        rt_log("settings", "settings: saving is disabled (%s); ignoring rt_settings_save()", g_save_blocked_reason.c_str());
        return false;
    }

    /* g_committed, not g_current: g_current is the UI's and window.cpp's
     * scratch struct and can be mid-edit (a resize handler writes a size
     * there a second before its debounced commit), and only the committed
     * struct has been through commit_validate. */
    write_struct_into_dom(g_committed, &g_dom);
    std::string text = rt_json_write(g_dom);

    if (!g_path.empty()) {
        return write_atomic(g_path, text);
    }

    std::string base_target = std::string(rt_base_dir()) + "/settings.json";
    if (write_atomic(base_target, text)) {
        g_path = base_target;
        return true;
    }
    std::string user_dir = rt_user_config_dir();
    if (!user_dir.empty()) {
        std::string user_target = user_dir + "/settings.json";
        if (write_atomic(user_target, text)) {
            g_path = user_target;
            return true;
        }
    }
    rt_log("settings", "settings: could not save to %s or the per-user config directory; settings will not persist",
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

const char* rt_settings_default_binding(bool gamepad, int slot) {
    if (gamepad) {
        return (slot >= 0 && slot < RT_GP_COUNT) ? kGamepadBinds[slot].def : "";
    }
    return (slot >= 0 && slot < RT_KB_COUNT) ? kKeyboardBinds[slot].def : "";
}

const char* rt_settings_binding_key(bool gamepad, int slot) {
    if (gamepad) {
        return (slot >= 0 && slot < RT_GP_COUNT) ? kGamepadBinds[slot].json_key : "";
    }
    return (slot >= 0 && slot < RT_KB_COUNT) ? kKeyboardBinds[slot].json_key : "";
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
