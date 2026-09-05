/* host/screenshot.cpp: see screenshot.h.
 *
 * Guarded like host/window.cpp and host/input.cpp: SDL is only in the
 * executable when this build has it (ICORECOMP_HAVE_SDL; see CMakeLists.txt).
 * Without it there is no window, no swapchain and so no presented picture,
 * and every function here is a no-op with no SDL calls compiled in.
 *
 * The capture goes through GsBackend both ways, so it reaches whichever
 * renderer is live: the arm rides the GS command ring, the pixels come back
 * off it (gs/gs_backend.h). The present rectangle it reports is the window
 * service's (host/window_service.h), published by whichever backend
 * presented.
 *
 * Reentrancy: rt_screenshot_on_sdl_event runs from rt_window_pump, which can
 * execute from inside Granite's WSI::begin_frame, so it records a flag and
 * does nothing else. Every backend call this file makes is in
 * rt_screenshot_tick, which runs from the vsync hook at the field boundary.
 */
#include "screenshot.h"

#include "../runtime.h"
#include "png_write.h"
#include "portable.h"
#include "settings.h"
#include "window.h"

#ifdef ICORECOMP_HAVE_SDL
#include "../gs/gs_backend.h"
#include "../gs/gs_parallel_api.h"
#include "mouse_names.h"
#include "window_service.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>
#endif

#ifdef ICORECOMP_HAVE_SDL

namespace fs = std::filesystem;

namespace {

using ShotClock = std::chrono::steady_clock;

/* An axis bound to the hotkey counts as pressed past the same raw value
 * host/input.cpp uses for a trigger bound to a DS2 button, so a trigger reads
 * the same whichever of the two it is bound to. */
constexpr Sint16 kAxisPress = 8192;

/* How long a capture may stay armed with nothing coming back before this file
 * gives up on it. A capture is consumed by the first field that presents a
 * picture, so the only way to sit here is for no such field to arrive: a
 * minimized window, or a swapchain format drain_screenshots refused (it logs
 * its own reason on the library side). Two seconds is about 120 fields. */
constexpr auto kArmTimeout = std::chrono::seconds(2);

/* Resolved hotkeys, rebuilt when rt_settings_generation() moves, exactly as
 * host/input.cpp rebuilds its pad tables. */
unsigned g_tables_gen = 0;
SDL_Scancode g_key = SDL_SCANCODE_UNKNOWN;
SDL_GamepadButton g_pad_button = SDL_GAMEPAD_BUTTON_INVALID;
SDL_GamepadAxis g_pad_axis = SDL_GAMEPAD_AXIS_INVALID;
int g_pad_axis_dir = 0;
bool g_pad_axis_down = false;
bool g_mouse_bound = false;
RtMouseInput g_mouse_input = RT_MOUSE_LEFT;
std::string g_hotkey_line; /* the last line logged, so a commit that changed nothing is quiet */

/* Request state. g_requested is set from the pump; the tick turns it into an
 * arm and then waits for the pixels. */
bool g_requested = false;
bool g_armed = false;
uint32_t g_armed_slots = 1;
ShotClock::time_point g_armed_at;
bool g_no_backend_logged = false;
/* The first capture of a run takes two copies of the field, one before the
 * overlay pass and one after, and compares them. That comparison is what
 * settles by measurement that the file this feature writes has no overlay
 * in it. It used to be gated on a verbose channel, which meant the claim
 * rested on a run nobody makes; it now fires on its own, exactly once, and
 * is bounded by this flag. Only one file is ever written either way: the
 * second copy is compared and dropped. */
bool g_overlay_checked = false;

/* Resolved capture folder, keyed on the display.screenshot_dir value it was
 * resolved for, so a change from the menu takes effect at once (the key is
 * hot) and an uncreatable folder is logged once per value rather than once a
 * capture. Empty means there is nowhere writable and captures are skipped. */
std::string g_dir;
std::string g_dir_source;
bool g_dir_resolved = false;

/* ---- hotkey resolution ---------------------------------------------------- */

/* A gamepad binding is either an SDL button string or an SDL axis string with
 * a trailing '+' or '-' for the direction; the suffix is this port's
 * convention, not part of the SDL token, and settings.cpp's kGamepadBinds
 * spells it ("lefttrigger+"). Same grammar host/input.cpp reads for the pad
 * slots, kept to a button or an axis here: a chord is legal only in the menu
 * slot, and settings.cpp's rule 3 reverts one that reaches this slot. */
void resolve_gamepad(const std::string& name) {
    g_pad_button = SDL_GAMEPAD_BUTTON_INVALID;
    g_pad_axis = SDL_GAMEPAD_AXIS_INVALID;
    g_pad_axis_dir = 0;
    g_pad_axis_down = false;
    if (name.empty()) return; /* unbound, the shipped default */

    const char last = name.back();
    if (name.size() >= 2 && (last == '+' || last == '-')) {
        const std::string token = name.substr(0, name.size() - 1);
        const SDL_GamepadAxis axis = SDL_GetGamepadAxisFromString(token.c_str());
        if (axis != SDL_GAMEPAD_AXIS_INVALID) {
            g_pad_axis = axis;
            g_pad_axis_dir = (last == '+') ? 1 : -1;
            return;
        }
    }
    const SDL_GamepadButton button = SDL_GetGamepadButtonFromString(name.c_str());
    if (button != SDL_GAMEPAD_BUTTON_INVALID) {
        g_pad_button = button;
        return;
    }
    rt_log_warn("screenshot", "input.gamepad.screenshot = \"%s\" is not an SDL gamepad button or axis"
        " name; the screenshot has no gamepad binding this run", name.c_str());
}

void rebuild_hotkeys() {
    const RtSettings& s = rt_settings();

    /* A name that does not resolve leaves the device unbound and says so,
     * rather than falling back to the compiled default the way a pad slot
     * does. There is nothing to fall back to that is worth taking: two of the
     * three defaults are "" already, and silently restoring F12 would hide
     * the typo the user has to fix. */
    const std::string& kb = s.input.keyboard[RT_KB_SCREENSHOT];
    g_key = kb.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(kb.c_str());
    if (!kb.empty() && g_key == SDL_SCANCODE_UNKNOWN) {
        rt_log_warn("screenshot", "input.keyboard.screenshot = \"%s\" is not an SDL scancode name;"
            " the screenshot has no keyboard binding this run", kb.c_str());
    }

    resolve_gamepad(s.input.gamepad[RT_GP_SCREENSHOT]);

    const std::string& mb = s.input.mouse[RT_MB_SCREENSHOT];
    g_mouse_bound = rt_mouse_input_from_name(mb, &g_mouse_input);
    if (!mb.empty() && !g_mouse_bound) {
        rt_log_warn("screenshot", "input.mouse.screenshot = \"%s\" is not a mouse input name;"
            " the screenshot has no mouse binding this run", mb.c_str());
    }

    const char* key_name = (g_key != SDL_SCANCODE_UNKNOWN) ? SDL_GetScancodeName(g_key) : nullptr;
    std::string pad_name = "unbound";
    if (g_pad_button != SDL_GAMEPAD_BUTTON_INVALID) {
        const char* n = SDL_GetGamepadStringForButton(g_pad_button);
        pad_name = n ? n : "unbound";
    } else if (g_pad_axis != SDL_GAMEPAD_AXIS_INVALID) {
        const char* n = SDL_GetGamepadStringForAxis(g_pad_axis);
        pad_name = std::string(n ? n : "") + (g_pad_axis_dir > 0 ? "+" : "-");
    }
    std::string line = std::string("screenshot hotkey: keyboard ")
        + ((key_name && key_name[0]) ? key_name : "unbound")
        + ", gamepad " + pad_name
        + ", mouse " + (g_mouse_bound ? rt_mouse_input_name(g_mouse_input) : "unbound");
    /* One line per distinct resolution, not one per commit: the settings
     * generation moves on every commit, and most commits touch nothing here. */
    if (line != g_hotkey_line) {
        g_hotkey_line = line;
        rt_log_info("screenshot", "%s", line.c_str());
    }
}

void sync_hotkeys() {
    const unsigned gen = rt_settings_generation();
    if (gen == g_tables_gen) return;
    g_tables_gen = gen;
    rebuild_hotkeys();
}

/* ---- folder and file name ------------------------------------------------- */

/* screenshots/ next to the executable, which .gitignore and
 * tools/check_no_rom.sh both refuse outright: the pixels are the game's, so a
 * capture must not be committable. Same shape as ui/title_logo.cpp's
 * cache_path, with the per-user state directory as the fallback for an
 * installation whose own folder is read only, and display.screenshot_dir
 * ahead of both when the user named one.
 *
 * Returns "" when there is nowhere to write. Never fatal: the reason is
 * logged once per distinct setting value and the capture is skipped. */
const char* capture_dir() {
    const std::string& want = rt_settings().display.screenshot_dir;
    if (g_dir_resolved && g_dir_source == want) return g_dir.c_str();
    g_dir_resolved = true;
    g_dir_source = want;
    g_dir.clear();

    std::error_code ec;
    if (!want.empty()) {
        fs::create_directories(want, ec);
        std::error_code ec2;
        if (fs::is_directory(want, ec2)) {
            g_dir = want;
            rt_log_info("screenshot", "screenshots go to '%s' (display.screenshot_dir)", g_dir.c_str());
            return g_dir.c_str();
        }
        rt_log_warn("screenshot", "display.screenshot_dir = '%s' is not a directory and could not be"
            " created (%s); falling back to the default location", want.c_str(),
            ec ? ec.message().c_str() : "no such directory");
    }

    const std::string beside = std::string(rt_base_dir()) + "/screenshots";
    ec.clear();
    fs::create_directories(beside, ec);
    std::error_code ec2;
    if (fs::is_directory(beside, ec2)) {
        g_dir = beside;
        rt_log_info("screenshot", "screenshots go to '%s'", g_dir.c_str());
        return g_dir.c_str();
    }

    const std::string user = rt_user_state_dir();
    if (!user.empty()) {
        const std::string alt = user + "/screenshots";
        std::error_code ec3;
        fs::create_directories(alt, ec3);
        std::error_code ec4;
        if (fs::is_directory(alt, ec4)) {
            g_dir = alt;
            rt_log_warn("screenshot", "screenshots: '%s' is not writable; writing to '%s' instead",
                beside.c_str(), g_dir.c_str());
            return g_dir.c_str();
        }
    }

    rt_log_warn("screenshot", "no writable screenshot folder ('%s' and the per-user state directory"
        " both failed); captures are skipped this run", beside.c_str());
    return "";
}

/* ico-YYYYMMDD-HHMMSS, local time, with -2, -3 and so on appended until no
 * file of that stem exists. The stem is checked against every name the caller
 * is about to write (the plain one, or the -pre/-post pair), so a pair never
 * half-collides with an earlier capture. */
std::string unique_stem(const std::string& dir) {
    const std::time_t now = std::time(nullptr);
    std::tm tm = {};
    rt_localtime(now, &tm);
    /* 80, not 32: the six std::tm fields are ints, so GCC sizes each
     * conversion at the whole int range (11 characters) and 32 is a
     * truncation it can prove. */
    char base[80];
    std::snprintf(base, sizeof(base), "ico-%04d%02d%02d-%02d%02d%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    for (int n = 1; n <= 999; ++n) {
        std::string stem = base;
        if (n > 1) stem += "-" + std::to_string(n);
        std::error_code ec;
        if (!fs::exists(dir + "/" + stem + ".png", ec)) return stem;
    }
    /* A thousand captures in the same second is not a case worth a special
     * answer; the last stem is reused and the file is overwritten. */
    return std::string(base) + "-999";
}

/* ---- writing -------------------------------------------------------------- */

/* RGBA8 in, RGB out. The alpha the swapchain carries is whatever the scanout
 * blit and the clear left in it, and it is not a transparency the user asked
 * for: what they are looking at is opaque. Writing it would produce a PNG
 * that most viewers show as see-through or black. Dropped here for the same
 * reason rt_gs_write_scanout_ppm drops it. */
std::vector<uint8_t> rgba_to_rgb(const std::vector<uint8_t>& rgba) {
    const size_t texels = rgba.size() / 4;
    std::vector<uint8_t> rgb(texels * 3);
    for (size_t i = 0; i < texels; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    return rgb;
}

/* Consumes whatever a slot holds and throws it away. An image left in a slot
 * outlives the arm that produced it and would be handed to the next capture,
 * which would write a scene from minutes ago under the current timestamp. Two
 * callers: the arm path, before a new request goes out, and the timeout path
 * that gives up on one. */
void discard_slot(GsBackend* gs, uint32_t slot) {
    uint32_t w = 0, h = 0;
    const size_t need = gs->take_screenshot(slot, &w, &h, nullptr, 0);
    if (need == 0) return;
    std::vector<uint8_t> scratch(need, 0);
    gs->take_screenshot(slot, &w, &h, scratch.data(), scratch.size());
    rt_log_debug("screenshot", "slot %u still held a %ux%u image from an abandoned arm; discarded",
        slot, w, h);
}

void discard_slots(GsBackend* gs) {
    for (uint32_t slot = 0; slot < RT_PGS_SHOT_SLOTS; ++slot) discard_slot(gs, slot);
}

/* Pulls one slot. Returns false when the slot holds nothing (the usual answer
 * on a field the capture has not landed on yet). */
bool pull_slot(GsBackend* gs, uint32_t slot, std::vector<uint8_t>* rgba, uint32_t* w, uint32_t* h) {
    const size_t need = gs->take_screenshot(slot, w, h, nullptr, 0);
    if (need == 0) return false;
    rgba->assign(need, 0);
    const size_t got = gs->take_screenshot(slot, w, h, rgba->data(), rgba->size());
    if (got != need) {
        rt_log_warn("screenshot", "slot %u reported %zu bytes and then handed back %zu; capture dropped",
            slot, need, got);
        /* The slot is still holding that image; drop it here rather than
         * leaving it to be served as some later capture. */
        discard_slot(gs, slot);
        return false;
    }
    return true;
}

void write_png(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h,
               double ms) {
    const std::vector<uint8_t> rgb = rgba_to_rgb(rgba);
    char err[256] = {0};
    if (!rt_png_write(path.c_str(), w, h, rgb.data(), 3, err, sizeof(err))) {
        rt_log_warn("screenshot", "could not write %s: %s", path.c_str(), err);
        return;
    }
    rt_log_info("screenshot", "wrote %s (%ux%u, %.1f ms)", path.c_str(), w, h, ms);
}

} // namespace

bool rt_screenshot_on_sdl_event(const SDL_Event& e) {
    sync_hotkeys();
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat) break;
        if (g_key != SDL_SCANCODE_UNKNOWN && e.key.scancode == g_key) {
            rt_screenshot_request();
            return true;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (g_pad_button != SDL_GAMEPAD_BUTTON_INVALID
            && SDL_GamepadButton(e.gbutton.button) == g_pad_button) {
            rt_screenshot_request();
            return true;
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (g_pad_axis == SDL_GAMEPAD_AXIS_INVALID || SDL_GamepadAxis(e.gaxis.axis) != g_pad_axis) break;
        /* Edge, not level: an axis held past the press point must take one
         * capture, not one a field. */
        const bool down = (g_pad_axis_dir > 0) ? (e.gaxis.value > kAxisPress)
                                               : (e.gaxis.value < -kAxisPress);
        if (down && !g_pad_axis_down) rt_screenshot_request();
        g_pad_axis_down = down;
        /* Not consumed: nothing else in the runtime reads a gamepad axis
         * event (host/input.cpp polls axis state instead), and ui_events.cpp
         * returns false for these for the same reason. */
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (!g_mouse_bound) break;
        RtMouseInput pressed;
        switch (e.button.button) {
        case SDL_BUTTON_LEFT:   pressed = RT_MOUSE_LEFT; break;
        case SDL_BUTTON_RIGHT:  pressed = RT_MOUSE_RIGHT; break;
        case SDL_BUTTON_MIDDLE: pressed = RT_MOUSE_MIDDLE; break;
        case SDL_BUTTON_X1:     pressed = RT_MOUSE_X1; break;
        case SDL_BUTTON_X2:     pressed = RT_MOUSE_X2; break;
        default: return false;
        }
        if (pressed == g_mouse_input) {
            rt_screenshot_request();
            return true;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (!g_mouse_bound) break;
        if (g_mouse_input != RT_MOUSE_WHEEL_UP && g_mouse_input != RT_MOUSE_WHEEL_DOWN) break;
        /* One capture per event, however many ticks it carries: a flicked
         * wheel would otherwise queue captures the player could not have
         * meant, which is the same hazard host/input.cpp caps its wheel
         * press queue for. */
        const bool up = e.wheel.y > 0.0f;
        const bool down = e.wheel.y < 0.0f;
        if ((up && g_mouse_input == RT_MOUSE_WHEEL_UP)
            || (down && g_mouse_input == RT_MOUSE_WHEEL_DOWN)) {
            rt_screenshot_request();
            return true;
        }
        break;
    }
    default:
        break;
    }
    return false;
}

void rt_screenshot_request() {
    g_requested = true;
}

void rt_screenshot_tick() {
    sync_hotkeys();

    if (g_requested && !g_armed) {
        g_requested = false;
        GsBackend* gs = rt_gs_backend_if_created();
        if (!gs) {
            /* Nothing to capture out of yet: the launcher has not handed over
             * and no backend exists. rt_gs_backend() is deliberately not
             * called, for the reason gs_backend.h gives. */
            if (!g_no_backend_logged) {
                g_no_backend_logged = true;
                rt_log_warn("screenshot", "a screenshot was asked for before the GS backend existed;"
                    " nothing to capture yet");
            }
        } else {
            /* The first capture of the run asks for both slots so the
             * overlay comparison below can be taken; every later one asks
             * for the one slot it writes. See gs_parallel_api.h's
             * screenshot section for what the two arm points are. */
            g_armed_slots = g_overlay_checked ? 1u : RT_PGS_SHOT_SLOTS;
            g_overlay_checked = true;
            /* Before the arm, not after: the request rides the GS command
             * ring and the library clears the slots when it arrives, which is
             * one or more fields from now, while the pull below runs in this
             * same call. Anything a slot is holding at this moment belongs to
             * an arm that was abandoned, and this capture must not be served
             * it. */
            discard_slots(gs);
            gs->request_screenshot(g_armed_slots);
            g_armed = true;
            g_armed_at = ShotClock::now();
            rt_log_debug("screenshot", "armed %u slot(s) through the GS backend", g_armed_slots);
        }
    }

    if (!g_armed) return;

    /* The same backend the arm went through, so the native renderer's own
     * slots come back by this route too (gs_backend.h). It cannot be null
     * here: the arm above created it. */
    GsBackend* gs = rt_gs_backend_if_created();
    if (!gs) {
        g_armed = false;
        return;
    }

    std::vector<uint8_t> pre;
    uint32_t w = 0, h = 0;
    if (!pull_slot(gs, RT_PGS_SHOT_PRE, &pre, &w, &h)) {
        if (ShotClock::now() - g_armed_at > kArmTimeout) {
            g_armed = false;
            /* The library's arm may still be live and may still publish. The
             * slots are emptied here and again at the next arm, so an image
             * that lands after this point is never served as a later
             * capture. */
            discard_slots(gs);
            rt_log_warn("screenshot", "the capture armed %.1f s ago never came back; no field presented"
                " a picture, or the library refused the swapchain format (its own log line says"
                " which)", std::chrono::duration<double>(ShotClock::now() - g_armed_at).count());
        }
        return;
    }
    g_armed = false;

    const char* dir = capture_dir();
    /* Empty means nowhere writable; capture_dir has already said why, once
     * for this setting value. Never fatal, the capture is dropped. */
    if (!dir[0]) return;

    const bool pair = g_armed_slots >= RT_PGS_SHOT_SLOTS;
    const std::string stem = unique_stem(dir);
    const double ms = std::chrono::duration<double, std::milli>(ShotClock::now() - g_armed_at).count();

    /* One file per capture, always, and always the pre-overlay copy: that is
     * the picture the player asked for. */
    write_png(std::string(dir) + "/" + stem + ".png", pre, w, h, ms);
    if (!pair) return;

    std::vector<uint8_t> post;
    uint32_t pw = 0, ph = 0;
    if (!pull_slot(gs, RT_PGS_SHOT_POST, &post, &pw, &ph)) {
        rt_log_warn("screenshot", "the post-overlay copy of the same field was not published, so the"
            " overlay check could not be taken; the file itself was written");
        return;
    }

    /* The measurement the pair exists for. The second copy is not written:
     * it is read, compared and dropped, so the folder holds one file per
     * capture and the log carries the claim. With the menu closed the two
     * copies of one field must be byte identical, and with the menu open
     * they must differ. */
    size_t differing = 0;
    if (pre.size() != post.size() || w != pw || h != ph) {
        rt_log_info("screenshot", "the two copies of one field are %ux%u and %ux%u, which they never"
            " should be: both copy the same present rectangle", w, h, pw, ph);
    } else {
        for (size_t i = 0; i < pre.size(); ++i) {
            if (pre[i] != post[i]) ++differing;
        }
        rt_log_info("screenshot", "overlay check, once per run: the copy taken before the overlay pass"
            " and the copy taken after it are %s (%zu of %zu bytes differ); identical means no overlay"
            " was drawn on this field, differing means one was and the file written has none of it",
            differing == 0 ? "byte identical" : "different", differing, pre.size());
    }

    /* And the other half of the claim: the file is the present rectangle. Read
     * now rather than at the capture, which is one or two fields back, so the
     * line says which it is. */
    int32_t rx = 0, ry = 0, rw = 0, rh = 0, bw = 0, bh = 0;
    rt_window_present_rect(&rx, &ry, &rw, &rh, &bw, &bh);
    rt_log_debug("screenshot", "png %ux%u; the present rectangle reads %dx%d at (%d,%d) in a %dx%d"
        " backbuffer now, %s the capture", w, h, rw, rh, rx, ry, bw, bh,
        (rw == int32_t(w) && rh == int32_t(h)) ? "the same size as" : "a different size from");
}

#else /* !ICORECOMP_HAVE_SDL */

/* No window and no swapchain in this build, so there is no presented picture
 * to capture. The three entry points stay so their callers need no guard. */
bool rt_screenshot_on_sdl_event(const SDL_Event&) { return false; }
void rt_screenshot_tick() {}
void rt_screenshot_request() {}

#endif /* ICORECOMP_HAVE_SDL */
