// Token reader over the in-memory contents of a module text file.
//
// Evidence:
//   rglInputTextFileStream vftable 0x1405651e0  -> rglTextStream readers
//     +0x50 readStream, +0x58 readFloat, +0x60 readBool
//     +0x68 readUInt64, +0x70 readInt64
//     +0x78 readUInt32 / +0x80 readInt32 (same 32-bit parser)
//     +0x88 readByte, +0x90 readString
//   rglTextStream has no standalone vftable; the concrete input and output
//   file streams instantiate its readers and writers in their vtables.
//   rglStream layout (PDB type 0x1b18a): m_contents rglString at +0x58,
//     m_cursor at +0x54; the whole file is buffered and readers advance the
//     cursor.
//   mbGame::readTriggers (0x1400ccf20): skips every byte <= 0x20 before
//     rglString::getNextToken, which is the same whitespace rule used here.
//
// Module files are therefore whitespace-delimited decimal text, not binary
// records. A "token" is a maximal run of bytes > 0x20.
//
// Retail parser behaviour (rglTextStream, evidence ev_1e3c25667502f1b1e6ce2e296ecc792c1cd592a4e85440bf9b7e535e8e0fdb0b):
//   readFloat  0x1402afa80  strtod(token, nullptr), cast to float
//   readBool   0x1402afb10  _stricmp "true"/"false", else strtol(token) != 0
//   readInt32  0x1402afce0  manual 15-char decimal loop, leading '-'
//   readUInt64 0x1402afbe0  rglString::toUInt64Stream 0x14030f7b0, unchecked
//                           64-bit digit sum: oversized decimals wrap mod 2^64
//   readInt64  0x1402afc60  rglString::toInt64Stream (same decimal grammar)
//   readString 0x1402afe60  raw token, no quote stripping
// Writers emit the value, then a single 0x20 separator.
//
// The retail readers are deliberately lenient: strtod accepts a numeric
// prefix, readBool treats anything strtol cannot parse as false, and the
// integer loops never validate a digit. This port parses strictly and reports
// failure instead, so malformed module data cannot be silently misread. Valid
// retail module files contain plain decimal tokens and parse identically.

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
    // readUInt64 (0x1402afbe0) converts the token with rglString::toUInt64Stream
    // (0x14030f7b0), which sums `(c - '0') * 10^k` from the last character in unchecked
    // 64-bit arithmetic: a decimal token above UINT64_MAX wraps modulo 2^64 instead of
    // failing. Module System flag fields written from Python integers can exceed 64 bits
    // (e.g. `Phantasy Calradia` `item_kinds1.txt` `itm_drow_c_dagger` properties, 43 digits),
    // and retail loads them, so all-digit tokens wrap here too. Tokens with any non-digit
    // byte (including a sign) are still rejected, unlike retail's unvalidated loop.
    [[nodiscard]] bool read_u64(std::uint64_t& out) noexcept {
        const std::string_view token = next_token();
        if (token.empty()) {
            return false;
        }
        std::uint64_t value = 0;
        for (const char c : token) {
            if (c < '0' || c > '9') {
                return false;
            }
            value = value * 10U + static_cast<std::uint64_t>(c - '0');
        }
        out = value;
        return true;
    }

    [[nodiscard]] bool read_f32(float& out) noexcept { return read_real(out); }
    [[nodiscard]] bool read_f64(double& out) noexcept { return read_real(out); }

    // "true"/"false" compared case-insensitively (retail uses _stricmp),
    // otherwise a decimal integer where nonzero is true. Retail falls back to
    // strtol() and yields false for unparseable tokens; this reader rejects
    // them instead.
    [[nodiscard]] bool read_bool(bool& out) noexcept {
        const std::string_view token = next_token();
        if (token.empty()) {
            return false;
        }
        if (equals_ascii_case_insensitive(token, "true")) {
            out = true;
            return true;
        }
        if (equals_ascii_case_insensitive(token, "false")) {
            out = false;
            return true;
        }
        std::int64_t value = 0;
        const auto result =
            std::from_chars(token.data(), token.data() + token.size(), value, 10);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) {
            return false;
        }
        out = value != 0;
        return true;
    }

    [[nodiscard]] bool read_string(std::string& out) {
        const std::string_view token = next_token();
        if (token.empty()) {
            return false;
        }
        out.assign(token);
        return true;
    }

    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }

    // Unread bytes. Loaders use it to reject a record count that the rest of
    // the file cannot possibly hold before allocating storage for it.
    [[nodiscard]] std::size_t remaining() const noexcept { return contents_.size() - cursor_; }

    // Upper bound on how many of `count` records the unread input can hold
    // when each record needs at least `minRecordBytes` bytes (tokens plus one
    // separator). Loaders reserve this much instead of trusting the count.
    [[nodiscard]] std::size_t plausible_count(std::size_t count,
                                              std::size_t minRecordBytes) const noexcept {
        const std::size_t fits = (remaining() + 1) / minRecordBytes;
        return count < fits ? count : fits;
    }

private:
    static void skip_whitespace(std::size_t& position, std::string_view contents) noexcept {
        while (position < contents.size() &&
               static_cast<unsigned char>(contents[position]) <= 0x20) {
            ++position;
        }
    }

    [[nodiscard]] static bool equals_ascii_case_insensitive(std::string_view lhs,
                                                            std::string_view rhs) noexcept {
        if (lhs.size() != rhs.size()) {
            return false;
        }
        for (std::size_t index = 0; index < lhs.size(); ++index) {
            char left = lhs[index];
            char right = rhs[index];
            if (left >= 'A' && left <= 'Z') {
                left = static_cast<char>(left - 'A' + 'a');
            }
            if (right >= 'A' && right <= 'Z') {
                right = static_cast<char>(right - 'A' + 'a');
            }
            if (left != right) {
                return false;
            }
        }
        return true;
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
