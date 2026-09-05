/* host/settings_selftest.cpp: standalone exercise of the JSON DOM
 * (host/json.cpp) and the typed settings model (host/settings.cpp).
 *
 * Links json.cpp + settings.cpp against stub logging and rt_base_dir
 * (below); those are the only runtime.h externs settings.cpp calls. rt_base_dir()
 * returns a scratch directory this file creates and clears at start, taken
 * from ICORECOMP_SETTINGS_SELFTEST_DIR or defaulting to
 * "./settings-selftest-scratch". Each settings-layer test case points
 * ICORECOMP_SETTINGS at its own file under that directory and calls
 * rt_settings_init() to (re)load. Run:
 *
 *     ICORECOMP_SETTINGS_SELFTEST_DIR=/tmp/scratch ./icorecomp-settings-selftest
 *
 * Exit code 0 = every check passed; 2 on the first failing CHECK.
 */
#include "host/json.h"
#include "host/mouse_names.h"
#include "host/settings.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

/* ---- runtime stubs -------------------------------------------------------- */

namespace {
std::string g_base_dir = ".";
/* Every line rt_log has been handed since the last log_clear(). The
 * bad-value policy is half log line and half kept value, and a test that
 * only checks the kept value would pass just as well if the line naming the
 * key and the range were never emitted. */
std::string g_log;
} // namespace

/* The runtime's four level entry points, all onto the same recorded line.
 * A level filter here would be a second implementation of the thing under
 * test; what the cases check is the text, and every case that checks a
 * rejection is checking a warn line. */
void rt_log_line(const char* component, const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    std::printf("[%s] %s\n", component, buf);
    g_log += buf;
    g_log += '\n';
}

void rt_log_error(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_warn(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_info(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_debug(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}

/* settings.cpp names the level in a revert message and the applier stub
 * below never runs, so these two are all log.cpp owes this binary. */
const char* rt_log_level_name(RtLogLevel level) {
    switch (level) {
    case RT_LOG_DEBUG: return "debug";
    case RT_LOG_INFO:  return "info";
    case RT_LOG_WARN:  return "warn";
    case RT_LOG_ERROR: return "error";
    }
    return "warn";
}

std::atomic<int> g_rt_log_level{(int)RT_LOG_WARN};

const char* rt_base_dir() {
    return g_base_dir.c_str();
}

/* rt_settings_commit() (settings.cpp) calls rt_settings_apply(before, now)
 * after validation; the real implementation (settings_apply.cpp) pushes
 * changes into window control and the GS backend, which this selftest
 * deliberately does not link -- see the file comment above: JSON + the
 * settings model only, stubbed against exactly two runtime.h externs. A
 * stub here is simpler than linking settings_apply.cpp, which would pull in
 * window.cpp and gs_parallel.cpp/gs_select.cpp (SDL, the GS backend, hw.h)
 * for no benefit to what this target verifies. */
void rt_settings_apply(const RtSettings&, const RtSettings&) {}
void rt_settings_apply_pending() {}

/* ---- test harness ---------------------------------------------------------- */

namespace {

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void unset_env(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

std::string read_file(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string s;
    if (sz > 0) {
        s.resize((size_t)sz);
        size_t n = std::fread(s.data(), 1, (size_t)sz, f);
        s.resize(n);
    }
    std::fclose(f);
    return s;
}

void write_file(const std::string& path, const std::string& text) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::printf("[test] could not write %s\n", path.c_str());
        std::exit(2);
    }
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

bool json_equal(const RtJson& a, const RtJson& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
    case RtJson::Type::Null: return true;
    case RtJson::Type::Bool: return a.boolean == b.boolean;
    case RtJson::Type::Number: return a.number == b.number;
    case RtJson::Type::String: return a.str == b.str;
    case RtJson::Type::Array:
        if (a.arr.size() != b.arr.size()) return false;
        for (size_t i = 0; i < a.arr.size(); ++i) {
            if (!json_equal(a.arr[i], b.arr[i])) return false;
        }
        return true;
    case RtJson::Type::Object:
        if (a.obj.size() != b.obj.size()) return false;
        for (size_t i = 0; i < a.obj.size(); ++i) {
            if (a.obj[i].first != b.obj[i].first) return false;
            if (!json_equal(a.obj[i].second, b.obj[i].second)) return false;
        }
        return true;
    }
    return false;
}

void log_clear() {
    g_log.clear();
}

bool log_has(const char* needle) {
    return g_log.find(needle) != std::string::npos;
}

std::string nested_arrays(int n) {
    std::string s(size_t(n), '[');
    s += "1";
    s += std::string(size_t(n), ']');
    return s;
}

} // namespace

#define CHECK(expr) do { \
    if (!(expr)) { \
        std::printf("[test] FAIL at line %d: %s\n", __LINE__, #expr); \
        std::exit(2); \
    } \
} while (0)

int main() {
    const char* dir_env = std::getenv("ICORECOMP_SETTINGS_SELFTEST_DIR");
    std::string scratch = (dir_env && *dir_env) ? dir_env : "./settings-selftest-scratch";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);
    if (ec) {
        std::printf("[test] could not create scratch dir %s: %s\n", scratch.c_str(), ec.message().c_str());
        return 2;
    }
    g_base_dir = scratch;
    unset_env("ICORECOMP_SETTINGS");
    unset_env("ICORECOMP_FPS_LIMIT");

    /* ==== JSON layer ==== */

    { /* 1. round trip of a nested doc */
        const char* text =
            "{\n"
            "  \"obj\": {\"nested\": true, \"arr\": [1, 2.5, null, \"unicode: \\u00e9\", 59.94]},\n"
            "  \"flag\": false\n"
            "}\n";
        RtJson doc1, doc2;
        std::string err;
        CHECK(rt_json_parse(text, &doc1, &err));
        std::string written = rt_json_write(doc1);
        CHECK(rt_json_parse(written, &doc2, &err));
        CHECK(json_equal(doc1, doc2));
        CHECK(written.find("59.94") != std::string::npos);
    }
    { /* 2. error position */
        RtJson doc;
        std::string err;
        CHECK(!rt_json_parse("{\n  \"a\": tru\n}", &doc, &err));
        CHECK(err.rfind("2:", 0) == 0);
    }
    { /* 3. rejects */
        auto rejects = [](const char* text) {
            RtJson doc;
            std::string err;
            CHECK(!rt_json_parse(text, &doc, &err));
        };
        rejects("[1, 2,]");
        rejects("{\"a\": 1,}");
        rejects("// comment\n{}");
        rejects("[NaN]");
        rejects("[Infinity]");
        rejects("true false");
        rejects("\"\\udc00\"");
        rejects("{\"a\":1,\"a\":2}");
    }
    { /* 4. surrogate pair decode, control char escape on write */
        RtJson doc;
        std::string err;
        CHECK(rt_json_parse("\"\\ud83d\\ude00\"", &doc, &err));
        CHECK(doc.type == RtJson::Type::String);
        CHECK(doc.str.size() == 4);
        const unsigned char expected[4] = {0xF0, 0x9F, 0x98, 0x80};
        CHECK(std::memcmp(doc.str.data(), expected, 4) == 0);

        RtJson ctrl = RtJson::make_string(std::string(1, '\x01'));
        std::string out = rt_json_write(ctrl);
        CHECK(out.find("\\u0001") != std::string::npos);
    }
    { /* 5. depth */
        RtJson doc;
        std::string err;
        CHECK(!rt_json_parse(nested_arrays(100), &doc, &err));
        CHECK(rt_json_parse(nested_arrays(32), &doc, &err));
    }

    /* ==== Settings layer ==== */

    { /* 6. fresh path (no file): defaults, then a clean first save */
        std::string path = scratch + "/fresh.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        const RtSettings& s = rt_settings();
        CHECK(s.audio.master_volume == 100);
        /* The four category gains, with the chime the one that is not 100:
         * it is a host sound summed on top of a mix that is already using
         * the range. */
        CHECK(s.audio.music_volume == 100);
        CHECK(s.audio.effects_volume == 100);
        CHECK(s.audio.movie_volume == 100);
        CHECK(s.audio.chime_volume == 60);
        CHECK(s.display.window_width == 1280 && s.display.window_height == 960);
        CHECK(s.debug.fps_limit_hz == RT_FPS_LIMIT_MODE_RATE);
        CHECK(s.input.keyboard[RT_KB_CROSS] == "X");
        CHECK(s.input.gamepad[RT_GP_L2] == "lefttrigger+");
        CHECK(s.gameplay.run_any_direction == false);
        CHECK(rt_settings_save());
        CHECK(std::filesystem::exists(path));
        CHECK(!std::filesystem::exists(path + ".tmp"));
        RtJson doc;
        std::string err;
        CHECK(rt_json_parse(read_file(path), &doc, &err));
        const RtJson* ver = doc.find("version");
        CHECK(ver && ver->type == RtJson::Type::Number && ver->number == 1.0);
    }
    { /* 7. round trip through mutate/commit/reinit */
        std::string path = scratch + "/roundtrip.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        RtSettings& m = rt_settings_mutable();
        m.display.mode = RtDisplayMode::FullscreenDesktop;
        m.audio.master_volume = 55;
        m.input.left_deadzone = 0.25f;
        m.debug.fps_limit_hz = 0.0;
        m.display.render_scale = 4;
        m.input.keyboard[RT_KB_MENU] = "F2";
        m.input.gamepad[RT_GP_MENU] = "back+start";
        m.input.gamepad2[RT_GP_CROSS] = "b";
        m.input.gamepad2[RT_GP_CIRCLE] = "a";
        m.audio.music_volume = 40;
        m.audio.chime_volume = 0;
        m.gameplay.run_any_direction = true;
        rt_settings_commit();

        rt_settings_init();
        const RtSettings& s = rt_settings();
        CHECK(s.display.mode == RtDisplayMode::FullscreenDesktop);
        CHECK(s.audio.master_volume == 55);
        CHECK(s.input.left_deadzone == 0.25f);
        CHECK(s.debug.fps_limit_hz == 0.0);
        CHECK(s.display.render_scale == 4);
        CHECK(s.input.keyboard[RT_KB_MENU] == "F2");
        CHECK(s.input.gamepad[RT_GP_MENU] == "back+start");
        /* Player 2's table writes and reads back under input.gamepad2, and
         * a swap between two of its slots is not a duplicate. */
        CHECK(s.input.gamepad2[RT_GP_CROSS] == "b");
        CHECK(s.input.gamepad2[RT_GP_CIRCLE] == "a");
        /* Player 1's pad kept the names player 2 swapped: the two are
         * different devices and neither commit rule reaches across them. */
        CHECK(s.input.gamepad[RT_GP_CROSS] == "a");
        CHECK(s.input.gamepad[RT_GP_CIRCLE] == "b");
        CHECK(s.audio.music_volume == 40);
        CHECK(s.audio.chime_volume == 0);
        CHECK(s.gameplay.run_any_direction == true);
    }
    { /* 7b. The retired keys. Every one of them is left in the file exactly
       * as it was, named once at info, and read by nothing: the compiled-in
       * value is what the run uses. Settings handling is never fatal and a
       * stale key is the ordinary state of a file an older build wrote. */
        std::string path = scratch + "/retired.json";
        write_file(path,
            "{\n"
            "  \"version\": 1,\n"
            "  \"display\": {\"present\": \"fifo\", \"present_rate\": 144,"
            " \"deinterlace\": \"weave\"},\n"
            "  \"system\": {\"language\": \"german\"},\n"
            "  \"achievements\": {\"sound_volume\": 25, \"log_progress_bits\": true},\n"
            "  \"debug\": {\"verbose\": \"cdvd\"}\n"
            "}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        log_clear();
        rt_settings_init();
        const RtSettings& s = rt_settings();
        CHECK(s.display.present == RtPresentMode::Mailbox);
        CHECK(s.display.present_rate == 0.0);
        CHECK(s.display.deinterlace == RtDeinterlace::Bob);
        CHECK(s.system.language == RtLanguage::English);
        CHECK(s.audio.chime_volume == 60);
        /* One line each, naming the key. */
        CHECK(log_has("display.present is no longer read"));
        CHECK(log_has("display.present_rate is no longer read"));
        CHECK(log_has("display.deinterlace is no longer read"));
        CHECK(log_has("system.language is no longer read"));
        CHECK(log_has("achievements.sound_volume is no longer read"));
        CHECK(log_has("achievements.log_progress_bits is no longer read"));
        CHECK(log_has("debug.verbose is no longer read"));
        /* Not an unknown key: the loader still knows the name, it just does
         * not read it, so there is no second line about it. */
        CHECK(!log_has("unknown key \"display.present\""));

        /* A save keeps every one of them, untouched, and adds none of them
         * back to what this build writes. */
        CHECK(rt_settings_save());
        std::string text = read_file(path);
        CHECK(text.find("\"present\": \"fifo\"") != std::string::npos);
        CHECK(text.find("\"present_rate\": 144") != std::string::npos);
        CHECK(text.find("\"deinterlace\": \"weave\"") != std::string::npos);
        CHECK(text.find("\"language\": \"german\"") != std::string::npos);
        CHECK(text.find("\"sound_volume\": 25") != std::string::npos);
        CHECK(text.find("\"log_progress_bits\": true") != std::string::npos);
        CHECK(text.find("\"verbose\": \"cdvd\"") != std::string::npos);
    }
    { /* 7c. A file with no retired key says nothing, and a fresh save
       * writes none of them: the "system" section is not written at all
       * now that system.language is gone. */
        std::string path = scratch + "/no-retired.json";
        write_file(path, "{\"version\": 1}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        log_clear();
        rt_settings_init();
        CHECK(!log_has("is no longer read"));
        CHECK(rt_settings_save());
        std::string text = read_file(path);
        CHECK(text.find("\"present\"") == std::string::npos);
        CHECK(text.find("\"present_rate\"") == std::string::npos);
        CHECK(text.find("\"deinterlace\"") == std::string::npos);
        CHECK(text.find("\"system\"") == std::string::npos);
        CHECK(text.find("\"sound_volume\"") == std::string::npos);
        CHECK(text.find("\"log_progress_bits\"") == std::string::npos);
        CHECK(text.find("\"verbose\"") == std::string::npos);
        /* What it does write: the four audio gains. */
        CHECK(text.find("\"music_volume\": 100") != std::string::npos);
        CHECK(text.find("\"effects_volume\": 100") != std::string::npos);
        CHECK(text.find("\"movie_volume\": 100") != std::string::npos);
        CHECK(text.find("\"chime_volume\": 60") != std::string::npos);
    }
    { /* 8. unknown-key preservation across load/save */
        std::string path = scratch + "/unknown.json";
        write_file(path,
            "{\n"
            "  \"version\": 1,\n"
            "  \"display\": {\"foo\": 42},\n"
            "  \"gameplay\": {\"bar\": \"keep me\"},\n"
            "  \"custom_section\": {\"x\": 1}\n"
            "}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings_save());
        std::string text = read_file(path);
        CHECK(text.find("\"foo\"") != std::string::npos);
        CHECK(text.find("\"bar\"") != std::string::npos);
        CHECK(text.find("keep me") != std::string::npos);
        CHECK(text.find("custom_section") != std::string::npos);
    }
    { /* 9. bad value isolation: one field falls back, the rest loads */
        std::string path = scratch + "/badvalue.json";
        write_file(path,
            "{\n"
            "  \"version\": 1,\n"
            "  \"display\": {\"window_width\": -3},\n"
            "  \"audio\": {\"master_volume\": 55}\n"
            "}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        const RtSettings& s = rt_settings();
        CHECK(s.display.window_width == 1280);
        CHECK(s.audio.master_volume == 55);
    }
    { /* 10. bad enum falls back to its default */
        std::string path = scratch + "/badenum.json";
        write_file(path, "{\"version\": 1, \"display\": {\"raster\": \"warp\"}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.raster == RtRaster::Window);
    }
    { /* 11. version 2: defaults, save refused, file untouched */
        std::string path = scratch + "/version2.json";
        std::string original = "{\"version\": 2, \"display\": {\"window_width\": 800}}\n";
        write_file(path, original);
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.window_width == 1280);
        CHECK(!rt_settings_save());
        CHECK(read_file(path) == original);
    }
    { /* 12. malformed file: defaults, original bytes preserved in .bad */
        std::string path = scratch + "/malformed.json";
        std::string original = "{ nope";
        write_file(path, original);
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.window_width == 1280);
        CHECK(std::filesystem::exists(path + ".bad"));
        CHECK(read_file(path + ".bad") == original);
        /* The save target is still the broken file, so saving has to stay
         * off for the run: otherwise the next save replaces the file the
         * user has to fix with a defaults document, and "never overwritten
         * again" is not true. */
        CHECK(!rt_settings_save());
        CHECK(read_file(path) == original);
    }
    { /* 13. ICORECOMP_SETTINGS=- : defaults only, no file, save refused */
        set_env("ICORECOMP_SETTINGS", "-");
        rt_settings_init();
        CHECK(rt_settings().display.window_width == 1280);
        CHECK(!rt_settings_save());
        CHECK(std::string(rt_settings_path()).empty());
    }
    { /* 14. env-override reporting */
        unset_env("ICORECOMP_SETTINGS");
        set_env("ICORECOMP_FPS_LIMIT", "120");
        CHECK(rt_settings_overridden("debug.fps_limit_hz"));
        CHECK(!rt_settings_overridden("audio.master_volume"));
        unset_env("ICORECOMP_FPS_LIMIT");
    }
    { /* 15. rt_settings_peek_log_file: false in the file */
        std::string path = scratch + "/peek_false.json";
        write_file(path, "{\"version\": 1, \"debug\": {\"log_file\": false}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        CHECK(rt_settings_peek_log_file() == false);
    }
    { /* 16. rt_settings_peek_log_file: true in the file, and absent */
        std::string path = scratch + "/peek_true.json";
        write_file(path, "{\"version\": 1, \"debug\": {\"log_file\": true}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        CHECK(rt_settings_peek_log_file() == true);

        std::string path2 = scratch + "/peek_absent.json";
        write_file(path2, "{\"version\": 1}\n");
        set_env("ICORECOMP_SETTINGS", path2.c_str());
        CHECK(rt_settings_peek_log_file() == true);
    }
    { /* 17. rt_settings_peek_log_file: malformed file falls back to true,
       * with no .bad copy and no log line (the peek is silent). */
        std::string path = scratch + "/peek_malformed.json";
        std::string original = "{ nope";
        write_file(path, original);
        set_env("ICORECOMP_SETTINGS", path.c_str());
        CHECK(rt_settings_peek_log_file() == true);
        CHECK(!std::filesystem::exists(path + ".bad"));
    }
    { /* 18. rt_settings_peek_log_file: ICORECOMP_SETTINGS=- falls back to true */
        set_env("ICORECOMP_SETTINGS", "-");
        CHECK(rt_settings_peek_log_file() == true);
    }
    { /* 19. commit(false) applies without writing; the write comes from
       * request_save + flush_save. This is the settings menu's path: it
       * commits on every control change and asks for one debounced write. */
        std::string path = scratch + "/nosave.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(!std::filesystem::exists(path));

        rt_settings_mutable().audio.master_volume = 42;
        rt_settings_commit(false);
        CHECK(rt_settings().audio.master_volume == 42);
        CHECK(!std::filesystem::exists(path));

        /* Requesting a save does not write either: the debounce owns when. */
        rt_settings_request_save();
        CHECK(!std::filesystem::exists(path));

        rt_settings_flush_save();
        CHECK(std::filesystem::exists(path));
        CHECK(read_file(path).find("42") != std::string::npos);

        /* Nothing outstanding: a second flush is a no-op, not a second
         * write. Checked by removing the file and flushing again. */
        std::error_code rm_ec;
        std::filesystem::remove(path, rm_ec);
        rt_settings_flush_save();
        CHECK(!std::filesystem::exists(path));

        /* And commit(true) still writes, which is what every non-UI caller
         * relies on. */
        rt_settings_mutable().audio.master_volume = 43;
        rt_settings_commit();
        CHECK(std::filesystem::exists(path));
        rt_settings_init();
        CHECK(rt_settings().audio.master_volume == 43);
    }

    { /* 20. rt_settings_default_binding / rt_settings_binding_key are the
       * tables settings.cpp itself loads from, checked against the values a
       * fresh load produces. host/input.cpp and ui/ui_rebind.cpp reach the
       * compiled defaults only through these two, so a drift between the
       * accessor and the table would be silent everywhere else. */
        set_env("ICORECOMP_SETTINGS", "-");
        rt_settings_init();
        const RtSettings& s = rt_settings();
        for (int i = 0; i < RT_KB_COUNT; ++i) {
            CHECK(s.input.keyboard[i] == rt_settings_default_binding(RT_BIND_KEYBOARD, i));
            CHECK(rt_settings_binding_key(RT_BIND_KEYBOARD, i)[0] != '\0');
        }
        for (int i = 0; i < RT_GP_COUNT; ++i) {
            CHECK(s.input.gamepad[i] == rt_settings_default_binding(RT_BIND_GAMEPAD, i));
            CHECK(rt_settings_binding_key(RT_BIND_GAMEPAD, i)[0] != '\0');
        }
        for (int i = 0; i < RT_GP2_COUNT; ++i) {
            CHECK(s.input.gamepad2[i] == rt_settings_default_binding(RT_BIND_GAMEPAD2, i));
            CHECK(rt_settings_binding_key(RT_BIND_GAMEPAD2, i)[0] != '\0');
            /* Player 2 reads the first pad's table, so the two ship with the
             * same name in every slot. They are different devices, so that
             * is not a collision. */
            CHECK(s.input.gamepad2[i] == s.input.gamepad[i]);
        }
        for (int i = 0; i < RT_MB_COUNT; ++i) {
            CHECK(s.input.mouse[i] == rt_settings_default_binding(RT_BIND_MOUSE, i));
            CHECK(rt_settings_binding_key(RT_BIND_MOUSE, i)[0] != '\0');
        }
        CHECK(std::string(rt_settings_default_binding(RT_BIND_KEYBOARD, RT_KB_COUNT)).empty());
        CHECK(std::string(rt_settings_default_binding(RT_BIND_GAMEPAD, -1)).empty());
        CHECK(std::string(rt_settings_binding_key(RT_BIND_KEYBOARD, RT_KB_COUNT)).empty());
        CHECK(std::string(rt_settings_binding_key(RT_BIND_DEVICE_COUNT, 0)).empty());

        /* Slot counts and menu slots come from the same rows, so the callers
         * that walk a device never switch on the enum themselves. */
        CHECK(rt_settings_bind_slot_count(RT_BIND_KEYBOARD) == RT_KB_COUNT);
        CHECK(rt_settings_bind_slot_count(RT_BIND_GAMEPAD) == RT_GP_COUNT);
        CHECK(rt_settings_bind_slot_count(RT_BIND_GAMEPAD2) == RT_GP2_COUNT);
        /* Player 2's table stops at the sixteen DS2 buttons: the two hotkey
         * slots past them are the host's, so it is two shorter than the
         * first pad's. */
        CHECK(RT_GP2_COUNT == RT_GP_COUNT - 2);
        CHECK(rt_settings_bind_slot_count(RT_BIND_MOUSE) == RT_MB_COUNT);
        CHECK(rt_settings_bind_slot_count(RT_BIND_DEVICE_COUNT) == 0);
        CHECK(rt_settings_bind_menu_slot(RT_BIND_KEYBOARD) == RT_KB_MENU);
        CHECK(rt_settings_bind_menu_slot(RT_BIND_GAMEPAD) == RT_GP_MENU);
        /* The mouse has no menu slot; -1 is what turns rule 1 off for it.
         * Player 2's pad has neither hotkey, so rule 1 does not run for it
         * at all. */
        CHECK(rt_settings_bind_menu_slot(RT_BIND_MOUSE) == -1);
        CHECK(rt_settings_bind_menu_slot(RT_BIND_GAMEPAD2) == -1);
        CHECK(rt_settings_bind_screenshot_slot(RT_BIND_GAMEPAD2) == -1);
        CHECK(rt_settings_bind_menu_slot(RT_BIND_DEVICE_COUNT) == -1);
        /* Every device but player 2's pad has a screenshot slot, the mouse
         * included: it is a host hotkey, not a DS2 button, and does not
         * compete with the pointer the menu draws. */
        CHECK(rt_settings_bind_screenshot_slot(RT_BIND_KEYBOARD) == RT_KB_SCREENSHOT);
        CHECK(rt_settings_bind_screenshot_slot(RT_BIND_GAMEPAD) == RT_GP_SCREENSHOT);
        CHECK(rt_settings_bind_screenshot_slot(RT_BIND_MOUSE) == RT_MB_SCREENSHOT);
        CHECK(rt_settings_bind_screenshot_slot(RT_BIND_DEVICE_COUNT) == -1);
    }
    { /* 21. Every default binding is a usable name and unique on its device.
       * Pure string checks: this target links no SDL, so it cannot ask SDL
       * whether a scancode name resolves. What it can prove is that the
       * table has no empty slot (which host/input.cpp would report as
       * unresolvable), no duplicate (which the commit rules below reject),
       * and that a name carrying an axis direction carries a valid one. */
        set_env("ICORECOMP_SETTINGS", "-");
        rt_settings_init();
        const RtSettings& s = rt_settings();
        for (int i = 0; i < RT_KB_COUNT; ++i) {
            CHECK(!s.input.keyboard[i].empty());
            /* A keyboard binding is a scancode name; the axis suffix is a
             * gamepad-only convention and must not appear here. */
            const char last = s.input.keyboard[i].back();
            CHECK(last != '+' && last != '-');
            for (int j = i + 1; j < RT_KB_COUNT; ++j) {
                CHECK(s.input.keyboard[i] != s.input.keyboard[j]);
            }
        }
        /* The screenshot hotkey ships on the keyboard only, so the keyboard
         * row above is a real name and this one is deliberately "". */
        CHECK(s.input.keyboard[RT_KB_SCREENSHOT] == "F12");
        CHECK(s.input.gamepad[RT_GP_SCREENSHOT].empty());
        CHECK(s.input.mouse[RT_MB_SCREENSHOT].empty());
        int axis_defaults = 0;
        for (int i = 0; i < RT_GP_COUNT; ++i) {
            /* Every gamepad slot but the screenshot hotkey has a default; see
             * the check just above for that one. */
            if (i == RT_GP_SCREENSHOT) continue;
            CHECK(!s.input.gamepad[i].empty());
            const char last = s.input.gamepad[i].back();
            if (last == '+' || last == '-') {
                ++axis_defaults;
                CHECK(s.input.gamepad[i].size() > 1);
            }
            for (int j = i + 1; j < RT_GP_COUNT; ++j) {
                if (j == RT_GP_SCREENSHOT) continue;
                CHECK(s.input.gamepad[i] != s.input.gamepad[j]);
            }
        }
        /* L2 and R2 ship as trigger axes. */
        CHECK(axis_defaults == 2);
        CHECK(s.input.gamepad[RT_GP_L2].back() == '+');
        CHECK(s.input.gamepad[RT_GP_R2].back() == '+');
        /* Player 2's sixteen: every one a real name, no duplicate among
         * them, and the same two trigger axes. */
        int axis2_defaults = 0;
        for (int i = 0; i < RT_GP2_COUNT; ++i) {
            CHECK(!s.input.gamepad2[i].empty());
            const char last = s.input.gamepad2[i].back();
            if (last == '+' || last == '-') {
                ++axis2_defaults;
                CHECK(s.input.gamepad2[i].size() > 1);
            }
            for (int j = i + 1; j < RT_GP2_COUNT; ++j) {
                CHECK(s.input.gamepad2[i] != s.input.gamepad2[j]);
            }
        }
        CHECK(axis2_defaults == 2);
        CHECK(s.input.gamepad2[RT_GP_L2].back() == '+');
        CHECK(s.input.gamepad2[RT_GP_R2].back() == '+');
    }
    { /* 22. duplicate at commit: the slot that changed reverts, the other
       * keeps its name, and the reject message survives for the menu. */
        std::string path = scratch + "/dupbind.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(std::string(rt_settings_last_reject()).empty());
        const std::string cross = rt_settings().input.keyboard[RT_KB_CROSS];
        const std::string circle = rt_settings().input.keyboard[RT_KB_CIRCLE];
        CHECK(cross != circle);

        rt_settings_mutable().input.keyboard[RT_KB_CIRCLE] = cross;
        rt_settings_commit(false);
        CHECK(rt_settings().input.keyboard[RT_KB_CIRCLE] == circle);
        CHECK(rt_settings().input.keyboard[RT_KB_CROSS] == cross);
        CHECK(!std::string(rt_settings_last_reject()).empty());

        /* Case only: SDL resolves names case-insensitively, so this is the
         * same key and has to be rejected the same way. */
        std::string lower = cross;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        rt_settings_mutable().input.keyboard[RT_KB_CIRCLE] = lower;
        rt_settings_commit(false);
        CHECK(rt_settings().input.keyboard[RT_KB_CIRCLE] == circle);

        /* A commit with nothing wrong clears the message again. */
        rt_settings_mutable().audio.master_volume = 70;
        rt_settings_commit(false);
        CHECK(std::string(rt_settings_last_reject()).empty());
    }
    { /* 22b. Player 2's own duplicate reverts, and a name shared with
       * player 1's pad does not: two pads are two devices, and one button
       * on each is two different buttons. */
        std::string path = scratch + "/dupbind2.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        const std::string p2_cross = rt_settings().input.gamepad2[RT_GP_CROSS];
        const std::string p2_circle = rt_settings().input.gamepad2[RT_GP_CIRCLE];
        CHECK(p2_cross != p2_circle);

        rt_settings_mutable().input.gamepad2[RT_GP_CIRCLE] = p2_cross;
        rt_settings_commit(false);
        CHECK(rt_settings().input.gamepad2[RT_GP_CIRCLE] == p2_circle);
        CHECK(!std::string(rt_settings_last_reject()).empty());

        /* A name shared across the two pads is not a conflict. Both pads
         * ship from the same default table, so every pad 2 name is already
         * the pad 1 name in the same slot; to test the cross-pad case and
         * not pad 1's own namespace the name has to be one pad 1 does not
         * hold. Put "misc1" on player 2's r3 first, then on player 1's l3:
         * the second commit is a pad 1 slot taking a name only player 2's
         * pad carries, and it is accepted with both slots left standing. */
        rt_settings_mutable().input.gamepad2[RT_GP_R3] = "misc1";
        rt_settings_commit(false);
        CHECK(rt_settings().input.gamepad2[RT_GP_R3] == "misc1");
        CHECK(std::string(rt_settings_last_reject()).empty());

        rt_settings_mutable().input.gamepad[RT_GP_L3] = "misc1";
        rt_settings_commit(false);
        CHECK(rt_settings().input.gamepad[RT_GP_L3] == "misc1");
        CHECK(rt_settings().input.gamepad2[RT_GP_R3] == "misc1");
        CHECK(std::string(rt_settings_last_reject()).empty());

        /* And within one pad it still is a conflict: player 1's l3 now
         * holds "misc1", so moving player 1's r3 onto it reverts. */
        const std::string p1_r3 = rt_settings().input.gamepad[RT_GP_R3];
        rt_settings_mutable().input.gamepad[RT_GP_R3] = "misc1";
        rt_settings_commit(false);
        CHECK(rt_settings().input.gamepad[RT_GP_R3] == p1_r3);
        CHECK(!std::string(rt_settings_last_reject()).empty());

        /* Rule 3 still reaches player 2: a chord is legal only in a menu
         * slot, and player 2 has none, so it is legal nowhere here. */
        rt_settings_mutable().input.gamepad2[RT_GP_START] = "back+start";
        rt_settings_commit(false);
        CHECK(rt_settings().input.gamepad2[RT_GP_START] != "back+start");
        CHECK(!std::string(rt_settings_last_reject()).empty());
    }
    { /* 23. menu key colliding with a pad binding, on both devices. */
        std::string path = scratch + "/menubind.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        const std::string kb_menu = rt_settings().input.keyboard[RT_KB_MENU];
        const std::string gp_menu = rt_settings().input.gamepad[RT_GP_MENU];

        rt_settings_mutable().input.keyboard[RT_KB_MENU] =
            rt_settings().input.keyboard[RT_KB_START];
        rt_settings_commit(false);
        CHECK(rt_settings().input.keyboard[RT_KB_MENU] == kb_menu);
        CHECK(!std::string(rt_settings_last_reject()).empty());

        rt_settings_mutable().input.gamepad[RT_GP_MENU] =
            rt_settings().input.gamepad[RT_GP_CROSS];
        rt_settings_commit(false);
        CHECK(rt_settings().input.gamepad[RT_GP_MENU] == gp_menu);
        CHECK(!std::string(rt_settings_last_reject()).empty());

        /* A menu key that collides with nothing is accepted. */
        rt_settings_mutable().input.keyboard[RT_KB_MENU] = "F9";
        rt_settings_commit(false);
        CHECK(rt_settings().input.keyboard[RT_KB_MENU] == "F9");
        CHECK(std::string(rt_settings_last_reject()).empty());
    }
    { /* 24. rt_settings_generation moves on every init and every commit, and
       * is never zero: host/input.cpp rebuilds its SDL tables on the
       * difference, and a zero-initialized cache has to differ from it. */
        set_env("ICORECOMP_SETTINGS", "-");
        rt_settings_init();
        const unsigned after_init = rt_settings_generation();
        CHECK(after_init != 0);
        rt_settings_commit(false);
        CHECK(rt_settings_generation() != after_init);
        const unsigned after_commit = rt_settings_generation();
        rt_settings_init();
        CHECK(rt_settings_generation() != after_commit);
        CHECK(rt_settings_generation() != 0);
    }

    { /* 25. the same menu-key collision from the other side: the ordinary
       * slot is what changed, so it is what reverts. Reverting the menu key
       * here would leave both slots holding the same name. */
        std::string path = scratch + "/menubind_other.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        const std::string kb_menu = rt_settings().input.keyboard[RT_KB_MENU];
        const std::string start = rt_settings().input.keyboard[RT_KB_START];
        CHECK(kb_menu != start);

        rt_settings_mutable().input.keyboard[RT_KB_START] = kb_menu;
        rt_settings_commit(false);
        CHECK(rt_settings().input.keyboard[RT_KB_START] == start);
        CHECK(rt_settings().input.keyboard[RT_KB_MENU] == kb_menu);
        CHECK(!std::string(rt_settings_last_reject()).empty());
    }
    { /* 26. a menu-key collision that arrived in the file is reported once
       * at load and then left alone. A later commit that changes something
       * else must not re-reject it: the message is what the menu shows
       * inline, and the user cannot fix a file-borne collision from there. */
        std::string path = scratch + "/menubind_file.json";
        write_file(path, "{\"version\": 1, \"input\": {\"keyboard\": {\"start\": \"F1\"}}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().input.keyboard[RT_KB_MENU] == "F1");
        CHECK(rt_settings().input.keyboard[RT_KB_START] == "F1");

        rt_settings_mutable().audio.master_volume = 61;
        rt_settings_commit(false);
        CHECK(std::string(rt_settings_last_reject()).empty());
        CHECK(rt_settings().input.keyboard[RT_KB_MENU] == "F1");
        CHECK(rt_settings().input.keyboard[RT_KB_START] == "F1");
    }
    { /* 27. the documented maximum deadzone loads as itself. 0.95 parsed as
       * a double is above (double)0.95f, so float-narrowed bounds would
       * reject exactly the value the schema names as the top of the range
       * while the commit path accepted it. */
        std::string path = scratch + "/deadzone_max.json";
        write_file(path, "{\"version\": 1, \"input\": {\"left_deadzone\": 0.95}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().input.left_deadzone == 0.95f);
    }
    { /* 28. display.render_scale: 2 was retired when high-resolution scanout
       * stopped being a separate setting, so it is out of the allowed set
       * {1, 4, 8, 16} and keeps the default like any other bad value. A
       * hires_scanout key left in the file is an unknown key: kept across a
       * save, and it changes nothing. */
        std::string path = scratch + "/render_scale2.json";
        write_file(path,
            "{\n"
            "  \"version\": 1,\n"
            "  \"display\": {\"render_scale\": 2, \"hires_scanout\": true},\n"
            "  \"audio\": {\"master_volume\": 55}\n"
            "}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.render_scale == 1);
        CHECK(rt_settings().audio.master_volume == 55);
        CHECK(rt_settings_save());
        CHECK(read_file(path).find("hires_scanout") != std::string::npos);

        /* And the same value arriving through the menu path reverts at
         * commit rather than reaching the backend. */
        rt_settings_mutable().display.render_scale = 8;
        rt_settings_commit(false);
        CHECK(rt_settings().display.render_scale == 8);
        rt_settings_mutable().display.render_scale = 2;
        rt_settings_commit(false);
        CHECK(rt_settings().display.render_scale == 8);
    }

    { /* 29. compiled-in defaults for the mouse table and the mouse-look
       * keys, and the mouse_names.h lookup they are spelled with. Two slots
       * ship bound and the other fourteen ship "": on this one device an
       * empty slot is a real value, not a name that failed to resolve. */
        set_env("ICORECOMP_SETTINGS", "-");
        rt_settings_init();
        const RtSettings& s = rt_settings();
        CHECK(s.input.keyboard[RT_KB_TRIANGLE] == "Space");
        CHECK(s.input.mouse[RT_MB_SQUARE] == "left");
        CHECK(s.input.mouse[RT_MB_R1] == "right");
        for (int i = 0; i < RT_MB_COUNT; ++i) {
            if (i == RT_MB_SQUARE || i == RT_MB_R1) continue;
            CHECK(s.input.mouse[i].empty());
        }
        CHECK(s.input.mouse_look == true);
        CHECK(s.input.mouse_look_sensitivity == 1.0f);
        CHECK(s.input.mouse_look_invert_y == false);

        /* The names in the table are the names the file is written with, and
         * they resolve the way SDL resolves a scancode name: case folded. */
        RtMouseInput mi = RT_MOUSE_INPUT_COUNT;
        CHECK(rt_mouse_input_from_name("left", &mi) && mi == RT_MOUSE_LEFT);
        CHECK(rt_mouse_input_from_name("LEFT", &mi) && mi == RT_MOUSE_LEFT);
        CHECK(rt_mouse_input_from_name("WheelDown", &mi) && mi == RT_MOUSE_WHEEL_DOWN);
        CHECK(rt_mouse_input_from_name("x2", &mi) && mi == RT_MOUSE_X2);
        /* "" is the unbound slot, not an input, and a prefix or a longer
         * string is not a match either. */
        CHECK(!rt_mouse_input_from_name("", &mi));
        CHECK(!rt_mouse_input_from_name("lef", &mi));
        CHECK(!rt_mouse_input_from_name("leftx", &mi));
        CHECK(!rt_mouse_input_from_name("mouse1", &mi));
        for (int i = 0; i < RT_MOUSE_INPUT_COUNT; ++i) {
            CHECK(rt_mouse_input_from_name(rt_mouse_input_name((RtMouseInput)i), &mi));
            CHECK(mi == (RtMouseInput)i);
        }
        CHECK(std::string(rt_mouse_input_name(RT_MOUSE_INPUT_COUNT)).empty());
    }
    { /* 30. the mouse table and the mouse-look keys round trip through a
       * save and a reload, empty slots included: an unbound slot has to come
       * back unbound, not back at its compiled default. */
        std::string path = scratch + "/mouse_roundtrip.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        RtSettings& m = rt_settings_mutable();
        m.input.mouse[RT_MB_CROSS] = "middle";
        m.input.mouse[RT_MB_SQUARE] = "";       /* unbind a slot that ships bound */
        m.input.mouse[RT_MB_L1] = "wheelup";
        m.input.mouse_look = false;
        m.input.mouse_look_sensitivity = 2.5f;
        m.input.mouse_look_invert_y = true;
        rt_settings_commit();
        CHECK(std::string(rt_settings_last_reject()).empty());

        std::string text = read_file(path);
        CHECK(text.find("\"mouse\"") != std::string::npos);
        CHECK(text.find("wheelup") != std::string::npos);
        /* Retired: the key is never written out again. */
        CHECK(text.find("menu_hit_editor") == std::string::npos);

        rt_settings_init();
        const RtSettings& s = rt_settings();
        CHECK(s.input.mouse[RT_MB_CROSS] == "middle");
        CHECK(s.input.mouse[RT_MB_SQUARE].empty());
        CHECK(s.input.mouse[RT_MB_L1] == "wheelup");
        CHECK(s.input.mouse[RT_MB_R1] == "right");
        CHECK(s.input.mouse[RT_MB_TRIANGLE].empty());
        CHECK(s.input.mouse_look == false);
        CHECK(s.input.mouse_look_sensitivity == 2.5f);
        CHECK(s.input.mouse_look_invert_y == true);
    }
    { /* 31. a file written before the mouse existed: input.keyboard loads,
       * and every mouse key comes from the compiled defaults rather than
       * from an absent section read as empty. */
        std::string path = scratch + "/kbonly.json";
        write_file(path, "{\"version\": 1, \"input\": {\"keyboard\": {\"cross\": \"N\"}}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        const RtSettings& s = rt_settings();
        CHECK(s.input.keyboard[RT_KB_CROSS] == "N");
        CHECK(s.input.keyboard[RT_KB_TRIANGLE] == "Space");
        CHECK(s.input.mouse[RT_MB_SQUARE] == "left");
        CHECK(s.input.mouse[RT_MB_R1] == "right");
        CHECK(s.input.mouse[RT_MB_CROSS].empty());
        CHECK(s.input.mouse_look == true);
        CHECK(s.input.mouse_look_sensitivity == 1.0f);
    }
    { /* 32. input.mouse_look_sensitivity: a bad value in the file keeps the
       * compiled default and says so, and a bad value arriving through the
       * menu reverts at commit to the previously committed value. */
        std::string path = scratch + "/sens.json";
        write_file(path, "{\"version\": 1, \"input\": {\"mouse_look_sensitivity\": 40}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().input.mouse_look_sensitivity == 1.0f);
        CHECK(log_has("input.mouse_look_sensitivity"));
        CHECK(log_has("out of range"));

        /* Both ends of the documented range load as themselves. */
        write_file(path, "{\"version\": 1, \"input\": {\"mouse_look_sensitivity\": 0.05}}\n");
        rt_settings_init();
        CHECK(rt_settings().input.mouse_look_sensitivity == 0.05f);
        write_file(path, "{\"version\": 1, \"input\": {\"mouse_look_sensitivity\": 20}}\n");
        rt_settings_init();
        CHECK(rt_settings().input.mouse_look_sensitivity == 20.0f);

        rt_settings_mutable().input.mouse_look_sensitivity = 3.0f;
        rt_settings_commit(false);
        CHECK(rt_settings().input.mouse_look_sensitivity == 3.0f);
        rt_settings_mutable().input.mouse_look_sensitivity = 0.0f;
        rt_settings_commit(false);
        CHECK(rt_settings().input.mouse_look_sensitivity == 3.0f);
        rt_settings_mutable().input.mouse_look_sensitivity = 25.0f;
        rt_settings_commit(false);
        CHECK(rt_settings().input.mouse_look_sensitivity == 3.0f);
    }
    { /* 33. the same-device duplicate rule on the mouse, and the exemption
       * that makes the mouse different: "" is a legitimate value here, so
       * two unbound slots are not a collision. The mouse has no menu slot,
       * so rule 1 does not run for it at all. */
        std::string path = scratch + "/mousedup.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().input.mouse[RT_MB_SQUARE] == "left");
        CHECK(std::string(rt_settings_last_reject()).empty());

        rt_settings_mutable().input.mouse[RT_MB_CROSS] = "left";
        rt_settings_commit(false);
        CHECK(rt_settings().input.mouse[RT_MB_CROSS].empty());
        CHECK(rt_settings().input.mouse[RT_MB_SQUARE] == "left");
        const std::string reject = rt_settings_last_reject();
        CHECK(reject.find("input.mouse.square") != std::string::npos);
        CHECK(reject.find("input.mouse.cross") != std::string::npos);

        /* Case only, rejected the same way as on the other two devices. */
        rt_settings_mutable().input.mouse[RT_MB_CROSS] = "LEFT";
        rt_settings_commit(false);
        CHECK(rt_settings().input.mouse[RT_MB_CROSS].empty());

        /* Unbinding both shipped slots leaves two empty names, which is not
         * a duplicate and not a reject. */
        rt_settings_mutable().input.mouse[RT_MB_SQUARE] = "";
        rt_settings_mutable().input.mouse[RT_MB_R1] = "";
        rt_settings_commit(false);
        CHECK(rt_settings().input.mouse[RT_MB_SQUARE].empty());
        CHECK(rt_settings().input.mouse[RT_MB_R1].empty());
        CHECK(std::string(rt_settings_last_reject()).empty());

        /* Nothing on the mouse can collide with a menu key, so a name is
         * only ever checked against the other fifteen mouse slots. */
        rt_settings_mutable().input.mouse[RT_MB_TRIANGLE] = "middle";
        rt_settings_mutable().input.keyboard[RT_KB_MENU] = "F1";
        rt_settings_commit(false);
        CHECK(rt_settings().input.mouse[RT_MB_TRIANGLE] == "middle");
        CHECK(std::string(rt_settings_last_reject()).empty());
    }
    { /* 34. unknown keys under input, in the section itself and inside the
       * new mouse object: logged by dotted name and kept across a save. */
        std::string path = scratch + "/unknown_input.json";
        write_file(path,
            "{\"version\": 1, \"input\": {\"mouse_looke\": true,"
            " \"mouse\": {\"nope\": \"left\"}}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        log_clear();
        rt_settings_init();
        CHECK(log_has("unknown key \"input.mouse_looke\""));
        CHECK(log_has("unknown key \"input.mouse.nope\""));
        CHECK(rt_settings().input.mouse_look == true);
        CHECK(rt_settings().input.mouse[RT_MB_SQUARE] == "left");
        CHECK(rt_settings_save());
        std::string text = read_file(path);
        CHECK(text.find("mouse_looke") != std::string::npos);
        CHECK(text.find("nope") != std::string::npos);
    }
    { /* 35. reset-to-defaults covers the mouse table and the new keys. */
        set_env("ICORECOMP_SETTINGS", "-");
        rt_settings_init();
        RtSettings& m = rt_settings_mutable();
        m.input.mouse[RT_MB_SQUARE] = "middle";
        m.input.mouse[RT_MB_START] = "x1";
        m.input.mouse_look = false;
        m.input.mouse_look_sensitivity = 7.0f;
        m.input.mouse_look_invert_y = true;
        rt_settings_reset_defaults();
        const RtSettings& s = rt_settings();
        CHECK(s.input.mouse[RT_MB_SQUARE] == "left");
        CHECK(s.input.mouse[RT_MB_R1] == "right");
        CHECK(s.input.mouse[RT_MB_START].empty());
        CHECK(s.input.mouse_look == true);
        CHECK(s.input.mouse_look_sensitivity == 1.0f);
        CHECK(s.input.mouse_look_invert_y == false);
        CHECK(s.input.keyboard[RT_KB_TRIANGLE] == "Space");
    }
    { /* 36. debug.menu_hit_editor was retired when the pointer on the game's
       * menus started reading the game's own scene objects. A file that
       * still holds it says so once, keeps the key across a save, and
       * changes nothing else in the section. */
        std::string path = scratch + "/hit_editor.json";
        write_file(path,
            "{\n"
            "  \"version\": 1,\n"
            "  \"debug\": {\"menu_hit_editor\": true, \"profile_fields\": 90}\n"
            "}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        log_clear();
        rt_settings_init();
        CHECK(log_has("debug.menu_hit_editor is no longer a setting"));
        CHECK(rt_settings().debug.profile_fields == 90);
        CHECK(rt_settings_save());
        CHECK(read_file(path).find("menu_hit_editor") != std::string::npos);
    }
    { /* 37. display.raster: "crt" (the non-default) loads and round-trips
       * through a save, and an unrecognised value keeps the window default
       * without failing the rest of the load. */
        std::string path = scratch + "/raster.json";
        write_file(path, "{\"version\": 1, \"display\": {\"raster\": \"crt\"}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.raster == RtRaster::Crt);
        CHECK(rt_settings_save());
        CHECK(read_file(path).find("\"raster\": \"crt\"") != std::string::npos);

        std::string bad = scratch + "/rasterbad.json";
        write_file(bad, "{\"version\": 1, \"display\": {\"raster\": \"bogus\", \"show_fps\": true}}\n");
        set_env("ICORECOMP_SETTINGS", bad.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.raster == RtRaster::Window);
        CHECK(rt_settings().display.show_fps);
    }

    { /* 38. The deinterlace mode is fixed at bob and is not a settings
       * key: a file naming another mode keeps bob, says so once, and does
       * not fail the rest of the load. */
        std::string bad = scratch + "/deinterlace.json";
        write_file(bad, "{\"version\": 1, \"display\": {\"deinterlace\": \"weave\", \"show_fps\": true}}\n");
        set_env("ICORECOMP_SETTINGS", bad.c_str());
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().display.deinterlace == RtDeinterlace::Bob);
        CHECK(log_has("display.deinterlace is no longer read"));
        CHECK(rt_settings().display.show_fps);
    }

    { /* 38a. display.widescreen: the default is off, both non-default modes
       * load and round-trip, an unrecognised value keeps the default without
       * failing the rest of the load, and a value written straight into the
       * struct reverts at commit to the last committed one rather than to
       * the compiled-in default. There is no environment twin. */
        std::string path = scratch + "/widescreen.json";
        write_file(path, "{\"version\": 1}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.widescreen == RtWidescreen::Off);

        write_file(path, "{\"version\": 1, \"display\": {\"widescreen\": \"16_9\"}}\n");
        rt_settings_init();
        CHECK(rt_settings().display.widescreen == RtWidescreen::SixteenNine);
        CHECK(rt_settings_save());
        CHECK(read_file(path).find("\"widescreen\": \"16_9\"") != std::string::npos);

        write_file(path, "{\"version\": 1, \"display\": {\"widescreen\": \"window\"}}\n");
        rt_settings_init();
        CHECK(rt_settings().display.widescreen == RtWidescreen::Window);

        std::string bad = scratch + "/widescreenbad.json";
        write_file(bad, "{\"version\": 1, \"display\": {\"widescreen\": \"16:9\", \"show_fps\": true}}\n");
        set_env("ICORECOMP_SETTINGS", bad.c_str());
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().display.widescreen == RtWidescreen::Off);
        CHECK(rt_settings().display.show_fps);
        CHECK(log_has("display.widescreen"));

        /* Committed value first, then an out-of-enum value straight into the
         * struct: the revert target is the committed one. */
        write_file(path, "{\"version\": 1, \"display\": {\"widescreen\": \"window\"}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        rt_settings_mutable().display.widescreen = (RtWidescreen)7;
        log_clear();
        rt_settings_commit(false);
        CHECK(rt_settings().display.widescreen == RtWidescreen::Window);
        CHECK(log_has("display.widescreen"));
        CHECK(log_has("reverted"));

        CHECK(std::string(rt_settings_env_twin("display.widescreen")).empty());
        CHECK(!rt_settings_overridden("display.widescreen"));
    }

    { /* 39. rt_settings_split_chord's grammar: exactly one interior '+',
       * neither half ending in '+' or '-' (that suffix is the axis-
       * direction convention and must never be mistaken for a chord). */
        std::string a, b;
        CHECK(rt_settings_split_chord("back+start", &a, &b));
        CHECK(a == "back" && b == "start");
        CHECK(!rt_settings_split_chord("lefttrigger+", &a, &b));  /* axis, not a chord */
        CHECK(!rt_settings_split_chord("righttrigger-", &a, &b)); /* axis, not a chord */
        CHECK(!rt_settings_split_chord("+start", &a, &b));        /* empty first half */
        CHECK(!rt_settings_split_chord("back+", &a, &b));         /* empty second half */
        CHECK(!rt_settings_split_chord("a+b+c", &a, &b));         /* two interior '+'s */
        CHECK(!rt_settings_split_chord("back+start-", &a, &b));   /* second half ends in a direction suffix */
        CHECK(!rt_settings_split_chord("start", &a, &b));         /* no '+' at all */
        CHECK(!rt_settings_split_chord("Keypad +", &a, &b));      /* trailing '+', same shape as an axis */
    }
    { /* 40. the keyboard table is never chord-parsed: "Keypad +" is a
       * legitimate SDL scancode name, and BindTable::chords is false for
       * RT_BIND_KEYBOARD, so rules 3 and 4 never even look at it. A name
       * that happens to look chord-shaped on the keyboard commits
       * untouched, the same as any other scancode name. */
        std::string path = scratch + "/chord_keyboard.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        rt_settings_mutable().input.keyboard[RT_KB_L2] = "Keypad +";
        rt_settings_commit(false);
        CHECK(rt_settings().input.keyboard[RT_KB_L2] == "Keypad +");
        CHECK(std::string(rt_settings_last_reject()).empty());

        rt_settings_mutable().input.keyboard[RT_KB_L3] = "F1+F2";
        rt_settings_commit(false);
        CHECK(rt_settings().input.keyboard[RT_KB_L3] == "F1+F2");
        CHECK(std::string(rt_settings_last_reject()).empty());
    }
    { /* 41. rule 1 (menu key vs. an ordinary pad binding) is skipped whole
       * when the menu slot holds a chord. bind_name_equal compares the two
       * slots' whole strings, not the chord's parts against them, so
       * "back+start" in the menu slot is never equal to "back" or "start"
       * held by the compiled-in select/start slots -- with or without the
       * skip, rule 1 never fires there and that scenario proves nothing.
       * The only string that can ever match a menu-slot chord is the
       * identical chord string in another slot. Build that: a chord that
       * arrives from the settings file into an ordinary slot survives load
       * untouched (rule 3 only reverts a slot the running commit itself
       * moved) and log_bind_duplicates reports it once; committing that
       * same chord string onto gamepad.menu then makes bind_name_equal
       * true for rule 1, and it must still be accepted because
       * menu_is_chord skips rule 1 whole. */
        std::string path = scratch + "/menuchord.json";
        write_file(path,
            "{\"version\": 1, \"input\": {\"gamepad\": {\"cross\": \"back+start\", \"menu\": \"guide\"}}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().input.gamepad[RT_GP_CROSS] == "back+start");
        CHECK(rt_settings().input.gamepad[RT_GP_MENU] == "guide");
        CHECK(log_has("input.gamepad.cross = \"back+start\" is a chord"));

        rt_settings_mutable().input.gamepad[RT_GP_MENU] = "back+start";
        rt_settings_commit(false);
        CHECK(rt_settings().input.gamepad[RT_GP_MENU] == "back+start");
        CHECK(rt_settings().input.gamepad[RT_GP_CROSS] == "back+start");
        CHECK(std::string(rt_settings_last_reject()).empty());
    }
    { /* 42. rule 3: a chord anywhere but the menu slot reverts at commit,
       * and one that arrives in the file is reported once at load and then
       * left alone -- the same load-once, commit-silent shape as case 26's
       * menu-key collision. */
        std::string path = scratch + "/chord_wrong_slot.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        const std::string up_default = rt_settings().input.gamepad[RT_GP_UP];

        rt_settings_mutable().input.gamepad[RT_GP_UP] = "dpup+dpdown";
        rt_settings_commit(false);
        CHECK(rt_settings().input.gamepad[RT_GP_UP] == up_default);
        CHECK(!std::string(rt_settings_last_reject()).empty());

        std::string file_path = scratch + "/chord_wrong_slot_file.json";
        write_file(file_path, "{\"version\": 1, \"input\": {\"gamepad\": {\"up\": \"dpup+dpdown\"}}}\n");
        set_env("ICORECOMP_SETTINGS", file_path.c_str());
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().input.gamepad[RT_GP_UP] == "dpup+dpdown");
        CHECK(log_has("input.gamepad.up = \"dpup+dpdown\" is a chord"));

        rt_settings_mutable().audio.master_volume = 62;
        rt_settings_commit(false);
        CHECK(std::string(rt_settings_last_reject()).empty());
        CHECK(rt_settings().input.gamepad[RT_GP_UP] == "dpup+dpdown");
    }
    { /* 43. rule 4: a chord whose two parts are the same button is not
       * two buttons at all. */
        std::string path = scratch + "/chord_equal.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        const std::string menu_default = rt_settings().input.gamepad[RT_GP_MENU];

        rt_settings_mutable().input.gamepad[RT_GP_MENU] = "start+start";
        rt_settings_commit(false);
        CHECK(rt_settings().input.gamepad[RT_GP_MENU] == menu_default);
        CHECK(!std::string(rt_settings_last_reject()).empty());

        /* Case only, rejected the same way as every other equality check in
         * this file. */
        rt_settings_mutable().input.gamepad[RT_GP_MENU] = "Start+START";
        rt_settings_commit(false);
        CHECK(rt_settings().input.gamepad[RT_GP_MENU] == menu_default);

        /* The load-time twin, the same load-once, commit-silent shape rule 3
         * has: one that arrives in the file keeps its value and is reported
         * once, because the commit rule only reverts a slot the running
         * commit itself moved. */
        std::string file_path = scratch + "/chord_equal_file.json";
        write_file(file_path, "{\"version\": 1, \"input\": {\"gamepad\": {\"menu\": \"start+start\"}}}\n");
        set_env("ICORECOMP_SETTINGS", file_path.c_str());
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().input.gamepad[RT_GP_MENU] == "start+start");
        CHECK(log_has("chords a button with itself"));

        rt_settings_mutable().audio.master_volume = 41;
        rt_settings_commit(false);
        CHECK(std::string(rt_settings_last_reject()).empty());
        CHECK(rt_settings().input.gamepad[RT_GP_MENU] == "start+start");
    }

    { /* 44. display.screenshot_dir: default, round trip, and that a value is
       * kept as written. There is no allowed set for a path, so there is no
       * bad value for the loader to reject: what has to hold is that the key
       * survives a save and a reload unchanged, including an empty one, which
       * is what tells host/screenshot.cpp to use its own default location. */
        std::string path = scratch + "/shotdir.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.screenshot_dir.empty());

        rt_settings_mutable().display.screenshot_dir = "D:/pictures/ico shots";
        rt_settings_commit(true);
        CHECK(rt_settings().display.screenshot_dir == "D:/pictures/ico shots");
        rt_settings_init();
        CHECK(rt_settings().display.screenshot_dir == "D:/pictures/ico shots");

        rt_settings_mutable().display.screenshot_dir.clear();
        rt_settings_commit(true);
        rt_settings_init();
        CHECK(rt_settings().display.screenshot_dir.empty());

        /* A non-string in the file keeps the compiled default and says so,
         * the same as every other bad value in this layer. */
        std::string bad_path = scratch + "/shotdir_bad.json";
        write_file(bad_path, "{\"version\": 1, \"display\": {\"screenshot_dir\": 7}}\n");
        set_env("ICORECOMP_SETTINGS", bad_path.c_str());
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().display.screenshot_dir.empty());
        CHECK(log_has("display.screenshot_dir"));
    }
    { /* 45. the screenshot hotkey slots: defaults, round trip, and rule 1
       * treating them the way it treats the menu key. */
        std::string path = scratch + "/shotkey.json";
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().input.keyboard[RT_KB_SCREENSHOT] == "F12");
        CHECK(std::string(rt_settings_binding_key(RT_BIND_KEYBOARD, RT_KB_SCREENSHOT)) == "screenshot");
        CHECK(std::string(rt_settings_binding_key(RT_BIND_GAMEPAD, RT_GP_SCREENSHOT)) == "screenshot");
        CHECK(std::string(rt_settings_binding_key(RT_BIND_MOUSE, RT_MB_SCREENSHOT)) == "screenshot");

        /* Round trip through the file, on all three devices. */
        rt_settings_mutable().input.keyboard[RT_KB_SCREENSHOT] = "F10";
        rt_settings_mutable().input.gamepad[RT_GP_SCREENSHOT] = "misc1";
        rt_settings_mutable().input.mouse[RT_MB_SCREENSHOT] = rt_mouse_input_name(RT_MOUSE_MIDDLE);
        rt_settings_commit(true);
        rt_settings_init();
        CHECK(rt_settings().input.keyboard[RT_KB_SCREENSHOT] == "F10");
        CHECK(rt_settings().input.gamepad[RT_GP_SCREENSHOT] == "misc1");
        CHECK(rt_settings().input.mouse[RT_MB_SCREENSHOT] == rt_mouse_input_name(RT_MOUSE_MIDDLE));

        /* Rule 1: the screenshot key onto a pad slot is a pad button the game
         * could never see, so the slot this commit moved reverts. */
        const std::string shot = rt_settings().input.keyboard[RT_KB_SCREENSHOT];
        rt_settings_mutable().input.keyboard[RT_KB_SCREENSHOT] =
            rt_settings().input.keyboard[RT_KB_CROSS];
        rt_settings_commit(false);
        CHECK(rt_settings().input.keyboard[RT_KB_SCREENSHOT] == shot);
        CHECK(!std::string(rt_settings_last_reject()).empty());

        /* And the two hotkeys sharing one key, which is the same rule. */
        rt_settings_mutable().input.keyboard[RT_KB_SCREENSHOT] =
            rt_settings().input.keyboard[RT_KB_MENU];
        rt_settings_commit(false);
        CHECK(rt_settings().input.keyboard[RT_KB_SCREENSHOT] == shot);
        CHECK(!std::string(rt_settings_last_reject()).empty());

        /* A key that collides with nothing is accepted, and the mouse slot
         * takes a name no other mouse slot holds. */
        rt_settings_mutable().input.keyboard[RT_KB_SCREENSHOT] = "F8";
        rt_settings_mutable().input.mouse[RT_MB_SCREENSHOT] = rt_mouse_input_name(RT_MOUSE_X1);
        rt_settings_commit(false);
        CHECK(rt_settings().input.keyboard[RT_KB_SCREENSHOT] == "F8");
        CHECK(rt_settings().input.mouse[RT_MB_SCREENSHOT] == rt_mouse_input_name(RT_MOUSE_X1));
        CHECK(std::string(rt_settings_last_reject()).empty());

        /* The mouse screenshot slot is a hotkey too: sharing a name with a
         * mouse pad slot is rule 1, not rule 2. */
        rt_settings_mutable().input.mouse[RT_MB_SCREENSHOT] =
            rt_settings().input.mouse[RT_MB_SQUARE];
        rt_settings_commit(false);
        CHECK(rt_settings().input.mouse[RT_MB_SCREENSHOT] == rt_mouse_input_name(RT_MOUSE_X1));
        CHECK(!std::string(rt_settings_last_reject()).empty());
    }

    { /* debug.log_level: the default, each accepted name, and a bad value.
       *
       * The level itself lives in log.cpp, which this binary does not
       * link (see the file comment); what is under test here is the
       * settings half: the default, the four names the loader accepts,
       * that a name outside them keeps the default with a line naming the
       * value and the allowed set, and that the value survives a save and
       * reload. */
        std::string path = scratch + "/log_level.json";
        write_file(path, "{\"version\": 1}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().debug.log_level == RT_LOG_WARN);

        struct { const char* name; RtLogLevel value; } cases[] = {
            {"error", RT_LOG_ERROR}, {"warn", RT_LOG_WARN},
            {"info", RT_LOG_INFO}, {"debug", RT_LOG_DEBUG},
        };
        for (const auto& c : cases) {
            write_file(path, std::string("{\"version\": 1, \"debug\": {\"log_level\": \"")
                + c.name + "\"}}\n");
            rt_settings_init();
            CHECK(rt_settings().debug.log_level == c.value);
        }

        write_file(path, "{\"version\": 1, \"debug\": {\"log_level\": \"chatty\"}}\n");
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().debug.log_level == RT_LOG_WARN);
        CHECK(log_has("debug.log_level"));
        CHECK(log_has("chatty"));
        CHECK(log_has("error/warn/info/debug"));
        CHECK(log_has("kept default"));

        /* Not a string at all takes the same path. */
        write_file(path, "{\"version\": 1, \"debug\": {\"log_level\": 3}}\n");
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().debug.log_level == RT_LOG_WARN);
        CHECK(log_has("debug.log_level"));
        CHECK(log_has("is not a string"));

        /* Round trip: set it through the UI path, save, reload. */
        write_file(path, "{\"version\": 1}\n");
        rt_settings_init();
        rt_settings_mutable().debug.log_level = RT_LOG_DEBUG;
        rt_settings_commit(true);
        CHECK(rt_settings().debug.log_level == RT_LOG_DEBUG);
        rt_settings_init();
        CHECK(rt_settings().debug.log_level == RT_LOG_DEBUG);

        /* An out-of-set value written straight into the struct reverts to
         * the last committed one rather than to the compiled-in default,
         * exactly like display.render_scale. */
        rt_settings_mutable().debug.log_level = (RtLogLevel)9;
        log_clear();
        rt_settings_commit(false);
        CHECK(rt_settings().debug.log_level == RT_LOG_DEBUG);
        CHECK(log_has("debug.log_level"));
        CHECK(log_has("reverted"));
    }
    { /* debug.log_level has an environment twin, and it is reported as
       * overriding the file the way every other twin is. Whether the
       * variable actually wins is log.cpp's and main's job, not this
       * binary's; what settings owes is the report. */
        std::string path = scratch + "/log_level_env.json";
        write_file(path, "{\"version\": 1, \"debug\": {\"log_level\": \"info\"}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(!rt_settings_overridden("debug.log_level"));
        CHECK(std::string(rt_settings_env_twin("debug.log_level")) == "ICORECOMP_LOG_LEVEL");

        set_env("ICORECOMP_LOG_LEVEL", "error");
        CHECK(rt_settings_overridden("debug.log_level"));
        log_clear();
        rt_settings_init();
        CHECK(log_has("debug.log_level is overridden by ICORECOMP_LOG_LEVEL=error"));
        /* The file's value still loads: the environment wins at the point
         * of use (main.cpp, settings_apply.cpp), not by rewriting the
         * model, so the menu can still show what the file says. */
        CHECK(rt_settings().debug.log_level == RT_LOG_INFO);
        unset_env("ICORECOMP_LOG_LEVEL");
    }
    { /* The four audio category gains: audio.music_volume,
       * audio.effects_volume, audio.movie_volume and audio.chime_volume.
       * The default, both ends of the range, a value outside it, a value of
       * the wrong type, the round trip, the commit-time revert, and that
       * none of them has an environment twin.
       *
       * All four are host output gains. Nothing the game supplied is
       * touched by any of them: three are applied where snd/engine.cpp sums
       * that category, the chime is applied where host/audio.cpp mixes a
       * sound the game never asked for. */
        std::string path = scratch + "/audio_volumes.json";
        write_file(path, "{\"version\": 1}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().audio.music_volume == 100);
        CHECK(rt_settings().audio.effects_volume == 100);
        CHECK(rt_settings().audio.movie_volume == 100);
        CHECK(rt_settings().audio.chime_volume == 60);

        /* Both ends of [0, 100] load, on every one of the four. */
        write_file(path, "{\"version\": 1, \"audio\": {\"music_volume\": 0,"
                         " \"effects_volume\": 100, \"movie_volume\": 0,"
                         " \"chime_volume\": 100}}\n");
        rt_settings_init();
        CHECK(rt_settings().audio.music_volume == 0);
        CHECK(rt_settings().audio.effects_volume == 100);
        CHECK(rt_settings().audio.movie_volume == 0);
        CHECK(rt_settings().audio.chime_volume == 100);

        /* Out of range and the wrong type each keep the compiled-in default
         * and name the key. */
        write_file(path, "{\"version\": 1, \"audio\": {\"music_volume\": 101,"
                         " \"effects_volume\": -1, \"movie_volume\": \"loud\","
                         " \"chime_volume\": 1000}}\n");
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().audio.music_volume == 100);
        CHECK(rt_settings().audio.effects_volume == 100);
        CHECK(rt_settings().audio.movie_volume == 100);
        CHECK(rt_settings().audio.chime_volume == 60);
        CHECK(log_has("audio.music_volume"));
        CHECK(log_has("audio.effects_volume"));
        CHECK(log_has("audio.movie_volume"));
        CHECK(log_has("audio.chime_volume"));
        CHECK(log_has("kept default"));

        /* Round trip through the UI path, then the commit-time revert to
         * the last committed value rather than to the compiled-in one. */
        write_file(path, "{\"version\": 1}\n");
        rt_settings_init();
        rt_settings_mutable().audio.music_volume = 30;
        rt_settings_mutable().audio.effects_volume = 70;
        rt_settings_mutable().audio.movie_volume = 10;
        rt_settings_mutable().audio.chime_volume = 5;
        rt_settings_commit(true);
        rt_settings_init();
        CHECK(rt_settings().audio.music_volume == 30);
        CHECK(rt_settings().audio.effects_volume == 70);
        CHECK(rt_settings().audio.movie_volume == 10);
        CHECK(rt_settings().audio.chime_volume == 5);

        rt_settings_mutable().audio.music_volume = 500;
        log_clear();
        rt_settings_commit(false);
        CHECK(rt_settings().audio.music_volume == 30);
        CHECK(log_has("audio.music_volume"));
        CHECK(log_has("reverted"));

        /* No environment twins: these are settings.json and the menu only. */
        CHECK(std::string(rt_settings_env_twin("audio.music_volume")).empty());
        CHECK(std::string(rt_settings_env_twin("audio.chime_volume")).empty());
        CHECK(!rt_settings_overridden("audio.effects_volume"));
        CHECK(!rt_settings_overridden("audio.movie_volume"));
    }
    { /* debug.fps_limit_hz: the guest field pacer's cap.
       *
       * Its default is not a rate. RT_FPS_LIMIT_MODE_RATE means "the rate
       * of the video mode the game programmed", which is 59.94 on NTSC and
       * 50 on PAL, so the PAL disc's two display options are both paced
       * right without the player touching the key. 0 still means no pacing
       * and [1, 1000] is still a plain rate. */
        std::string path = scratch + "/fps_limit.json";
        write_file(path, "{\"version\": 1}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().debug.fps_limit_hz == RT_FPS_LIMIT_MODE_RATE);

        const double accepted[] = { RT_FPS_LIMIT_MODE_RATE, 0.0, 1.0, 50.0, 59.94, 1000.0 };
        for (double hz : accepted) {
            write_file(path, "{\"version\": 1, \"debug\": {\"fps_limit_hz\": "
                + std::to_string(hz) + "}}\n");
            rt_settings_init();
            CHECK(rt_settings().debug.fps_limit_hz == hz);
        }

        /* A negative number that is not the sentinel is not a rate and not
         * the sentinel, so it keeps the default and says so. */
        const char* rejected[] = { "-2", "0.5", "1001" };
        for (const char* text : rejected) {
            write_file(path, std::string("{\"version\": 1, \"debug\": {\"fps_limit_hz\": ")
                + text + "}}\n");
            log_clear();
            rt_settings_init();
            CHECK(rt_settings().debug.fps_limit_hz == RT_FPS_LIMIT_MODE_RATE);
            CHECK(log_has("debug.fps_limit_hz"));
            CHECK(log_has("must be 0 or [1, 1000]"));
        }

        /* The sentinel survives a commit, and an out-of-range value written
         * straight into the struct reverts to the last committed one. */
        write_file(path, "{\"version\": 1}\n");
        rt_settings_init();
        rt_settings_mutable().debug.fps_limit_hz = 50.0;
        rt_settings_commit(true);
        CHECK(rt_settings().debug.fps_limit_hz == 50.0);
        rt_settings_mutable().debug.fps_limit_hz = RT_FPS_LIMIT_MODE_RATE;
        rt_settings_commit(false);
        CHECK(rt_settings().debug.fps_limit_hz == RT_FPS_LIMIT_MODE_RATE);
        rt_settings_mutable().debug.fps_limit_hz = -3.0;
        log_clear();
        rt_settings_commit(false);
        CHECK(rt_settings().debug.fps_limit_hz == RT_FPS_LIMIT_MODE_RATE);
        CHECK(log_has("debug.fps_limit_hz"));
        CHECK(log_has("reverted"));

        /* The environment twin is still ICORECOMP_FPS_LIMIT. */
        CHECK(std::string(rt_settings_env_twin("debug.fps_limit_hz")) == "ICORECOMP_FPS_LIMIT");
    }
    { /* debug.console: default, both values, and the pre-settings peek
       * rt_console_init uses. */
        std::string path = scratch + "/console.json";
        write_file(path, "{\"version\": 1}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().debug.console == false);
        CHECK(rt_settings_peek_console() == false);

        write_file(path, "{\"version\": 1, \"debug\": {\"console\": true}}\n");
        rt_settings_init();
        CHECK(rt_settings().debug.console == true);
        CHECK(rt_settings_peek_console() == true);

        /* The peek is silent and never fails: a broken file, a file from a
         * newer build and a missing file all read as the default. */
        write_file(path, "{ nope");
        CHECK(rt_settings_peek_console() == false);
        write_file(path, "{\"version\": 2, \"debug\": {\"console\": true}}\n");
        CHECK(rt_settings_peek_console() == false);
        set_env("ICORECOMP_SETTINGS", (scratch + "/no_such_file.json").c_str());
        CHECK(rt_settings_peek_console() == false);
        set_env("ICORECOMP_SETTINGS", path.c_str());

        /* The remaining shapes the log_file peek's cases 15 to 18 cover,
         * against the opposite fallback: an explicit false, a debug object
         * that carries other keys but not this one, a value of the wrong
         * type, and ICORECOMP_SETTINGS=- (no settings file at all). Only
         * an explicit true reads as true. */
        write_file(path, "{\"version\": 1, \"debug\": {\"console\": false}}\n");
        CHECK(rt_settings_peek_console() == false);
        write_file(path, "{\"version\": 1, \"debug\": {\"log_file\": true}}\n");
        CHECK(rt_settings_peek_console() == false);
        write_file(path, "{\"version\": 1, \"debug\": {\"console\": \"yes\"}}\n");
        CHECK(rt_settings_peek_console() == false);
        set_env("ICORECOMP_SETTINGS", "-");
        CHECK(rt_settings_peek_console() == false);
        write_file(path, "{\"version\": 1, \"debug\": {\"console\": true}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        CHECK(rt_settings_peek_console() == true);
    }

    { /* display.backend was withdrawn on 2026-09-05: the key is named once
       * at info as no longer read, it is not written back, and it is not an
       * unknown key. RtSettings carries no field for it any more, so what is
       * checked is that the rest of the same object still loads.
       * ICORECOMP_GS_BACKEND is no longer a twin of anything. */
        std::string path = scratch + "/backend.json";
        write_file(path, "{\"version\": 1, \"display\": {\"backend\": \"d3d12\", \"show_fps\": true}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().display.show_fps);
        CHECK(log_has("display.backend is no longer read"));
        CHECK(!log_has("unknown key"));
        CHECK(rt_settings_save());
        CHECK(read_file(path).find("\"backend\"") != std::string::npos); /* kept byte for byte, like every retired key */
        CHECK(rt_settings_env_twin("display.backend") == nullptr
              || std::string(rt_settings_env_twin("display.backend")).empty());
        unset_env("ICORECOMP_GS_BACKEND");
    }

    { /* achievements.*: the three keys default true/true/false, load,
       * round-trip through a save, reject a non-boolean without losing the
       * rest of the section, and have no environment twin. The chime's
       * volume is audio.chime_volume and is tested with the other gains;
       * the progress-bit diagnostic is not a setting at all. */
        std::string path = scratch + "/achievements.json";
        write_file(path, "{\"version\": 1}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().achievements.enabled == true);
        CHECK(rt_settings().achievements.toast == true);
        CHECK(rt_settings().achievements.sound == false);

        write_file(path, "{\"version\": 1, \"achievements\": {\"enabled\": false,"
                         " \"toast\": false, \"sound\": true}}\n");
        rt_settings_init();
        CHECK(rt_settings().achievements.enabled == false);
        CHECK(rt_settings().achievements.toast == false);
        CHECK(rt_settings().achievements.sound == true);
        CHECK(rt_settings_save());
        {
            std::string text = read_file(path);
            CHECK(text.find("\"enabled\": false") != std::string::npos);
            CHECK(text.find("\"sound\": true") != std::string::npos);
        }

        /* A wrong type keeps that one key's default and loads the rest. */
        write_file(path, "{\"version\": 1, \"achievements\": {\"toast\": \"yes\","
                         " \"sound\": true}}\n");
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().achievements.toast == true);
        CHECK(rt_settings().achievements.sound == true);
        CHECK(log_has("achievements.toast"));
        CHECK(log_has("is not a boolean"));

        /* An unknown key in the section is reported and kept, like any
         * other unknown key. */
        write_file(path, "{\"version\": 1, \"achievements\": {\"volume\": 3}}\n");
        log_clear();
        rt_settings_init();
        CHECK(log_has("achievements.volume"));

        CHECK(std::string(rt_settings_env_twin("achievements.enabled")).empty());
        CHECK(!rt_settings_overridden("achievements.toast"));
    }

    { /* "system" is still a section this build knows, even though it no
       * longer writes one: a file that has it, from an older build or a
       * hand edit, must not be reported as an unknown key. The audit that
       * added this found "system" missing from the top-level known-key
       * list, which made every saved file log
       * `unknown key "system" kept as-is` on the next load. */
        std::string path = scratch + "/known-sections.json";
        write_file(path, "{\"version\": 1, \"system\": {\"language\": \"italian\"}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        log_clear();
        rt_settings_init();
        CHECK(!log_has("unknown key"));
        /* Named as retired instead, once, and not read. */
        CHECK(log_has("system.language is no longer read"));
        CHECK(rt_settings().system.language == RtLanguage::English);

        /* The round trip the user actually takes: save, then load what was
         * saved. Every section write_struct_into_dom writes is known, and
         * the retired section it carried through is still not unknown. */
        CHECK(rt_settings_save());
        log_clear();
        rt_settings_init();
        CHECK(!log_has("unknown key"));
    }

    { /* display.remember_window_size: loads, round-trips, and keeps its
       * default on a wrong type. It is the key host/window.cpp's resize
       * handler reads (window.cpp maybe_save_window_size) and it now has a
       * menu control of its own in ui/menu.rml. */
        std::string path = scratch + "/remember-size.json";
        write_file(path, "{\"version\": 1}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.remember_window_size == true);

        write_file(path, "{\"version\": 1, \"display\": {\"remember_window_size\": false}}\n");
        rt_settings_init();
        CHECK(rt_settings().display.remember_window_size == false);
        CHECK(rt_settings_save());
        CHECK(read_file(path).find("\"remember_window_size\": false") != std::string::npos);

        write_file(path, "{\"version\": 1, \"display\": {\"remember_window_size\": 1}}\n");
        log_clear();
        rt_settings_init();
        CHECK(rt_settings().display.remember_window_size == true);
        CHECK(log_has("display.remember_window_size"));
        CHECK(log_has("is not a boolean"));
    }

    { /* The cold keys (host/settings.h): debug.console and debug.log_file
       * are the two whose consumer reads them once at process start, so the
       * runtime applies a change by restarting
       * itself. That is only legal
       * before the guest is running, and this case is both halves of the
       * rule: the pure predicate the menu asks before it restarts, and the
       * refusal that stands whoever writes the struct.
       *
       * The restart itself is not exercised here. It is process work
       * (runtime.h rt_restart_now) and this binary links neither main.cpp
       * nor a window. */
        std::string path = scratch + "/cold-keys.json";
        write_file(path, "{\"version\": 1, \"debug\": {\"console\": false}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        /* Nothing has said the game started, and a load must not. */
        CHECK(rt_settings_gameplay_active() == false);

        /* The predicate is pure: two structs in, the first cold key that
         * differs out, and a hot key is not one of them. */
        const RtSettings a = rt_settings();
        RtSettings b = a;
        CHECK(rt_settings_cold_key_changed(a, b) == nullptr);
        b.debug.console = true;
        CHECK(rt_settings_cold_key_changed(a, b) != nullptr);
        CHECK(std::strcmp(rt_settings_cold_key_changed(a, b), "debug.console") == 0);
        b = a;
        b.debug.log_file = !a.debug.log_file;
        CHECK(std::strcmp(rt_settings_cold_key_changed(a, b), "debug.log_file") == 0);
        b = a;
        b.audio.master_volume = 50;
        CHECK(rt_settings_cold_key_changed(a, b) == nullptr);
        /* launcher.* is cold for the run and deliberately not in this
         * class: it restarts nothing. */
        b = a;
        b.launcher.show_at_startup = !a.launcher.show_at_startup;
        CHECK(rt_settings_cold_key_changed(a, b) == nullptr);

        /* Before the game starts the commit keeps the change: restarting is
         * the caller's half (ui/ui_settings_model.cpp). */
        rt_settings_mutable().debug.console = true;
        rt_settings_commit(false);
        CHECK(rt_settings().debug.console == true);
        rt_settings_mutable().debug.console = false;
        rt_settings_commit(false);
        CHECK(rt_settings().debug.console == false);

        /* From rt_sched_boot on, both keys are refused and reverted with
         * a log naming them. The menu disables those controls, so what this
         * stands for is everything else that can reach the struct: a script,
         * a future caller, anything poking rt_settings_mutable() directly. */
        rt_settings_set_gameplay_active(true);
        CHECK(rt_settings_gameplay_active());
        log_clear();
        rt_settings_mutable().debug.console = true;
        rt_settings_mutable().debug.log_file = false;
        rt_settings_commit(false);
        CHECK(rt_settings().debug.console == false);
        CHECK(rt_settings().debug.log_file == true);
        CHECK(log_has("debug.console"));
        CHECK(log_has("debug.log_file"));
        CHECK(log_has("reverted"));

        /* launcher.show_at_startup still commits with the game running: it
         * is cold for the run but restarts nothing, so nothing gates it. */
        rt_settings_mutable().launcher.show_at_startup = false;
        rt_settings_commit(false);
        CHECK(rt_settings().launcher.show_at_startup == false);

        /* Only the cold keys are gated: a hot key still commits with the
         * game running, which is the whole point of the menu. */
        rt_settings_mutable().audio.master_volume = 42;
        rt_settings_commit(false);
        CHECK(rt_settings().audio.master_volume == 42);

        /* And the save carries the value the commit kept, not the refused
         * one: a hand edit of the file is still the way to change a cold key
         * mid-run, and it takes effect at the next launch. */
        CHECK(rt_settings_save());
        /* display.backend is no longer a file key (withdrawn 2026-09-05), so
         * the save writes no "backend" at all, whatever the struct holds. */
        CHECK(read_file(path).find("\"backend\"") == std::string::npos);

        rt_settings_set_gameplay_active(false);
        CHECK(rt_settings_gameplay_active() == false);
    }

    unset_env("ICORECOMP_SETTINGS");
    std::printf("settings-selftest: all checks passed\n");
    return 0;
}
