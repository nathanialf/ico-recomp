/* host/settings.h: typed settings model and JSON persistence for the
 * runtime's user-facing configuration (display, audio, input, gameplay,
 * debug, launcher).
 *
 * Everything in this layer runs on the main OS thread: guest threads are
 * minicoro coroutines scheduled on that same thread, never real OS threads,
 * so there is no concurrent access to the state here and no locking
 * anywhere in this file or settings.cpp. If that ever changes, this header
 * is the place to add it.
 *
 * Runtime-internal, like host/json.h: NOT part of the ABI contract
 * (include/recomp_*.h). Generated code and the interpreter never see this.
 */
#ifndef ICORECOMP_HOST_SETTINGS_H
#define ICORECOMP_HOST_SETTINGS_H

#include <string>

enum class RtDisplayMode { Windowed, FullscreenDesktop, FullscreenExclusive };
enum class RtPresentMode { Mailbox, Fifo, Immediate };
enum class RtFit        { Letterbox, IntegerScale, Stretch };
enum class RtRaster     { Crt, Window };
enum class RtDeinterlace { Adaptive, Bob, Weave };
enum class RtFilter     { Linear, Nearest };

/* Rebindable keyboard slots; order is the JSON schema order under
 * input.keyboard. */
enum RtKeyBind {
    RT_KB_UP, RT_KB_DOWN, RT_KB_LEFT, RT_KB_RIGHT,
    RT_KB_CROSS, RT_KB_CIRCLE, RT_KB_SQUARE, RT_KB_TRIANGLE,
    RT_KB_L1, RT_KB_R1, RT_KB_L2, RT_KB_R2, RT_KB_L3, RT_KB_R3,
    RT_KB_START, RT_KB_SELECT,
    RT_KB_LSTICK_UP, RT_KB_LSTICK_DOWN, RT_KB_LSTICK_LEFT, RT_KB_LSTICK_RIGHT,
    RT_KB_RSTICK_UP, RT_KB_RSTICK_DOWN, RT_KB_RSTICK_LEFT, RT_KB_RSTICK_RIGHT,
    RT_KB_MENU,
    RT_KB_COUNT
};

/* Rebindable gamepad slots (sticks map natively and are not slots). */
enum RtPadBind {
    RT_GP_UP, RT_GP_DOWN, RT_GP_LEFT, RT_GP_RIGHT,
    RT_GP_CROSS, RT_GP_CIRCLE, RT_GP_SQUARE, RT_GP_TRIANGLE,
    RT_GP_L1, RT_GP_R1, RT_GP_L2, RT_GP_R2, RT_GP_L3, RT_GP_R3,
    RT_GP_START, RT_GP_SELECT,
    RT_GP_MENU,
    RT_GP_COUNT
};

/* Rebindable mouse button slots. The first sixteen entries of RtKeyBind and
 * RtPadBind in the same order, and no menu slot: the menu hotkey is a key or
 * a pad button, never a mouse button, because the mouse is what drives the
 * menu's own pointer. There are no stick slots either; mouse motion is
 * mouse-look (input.mouse_look), not a bindable slot. */
enum RtMouseBind {
    RT_MB_UP, RT_MB_DOWN, RT_MB_LEFT, RT_MB_RIGHT,
    RT_MB_CROSS, RT_MB_CIRCLE, RT_MB_SQUARE, RT_MB_TRIANGLE,
    RT_MB_L1, RT_MB_R1, RT_MB_L2, RT_MB_R2, RT_MB_L3, RT_MB_R3,
    RT_MB_START, RT_MB_SELECT,
    RT_MB_COUNT
};

/* Which of the three binding tables a slot index belongs to. Every helper
 * below that used to take a `bool gamepad` takes one of these. */
enum RtBindDevice {
    RT_BIND_KEYBOARD,
    RT_BIND_GAMEPAD,
    RT_BIND_MOUSE,
    RT_BIND_DEVICE_COUNT
};

struct RtSettings {
    struct {
        RtDisplayMode mode = RtDisplayMode::Windowed;
        /* 1280x960: twice the 640x480 4:3 baseline the UI documents are
         * laid out against (src/runtime/ui/ui.cpp density_for). The
         * shim's own fallback when either field is 0 is still 640x480
         * (gs_parallel_present.cpp init_windowed); the two no longer
         * coincide. */
        int window_width = 1280, window_height = 960;
        bool remember_window_size = true;
        RtPresentMode present = RtPresentMode::Mailbox;
        RtFit fit = RtFit::Letterbox;
        /* Output frame the scanout is built at: Crt is the renderer's
         * visible area for the video mode, which crops a window that
         * overruns it (ICO's attract movie); Window grows the frame to hold
         * the whole window. Presentation only, see docs/SETTINGS.md. */
        RtRaster raster = RtRaster::Window;
        /* How the two fields of an interlaced scanout become one output
         * frame: Adaptive weaves the still parts and bobs the moving parts,
         * Bob shows each field on its own, Weave always pairs the two newest
         * fields. Presentation only, see docs/SETTINGS.md. */
        RtDeinterlace deinterlace = RtDeinterlace::Bob;
        RtFilter filter = RtFilter::Linear;
        /* 1/4/8/16, paraLLEl-GS SuperSampling. 4 and up also request
         * high-resolution scanout; there is no separate setting for it. */
        int render_scale = 1;
        bool show_fps = false;
    } display;
    struct {
        int master_volume = 100;        /* 0..100, host output gain only */
        bool mute = false;
    } audio;
    struct {
        std::string keyboard[RT_KB_COUNT];   /* SDL scancode names */
        std::string gamepad[RT_GP_COUNT];    /* SDL gamepad button/axis names */
        /* host/mouse_names.h names, not SDL strings. Unlike the other two
         * tables, "" is a legitimate value here and means the slot is
         * unbound: most mouse slots ship that way. */
        std::string mouse[RT_MB_COUNT];
        float left_deadzone = 0.0f;          /* 0 matches the pre-settings build */
        float right_deadzone = 0.0f;
        bool mouse_look = true;
        float mouse_look_sensitivity = 1.0f; /* [0.05, 20] */
        bool mouse_look_invert_y = false;
    } input;
    struct {
        /* Off reproduces retail stick behaviour. On pre-scales the left
         * stick by the game's octagonal-gate divisor so a full tilt runs at
         * every angle (see docs/SETTINGS.md). */
        bool run_any_direction = false;
    } gameplay;
    struct {
        std::string verbose;            /* ICORECOMP_VERBOSE channel spec */
        bool log_file = true;
        int profile_fields = 180;       /* 0 disables; 180 is the pre-settings
                                         * ICORECOMP_PROFILE-unset default (prof.h
                                         * g_every), kept so an env-less run still
                                         * produces a diagnosable log */
        double fps_limit_hz = 59.94;    /* 0 disables pacing */
    } debug;
    struct {
        bool show_at_startup = true;
        std::string disc_path;
    } launcher;
};

/* Reads only debug.log_file from the settings file that rt_settings_init
 * would load (same source resolution: ICORECOMP_SETTINGS, then
 * rt_base_dir()/settings.json, then rt_user_config_dir()/settings.json),
 * without logging and without touching the settings model. Exists only
 * because rt_log_init must decide about the log file before any logging
 * (and so before rt_settings_init) can run. Returns true (the default)
 * when no file is found, the file does not parse, "version" is not 1, or
 * the key is absent or not a boolean; rt_settings_init reports all of
 * those properly a moment later. */
bool rt_settings_peek_log_file();

/* Reloads the settings model from disk, replacing whatever is in memory.
 * Resolves the load source by precedence (ICORECOMP_SETTINGS env, then
 * rt_base_dir()/settings.json, then rt_user_config_dir()/settings.json,
 * then compiled-in defaults), logs where the settings came from, and logs
 * every environment-variable override (see rt_settings_overridden) that is
 * currently in effect. Safe to call repeatedly: main calls it once at
 * startup, the selftest calls it once per test case to reload against a
 * freshly staged file. */
void rt_settings_init();

/* The current, committed settings. Valid after rt_settings_init(). */
const RtSettings& rt_settings();

/* UI-only escape hatch: mutate freely, then call rt_settings_commit() to
 * validate and persist. Never read mid-edit by anything outside the UI
 * thread's own code path -- there is no locking (see the file comment). */
RtSettings& rt_settings_mutable();

/* Re-validates the struct returned by rt_settings_mutable() field by field,
 * using the same ranges and allowed sets as the JSON loader. A field that
 * fails validation reverts to its previously committed value (not the
 * compiled-in default) with a log naming the dotted key, the bad value, and
 * what it was reverted to. Then runs rt_settings_apply(before, now) with
 * the previously committed struct.
 *
 * `save` controls the file write only. The settings menu commits with
 * save=false on every control change (a slider drag would otherwise be one
 * atomic file write per field) and asks for the write through
 * rt_settings_request_save() below, which coalesces them.
 */
void rt_settings_commit(bool save = true);

/* Marks the settings dirty and timestamps the request (steady_clock). The
 * write itself happens later, from rt_settings_apply_pending() once a
 * second has passed with no further request, or immediately from
 * rt_settings_flush_save(). This is the runtime's one save debounce: window
 * resizes (host/window.cpp) and menu edits (ui/ui_settings_model.cpp) both
 * go through it. */
void rt_settings_request_save();

/* Writes now if a save was requested and has not been written yet; does
 * nothing otherwise. Called when the menu closes, so a change is on disk
 * before the user can quit. It runs no applier, but it does write and
 * fsync a file, so callers place it at the field boundary rather than
 * inside the event pump. */
void rt_settings_flush_save();

/* The debounced half of the pair: writes only when a save was requested and
 * the last request is at least a second old. rt_settings_apply_pending()
 * calls it every field. */
void rt_settings_flush_save_if_due();

/* Atomically writes the last committed settings to rt_settings_path()
 * (choosing a save target on first use if none is set yet). What goes to
 * the file is the struct rt_settings_commit() last accepted, never the
 * in-flight rt_settings_mutable() one: an edit that has not been committed
 * has not been validated, and host/window.cpp's resize handler holds an
 * uncommitted size there for up to a second. Commit first, then save.
 *
 * Returns false, with a log naming the reason, when saving is blocked
 * (ICORECOMP_SETTINGS=-/0, a loaded file with version > 1, or a loaded file
 * that failed to parse and was copied to <path>.bad) or when the write
 * itself fails. */
bool rt_settings_save();

/* Resets the in-memory struct (rt_settings_mutable()'s target) to compiled-
 * in defaults. Does not save; call rt_settings_commit() or
 * rt_settings_save() afterward to persist it. */
void rt_settings_reset_defaults();

/* Path settings were loaded from / will be saved to. Empty string ("")
 * until a load or save has picked one (no file found yet and nothing saved
 * yet, or ICORECOMP_SETTINGS selected defaults-only). */
const char* rt_settings_path();

/* Bumped by every rt_settings_init() and every rt_settings_commit(), and
 * never zero, so a consumer that caches something derived from the settings
 * can hold the generation it built from and rebuild when the two differ.
 * host/input.cpp uses it for its SDL scancode/button tables, which are the
 * one derived structure in the runtime that is too expensive to rebuild per
 * poll. Everything else reads rt_settings() fresh. */
unsigned rt_settings_generation();

/* The compiled-in default binding for one slot, and the JSON key that slot
 * is written under ("cross", "lstick_up", "menu"). `device` selects the
 * table (RtKeyBind, RtPadBind or RtMouseBind slots); an out-of-range slot or
 * device returns "".
 *
 * A "" default is a real answer for RT_BIND_MOUSE and only for it: every
 * mouse slot but square and r1 ships unbound. Keyboard and gamepad have no
 * empty default, so "" from those two still means the slot index was bad.
 *
 * host/input.cpp needs both: when a stored name does not resolve it falls
 * back to the compiled default for that slot and names the slot in the log
 * line, and it must not carry its own second copy of these tables (the
 * pre-settings hardcoded map drifting out of sync with the JSON is exactly
 * what this milestone removes). ui/ui_rebind.cpp needs them for the same
 * reason. */
const char* rt_settings_default_binding(RtBindDevice device, int slot);
const char* rt_settings_binding_key(RtBindDevice device, int slot);

/* The number of slots on one device (RT_KB_COUNT / RT_GP_COUNT /
 * RT_MB_COUNT), and the index of its menu slot, or -1 when it has none.
 * Only the mouse has none. Both return the same shape for a bad device as
 * the two accessors above: 0 slots, no menu slot. Callers that walk a
 * device's slots go through these rather than switching on the enum
 * themselves. */
int rt_settings_bind_slot_count(RtBindDevice device);
int rt_settings_bind_menu_slot(RtBindDevice device);

/* Chord grammar for input.gamepad.menu, the only slot a chord is legal on
 * (see docs/SETTINGS.md section 7): "<button>+<button>", exactly one
 * interior '+', and neither side may end in '+' or '-' -- that suffix is
 * the axis-direction convention ("lefttrigger+"), so a trailing '+' with
 * nothing after it is an axis, not a chord, and "a+b+c" (two interior '+'s)
 * fails resolution rather than picking one of them. Order is not
 * significant: nothing compares the two parts against which one struck
 * first. Splits `name` into `*first`/`*second` and returns true when it
 * parses as a chord; returns false, leaving both untouched, for the common
 * case of an ordinary button or axis name. Never called on a keyboard
 * name: "Keypad +" is a legitimate SDL scancode name and must not be
 * chord-parsed. */
bool rt_settings_split_chord(const std::string& name, std::string* first, std::string* second);

/* The message from the last rt_settings_commit() that rejected something and
 * reverted it, or "" when the last commit accepted everything. Cleared at
 * the start of every commit. Only the binding rules (the menu key colliding
 * with a pad binding, or two slots on one device sharing a name) fill this
 * in: they are the rejections a user makes deliberately, from the menu, and
 * the menu has to say so inline instead of only in the log. Range reverts
 * stay log-only, as before. */
const char* rt_settings_last_reject();

/* True when `dotted_key` (e.g. "debug.fps_limit_hz") has an environment
 * variable twin and that variable is currently set. The environment always
 * wins over the file for these; each consumer reads its own env var at its
 * own call site, this only powers the startup log and UI "overridden by"
 * display.
 *
 * "Set" means present, even as an empty string, matching the getenv() !=
 * NULL every consumer tests. ICORECOMP_LOG is the exception: log.cpp reads
 * it as `env && *env`, so an empty value leaves debug.log_file in charge
 * and this returns false for it. */
bool rt_settings_overridden(const char* dotted_key);

/* The environment variable that overrides `dotted_key`, or "" when that key
 * has no environment twin. The settings menu names it in the hint under a
 * disabled control ("overridden by ICORECOMP_X"), which is why the name is
 * exposed here instead of being duplicated in the UI. */
const char* rt_settings_env_twin(const char* dotted_key);

/* The one place a settings diff becomes subsystem calls (settings_apply.cpp).
 * Called from rt_settings_commit(), after validation, with the previously
 * committed struct and the newly committed one. Hot settings (fullscreen,
 * window size, presentation fit/filter) apply immediately; warm settings
 * (present mode, render scale) queue for rt_settings_apply_pending(), which
 * rt_gs_vsync_hook (hw/gspriv.cpp) calls at the next field boundary, because
 * their subsystem entry points fatal when called mid-frame. Verbosity is
 * hot too, pushed through rt_log_set_verbose here (log.cpp parses the spec
 * once, it does not poll the struct). Volume, fps cap and profiler period
 * need no applier: their consumers read rt_settings() fresh on every use.
 * A setting whose env twin is set (rt_settings_overridden) is never
 * applied; the environment owns it for the life of the run. */
void rt_settings_apply(const RtSettings& before, const RtSettings& now);
void rt_settings_apply_pending();

#endif /* ICORECOMP_HOST_SETTINGS_H */
