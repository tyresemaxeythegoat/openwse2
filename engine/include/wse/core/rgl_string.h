// Reconstructed value and token semantics for retail `rglString` (Phase 2G).
//
// Evidence (WSE2 v1.1.5.5 PE + PDB):
//   rglString            PDB type 0x35f5, 72 B (`docs/ARCHITECTURE.md` §4):
//                          +0x00 int32_t  m_cursor
//                          +0x08 char*    m_buffer (points to static or heap storage)
//                          +0x10 int32_t  m_bufferLength (inline capacity 48)
//                          +0x14 int32_t  m_strLength
//                          +0x18 char[48] m_staticBuffer
//   getNextToken         0x14030f5d0: skips bytes <= 0x20, extracts maximal run of
//                          bytes > 0x20, and advances m_cursor past the token.
//   reallocate           0x14030f720: grows buffer on heap once length >= 48.
//   swapUnderscoresWithSpaces 0x140310000: in-place '_' -> ' '.
//   swapSpacesWithUnderscores 0x140310030: in-place ' ' -> '_'.
//   toInt64Stream        0x140310490: decimal integer parse from current string.
//   toUInt64Stream       0x140310570: unsigned decimal integer parse.
//   Savegame log string  mbGame::saveGame 0x1400ca120..0x1400ca157 writes "-1"
//                          when empty and replaces ' ' with '_' otherwise;
//                          mbGame::loadGame 0x1400c8c86..0x1400c8cf3 reverses it.

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace wse::core {

// Portable value type matching retail `rglString` SSO threshold (48 inline bytes,
// switching to heap above 47 payload bytes + null terminator), token cursor, and
// whitespace/space-underscore/savegame sentinel transformations.
class RglString {
public:
    static constexpr std::size_t kInlineCapacity = 48;

    RglString() noexcept = default;
    explicit RglString(std::string_view text) { assign(text); }

    RglString(const RglString&) = default;
    RglString& operator=(const RglString&) = default;

    // A defaulted move would empty `heap_` but keep the source's heap `capacity_`, so the
    // next `assign`/`append` on the moved-from object skipped `reserve_buffer` and wrote
    // past the 48-byte inline buffer. Moves reset the source to an empty inline string.
    RglString(RglString&& other) noexcept
        : cursor_(other.cursor_),
          length_(other.length_),
          capacity_(other.capacity_),
          inline_(other.inline_),
          heap_(std::move(other.heap_)) {
        other.reset_to_empty_inline();
    }

    RglString& operator=(RglString&& other) noexcept {
        if (this != &other) {
            cursor_ = other.cursor_;
            length_ = other.length_;
            capacity_ = other.capacity_;
            inline_ = other.inline_;
            heap_ = std::move(other.heap_);
            other.reset_to_empty_inline();
        }
        return *this;
    }

    ~RglString() = default;

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(data_ptr(), length_);
    }

    [[nodiscard]] std::string str() const { return std::string(view()); }
    [[nodiscard]] const char* c_str() const noexcept { return data_ptr(); }
    [[nodiscard]] std::size_t size() const noexcept { return length_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return length_ == 0U; }
    [[nodiscard]] bool using_inline_storage() const noexcept { return heap_.empty(); }
    [[nodiscard]] std::int32_t cursor() const noexcept { return cursor_; }

    void set_cursor(std::int32_t pos) noexcept {
        if (pos < 0) {
            cursor_ = 0;
            return;
        }
        const std::size_t clamped =
            static_cast<std::size_t>(pos) > length_ ? length_ : static_cast<std::size_t>(pos);
        cursor_ = static_cast<std::int32_t>(clamped);
    }

    void reset_cursor() noexcept { cursor_ = 0; }

    void clear() noexcept {
        length_ = 0U;
        cursor_ = 0;
        if (!heap_.empty()) {
            heap_[0] = '\0';
        } else {
            inline_[0] = '\0';
        }
    }

    void assign(std::string_view text) {
        reserve_buffer(text.size() + 1U);
        char* dst = mutable_data_ptr();
        for (std::size_t i = 0; i < text.size(); ++i) {
            dst[i] = text[i];
        }
        dst[text.size()] = '\0';
        length_ = text.size();
        cursor_ = 0;
    }

    void append(std::string_view text) {
        if (text.empty()) {
            return;
        }
        const std::size_t next_len = length_ + text.size();
        reserve_buffer(next_len + 1U);
        char* dst = mutable_data_ptr();
        for (std::size_t i = 0; i < text.size(); ++i) {
            dst[length_ + i] = text[i];
        }
        dst[next_len] = '\0';
        length_ = next_len;
    }

    // Mirrors rglString::swapSpacesWithUnderscores (0x140310030).
    void swap_spaces_with_underscores() noexcept {
        char* dst = mutable_data_ptr();
        for (std::size_t i = 0; i < length_; ++i) {
            if (dst[i] == ' ') {
                dst[i] = '_';
            }
        }
    }

    // Mirrors rglString::swapUnderscoresWithSpaces (0x140310000).
    void swap_underscores_with_spaces() noexcept {
        char* dst = mutable_data_ptr();
        for (std::size_t i = 0; i < length_; ++i) {
            if (dst[i] == '_') {
                dst[i] = ' ';
            }
        }
    }

    // Mirrors rglString::getNextToken (0x14030f5d0): skips bytes <= 0x20, then
    // reads maximal run of bytes > 0x20 and leaves cursor after the token.
    [[nodiscard]] std::string next_token() {
        const char* src = data_ptr();
        std::size_t pos = cursor_ > 0 ? static_cast<std::size_t>(cursor_) : 0U;
        while (pos < length_ && static_cast<unsigned char>(src[pos]) <= 0x20U) {
            ++pos;
        }
        if (pos >= length_) {
            cursor_ = static_cast<std::int32_t>(pos);
            return {};
        }
        const std::size_t begin = pos;
        while (pos < length_ && static_cast<unsigned char>(src[pos]) > 0x20U) {
            ++pos;
        }
        cursor_ = static_cast<std::int32_t>(pos);
        return std::string(src + begin, pos - begin);
    }

    // Strict decimal parsers: the whole string must be one optionally signed integer.
    // Retail `toInt64Stream` (`0x140310490`) / `toUInt64Stream` (`0x140310570`) have no error path:
    // they inspect only byte 0 for `'-'` and accumulate backward mod `2^64` without validating
    // digits or whitespace. Strict validation here (`from_chars` / full-slice match) is an
    // intentional safety deviation so malformed inputs fail cleanly instead of wrapping garbage.
    [[nodiscard]] bool parse_i64(std::int64_t& out) const noexcept {
        const std::string_view v = view();
        if (v.empty()) {
            return false;
        }
        std::int64_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), parsed, 10);
        if (ec != std::errc{} || ptr != v.data() + v.size()) {
            return false;
        }
        out = parsed;
        return true;
    }

    [[nodiscard]] bool parse_u64(std::uint64_t& out) const noexcept {
        const std::string_view v = view();
        if (v.empty()) {
            return false;
        }
        std::uint64_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), parsed, 10);
        if (ec != std::errc{} || ptr != v.data() + v.size()) {
            return false;
        }
        out = parsed;
        return true;
    }

    [[nodiscard]] static std::string encode_savegame_string(std::string_view text) {
        if (text.empty()) {
            return "-1";
        }
        std::string out(text);
        for (char& ch : out) {
            if (ch == ' ') {
                ch = '_';
            }
        }
        return out;
    }

    [[nodiscard]] static std::string decode_savegame_string(std::string_view encoded) {
        std::string out(encoded);
        for (char& ch : out) {
            if (ch == '_') {
                ch = ' ';
            }
        }
        if (out == "-1") {
            out.clear();
        }
        return out;
    }

private:
    void reset_to_empty_inline() noexcept {
        std::vector<char>().swap(heap_);
        capacity_ = kInlineCapacity;
        length_ = 0U;
        cursor_ = 0;
        inline_[0] = '\0';
    }

    // Mirrors rglString::reallocate (0x14030f720) 8-byte aligned growth once
    // payload + NUL exceeds 48 bytes.
    void reserve_buffer(std::size_t required_bytes) {
        if (required_bytes <= capacity_) {
            return;
        }
        const std::size_t grown = std::max<std::size_t>({required_bytes, capacity_ * 2U, 96U});
        const std::size_t aligned = (grown + 7U) & ~std::size_t{7U};
        std::vector<char> next(aligned, '\0');
        const char* cur = data_ptr();
        for (std::size_t i = 0; i <= length_; ++i) {
            next[i] = cur[i];
        }
        heap_ = std::move(next);
        capacity_ = aligned;
    }

    [[nodiscard]] const char* data_ptr() const noexcept {
        return heap_.empty() ? inline_.data() : heap_.data();
    }

    [[nodiscard]] char* mutable_data_ptr() noexcept {
        return heap_.empty() ? inline_.data() : heap_.data();
    }

    std::int32_t cursor_ = 0;
    std::size_t length_ = 0U;
    std::size_t capacity_ = kInlineCapacity;
    std::array<char, kInlineCapacity> inline_{};
    std::vector<char> heap_{};
};

}  // namespace wse::core
