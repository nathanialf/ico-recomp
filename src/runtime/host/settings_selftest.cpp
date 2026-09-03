/* host/settings_selftest.cpp: standalone exercise of the JSON DOM
 * (host/json.cpp) and the typed settings model (host/settings.cpp).
 *
 * Links json.cpp + settings.cpp against stub rt_log/rt_base_dir (below);
 * those are the only two runtime.h externs settings.cpp calls. rt_base_dir()
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

void rt_log(const char* component, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("[%s] %s\n", component, buf);
    g_log += buf;
    g_log += '\n';
}

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
        CHECK(s.display.window_width == 1280 && s.display.window_height == 960);
        CHECK(s.debug.fps_limit_hz == 59.94);
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
        CHECK(s.gameplay.run_any_direction == true);
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
        write_file(path, "{\"version\": 1, \"display\": {\"present\": \"warp\"}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.present == RtPresentMode::Mailbox);
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
        CHECK(rt_settings_bind_slot_count(RT_BIND_MOUSE) == RT_MB_COUNT);
        CHECK(rt_settings_bind_slot_count(RT_BIND_DEVICE_COUNT) == 0);
        CHECK(rt_settings_bind_menu_slot(RT_BIND_KEYBOARD) == RT_KB_MENU);
        CHECK(rt_settings_bind_menu_slot(RT_BIND_GAMEPAD) == RT_GP_MENU);
        /* The mouse has no menu slot; -1 is what turns rule 1 off for it. */
        CHECK(rt_settings_bind_menu_slot(RT_BIND_MOUSE) == -1);
        CHECK(rt_settings_bind_menu_slot(RT_BIND_DEVICE_COUNT) == -1);
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
        int axis_defaults = 0;
        for (int i = 0; i < RT_GP_COUNT; ++i) {
            CHECK(!s.input.gamepad[i].empty());
            const char last = s.input.gamepad[i].back();
            if (last == '+' || last == '-') {
                ++axis_defaults;
                CHECK(s.input.gamepad[i].size() > 1);
            }
            for (int j = i + 1; j < RT_GP_COUNT; ++j) {
                CHECK(s.input.gamepad[i] != s.input.gamepad[j]);
            }
        }
        /* L2 and R2 ship as trigger axes. */
        CHECK(axis_defaults == 2);
        CHECK(s.input.gamepad[RT_GP_L2].back() == '+');
        CHECK(s.input.gamepad[RT_GP_R2].back() == '+');
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

    { /* 38. display.deinterlace: "weave" (a non-default) loads and
       * round-trips through a save, and an unrecognised value keeps the bob
       * default without failing the rest of the load. */
        std::string path = scratch + "/deinterlace.json";
        write_file(path, "{\"version\": 1, \"display\": {\"deinterlace\": \"weave\"}}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.deinterlace == RtDeinterlace::Weave);
        CHECK(rt_settings_save());
        CHECK(read_file(path).find("\"deinterlace\": \"weave\"") != std::string::npos);

        std::string bad = scratch + "/deinterlacebad.json";
        write_file(bad, "{\"version\": 1, \"display\": {\"deinterlace\": \"bogus\", \"show_fps\": true}}\n");
        set_env("ICORECOMP_SETTINGS", bad.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.deinterlace == RtDeinterlace::Bob);
        CHECK(rt_settings().display.show_fps);
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

    unset_env("ICORECOMP_SETTINGS");
    std::printf("settings-selftest: all checks passed\n");
    return 0;
}
