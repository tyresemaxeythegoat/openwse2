// Win64 strings, code pages and locale for translated code (Phase 5 R4, `docs/RECOMP_PLAN.md`
// "R4 design", family "Strings and locale"; v1.1.5.6 call sites: MultiByteToWideChar 34,
// WideCharToMultiByte 21, FormatMessageA/W 8, the rest 1-4 each; lua5164 adds the *W locale calls):
//
//   MultiByteToWideChar WideCharToMultiByte GetACP GetOEMCP GetCPInfo IsValidCodePage
//   AreFileApisANSI GetConsoleCP GetConsoleOutputCP lstrlenA lstrlenW
//   GetStringTypeW LCMapStringEx LCMapStringW CompareStringEx CompareStringW
//   GetLocaleInfoEx GetLocaleInfoW GetUserDefaultLocaleName GetUserDefaultLCID IsValidLocaleName
//   IsValidLocale EnumSystemLocalesEx EnumSystemLocalesW GetDateFormatEx GetDateFormatW
//   GetTimeFormatEx GetTimeFormatW FormatMessageA FormatMessageW
//
// The process runs as en-US with ANSI code page 1252 and OEM code page 437 (Windows' defaults
// for an English system); UTF-8 (65001) and US-ASCII (20127) also convert. Differences from
// Windows, all documented in `engine/README.md`: no best-fit mapping (a character the code page
// lacks becomes the default character), case mapping and character types beyond Latin-1 come from
// simple Unicode ranges, string comparison is ordinal on case-folded text (not linguistic), and
// sort keys are the code units. Header-only.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/shim_crt.h"
#include "wse/recomp/shim_memory.h"
#include "wse/recomp/shim_time.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::shim {

inline constexpr std::uint32_t kCpAcp = 0, kCpOemcp = 1, kCpThreadAcp = 3, kCpUtf8 = 65001, kCpAscii = 20127;
inline constexpr std::uint32_t kAnsiCodePage = 1252, kOemCodePage = 437;
inline constexpr std::uint32_t kErrorNoUnicodeTranslation = 1113;
inline constexpr std::uint32_t kErrorInvalidFlags = 1004;
inline constexpr std::uint32_t kErrorMrMidNotFound = 317;
inline constexpr std::uint32_t kMbErrInvalidChars = 0x8, kWcErrInvalidChars = 0x80;
inline constexpr std::uint32_t kLcmapLowercase = 0x100, kLcmapUppercase = 0x200, kLcmapSortkey = 0x400;
inline constexpr std::uint32_t kNormIgnoreCase = 0x1, kLinguisticIgnoreCase = 0x10;
inline constexpr std::uint32_t kLocaleReturnNumber = 0x20000000, kLocaleNoUserOverride = 0x80000000;
inline constexpr std::uint32_t kLcidEnUs = 0x409;

namespace str_detail {
template <class T>
T get(std::uint64_t va) noexcept {
    T v{};
    std::memcpy(&v, reinterpret_cast<const void*>(va), sizeof(T));
    return v;
}
template <class T>
void put(std::uint64_t va, T v) noexcept {
    std::memcpy(reinterpret_cast<void*>(va), &v, sizeof(T));
}

// Windows-1252 0x80-0x9F (the rest is Latin-1); undefined bytes map to the C1 control.
inline constexpr char16_t kCp1252High[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
// IBM437 0x80-0xFF (0x00-0x7F convert as themselves, without MB_USEGLYPHCHARS).
inline constexpr char16_t kCp437High[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

inline std::uint32_t resolve_cp(std::uint32_t cp) noexcept {
    if (cp == kCpAcp || cp == kCpThreadAcp) return kAnsiCodePage;
    if (cp == kCpOemcp) return kOemCodePage;
    return cp;
}
inline bool valid_cp(std::uint32_t cp) noexcept {
    cp = resolve_cp(cp);
    return cp == kAnsiCodePage || cp == kOemCodePage || cp == kCpUtf8 || cp == kCpAscii;
}
inline char16_t byte_to_unit(std::uint32_t cp, std::uint8_t b) noexcept {
    if (b < 0x80) return b;
    if (cp == kAnsiCodePage) return b < 0xA0 ? kCp1252High[b - 0x80] : static_cast<char16_t>(b);
    if (cp == kOemCodePage) return kCp437High[b - 0x80];
    return static_cast<char16_t>(b & 0x7F);  // 20127: the high bit is dropped, as Windows does
}
// -1 when the code page has no byte for `u`.
inline int unit_to_byte(std::uint32_t cp, char16_t u) noexcept {
    if (u < 0x80) return u;
    if (cp == kAnsiCodePage) {
        if (u >= 0xA0 && u <= 0xFF) return u;
        for (int i = 0; i < 32; ++i) {
            if (kCp1252High[i] == u) return 0x80 + i;
        }
        return -1;
    }
    if (cp == kOemCodePage) {
        for (int i = 0; i < 128; ++i) {
            if (kCp437High[i] == u) return 0x80 + i;
        }
    }
    return -1;
}

// UTF-8 -> UTF-16; `bad` is set by any invalid sequence (each becomes U+FFFD).
inline std::u16string decode_utf8(const std::uint8_t* p, std::size_t n, bool& bad) {
    std::u16string out;
    bad = false;
    for (std::size_t i = 0; i < n;) {
        const std::uint8_t c = p[i];
        std::uint32_t cp = 0;
        std::size_t len = 0;
        std::uint32_t min = 0;
        if (c < 0x80) {
            out.push_back(c);
            ++i;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            cp = c & 0x1FU;
            len = 2;
            min = 0x80;
        } else if (c >= 0xE0 && c <= 0xEF) {
            cp = c & 0x0FU;
            len = 3;
            min = 0x800;
        } else if (c >= 0xF0 && c <= 0xF4) {
            cp = c & 0x07U;
            len = 4;
            min = 0x10000;
        }
        (void)min;
        // The second byte's range depends on the lead (no overlongs, surrogates or > U+10FFFF).
        // As Windows does (checked against MultiByteToWideChar): a continuation byte after a
        // valid lead is always taken into the sequence, and one outside that range ends it as a
        // single U+FFFD; C0, C1 and F5-FF are invalid leads on their own.
        const std::uint8_t lo = c == 0xE0 ? 0xA0 : (c == 0xF0 ? 0x90 : 0x80);
        const std::uint8_t hi = c == 0xED ? 0x9F : (c == 0xF4 ? 0x8F : 0xBF);
        std::size_t k = 1;
        bool broken = false;
        for (; len != 0 && k < len && i + k < n; ++k) {
            const std::uint8_t b = p[i + k];
            if ((b & 0xC0) != 0x80) {
                break;
            }
            cp = (cp << 6) | (b & 0x3FU);
            if (k == 1 && (b < lo || b > hi)) {
                broken = true;
                ++k;
                break;
            }
        }
        if (len == 0 || broken || k != len) {
            bad = true;
            out.push_back(0xFFFD);
            i += k;  // the lead byte and the continuation bytes that were valid
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += len;
    }
    return out;
}
// UTF-16 -> UTF-8; an unpaired surrogate becomes U+FFFD and sets `bad`.
inline std::string encode_utf8(const char16_t* p, std::size_t n, bool& bad) {
    std::string out;
    bad = false;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t cp = p[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n && p[i + 1] >= 0xDC00 && p[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (p[i + 1] - 0xDC00U);
            ++i;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            bad = true;
            cp = 0xFFFD;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// Greek (U+0370-U+03FF) case pairs beyond the basic alphabet, as Windows maps them
// (checked against LCMapStringEx): {upper, lower}.
inline constexpr char16_t kGreekPairs[][2] = {
    {0x370, 0x371}, {0x372, 0x373}, {0x376, 0x377}, {0x3FD, 0x37B}, {0x3FE, 0x37C}, {0x3FF, 0x37D}, {0x386, 0x3AC},
    {0x388, 0x3AD}, {0x389, 0x3AE}, {0x38A, 0x3AF}, {0x38C, 0x3CC}, {0x38E, 0x3CD}, {0x38F, 0x3CE}, {0x3AA, 0x3CA},
    {0x3AB, 0x3CB}, {0x3CF, 0x3D7}, {0x3D8, 0x3D9}, {0x3DA, 0x3DB}, {0x3DC, 0x3DD}, {0x3DE, 0x3DF}, {0x3E0, 0x3E1},
    {0x3E2, 0x3E3}, {0x3E4, 0x3E5}, {0x3E6, 0x3E7}, {0x3E8, 0x3E9}, {0x3EA, 0x3EB}, {0x3EC, 0x3ED}, {0x3EE, 0x3EF},
    {0x3F7, 0x3F8}, {0x3F9, 0x3F2}, {0x3FA, 0x3FB}};

// Simple case mapping: ASCII, Latin-1, Latin Extended-A pairs, Greek and Cyrillic.
inline char16_t to_upper(char16_t c) noexcept {
    if (c >= 0x370 && c <= 0x3FF) {
        for (const auto& p : kGreekPairs) {
            if (p[1] == c) return p[0];
        }
    }
    if (c >= u'a' && c <= u'z') return static_cast<char16_t>(c - 32);
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return static_cast<char16_t>(c - 32);
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        const bool odd_pairs = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        return odd_pairs ? ((c & 1) == 0 ? static_cast<char16_t>(c - 1) : c) : static_cast<char16_t>(c & ~1);
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return static_cast<char16_t>(c - 32);
    if (c >= 0x430 && c <= 0x44F) return static_cast<char16_t>(c - 32);
    if (c >= 0x450 && c <= 0x45F) return static_cast<char16_t>(c - 80);
    return c;
}
inline char16_t to_lower(char16_t c) noexcept {
    if (c >= 0x370 && c <= 0x3FF) {
        for (const auto& p : kGreekPairs) {
            if (p[0] == c) return p[1];
        }
    }
    if (c >= u'A' && c <= u'Z') return static_cast<char16_t>(c + 32);
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7)) return static_cast<char16_t>(c + 32);
    if (c == 0x178) return 0xFF;
    if (c >= 0x100 && c <= 0x17F && c != 0x130 && c != 0x131 && c != 0x138 && c != 0x149 && c != 0x17F) {
        const bool odd_pairs = (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E);
        return odd_pairs ? ((c & 1) != 0 ? static_cast<char16_t>(c + 1) : c) : static_cast<char16_t>(c | 1);
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return static_cast<char16_t>(c + 32);
    if (c >= 0x410 && c <= 0x42F) return static_cast<char16_t>(c + 32);
    if (c >= 0x400 && c <= 0x40F) return static_cast<char16_t>(c + 80);
    return c;
}

// CT_CTYPE1 flags for one UTF-16 unit.
inline std::uint16_t ctype1(char16_t c) noexcept {
    constexpr std::uint16_t kUpper = 1, kLower = 2, kDigit = 4, kSpace = 8, kPunct = 0x10, kCntrl = 0x20, kBlank = 0x40,
                            kXdigit = 0x80, kAlpha = 0x100, kDefined = 0x200;
    std::uint16_t f = kDefined;
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) {
        f |= kCntrl;
        if (c == 9) f |= kSpace | kBlank;
        if ((c >= 10 && c <= 13) || c == 0x85) f |= kSpace;
        return f;
    }
    // Latin-1 and Latin Extended-A characters Windows classifies specially (checked against
    // GetStringTypeW).
    switch (c) {
    case 0xAA: case 0xB5: case 0xBA: return f | kLower | kAlpha | kPunct;
    case 0xAD: return f | kCntrl | kPunct;
    case 0xB2: case 0xB3: case 0xB9: return f | kDigit | kPunct;
    case 0x130: return f | kUpper | kAlpha;
    case 0x131: case 0x138: case 0x149: case 0x17F: return f | kLower | kAlpha;
    // Greek: unassigned in Windows' tables, signs, punctuation, caseless letters.
    case 0x378: case 0x379: case 0x37F: case 0x380: case 0x381: case 0x382: case 0x383: case 0x38B: case 0x38D:
    case 0x3A2: return 0;
    case 0x374: case 0x375: case 0x384: case 0x385: return f;
    case 0x37E: case 0x387: return f | kPunct;
    case 0x390: case 0x3B0: case 0x3C2: case 0x3D0: case 0x3D1: case 0x3D5: case 0x3D6: case 0x3F0: case 0x3F1:
    case 0x3F3: case 0x3F5: case 0x3FC:
        return f | kLower | kAlpha;
    case 0x3D2: case 0x3D3: case 0x3D4: case 0x3F4: return f | kUpper | kAlpha;
    case 0x3F6: case 0x2044: case 0x2052: return f;
    // General Punctuation: spaces (not blanks), format controls, unassigned.
    case 0x2028: case 0x2029: case 0x202F: case 0x205F: return f | kSpace;
    case 0x2065: case 0x2066: case 0x2067: case 0x2068: case 0x2069: return 0;
    default: break;
    }
    if (c >= 0x2000 && c <= 0x200A) return f | kSpace;
    if ((c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) ||
        (c >= 0x206A && c <= 0x206F)) {
        return f | kCntrl;
    }
    if (c == 0x20 || c == 0xA0 || c == 0x3000) return f | kSpace | kBlank;
    if (c >= u'0' && c <= u'9') return f | kDigit | kXdigit;
    if (to_lower(c) != c) return f | kUpper | kAlpha | ((c >= u'A' && c <= u'F') ? kXdigit : 0);
    if (to_upper(c) != c || c == 0xDF || c == 0xAA || c == 0xBA) {
        return f | kLower | kAlpha | ((c >= u'a' && c <= u'f') ? kXdigit : 0);
    }
    if (c < 0x80 || (c >= 0xA1 && c <= 0xBF) || c == 0xD7 || c == 0xF7 || (c >= 0x2010 && c <= 0x206F)) return f | kPunct;
    if (c >= 0xD800 && c < 0xE000) return 0;
    return f | kAlpha;  // other scripts: letters
}

inline std::u16string wstr(std::uint64_t va, std::int64_t n) {
    std::u16string s;
    if (va == 0) return s;
    if (n < 0) {
        for (std::size_t i = 0; i < 1U << 24; ++i) {
            const char16_t c = get<char16_t>(va + 2 * i);
            if (c == 0) break;
            s.push_back(c);
        }
        return s;
    }
    s.resize(static_cast<std::size_t>(n));
    std::memcpy(s.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(va)), 2 * s.size());
    return s;
}
inline std::u16string widen(const char* s) {
    std::u16string o;
    for (; *s != 0; ++s) o.push_back(static_cast<unsigned char>(*s));
    return o;
}
}  // namespace str_detail

class Strings {
public:
    Strings(Runtime& rt, Memory& memory) : rt_(rt), mem_(memory) {}
    Strings(const Strings&) = delete;
    Strings& operator=(const Strings&) = delete;

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* name, Runtime::ImportShim shim) {
            for (const char* dll : {"KERNEL32.dll", "KERNELBASE.dll"}) {
                n += rt_.set_import(dll, name, shim);
            }
        };
        bind("MultiByteToWideChar", [](CpuState& s) { mb_to_wc(s); });
        bind("WideCharToMultiByte", [](CpuState& s) { wc_to_mb(s); });
        bind("GetACP", [](CpuState& s) { win64_return(s, kAnsiCodePage); });
        bind("GetOEMCP", [](CpuState& s) { win64_return(s, kOemCodePage); });
        bind("GetConsoleCP", [](CpuState& s) { win64_return(s, kOemCodePage); });
        bind("GetConsoleOutputCP", [](CpuState& s) { win64_return(s, kOemCodePage); });
        bind("AreFileApisANSI", [](CpuState& s) { win64_return(s, 1); });
        bind("IsValidCodePage", [](CpuState& s) {
            const auto cp = static_cast<std::uint32_t>(win64_arg(s, 0));
            win64_return(s, cp != kCpAcp && cp != kCpOemcp && str_detail::valid_cp(cp) ? 1 : 0);
        });
        bind("GetCPInfo", [](CpuState& s) {
            const auto cp = static_cast<std::uint32_t>(win64_arg(s, 0));
            const std::uint64_t info = win64_arg(s, 1);
            if (!str_detail::valid_cp(cp)) {
                teb::set_error(s, teb::kErrorInvalidParameter);
                win64_return(s, 0);
                return;
            }
            // CPINFO: MaxCharSize, DefaultChar[2], LeadByte[12].
            std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(info)), 0, 20);
            str_detail::put<std::uint32_t>(info, str_detail::resolve_cp(cp) == kCpUtf8 ? 4 : 1);
            str_detail::put<std::uint8_t>(info + 4, '?');
            win64_return(s, 1);
        });
        bind("lstrlenA", [](CpuState& s) {
            const std::uint64_t p = win64_arg(s, 0);
            win64_return(s, p == 0 ? 0 : std::strlen(reinterpret_cast<const char*>(static_cast<std::uintptr_t>(p))));
        });
        bind("lstrlenW", [](CpuState& s) { win64_return(s, str_detail::wstr(win64_arg(s, 0), -1).size()); });
        bind("GetStringTypeW", [](CpuState& s) { string_type(s); });
        bind("LCMapStringEx", [](CpuState& s) { lcmap(s, 1); });
        bind("LCMapStringW", [](CpuState& s) { lcmap(s, 1); });
        bind("CompareStringEx", [](CpuState& s) { compare(s); });
        bind("CompareStringW", [](CpuState& s) { compare(s); });
        bind("GetLocaleInfoEx", [](CpuState& s) { locale_info(s); });
        bind("GetLocaleInfoW", [](CpuState& s) { locale_info(s); });
        bind("GetUserDefaultLocaleName", [](CpuState& s) {
            const std::u16string name = u"en-US";
            if (static_cast<std::int32_t>(win64_arg(s, 1)) < static_cast<std::int32_t>(name.size() + 1)) {
                teb::set_error(s, kErrorInsufficientBuffer);
                win64_return(s, 0);
                return;
            }
            copy_w(win64_arg(s, 0), name);
            win64_return(s, name.size() + 1);
        });
        bind("GetUserDefaultLCID", [](CpuState& s) { win64_return(s, kLcidEnUs); });
        bind("IsValidLocaleName", [](CpuState& s) {
            const std::u16string n = str_detail::wstr(win64_arg(s, 0), -1);
            win64_return(s, n == u"en-US" || n == u"en" || n.empty() ? 1 : 0);
        });
        bind("IsValidLocale", [](CpuState& s) {
            const auto lcid = static_cast<std::uint32_t>(win64_arg(s, 0));
            win64_return(s, lcid == kLcidEnUs || lcid == 0x7F || lcid == 0x400 || lcid == 0x800 ? 1 : 0);
        });
        // EnumSystemLocalesEx(proc(name, flags, lparam), flags, lparam, reserved): en-US only.
        bind("EnumSystemLocalesEx", [this](CpuState& s) {
            const std::uint64_t proc = win64_arg(s, 0);
            (void)rt_.call_guest(s, proc, {reinterpret_cast<std::uintptr_t>(kEnUs), win64_arg(s, 1), win64_arg(s, 2)});
            if (s.unwind_rsp == 0) win64_return(s, 1);
        });
        bind("EnumSystemLocalesW", [this](CpuState& s) {
            (void)rt_.call_guest(s, win64_arg(s, 0), {reinterpret_cast<std::uintptr_t>(kLcid409)});
            if (s.unwind_rsp == 0) win64_return(s, 1);
        });
        bind("GetDateFormatEx", [](CpuState& s) { date_time(s, true, true); });
        bind("GetDateFormatW", [](CpuState& s) { date_time(s, true, false); });
        bind("GetTimeFormatEx", [](CpuState& s) { date_time(s, false, true); });
        bind("GetTimeFormatW", [](CpuState& s) { date_time(s, false, false); });
        bind("FormatMessageA", [this](CpuState& s) { format_message(s, false); });
        bind("FormatMessageW", [this](CpuState& s) { format_message(s, true); });
        return n;
    }

    // The locale strings answered for en-US (LCTYPE -> text; an LCTYPE not listed fails with
    // ERROR_INVALID_FLAGS, which the R4 bring-up log shows so it can be added).
    static bool locale_string(std::uint32_t lctype, std::u16string& out) {
        static const char* const kDays[7] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
        static const char* const kMonths[12] = {"January", "February", "March", "April", "May", "June", "July",
                                                "August", "September", "October", "November", "December"};
        if (lctype >= 0x2A && lctype <= 0x30) { out = str_detail::widen(kDays[lctype - 0x2A]); return true; }
        if (lctype >= 0x31 && lctype <= 0x37) { out = str_detail::widen(kDays[lctype - 0x31]).substr(0, 3); return true; }
        if (lctype >= 0x38 && lctype <= 0x43) { out = str_detail::widen(kMonths[lctype - 0x38]); return true; }
        if (lctype >= 0x44 && lctype <= 0x4F) { out = str_detail::widen(kMonths[lctype - 0x44]).substr(0, 3); return true; }
        const char* v = nullptr;
        switch (lctype) {
        case 0x01: v = "0409"; break;            // LOCALE_ILANGUAGE
        case 0x02: v = "English (United States)"; break;  // LOCALE_SLANGUAGE
        case 0x03: v = "ENU"; break;             // LOCALE_SABBREVLANGNAME
        case 0x04: v = "English"; break;         // LOCALE_SNATIVELANGNAME
        case 0x05: v = "1"; break;               // LOCALE_ICOUNTRY
        case 0x06: v = "United States"; break;   // LOCALE_SCOUNTRY
        case 0x07: v = "USA"; break;             // LOCALE_SABBREVCTRYNAME
        case 0x08: v = "United States"; break;   // LOCALE_SNATIVECTRYNAME
        case 0x09: v = "0409"; break;            // LOCALE_IDEFAULTLANGUAGE
        case 0x0A: v = "1"; break;               // LOCALE_IDEFAULTCOUNTRY
        case 0x0B: v = "437"; break;             // LOCALE_IDEFAULTCODEPAGE
        case 0x0C: v = ","; break;               // LOCALE_SLIST
        case 0x0D: v = "1"; break;               // LOCALE_IMEASURE (US)
        case 0x0E: v = "."; break;               // LOCALE_SDECIMAL
        case 0x0F: v = ","; break;               // LOCALE_STHOUSAND
        case 0x10: v = "3;0"; break;             // LOCALE_SGROUPING
        case 0x11: v = "2"; break;               // LOCALE_IDIGITS
        case 0x12: v = "1"; break;               // LOCALE_ILZERO
        case 0x13: v = "0123456789"; break;      // LOCALE_SNATIVEDIGITS
        case 0x14: v = "$"; break;               // LOCALE_SCURRENCY
        case 0x15: v = "USD"; break;             // LOCALE_SINTLSYMBOL
        case 0x16: v = "."; break;               // LOCALE_SMONDECIMALSEP
        case 0x17: v = ","; break;               // LOCALE_SMONTHOUSANDSEP
        case 0x18: v = "3;0"; break;             // LOCALE_SMONGROUPING
        case 0x19: v = "2"; break;               // LOCALE_ICURRDIGITS
        case 0x1A: v = "2"; break;               // LOCALE_IINTLCURRDIGITS
        case 0x1B: v = "0"; break;               // LOCALE_ICURRENCY
        case 0x1C: v = "0"; break;               // LOCALE_INEGCURR
        case 0x1D: v = "/"; break;               // LOCALE_SDATE
        case 0x1E: v = ":"; break;               // LOCALE_STIME
        case 0x1F: v = "M/d/yyyy"; break;        // LOCALE_SSHORTDATE
        case 0x20: v = "dddd, MMMM d, yyyy"; break;  // LOCALE_SLONGDATE
        case 0x21: v = "0"; break;               // LOCALE_IDATE
        case 0x22: v = "0"; break;               // LOCALE_ILDATE
        case 0x23: v = "0"; break;               // LOCALE_ITIME (12 h)
        case 0x24: v = "1"; break;               // LOCALE_ICENTURY (4-digit years)
        case 0x25: v = "0"; break;               // LOCALE_ITLZERO
        case 0x26: v = "0"; break;               // LOCALE_IDAYLZERO
        case 0x27: v = "0"; break;               // LOCALE_IMONLZERO
        case 0x28: v = "AM"; break;              // LOCALE_S1159
        case 0x29: v = "PM"; break;              // LOCALE_S2359
        case 0x50: v = ""; break;                // LOCALE_SPOSITIVESIGN
        case 0x51: v = "-"; break;               // LOCALE_SNEGATIVESIGN
        case 0x59: v = "en"; break;              // LOCALE_SISO639LANGNAME
        case 0x5A: v = "US"; break;              // LOCALE_SISO3166CTRYNAME
        case 0x5C: v = "en-US"; break;           // LOCALE_SNAME
        case 0x1001: v = "English"; break;       // LOCALE_SENGLISHLANGUAGENAME
        case 0x1002: v = "United States"; break; // LOCALE_SENGLISHCOUNTRYNAME
        case 0x1003: v = "h:mm:ss tt"; break;    // LOCALE_STIMEFORMAT
        case 0x1004: v = "1252"; break;          // LOCALE_IDEFAULTANSICODEPAGE
        case 0x1009: v = "1"; break;             // LOCALE_ICALENDARTYPE
        case 0x100C: v = "6"; break;             // LOCALE_IFIRSTDAYOFWEEK (Sunday)
        case 0x100D: v = "0"; break;             // LOCALE_IFIRSTWEEKOFYEAR
        case 0x1010: v = "1"; break;             // LOCALE_INEGNUMBER
        case 0x1011: v = "10000"; break;         // LOCALE_IDEFAULTMACCODEPAGE
        default: return false;
        }
        out = str_detail::widen(v);
        return true;
    }

private:
    static constexpr const char16_t* kEnUs = u"en-US";
    static constexpr const char16_t* kLcid409 = u"00000409";

    static void copy_w(std::uint64_t dst, const std::u16string& s) {
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(dst)), s.c_str(), 2 * (s.size() + 1));
    }

    // MultiByteToWideChar(cp, flags, src, srcLen, dst, dstLen).
    static void mb_to_wc(CpuState& s) {
        const auto cp = str_detail::resolve_cp(static_cast<std::uint32_t>(win64_arg(s, 0)));
        const auto flags = static_cast<std::uint32_t>(win64_arg(s, 1));
        const std::uint64_t src = win64_arg(s, 2);
        const auto len = static_cast<std::int32_t>(win64_arg(s, 3));
        const std::uint64_t dst = win64_arg(s, 4);
        const auto cap = static_cast<std::int32_t>(win64_arg(s, 5));
        if (!str_detail::valid_cp(cp) || src == 0 || len == 0 || cap < 0 || (cap > 0 && dst == 0)) {
            teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, 0);
            return;
        }
        const auto* p = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(src));
        const std::size_t n = len < 0 ? std::strlen(reinterpret_cast<const char*>(p)) + 1 : static_cast<std::size_t>(len);
        std::u16string out;
        if (cp == kCpUtf8) {
            bool bad = false;
            out = str_detail::decode_utf8(p, n, bad);
            if (bad && (flags & kMbErrInvalidChars) != 0) {
                teb::set_error(s, kErrorNoUnicodeTranslation);
                win64_return(s, 0);
                return;
            }
        } else {
            for (std::size_t i = 0; i < n; ++i) out.push_back(str_detail::byte_to_unit(cp, p[i]));
        }
        finish_w(s, out, dst, cap);
    }

    static void finish_w(CpuState& s, const std::u16string& out, std::uint64_t dst, std::int32_t cap) {
        if (cap == 0) {
            win64_return(s, out.size());
            return;
        }
        if (out.size() > static_cast<std::size_t>(cap)) {
            teb::set_error(s, kErrorInsufficientBuffer);
            win64_return(s, 0);
            return;
        }
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(dst)), out.data(), 2 * out.size());
        win64_return(s, out.size());
    }

    // WideCharToMultiByte(cp, flags, src, srcLen, dst, dstLen, defaultChar, *usedDefaultChar).
    static void wc_to_mb(CpuState& s) {
        const auto cp = str_detail::resolve_cp(static_cast<std::uint32_t>(win64_arg(s, 0)));
        const auto flags = static_cast<std::uint32_t>(win64_arg(s, 1));
        const std::uint64_t src = win64_arg(s, 2);
        const auto len = static_cast<std::int32_t>(win64_arg(s, 3));
        const std::uint64_t dst = win64_arg(s, 4);
        const auto cap = static_cast<std::int32_t>(win64_arg(s, 5));
        const std::uint64_t def = win64_arg(s, 6);
        const std::uint64_t used = win64_arg(s, 7);
        if (!str_detail::valid_cp(cp) || src == 0 || len == 0 || cap < 0 || (cap > 0 && dst == 0) ||
            (cp == kCpUtf8 && (def != 0 || used != 0))) {
            teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, 0);
            return;
        }
        const std::u16string in = str_detail::wstr(src, len < 0 ? -1 : len);
        const std::size_t n = in.size() + (len < 0 ? 1 : 0);  // -1 counts the terminator
        std::string out;
        bool defaulted = false;
        if (cp == kCpUtf8) {
            bool bad = false;
            out = str_detail::encode_utf8(in.c_str(), n, bad);
            if (bad && (flags & kWcErrInvalidChars) != 0) {
                teb::set_error(s, kErrorNoUnicodeTranslation);
                win64_return(s, 0);
                return;
            }
        } else {
            const char fallback = def != 0 ? str_detail::get<char>(def) : '?';
            for (std::size_t i = 0; i < n; ++i) {
                const int b = str_detail::unit_to_byte(cp, in.c_str()[i]);
                if (b < 0) {
                    out.push_back(fallback);
                    defaulted = true;
                } else {
                    out.push_back(static_cast<char>(b));
                }
            }
        }
        if (used != 0) str_detail::put<std::int32_t>(used, defaulted ? 1 : 0);
        if (cap == 0) {
            win64_return(s, out.size());
            return;
        }
        if (out.size() > static_cast<std::size_t>(cap)) {
            teb::set_error(s, kErrorInsufficientBuffer);
            win64_return(s, 0);
            return;
        }
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(dst)), out.data(), out.size());
        win64_return(s, out.size());
    }

    // GetStringTypeW(infoType, src, len, *charType): CT_CTYPE1 only.
    static void string_type(CpuState& s) {
        const auto type = static_cast<std::uint32_t>(win64_arg(s, 0));
        const std::u16string in = str_detail::wstr(win64_arg(s, 1), static_cast<std::int32_t>(win64_arg(s, 2)));
        const std::uint64_t out = win64_arg(s, 3);
        if (type != 1 || out == 0) {
            teb::set_error(s, type != 1 ? kErrorInvalidFlags : teb::kErrorInvalidParameter);
            win64_return(s, 0);
            return;
        }
        for (std::size_t i = 0; i < in.size(); ++i) str_detail::put<std::uint16_t>(out + 2 * i, str_detail::ctype1(in[i]));
        win64_return(s, 1);
    }

    // LCMapStringEx(locale, flags, src, srcLen, dst, dstLen, ...) / LCMapStringW(lcid, ...).
    static void lcmap(CpuState& s, int first) {
        const auto flags = static_cast<std::uint32_t>(win64_arg(s, first));
        const auto len = static_cast<std::int32_t>(win64_arg(s, first + 2));
        const std::u16string in = str_detail::wstr(win64_arg(s, first + 1), len < 0 ? -1 : len);
        const std::uint64_t dst = win64_arg(s, first + 3);
        const auto cap = static_cast<std::int32_t>(win64_arg(s, first + 4));
        std::u16string out = in;
        if (len < 0) out.push_back(0);
        if ((flags & kLcmapSortkey) != 0) {
            // A sort key: the case-folded code units, big-endian, then a 0 terminator (bytes).
            std::string key;
            for (const char16_t c : in) {
                const char16_t u = (flags & kNormIgnoreCase) != 0 ? str_detail::to_upper(c) : c;
                key.push_back(static_cast<char>(u >> 8));
                key.push_back(static_cast<char>(u & 0xFF));
            }
            key.push_back(0);
            if (cap == 0) {
                win64_return(s, key.size());
                return;
            }
            if (key.size() > static_cast<std::size_t>(cap)) {
                teb::set_error(s, kErrorInsufficientBuffer);
                win64_return(s, 0);
                return;
            }
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(dst)), key.data(), key.size());
            win64_return(s, key.size());
            return;
        }
        for (char16_t& c : out) {
            if ((flags & kLcmapUppercase) != 0) c = str_detail::to_upper(c);
            if ((flags & kLcmapLowercase) != 0) c = str_detail::to_lower(c);
        }
        finish_w(s, out, dst, cap);
    }

    // CompareStringEx(locale, flags, s1, n1, s2, n2, ...) / CompareStringW(lcid, flags, ...).
    static void compare(CpuState& s) {
        const auto flags = static_cast<std::uint32_t>(win64_arg(s, 1));
        const std::u16string a = str_detail::wstr(win64_arg(s, 2), static_cast<std::int32_t>(win64_arg(s, 3)));
        const std::u16string b = str_detail::wstr(win64_arg(s, 4), static_cast<std::int32_t>(win64_arg(s, 5)));
        win64_return(s, static_cast<std::uint64_t>(compare_strings(a, b, flags)));
    }

public:
    // CSTR_LESS_THAN (1) / CSTR_EQUAL (2) / CSTR_GREATER_THAN (3), as Windows' word sort orders
    // them: punctuation and symbols before digits before letters, letters case-folded, '-' and
    // '\'' ignored; then (when case matters) a lowercase letter before its uppercase one; then
    // the text without the ignored marks first.
    static int compare_strings(const std::u16string& a, const std::u16string& b, std::uint32_t flags) {
        auto primary = [](const std::u16string& s) {
            std::u32string k;
            for (const char16_t c : s) {
                if (c == u'-' || c == u'\'') continue;
                const std::uint16_t t = str_detail::ctype1(c);
                const char32_t base = (t & 0x100) != 0 ? 0x30000 : ((t & 0x4) != 0 ? 0x20000 : 0x10000);
                k.push_back(static_cast<char32_t>(static_cast<std::uint32_t>(base) + str_detail::to_upper(c)));
            }
            return k;
        };
        const std::u32string ka = primary(a);
        const std::u32string kb = primary(b);
        if (ka != kb) return ka < kb ? 1 : 3;
        if ((flags & (kNormIgnoreCase | kLinguisticIgnoreCase)) == 0) {
            std::u16string la;
            std::u16string lb;
            for (const char16_t c : a) if (c != u'-' && c != u'\'') la.push_back(c);
            for (const char16_t c : b) if (c != u'-' && c != u'\'') lb.push_back(c);
            for (std::size_t i = 0; i < la.size() && i < lb.size(); ++i) {
                if (la[i] != lb[i]) return str_detail::to_upper(la[i]) != la[i] ? 1 : 3;
            }
        }
        if (a.size() != b.size()) return a.size() < b.size() ? 1 : 3;  // the one without '-' / '\'' first
        return 2;
    }

private:

    // GetLocaleInfoEx(name, lctype, buf, len) / GetLocaleInfoW(lcid, lctype, buf, len).
    static void locale_info(CpuState& s) {
        const auto raw = static_cast<std::uint32_t>(win64_arg(s, 1));
        const std::uint32_t lctype = raw & ~(kLocaleReturnNumber | kLocaleNoUserOverride | 0x40000000U);
        const std::uint64_t buf = win64_arg(s, 2);
        const auto cap = static_cast<std::int32_t>(win64_arg(s, 3));
        std::u16string v;
        if (!locale_string(lctype, v)) {
            teb::set_error(s, kErrorInvalidFlags);
            win64_return(s, 0);
            return;
        }
        if ((raw & kLocaleReturnNumber) != 0) {
            std::uint32_t num = 0;
            for (const char16_t c : v) {
                if (c < u'0' || c > u'9') break;
                num = num * 10 + static_cast<std::uint32_t>(c - u'0');
            }
            if (cap == 0) {
                win64_return(s, 2);  // in WCHARs
                return;
            }
            if (cap < 2) {
                teb::set_error(s, kErrorInsufficientBuffer);
                win64_return(s, 0);
                return;
            }
            str_detail::put<std::uint32_t>(buf, num);
            win64_return(s, 2);
            return;
        }
        v.push_back(0);
        finish_w(s, v, buf, cap);
    }

    // Get(Date|Time)Format(Ex|W)(locale, flags, *SYSTEMTIME, format, buf, len).
    static void date_time(CpuState& s, bool date, bool ex) {
        (void)ex;
        const auto flags = static_cast<std::uint32_t>(win64_arg(s, 1));
        const std::uint64_t stp = win64_arg(s, 2);
        const std::uint64_t fmt = win64_arg(s, 3);
        const std::uint64_t buf = win64_arg(s, 4);
        const auto cap = static_cast<std::int32_t>(win64_arg(s, 5));
        SystemTime st;
        if (stp != 0) {
            st = read_systemtime(stp);
        } else {
            (void)filetime_to_systemtime(Time::filetime_now(), st);
        }
        std::u16string pic = fmt != 0 ? str_detail::wstr(fmt, -1)
                                      : str_detail::widen(date ? ((flags & 2) != 0 ? "dddd, MMMM d, yyyy" : "M/d/yyyy")
                                                               : "h:mm:ss tt");
        std::u16string out = format_picture(pic, st);
        out.push_back(0);
        finish_w(s, out, buf, cap);
    }

    static std::u16string format_picture(const std::u16string& pic, const SystemTime& st) {
        static const char* const kDays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        static const char* const kMonths[12] = {"January", "February", "March", "April", "May", "June", "July",
                                                "August", "September", "October", "November", "December"};
        auto num = [](unsigned v, std::size_t width) {
            std::u16string d;
            do {
                d.insert(d.begin(), static_cast<char16_t>(u'0' + v % 10));
                v /= 10;
            } while (v != 0);
            while (d.size() < width) d.insert(d.begin(), u'0');
            return d;
        };
        std::u16string out;
        for (std::size_t i = 0; i < pic.size();) {
            const char16_t c = pic[i];
            if (c == u'\'') {
                std::size_t j = i + 1;
                for (; j < pic.size() && pic[j] != u'\''; ++j) out.push_back(pic[j]);
                i = j + 1;
                continue;
            }
            std::size_t run = 1;
            while (i + run < pic.size() && pic[i + run] == c) ++run;
            const unsigned h12 = st.hour % 12 == 0 ? 12U : st.hour % 12U;
            switch (c) {
            case u'd':
                if (run >= 4) out += str_detail::widen(kDays[st.day_of_week % 7]);
                else if (run == 3) out += str_detail::widen(kDays[st.day_of_week % 7]).substr(0, 3);
                else out += num(st.day, run);
                break;
            case u'M':
                if (run >= 4) out += str_detail::widen(kMonths[(st.month + 11) % 12]);
                else if (run == 3) out += str_detail::widen(kMonths[(st.month + 11) % 12]).substr(0, 3);
                else out += num(st.month, run);
                break;
            case u'y':
                if (run <= 2) out += num(st.year % 100, run == 1 ? 1 : 2);
                else out += num(st.year, 4);
                break;
            case u'h': out += num(h12, run >= 2 ? 2 : 1); break;
            case u'H': out += num(st.hour, run >= 2 ? 2 : 1); break;
            case u'm': out += num(st.minute, run >= 2 ? 2 : 1); break;
            case u's': out += num(st.second, run >= 2 ? 2 : 1); break;
            case u't': out += (st.hour < 12 ? u"AM" : u"PM"); if (run == 1) out.pop_back(); break;
            default: out.append(run, c); break;
            }
            i += run;
        }
        return out;
    }

    // System messages for the error codes the exe and its CRT ask about.
    static const char* system_message(std::uint32_t id) {
        switch (id) {
        case 0: return "The operation completed successfully.";
        case 2: return "The system cannot find the file specified.";
        case 3: return "The system cannot find the path specified.";
        case 5: return "Access is denied.";
        case 6: return "The handle is invalid.";
        case 8: return "Not enough memory resources are available to process this command.";
        case 32: return "The process cannot access the file because it is being used by another process.";
        case 80: return "The file exists.";
        case 87: return "The parameter is incorrect.";
        case 122: return "The data area passed to a system call is too small.";
        case 183: return "Cannot create a file when that file already exists.";
        default: return nullptr;
        }
    }

    // FormatMessage(flags, source, id, language, buffer, size, args).
    void format_message(CpuState& s, bool wide) {
        const auto flags = static_cast<std::uint32_t>(win64_arg(s, 0));
        const std::uint64_t source = win64_arg(s, 1);
        const auto id = static_cast<std::uint32_t>(win64_arg(s, 2));
        const std::uint64_t buffer = win64_arg(s, 4);
        const auto size = static_cast<std::uint32_t>(win64_arg(s, 5));
        const std::uint64_t args = win64_arg(s, 6);
        constexpr std::uint32_t kAllocate = 0x100, kIgnoreInserts = 0x200, kFromString = 0x400, kFromSystem = 0x1000,
                                kArgumentArray = 0x2000;
        std::u16string text;
        if ((flags & kFromString) != 0) {
            text = wide ? str_detail::wstr(source, -1)
                        : str_detail::widen(reinterpret_cast<const char*>(static_cast<std::uintptr_t>(source)));
        } else if ((flags & kFromSystem) != 0) {
            const char* m = system_message(id);
            if (m == nullptr) {
                teb::set_error(s, kErrorMrMidNotFound);
                win64_return(s, 0);
                return;
            }
            text = str_detail::widen(m) + u"\r\n";
        } else {
            teb::set_error(s, kErrorMrMidNotFound);  // message tables in modules are not read
            win64_return(s, 0);
            return;
        }
        std::u16string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] != u'%' || i + 1 >= text.size() || (flags & kIgnoreInserts) != 0) {
                out.push_back(text[i]);
                continue;
            }
            const char16_t n = text[i + 1];
            if (n >= u'1' && n <= u'9') {
                unsigned index = static_cast<unsigned>(n - u'0');
                ++i;
                if (i + 1 < text.size() && text[i + 1] >= u'0' && text[i + 1] <= u'9') {
                    index = index * 10 + static_cast<unsigned>(text[++i] - u'0');
                }
                if (i + 1 < text.size() && text[i + 1] == u'!') {  // !fmt!: only s is supported
                    const std::size_t end = text.find(u'!', i + 2);
                    i = end == std::u16string::npos ? text.size() : end;
                }
                // va_list on Win64 and an argument array both hold 8-byte slots.
                const std::uint64_t slots = (flags & kArgumentArray) != 0 ? args : (args != 0 ? str_detail::get<std::uint64_t>(args) : 0);
                const std::uint64_t arg = slots != 0 ? str_detail::get<std::uint64_t>(slots + 8ULL * (index - 1)) : 0;
                if (arg != 0) {
                    out += wide ? str_detail::wstr(arg, -1)
                                : str_detail::widen(reinterpret_cast<const char*>(static_cast<std::uintptr_t>(arg)));
                }
            } else if (n == u'n') {
                out += u"\r\n";
                ++i;
            } else {
                out.push_back(n);  // %% %. %! and friends
                ++i;
            }
        }
        std::string narrow;
        if (!wide) {
            for (const char16_t c : out) {
                const int b = str_detail::unit_to_byte(kAnsiCodePage, c);
                narrow.push_back(b < 0 ? '?' : static_cast<char>(b));
            }
        }
        const std::size_t chars = wide ? out.size() : narrow.size();
        const std::size_t bytes = (chars + 1) * (wide ? 2 : 1);
        std::uint64_t dst = buffer;
        if ((flags & kAllocate) != 0) {
            dst = mem_.alloc(Memory::kProcessHeap, bytes < 2 ? 2 : bytes, false);
            if (dst == 0) {
                teb::set_error(s, kErrorNotEnoughMemory);
                win64_return(s, 0);
                return;
            }
            str_detail::put<std::uint64_t>(buffer, dst);
        } else if (chars + 1 > size) {
            teb::set_error(s, kErrorInsufficientBuffer);
            win64_return(s, 0);
            return;
        }
        if (wide) {
            copy_w(dst, out);
        } else {
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(dst)), narrow.c_str(), narrow.size() + 1);
        }
        win64_return(s, chars);
    }

    Runtime& rt_;
    Memory& mem_;
};

}  // namespace wse::recomp::shim
