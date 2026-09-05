/* host/json.cpp: see json.h for scope and strictness rules. */
#include "host/json.h"

#include "host/portable.h"
#include "runtime.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

constexpr int kMaxDepth = 64;

struct Parser {
    const char* s = nullptr;
    size_t n = 0;
    size_t i = 0;
    int depth = 0;
    std::string err;

    /* Records "line:column: message" for the current offset. Line/column
     * are computed here rather than tracked per character because they are
     * only needed once, on the failure path. */
    bool fail(const char* msg) {
        size_t line = 1, col = 1;
        for (size_t k = 0; k < i && k < n; ++k) {
            if (s[k] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%zu:%zu: ", line, col);
        err = buf;
        err += msg;
        return false;
    }

    void skip_ws() {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    }

    bool literal(const char* word, size_t len) {
        if (i + len > n || std::memcmp(s + i, word, len) != 0) return false;
        i += len;
        return true;
    }

    /* One \uXXXX escape's four hex digits, already past the 'u'. */
    bool hex4(uint32_t* out) {
        if (i + 4 > n) return fail("truncated \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i + k];
            uint32_t d;
            if (c >= '0' && c <= '9') d = uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') d = uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = uint32_t(c - 'A' + 10);
            else return fail("bad hex digit in \\u escape");
            v = (v << 4) | d;
        }
        i += 4;
        *out = v;
        return true;
    }

    static void append_utf8(std::string* out, uint32_t cp) {
        if (cp < 0x80) {
            out->push_back(char(cp));
        } else if (cp < 0x800) {
            out->push_back(char(0xC0 | (cp >> 6)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out->push_back(char(0xE0 | (cp >> 12)));
            out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out->push_back(char(0xF0 | (cp >> 18)));
            out->push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out->push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out->push_back(char(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_string(std::string* out) {
        if (i >= n || s[i] != '"') return fail("expected string");
        ++i;
        out->clear();
        while (true) {
            if (i >= n) return fail("unterminated string");
            unsigned char c = (unsigned char)s[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (c < 0x20) return fail("raw control character in string (use \\u escape)");
            if (c != '\\') {
                out->push_back(char(c));
                ++i;
                continue;
            }
            ++i;
            if (i >= n) return fail("truncated escape");
            char e = s[i];
            ++i;
            switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                uint32_t cp;
                if (!hex4(&cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* High surrogate: the low half must follow. */
                    if (i + 2 > n || s[i] != '\\' || s[i + 1] != 'u') {
                        return fail("high surrogate not followed by \\u low surrogate");
                    }
                    i += 2;
                    uint32_t lo;
                    if (!hex4(&lo)) return false;
                    if (lo < 0xDC00 || lo > 0xDFFF) return fail("invalid low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("unpaired low surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default:
                --i; /* point at the bad escape character */
                return fail("unknown escape character");
            }
        }
    }

    /* JSON number grammar checked by hand so that strtod extensions (hex,
     * "nan", "inf", leading '+') can never slip through. */
    bool parse_number(double* out) {
        size_t start = i;
        if (i < n && s[i] == '-') ++i;
        if (i >= n || s[i] < '0' || s[i] > '9') return fail("bad number");
        if (s[i] == '0') {
            ++i;
        } else {
            while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
        }
        if (i < n && s[i] == '.') {
            ++i;
            if (i >= n || s[i] < '0' || s[i] > '9') return fail("bad number: digit required after decimal point");
            while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
        }
        if (i < n && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < n && (s[i] == '+' || s[i] == '-')) ++i;
            if (i >= n || s[i] < '0' || s[i] > '9') return fail("bad number: digit required in exponent");
            while (i < n && s[i] >= '0' && s[i] <= '9') ++i;
        }
        std::string tmp(s + start, i - start);
        *out = std::strtod(tmp.c_str(), nullptr);
        if (!std::isfinite(*out)) {
            i = start;
            return fail("number out of range");
        }
        return true;
    }

    bool parse_value(RtJson* out) {
        if (++depth > kMaxDepth) return fail("nesting too deep");
        skip_ws();
        if (i >= n) {
            --depth;
            return fail("unexpected end of input");
        }
        bool ok;
        char c = s[i];
        if (c == '{') {
            ok = parse_object(out);
        } else if (c == '[') {
            ok = parse_array(out);
        } else if (c == '"') {
            out->type = RtJson::Type::String;
            ok = parse_string(&out->str);
        } else if (c == 't') {
            ok = literal("true", 4) ? (out->type = RtJson::Type::Bool, out->boolean = true, true)
                                    : fail("bad literal (expected true)");
        } else if (c == 'f') {
            ok = literal("false", 5) ? (out->type = RtJson::Type::Bool, out->boolean = false, true)
                                     : fail("bad literal (expected false)");
        } else if (c == 'n') {
            ok = literal("null", 4) ? (out->type = RtJson::Type::Null, true)
                                    : fail("bad literal (expected null)");
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            out->type = RtJson::Type::Number;
            ok = parse_number(&out->number);
        } else {
            ok = fail("unexpected character");
        }
        --depth;
        return ok;
    }

    bool parse_object(RtJson* out) {
        out->type = RtJson::Type::Object;
        ++i; /* '{' */
        skip_ws();
        if (i < n && s[i] == '}') {
            ++i;
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (!parse_string(&key)) return false;
            for (const auto& kv : out->obj) {
                if (kv.first == key) {
                    std::string msg = "duplicate key \"" + key + "\"";
                    return fail(msg.c_str());
                }
            }
            skip_ws();
            if (i >= n || s[i] != ':') return fail("expected ':' after object key");
            ++i;
            RtJson v;
            if (!parse_value(&v)) return false;
            out->obj.emplace_back(std::move(key), std::move(v));
            skip_ws();
            if (i >= n) return fail("unterminated object");
            if (s[i] == ',') {
                ++i;
                skip_ws();
                if (i < n && s[i] == '}') return fail("trailing comma in object");
                continue;
            }
            if (s[i] == '}') {
                ++i;
                return true;
            }
            return fail("expected ',' or '}' in object");
        }
    }

    bool parse_array(RtJson* out) {
        out->type = RtJson::Type::Array;
        ++i; /* '[' */
        skip_ws();
        if (i < n && s[i] == ']') {
            ++i;
            return true;
        }
        while (true) {
            RtJson v;
            if (!parse_value(&v)) return false;
            out->arr.push_back(std::move(v));
            skip_ws();
            if (i >= n) return fail("unterminated array");
            if (s[i] == ',') {
                ++i;
                skip_ws();
                if (i < n && s[i] == ']') return fail("trailing comma in array");
                continue;
            }
            if (s[i] == ']') {
                ++i;
                return true;
            }
            return fail("expected ',' or ']' in array");
        }
    }
};

void write_escaped(const std::string& in, std::string* out) {
    out->push_back('"');
    for (unsigned char c : in) {
        switch (c) {
        case '"': *out += "\\\""; break;
        case '\\': *out += "\\\\"; break;
        case '\b': *out += "\\b"; break;
        case '\f': *out += "\\f"; break;
        case '\n': *out += "\\n"; break;
        case '\r': *out += "\\r"; break;
        case '\t': *out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                *out += buf;
            } else {
                out->push_back(char(c)); /* UTF-8 bytes pass through */
            }
        }
    }
    out->push_back('"');
}

void write_number(double v, std::string* out) {
    /* -0.0 prints as 0; nothing in the schema distinguishes them. */
    if (v == 0.0) v = 0.0;
    if (v == std::floor(v) && std::fabs(v) < 9.0e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", (long long)v);
        *out += buf;
        return;
    }
    /* Shortest form that survives a strtod round trip. */
    char buf[40];
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    *out += buf;
}

void write_value(const RtJson& v, int indent, std::string* out) {
    auto pad = [out](int levels) {
        for (int k = 0; k < levels; ++k) *out += "  ";
    };
    switch (v.type) {
    case RtJson::Type::Null:
        *out += "null";
        break;
    case RtJson::Type::Bool:
        *out += v.boolean ? "true" : "false";
        break;
    case RtJson::Type::Number:
        write_number(v.number, out);
        break;
    case RtJson::Type::String:
        write_escaped(v.str, out);
        break;
    case RtJson::Type::Array:
        if (v.arr.empty()) {
            *out += "[]";
            break;
        }
        *out += "[\n";
        for (size_t k = 0; k < v.arr.size(); ++k) {
            pad(indent + 1);
            write_value(v.arr[k], indent + 1, out);
            if (k + 1 < v.arr.size()) *out += ",";
            *out += "\n";
        }
        pad(indent);
        *out += "]";
        break;
    case RtJson::Type::Object:
        if (v.obj.empty()) {
            *out += "{}";
            break;
        }
        *out += "{\n";
        for (size_t k = 0; k < v.obj.size(); ++k) {
            pad(indent + 1);
            write_escaped(v.obj[k].first, out);
            *out += ": ";
            write_value(v.obj[k].second, indent + 1, out);
            if (k + 1 < v.obj.size()) *out += ",";
            *out += "\n";
        }
        pad(indent);
        *out += "}";
        break;
    }
}

} // namespace

const RtJson* RtJson::find(const char* key) const {
    if (type != Type::Object) return nullptr;
    for (const auto& kv : obj) {
        if (kv.first == key) return &kv.second;
    }
    return nullptr;
}

RtJson* RtJson::find(const char* key) {
    return const_cast<RtJson*>(static_cast<const RtJson*>(this)->find(key));
}

RtJson& RtJson::set(const char* key, RtJson v) {
    if (type == Type::Null) type = Type::Object;
    for (auto& kv : obj) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return kv.second;
        }
    }
    obj.emplace_back(key, std::move(v));
    return obj.back().second;
}

RtJson RtJson::make_null() { return RtJson{}; }
RtJson RtJson::make_bool(bool v) {
    RtJson j;
    j.type = Type::Bool;
    j.boolean = v;
    return j;
}
RtJson RtJson::make_number(double v) {
    RtJson j;
    j.type = Type::Number;
    j.number = v;
    return j;
}
RtJson RtJson::make_string(const char* v) {
    RtJson j;
    j.type = Type::String;
    j.str = v;
    return j;
}
RtJson RtJson::make_string(const std::string& v) {
    RtJson j;
    j.type = Type::String;
    j.str = v;
    return j;
}
RtJson RtJson::make_object() {
    RtJson j;
    j.type = Type::Object;
    return j;
}
RtJson RtJson::make_array() {
    RtJson j;
    j.type = Type::Array;
    return j;
}

bool rt_json_parse(const std::string& text, RtJson* out, std::string* err) {
    Parser p;
    p.s = text.data();
    p.n = text.size();
    RtJson v;
    if (!p.parse_value(&v)) {
        if (err) *err = p.err;
        return false;
    }
    p.skip_ws();
    if (p.i != p.n) {
        p.fail("trailing content after value");
        if (err) *err = p.err;
        return false;
    }
    *out = std::move(v);
    return true;
}

std::string rt_json_write(const RtJson& v) {
    std::string out;
    write_value(v, 0, &out);
    out += "\n";
    return out;
}

bool rt_json_write_file(const std::string& path, const std::string& text) {
    std::error_code ec;
    std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    std::string tmp = path + ".tmp";
    std::FILE* f = rt_fopen_utf8(tmp.c_str(), "wb");
    if (!f) {
        rt_log_warn("json", "json: could not open %s for writing: %s", tmp.c_str(), std::strerror(errno));
        return false;
    }
    size_t written = std::fwrite(text.data(), 1, text.size(), f);
    if (written != text.size()) {
        rt_log_error("json", "json: short write to %s: %s", tmp.c_str(), std::strerror(errno));
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

    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        rt_log_warn("json", "json: rename %s -> %s failed: %s", tmp.c_str(), path.c_str(), ec.message().c_str());
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}
