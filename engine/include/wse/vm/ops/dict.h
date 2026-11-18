#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>

#include "wse/vm/wse_state.h"

namespace wse::vm {

[[nodiscard]] inline bool wse_dict_load_ini(
    wse::platform::Vfs* vfs,
    std::string_view rel_path,
    std::map<std::string, std::string>& out) {
    std::string data;
    if (!wse_dict_read_file_bytes(vfs, rel_path, data)) {
        return false;
    }
    std::string_view view(data);
    bool first_line = true;
    while (!view.empty()) {
        const std::size_t nl = view.find('\n');
        std::string_view line =
            nl == std::string_view::npos ? view : view.substr(0, nl);
        view = nl == std::string_view::npos ? std::string_view{}
                                            : view.substr(nl + 1);
        if (first_line && line.starts_with("\xEF\xBB\xBF")) {
            line.remove_prefix(3);
        }
        first_line = false;
        line = wse_trim_ascii(line);
        std::size_t comment = line.size();
        for (const std::string_view marker :
             {std::string_view{"//"}, std::string_view{";"},
              std::string_view{"#"}}) {
            const std::size_t hit = line.find(marker);
            if (hit != std::string_view::npos && hit < comment) {
                comment = hit;
            }
        }
        line = line.substr(0, comment);
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string_view key = wse_trim_ascii(line.substr(0, eq));
        const std::string_view val = wse_trim_ascii(line.substr(eq + 1));
        out[std::string(key)] = std::string(val);
    }
    return true;
}

inline void wse_dict_save_ini(
    wse::platform::Vfs* vfs,
    std::string_view rel_path,
    const std::map<std::string, std::string>& entries) {
    std::string text;
    for (const auto& [key, value] : entries) {
        text.append(key);
        text.push_back('=');
        text.append(value);
        text.push_back('\n');
    }
    (void)wse_dict_write_file_bytes(vfs, rel_path, text);
}


// Binary .wsedict format: LE magic 0x57534544, version <= 2, count,
// and count pairs of (int32 klen, int32 vlen, kbytes, vbytes).
inline constexpr std::uint32_t kWseDictBinaryMagic = 0x57534544U;
inline constexpr std::int32_t kWseDictBinaryVersion = 2;

[[nodiscard]] inline bool wse_dict_load_binary(
    wse::platform::Vfs* vfs,
    std::string_view rel_path,
    std::map<std::string, std::string>& out) {
    std::string bytes;
    if (!wse_dict_read_file_bytes(vfs, rel_path, bytes) || bytes.size() < 12) {
        return false;
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data());
    const std::uint32_t magic = wse_read_le32(raw);
    const auto version = static_cast<std::int32_t>(wse_read_le32(raw + 4));
    const auto count = static_cast<std::int32_t>(wse_read_le32(raw + 8));
    if (magic != kWseDictBinaryMagic || version < 0 ||
        version > kWseDictBinaryVersion || count < 0) {
        return false;
    }
    std::size_t offset = 12;
    for (std::int32_t i = 0; i < count; ++i) {
        if (bytes.size() - offset < 8) {
            return false;
        }
        const auto klen =
            static_cast<std::int32_t>(wse_read_le32(raw + offset));
        const auto vlen =
            static_cast<std::int32_t>(wse_read_le32(raw + offset + 4));
        offset += 8;
        if (klen < 0 || vlen < 0) {
            return false;
        }
        const auto uk = static_cast<std::size_t>(klen);
        const auto uv = static_cast<std::size_t>(vlen);
        if (bytes.size() - offset < uk ||
            bytes.size() - offset - uk < uv) {
            return false;
        }
        std::string key(bytes.substr(offset, uk));
        offset += uk;
        std::string value(bytes.substr(offset, uv));
        offset += uv;
        out[std::move(key)] = std::move(value);
    }
    return true;
}

inline void wse_dict_save_binary(
    wse::platform::Vfs* vfs,
    std::string_view rel_path,
    const std::map<std::string, std::string>& entries) {
    std::string out;
    wse_append_le32(out, kWseDictBinaryMagic);
    wse_append_le32(out, static_cast<std::uint32_t>(kWseDictBinaryVersion));
    wse_append_le32(out, static_cast<std::uint32_t>(entries.size()));
    for (const auto& [key, value] : entries) {
        wse_append_le32(out, static_cast<std::uint32_t>(key.size()));
        wse_append_le32(out, static_cast<std::uint32_t>(value.size()));
        out.append(key);
        out.append(value);
    }
    (void)wse_dict_write_file_bytes(vfs, rel_path, out);
}

// Shared loader applying 0x1401e6d98..0x1401e80c8 mode semantics:
// mode == 0 clears then inserts all; mode == 1 overwrites existing;
// any other mode inserts only missing keys.
inline void wse_dict_apply_load(
    WseDictionary& dict,
    const std::map<std::string, std::string>& loaded,
    std::int32_t mode) {
    if (mode == 0) {
        dict.clear();
        dict.merge(loaded, false);
    } else if (mode == 1) {
        dict.merge(loaded, true);
    } else {
        dict.merge(loaded, false);
    }
}

// 0x1402aab90: serializes o.xyz, f.xyz, u.xyz using "%f" separated by ','.
inline void wse_dict_set_transform(WseDictionary& dict,
                                   const std::string& key,
                                   const WseTransform& t) {
    const std::array<float, 9> values = {t.o.x, t.o.y, t.o.z, t.f.x, t.f.y,
                                         t.f.z, t.u.x, t.u.y, t.u.z};
    std::string encoded;
    std::array<char, 64> buf{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            encoded.push_back(',');
        }
        const int written =
            std::snprintf(buf.data(), buf.size(), "%f",
                          static_cast<double>(values[i]));
        if (written > 0) {
            encoded.append(buf.data(), static_cast<std::size_t>(written));
        }
    }
    dict.set(key, std::move(encoded));
}

// 0x1402aa810: splits on ',' preserving empty tokens, parses 9 floats into
// (o, f, u) and orthonormalizes, or returns `fallback` when token count != 9.
[[nodiscard]] inline WseTransform wse_dict_get_transform(
    const WseDictionary& dict,
    const std::string& key,
    const WseTransform& fallback) {
    const auto it = dict.entries.find(key);
    if (it == dict.entries.end()) {
        return fallback;
    }
    std::vector<std::string_view> tokens;
    const std::string_view raw = it->second;
    std::size_t start = 0;
    for (;;) {
        const std::size_t comma = raw.find(',', start);
        if (comma == std::string_view::npos) {
            tokens.push_back(raw.substr(start));
            break;
        }
        tokens.push_back(raw.substr(start, comma - start));
        start = comma + 1;
    }
    if (tokens.size() != 9) {
        return fallback;
    }
    std::array<float, 9> parsed{};
    for (std::size_t i = 0; i < 9; ++i) {
        std::string tok(tokens[i]);
        parsed[i] = static_cast<float>(std::strtod(tok.c_str(), nullptr));
    }
    WseTransform out{};
    out.o = {parsed[0], parsed[1], parsed[2], 1.0f};
    out.f = {parsed[3], parsed[4], parsed[5], 0.0f};
    out.u = {parsed[6], parsed[7], parsed[8], 0.0f};
    out.orthonormalize();
    return out;
}


[[nodiscard]] inline WseDispatch execute_wse_dict(std::uint32_t opcode,
                                                     const WseOperands& operands,
                                                     WseEffect& effect) {
    const auto narrow = operands.narrow;
    auto strings = operands.state.strings;

    switch (static_cast<WseOpcode>(opcode)) {
    // Dictionary group (3200..3222 / 0xc80..0xc96).
    case WseOpcode::dict_create: {
        // 3200 (0xc80, 0x1401e6cb0): allocates handle in WSEObjectManager.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.dictionaries == nullptr) {
            return WseDispatch::ignored;
        }
        effect.value =
            static_cast<std::int64_t>(operands.state.dictionaries->create());
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::dict_free: {
        // 3201 (0xc81, 0x1401e6cec): destroys dictionary and frees ID.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.dictionaries != nullptr) {
            operands.state.dictionaries->destroy(narrow[0]);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_load_file: {
        // 3202 (0xc82): (dict, file_str, [mode], [format]); format 1 = .ini,
        // otherwise binary .wsedict.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        const std::int32_t format = narrow.size() >= 4 ? narrow[3] : 0;
        std::string target;
        if (!dict ||
            !wse_dict_target_path(operands.state.dictionaries, file_name,
                                  format == 1 ? ".ini" : ".wsedict", target)) {
            return WseDispatch::handled_no_store;
        }
        wse::platform::Vfs* const vfs = operands.state.dictionaries->vfs;
        const std::int32_t mode = narrow.size() >= 3 ? narrow[2] : 0;
        if (mode == 0) {
            dict->clear();
        }
        std::map<std::string, std::string> loaded;
        if (format == 1) {
            (void)wse_dict_load_ini(vfs, target, loaded);
        } else {
            (void)wse_dict_load_binary(vfs, target, loaded);
        }
        wse_dict_apply_load(*dict, loaded, mode);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_load_dict: {
        // 3203 (0xc83): (dst, src, [mode]); mode 0 is no-op, 1 overwrites,
        // 2 inserts missing keys.
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (operands.state.dictionaries == nullptr) {
            return WseDispatch::ignored;
        }
        WseDictionary* dst = operands.state.dictionaries->get(narrow[0]);
        const WseDictionary* src = operands.state.dictionaries->get(narrow[1]);
        const std::int32_t mode = narrow.size() >= 3 ? narrow[2] : 0;
        if (!dst || !src || mode == 0 || dst == src) {
            return WseDispatch::handled_no_store;
        }
        if (mode == 1 || mode == 2) {
            dst->merge(src->entries, mode == 1);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_save: {
        // 3204 (0xc84): (dict, file_str, [format]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[0])
                : nullptr;
        const std::int32_t format = narrow.size() >= 3 ? narrow[2] : 0;
        std::string target;
        if (!dict ||
            !wse_dict_target_path(operands.state.dictionaries, file_name,
                                  format == 1 ? ".ini" : ".wsedict", target)) {
            return WseDispatch::handled_no_store;
        }
        wse::platform::Vfs* const vfs = operands.state.dictionaries->vfs;
        if (format == 1) {
            wse_dict_save_ini(vfs, target, dict->entries);
        } else {
            wse_dict_save_binary(vfs, target, dict->entries);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_clear: {
        // 3205 (0xc85).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (dict) {
            dict->clear();
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_is_empty: {
        // 3206 (0xc86): invalid dict returns true matching executeWSE.
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[0])
                : nullptr;
        if (!dict || dict->empty()) {
            return WseDispatch::handled_no_store;
        }
        return WseDispatch::condition_false;
    }
    case WseOpcode::dict_has_key: {
        // 3207 (0xc87).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(1, operands, key)) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[0])
                : nullptr;
        if (!dict) {
            return WseDispatch::condition_false;
        }
        return dict->has_key(key) ? WseDispatch::handled_no_store
                                  : WseDispatch::condition_false;
    }
    case WseOpcode::dict_get_size: {
        // 3208 (0xc88): (dest, dict).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        effect.value = dict ? static_cast<std::int64_t>(dict->size()) : 0;
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::dict_delete_file: {
        // 3209 (0xc89): (file_str, [format]).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(0, operands, file_name)) {
            return WseDispatch::ignored;
        }
        const std::int32_t format = narrow.size() >= 2 ? narrow[1] : 0;
        std::string target;
        if (wse_dict_target_path(operands.state.dictionaries, file_name,
                                 format == 1 ? ".ini" : ".wsedict", target)) {
            (void)operands.state.dictionaries->vfs->remove_file(target);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_get_str: {
        // 3210 (0xc8a): (sreg, dict, key_str, [default_str]).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(2, operands, key)) {
            return WseDispatch::ignored;
        }
        std::string def_value;
        if (narrow.size() >= 4 &&
            !wse_resolve_utf8_operand(3, operands, def_value)) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        if (!dict) {
            return WseDispatch::handled_no_store;
        }
        const auto it = dict->entries.find(key);
        return wse_store_dict_string(
            strings, narrow[0],
            it != dict->entries.end() ? std::string_view(it->second)
                                      : std::string_view(def_value));
    }
    case WseOpcode::dict_get_int: {
        // 3211 (0xc8b): (dest, dict, key_str, [default_int]).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(2, operands, key)) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        std::int32_t value = 0;
        if (dict) {
            const auto it = dict->entries.find(key);
            if (it != dict->entries.end()) {
                value = static_cast<std::int32_t>(
                    std::strtol(it->second.c_str(), nullptr, 10));
            } else {
                value = narrow.size() >= 4 ? narrow[3] : 0;
            }
        }
        effect.value = static_cast<std::int64_t>(value);
        effect.store_value = true;
        return WseDispatch::handled;
    }
    case WseOpcode::dict_set_str: {
        // 3212 (0xc8c): (dict, key_str, val_str).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        std::string value;
        if (!wse_resolve_utf8_operand(1, operands, key) ||
            !wse_resolve_utf8_operand(2, operands, value)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (dict) {
            dict->set(key, std::move(value));
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_set_int: {
        // 3213 (0xc8d): (dict, key_str, int_val).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(1, operands, key)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (dict) {
            dict->set(key, std::to_string(narrow[2]));
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_get_key_by_iterator: {
        // 3214 (0xc8e, 0x1402ab090): (sreg, dict, [index]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        if (!dict) {
            return WseDispatch::handled_no_store;
        }
        const std::int32_t steps = narrow.size() >= 3 ? narrow[2] : 0;
        auto it = dict->entries.begin();
        for (std::int32_t i = 0; i < steps && it != dict->entries.end(); ++i) {
            ++it;
        }
        return wse_store_dict_string(
            strings, narrow[0],
            it != dict->entries.end() ? std::string_view(it->first)
                                      : std::string_view{});
    }
    case WseOpcode::dict_get_pos: {
        // 3215 (0xc8f, 0x1402aa810): (pos_dest, dict, key_str, [default_pos]).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[0]) || positions.empty() ||
            static_cast<std::size_t>(narrow[0]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(2, operands, key)) {
            return WseDispatch::ignored;
        }
        WseTransform fallback = WseTransform::identity();
        if (narrow.size() >= 4) {
            if (!wse_register_ok(narrow[3]) ||
                static_cast<std::size_t>(narrow[3]) >= positions.size()) {
                return WseDispatch::ignored;
            }
            fallback = positions[static_cast<std::size_t>(narrow[3])];
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        if (!dict) {
            return WseDispatch::handled_no_store;
        }
        positions[static_cast<std::size_t>(narrow[0])] =
            wse_dict_get_transform(*dict, key, fallback);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_set_pos: {
        // 3216 (0xc90, 0x1402aab90): (dict, key_str, pos_no).
        if (narrow.size() < 3) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(1, operands, key)) {
            return WseDispatch::ignored;
        }
        const auto positions = operands.state.positions;
        if (!wse_register_ok(narrow[2]) || positions.empty() ||
            static_cast<std::size_t>(narrow[2]) >= positions.size()) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (dict) {
            wse_dict_set_transform(
                *dict, key, positions[static_cast<std::size_t>(narrow[2])]);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_load_file_json: {
        // 3217 (0xc91, 0x1402a9a60): (dict, file_str, [mode]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        std::string target;
        if (!dict ||
            !wse_dict_target_path(operands.state.dictionaries, file_name,
                                  ".json", target)) {
            return WseDispatch::handled_no_store;
        }
        const std::int32_t mode = narrow.size() >= 3 ? narrow[2] : 0;
        if (mode == 0) {
            dict->clear();
        }
        std::string raw_json;
        std::map<std::string, std::string> loaded;
        if (wse_dict_read_file_bytes(operands.state.dictionaries->vfs, target,
                                     raw_json)) {
            (void)wse_parse_json_object(raw_json, loaded);
        }
        wse_dict_apply_load(*dict, loaded, mode);
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_save_json: {
        // 3218 (0xc92, 0x1402aa1e0): (dict, file_str).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(1, operands, file_name)) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[0])
                : nullptr;
        std::string target;
        if (dict && wse_dict_target_path(operands.state.dictionaries, file_name,
                                         ".json", target)) {
            (void)wse_dict_write_file_bytes(
                operands.state.dictionaries->vfs, target,
                wse_serialize_json_object(dict->entries, true));
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_from_url_encoded_json: {
        // 3219 (0xc93, 0x1402ab110): (dict, encoded_str, [mode]).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string encoded;
        if (!wse_resolve_utf8_operand(1, operands, encoded)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (!dict) {
            return WseDispatch::handled_no_store;
        }
        const std::int32_t mode = narrow.size() >= 3 ? narrow[2] : 0;
        if (mode == 0) {
            dict->clear();
        }
        for (char& ch : encoded) {
            if (ch == '+') {
                ch = ' ';
            }
        }
        const std::string json_text = wse_url_unescape(encoded);
        std::map<std::string, std::string> loaded;
        if (wse_parse_json_object(json_text, loaded)) {
            wse_dict_apply_load(*dict, loaded, mode);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_to_url_encoded_json: {
        // 3220 (0xc94, 0x1402ab5e0): (sreg, dict).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        if (!wse_register_ok(narrow[0])) {
            return WseDispatch::ignored;
        }
        const WseDictionary* dict =
            operands.state.dictionaries != nullptr
                ? operands.state.dictionaries->get(narrow[1])
                : nullptr;
        if (!dict) {
            return WseDispatch::handled_no_store;
        }
        const std::string compact =
            wse_serialize_json_object(dict->entries, false);
        return wse_store_dict_string(strings, narrow[0],
                                     wse_url_escape(compact));
    }
    case WseOpcode::dict_erase: {
        // 3221 (0xc95, 0x1402abb70): (dict, key_str).
        if (narrow.size() < 2) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string key;
        if (!wse_resolve_utf8_operand(1, operands, key)) {
            return WseDispatch::ignored;
        }
        WseDictionary* dict = operands.state.dictionaries != nullptr
                                  ? operands.state.dictionaries->get(narrow[0])
                                  : nullptr;
        if (dict) {
            dict->entries.erase(key);
        }
        return WseDispatch::handled_no_store;
    }
    case WseOpcode::dict_delete_file_json: {
        // 3222 (0xc96): (file_str).
        if (narrow.size() < 1) {
            return WseDispatch::operand_count_out_of_range;
        }
        std::string file_name;
        if (!wse_resolve_utf8_operand(0, operands, file_name)) {
            return WseDispatch::ignored;
        }
        std::string target;
        if (wse_dict_target_path(operands.state.dictionaries, file_name,
                                 ".json", target)) {
            (void)operands.state.dictionaries->vfs->remove_file(target);
        }
        return WseDispatch::handled_no_store;
    }
    default:
        return WseDispatch::unimplemented;
    }
}

}  // namespace wse::vm
