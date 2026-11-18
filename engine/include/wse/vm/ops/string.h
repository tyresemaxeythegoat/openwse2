#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/vm/wse_state.h"

namespace wse::vm {

// Shared ECMAScript std::regex predicate for str_regex_match (4240) and
// str_regex_search (4241). Invalid patterns throw std::regex_error, caught to
// match the retail SEH result false.
[[nodiscard]] inline WseDispatch wse_regex_predicate(
    std::u16string_view text, std::u16string_view pattern,
    bool full_match) noexcept {
    try {
        const std::string text_utf8 = wse_utf16_to_utf8(text);
        const std::regex re(wse_utf16_to_utf8(pattern));
        const bool matched = full_match ? std::regex_match(text_utf8, re)
                                        : std::regex_search(text_utf8, re);
        return matched ? WseDispatch::handled_no_store
                       : WseDispatch::condition_false;
    } catch (...) {
        return WseDispatch::condition_false;
    }
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

// Shared implementation for 4232 str_store_skill_name (0x140110530) and 4245
// str_store_skill_desc (0x1402136a0, adding "_desc" before translation lookup).
[[nodiscard]] inline WseDispatch wse_store_skill_text(
    const wse::game::WseWorldState* world,
    WseStringBanks& strings,
    std::int32_t sreg,
    std::int32_t skill_no,
    bool is_desc) {
    if (!wse_register_ok(sreg) || world == nullptr || skill_no < 0 ||
        skill_no >= wse::game::kMaxSkills) {
        return WseDispatch::ignored;
    }
    const auto& skill = world->skills[static_cast<std::size_t>(skill_no)];
    if (!skill.id.empty()) {
        const std::string key = is_desc ? (skill.id + "_desc") : skill.id;
        if (const auto it = world->translations.find(key);
            it != world->translations.end()) {
            return wse_store_dict_string(strings, sreg, it->second);
        }
    }
    return wse_store_dict_string(strings, sreg,
                                 is_desc ? skill.description : skill.name);
}

// Mirrors 0x14024f640 ("sg%d%d.sav" using slot/10 and slot%10) and 4246
// (0x1401efc5c..0x1401efe0d). Clears destination sreg before file open.
[[nodiscard]] inline WseDispatch wse_store_savegame_md5(
    const wse::game::WseWorldState* world,
    WseStringBanks& strings,
    std::int32_t sreg,
    std::int32_t slot_no) {
    if (!wse_register_ok(sreg)) {
        return WseDispatch::ignored;
    }
    if (!wse_store_string(strings, sreg, u"")) {
        return WseDispatch::ignored;
    }
    if (world == nullptr || world->savegames_vfs == nullptr) {
        return WseDispatch::handled_no_store;
    }
    const std::int32_t q = slot_no / 10;
    const std::int32_t r = slot_no - q * 10;
    const std::string filename =
        "sg" + std::to_string(q) + std::to_string(r) + ".sav";
    std::string bytes;
    if (!world->savegames_vfs->read_text(filename, bytes) || bytes.empty()) {
        return WseDispatch::handled_no_store;
    }
    (void)wse_store_string(strings, sreg, wse_md5_hex(bytes));
    return WseDispatch::handled_no_store;
}

[[nodiscard]] inline WseDispatch execute_wse_string(std::uint32_t opcode,
                                                     const WseOperands& operands,
                                                     WseEffect& effect) {
    const auto narrow = operands.narrow;
    const auto tags = operands.tags;
    const auto fp = operands.fp;
    auto strings = operands.state.strings;

    switch (static_cast<WseOpcode>(opcode)) {
    // String group 4200..4211. A failed mbCheckStringRegister returns true
    // without an effect; condition handlers therefore report `ignored`, which
    // the trigger runtime treats as a passing condition.
    case WseOpcode::str_equals: {
        // 4200 (0x1068 equality branch, 0x1401ed33d): compare operands 0 and
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
            wse_is_ascii_alpha);
    }
    case WseOpcode::str_is_digit: {
        // 4205 (0x106d): "C"-locale iswdigit over operand 0, or only operand 1
        // when it is present and not -1.
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
            wse_is_ascii_digit);
    }
    case WseOpcode::str_is_whitespace: {
        // 4206 (0x106e): "C"-locale iswspace over operand 0, or only operand 1
        // when it is present and not -1.
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
            wse_is_space);
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
        // _wcsicmp (0x1401f8870) instead of the normalised compare
        // (0x1401f8830). The result is written through operand 0.
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
        // 4215 (0x1077): MakeLower (0x1401f8cc0) of operand 1 into tag-0
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
        // 4216 (0x1078): MakeUpper (0x1401f8c40) of operand 1 into tag-0
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
        // 4217 (0x1079): operand 2 selects Trim (0x1401f8fa0), TrimLeft
        // (0x1401f8e80), or TrimRight (0x1401f8dc0); other values copy the
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
        // operand 3 (0x1401f8890) and store the result in tag-0 operand 0.
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
    case WseOpcode::str_store_md5: {
        // 4219 (0x107b, 0x1401eead6): UTF-8 MD5 digest formatted as 32
        // lowercase hex digits in tag-0 operand 0.
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
        const auto digest = wse_md5_hex(
            wse_utf16_to_utf8(wse_resolve_string(narrow[1], tag, strings)));
        return wse_store_string(strings, narrow[0], digest)
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
        // tag-0 operand 0 (std::reverse at 0x14049a040).
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
        // 4223 (0x107f): replace u" " (0x14055c410) with u"_"
        // (0x14055c40c) in operand 1 and store the result in operand 0.
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
    case WseOpcode::str_store_float: {
        // 4233 (0x1089, 0x1401ef528): format fp[operand 1] with "%.*f"
        // (0x14055c414) into tag-0 operand 0; precision defaults to 5 when
        // fewer than 3 operands or negative.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) || !wse_register_ok(narrow[1]) ||
            fp.empty()) {
            return WseDispatch::ignored;
        }
        const auto fp_index = static_cast<std::size_t>(narrow[1]);
        if (fp_index >= fp.size()) {
            return WseDispatch::ignored;
        }
        const std::int32_t precision =
            (narrow.size() < 3 || narrow[2] < 0) ? 5 : narrow[2];
        return wse_store_string(strings, narrow[0],
                                wse_format_float(fp[fp_index], precision))
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_sanitize: {
        // 4234 (0x108a, 0x1401ef5b6): strip '{', '}', '<', '>' from tag-0
        // register operand 0 in place.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            static_cast<std::size_t>(narrow[0]) >= strings.registers.size()) {
            return WseDispatch::ignored;
        }
        const auto index = static_cast<std::size_t>(narrow[0]);
        strings.registers[index] = wse_sanitize_string(strings.registers[index]);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::str_is_integer: {
        // 4236 (0x108c, 0x1401ef6df): trim both ends, reject empty or bare
        // '-', and check remaining code units with iswdigit (0x1404995f0).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t tag = wse_tag_at(tags, 0);
        if (!wse_string_register_ok(narrow[0], tag, strings)) {
            return WseDispatch::ignored;
        }
        std::u16string text(wse_resolve_string(narrow[0], tag, strings));
        wse_trim(text);
        if (text.empty()) {
            return WseDispatch::condition_false;
        }
        std::size_t index = text[0] == u'-' ? 1 : 0;
        if (index >= text.size()) {
            return WseDispatch::condition_false;
        }
        for (; index < text.size(); ++index) {
            if (!wse_is_ascii_digit(text[index])) {
                return WseDispatch::condition_false;
            }
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::str_store_server_password_rcon: {
        // 4238 (0x108e -> 0x1401f4b41): retail jump table exits immediately
        // returning true with no side effects.
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::str_regex_match:
    case WseOpcode::str_regex_search: {
        // 4240/4241 (0x1090/0x1091, 0x1401ef8c1/0x1401ef9cd): ECMAScript regex
        // match/search over UTF-8 operand 0 with pattern operand 1.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const std::uint8_t text_tag = wse_tag_at(tags, 0);
        const std::uint8_t pattern_tag = wse_tag_at(tags, 1);
        if (!wse_string_register_ok(narrow[0], text_tag, strings) ||
            !wse_string_register_ok(narrow[1], pattern_tag, strings)) {
            return WseDispatch::ignored;
        }
        const bool full_match =
            static_cast<WseOpcode>(opcode) == WseOpcode::str_regex_match;
        return wse_regex_predicate(
            wse_resolve_string(narrow[0], text_tag, strings),
            wse_resolve_string(narrow[1], pattern_tag, strings), full_match);
    }
    case WseOpcode::str_regex_get_matches: {
        // 4242 (0x1092, 0x1401efad9): write full regex matches of operand 2
        // against operand 1 into tag-0 registers starting at operand 0,
        // stopping after optional operand 3 matches when nonzero or at s127.
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t text_tag = wse_tag_at(tags, 1);
        const std::uint8_t pattern_tag = wse_tag_at(tags, 2);
        if (!wse_string_register_ok(narrow[1], text_tag, strings) ||
            !wse_string_register_ok(narrow[2], pattern_tag, strings)) {
            return WseDispatch::ignored;
        }
        const std::string text_utf8 = wse_utf16_to_utf8(
            wse_resolve_string(narrow[1], text_tag, strings));
        const std::string pattern_utf8 = wse_utf16_to_utf8(
            wse_resolve_string(narrow[2], pattern_tag, strings));
        const std::int32_t max_matches = narrow.size() >= 4 ? narrow[3] : 0;
        std::int32_t dest = narrow[0];
        std::int32_t count = 0;
        try {
            const std::regex re(pattern_utf8);
            std::sregex_iterator it(text_utf8.begin(), text_utf8.end(), re);
            const std::sregex_iterator end;
            for (; it != end; ++it) {
                if ((max_matches != 0 && count == max_matches) || dest >= 0x80 ||
                    static_cast<std::size_t>(dest) >= strings.registers.size()) {
                    break;
                }
                strings.registers[static_cast<std::size_t>(dest)] =
                    wse_utf8_to_utf16(it->str(0));
                ++dest;
                ++count;
            }
        } catch (...) {
            return WseDispatch::handled_no_store;
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::str_store_regex_replace: {
        // 4243 (0x1093, 0x1401efd40): regex_replace(operand 1, operand 2,
        // operand 3) stored in tag-0 operand 0.
        if (narrow.size() < 4) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const std::uint8_t text_tag = wse_tag_at(tags, 1);
        const std::uint8_t pattern_tag = wse_tag_at(tags, 2);
        const std::uint8_t replacement_tag = wse_tag_at(tags, 3);
        if (!wse_string_register_ok(narrow[1], text_tag, strings) ||
            !wse_string_register_ok(narrow[2], pattern_tag, strings) ||
            !wse_string_register_ok(narrow[3], replacement_tag, strings)) {
            return WseDispatch::ignored;
        }
        try {
            const std::string text_utf8 = wse_utf16_to_utf8(
                wse_resolve_string(narrow[1], text_tag, strings));
            const std::regex re(wse_utf16_to_utf8(
                wse_resolve_string(narrow[2], pattern_tag, strings)));
            const std::string replacement_utf8 = wse_utf16_to_utf8(
                wse_resolve_string(narrow[3], replacement_tag, strings));
            const std::string replaced =
                std::regex_replace(text_utf8, re, replacement_utf8);
            return wse_store_string(strings, narrow[0],
                                    wse_utf8_to_utf16(replaced))
                       ? WseDispatch::handled_no_store
                       : WseDispatch::ignored;
        } catch (...) {
            return WseDispatch::handled_no_store;
        }
    }
    case WseOpcode::str_decode_url: {
        // 4244 (0x1094, 0x1401efecd): UrlUnescapeA (0x140311670) on UTF-8
        // operand 1 stored into tag-0 operand 0.
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
        const auto decoded = wse_utf8_to_utf16(wse_url_unescape(
            wse_utf16_to_utf8(wse_resolve_string(narrow[1], tag, strings))));
        return wse_store_string(strings, narrow[0], decoded)
                   ? WseDispatch::handled_no_store
                   : WseDispatch::ignored;
    }
    case WseOpcode::str_store_module_setting: {
        // 4226 (0x1082, 0x1401ef26f / L6003): (sreg, setting, section).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            !wse_string_register_ok(narrow[1], wse_tag_at(tags, 1), strings) ||
            !wse_string_register_ok(narrow[2], wse_tag_at(tags, 2), strings)) {
            return WseDispatch::ignored;
        }
        std::string setting;
        std::string section;
        if (!wse_resolve_parsed_utf8_operand(1, operands, setting) ||
            !wse_resolve_parsed_utf8_operand(2, operands, section)) {
            return WseDispatch::ignored;
        }
        wse_spaces_to_underscores(setting);
        wse_spaces_to_underscores(section);
        std::string_view found{};
        if (const auto* const world = operands.state.world; world != nullptr) {
            if (const auto sec_it = world->module_settings.find(section);
                sec_it != world->module_settings.end()) {
                if (const auto val_it = sec_it->second.find(setting);
                    val_it != sec_it->second.end()) {
                    found = val_it->second;
                }
            }
        }
        return wse_store_dict_string(strings, narrow[0], found);
    }
    case WseOpcode::str_store_game_variable: {
        // 4231 (0x1087, L6075): (sreg, variable) from game_variables.txt.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0]) ||
            !wse_string_register_ok(narrow[1], wse_tag_at(tags, 1), strings)) {
            return WseDispatch::ignored;
        }
        std::string variable;
        if (!wse_resolve_parsed_utf8_operand(1, operands, variable)) {
            return WseDispatch::ignored;
        }
        wse_spaces_to_underscores(variable);
        std::string_view found{};
        if (const auto* const world = operands.state.world; world != nullptr) {
            if (const auto it = world->game_variables.find(variable);
                it != world->game_variables.end()) {
                found = it->second;
            }
        }
        return wse_store_dict_string(strings, narrow[0], found);
    }
    case WseOpcode::str_store_skill_name:
    case WseOpcode::str_store_skill_desc: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_store_skill_text(
            operands.state.world, strings, narrow[0], narrow[1],
            static_cast<WseOpcode>(opcode) == WseOpcode::str_store_skill_desc);
    }
    case WseOpcode::str_store_item_id:
    case WseOpcode::str_store_item_mesh_name: {
        // 4235 (0x108b, 0x1401ef6ae) / 4239 (0x108f, 0x1401ef890).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto* const world = operands.state.world;
        if (!wse_register_ok(narrow[0]) || world == nullptr ||
            !world->is_valid_item(narrow[1])) {
            return WseDispatch::ignored;
        }
        const auto& item = world->items[static_cast<std::size_t>(narrow[1])];
        if (static_cast<WseOpcode>(opcode) == WseOpcode::str_store_item_id) {
            return wse_store_dict_string(strings, narrow[0], item.id);
        }
        const std::string_view mesh_name =
            item.variations.empty() ? std::string_view{}
                                    : std::string_view{item.variations[0].mesh_name};
        return wse_store_dict_string(strings, narrow[0], mesh_name);
    }
    case WseOpcode::str_store_savegame_md5: {
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        return wse_store_savegame_md5(operands.state.world, strings, narrow[0],
                                      narrow[1]);
    }
    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
