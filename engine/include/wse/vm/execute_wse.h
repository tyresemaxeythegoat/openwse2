// Portable dispatch for WSE-extended statement opcodes.
//
// Evidence (Ghidra 12.1.4 pseudocode):
//   mbStatement::execute 0x1401c6150 decodes every operand through
//     mbStatement::parseOperand (0x1401c6070) into three fixed 32-element
//     arrays: int64 values, int32 truncations, and 4-byte tags. It masks the
//     opcode with 0x0FFFFFFF, dispatches opcodes 30..2767 through the native
//     jump table at 0x1401e22f0, and calls executeWSE for everything else.
//   mbStatement::executeWSE 0x1401e4dc0 receives those arrays, computes a
//     64-bit result in a handler, then writes it through operand 0 with
//     mbStatement::returnValue (0x1401c6100). A handler returning false is a
//     failed condition; a handler returning true without using returnValue is
//     a successful no-op (for example an out-of-range register).
//   Opcode 2800 is an equality-tested handler at pseudocode line 859 and
//     opcodes 2801..2809 are switch cases at lines 638..665.
//   The floating-point group starts at case 0x1130 (line 5192) and uses the
//     float array DAT_140698718 (g_basicGame+0x35d58), indexed by a register
//     number. mbCheckRegister
//     0x1401c4e60 accepts 0..127 (`cmp ecx,0x7f; ja`). DAT_140662bb8 is the
//     fixed-point scale and is set to 0x64 (100) at 0x140070ffa. fst uses
//     rglRound64 0x1402a1910: floorf(x) plus one when the fraction is > 0.5.
//     feq/fge/fle compare through rglAbs 0x1402a1850 with FLT_EPSILON.
//
// The complete 440-opcode structure is in docs/WSE_OPCODES.md and
// docs/wse_opcodes.json; wse_opcodes.h is generated from that map. This file
// implements the arithmetic/bitwise group 2800..2809, the register group
// 3000..3002, the script-context query group 3014/3016/3017/3035/3600, the
// string group 4200..4218 and 4220..4224, and the self-contained
// floating-point register group 4400, 4405..4436, and 4438. Every other known
// opcode reports WseDispatch::unimplemented instead of guessing; 4219
// (str_store_md5) is deferred until a portable MD5 is available.
//
// Register group evidence:
//   3000 register_get (line 628): mbCheckRegister(param_4[1]), then
//     (&g_globalVariables)[index] through the shared operand-0 store tail.
//   3001 register_set (line 870): mbCheckRegister(param_4[0]), then
//     (&g_globalVariables)[index] = param_3[1]; returns true, no store.
//   The bank is g_basicGame+0x270 (0x140662c30, confirmed at 0x1401e552d);
//     the 128-entry bound is the same mbCheckRegister used by the float group.
//   3002 store_wse_version (line 877, 0x1401e553a-0x1401e555b): selector 0
//     yields r15d = 5, selector 1 yields r15d = 0x483 (build 1155), and any
//     other selector falls through to the zeroed default.
//
// Script-context query evidence:
//   3014 store_current_trigger (0x1401e5917): return param_6 when it is <= 0,
//     else 0. mbStatement::execute passes its r8d context argument here; WSE
//     stores a negative value while inside a trigger (GetCurrentTrigger).
//   3016 store_num_return_values (0x1401e5979): g_numReturnValues at
//     0x140698ae8.
//   3017 store_return_value (0x1401e5985): 1-based index into the 16-slot
//     return-value bank at 0x140698af0; index 1..16 and <= g_numReturnValues
//     stores the slot, otherwise it logs and returns true without storing.
//   3035 store_application_time (0x1401e63a4): rglRound64((float)
//     DXUtil_Timer(TIMER_GETAPPTIME) * 1000.0f); FUN_1400b9690(5) returns QPC
//     seconds since the app-time baseline, and the constant at 0x140576b90 is
//     1000.0f.
//   3600 store_cur_mission_template_no (0x1401ea6b2): null
//     [0x14061f078] yields -1, otherwise the int32 at +0x9ae8.
//
// String group evidence:
//   String operands are decoded through mbCheckStringRegister (0x1401c51f0)
//   and the resolver at 0x140219c70. Tag 0 is the fixed 128-entry literal
//   table at 0x140665030 (stride 0x48); tag 0x16 addresses the local string
//   registers ([0x140621778], count [0x140621784], stride 0x90); tag 3 and
//   any unknown tag address the global string registers ([0x140621770],
//   count [0x140621780], stride 0x90). mbCheckStringRegister requires
//   0 <= index < count for tags 3/0x16, index <= 0x7f for tag 0, and accepts
//   unknown tags; a failed check logs and returns true without an effect.
//   4200 str_equals (0x1068 equality branch, 0x1401ed1bd): checks both
//     operands, then compares with wcscmp (0x1401f86b0) or _wcsicmp
//     (0x1401f86f0) when operand 2 is nonzero; equal means true.
//   4201 str_contains (0x1069): lower-cases both copies (0x1401f8b40) when
//     operand 2 is nonzero, then finds the needle (0x1401f8a80) from 0. A
//     null needle data pointer (empty string) yields -1, so an empty needle
//     is not contained.
//   4202/4203 str_starts_with/str_ends_with (0x106a/0x106b): take
//     Left/Right(needle length) (0x1401f8fd0/0x1401f8f20) and compare as in
//     str_equals.
//   4204..4206 str_is_alpha/digit/whitespace (0x106c..0x106e): iswalpha
//     (0x140498854, category 0x103), iswdigit (0x140498860, category 4), and
//     iswspace (0x14049886c, category 8). Fewer than 2 operands or index -1
//     scans the whole string; an out-of-bounds index logs and leaves the
//     initial true result.
//   4207 str_length (0x106f): length of operand 1 written through operand 0.
//   4208/4209 str_index_of/last_index_of (0x1070/0x1071): operand 1 is the
//     haystack, operand 2 the needle, operand 3 the start (default 0), and
//     operand 4 the end (default length-1). An empty needle yields -1; the
//     result is -1 unless the match is within [start, end].
//   4210 str_get_char (0x1072): UTF-16 code unit of operand 1 at operand 2;
//     an out-of-bounds index logs and stores nothing.
//   4211 str_to_num (0x1073): _wtof (0x140498878) of operand 1, multiplied by
//     DAT_140662bb8 (100.0) when operand 2 is nonzero, then truncated to
//     int64 (cvttsd2si).
//   4212 str_compare (0x1074, 0x1401ee042): operands 1/2 are strings, operand
//     3 selects the case-insensitive path. The case-sensitive wrapper
//     0x1401f86b0 normalises to -1/0/1; the case-insensitive path tail-jumps
//     to _wcsicmp (0x1404989d4), which returns the folded code-unit
//     difference. The result is written through operand 0.
//   4213 str_split (0x1075, 0x1401ee178): operand 1 is the destination tag-0
//     register (mbCheckRegister), operand 2 the source, operand 3 the
//     delimiter, operand 4 skip-empty, operand 5 max pieces (honoured only
//     when the statement has more than five operands). Operand 0 receives the
//     count. An empty delimiter cannot advance the retail loop; the port
//     stores the whole source once and terminates.
//   4214 str_sort (0x1076, 0x1401ee3eb): sorts operand 1 tag-0 registers from
//     operand 0; operand 2 selects _wcsicmp ordering and operand 3 reverses
//     the copy back into the table.
//   4215/4216 str_store_lower/upper (0x1077/0x1078): MakeLower
//     (0x1401f8b40) / MakeUpper (0x1401f8ac0) into tag-0 operand 0.
//   4217 str_store_trim (0x1079): operand 2 selects Trim (0x1401f8e20 =
//     right then left), TrimLeft (0x1401f8d00), or TrimRight (0x1401f8c40);
//     whitespace is iswspace (0x14049886c).
//   4218 str_store_replace (0x107a): replace-all (0x1401f8710) of operand 2
//     with operand 3, resuming after the replacement; an empty needle leaves
//     the string unchanged.
//   4219 str_store_md5 (0x107b): CryptAcquireContextA + 0x140330510; deferred.
//   4220 str_store_substring (0x107c, 0x1401eea5a): start operand 2 defaults
//     to 0 and length operand 3 to the end; a negative or out-of-bounds range
//     logs and leaves the destination unchanged.
//   4221 str_store_reverse (0x107d): std::reverse (0x1404992b0).
//   4222 str_store_join (0x107e): join operand 2 tag-0 registers from operand
//     1 with operand 3 when it is present and valid; an invalid separator is
//     ignored rather than failing the handler.
//   4223/4224 str_store_replace_spaces_with_underscores and the reverse
//     (0x107f/0x1080): replace u" " (0x14055b3e0) with u"_" (0x14055b3dc)
//     or the reverse.
//   The tag-0 table at 0x140665030 is writable: 0x14030fd60 assigns a
//     register, while 0x140310970 copies a register into a temporary handle.
//
// Transcendental helpers (case addresses in executeWSE):
//   pow 0x1400b0400, log 0x1400a6c20, log10 0x1400a6c10, fmod 0x14006aac0,
//   atan2 0x14006f9b0. The call sites (0x1401f08ef, 0x1401f0cbc, 0x1401f0a28)
//   pass xmm0 = fp[operand 1] and
//   xmm1 = fp[operand 2], so fpow is pow(base, exponent), atan2 is
//   atan2(y, x), and fmod is fmod(dividend, divisor). flog reads its base from
//   the int32 operand 2 (a literal): fewer than 3 operands default to 10, and
//   a base below 2 logs "invalid logarithm base %d" and returns true without
//   storing. Division and modulo by zero also log and return true.

#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <span>
#include <string>
#include <string_view>

#include "wse/vm/wse_opcodes.h"

namespace wse::vm {

inline constexpr std::uint32_t kWseOpcodeMask = 0x0FFFFFFFu;

// mbCheckRegister accepts unsigned indices 0..0x7f.
inline constexpr std::size_t kFpRegisterCount = 128;

// DAT_140662bb8, fixed-point values per unit (set to 100 at 0x140070ffa).
inline constexpr float kWseFixedPointScale = 100.0f;

// String banks addressed by the str_* handlers. The spans hold views into the
// runtime's string registers; a missing entry resolves to the empty string,
// matching the static empty rgl::string returned by 0x140219c70. Tag 0 is
// writable because str_store_* and str_split/str_sort assign into it.
struct WseStringBanks {
    // Tag 0: the 128-entry string register table at 0x140665030 (stride 0x48).
    // Script literals live here too; str_store_* overwrite entries.
    std::span<std::u16string> registers{};
    // Tag 3 (and unknown tags): [0x140621770], count [0x140621780].
    std::span<const std::u16string_view> global{};
    // Tag 0x16: [0x140621778], count [0x140621784].
    std::span<const std::u16string_view> local{};
};

// Runtime state read by the script-context query handlers. The defaults model
// a statement executed outside any trigger, with no return values, zero
// application time, and no active mission.
struct WseVmState {
    // param_6 of executeWSE: the statement block's context flags. WSE stores a
    // negative value while inside a trigger (0 outside), so store_current_trigger
    // returns it only when it is <= 0.
    std::int32_t caller = 0;
    // 0x140698af0: NUM_RETURN_VALUES (16) int64 slots written by return_values.
    std::span<const std::int64_t> return_values{};
    // 0x140698ae8: number of slots currently set by return_values.
    std::int32_t num_return_values = 0;
    // DXUtil_Timer(TIMER_GETAPPTIME) in seconds; 3035 scales it by 1000.
    double app_time_seconds = 0.0;
    // [0x14061f078] + 0x9ae8: cur_mission->cur_mission_template_no. A null
    // pointer means there is no current mission, which yields -1.
    const std::int32_t* current_mission_template_no = nullptr;
    // String registers used by the string group (4200-4218, 4220-4224).
    WseStringBanks strings{};
};

// Decoded operands, matching the arrays built by mbStatement::execute.
struct WseOperands {
    std::span<const std::int64_t> wide{};     // param_3, full int64 values
    std::span<const std::int32_t> narrow{};   // param_4, int32 truncations
    std::span<const std::uint8_t> tags{};     // param_5, parseOperand tags
    std::span<float> fp{};                    // DAT_140698718, 128 floats
    // g_basicGame+0x270 (0x140662c30): the 128 int64 registers shared with
    // operand tag 1 and used by register_get/register_set (3000/3001).
    std::span<std::int64_t> registers{};
    // Script-context state for the global query handlers (3014-3600).
    WseVmState state{};
};

// Result of a successful handler. Value-writing handlers (mbStatement's
// common LAB_1401e0e0e tail) write effect.value through operand 0; the
// floating-point and comparison handlers write their own destinations.
struct WseEffect {
    std::int64_t value = 0;
    bool store_value = true;
};

enum class WseDispatch : std::uint8_t {
    handled = 0,                 // success; write effect.value through operand 0
    handled_no_store,            // success; handler wrote its own destination
    condition_false,             // executeWSE returned false (failed condition)
    ignored,                     // success; retail returned true without effect
    unimplemented,
    operand_count_out_of_range,
};

[[nodiscard]] constexpr std::string_view describe(WseDispatch result) noexcept {
    switch (result) {
    case WseDispatch::handled:
        return "handled";
    case WseDispatch::handled_no_store:
        return "handled without operand-0 store";
    case WseDispatch::condition_false:
        return "false condition";
    case WseDispatch::ignored:
        return "ignored (invalid index)";
    case WseDispatch::unimplemented:
        return "unimplemented WSE opcode";
    case WseDispatch::operand_count_out_of_range:
        return "WSE opcode operand count out of range";
    }
    return "unknown WSE dispatch result";
}

// The shift amount is `(byte)operand & 0x3F`, exactly as the retail handlers
// use it (x86-64 masks a 64-bit shift by 63).
[[nodiscard]] constexpr unsigned wse_shift(std::int64_t value) noexcept {
    return static_cast<unsigned>(value & 0x3F);
}

// mbCheckRegister: unsigned comparison against 0x7f.
[[nodiscard]] constexpr bool wse_register_ok(std::int32_t index) noexcept {
    return static_cast<std::uint32_t>(index) <= 0x7Fu;
}

// rglRound64 (0x1402a1910): floorf(x), plus one when the fraction is > 0.5.
// Out-of-range values become INT64_MIN because the retail conversion is
// cvttss2si; this avoids undefined behaviour on the host.
[[nodiscard]] inline std::int64_t wse_round64(float value) noexcept {
    const float base = std::floor(value);
    if (!std::isfinite(base) ||
        base < static_cast<float>(std::numeric_limits<std::int64_t>::min()) ||
        base >= static_cast<float>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    auto result = static_cast<std::int64_t>(base);
    if (value - base > 0.5f) {
        ++result;
    }
    return result;
}

// cvttsd2si semantics: NaN and out-of-range values become INT64_MIN.
[[nodiscard]] inline std::int64_t wse_trunc64(double value) noexcept {
    if (!std::isfinite(value) ||
        value < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

// mbCheckStringRegister (0x1401c51f0): tags 3 and 0x16 are bounded by the
// runtime register counts, tag 0 by the fixed 128-entry literal table, and
// unknown tags are accepted.
[[nodiscard]] inline bool wse_string_register_ok(
    std::int32_t index, std::uint8_t tag, const WseStringBanks& banks) noexcept {
    switch (tag) {
    case 0:
        return static_cast<std::uint32_t>(index) <= 0x7Fu;
    case 3:
        return index >= 0 &&
               static_cast<std::size_t>(index) < banks.global.size();
    case 0x16:
        return index >= 0 &&
               static_cast<std::size_t>(index) < banks.local.size();
    default:
        return true;
    }
}

// 0x140219c70: tag 0 reads the string register table, tag 0x16 the local
// bank, and tag 3 or an unknown tag the global bank. Out-of-range reads
// resolve to the static empty rgl::string (0x140621818 / 0x140621788).
[[nodiscard]] inline std::u16string_view wse_resolve_string(
    std::int32_t index, std::uint8_t tag, const WseStringBanks& banks) noexcept {
    if (tag == 0) {
        if (static_cast<std::uint32_t>(index) <= 0x7Fu &&
            static_cast<std::size_t>(index) < banks.registers.size()) {
            return banks.registers[static_cast<std::size_t>(index)];
        }
        return {};
    }
    const auto bank = tag == 0x16 ? banks.local : banks.global;
    if (index >= 0 && static_cast<std::size_t>(index) < bank.size()) {
        return bank[static_cast<std::size_t>(index)];
    }
    return {};
}

// 0x14030fd60 assigns a tag-0 string register. A missing table entry cannot
// be written, so the handler reports the retail invalid-register no-op.
[[nodiscard]] inline bool wse_store_string(WseStringBanks& banks,
                                           std::int32_t index,
                                           std::u16string_view value) {
    if (index < 0 || static_cast<std::uint32_t>(index) > 0x7Fu ||
        static_cast<std::size_t>(index) >= banks.registers.size()) {
        return false;
    }
    banks.registers[static_cast<std::size_t>(index)].assign(value);
    return true;
}

// Missing tags and operands read as zero: mbStatement::execute zeroes the
// first five tag and operand slots before parsing (0x1401c6219-0x1401c626c).
[[nodiscard]] constexpr std::uint8_t wse_tag_at(
    std::span<const std::uint8_t> tags, std::size_t index) noexcept {
    return index < tags.size() ? tags[index] : std::uint8_t{0};
}

// _wcsicmp folds one UTF-16 code unit through towlower; keep ASCII folding
// deterministic and fall back to the C library for the rest of the BMP.
[[nodiscard]] inline char16_t wse_fold_case(char16_t value) noexcept {
    if (value >= u'A' && value <= u'Z') {
        return static_cast<char16_t>(value + (u'a' - u'A'));
    }
    if (value < 0x80) {
        return value;
    }
    const long folded =
        static_cast<long>(std::towlower(static_cast<std::wint_t>(value)));
    if (folded >= 0 && folded <= 0xFFFF) {
        return static_cast<char16_t>(folded);
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

// _wtof (0x140498878): the CRT parses the longest decimal/inf/nan prefix and
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

// towupper counterpart of wse_fold_case for str_store_upper (0x1401f8ac0).
[[nodiscard]] inline char16_t wse_upper_case(char16_t value) noexcept {
    if (value >= u'a' && value <= u'z') {
        return static_cast<char16_t>(value - (u'a' - u'A'));
    }
    if (value < 0x80) {
        return value;
    }
    const long folded =
        static_cast<long>(std::towupper(static_cast<std::wint_t>(value)));
    if (folded >= 0 && folded <= 0xFFFF) {
        return static_cast<char16_t>(folded);
    }
    return value;
}

// mbString comparison behind str_compare (0x1074). The case-sensitive wrapper
// 0x1401f86b0 normalises to -1/0/1; the case-insensitive path tail-jumps to
// _wcsicmp (0x1404989d4), which returns the folded code-unit difference.
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

// 0x1401f8710: replace every non-overlapping occurrence of `needle`, resuming
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

[[nodiscard]] inline bool wse_is_space(char16_t value) noexcept {
    return std::iswspace(static_cast<std::wint_t>(value)) != 0;
}

// 0x1401f8d00 removes leading whitespace.
inline void wse_trim_left(std::u16string& text) {
    std::size_t first = 0;
    while (first < text.size() && wse_is_space(text[first])) {
        ++first;
    }
    text.erase(0, first);
}

// 0x1401f8c40 removes trailing whitespace.
inline void wse_trim_right(std::u16string& text) {
    std::size_t last = text.size();
    while (last > 0 && wse_is_space(text[last - 1])) {
        --last;
    }
    text.erase(last);
}

// 0x1401f8e20 trims both ends (right first, then left).
inline void wse_trim(std::u16string& text) {
    wse_trim_right(text);
    wse_trim_left(text);
}

// Shared body of str_is_alpha/digit/whitespace. `has_index == false` or
// index -1 scans the whole string; an out-of-bounds index logs and returns
// the initial true result.
template <typename Predicate>
[[nodiscard]] inline WseDispatch wse_string_test(std::u16string_view text,
                                                 std::int32_t index,
                                                 bool has_index,
                                                 Predicate predicate) noexcept {
    if (!has_index || index == -1) {
        for (const char16_t value : text) {
            if (!predicate(value)) {
                return WseDispatch::condition_false;
            }
        }
        return WseDispatch::handled_no_store;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= text.size()) {
        return WseDispatch::ignored;
    }
    return predicate(text[static_cast<std::size_t>(index)])
               ? WseDispatch::handled_no_store
               : WseDispatch::condition_false;
}

// `val_*` uses operand 0 as both source and destination; `store_*` reads
// operands 1.. and leaves operand 0 as the destination lvalue for the caller.
[[nodiscard]] inline WseDispatch execute_wse(std::uint32_t opcode,
                                             const WseOperands& operands,
                                             WseEffect& effect) noexcept {
    opcode &= kWseOpcodeMask;
    effect.store_value = false;
    const auto wide = operands.wide;
    const auto narrow = operands.narrow;
    const auto tags = operands.tags;
    const auto fp = operands.fp;
    const auto registers = operands.registers;
    auto strings = operands.state.strings;

    switch (static_cast<WseOpcode>(opcode)) {
    case WseOpcode::val_shr: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[0] >> wse_shift(wide[1]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_shr: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[1] >> wse_shift(wide[2]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_lshr: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[0]) >> wse_shift(wide[1]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_lshr: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[1]) >> wse_shift(wide[2]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_shl: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[0]) << wse_shift(wide[1]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_shl: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = static_cast<std::int64_t>(
            static_cast<std::uint64_t>(wide[1]) << wse_shift(wide[2]));
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_xor: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[0] ^ wide[1];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_xor: {
        if (wide.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = wide[1] ^ wide[2];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::val_not: {
        if (wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = ~wide[0];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_not: {
        if (wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        effect.value = ~wide[1];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::register_get: {
        // 3000 (pseudocode line 628): checked index in the int64 register
        // bank, then the shared operand-0 store tail.
        if (narrow.size() < 2 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[1]) || registers.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[1]);
        if (index >= registers.size()) {
            return WseDispatch::ignored;
        }
        effect.value = registers[index];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::register_set: {
        // 3001 (line 870): the destination is the register named by operand 0;
        // the value is operand 1. Retail returns true without returnValue.
        if (narrow.size() < 1 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || registers.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[0]);
        if (index >= registers.size()) {
            return WseDispatch::ignored;
        }
        registers[index] = wide[1];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::store_wse_version: {
        // 3002 (line 877): 0 yields the WSE version (5), 1 the Warband build
        // (0x483 = 1155); any other argument yields the zeroed r15 default.
        if (narrow.size() < 2 || wide.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::int32_t which = narrow[1];
        effect.value = which == 0 ? 5 : (which == 1 ? 0x483 : 0);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_current_trigger: {
        // 3014 (0x1401e5917): return the context flags while inside a trigger
        // (negative), otherwise 0. WSEOperationContext::GetCurrentTrigger uses
        // exactly this `m_context_flags < 0 ? m_context_flags : 0` rule.
        effect.value = operands.state.caller <= 0 ? operands.state.caller : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_num_return_values: {
        // 3016 (0x1401e5979): g_numReturnValues at 0x140698ae8.
        effect.value = operands.state.num_return_values;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_return_value: {
        // 3017 (0x1401e5985): operand 1 is a 1-based index into the 16-slot
        // return-value bank at 0x140698af0. Out-of-range or unset indices log
        // ("invalid script result index %d" / "unset result value %d") and
        // return true without storing.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::int32_t index = narrow[1];
        if (index < 1 || index > 16) {
            return WseDispatch::ignored;
        }
        if (index > operands.state.num_return_values) {
            return WseDispatch::ignored;
        }
        const auto slot = static_cast<std::size_t>(index - 1);
        if (slot >= operands.state.return_values.size()) {
            return WseDispatch::ignored;
        }
        effect.value = operands.state.return_values[slot];
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_application_time: {
        // 3035 (0x1401e63a4): rglRound64((float)DXUtil_Timer(5) * 1000.0f).
        // FUN_1400b9690(5) returns QPC seconds since the app-time baseline.
        effect.value = wse_round64(
            static_cast<float>(operands.state.app_time_seconds) * 1000.0f);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::store_cur_mission_template_no: {
        // 3600 (0x1401ea6b2): null g_currentMission (0x14061f078) yields -1,
        // otherwise the int32 at +0x9ae8 (cur_mission_template_no).
        effect.value = operands.state.current_mission_template_no
                           ? *operands.state.current_mission_template_no
                           : -1;
        effect.store_value = true;
        return WseDispatch::handled;
    }

    // String group 4200..4211. A failed mbCheckStringRegister returns true
    // without an effect; condition handlers therefore report `ignored`, which
    // the trigger runtime treats as a passing condition.
    case WseOpcode::str_equals: {
        // 4200 (0x1068 equality branch, 0x1401ed1bd): compare operands 0 and
        // 1 with wcscmp or _wcsicmp when operand 2 is nonzero.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[0], tag0, strings) ||
            !wse_string_register_ok(narrow[1], tag1, strings)) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 3 && narrow[2] != 0;
        return wse_string_equals(wse_resolve_string(narrow[0], tag0, strings),
                                 wse_resolve_string(narrow[1], tag1, strings),
                                 case_insensitive)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::str_contains: {
        // 4201 (0x1069): lower-case both strings when operand 2 is nonzero,
        // then find the needle from 0. An empty needle is not contained.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[0], tag0, strings) ||
            !wse_string_register_ok(narrow[1], tag1, strings)) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 3 && narrow[2] != 0;
        return wse_string_contains(wse_resolve_string(narrow[0], tag0, strings),
                                   wse_resolve_string(narrow[1], tag1, strings),
                                   case_insensitive)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::str_starts_with: {
        // 4202 (0x106a): compare Left(haystack, needle length) with the
        // needle, using the same case rule as str_equals.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[0], tag0, strings) ||
            !wse_string_register_ok(narrow[1], tag1, strings)) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 3 && narrow[2] != 0;
        return wse_string_starts_with(
                   wse_resolve_string(narrow[0], tag0, strings),
                   wse_resolve_string(narrow[1], tag1, strings),
                   case_insensitive)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::str_ends_with: {
        // 4203 (0x106b): compare Right(haystack, needle length) with the
        // needle, using the same case rule as str_equals.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag0 = wse_tag_at(tags, 0);
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[0], tag0, strings) ||
            !wse_string_register_ok(narrow[1], tag1, strings)) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 3 && narrow[2] != 0;
        return wse_string_ends_with(
                   wse_resolve_string(narrow[0], tag0, strings),
                   wse_resolve_string(narrow[1], tag1, strings),
                   case_insensitive)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::str_is_alpha: {
        // 4204 (0x106c): iswalpha over operand 0, or only operand 1 when it
        // is present and not -1.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 0);
        if (!wse_string_register_ok(narrow[0], tag, strings)) {
            return WseDispatch::ignored;
        }
        return wse_string_test(
            wse_resolve_string(narrow[0], tag, strings),
            narrow.size() >= 2 ? narrow[1] : -1, narrow.size() >= 2,
            [](char16_t value) {
                return std::iswalpha(static_cast<std::wint_t>(value)) != 0;
            });
    }
    case WseOpcode::str_is_digit: {
        // 4205 (0x106d): iswdigit over operand 0, or only operand 1 when it
        // is present and not -1.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 0);
        if (!wse_string_register_ok(narrow[0], tag, strings)) {
            return WseDispatch::ignored;
        }
        return wse_string_test(
            wse_resolve_string(narrow[0], tag, strings),
            narrow.size() >= 2 ? narrow[1] : -1, narrow.size() >= 2,
            [](char16_t value) {
                return std::iswdigit(static_cast<std::wint_t>(value)) != 0;
            });
    }
    case WseOpcode::str_is_whitespace: {
        // 4206 (0x106e): iswspace over operand 0, or only operand 1 when it
        // is present and not -1.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 0);
        if (!wse_string_register_ok(narrow[0], tag, strings)) {
            return WseDispatch::ignored;
        }
        return wse_string_test(
            wse_resolve_string(narrow[0], tag, strings),
            narrow.size() >= 2 ? narrow[1] : -1, narrow.size() >= 2,
            [](char16_t value) {
                return std::iswspace(static_cast<std::wint_t>(value)) != 0;
            });
    }
    case WseOpcode::str_length: {
        // 4207 (0x106f): length of operand 1 written through operand 0.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        effect.value = static_cast<std::int64_t>(
            wse_resolve_string(narrow[1], tag, strings).size());
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_index_of: {
        // 4208 (0x1070): operand 1 is the haystack, operand 2 the needle,
        // operand 3 the start (default 0), operand 4 the end (default
        // length-1). The result is -1 unless the match is inside [start, end].
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t haystack_tag = wse_tag_at(tags, 1);
        const std::uint8_t needle_tag = wse_tag_at(tags, 2);
        if (!wse_string_register_ok(narrow[1], haystack_tag, strings) ||
            !wse_string_register_ok(narrow[2], needle_tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto haystack =
            wse_resolve_string(narrow[1], haystack_tag, strings);
        const auto needle = wse_resolve_string(narrow[2], needle_tag, strings);
        const std::int32_t start = narrow.size() >= 4 ? narrow[3] : 0;
        const std::int32_t end =
            narrow.size() >= 5
                ? narrow[4]
                : static_cast<std::int32_t>(haystack.size()) - 1;
        std::int64_t result = -1;
        if (start >= 0 && end >= 0 &&
            static_cast<std::size_t>(start) < haystack.size() &&
            static_cast<std::size_t>(end) < haystack.size() && start <= end &&
            !needle.empty()) {
            const auto found =
                haystack.find(needle, static_cast<std::size_t>(start));
            if (found != std::u16string_view::npos &&
                found <= static_cast<std::size_t>(end)) {
                result = static_cast<std::int64_t>(found);
            }
        }
        effect.value = result;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_last_index_of: {
        // 4209 (0x1071): mirror the retail loop, which advances past each
        // match and returns the previous match when the next search starts at
        // or beyond `end`; the final result must also be <= end.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t haystack_tag = wse_tag_at(tags, 1);
        const std::uint8_t needle_tag = wse_tag_at(tags, 2);
        if (!wse_string_register_ok(narrow[1], haystack_tag, strings) ||
            !wse_string_register_ok(narrow[2], needle_tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto haystack =
            wse_resolve_string(narrow[1], haystack_tag, strings);
        const auto needle = wse_resolve_string(narrow[2], needle_tag, strings);
        const std::int32_t start = narrow.size() >= 4 ? narrow[3] : 0;
        const std::int32_t end =
            narrow.size() >= 5
                ? narrow[4]
                : static_cast<std::int32_t>(haystack.size()) - 1;
        std::int64_t result = -1;
        if (start >= 0 && end >= 0 &&
            static_cast<std::size_t>(start) < haystack.size() &&
            static_cast<std::size_t>(end) < haystack.size() && start <= end) {
            std::int64_t found = -1;
            std::int64_t previous = -1;
            std::int32_t position = start;
            for (;;) {
                previous = found;
                if (end <= position) {
                    break;
                }
                if (needle.empty()) {
                    found = -1;
                } else {
                    const auto at = haystack.find(
                        needle, static_cast<std::size_t>(position));
                    found = at == std::u16string_view::npos
                                ? -1
                                : static_cast<std::int64_t>(at);
                }
                position = static_cast<std::int32_t>(found + 1);
                if (found == -1) {
                    break;
                }
            }
            if (previous <= end) {
                result = previous;
            }
        }
        effect.value = result;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_get_char: {
        // 4210 (0x1072): UTF-16 code unit of operand 1 at operand 2. An
        // out-of-bounds index logs and stores nothing.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto text = wse_resolve_string(narrow[1], tag, strings);
        const std::int32_t index = narrow.size() >= 3 ? narrow[2] : 0;
        if (index < 0 || static_cast<std::size_t>(index) >= text.size()) {
            return WseDispatch::ignored;
        }
        effect.value =
            static_cast<std::int64_t>(text[static_cast<std::size_t>(index)]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_to_num: {
        // 4211 (0x1073): _wtof of operand 1, scaled by the fixed-point factor
        // when operand 2 is nonzero, then truncated to int64.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        double value = wse_wtof(wse_resolve_string(narrow[1], tag, strings));
        if (narrow.size() >= 3 && narrow[2] != 0) {
            value *= static_cast<double>(kWseFixedPointScale);
        }
        effect.value = wse_trunc64(value);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_compare: {
        // 4212 (0x1074): operand 1 and 2 are strings, operand 3 selects
        // _wcsicmp (0x1401f86f0) instead of the normalised compare
        // (0x1401f86b0). The result is written through operand 0.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag1 = wse_tag_at(tags, 1);
        const std::uint8_t tag2 = wse_tag_at(tags, 2);
        if (!wse_string_register_ok(narrow[1], tag1, strings) ||
            !wse_string_register_ok(narrow[2], tag2, strings)) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 4 && narrow[3] != 0;
        effect.value = wse_string_compare(
            wse_resolve_string(narrow[1], tag1, strings),
            wse_resolve_string(narrow[2], tag2, strings), case_insensitive);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_split: {
        // 4213 (0x1075): split operand 2 by operand 3 into tag-0 registers
        // starting at operand 1, at most operand 5 pieces, skipping empty
        // pieces when operand 4 is nonzero. Operand 0 receives the count.
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[1])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t source_tag = wse_tag_at(tags, 2);
        const std::uint8_t delimiter_tag = wse_tag_at(tags, 3);
        if (!wse_string_register_ok(narrow[2], source_tag, strings) ||
            !wse_string_register_ok(narrow[3], delimiter_tag, strings)) {
            return WseDispatch::ignored;
        }
        const std::u16string source(
            wse_resolve_string(narrow[2], source_tag, strings));
        const std::u16string delimiter(
            wse_resolve_string(narrow[3], delimiter_tag, strings));
        const bool skip_empty = narrow.size() >= 5 && narrow[4] != 0;
        const std::int32_t max_pieces =
            narrow.size() >= 6 ? narrow[5] : 0;
        std::int32_t destination = narrow[1];
        std::int32_t count = 0;

        if (delimiter.empty()) {
            // The retail loop cannot advance when the delimiter is empty and
            // would spin (forever when skip_empty is set). Treat the input as
            // a single piece so the port always terminates.
            const bool keep = !skip_empty || !source.empty();
            if (keep && !wse_store_string(strings, destination, source)) {
                return WseDispatch::ignored;
            }
            effect.value = keep ? 1 : 0;
            effect.store_value = true;
            return WseDispatch::handled;
        }

        std::int32_t position = 0;
        while (position <= static_cast<std::int32_t>(source.size())) {
            const auto at =
                source.find(delimiter, static_cast<std::size_t>(position));
            const std::int32_t found =
                at == std::u16string::npos
                    ? static_cast<std::int32_t>(source.size())
                    : static_cast<std::int32_t>(at);
            if (narrow.size() >= 6 && max_pieces != 0 &&
                count >= max_pieces) {
                break;
            }
            if (destination > 0x7F) {
                break;
            }
            const auto piece = std::u16string_view(source).substr(
                static_cast<std::size_t>(position),
                static_cast<std::size_t>(found - position));
            if (!skip_empty || !piece.empty()) {
                if (!wse_store_string(strings, destination, piece)) {
                    break;
                }
                ++destination;
                ++count;
            }
            position = found + static_cast<std::int32_t>(delimiter.size());
        }
        effect.value = count;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::str_sort: {
        // 4214 (0x1076): sort operand 1 tag-0 registers starting at operand 0.
        // Operand 2 selects the case-insensitive comparator; operand 3 copies
        // the sorted range back in reverse order (descending).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        std::int32_t count = narrow.size() >= 2 ? narrow[1] : 0;
        if (count < 1) {
            return WseDispatch::handled_no_store;
        }
        const std::int32_t first = narrow[0];
        if (first + count > 0x80) {
            count = 0x80 - first;
        }
        if (static_cast<std::size_t>(first + count) > strings.registers.size()) {
            count = static_cast<std::int32_t>(strings.registers.size()) - first;
        }
        if (count < 1) {
            return WseDispatch::ignored;
        }
        const bool case_insensitive = narrow.size() >= 3 && narrow[2] != 0;
        const bool descending = narrow.size() >= 4 && narrow[3] != 0;
        auto* begin = strings.registers.data() + first;
        auto* end = begin + count;
        std::sort(begin, end,
                  [case_insensitive](const std::u16string& left,
                                     const std::u16string& right) {
                      return wse_string_compare(left, right, case_insensitive) <
                             0;
                  });
        if (descending) {
            std::reverse(begin, end);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::str_store_lower: {
        // 4215 (0x1077): MakeLower (0x1401f8b40) of operand 1 into tag-0
        // register operand 0.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        std::u16string text(wse_resolve_string(narrow[1], tag, strings));
        for (char16_t& value : text) {
            value = wse_fold_case(value);
        }
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_upper: {
        // 4216 (0x1078): MakeUpper (0x1401f8ac0) of operand 1 into tag-0
        // register operand 0.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        std::u16string text(wse_resolve_string(narrow[1], tag, strings));
        for (char16_t& value : text) {
            value = wse_upper_case(value);
        }
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_trim: {
        // 4217 (0x1079): operand 2 selects Trim (0x1401f8e20), TrimLeft
        // (0x1401f8d00), or TrimRight (0x1401f8c40); other values copy the
        // source unchanged.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        std::u16string text(wse_resolve_string(narrow[1], tag, strings));
        const std::int32_t mode = narrow.size() >= 3 ? narrow[2] : 0;
        if (mode == 0) {
            wse_trim(text);
        } else if (mode == 1) {
            wse_trim_left(text);
        } else if (mode == 2) {
            wse_trim_right(text);
        }
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_replace: {
        // 4218 (0x107a): replace occurrences of operand 2 in operand 1 with
        // operand 3 (0x1401f8710) and store the result in tag-0 operand 0.
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t source_tag = wse_tag_at(tags, 1);
        const std::uint8_t needle_tag = wse_tag_at(tags, 2);
        const std::uint8_t replacement_tag = wse_tag_at(tags, 3);
        if (!wse_string_register_ok(narrow[1], source_tag, strings) ||
            !wse_string_register_ok(narrow[2], needle_tag, strings) ||
            !wse_string_register_ok(narrow[3], replacement_tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto text = wse_replace_all(
            std::u16string(wse_resolve_string(narrow[1], source_tag, strings)),
            wse_resolve_string(narrow[2], needle_tag, strings),
            wse_resolve_string(narrow[3], replacement_tag, strings));
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_substring: {
        // 4220 (0x107c): substring of operand 1 from operand 2 (default 0)
        // with operand 3 characters (default to the end). An out-of-bounds
        // range logs and leaves the destination unchanged.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        const std::u16string source(
            wse_resolve_string(narrow[1], tag, strings));
        const std::int32_t start = narrow.size() >= 3 ? narrow[2] : 0;
        const std::int32_t length =
            narrow.size() >= 4
                ? narrow[3]
                : static_cast<std::int32_t>(source.size()) - start;
        if (start < 0 || length < 0 ||
            static_cast<std::int64_t>(start) + length >
                static_cast<std::int64_t>(source.size())) {
            return WseDispatch::ignored;
        }
        const auto piece = std::u16string_view(source).substr(
            static_cast<std::size_t>(start),
            static_cast<std::size_t>(length));
        return wse_store_string(strings, narrow[0], piece)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_reverse: {
        // 4221 (0x107d): reverse the UTF-16 code units of operand 1 into
        // tag-0 operand 0 (std::reverse at 0x1404992b0).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        std::u16string text(wse_resolve_string(narrow[1], tag, strings));
        std::reverse(text.begin(), text.end());
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_join: {
        // 4222 (0x107e): join operand 2 tag-0 registers starting at operand 1
        // with operand 3 (only when the statement supplies it and the
        // separator register is valid) into operand 0.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1])) {
            return WseDispatch::ignored;
        }
        const std::int32_t count = narrow[2];
        if (count < 1) {
            return WseDispatch::handled_no_store;
        }
        std::u16string separator;
        bool has_separator = false;
        if (narrow.size() >= 4) {
            const std::uint8_t separator_tag = wse_tag_at(tags, 3);
            if (wse_string_register_ok(narrow[3], separator_tag, strings)) {
                separator.assign(
                    wse_resolve_string(narrow[3], separator_tag, strings));
                has_separator = true;
            }
        }
        std::int32_t end = narrow[1] + count;
        if (end > 0x80) {
            end = 0x80;
        }
        if (static_cast<std::size_t>(end) > strings.registers.size()) {
            end = static_cast<std::int32_t>(strings.registers.size());
        }
        std::u16string joined;
        for (std::int32_t index = narrow[1]; index < end; ++index) {
            joined.append(strings.registers[static_cast<std::size_t>(index)]);
            if (has_separator && index < end - 1) {
                joined.append(separator);
            }
        }
        return wse_store_string(strings, narrow[0], joined)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_replace_spaces_with_underscores: {
        // 4223 (0x107f): replace u" " (0x14055b3e0) with u"_"
        // (0x14055b3dc) in operand 1 and store the result in operand 0.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto text = wse_replace_all(
            std::u16string(wse_resolve_string(narrow[1], tag, strings)), u" ",
            u"_");
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_replace_underscores_with_spaces: {
        // 4224 (0x1080): the reverse of 4223, replacing u"_" with u" ".
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[1], tag, strings)) {
            return WseDispatch::ignored;
        }
        const auto text = wse_replace_all(
            std::u16string(wse_resolve_string(narrow[1], tag, strings)), u"_",
            u" ");
        return wse_store_string(strings, narrow[0], text)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }

    // Floating-point registers. The retail cases check every register before
    // doing anything; a failed check falls through to `return true`, which is
    // a successful no-op.
    case WseOpcode::fld: {
        if (narrow.size() < 1 || wide.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[0]);
        if (index >= fp.size()) {
            return WseDispatch::ignored;
        }
        fp[index] = static_cast<float>(wide[1]) / kWseFixedPointScale;
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::fst: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[1]);
        if (index >= fp.size()) {
            return WseDispatch::ignored;
        }
        effect.value = wse_round64(kWseFixedPointScale * fp[index]);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::fcpy: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto source = static_cast<std::size_t>(narrow[1]);
        if (destination >= fp.size() || source >= fp.size()) {
            return WseDispatch::ignored;
        }
        fp[destination] = fp[source];
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::feq: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float difference = std::fabs(fp[lhs] - fp[rhs]);
        return difference <= FLT_EPSILON ? WseDispatch::handled_no_store
                                         : WseDispatch::condition_false;
    }
    case WseOpcode::fgt: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        return fp[rhs] <= fp[lhs] && fp[lhs] != fp[rhs]
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::flt: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        return fp[lhs] <= fp[rhs] && fp[lhs] != fp[rhs]
                   ? WseDispatch::handled_no_store
                   : WseDispatch::condition_false;
    }
    case WseOpcode::fge: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float a = fp[lhs];
        const float b = fp[rhs];
        bool result = true;
        if (a < b || a == b) {
            if (std::fabs(a - b) > FLT_EPSILON) {
                result = false;
            }
        }
        return result ? WseDispatch::handled_no_store
                      : WseDispatch::condition_false;
    }
    case WseOpcode::fle: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto lhs = static_cast<std::size_t>(narrow[0]);
        const auto rhs = static_cast<std::size_t>(narrow[1]);
        if (lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float a = fp[lhs];
        const float b = fp[rhs];
        bool result = true;
        if (b < a || b == a) {
            if (std::fabs(a - b) > FLT_EPSILON) {
                result = false;
            }
        }
        return result ? WseDispatch::handled_no_store
                      : WseDispatch::condition_false;
    }
    case WseOpcode::fsub:
    case WseOpcode::fmul:
    case WseOpcode::fdiv:
    case WseOpcode::fmin:
    case WseOpcode::fmax:
    case WseOpcode::fadd: {
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto lhs = static_cast<std::size_t>(narrow[1]);
        const auto rhs = static_cast<std::size_t>(narrow[2]);
        if (destination >= fp.size() || lhs >= fp.size() || rhs >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float a = fp[lhs];
        const float b = fp[rhs];
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fsub:
            fp[destination] = a - b;
            break;
        case WseOpcode::fmul:
            fp[destination] = a * b;
            break;
        case WseOpcode::fdiv:
            if (b == 0.0f) {
                return WseDispatch::handled_no_store;
            }
            fp[destination] = a / b;
            break;
        case WseOpcode::fmin:
            fp[destination] = b <= a ? b : a;
            break;
        case WseOpcode::fmax:
            fp[destination] = a <= b ? b : a;
            break;
        case WseOpcode::fadd:
            fp[destination] = a + b;
            break;
        default:
            break;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::fclamp: {
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || !wse_register_ok(narrow[3]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto value = static_cast<std::size_t>(narrow[1]);
        const auto first = static_cast<std::size_t>(narrow[2]);
        const auto second = static_cast<std::size_t>(narrow[3]);
        if (destination >= fp.size() || value >= fp.size() || first >= fp.size() ||
            second >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float v = fp[value];
        const float bound_a = fp[first];
        const float bound_b = fp[second];
        const float low = bound_b <= bound_a ? bound_b : bound_a;
        const float high = bound_a <= bound_b ? bound_b : bound_a;
        fp[destination] = v < low ? low : (v <= high ? v : high);
        return WseDispatch::handled_no_store;
    }

    // Transcendental unary register operations: operand 0 is the destination,
    // operand 1 the source. All register checks happen before evaluation, and
    // a failed check is the retail `return true` no-op.
    case WseOpcode::fsqrt:
    case WseOpcode::fabs:
    case WseOpcode::fceil:
    case WseOpcode::ffloor:
    case WseOpcode::fexp:
    case WseOpcode::fln:
    case WseOpcode::fsin:
    case WseOpcode::fcos:
    case WseOpcode::ftan:
    case WseOpcode::fsinh:
    case WseOpcode::fcosh:
    case WseOpcode::ftanh:
    case WseOpcode::fasin:
    case WseOpcode::facos:
    case WseOpcode::fatan: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto source = static_cast<std::size_t>(narrow[1]);
        if (destination >= fp.size() || source >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float value = fp[source];
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fsqrt:
            fp[destination] = std::sqrt(value);
            break;
        case WseOpcode::fabs:
            fp[destination] = std::fabs(value);
            break;
        case WseOpcode::fceil:
            fp[destination] = std::ceil(value);
            break;
        case WseOpcode::ffloor:
            fp[destination] = std::floor(value);
            break;
        case WseOpcode::fexp:
            fp[destination] = std::exp(value);
            break;
        case WseOpcode::fln:
            fp[destination] = std::log(value);
            break;
        case WseOpcode::fsin:
            fp[destination] = std::sin(value);
            break;
        case WseOpcode::fcos:
            fp[destination] = std::cos(value);
            break;
        case WseOpcode::ftan:
            fp[destination] = std::tan(value);
            break;
        case WseOpcode::fsinh:
            fp[destination] = std::sinh(value);
            break;
        case WseOpcode::fcosh:
            fp[destination] = std::cosh(value);
            break;
        case WseOpcode::ftanh:
            fp[destination] = std::tanh(value);
            break;
        case WseOpcode::fasin:
            fp[destination] = std::asin(value);
            break;
        case WseOpcode::facos:
            fp[destination] = std::acos(value);
            break;
        case WseOpcode::fatan:
            fp[destination] = std::atan(value);
            break;
        default:
            break;
        }
        return WseDispatch::handled_no_store;
    }

    // Binary register operations. xmm0 carries operand 1, xmm1 operand 2:
    // pow(base, exponent), atan2(y, x), and fmod(dividend, divisor).
    case WseOpcode::fpow:
    case WseOpcode::fmod:
    case WseOpcode::fatan2: {
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            !wse_register_ok(narrow[2]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto lhs_index = static_cast<std::size_t>(narrow[1]);
        const auto rhs_index = static_cast<std::size_t>(narrow[2]);
        if (destination >= fp.size() || lhs_index >= fp.size() ||
            rhs_index >= fp.size()) {
            return WseDispatch::ignored;
        }
        const float lhs = fp[lhs_index];
        const float rhs = fp[rhs_index];
        switch (static_cast<WseOpcode>(opcode)) {
        case WseOpcode::fpow:
            fp[destination] = std::pow(lhs, rhs);
            break;
        case WseOpcode::fmod:
            if (rhs == 0.0f) {
                return WseDispatch::handled_no_store;
            }
            fp[destination] = std::fmod(lhs, rhs);
            break;
        case WseOpcode::fatan2:
            fp[destination] = std::atan2(lhs, rhs);
            break;
        default:
            break;
        }
        return WseDispatch::handled_no_store;
    }

    // flog: log10(fp[operand 1]) / log10(base). The base is the int32 literal
    // in operand 2; with fewer than three operands the retail default is 10.
    case WseOpcode::flog: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) || fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto destination = static_cast<std::size_t>(narrow[0]);
        const auto source = static_cast<std::size_t>(narrow[1]);
        if (destination >= fp.size() || source >= fp.size()) {
            return WseDispatch::ignored;
        }
        std::int32_t base = 10;
        if (narrow.size() >= 3) {
            base = narrow[2];
        }
        if (base < 2) {
            return WseDispatch::handled_no_store;  // "invalid logarithm base %d"
        }
        fp[destination] =
            std::log10(fp[source]) / std::log10(static_cast<float>(base));
        return WseDispatch::handled_no_store;
    }

    default:
        break;
    }
    return WseDispatch::unimplemented;
}

}  // namespace wse::vm
