// Shared minimal UTF-8 XML DOM parser for Warband configuration assets
// (`Data/skeleton_bodies.xml`, `Data/font_data.xml`).
//
// Port infrastructure, not a reconstruction: carries no PE/PDB citations.
#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wse::io {

struct XmlElementNode {
    std::string tag{};
    std::unordered_map<std::string, std::string> attrs{};
    std::vector<XmlElementNode> children{};
};

namespace xml_detail {

inline constexpr int kMaxXmlDepth = 32;

inline void skip_ws(std::string_view xml, std::size_t& pos) noexcept {
    while (pos < xml.size() && static_cast<unsigned char>(xml[pos]) <= 0x20U) {
        ++pos;
    }
}

[[nodiscard]] inline std::string unescape_xml_entities(std::string_view raw) {
    if (raw.find('&') == std::string_view::npos) {
        return std::string(raw);
    }
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '&') {
            const std::string_view rest = raw.substr(i);
            if (rest.rfind("&lt;", 0) == 0) {
                out.push_back('<');
                i += 3U;
                continue;
            }
            if (rest.rfind("&gt;", 0) == 0) {
                out.push_back('>');
                i += 3U;
                continue;
            }
            if (rest.rfind("&amp;", 0) == 0) {
                out.push_back('&');
                i += 4U;
                continue;
            }
            if (rest.rfind("&quot;", 0) == 0) {
                out.push_back('"');
                i += 5U;
                continue;
            }
            if (rest.rfind("&apos;", 0) == 0) {
                out.push_back('\'');
                i += 5U;
                continue;
            }
        }
        out.push_back(raw[i]);
    }
    return out;
}

[[nodiscard]] inline bool parse_f32_sv(std::string_view s, float& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size() && std::isfinite(out);
}

[[nodiscard]] inline bool parse_i32_sv(std::string_view s, std::int32_t& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

inline bool skip_prolog_or_comment(std::string_view xml,
                                   std::size_t& pos,
                                   std::string* error_out) {
    while (pos < xml.size()) {
        skip_ws(xml, pos);
        if (pos + 1U < xml.size() && xml.substr(pos, 2U) == "<?") {
            const std::size_t end_pi = xml.find("?>", pos + 2U);
            if (end_pi == std::string_view::npos) {
                if (error_out) *error_out = "unterminated XML processing instruction";
                return false;
            }
            pos = end_pi + 2U;
            continue;
        }
        if (pos + 3U < xml.size() && xml.substr(pos, 4U) == "<!--") {
            const std::size_t end_cmt = xml.find("-->", pos + 4U);
            if (end_cmt == std::string_view::npos) {
                if (error_out) *error_out = "unterminated XML comment";
                return false;
            }
            pos = end_cmt + 3U;
            continue;
        }
        break;
    }
    return true;
}

inline bool parse_xml_element(std::string_view xml,
                              std::size_t& pos,
                              XmlElementNode& out,
                              int depth,
                              std::string* error_out) {
    if (depth > kMaxXmlDepth) {
        if (error_out) *error_out = "XML nesting exceeds depth limit";
        return false;
    }
    if (pos >= xml.size() || xml[pos] != '<') {
        if (error_out) *error_out = "expected '<' in XML";
        return false;
    }
    ++pos;
    const std::size_t tag_start = pos;
    while (pos < xml.size() && static_cast<unsigned char>(xml[pos]) > 0x20U &&
           xml[pos] != '>' && xml[pos] != '/') {
        ++pos;
    }
    out.tag = std::string(xml.substr(tag_start, pos - tag_start));
    if (out.tag.empty()) {
        if (error_out) *error_out = "empty XML tag name";
        return false;
    }

    bool self_closing = false;
    while (pos < xml.size()) {
        skip_ws(xml, pos);
        if (pos >= xml.size()) {
            if (error_out) *error_out = "unterminated XML open tag";
            return false;
        }
        if (xml[pos] == '>') {
            ++pos;
            break;
        }
        if (pos + 1U < xml.size() && xml[pos] == '/' && xml[pos + 1U] == '>') {
            pos += 2U;
            self_closing = true;
            break;
        }
        const std::size_t kstart = pos;
        while (pos < xml.size() && static_cast<unsigned char>(xml[pos]) > 0x20U &&
               xml[pos] != '=' && xml[pos] != '>' && xml[pos] != '/') {
            ++pos;
        }
        const std::string key(xml.substr(kstart, pos - kstart));
        skip_ws(xml, pos);
        if (pos >= xml.size() || xml[pos] != '=') {
            if (error_out) *error_out = "expected '=' after XML attribute " + key;
            return false;
        }
        ++pos;
        skip_ws(xml, pos);
        if (pos >= xml.size() || (xml[pos] != '"' && xml[pos] != '\'')) {
            if (error_out) *error_out = "expected quoted attribute value for " + key;
            return false;
        }
        const char quote = xml[pos++];
        const std::size_t vstart = pos;
        const std::size_t vend = xml.find(quote, vstart);
        if (vend == std::string_view::npos) {
            if (error_out) *error_out = "unterminated XML attribute string";
            return false;
        }
        out.attrs[key] = unescape_xml_entities(xml.substr(vstart, vend - vstart));
        pos = vend + 1U;
    }

    if (self_closing) {
        return true;
    }

    while (pos < xml.size()) {
        const std::size_t lt = xml.find('<', pos);
        if (lt == std::string_view::npos) {
            if (error_out) *error_out = "unclosed XML element <" + out.tag + ">";
            return false;
        }
        pos = lt;
        if (pos + 3U < xml.size() && xml.substr(pos, 4U) == "<!--") {
            const std::size_t end_cmt = xml.find("-->", pos + 4U);
            if (end_cmt == std::string_view::npos) {
                if (error_out) *error_out = "unterminated XML comment";
                return false;
            }
            pos = end_cmt + 3U;
            continue;
        }
        if (pos + 1U < xml.size() && xml[pos + 1U] == '/') {
            pos += 2U;
            const std::size_t cstart = pos;
            while (pos < xml.size() && static_cast<unsigned char>(xml[pos]) > 0x20U &&
                   xml[pos] != '>') {
                ++pos;
            }
            const std::string_view close_tag = xml.substr(cstart, pos - cstart);
            skip_ws(xml, pos);
            if (pos >= xml.size() || xml[pos] != '>' || close_tag != out.tag) {
                if (error_out) *error_out = "mismatched closing tag for <" + out.tag + ">";
                return false;
            }
            ++pos;
            return true;
        }
        XmlElementNode child{};
        if (!parse_xml_element(xml, pos, child, depth + 1, error_out)) {
            return false;
        }
        out.children.push_back(std::move(child));
    }

    if (error_out) *error_out = "unexpected EOF inside <" + out.tag + ">";
    return false;
}

}  // namespace xml_detail

[[nodiscard]] inline bool parse_xml_document(std::string_view xml,
                                             XmlElementNode& out_root,
                                             std::string* error_out = nullptr) {
    out_root = {};
    std::size_t pos = 0;
    if (xml.size() >= 3U && static_cast<unsigned char>(xml[0]) == 0xEFU &&
        static_cast<unsigned char>(xml[1]) == 0xBBU &&
        static_cast<unsigned char>(xml[2]) == 0xBFU) {
        pos = 3U;
    }
    if (!xml_detail::skip_prolog_or_comment(xml, pos, error_out)) {
        return false;
    }
    if (pos >= xml.size()) {
        if (error_out) *error_out = "empty XML document";
        return false;
    }
    return xml_detail::parse_xml_element(xml, pos, out_root, 0, error_out);
}

[[nodiscard]] inline const XmlElementNode* find_xml_child(const XmlElementNode& node,
                                                          std::string_view tag) noexcept {
    for (const XmlElementNode& c : node.children) {
        if (c.tag == tag) {
            return &c;
        }
    }
    return nullptr;
}

inline void read_xml_attr_str(const XmlElementNode& node,
                              const char* key,
                              std::string& dst) {
    const auto it = node.attrs.find(key);
    if (it != node.attrs.end()) {
        dst = it->second;
    }
}

// The attribute readers below leave `dst` untouched when the attribute is absent or
// malformed, so callers pre-load their defaults. They are not `noexcept`: looking up a
// `const char*` key in the `std::string`-keyed map builds a temporary string.
[[nodiscard]] inline bool read_xml_attr_i32(const XmlElementNode& node,
                                            const char* key,
                                            std::int32_t& dst) {
    const auto it = node.attrs.find(key);
    std::int32_t val = 0;
    if (it != node.attrs.end() && xml_detail::parse_i32_sv(it->second, val)) {
        dst = val;
        return true;
    }
    return false;
}

inline void read_xml_attr_f32(const XmlElementNode& node,
                              const char* key,
                              float& dst,
                              float scale = 1.0f) {
    const auto it = node.attrs.find(key);
    float val = 0.0f;
    if (it != node.attrs.end() && xml_detail::parse_f32_sv(it->second, val)) {
        dst = val * scale;
    }
}

inline void read_xml_attr_flag_bit(const XmlElementNode& node,
                                   const char* key,
                                   bool& dst) {
    std::int32_t val = 0;
    if (read_xml_attr_i32(node, key, val)) {
        dst = (val != 0);
        return;
    }
    const auto it = node.attrs.find(key);
    if (it == node.attrs.end()) {
        return;
    }
    if (it->second == "true" || it->second == "TRUE") {
        dst = true;
    } else if (it->second == "false" || it->second == "FALSE") {
        dst = false;
    }
}

}  // namespace wse::io
