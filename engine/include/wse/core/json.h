#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "wse/core/encoding.h"

namespace wse::core {

inline void wse_append_json_string(std::string& out, std::string_view sv) {
    constexpr char kHex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char uc : sv) {
        switch (uc) {
        case '"':
            out.append("\\\"");
            break;
        case '\\':
            out.append("\\\\");
            break;
        case '\b':
            out.append("\\b");
            break;
        case '\f':
            out.append("\\f");
            break;
        case '\n':
            out.append("\\n");
            break;
        case '\r':
            out.append("\\r");
            break;
        case '\t':
            out.append("\\t");
            break;
        default:
            if (uc < 0x20U) {
                out.append("\\u00");
                out.push_back(kHex[(uc >> 4U) & 0x0fU]);
                out.push_back(kHex[uc & 0x0fU]);
            } else {
                out.push_back(static_cast<char>(uc));
            }
            break;
        }
    }
    out.push_back('"');
}

[[nodiscard]] inline std::string wse_serialize_json_object(
    const std::map<std::string, std::string>& entries, bool pretty) {
    if (entries.empty()) {
        return pretty ? "{}\n" : "{}";
    }
    std::string out;
    out.push_back('{');
    if (pretty) {
        out.push_back('\n');
    }
    bool first = true;
    for (const auto& [key, value] : entries) {
        if (!first) {
            out.push_back(',');
            if (pretty) {
                out.push_back('\n');
            }
        }
        first = false;
        if (pretty) {
            out.append("  ");
        }
        wse_append_json_string(out, key);
        out.push_back(':');
        if (pretty) {
            out.push_back(' ');
        }
        wse_append_json_string(out, value);
    }
    if (pretty) {
        out.append("\n}\n");
    } else {
        out.push_back('}');
    }
    return out;
}

inline void wse_skip_json_ws(std::string_view src, std::size_t& pos) noexcept {
    while (pos < src.size()) {
        const char ch = src[pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
            ++pos;
        } else {
            break;
        }
    }
}

[[nodiscard]] inline bool wse_parse_hex4(std::string_view src,
                                         std::size_t pos,
                                         std::uint32_t& out) noexcept {
    if (pos + 4 > src.size()) {
        return false;
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const int d = wse_hex_nibble(src[pos + i]);
        if (d < 0) {
            return false;
        }
        value = (value << 4U) | static_cast<std::uint32_t>(d);
    }
    out = value;
    return true;
}

[[nodiscard]] inline bool wse_parse_json_string(std::string_view src,
                                                std::size_t& pos,
                                                std::string& out) {
    if (pos >= src.size() || src[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < src.size()) {
        const char ch = src[pos++];
        if (ch == '"') {
            return true;
        }
        if (static_cast<unsigned char>(ch) < 0x20U) {
            return false;
        }
        if (ch != '\\') {
            out.push_back(ch);
            continue;
        }
        if (pos >= src.size()) {
            return false;
        }
        const char esc = src[pos++];
        switch (esc) {
        case '"':
        case '\\':
        case '/':
            out.push_back(esc);
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u': {
            std::uint32_t cp = 0;
            if (!wse_parse_hex4(src, pos, cp)) {
                return false;
            }
            pos += 4;
            if (cp >= 0xd800U && cp <= 0xdbffU && pos + 6 <= src.size() &&
                src[pos] == '\\' && src[pos + 1] == 'u') {
                std::uint32_t low = 0;
                if (wse_parse_hex4(src, pos + 2, low) && low >= 0xdc00U &&
                    low <= 0xdfffU) {
                    pos += 6;
                    cp = 0x10000U +
                         (((cp - 0xd800U) << 10U) | (low - 0xdc00U));
                }
            }
            wse_append_utf8_codepoint(out, cp);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

[[nodiscard]] inline bool wse_parse_json_scalar(std::string_view src,
                                                std::size_t& pos,
                                                std::string& out) {
    if (pos >= src.size()) {
        return false;
    }
    if (src[pos] == '"') {
        return wse_parse_json_string(src, pos, out);
    }
    if (src.substr(pos).starts_with("true")) {
        pos += 4;
        out = "true";
        return true;
    }
    if (src.substr(pos).starts_with("false")) {
        pos += 5;
        out = "false";
        return true;
    }
    if (src.substr(pos).starts_with("null")) {
        pos += 4;
        out.clear();
        return true;
    }
    const std::size_t start = pos;
    if (src[pos] == '-') {
        ++pos;
    }
    if (pos >= src.size()) {
        return false;
    }
    if (src[pos] == '0') {
        ++pos;
    } else if (src[pos] >= '1' && src[pos] <= '9') {
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
            ++pos;
        }
    } else {
        return false;
    }
    if (pos < src.size() && src[pos] == '.') {
        ++pos;
        if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
            return false;
        }
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
            ++pos;
        }
    }
    if (pos < src.size() && (src[pos] == 'e' || src[pos] == 'E')) {
        ++pos;
        if (pos < src.size() && (src[pos] == '+' || src[pos] == '-')) {
            ++pos;
        }
        if (pos >= src.size() || src[pos] < '0' || src[pos] > '9') {
            return false;
        }
        while (pos < src.size() && src[pos] >= '0' && src[pos] <= '9') {
            ++pos;
        }
    }
    out.assign(src.substr(start, pos - start));
    return true;
}

// Parses top-level JSON object (0x1402a97f0 requires Json::objectValue).
// Validates and steps over one JSON value of any kind (nested arrays/objects
// included, up to `max_depth` levels) without storing it.
[[nodiscard]] inline bool wse_skip_json_value(std::string_view src,
                                              std::size_t& pos,
                                              int max_depth = 64) {
    wse_skip_json_ws(src, pos);
    if (pos >= src.size()) {
        return false;
    }
    const char open = src[pos];
    if (open != '{' && open != '[') {
        std::string ignored;
        return wse_parse_json_scalar(src, pos, ignored);
    }
    if (max_depth <= 0) {
        return false;
    }
    const char close = (open == '{') ? '}' : ']';
    ++pos;
    wse_skip_json_ws(src, pos);
    if (pos < src.size() && src[pos] == close) {
        ++pos;
        return true;
    }
    for (;;) {
        wse_skip_json_ws(src, pos);
        if (open == '{') {
            std::string key;
            if (!wse_parse_json_string(src, pos, key)) {
                return false;
            }
            wse_skip_json_ws(src, pos);
            if (pos >= src.size() || src[pos] != ':') {
                return false;
            }
            ++pos;
        }
        if (!wse_skip_json_value(src, pos, max_depth - 1)) {
            return false;
        }
        wse_skip_json_ws(src, pos);
        if (pos >= src.size()) {
            return false;
        }
        if (src[pos] == close) {
            ++pos;
            return true;
        }
        if (src[pos] != ',') {
            return false;
        }
        ++pos;
    }
}

// Parses a flat object of scalar values into `out` (strings unescaped,
// numbers/booleans as their text, null as ""). With `skip_nested`, members whose
// value is an array or object are validated and left out instead of failing the
// parse (for foreign files such as driver `meta.json` that may grow structured
// fields); the WSE dict ops keep the strict default.
[[nodiscard]] inline bool wse_parse_json_object(
    std::string_view src, std::map<std::string, std::string>& out, bool skip_nested = false) {
    if (src.starts_with("\xEF\xBB\xBF")) {
        src.remove_prefix(3);
    }
    std::size_t pos = 0;
    wse_skip_json_ws(src, pos);
    if (pos >= src.size() || src[pos] != '{') {
        return false;
    }
    ++pos;
    std::map<std::string, std::string> parsed;
    wse_skip_json_ws(src, pos);
    if (pos < src.size() && src[pos] == '}') {
        ++pos;
        wse_skip_json_ws(src, pos);
        if (pos != src.size()) {
            return false;
        }
        out = std::move(parsed);
        return true;
    }
    for (;;) {
        wse_skip_json_ws(src, pos);
        std::string key;
        if (!wse_parse_json_string(src, pos, key)) {
            return false;
        }
        wse_skip_json_ws(src, pos);
        if (pos >= src.size() || src[pos] != ':') {
            return false;
        }
        ++pos;
        wse_skip_json_ws(src, pos);
        if (skip_nested && pos < src.size() && (src[pos] == '{' || src[pos] == '[')) {
            if (!wse_skip_json_value(src, pos)) {
                return false;
            }
        } else {
            std::string value;
            if (!wse_parse_json_scalar(src, pos, value)) {
                return false;
            }
            parsed[std::move(key)] = std::move(value);
        }
        wse_skip_json_ws(src, pos);
        if (pos >= src.size()) {
            return false;
        }
        if (src[pos] == '}') {
            ++pos;
            break;
        }
        if (src[pos] != ',') {
            return false;
        }
        ++pos;
    }
    wse_skip_json_ws(src, pos);
    if (pos != src.size()) {
        return false;
    }
    out = std::move(parsed);
    return true;
}

}  // namespace wse::core
