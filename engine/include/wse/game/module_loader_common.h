// Clean-room v1.1.5.5 module loader primitives: error diagnostics, token/INI
// helpers, and VFS path utilities. All file I/O routes through wse::platform::Vfs.
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
#include <unordered_map>
#include <utility>
#include <vector>

#include "wse/game/world_campaign_helpers.h"
#include "wse/io/text_stream.h"
#include "wse/platform/vfs.h"
#include "wse/vm/statement.h"

namespace wse::game {

inline constexpr std::int32_t kMaxModuleRecords = 1'000'000;

struct ModuleLoadError {
    bool ok = true;
    std::string file{};
    std::string message{};
    std::size_t record_index = 0;
    std::string record_id{};

    [[nodiscard]] explicit operator bool() const noexcept { return ok; }

    [[nodiscard]] std::string format() const {
        if (ok) {
            return "ok";
        }
        std::string out = file.empty() ? std::string("<module>") : file;
        if (record_index > 0 || !record_id.empty()) {
            out.append(" [record ");
            out.append(std::to_string(record_index));
            if (!record_id.empty()) {
                out.append(" '");
                out.append(record_id);
                out.push_back('\'');
            }
            out.push_back(']');
        }
        out.append(": ");
        out.append(message.empty() ? "load failed" : message);
        return out;
    }
};

[[nodiscard]] inline ModuleLoadError make_load_error(
    std::string_view file,
    std::string_view message,
    std::size_t record_index = 0,
    std::string_view record_id = {}) {
    ModuleLoadError err{};
    err.ok = false;
    err.file = std::string(file);
    err.message = std::string(message);
    err.record_index = record_index;
    err.record_id = std::string(record_id);
    return err;
}

[[nodiscard]] inline ModuleLoadError make_read_error(
    std::string_view file,
    vm::ReadError code,
    std::size_t record_index = 0,
    std::string_view record_id = {}) {
    return make_load_error(file, vm::describe(code), record_index, record_id);
}

inline void underscores_to_spaces(std::string& text) noexcept {
    for (char& ch : text) {
        if (ch == '_') {
            ch = ' ';
        }
    }
}

[[nodiscard]] inline std::string trim_ascii(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && static_cast<unsigned char>(text[first]) <= 0x20U) {
        ++first;
    }
    std::size_t last = text.size();
    while (last > first && static_cast<unsigned char>(text[last - 1]) <= 0x20U) {
        --last;
    }
    return std::string(text.substr(first, last - first));
}

[[nodiscard]] inline std::string vfs_join_path(std::string_view dir,
                                               std::string_view child) {
    if (dir.empty()) {
        return std::string(child);
    }
    std::string out(dir);
    while (!out.empty() && (out.back() == '/' || out.back() == '\\')) {
        out.pop_back();
    }
    while (!child.empty() && (child.front() == '/' || child.front() == '\\')) {
        child.remove_prefix(1);
    }
    if (out.empty()) {
        return std::string(child);
    }
    if (child.empty()) {
        return out;
    }
    out.push_back('/');
    out.append(child);
    return out;
}

// Mirrors rglTextStream::readInt32 (0x1402afce0): accepts signed or unsigned
// 32-bit decimals that wrap modulo 2^32 in retail module text generators.
[[nodiscard]] inline bool read_i32_token(io::TokenReader& reader,
                                         std::int32_t& out) noexcept {
    const std::string_view token = reader.next_token();
    if (token.empty()) {
        return false;
    }
    std::int64_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), parsed);
    if (ec != std::errc{} || ptr != token.data() + token.size()) {
        return false;
    }
    if (parsed < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) ||
        parsed > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    out = static_cast<std::int32_t>(static_cast<std::uint32_t>(parsed));
    return true;
}

[[nodiscard]] inline bool read_u32_token(io::TokenReader& reader,
                                         std::uint32_t& out) noexcept {
    std::int32_t signed_val = 0;
    if (!read_i32_token(reader, signed_val)) {
        return false;
    }
    out = static_cast<std::uint32_t>(signed_val);
    return true;
}

// Mirrors retail rglInputTextFileStream::readUint32 (0x1402afce0) modulo-2^32
// decimal wrap (up to 15 chars), needed for retail color literals such as
// `68451502092` in Native `skins.txt` while rejecting non-numeric garbage.
[[nodiscard]] inline bool read_wrapping_u32_token(io::TokenReader& reader,
                                                  std::uint32_t& out) noexcept {
    const std::string_view token = reader.next_token();
    if (token.empty()) {
        return false;
    }
    std::size_t idx = 0;
    bool neg = false;
    if (token[0] == '-') {
        neg = true;
        idx = 1;
    }
    if (idx >= token.size() || token.size() > 15U) {
        return false;
    }
    std::uint32_t acc = 0;
    for (; idx < token.size(); ++idx) {
        const char ch = token[idx];
        if (ch < '0' || ch > '9') {
            return false;
        }
        acc = static_cast<std::uint32_t>(acc * 10U + static_cast<std::uint32_t>(ch - '0'));
    }
    // Negate in unsigned arithmetic: `-2147483648` must wrap, not overflow int32.
    out = neg ? (0U - acc) : acc;
    return true;
}

[[nodiscard]] inline bool read_count_token(io::TokenReader& reader,
                                           std::int32_t& out,
                                           std::int32_t max_count,
                                           std::string_view file,
                                           std::string_view what,
                                           ModuleLoadError& err) {
    if (!reader.read_i32(out)) {
        err = make_load_error(file, std::string("missing/invalid ") + std::string(what));
        return false;
    }
    if (out < 0 || out > max_count) {
        err = make_load_error(file, std::string(what) + " count out of range");
        return false;
    }
    return true;
}

[[nodiscard]] inline bool skip_header_tokens(io::TokenReader& reader,
                                             std::size_t count,
                                             std::string_view file,
                                             ModuleLoadError& err) {
    if (!reader.skip_tokens(count)) {
        err = make_load_error(file, "truncated file header");
        return false;
    }
    return true;
}

[[nodiscard]] inline bool read_versioned_header(io::TokenReader& reader,
                                                std::int32_t& version,
                                                std::string_view file,
                                                ModuleLoadError& err) {
    if (!reader.skip_tokens(2) || !read_i32_token(reader, version)) {
        err = make_load_error(file, "truncated versioned header");
        return false;
    }
    return true;
}

[[nodiscard]] inline bool read_vfs_text_checked(platform::Vfs& vfs,
                                                std::string_view path,
                                                std::string& out,
                                                ModuleLoadError& err) {
    if (!vfs.read_text(path, out)) {
        err = make_load_error(path, "unable to open file");
        return false;
    }
    return true;
}

[[nodiscard]] inline std::string_view truncate_ini_comments(std::string_view line) noexcept {
    std::size_t cut = line.size();
    const std::size_t slash = line.find("//");
    if (slash != std::string_view::npos) {
        cut = std::min(cut, slash);
    }
    const std::size_t semi = line.find(';');
    if (semi != std::string_view::npos) {
        cut = std::min(cut, semi);
    }
    const std::size_t hash = line.find('#');
    if (hash != std::string_view::npos) {
        cut = std::min(cut, hash);
    }
    return line.substr(0, cut);
}

template <class Callback>
inline void for_each_ini_line(std::string_view text, Callback&& cb) {
    std::size_t pos = 0;
    bool first_line = true;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view raw = text.substr(pos, end - pos);
        std::string trimmed = trim_ascii(raw);
        if (first_line) {
            first_line = false;
            if (trimmed.size() >= 3U &&
                static_cast<unsigned char>(trimmed[0]) == 0xefU &&
                static_cast<unsigned char>(trimmed[1]) == 0xbbU &&
                static_cast<unsigned char>(trimmed[2]) == 0xbfU) {
                trimmed.erase(0, 3U);
                trimmed = trim_ascii(trimmed);
            }
        }
        cb(trimmed);
        if (end == text.size()) {
            break;
        }
        pos = end + 1U;
    }
}

// Mirrors rglSectionlessIni::read (0x1402a8f20): strips UTF-8 BOM on line 1,
// truncates comments at `//`, `;`, `#`, and stores `key = value` pairs.
inline void parse_sectionless_ini(
    std::string_view text,
    std::unordered_map<std::string, std::string>& out) {
    for_each_ini_line(text, [&](std::string_view line) {
        const std::string_view body = truncate_ini_comments(line);
        const std::size_t eq = body.find('=');
        if (eq == std::string_view::npos) {
            return;
        }
        const std::string key = trim_ascii(body.substr(0, eq));
        if (key.empty()) {
            return;
        }
        out[key] = trim_ascii(body.substr(eq + 1U));
    });
}

// Mirrors rglIni::read (0x1402a6d80): supports `[section]` headers and `key = value` lines.
inline void parse_ini_sections(
    std::string_view text,
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>>& out) {
    std::string current_section{};
    for_each_ini_line(text, [&](std::string_view line) {
        // Comments are stripped before the header test so `[section] ; note`
        // still opens the section instead of leaking keys into the previous one.
        const std::string stripped = trim_ascii(truncate_ini_comments(line));
        const std::string_view body = stripped;
        if (body.size() >= 2U && body.front() == '[' && body.back() == ']') {
            current_section = trim_ascii(body.substr(1U, body.size() - 2U));
            return;
        }
        const std::size_t eq = body.find('=');
        if (eq == std::string_view::npos) {
            return;
        }
        const std::string key = trim_ascii(body.substr(0, eq));
        if (key.empty()) {
            return;
        }
        out[current_section][key] = trim_ascii(body.substr(eq + 1U));
    });
}

struct ModuleIniCommand {
    std::string key{};
    std::string value{};
    std::string raw_line{};
};

struct ModuleIniData {
    std::vector<ModuleIniCommand> commands{};
    std::unordered_map<std::string, std::string> settings{};
    std::vector<std::string> load_resources{};
    std::vector<std::string> load_mod_resources{};
};

// Mirrors mbCoreGame::readModuleCommands (0x14008cb40): strips `#` comments,
// trims whitespace, keeps non-empty lines, and records `load_resource` / `load_mod_resource`.
inline void parse_module_ini(std::string_view text, ModuleIniData& out) {
    out.commands.clear();
    out.settings.clear();
    out.load_resources.clear();
    out.load_mod_resources.clear();

    for_each_ini_line(text, [&](std::string_view line) {
        const std::size_t hash = line.find('#');
        const std::string clean = trim_ascii(
            hash == std::string_view::npos ? line : line.substr(0, hash));
        if (clean.empty()) {
            return;
        }
        ModuleIniCommand cmd{};
        cmd.raw_line = clean;
        const std::size_t eq = clean.find('=');
        if (eq != std::string::npos) {
            cmd.key = trim_ascii(std::string_view(clean).substr(0, eq));
            cmd.value = trim_ascii(std::string_view(clean).substr(eq + 1U));
            if (!cmd.key.empty()) {
                out.settings[cmd.key] = cmd.value;
                if (cmd.key == "load_resource" ||
                    cmd.key == "load_resource_nofast") {
                    out.load_resources.push_back(cmd.value);
                } else if (cmd.key == "load_mod_resource" ||
                           cmd.key == "load_module_resource") {
                    out.load_mod_resources.push_back(cmd.value);
                }
            }
        } else {
            cmd.key = clean;
        }
        out.commands.push_back(std::move(cmd));
    });
}

// Mirrors mbTroop::readStream (0x1402455f0) XP seeding via g_levelTable (0x140618b20).
// kCampaignLevelXpTable[level - 1] matches g_levelTable[level] for levels 1..63.
[[nodiscard]] inline std::int32_t initial_troop_xp(
    std::int32_t level,
    std::uint32_t* prng_state = nullptr) noexcept {
    const std::int32_t clamped = std::clamp<std::int32_t>(
        level, 1, static_cast<std::int32_t>(kCampaignLevelXpTable.size() - 1U));
    const std::int32_t base_xp =
        kCampaignLevelXpTable[static_cast<std::size_t>(clamped - 1)];
    if (prng_state == nullptr) {
        return base_xp;
    }
    const std::int32_t next_xp =
        kCampaignLevelXpTable[static_cast<std::size_t>(clamped)];
    *prng_state = (*prng_state * 0x343fdU) + 0x269ec3U;
    const std::uint32_t r = ((*prng_state >> 16U) & 0x7fffU) % 0x3dc9U;
    const float frac = static_cast<float>(r) / 15817.0f;
    return base_xp + static_cast<std::int32_t>(
                         frac * static_cast<float>(next_xp - base_xp));
}

} // namespace wse::game
