/* host/json.h: minimal JSON DOM reader/writer for settings.json.
 *
 * Hand-rolled for the same reason the TOML readers are (see loader.cpp):
 * the schema is one nesting level of objects holding scalars plus two
 * string-to-string maps, and the house position is no heavy parsing
 * dependencies. nlohmann/json's single header is ~900 KB and would be
 * rejected by tools/check_no_rom.sh's 512 KB size gate outright.
 *
 * Strictness follows "loud failure beats silent wrongness":
 *   - every parse error reports "line:column: message" (1-based);
 *   - trailing commas, comments, NaN and Infinity are rejected;
 *   - duplicate keys inside one object are a parse error (a hand-edited
 *     settings file with two "master_volume" lines should say so, not
 *     silently keep one);
 *   - \uXXXX escapes are decoded to UTF-8, surrogate pairs included.
 *
 * Objects preserve insertion order, so a load-modify-save round trip keeps
 * the file diffable and keeps keys this build does not know about.
 *
 * Runtime-internal, NOT part of the ABI contract (include/recomp_*.h).
 */
#ifndef ICORECOMP_HOST_JSON_H
#define ICORECOMP_HOST_JSON_H

#include <string>
#include <utility>
#include <vector>

struct RtJson {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string str;
    std::vector<RtJson> arr;
    /* Insertion-ordered; rt_json_parse guarantees unique keys. */
    std::vector<std::pair<std::string, RtJson>> obj;

    /* Object key lookup; null when absent or when this is not an object. */
    const RtJson* find(const char* key) const;
    RtJson* find(const char* key);

    /* Replaces the value under `key` in place (keeping its position) or
     * appends it. Makes this value an object if it was null. Returns the
     * stored value. */
    RtJson& set(const char* key, RtJson v);

    static RtJson make_null();
    static RtJson make_bool(bool v);
    static RtJson make_number(double v);
    static RtJson make_string(const char* v);
    static RtJson make_string(const std::string& v);
    static RtJson make_object();
    static RtJson make_array();
};

/* Parses `text` into *out. On failure returns false, leaves *out untouched,
 * and fills *err with "line:column: message". Nesting is capped at 64
 * levels so a hostile file cannot exhaust the stack. */
bool rt_json_parse(const std::string& text, RtJson* out, std::string* err);

/* Serializes with two-space indent, insertion key order, and \n line
 * endings on every platform, ending with a final newline. Numbers print as
 * integers when they are integral, otherwise with the shortest
 * representation that round-trips through strtod. */
std::string rt_json_write(const RtJson& v);

/* Atomic write of `text` to `path`: "<path>.tmp", flushed and fsynced,
 * then renamed over the target. Parent directories are created. Any
 * failure logs with strerror, removes the .tmp best-effort, leaves
 * whatever was at `path` alone, and returns false; it is never fatal.
 *
 * Lives here rather than in settings.cpp because it is about writing a file
 * and not about settings. Nothing about it is JSON specific beyond the
 * callers it has; settings.cpp is the only one left since the menu pointer
 * stopped keeping a table of its own.
 */
bool rt_json_write_file(const std::string& path, const std::string& text);

#endif /* ICORECOMP_HOST_JSON_H */
