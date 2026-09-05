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

/* debug.fps_limit_hz: "the rate of the video mode the game programmed".
 * Not a rate, so it is outside the [1, 1000] a real rate has to be in and
 * outside the 0 that means "no pacing". */
constexpr double RT_FPS_LIMIT_MODE_RATE = -1.0;

#include <string>

/* For RtLogLevel: debug.log_level is a typed field like display.raster,
 * and the logging API is where that enum is defined. */
#include "runtime.h"

enum class RtDisplayMode { Windowed, FullscreenDesktop, FullscreenExclusive };
enum class RtPresentMode { Mailbox, Fifo, Immediate };
enum class RtFit        { Letterbox, IntegerScale, Stretch };
enum class RtRaster     { Crt, Window };
/* display.widescreen. Off is the retail 4:3 picture; Window follows the
 * window's own aspect; SixteenNine fixes it at 16:9 whatever the window is.
 * The one setting besides display.raster that changes a value the game
 * supplied; see docs/SETTINGS.md section 6 and guest/widescreen.cpp. */
enum class RtWidescreen { Off, Window, SixteenNine };
enum class RtDeinterlace { Adaptive, Bob, Weave };
enum class RtFilter     { Linear, Nearest };
/* Which live GS renderer a run builds, and for the native renderer which
 * graphics API it runs on. Auto resolves to ParallelGs wherever that backend
 * is built, because the native renderer has not passed its parity gate;
 * failing that it is Metal on macOS and Vulkan everywhere else. Vulkan,
 * D3D12 and Metal all mean the native renderer (gs/render/gs_native.cpp) on
 * that RHI backend.
 *
 * NOT a setting. display.backend was retired on 2026-09-05 and RtSettings
 * carries no field for it; the only thing that resolves one of these is
 * gs/gs_select.cpp, off ICORECOMP_GS_BACKEND, once, at rt_hw_init(). The
 * enum lives here because that is where the rest of the display vocabulary
 * is. See gs/gs_select.cpp and docs/SETTINGS.md section 6. */
enum class RtGsBackend  { Auto, ParallelGs, Vulkan, D3D12, Metal };

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
    RT_KB_SCREENSHOT,
    RT_KB_COUNT
};

/* Rebindable gamepad slots (sticks map natively and are not slots). */
enum RtPadBind {
    RT_GP_UP, RT_GP_DOWN, RT_GP_LEFT, RT_GP_RIGHT,
    RT_GP_CROSS, RT_GP_CIRCLE, RT_GP_SQUARE, RT_GP_TRIANGLE,
    RT_GP_L1, RT_GP_R1, RT_GP_L2, RT_GP_R2, RT_GP_L3, RT_GP_R3,
    RT_GP_START, RT_GP_SELECT,
    RT_GP_MENU,
    RT_GP_SCREENSHOT,
    RT_GP_COUNT
};

/* The second controller's slots are the same sixteen DS2 buttons in the same
 * order, and they stop there. Neither host hotkey is on it: the menu key and
 * the screenshot key are consumed in the event pump, and a second way into a
 * menu that already blanks both pads is not one player 2 should have.
 * RT_GP2_COUNT is therefore RT_GP_MENU, the first slot past the sixteen, and
 * the compiled-in defaults are the same table the first pad uses
 * (host/settings.cpp kGamepadBinds, read with this count): the two pads are
 * different devices, so the same name on both is not a collision. */
constexpr int RT_GP2_COUNT = RT_GP_MENU;

/* Rebindable mouse button slots. The first sixteen entries of RtKeyBind and
 * RtPadBind in the same order, and no menu slot: the menu hotkey is a key or
 * a pad button, never a mouse button, because the mouse is what drives the
 * menu's own pointer. There are no stick slots either; mouse motion is
 * mouse-look (input.mouse_look), not a bindable slot.
 *
 * RT_MB_SCREENSHOT is the one slot past the sixteen. It is a host hotkey,
 * not a DS2 button: unlike the menu key it is legal on the mouse, because it
 * does not have to compete with the pointer the menu itself draws. It ships
 * unbound. Everything that maps a slot to a pad bit stops at sixteen for
 * that reason; see kSlotBits in host/input.cpp. */
enum RtMouseBind {
    RT_MB_UP, RT_MB_DOWN, RT_MB_LEFT, RT_MB_RIGHT,
    RT_MB_CROSS, RT_MB_CIRCLE, RT_MB_SQUARE, RT_MB_TRIANGLE,
    RT_MB_L1, RT_MB_R1, RT_MB_L2, RT_MB_R2, RT_MB_L3, RT_MB_R3,
    RT_MB_START, RT_MB_SELECT,
    RT_MB_SCREENSHOT,
    RT_MB_COUNT
};

/* The console's configured language, which is the one OSD field this game
 * reads. Measured on SCES_507.60: the vendor routine at 0x00272958 (the
 * disc listing's sceScfGetLanguage, disc 0x00276818, 24 words and no masked
 * mismatch) calls the GetOsdConfigParam syscall, takes the version field at
 * bits 13..15, and for a nonzero version returns bits 16..20 of the same
 * word. kanbanBootMcCheck calls it at 0x001B9614 and maps the result
 * through a five-entry table (the `sltiu $3, $4, 0x5` at 0x001B9624 over
 * `language - 1`, also cited by address in guest/ico_syms.h) to pick which
 * set of pre-rendered subtitle images it loads; anything outside 1..5 keeps
 * the compiled-in default.
 *
 * The numbering is the SCE OSD one and the values below are those numbers,
 * so the value is the number the syscall reports rather than a translation
 * of it: 1 English, 2 French, 3 Spanish, 4 German, 5 Italian. Japanese (0)
 * and the two the OSD has past Italian are not offered: this disc's table
 * has five entries and nothing behind the other values.
 *
 * This is not a value the game supplied, and it is not a setting either.
 * A console answers this syscall from its own OSD settings, which this port
 * has no equivalent of; system.language was retired on 2026-09-05
 * (host/settings.cpp `load_retired`) and the value is compiled in at
 * English, because this disc puts its own language screen up when the card's
 * product block does not load and that screen is where the player chooses.
 * The struct field below says the same thing; nothing reads this enum from
 * a file. */
enum class RtLanguage {
    English = 1,
    French = 2,
    Spanish = 3,
    German = 4,
    Italian = 5,
};

/* Which of the four binding tables a slot index belongs to. Every helper
 * below that used to take a `bool gamepad` takes one of these. */
enum RtBindDevice {
    RT_BIND_KEYBOARD,
    RT_BIND_GAMEPAD,
    RT_BIND_GAMEPAD2,
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
/* Mailbox, and nothing else in a shipped run: display.present is no longer
         * a settings key. The swapchain present mode is still reachable for
         * scripts and CI through ICORECOMP_GS_PRESENT, which gs/gs_select.cpp
         * reads at its own call site; see docs/SETTINGS.md section 3. */
        RtPresentMode present = RtPresentMode::Mailbox;
        RtFit fit = RtFit::Letterbox;
        /* Output frame the scanout is built at: Crt is the renderer's
         * visible area for the video mode, which crops a window that
         * overruns it (ICO's attract movie); Window grows the frame to hold
         * the whole window. Presentation only, see docs/SETTINGS.md. */
        RtRaster raster = RtRaster::Window;
        /* Off by default: it is the one display key that reaches into guest
         * memory (one float of the game's own projection block, scaled at
         * the matrix composer's entry) and the retail picture is 4:3, so a
         * build with an untouched settings.json renders exactly what every
         * earlier build did. Window derives the factor from the presentation
         * surface's aspect and follows it across a resize; SixteenNine holds
         * 16/9. See guest/widescreen.cpp and docs/SETTINGS.md section 6. */
        RtWidescreen widescreen = RtWidescreen::Off;
        /* How the two fields of an interlaced scanout become one output
         * frame: Adaptive weaves the still parts and bobs the moving parts,
         * Bob shows each field on its own, Weave always pairs the two newest
         * fields. Presentation only, see docs/SETTINGS.md.
         *
         * Fixed at Bob and no longer a settings key: the attract movie is
         * interlaced video whose two fields are different moments, and bob
         * is the mode that shows it as the disc holds it (docs/SETTINGS.md
         * section 6). */
        RtDeinterlace deinterlace = RtDeinterlace::Bob;
        RtFilter filter = RtFilter::Linear;
        /* Host present rate in Hz: how often the window is refreshed with
         * the newest finished field. 0 (the default) is one present per
         * field, which is what the port did before this key existed; any
         * other value in [1, 1000] repeats the newest field at that rate
         * between fields. Host-side only in the strict sense: it moves no
         * guest tick and reshapes no input. It is not debug.fps_limit_hz,
         * which paces the guest's own field rate. See docs/SETTINGS.md
         * section 6.
         *
         * Fixed at 0 and no longer a settings key: one present per field is
         * what every shipped run does. The plumbing below it stays, so the
         * ring's own selftest still exercises a rate. */
        double present_rate = 0.0;
        /* 1/4/8/16, paraLLEl-GS SuperSampling. 4 and up also request
         * high-resolution scanout; there is no separate setting for it. */
        int render_scale = 1;
        bool show_fps = false;
        /* Where F12 writes its PNG. Empty (the default) means
         * <rt_base_dir()>/screenshots, with the per-user state directory as
         * the fallback when that is not writable; any absolute or relative
         * path is accepted as written. Host-side only: nothing the game
         * supplied is read or changed by it. See host/screenshot.cpp. */
        std::string screenshot_dir;
    } display;
    /* Host output gains. Every one of these is 0..100 and applies to the
     * host's own mix of the rendered audio, never to a value the game
     * supplied: no SPU2 register, no command word and no guest memory is
     * touched by any of them. master_volume and mute apply to the sum, at
     * the SDL sink (host/audio.cpp); the four category gains apply to the
     * samples of their own category where the engine sums them
     * (snd/engine.cpp), which is the only place the port knows which
     * category a sample belongs to. See docs/SETTINGS.md section 2, audio. */
    struct {
        int master_volume = 100;        /* 0..100, host output gain only */
        bool mute = false;
        /* The streamed ADPCM voices (SgStAdpcmOpen, snd/engine.cpp cmd
         * 0x3E): the game's music and ambience. */
        int music_volume = 100;
        /* Every other SPU2 voice, the ones keyed on from SPU RAM: sound
         * effects and character sounds. */
        int effects_volume = 100;
        /* The SgStPcm stream (snd/engine.cpp commands 0x46-0x4F), which in
         * this binary is the attract movie's audio and nothing else. */
        int movie_volume = 100;
        /* The achievement unlock chime the runtime synthesises itself
         * (snd/chime.h). Not the game's audio at all, which is why it is
         * quieter than the rest by default. */
        int chime_volume = 60;
    } audio;
    struct {
        std::string keyboard[RT_KB_COUNT];   /* SDL scancode names */
        std::string gamepad[RT_GP_COUNT];    /* SDL gamepad button/axis names */
        /* Player 2's pad: the same sixteen DS2 slots, no host hotkeys. Read
         * by the second SDL gamepad only, and reported on pad port 1, which
         * is the port the PAL disc's two-player mode opens for Yorda. */
        std::string gamepad2[RT_GP2_COUNT];
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
    /* What the console itself would tell the game about its own
     * configuration. Nothing here changes a value the game computed; it
     * answers a question the game asks the machine it is running on. */
    struct {
        /* The language the OSD reports (RtLanguage above). Fixed at
         * English and no longer a settings key: this disc puts its own
         * language screen up when the memory card's product block does not
         * load, and that screen, not a host setting, is where the player
         * chooses. English is also what this build falls back to when the
         * value it reads is outside its own five-entry table, so a run
         * behaves as the port always did. */
        RtLanguage language = RtLanguage::English;
    } system;
    /* The local achievement observer (guest/achievements.h). All three are
     * hot and all three are host-side: the observer reads the game's own
     * progress bits and writes nothing into guest memory.
     *
     * The chime's volume lives in audio (audio.chime_volume) with the rest
     * of the host output gains. The progress-bit diagnostic is no longer a
     * setting: it is always on, at info level (guest/achievements.cpp). */
    struct {
        bool enabled = true;
        bool toast = true;
        /* On, an unlock plays a chime the runtime synthesises itself
         * (snd/chime.h); no audio asset ships with this port. The chime is
         * summed into the samples handed to the device and into nothing
         * else: the game's own mix is not altered. */
        bool sound = false;
    } achievements;
    struct {
        bool log_file = true;
        int profile_fields = 180;       /* 0 disables; 180 is the pre-settings
                                         * ICORECOMP_PROFILE-unset default (prof.h
                                         * g_every), kept so an env-less run still
                                         * produces a diagnosable log */
        /* The guest field pacer's cap, in fields per second.
         *
         *   RT_FPS_LIMIT_MODE_RATE (-1, the default)
         *                 pace to the rate of the video mode the game
         *                 programmed: 59.94 on NTSC, 50 on PAL. The PAL
         *                 disc changes mode at the player's request, so a
         *                 fixed number here would be wrong on one of its
         *                 two settings.
         *   0             no pacing at all.
         *   [1, 1000]     that many fields per second, whatever the mode.
         */
        double fps_limit_hz = RT_FPS_LIMIT_MODE_RATE;
        /* The level a line has to reach to be logged. Warn is the shipped
         * default: it keeps everything that says the run did something
         * other than what was asked, and drops the startup and per-field
         * commentary that made a default log long enough to be unread. */
        RtLogLevel log_level = RT_LOG_WARN;
        /* Windows only: allocate a console for a double-clicked run and
         * echo the log to it. ico.exe is a GUI-subsystem binary, so off
         * means no console window at all; a run started from a shell
         * attaches to that shell's console either way. */
        bool console = false;
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

/* Reads only debug.console from the same file, by the same route and for
 * the same reason: rt_console_init has to decide whether to allocate a
 * console before rt_log_init opens the sink, which is before
 * rt_settings_init can run. Returns false (the default) when no file is
 * found, the file does not parse, "version" is not 1, or the key is absent
 * or not a boolean. */
bool rt_settings_peek_console();

/* Both peeks in one parse, which is what boot actually wants: rt_log_init
 * needs the pair, and calling the two above would resolve, read and
 * JSON-parse the same file twice before there is any logging to say so.
 * Either pointer may be null. The fallbacks are the ones the two singles
 * document: true for the log file, false for the console. */
void rt_settings_peek_boot(bool* log_file, bool* console);

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
 * A "" default is a real answer for RT_BIND_MOUSE, and for the screenshot
 * slot on the three devices that have one: every mouse slot but square and
 * r1 ships unbound, and the screenshot hotkey ships bound on the keyboard
 * only. Every other keyboard and gamepad slot has a non-empty default, and
 * RT_BIND_GAMEPAD2 reads the same table, so "" from one of those still means
 * the slot index was bad.
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
 * RT_GP2_COUNT / RT_MB_COUNT), and the index of its menu slot, or -1 when it
 * has none. The mouse has none because the mouse drives the pointer the menu
 * draws; player 2's pad has none because the menu is player 1's. Both return
 * the same shape for a bad device as the two accessors above: 0 slots, no
 * menu slot. Callers that walk a device's slots go through these rather than
 * switching on the enum themselves. */
int rt_settings_bind_slot_count(RtBindDevice device);
int rt_settings_bind_menu_slot(RtBindDevice device);

/* The index of a device's screenshot slot (the hotkey host/screenshot.cpp
 * reads), or -1 when it has none. Player 2's pad is the one device with
 * neither hotkey, because both are host hotkeys and the host is player 1's;
 * a bad device answers -1 the same way.
 *
 * Both hotkey slots are consumed in the event pump and never reach the
 * virtual pad, which is what the commit-time binding rules in settings.cpp
 * use these two accessors to know. */
int rt_settings_bind_screenshot_slot(RtBindDevice device);

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
 * (render scale) queue for rt_settings_apply_pending(), which
 * rt_gs_vsync_hook (hw/gspriv.cpp) calls at the next field boundary, because
 * their subsystem entry points fatal when called mid-frame. Volumes, fps cap
 * and profiler period need no applier: their consumers read rt_settings()
 * fresh on every use.
 * A setting whose env twin is set (rt_settings_overridden) is never
 * applied; the environment owns it for the life of the run. */
void rt_settings_apply(const RtSettings& before, const RtSettings& now);
void rt_settings_apply_pending();

/* The startup half of rt_settings_apply, called once from main.cpp after
 * rt_settings_init(). rt_settings_init runs no applier (a load has no
 * "before" to diff against), so a hot setting whose subsystem keeps its own
 * copy instead of reading rt_settings() fresh would otherwise never see the
 * value the file holds until the user edited it in the menu. Today that is
 * achievements.*; debug.log_level is the one main.cpp still pushes itself,
 * because it has to land before anything else can log. Safe before the
 * subsystems exist. */
void rt_settings_apply_loaded();

/* ---- the cold keys ------------------------------------------------------
 *
 * A cold key is one whose consumer reads it once, at process start, so a
 * change cannot reach a running process at all. There are two:
 * debug.console (a console cannot be attached to a process that has already
 * decided it has none; rt_console_init runs off rt_settings_peek_console
 * before the log sink exists) and debug.log_file (the sink is opened once,
 * off the same peek). host/settings_apply.cpp owns the hot/warm/cold
 * classification and lists both. display.backend was the third until it was
 * retired on 2026-09-05.
 *
 * Rather than leaving such a change to sit in the file until the user next
 * starts the program, the runtime restarts itself to apply it
 * (rt_restart_now in runtime.h). That is only legal before the guest is
 * running, so the pair below is the gate: main.cpp calls
 * rt_settings_set_gameplay_active(true) just before rt_sched_boot, and from
 * then on a commit that moved a cold key reverts it with a log, whoever
 * asked -- the menu disables those controls, but a script writing through
 * rt_settings_mutable() reaches the same refusal.
 *
 * launcher.* is cold for the current run as well and is deliberately not in
 * this class: those keys only decide what the next launch's launcher does,
 * and they are changed from the launcher's own window, where restarting to
 * apply "show this at startup" would throw away the screen the user is
 * looking at. They take effect at the next run, as they always did.
 */
void rt_settings_set_gameplay_active(bool active);
bool rt_settings_gameplay_active();

/* The first cold key whose value differs between two settings structs, as
 * its dotted name ("debug.console" or "debug.log_file"), or null when none
 * does. Pure: the caller passes the struct it committed from and the one it
 * committed to. ui/ui_settings_model.cpp uses it to decide whether a commit
 * has to restart the program. */
const char* rt_settings_cold_key_changed(const RtSettings& before, const RtSettings& now);

#endif /* ICORECOMP_HOST_SETTINGS_H */
