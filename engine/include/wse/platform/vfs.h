// Platform seam: virtual file system.
//
// Port infrastructure, not a reconstruction: nothing here mirrors a retail
// layout, so it carries no v1.1.5.5 citation. The retail engine opens module
// files through Win32 paths (`rglInputTextFileStream`, KERNEL32 imports, see
// docs/ARCHITECTURE.md section 6); every portable loader goes through `Vfs`
// instead so the same code reads a host directory, an in-memory fixture, or an
// Android Storage Access Framework tree (docs/PLATFORM_PLAN.md, slice P2).
//
// Path rules, shared by every implementation:
//   * `/` and `\` both separate components (module files use Windows paths).
//   * Lookups are ASCII case-insensitive (modules rely on Windows semantics).
//   * Paths are relative to the Vfs root; `.` is dropped, `..` may not climb
//     above the root, and drive letters / empty paths are rejected.
//
// No function throws: failures are reported through the return value.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace wse::platform {

struct VfsEntry {
    std::string name;         // original spelling of the final component
    bool is_directory = false;
};

constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = ascii_lower(c);
    return out;
}

// Splits `path` into components under the rules above. Returns nullopt for a
// path that escapes the root or names a drive/stream (`:`) or contains NUL.
// The root itself ("", ".", "/") yields an empty component list.
inline std::optional<std::vector<std::string>> split_path(std::string_view path) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= path.size()) {
        std::size_t end = begin;
        while (end < path.size() && path[end] != '/' && path[end] != '\\') ++end;
        const std::string_view part = path.substr(begin, end - begin);
        if (part.find(':') != std::string_view::npos ||
            part.find('\0') != std::string_view::npos) {
            return std::nullopt;
        }
        if (part == "..") {
            if (parts.empty()) return std::nullopt;
            parts.pop_back();
        } else if (!part.empty() && part != ".") {
            parts.emplace_back(part);
        }
        begin = end + 1;
    }
    return parts;
}

// Canonical lookup key: lower-case components joined by '/'.
inline std::optional<std::string> normalize_path(std::string_view path) {
    auto parts = split_path(path);
    if (!parts) return std::nullopt;
    std::string key;
    for (const std::string& part : *parts) {
        if (!key.empty()) key.push_back('/');
        key += ascii_lower(part);
    }
    return key;
}

class Vfs {
public:
    virtual ~Vfs() = default;

    // Replaces `out` with the whole file. False when missing or unreadable.
    virtual bool read_file(std::string_view path, std::vector<std::uint8_t>& out) = 0;

    // Creates or truncates the file; parent directories must already exist
    // except where an implementation documents otherwise.
    virtual bool write_file(std::string_view path, std::span<const std::uint8_t> data) = 0;

    virtual bool remove_file(std::string_view path) = 0;

    // Replaces `out` with the directory's entries sorted by lower-case name.
    virtual bool list_dir(std::string_view path, std::vector<VfsEntry>& out) = 0;

    virtual bool is_file(std::string_view path) = 0;
    virtual bool is_dir(std::string_view path) = 0;

    bool read_text(std::string_view path, std::string& out) {
        std::vector<std::uint8_t> bytes;
        if (!read_file(path, bytes)) return false;
        out.assign(bytes.begin(), bytes.end());
        return true;
    }

    bool write_text(std::string_view path, std::string_view text) {
        return write_file(path, std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }
};

// In-memory tree for tests and for assets unpacked at import time.
// `write_file` creates missing parent directories.
class MemoryVfs final : public Vfs {
public:
    bool read_file(std::string_view path, std::vector<std::uint8_t>& out) override {
        const auto key = normalize_path(path);
        if (!key) return false;
        const auto it = files_.find(*key);
        if (it == files_.end()) return false;
        out = it->second.data;
        return true;
    }

    bool write_file(std::string_view path, std::span<const std::uint8_t> data) override {
        const auto parts = split_path(path);
        if (!parts || parts->empty()) return false;
        std::string key;
        for (std::size_t i = 0; i < parts->size(); ++i) {
            if (i != 0) key.push_back('/');
            key += ascii_lower((*parts)[i]);
            if (i + 1 == parts->size()) break;
            if (files_.count(key) != 0) return false;  // a file is in the way
            dirs_.emplace(key, (*parts)[i]);
        }
        if (dirs_.count(key) != 0) return false;
        File& file = files_[key];
        if (file.name.empty()) file.name = parts->back();
        file.data.assign(data.begin(), data.end());
        return true;
    }

    bool remove_file(std::string_view path) override {
        const auto key = normalize_path(path);
        return key && files_.erase(*key) != 0;
    }

    bool list_dir(std::string_view path, std::vector<VfsEntry>& out) override {
        const auto key = normalize_path(path);
        if (!key || (!key->empty() && dirs_.count(*key) == 0)) return false;
        out.clear();
        const std::string prefix = key->empty() ? std::string() : *key + "/";
        std::map<std::string, VfsEntry> sorted;
        const auto direct_child = [&](const std::string& candidate) {
            return candidate.size() > prefix.size() &&
                   candidate.compare(0, prefix.size(), prefix) == 0 &&
                   candidate.find('/', prefix.size()) == std::string::npos;
        };
        for (const auto& [name, original] : dirs_) {
            if (direct_child(name)) sorted[name.substr(prefix.size())] = {original, true};
        }
        for (const auto& [name, file] : files_) {
            if (direct_child(name)) sorted[name.substr(prefix.size())] = {file.name, false};
        }
        for (auto& [name, entry] : sorted) out.push_back(std::move(entry));
        return true;
    }

    bool is_file(std::string_view path) override {
        const auto key = normalize_path(path);
        return key && files_.count(*key) != 0;
    }

    bool is_dir(std::string_view path) override {
        const auto key = normalize_path(path);
        return key && (key->empty() || dirs_.count(*key) != 0);
    }

private:
    struct File {
        std::string name;
        std::vector<std::uint8_t> data;
    };
    std::map<std::string, File> files_;
    std::map<std::string, std::string> dirs_;  // key -> original final component
};

// A directory on the host file system. Each component is matched exactly
// first and then case-insensitively by scanning its parent, so a module that
// asks for `Resource\Foo.BRF` finds `resource/foo.brf` on a case-sensitive
// volume. `write_file` does not create parent directories.
class HostVfs final : public Vfs {
public:
    explicit HostVfs(std::filesystem::path root) : root_(std::move(root)) {}

    bool read_file(std::string_view path, std::vector<std::uint8_t>& out) override {
        const auto real = resolve(path, false);
        if (!real) return false;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*real, ec) || ec) return false;
        std::FILE* file = open(*real, "rb");
        if (file == nullptr) return false;
        out.clear();
        std::uint8_t chunk[16384];
        std::size_t got = 0;
        while ((got = std::fread(chunk, 1, sizeof chunk, file)) != 0) {
            out.insert(out.end(), chunk, chunk + got);
        }
        const bool ok = std::ferror(file) == 0;
        std::fclose(file);
        return ok;
    }

    bool write_file(std::string_view path, std::span<const std::uint8_t> data) override {
        const auto real = resolve(path, true);
        if (!real) return false;
        std::FILE* file = open(*real, "wb");
        if (file == nullptr) return false;
        const bool wrote = data.empty() ||
                           std::fwrite(data.data(), 1, data.size(), file) == data.size();
        return (std::fclose(file) == 0) && wrote;
    }

    bool remove_file(std::string_view path) override {
        const auto real = resolve(path, false);
        if (!real) return false;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*real, ec) || ec) return false;
        return std::filesystem::remove(*real, ec) && !ec;
    }

    bool list_dir(std::string_view path, std::vector<VfsEntry>& out) override {
        const auto real = resolve(path, false);
        if (!real) return false;
        std::error_code ec;
        std::filesystem::directory_iterator it(*real, ec);
        if (ec) return false;
        std::multimap<std::string, VfsEntry> sorted;
        for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
            if (ec) return false;
            std::error_code type_ec;
            VfsEntry entry{it->path().filename().string(), it->is_directory(type_ec)};
            std::string key = ascii_lower(entry.name);
            sorted.emplace(std::move(key), std::move(entry));
        }
        out.clear();
        for (auto& [name, entry] : sorted) out.push_back(std::move(entry));
        return true;
    }

    bool is_file(std::string_view path) override {
        const auto real = resolve(path, false);
        std::error_code ec;
        return real && std::filesystem::is_regular_file(*real, ec) && !ec;
    }

    bool is_dir(std::string_view path) override {
        const auto real = resolve(path, false);
        std::error_code ec;
        return real && std::filesystem::is_directory(*real, ec) && !ec;
    }

private:
    static std::FILE* open(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
        const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
        return ::_wfopen(path.c_str(), wide_mode.c_str());
#else
        return std::fopen(path.c_str(), mode);
#endif
    }

    // Maps a Vfs path to a host path. With `for_create`, the final component
    // may be absent (its spelling is then taken from the request).
    std::optional<std::filesystem::path> resolve(std::string_view path, bool for_create) const {
        const auto parts = split_path(path);
        if (!parts) return std::nullopt;
        std::filesystem::path current = root_;
        for (std::size_t i = 0; i < parts->size(); ++i) {
            const std::string& part = (*parts)[i];
            std::error_code ec;
            std::filesystem::path exact = current / part;
            if (std::filesystem::exists(exact, ec) && !ec) {
                current = std::move(exact);
                continue;
            }
            const std::string wanted = ascii_lower(part);
            bool found = false;
            std::filesystem::directory_iterator it(current, ec);
            for (const std::filesystem::directory_iterator end; !ec && it != end;
                 it.increment(ec)) {
                if (ascii_lower(it->path().filename().string()) == wanted) {
                    current = it->path();
                    found = true;
                    break;
                }
            }
            if (found) continue;
            if (for_create && i + 1 == parts->size()) return exact;
            return std::nullopt;
        }
        return current;
    }

    std::filesystem::path root_;
};

}  // namespace wse::platform
