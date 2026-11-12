// Token reader over the in-memory contents of a module text file.
//
// Evidence:
//   rglInputTextFileStream vftable 0x14051c0b0  -> rglTextStream readers
//     +0x58 rglTextStream::readFloat
//     +0x70 rglTextStream::readInt64
//     +0x78 rglTextStream::readUInt32
//     +0x80 rglTextStream::readUInt32
//   rglStream layout (PDB type 0x1b18a): m_contents rglString at +0x58,
//     m_cursor at +0x54; the whole file is buffered and readers advance the
//     cursor.
//   mbGame::readTriggers (0x1400cca30): skips every byte <= 0x20 before
//     rglString::getNextToken, which is the same whitespace rule used here.
//
// Module files are therefore whitespace-delimited decimal text, not binary
// records. A "token" is a maximal run of bytes > 0x20.

#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>

namespace wse::io {

class TokenReader {
public:
    explicit TokenReader(std::string_view contents) noexcept : contents_(contents) {}

    [[nodiscard]] bool at_end() noexcept {
        std::size_t position = cursor_;
        skip_whitespace(position);
        return position >= contents_.size();
    }

    // Returns the next whitespace-delimited token, or an empty view at EOF.
    [[nodiscard]] std::string_view next_token() noexcept {
        skip_whitespace(cursor_);
        const std::size_t begin = cursor_;
        while (cursor_ < contents_.size() &&
               static_cast<unsigned char>(contents_[cursor_]) > 0x20) {
            ++cursor_;
        }
        return contents_.substr(begin, cursor_ - begin);
    }

    [[nodiscard]] bool skip_tokens(std::size_t count) noexcept {
        for (std::size_t index = 0; index < count; ++index) {
            if (next_token().empty()) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool read_i32(std::int32_t& out) noexcept { return read_signed(out); }
    [[nodiscard]] bool read_u32(std::uint32_t& out) noexcept { return read_unsigned(out); }
    [[nodiscard]] bool read_i64(std::int64_t& out) noexcept { return read_signed(out); }
    [[nodiscard]] bool read_u64(std::uint64_t& out) noexcept { return read_unsigned(out); }

    [[nodiscard]] bool read_f32(float& out) noexcept { return read_real(out); }
    [[nodiscard]] bool read_f64(double& out) noexcept { return read_real(out); }

    [[nodiscard]] bool read_string(std::string& out) {
        const std::string_view token = next_token();
        if (token.empty()) {
            return false;
        }
        out.assign(token);
        return true;
    }

    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }

private:
    static void skip_whitespace(std::size_t& position, std::string_view contents) noexcept {
        while (position < contents.size() &&
               static_cast<unsigned char>(contents[position]) <= 0x20) {
            ++position;
        }
    }

    void skip_whitespace(std::size_t& position) const noexcept {
        skip_whitespace(position, contents_);
    }

    template <class Integer>
    [[nodiscard]] bool read_signed(Integer& out) noexcept {
        const std::string_view token = next_token();
        if (token.empty()) {
            return false;
        }
        Integer value{};
        const auto result =
            std::from_chars(token.data(), token.data() + token.size(), value, 10);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            return false;
        }
        out = value;
        return true;
    }

    template <class Integer>
    [[nodiscard]] bool read_unsigned(Integer& out) noexcept {
        const std::string_view token = next_token();
        if (token.empty()) {
            return false;
        }
        Integer value{};
        const auto result =
            std::from_chars(token.data(), token.data() + token.size(), value, 10);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            return false;
        }
        out = value;
        return true;
    }

    template <class Real>
    [[nodiscard]] bool read_real(Real& out) noexcept {
        const std::string_view token = next_token();
        if (token.empty()) {
            return false;
        }
        Real value{};
        const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            return false;
        }
        out = value;
        return true;
    }

    std::string_view contents_;
    std::size_t cursor_ = 0;
};

}  // namespace wse::io
