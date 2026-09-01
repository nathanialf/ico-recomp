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
#include "host/settings.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

/* ---- runtime stubs -------------------------------------------------------- */

namespace {
std::string g_base_dir = ".";
} // namespace

void rt_log(const char* component, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::printf("[%s] ", component);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
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
        CHECK(s.display.window_width == 640 && s.display.window_height == 480);
        CHECK(s.debug.fps_limit_hz == 59.94);
        CHECK(s.input.keyboard[RT_KB_CROSS] == "X");
        CHECK(s.input.gamepad[RT_GP_L2] == "lefttrigger+");
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
        rt_settings_commit();

        rt_settings_init();
        const RtSettings& s = rt_settings();
        CHECK(s.display.mode == RtDisplayMode::FullscreenDesktop);
        CHECK(s.audio.master_volume == 55);
        CHECK(s.input.left_deadzone == 0.25f);
        CHECK(s.debug.fps_limit_hz == 0.0);
        CHECK(s.display.render_scale == 4);
        CHECK(s.input.keyboard[RT_KB_MENU] == "F2");
    }
    { /* 8. unknown-key preservation across load/save */
        std::string path = scratch + "/unknown.json";
        write_file(path,
            "{\n"
            "  \"version\": 1,\n"
            "  \"display\": {\"foo\": 42},\n"
            "  \"custom_section\": {\"x\": 1}\n"
            "}\n");
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings_save());
        std::string text = read_file(path);
        CHECK(text.find("\"foo\"") != std::string::npos);
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
        CHECK(s.display.window_width == 640);
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
        CHECK(rt_settings().display.window_width == 640);
        CHECK(!rt_settings_save());
        CHECK(read_file(path) == original);
    }
    { /* 12. malformed file: defaults, original bytes preserved in .bad */
        std::string path = scratch + "/malformed.json";
        std::string original = "{ nope";
        write_file(path, original);
        set_env("ICORECOMP_SETTINGS", path.c_str());
        rt_settings_init();
        CHECK(rt_settings().display.window_width == 640);
        CHECK(std::filesystem::exists(path + ".bad"));
        CHECK(read_file(path + ".bad") == original);
    }
    { /* 13. ICORECOMP_SETTINGS=- : defaults only, no file, save refused */
        set_env("ICORECOMP_SETTINGS", "-");
        rt_settings_init();
        CHECK(rt_settings().display.window_width == 640);
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

    unset_env("ICORECOMP_SETTINGS");
    std::printf("settings-selftest: all checks passed\n");
    return 0;
}
