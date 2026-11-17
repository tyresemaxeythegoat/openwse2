// Clean-room v1.1.5.5 bitmap font loader, glyph metric calculator, word wrapper,
// and quad text layout engine (`Data/font_data.xml` + `Textures/font.dds`).
//
// Reconstructed from v1.1.5.5 PE + PDB evidence (`mb_warband_wse2_x64.pdb`):
// - rglFont          size 0x3890 (14480 B), +0x00 m_width, +0x04 m_height,
//                    +0x08 m_padding, +0x0c m_fontSize, +0x10 m_fontWidth,
//                    +0x14 m_fontHeight, +0x18 m_fontScale, +0x1c m_lineSpacing,
//                    +0x20 m_baseSize, +0x24 m_baseScale, +0x28 m_defaultGlyph,
//                    +0x48 m_unicodeGlyphs, +0x88 m_glyphs[512], +0x3888 m_tabSize.
// - rglFontGlyph     size 0x1c (28 B), +0x00 m_uv (x, y), +0x08 m_size (u2, v2),
//                    +0x10 m_preShift, +0x14 m_yAdjust, +0x18 m_postShift.
// - rglFont::readFile      (0x1402978c0), rglFont::measureText (0x14029b4f0),
//   rglFont::drawGlyph     (0x14029b670), rglFont::drawText    (0x140297fa0).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wse/io/xml.h"
#include "wse/platform/vfs.h"

namespace wse::asset {

// Text alignment and layout flags decoded in `rglFont::drawText` (`0x140297fa0`).
inline constexpr std::uint32_t kTextFlagRightAlign = 0x00000008U;
inline constexpr std::uint32_t kTextFlagCenterAlign = 0x00000010U;
inline constexpr std::uint32_t kTextFlagBoxWrap = 0x00000100U;
inline constexpr std::uint32_t kTextFlagVCenterOrLeft = 0x00000200U;
inline constexpr std::uint32_t kTextFlagParagraphSpacing = 0x00000800U;
inline constexpr std::uint32_t kTextFlagSingleLine = 0x00004000U;
inline constexpr std::uint32_t kTextFlagClampOverflow = 0x00010000U;

struct FontGlyph {
    float uv_x = 0.0f;       // +0x00 m_uv.x: left U in [0, 1]
    float uv_y = 0.0f;       // +0x04 m_uv.y: Warband bottom-up V of top edge
    float u_right = 0.0f;    // +0x08 m_size.x: right U in [0, 1]
    float v_bottom = 0.0f;   // +0x0c m_size.y: Warband bottom-up V of bottom edge
    float preshift = 0.0f;   // +0x10 m_preShift: horizontal bearing before glyph / width
    float yadjust = 0.0f;    // +0x14 m_yAdjust: baseline-to-top ascent / height
    float postshift = 0.0f;  // +0x18 m_postShift: advance after glyph / width

    [[nodiscard]] bool has_quad() const noexcept {
        return u_right > uv_x && uv_y > v_bottom;
    }
};

// Emitted glyph quad in Warband Y-up `[0, 1] x [0, 0.75]` UI space and
// Warband bottom-up UV coordinates (`v_dds = 1.0f - v` maps to top-down DDS rows).
struct FontQuad {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float u0 = 0.0f;
    float v0 = 0.0f;  // UV V at y0 (bottom edge)
    float u1 = 0.0f;
    float v1 = 0.0f;  // UV V at y1 (top edge)
    std::uint32_t color_argb = 0xFFFFFFFFU;
};

struct TextLayoutResult {
    std::vector<FontQuad> quads{};
    float width = 0.0f;
    float height = 0.0f;
};

namespace font_detail {

inline void decode_utf8(std::string_view utf8, std::vector<std::int32_t>& out_cp) {
    out_cp.clear();
    out_cp.reserve(utf8.size());
    const std::size_t n = utf8.size();
    std::size_t i = 0;
    while (i < n) {
        const auto b0 = static_cast<std::uint8_t>(utf8[i]);
        if (b0 < 0x80U) {
            out_cp.push_back(static_cast<std::int32_t>(b0));
            ++i;
            continue;
        }
        std::uint32_t cp = 0x3FU;  // '?' replacement on malformed sequence
        std::size_t len = 1;
        if ((b0 & 0xE0U) == 0xC0U && i + 1U < n) {
            const auto b1 = static_cast<std::uint8_t>(utf8[i + 1U]);
            if ((b1 & 0xC0U) == 0x80U) {
                cp = ((b0 & 0x1FU) << 6U) | (b1 & 0x3FU);
                len = 2U;
            }
        } else if ((b0 & 0xF0U) == 0xE0U && i + 2U < n) {
            const auto b1 = static_cast<std::uint8_t>(utf8[i + 1U]);
            const auto b2 = static_cast<std::uint8_t>(utf8[i + 2U]);
            if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U) {
                cp = ((b0 & 0x0FU) << 12U) | ((b1 & 0x3FU) << 6U) | (b2 & 0x3FU);
                len = 3U;
            }
        } else if ((b0 & 0xF8U) == 0xF0U && i + 3U < n) {
            const auto b1 = static_cast<std::uint8_t>(utf8[i + 1U]);
            const auto b2 = static_cast<std::uint8_t>(utf8[i + 2U]);
            const auto b3 = static_cast<std::uint8_t>(utf8[i + 3U]);
            if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U && (b3 & 0xC0U) == 0x80U) {
                cp = ((b0 & 0x07U) << 18U) | ((b1 & 0x3FU) << 12U) |
                     ((b2 & 0x3FU) << 6U) | (b3 & 0x3FU);
                len = 4U;
            }
        }
        out_cp.push_back(static_cast<std::int32_t>(cp));
        i += len;
    }
}

// Parses `<c=...>` starting at index `i` (which points at '<'). On success
// advances `i` past '>' and sets `out_color_argb` (with alpha forced to 0xFF per
// `0x1402984cd`).
inline bool try_parse_color_tag(const std::vector<std::int32_t>& cps,
                                std::size_t end_idx,
                                std::size_t& i,
                                std::uint32_t& out_color_argb) {
    if (i + 3U >= end_idx || cps[i] != '<' || cps[i + 1U] != 'c' || cps[i + 2U] != '=') {
        return false;
    }
    std::string hex_buf;
    std::size_t j = i + 3U;
    while (j < end_idx && cps[j] != '>') {
        if (cps[j] >= 0 && cps[j] < 128) {
            hex_buf.push_back(static_cast<char>(cps[j]));
        }
        ++j;
    }
    if (j >= end_idx) {
        return false;
    }
    if (hex_buf.empty()) {
        out_color_argb = 0xFF000000U;
    } else {
        char* endp = nullptr;
        unsigned long parsed = std::strtoul(hex_buf.c_str(), &endp, 0);
        out_color_argb = static_cast<std::uint32_t>(parsed) | 0xFF000000U;
    }
    i = j;  // Caller loop increments i once more past '>'.
    return true;
}

inline bool is_close_color_tag(const std::vector<std::int32_t>& cps,
                               std::size_t end_idx,
                               std::size_t& i) noexcept {
    if (i + 3U < end_idx && cps[i] == '<' && cps[i + 1U] == '/' &&
        cps[i + 2U] == 'c' && cps[i + 3U] == '>') {
        i += 3U;  // Caller loop increments i once more past '>'.
        return true;
    }
    return false;
}

}  // namespace font_detail

class FontData {
public:
    static constexpr std::size_t kAsciiTableSize = 512U;

    std::int32_t width = 2048;
    std::int32_t height = 1024;
    std::int32_t padding = 10;
    std::int32_t font_size = 75;
    std::int32_t base_size = 75;
    std::int32_t base_scale = 100;
    float font_width = 0.0f;
    float font_height = 0.0f;
    float font_scale = 1.0f;
    float line_spacing = 1.0f;
    float tab_size = 0.0f;
    FontGlyph default_glyph{};
    std::array<FontGlyph, kAsciiTableSize> glyphs{};
    std::unordered_map<std::int32_t, FontGlyph> unicode_glyphs{};

    [[nodiscard]] const FontGlyph& get_glyph(std::int32_t code) const noexcept {
        if (code >= 0 && static_cast<std::size_t>(code) < kAsciiTableSize) {
            return glyphs[static_cast<std::size_t>(code)];
        }
        const auto it = unicode_glyphs.find(code);
        return (it != unicode_glyphs.end()) ? it->second : default_glyph;
    }

    // Loads and parses `Data/font_data.xml` mirroring `rglFont::readFile` (`0x1402978c0`).
    [[nodiscard]] bool parse_xml(std::string_view xml, std::string* error_out = nullptr) {
        *this = FontData{};
        io::XmlElementNode root{};
        if (!io::parse_xml_document(xml, root, error_out)) {
            return false;
        }
        const io::XmlElementNode* font_el =
            (root.tag == "FontData") ? &root : io::find_xml_child(root, "FontData");
        if (font_el == nullptr) {
            if (error_out) *error_out = "missing <FontData> root element";
            return false;
        }

        std::int32_t line_spacing_int = 100;
        (void)io::read_xml_attr_i32(*font_el, "padding", padding);
        (void)io::read_xml_attr_i32(*font_el, "width", width);
        (void)io::read_xml_attr_i32(*font_el, "height", height);
        (void)io::read_xml_attr_i32(*font_el, "font_scale", base_scale);
        (void)io::read_xml_attr_i32(*font_el, "line_spacing", line_spacing_int);
        (void)io::read_xml_attr_i32(*font_el, "font_size", base_size);

        // Safety deviation: a non-positive `font_scale` would make every layout scale 0 and
        // the tab-stop math divide by it, so it is rejected like a bad atlas size.
        if (width <= 0 || height <= 0 || base_size <= 0 || base_scale <= 0) {
            if (error_out) *error_out = "font_data.xml has non-positive atlas, font_size, or font_scale";
            return false;
        }

        font_size = base_size;
        font_scale = static_cast<float>(base_scale) * 0.01f;
        font_width = static_cast<float>(font_size) / static_cast<float>(width);
        font_height = static_cast<float>(font_size) / static_cast<float>(height);
        line_spacing = static_cast<float>(line_spacing_int) * 0.01f;

        const io::XmlElementNode* details = io::find_xml_child(*font_el, "FontDetails");
        if (details == nullptr) {
            if (error_out) *error_out = "missing <FontDetails> element in font_data.xml";
            return false;
        }

        const float fw = static_cast<float>(width);
        const float fh = static_cast<float>(height);
        for (const io::XmlElementNode& ch : details->children) {
            if (ch.tag != "character") {
                continue;
            }
            std::int32_t code = 0;
            if (!io::read_xml_attr_i32(ch, "code", code) || code < 0) {
                continue;
            }
            FontGlyph g{};
            std::int32_t attr = 0;
            if (io::read_xml_attr_i32(ch, "u", attr)) {
                g.uv_x = static_cast<float>(attr - 4) / fw;
            }
            if (io::read_xml_attr_i32(ch, "v", attr)) {
                g.uv_y = static_cast<float>(height - attr + 4) / fh;
            }
            if (io::read_xml_attr_i32(ch, "w", attr)) {
                g.u_right = static_cast<float>(attr - padding + 6) / fw;
            }
            if (io::read_xml_attr_i32(ch, "h", attr)) {
                g.v_bottom = static_cast<float>(height - attr + padding - 6) / fh;
            }
            if (io::read_xml_attr_i32(ch, "preshift", attr)) {
                g.preshift = static_cast<float>(attr - 4) / fw;
            }
            if (io::read_xml_attr_i32(ch, "yadjust", attr)) {
                g.yadjust = static_cast<float>(attr + 4) / fh;
            }
            if (io::read_xml_attr_i32(ch, "postshift", attr)) {
                g.postshift = static_cast<float>(attr + 4) / fw;
            }
            if (static_cast<std::size_t>(code) < kAsciiTableSize) {
                glyphs[static_cast<std::size_t>(code)] = g;
            } else {
                unicode_glyphs[code] = g;
            }
        }

        const FontGlyph& space = get_glyph(32);
        tab_size = space.preshift + space.postshift;
        default_glyph = get_glyph('?');
        return true;
    }

    // Computes unscaled glyph advance width (see `rglFont::measureText` `0x14029b4f0`)
    // over codepoint slice `[start, end)`, skipping `<c=...>` and `</c>` tags.
    [[nodiscard]] float measure_span_raw(const std::vector<std::int32_t>& cps,
                                         std::size_t start,
                                         std::size_t end) const noexcept {
        float w = 0.0f;
        const std::size_t clamped_end = std::min(end, cps.size());
        for (std::size_t i = start; i < clamped_end; ++i) {
            std::uint32_t dummy_color = 0;
            if (cps[i] == '<') {
                if (font_detail::try_parse_color_tag(cps, clamped_end, i, dummy_color) ||
                    font_detail::is_close_color_tag(cps, clamped_end, i)) {
                    continue;
                }
            }
            if (cps[i] == '\n') {
                continue;
            }
            if (cps[i] == '\t') {
                const float stepped = std::floor(w * 4.0f) * 0.25f + 0.25f + tab_size * 0.25f;
                w = stepped;
                continue;
            }
            const FontGlyph& g = get_glyph(cps[i]);
            w += g.preshift + g.postshift;
        }
        return w;
    }

    // Lays out `utf8_text` inside horizontal `[min_x, max_x]` starting from baseline
    // `top_y` (`rglFont::drawText` `0x140297fa0` + `rglFont::drawGlyph` `0x14029b670`).
    [[nodiscard]] TextLayoutResult layout_text(std::string_view utf8_text,
                                               float min_x,
                                               float max_x,
                                               float top_y,
                                               float size_x,
                                               float size_y,
                                               std::uint32_t flags = 0U,
                                               std::uint32_t default_color_argb = 0xFFFFFFFFU) const {
        TextLayoutResult out{};
        if (font_width <= 0.0f || font_height <= 0.0f || !(font_scale > 0.0f) ||
            size_x <= 0.0f || size_y <= 0.0f || utf8_text.empty()) {
            return out;
        }

        std::vector<std::int32_t> cps;
        font_detail::decode_utf8(utf8_text, cps);
        if (cps.empty()) {
            return out;
        }

        const float scale_x = (size_x * font_scale) / font_width;
        const float scale_y = (size_y * font_scale) / font_height;
        const float avail_w = std::max(0.0f, max_x - min_x);
        const float space_w = (get_glyph(' ').preshift + get_glyph(' ').postshift) * scale_x;
        const float pad_pad = ((flags & kTextFlagClampOverflow) != 0U) ? (space_w * 0.5f) : 0.0f;
        const float line_adv = size_y * 0.9f * font_scale * line_spacing *
                               (((flags & kTextFlagParagraphSpacing) != 0U) ? 1.2f : 1.0f);

        std::uint32_t active_color = default_color_argb;
        float cursor_y = top_y;
        // Widest laid-out line, measured from each line's own start pen. Measuring from
        // `min_x` instead under-reports centred/right-aligned text (`measure_text` passes
        // `max_x == min_x`, which places those lines left of `min_x`).
        float max_line_w = 0.0f;
        std::size_t line_start = 0U;

        const bool single_line = (flags & kTextFlagSingleLine) != 0U;
        const bool center_align = ((flags & kTextFlagCenterAlign) != 0U) &&
                                  !(((flags & kTextFlagVCenterOrLeft) != 0U) &&
                                    ((flags & kTextFlagBoxWrap) == 0U));
        const bool right_align = ((flags & kTextFlagRightAlign) != 0U) && !center_align &&
                                 !(((flags & kTextFlagVCenterOrLeft) != 0U) &&
                                   ((flags & kTextFlagBoxWrap) == 0U));

        while (line_start < cps.size()) {
            std::size_t line_end = line_start;
            float line_w = 0.0f;
            // `avail_w <= 0` (no box, e.g. `measure_text` with `max_width == 0`) disables
            // word wrapping but still breaks lines at '\n'.
            const bool wrap = avail_w > 0.0f;
            if (single_line) {
                line_end = cps.size();
                line_w = measure_span_raw(cps, line_start, line_end) * scale_x + pad_pad;
            } else {
                bool first_word = true;
                std::size_t cur = line_start;
                while (cur < cps.size()) {
                    std::size_t word_end = cur;
                    bool hit_newline = false;
                    while (word_end < cps.size()) {
                        const std::int32_t c = cps[word_end];
                        ++word_end;
                        if (c == '\n') {
                            hit_newline = true;
                            break;
                        }
                        if (c == ' ' || c == '\t') {
                            break;
                        }
                    }
                    const float cand_w =
                        measure_span_raw(cps, line_start, word_end) * scale_x + pad_pad;
                    if (wrap && !first_word && cand_w > avail_w) {
                        break;
                    }
                    line_end = word_end;
                    line_w = cand_w;
                    cur = word_end;
                    first_word = false;
                    if (hit_newline) {
                        break;
                    }
                }
            }

            float pen_x = min_x;
            if (center_align) {
                pen_x = (min_x + max_x) * 0.5f - line_w * 0.5f;
            } else if (right_align) {
                pen_x = max_x - line_w;
            }
            const float start_pen_x = pen_x;

            for (std::size_t i = line_start; i < line_end; ++i) {
                const std::int32_t c = cps[i];
                if (c == '<') {
                    std::uint32_t tag_col = 0;
                    if (font_detail::try_parse_color_tag(cps, line_end, i, tag_col)) {
                        active_color = tag_col;
                        continue;
                    }
                    if (font_detail::is_close_color_tag(cps, line_end, i)) {
                        active_color = default_color_argb;
                        continue;
                    }
                }
                if (c == '\n') {
                    continue;
                }
                if (c == '\t') {
                    const float rel = (pen_x - start_pen_x) / scale_x;
                    const float stepped =
                        std::floor(rel * 4.0f) * 0.25f + 0.25f + tab_size * 0.25f;
                    pen_x = start_pen_x + stepped * scale_x;
                    continue;
                }
                const FontGlyph& g = get_glyph(c);
                pen_x += g.preshift * scale_x;
                if (g.has_quad()) {
                    const float qx0 = pen_x;
                    const float qx1 = pen_x + (g.u_right - g.uv_x) * scale_x;
                    const float qy1 = cursor_y + g.yadjust * scale_y;
                    const float qy0 = qy1 - (g.uv_y - g.v_bottom) * scale_y;
                    out.quads.push_back(FontQuad{
                        qx0,
                        qy0,
                        qx1,
                        qy1,
                        g.uv_x,
                        g.v_bottom,
                        g.u_right,
                        g.uv_y,
                        active_color,
                    });
                }
                pen_x += g.postshift * scale_x;
            }
            max_line_w = std::max(max_line_w, pen_x - start_pen_x);
            cursor_y -= line_adv;
            line_start = line_end;
        }

        out.width = max_line_w;
        out.height = std::max(0.0f, top_y - cursor_y);
        return out;
    }

    // Computes layout bounding size without emitting quads (`rglFont::measureText` `0x14029b4f0`).
    [[nodiscard]] std::pair<float, float> measure_text(std::string_view utf8_text,
                                                       float max_width,
                                                       float size_x,
                                                       float size_y,
                                                       std::uint32_t flags = 0U) const {
        const TextLayoutResult res =
            layout_text(utf8_text, 0.0f, max_width, 0.0f, size_x, size_y, flags);
        return {res.width, res.height};
    }
};

[[nodiscard]] inline bool load_font_data(platform::Vfs& vfs,
                                         std::string_view path,
                                         FontData& out,
                                         std::string* error_out = nullptr) {
    std::vector<std::uint8_t> raw;
    if (!vfs.read_file(path, raw)) {
        if (error_out) *error_out = "failed to read font XML: " + std::string(path);
        return false;
    }
    const std::string_view xml(reinterpret_cast<const char*>(raw.data()), raw.size());
    return out.parse_xml(xml, error_out);
}

}  // namespace wse::asset
