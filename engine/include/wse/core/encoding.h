#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace wse::core {

// _wcsicmp folds code units in the default "C" CRT locale; keep folding
// deterministic across MSVCRT and Bionic rather than delegating to towlower.
[[nodiscard]] constexpr char16_t wse_fold_case(char16_t value) noexcept {
    if (value >= u'A' && value <= u'Z') {
        return static_cast<char16_t>(value + (u'a' - u'A'));
    }
    return value;
}

[[nodiscard]] inline bool wse_string_equals(std::u16string_view lhs,
                                            std::u16string_view rhs,
                                            bool case_insensitive) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const char16_t left = case_insensitive ? wse_fold_case(lhs[i]) : lhs[i];
        const char16_t right = case_insensitive ? wse_fold_case(rhs[i]) : rhs[i];
        if (left != right) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline std::u16string wse_fold_string(std::u16string_view text) {
    std::u16string folded;
    folded.reserve(text.size());
    for (const char16_t value : text) {
        folded.push_back(wse_fold_case(value));
    }
    return folded;
}

[[nodiscard]] inline bool wse_string_contains(std::u16string_view haystack,
                                              std::u16string_view needle,
                                              bool case_insensitive) {
    if (needle.empty()) {
        // The retail find receives a null data pointer for an empty needle and
        // returns -1 (0x1069).
        return false;
    }
    if (case_insensitive) {
        return wse_fold_string(haystack).find(wse_fold_string(needle)) !=
               std::u16string::npos;
    }
    return haystack.find(needle) != std::u16string_view::npos;
}

[[nodiscard]] inline bool wse_string_starts_with(
    std::u16string_view haystack, std::u16string_view needle,
    bool case_insensitive) noexcept {
    return needle.size() <= haystack.size() &&
           wse_string_equals(haystack.substr(0, needle.size()), needle,
                             case_insensitive);
}

[[nodiscard]] inline bool wse_string_ends_with(std::u16string_view haystack,
                                               std::u16string_view needle,
                                               bool case_insensitive) noexcept {
    return needle.size() <= haystack.size() &&
           wse_string_equals(haystack.substr(haystack.size() - needle.size()),
                             needle, case_insensitive);
}

// _wtof (0x140499608): the CRT parses the longest decimal/inf/nan prefix and
// returns 0.0 when there is none. strtod gives the same correctly-rounded
// result; the numeric grammar is ASCII.
[[nodiscard]] inline double wse_wtof(std::u16string_view text) noexcept {
    std::string narrow;
    narrow.reserve(text.size());
    for (const char16_t value : text) {
        if (value == 0 || value > 0x7F) {
            break;
        }
        narrow.push_back(static_cast<char>(value));
    }
    return std::strtod(narrow.c_str(), nullptr);
}

// towupper counterpart of wse_fold_case for str_store_upper (0x1401f8c40),
// kept locale-independent ("C" locale ASCII rules) across platforms.
[[nodiscard]] constexpr char16_t wse_upper_case(char16_t value) noexcept {
    if (value >= u'a' && value <= u'z') {
        return static_cast<char16_t>(value - (u'a' - u'A'));
    }
    return value;
}

// mbString comparison behind str_compare (0x1074). The case-sensitive wrapper
// 0x1401f8830 normalises to -1/0/1; the case-insensitive path tail-jumps to
// _wcsicmp (0x140499764), which returns the folded code-unit difference.
[[nodiscard]] inline int wse_string_compare(std::u16string_view lhs,
                                            std::u16string_view rhs,
                                            bool case_insensitive) noexcept {
    const std::size_t common = std::min(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < common; ++i) {
        const char16_t left =
            case_insensitive ? wse_fold_case(lhs[i]) : lhs[i];
        const char16_t right =
            case_insensitive ? wse_fold_case(rhs[i]) : rhs[i];
        if (left != right) {
            if (case_insensitive) {
                return static_cast<int>(left) - static_cast<int>(right);
            }
            return left < right ? -1 : 1;
        }
    }
    if (lhs.size() == rhs.size()) {
        return 0;
    }
    if (!case_insensitive) {
        return lhs.size() < rhs.size() ? -1 : 1;
    }
    const char16_t left =
        lhs.size() > common ? wse_fold_case(lhs[common]) : char16_t{0};
    const char16_t right =
        rhs.size() > common ? wse_fold_case(rhs[common]) : char16_t{0};
    return static_cast<int>(left) - static_cast<int>(right);
}

// 0x1401f8890: replace every non-overlapping occurrence of `needle`, resuming
// after the inserted replacement. A null/empty needle returns the input
// unchanged (the retail function returns before touching the string).
[[nodiscard]] inline std::u16string wse_replace_all(
    std::u16string text, std::u16string_view needle,
    std::u16string_view replacement) {
    if (needle.empty()) {
        return text;
    }
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::u16string::npos) {
        text.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
    return text;
}

[[nodiscard]] constexpr bool wse_is_ascii_alpha(char16_t value) noexcept {
    return (value >= u'A' && value <= u'Z') || (value >= u'a' && value <= u'z');
}

[[nodiscard]] constexpr bool wse_is_ascii_digit(char16_t value) noexcept {
    return value >= u'0' && value <= u'9';
}

[[nodiscard]] constexpr bool wse_is_ascii_space(char value) noexcept {
    return value == ' ' || (value >= '\t' && value <= '\r');
}

// Deterministic "C"-locale whitespace check replacing std::iswspace across
// MSVCRT/glibc/Bionic.
[[nodiscard]] constexpr bool wse_is_space(char16_t value) noexcept {
    return value == u' ' || (value >= u'\t' && value <= u'\r');
}

// 0x1401f8e80 removes leading whitespace.
inline void wse_trim_left(std::u16string& text) {
    std::size_t first = 0;
    while (first < text.size() && wse_is_space(text[first])) {
        ++first;
    }
    text.erase(0, first);
}

// 0x1401f8dc0 removes trailing whitespace.
inline void wse_trim_right(std::u16string& text) {
    std::size_t last = text.size();
    while (last > 0 && wse_is_space(text[last - 1])) {
        --last;
    }
    text.erase(last);
}

// 0x1401f8fa0 trims both ends (right first, then left).
inline void wse_trim(std::u16string& text) {
    wse_trim_right(text);
    wse_trim_left(text);
}

// Encodes UTF-16 into UTF-8 (matching 0x140310b20 / CP_UTF8 0xfde9), replacing
// lone surrogates with U+FFFD.
[[nodiscard]] inline std::string wse_utf16_to_utf8(std::u16string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint32_t cp = static_cast<std::uint16_t>(text[i]);
        if (cp >= 0xD800u && cp <= 0xDBFFu) {
            if (i + 1 < text.size()) {
                const std::uint32_t low =
                    static_cast<std::uint16_t>(text[i + 1]);
                if (low >= 0xDC00u && low <= 0xDFFFu) {
                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                    ++i;
                } else {
                    cp = 0xFFFDu;
                }
            } else {
                cp = 0xFFFDu;
            }
        } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
            cp = 0xFFFDu;
        }
        if (cp <= 0x7Fu) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FFu) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

// Decodes UTF-8 into UTF-16 (matching MultiByteToWideChar CP_UTF8 0xfde9 in
// 0x140311730 / 0x140310e90), replacing malformed sequences with U+FFFD.
[[nodiscard]] inline std::u16string wse_utf8_to_utf16(std::string_view text) {
    std::u16string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<std::uint8_t>(text[i]);
        if (b0 < 0x80u) {
            out.push_back(static_cast<char16_t>(b0));
            ++i;
            continue;
        }
        std::uint32_t cp = 0xFFFDu;
        std::size_t len = 1;
        if ((b0 & 0xE0u) == 0xC0u && i + 1 < text.size()) {
            const auto b1 = static_cast<std::uint8_t>(text[i + 1]);
            if ((b1 & 0xC0u) == 0x80u) {
                const std::uint32_t decoded =
                    ((static_cast<std::uint32_t>(b0) & 0x1Fu) << 6) |
                    (static_cast<std::uint32_t>(b1) & 0x3Fu);
                if (decoded >= 0x80u) {
                    cp = decoded;
                    len = 2;
                }
            }
        } else if ((b0 & 0xF0u) == 0xE0u && i + 2 < text.size()) {
            const auto b1 = static_cast<std::uint8_t>(text[i + 1]);
            const auto b2 = static_cast<std::uint8_t>(text[i + 2]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
                const std::uint32_t decoded =
                    ((static_cast<std::uint32_t>(b0) & 0x0Fu) << 12) |
                    ((static_cast<std::uint32_t>(b1) & 0x3Fu) << 6) |
                    (static_cast<std::uint32_t>(b2) & 0x3Fu);
                if (decoded >= 0x800u &&
                    (decoded < 0xD800u || decoded > 0xDFFFu)) {
                    cp = decoded;
                    len = 3;
                }
            }
        } else if ((b0 & 0xF8u) == 0xF0u && i + 3 < text.size()) {
            const auto b1 = static_cast<std::uint8_t>(text[i + 1]);
            const auto b2 = static_cast<std::uint8_t>(text[i + 2]);
            const auto b3 = static_cast<std::uint8_t>(text[i + 3]);
            if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u &&
                (b3 & 0xC0u) == 0x80u) {
                const std::uint32_t decoded =
                    ((static_cast<std::uint32_t>(b0) & 0x07u) << 18) |
                    ((static_cast<std::uint32_t>(b1) & 0x3Fu) << 12) |
                    ((static_cast<std::uint32_t>(b2) & 0x3Fu) << 6) |
                    (static_cast<std::uint32_t>(b3) & 0x3Fu);
                if (decoded >= 0x10000u && decoded <= 0x10FFFFu) {
                    cp = decoded;
                    len = 4;
                }
            }
        }
        i += len;
        if (cp <= 0xFFFFu) {
            out.push_back(static_cast<char16_t>(cp));
        } else {
            const std::uint32_t adjusted = cp - 0x10000u;
            out.push_back(
                static_cast<char16_t>(0xD800u + (adjusted >> 10)));
            out.push_back(
                static_cast<char16_t>(0xDC00u + (adjusted & 0x3FFu)));
        }
    }
    return out;
}

// RFC 1321 MD5 returning 32 lowercase hex digits ("%.2x" / 0x14056cd58).
[[nodiscard]] inline std::u16string wse_md5_hex(std::string_view input) {
    static constexpr std::array<std::uint32_t, 64> kShifts = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    };
    static constexpr std::array<std::uint32_t, 64> kConstants = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
        0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
        0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
        0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
        0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
        0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
        0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
        0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
        0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
        0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
        0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
        0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u,
    };

    std::uint32_t a0 = 0x67452301u;
    std::uint32_t b0 = 0xefcdab89u;
    std::uint32_t c0 = 0x98badcfeu;
    std::uint32_t d0 = 0x10325476u;

    auto process_block = [&](const std::uint8_t* block) {
        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < 16; ++i) {
            m[i] = static_cast<std::uint32_t>(block[i * 4]) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(block[i * 4 + 3]) << 24);
        }
        std::uint32_t a = a0;
        std::uint32_t b = b0;
        std::uint32_t c = c0;
        std::uint32_t d = d0;
        for (std::size_t i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            std::size_t g = 0;
            if (i < 16) {
                f = (b & c) | ((~b) & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | ((~d) & c);
                g = (5u * i + 1u) & 15u;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3u * i + 5u) & 15u;
            } else {
                f = c ^ (b | (~d));
                g = (7u * i) & 15u;
            }
            const std::uint32_t temp = d;
            d = c;
            c = b;
            const std::uint32_t sum = a + f + kConstants[i] + m[g];
            const std::uint32_t s = kShifts[i];
            b = b + ((sum << s) | (sum >> (32u - s)));
            a = temp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    };

    const std::size_t full_blocks = input.size() / 64u;
    for (std::size_t block_index = 0; block_index < full_blocks; ++block_index) {
        process_block(reinterpret_cast<const std::uint8_t*>(input.data()) +
                      block_index * 64u);
    }

    std::array<std::uint8_t, 128> tail{};
    const std::size_t remainder = input.size() - full_blocks * 64u;
    for (std::size_t i = 0; i < remainder; ++i) {
        tail[i] = static_cast<std::uint8_t>(input[full_blocks * 64u + i]);
    }
    tail[remainder] = 0x80u;
    const std::size_t padded_bytes = remainder < 56u ? 64u : 128u;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(input.size()) * 8u;
    for (std::size_t i = 0; i < 8u; ++i) {
        tail[padded_bytes - 8u + i] =
            static_cast<std::uint8_t>((bit_length >> (i * 8u)) & 0xFFu);
    }
    process_block(tail.data());
    if (padded_bytes == 128u) {
        process_block(tail.data() + 64u);
    }

    static constexpr std::u16string_view kHexDigits = u"0123456789abcdef";
    std::u16string hex;
    hex.reserve(32);
    for (const std::uint32_t word : {a0, b0, c0, d0}) {
        for (std::size_t byte_index = 0; byte_index < 4; ++byte_index) {
            const auto byte =
                static_cast<std::uint8_t>((word >> (byte_index * 8u)) & 0xFFu);
            hex.push_back(kHexDigits[byte >> 4u]);
            hex.push_back(kHexDigits[byte & 0x0Fu]);
        }
    }
    return hex;
}

// 0x1401ef5b6: strips "{", "}", "<", ">" from a tag-0 string register.
[[nodiscard]] inline std::u16string wse_sanitize_string(std::u16string_view text) {
    std::u16string cleaned;
    cleaned.reserve(text.size());
    for (const char16_t ch : text) {
        if (ch != u'{' && ch != u'}' && ch != u'<' && ch != u'>') {
            cleaned.push_back(ch);
        }
    }
    return cleaned;
}

[[nodiscard]] inline int wse_hex_nibble(char ch) noexcept {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

// Models SHLWAPI.dll!UrlUnescapeA(..., dwFlags=0) in 0x140311670: decodes %XX
// hex bytes, preserves '+' and malformed '%' sequences, and terminates on NUL.
[[nodiscard]] inline std::string wse_url_unescape(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const char ch = input[i];
        if (ch == '\0') {
            break;
        }
        if (ch == '%' && i + 2 < input.size()) {
            const int hi = wse_hex_nibble(input[i + 1]);
            const int lo = wse_hex_nibble(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                const auto decoded = static_cast<std::uint8_t>((hi << 4) | lo);
                if (decoded == 0) {
                    break;
                }
                out.push_back(static_cast<char>(decoded));
                i += 2;
                continue;
            }
        }
        out.push_back(ch);
    }
    return out;
}

// 0x140200dc0: formats (double)value with "%.*f" (0x14055c414).
[[nodiscard]] inline std::u16string wse_format_float(
    float value, std::int32_t precision) {
    const int prec = precision < 0 ? 5 : static_cast<int>(precision);
    std::array<char, 512> stack_buf{};
    const int needed =
        std::snprintf(stack_buf.data(), stack_buf.size(), "%.*f", prec,
                      static_cast<double>(value));
    if (needed < 0) {
        return {};
    }
    if (static_cast<std::size_t>(needed) < stack_buf.size()) {
        return wse_utf8_to_utf16(
            std::string_view(stack_buf.data(), static_cast<std::size_t>(needed)));
    }
    std::string dynamic_buf(static_cast<std::size_t>(needed) + 1u, '\0');
    const int written =
        std::snprintf(dynamic_buf.data(), dynamic_buf.size(), "%.*f", prec,
                      static_cast<double>(value));
    if (written < 0) {
        return {};
    }
    return wse_utf8_to_utf16(
        std::string_view(dynamic_buf.data(), static_cast<std::size_t>(written)));
}

[[nodiscard]] constexpr std::int64_t wse_days_from_civil(
    std::int64_t y, std::int64_t m, std::int64_t d) noexcept {
    y -= (m <= 2) ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const std::int64_t yoe = y - era * 400;
    const std::int64_t doy =
        (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const std::int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

[[nodiscard]] inline std::string_view wse_trim_ascii(
    std::string_view sv) noexcept {
    while (!sv.empty() && wse_is_ascii_space(sv.front())) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && wse_is_ascii_space(sv.back())) {
        sv.remove_suffix(1);
    }
    return sv;
}

// Mirrors mbCheckPath (0x1401c57c0): rejects "<>:\"/\\|?*" (0x14055bc50),
// trims whitespace, strips leading '.', trims again, and rejects empty names.
[[nodiscard]] inline bool wse_check_path(std::string& name) {
    constexpr std::string_view kBadChars = "<>:\"/\\|?*";
    for (const char ch : name) {
        if (ch == '\0' || kBadChars.find(ch) != std::string_view::npos) {
            return false;
        }
    }
    std::string_view sv = wse_trim_ascii(name);
    while (!sv.empty() && sv.front() == '.') {
        sv.remove_prefix(1);
    }
    sv = wse_trim_ascii(sv);
    if (sv.empty() || sv == "." || sv == "..") {
        return false;
    }
    name.assign(sv);
    return true;
}

[[nodiscard]] inline std::string wse_url_escape(std::string_view input) {
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size());
    for (const unsigned char uc : input) {
        const bool unreserved =
            (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') ||
            (uc >= '0' && uc <= '9') || uc == '-' || uc == '_' || uc == '.' ||
            uc == '~';
        if (unreserved) {
            out.push_back(static_cast<char>(uc));
        } else {
            out.push_back('%');
            out.push_back(kHex[(uc >> 4U) & 0x0fU]);
            out.push_back(kHex[uc & 0x0fU]);
        }
    }
    return out;
}

inline void wse_append_utf8_codepoint(std::string& out,
                                      std::uint32_t cp) {
    if (cp <= 0x7fU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | ((cp >> 6U) & 0x1fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else if (cp <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | ((cp >> 12U) & 0x0fU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else if (cp <= 0x10ffffU) {
        out.push_back(static_cast<char>(0xf0U | ((cp >> 18U) & 0x07U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    }
}

[[nodiscard]] constexpr std::uint32_t wse_read_le32(
    const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

inline void wse_append_le32(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xffU));
    out.push_back(static_cast<char>((value >> 8U) & 0xffU));
    out.push_back(static_cast<char>((value >> 16U) & 0xffU));
    out.push_back(static_cast<char>((value >> 24U) & 0xffU));
}

inline void wse_append_le_f32(std::string& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    wse_append_le32(out, bits);
}

[[nodiscard]] inline float wse_read_le_f32(const std::uint8_t* p) noexcept {
    const std::uint32_t bits = wse_read_le32(p);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}


inline void wse_spaces_to_underscores(std::string& text) noexcept {
    std::replace(text.begin(), text.end(), ' ', '_');
}

// Shared entity slot lookup for opcodes 3003..3013 (0x1401e5745..0x1401e5a92).

}  // namespace wse::core
