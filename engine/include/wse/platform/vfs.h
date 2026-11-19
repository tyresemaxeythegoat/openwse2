// Platform seam: virtual file system.
//
// Port infrastructure, not a reconstruction: nothing here mirrors a retail
// layout, so it carries no v1.1.5.6 citation. The retail engine opens module
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
//   * HostVfs also refuses a path whose symbolic links resolve outside the
//     root.
//
// Ownership: engine state (`WseObjectManager::vfs`,
// `WseWorldState::savegames_vfs`) holds plain `Vfs*` pointers. The embedder
// owns every Vfs and must keep it alive for as long as that state can run
// script. A Vfs is not synchronised; share one between threads only with
// external locking.
//
// No function throws: failures are reported through the return value.

#pragma once

#include <algorithm>
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
#include <unordered_map>
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

    // Optional directory creation/removal (non-pure default so custom mock Vfs
    // subclasses stay valid). Creates/removes a single directory whose parent
    // exists; remove_dir refuses non-empty directories.
    virtual bool make_dir(std::string_view /*path*/) { return false; }
    virtual bool remove_dir(std::string_view /*path*/) { return false; }

    // Optional atomic replace: moves file `from` to `to`, replacing an existing
    // file at `to` in one step (POSIX `rename`). `to`'s parent must exist. The
    // replaced file keeps its own storage, so a reader that still has it open
    // or mapped (a `dlopen`ed driver) is unaffected. False when unsupported.
    virtual bool rename_file(std::string_view /*from*/, std::string_view /*to*/) {
        return false;
    }

    // The size of a file in bytes. False when missing. The default reads the
    // whole file; HostVfs and MemoryVfs answer without reading it (R4 file
    // shims list directories with sizes).
    virtual bool file_size(std::string_view path, std::uint64_t& size) {
        std::vector<std::uint8_t> bytes;
        if (!read_file(path, bytes)) return false;
        size = bytes.size();
        return true;
    }

    // Same as `read_file`, into a string. Implementations override this to
    // fill `out` directly; the default pays one extra copy.
    virtual bool read_text(std::string_view path, std::string& out) {
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

// Creates each missing directory above `file_path` (`a/b/c.json` -> `a`, `a/b`), since
// `write_file` requires the parent to exist on `HostVfs` / `SafVfs`. For savers whose
// file lives in a subdirectory a fresh install does not have yet. False when a
// directory cannot be created (including on a `Vfs` without `make_dir`) or the path is
// invalid.
[[nodiscard]] inline bool make_parent_dirs(Vfs& vfs, std::string_view file_path) {
    const auto parts = split_path(file_path);
    if (!parts) {
        return false;
    }
    std::string dir;
    for (std::size_t i = 0; i + 1U < parts->size(); ++i) {
        if (i != 0U) {
            dir.push_back('/');
        }
        dir += (*parts)[i];
        if (!vfs.is_dir(dir) && !vfs.make_dir(dir)) {
            return false;
        }
    }
    return true;
}

// In-memory tree for tests and for assets unpacked at import time.
// `write_file` creates missing parent directories.
class MemoryVfs final : public Vfs {
public:
    bool file_size(std::string_view path, std::uint64_t& size) override {
        const auto key = normalize_path(path);
        if (!key) return false;
        const auto it = files_.find(*key);
        if (it == files_.end()) return false;
        size = it->second.data.size();
        return true;
    }

    bool read_file(std::string_view path, std::vector<std::uint8_t>& out) override {
        const auto key = normalize_path(path);
        if (!key) return false;
        const auto it = files_.find(*key);
        if (it == files_.end()) return false;
        out = it->second.data;
        return true;
    }

    bool read_text(std::string_view path, std::string& out) override {
        const auto key = normalize_path(path);
        if (!key) return false;
        const auto it = files_.find(*key);
        if (it == files_.end()) return false;
        out.assign(it->second.data.begin(), it->second.data.end());
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

    bool rename_file(std::string_view from, std::string_view to) override {
        const auto from_key = normalize_path(from);
        const auto parts = split_path(to);
        if (!from_key || !parts || parts->empty()) return false;
        const auto src = files_.find(*from_key);
        if (src == files_.end()) return false;
        std::string to_key;
        for (std::size_t i = 0; i < parts->size(); ++i) {
            if (i != 0) to_key.push_back('/');
            to_key += ascii_lower((*parts)[i]);
        }
        const std::size_t slash = to_key.rfind('/');
        if (slash != std::string::npos && dirs_.count(to_key.substr(0, slash)) == 0) {
            return false;  // missing parent
        }
        if (dirs_.count(to_key) != 0) return false;
        if (to_key == *from_key) return true;
        File moved = std::move(src->second);
        files_.erase(src);
        moved.name = parts->back();
        files_.insert_or_assign(std::move(to_key), std::move(moved));
        return true;
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

    bool make_dir(std::string_view path) override {
        const auto parts = split_path(path);
        if (!parts || parts->empty()) return false;
        std::string key;
        for (std::size_t i = 0; i < parts->size(); ++i) {
            if (i != 0) key.push_back('/');
            key += ascii_lower((*parts)[i]);
            if (files_.count(key) != 0) return false;
            if (i + 1 == parts->size()) {
                if (dirs_.count(key) != 0) return false;  // already exists
                dirs_.emplace(key, (*parts)[i]);
                return true;
            }
            if (dirs_.count(key) == 0) return false;  // missing parent
        }
        return false;
    }

    bool remove_dir(std::string_view path) override {
        const auto key = normalize_path(path);
        if (!key || key->empty() || dirs_.count(*key) == 0) return false;
        const std::string prefix = *key + "/";
        for (const auto& [fk, _] : files_) {
            if (fk.compare(0, prefix.size(), prefix) == 0) return false;
        }
        for (const auto& [dk, _] : dirs_) {
            if (dk.compare(0, prefix.size(), prefix) == 0) return false;
        }
        return dirs_.erase(*key) != 0;
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
// first and then case-insensitively against its parent's listing, so a module
// that asks for `Resource\Foo.BRF` finds `resource/foo.brf` on a
// case-sensitive volume. When several entries differ only in case, the one
// whose name sorts first byte-wise wins.
//
// Each parent directory is listed once and its lower-case name index kept, so
// probing many missing names (the texture search chain) does not rescan large
// directories such as `Textures`. The index is rebuilt when the directory's
// last-write time changes and dropped on every write, remove, or make_dir
// through this Vfs. A differently-cased entry added by another process within
// the file system's timestamp resolution may stay unseen until the directory
// changes again; exact-spelling lookups never depend on the index.
//
// `write_file` does not create parent directories below the root, but it does
// create the root itself when that is missing, so a save directory that has
// never been written to works on first use.
//
// The root is made absolute and its symlink-free form is computed once at
// construction; a root that is later re-pointed through a symbolic link is
// refused rather than followed.
class HostVfs final : public Vfs {
public:
    explicit HostVfs(std::filesystem::path root) : root_(std::move(root)) {
        std::error_code ec;
        auto absolute = std::filesystem::absolute(root_, ec);
        if (!ec) root_ = std::move(absolute);
        auto canonical = std::filesystem::weakly_canonical(root_, ec);
        if (!ec) canonical_root_ = std::move(canonical);
    }

    bool read_file(std::string_view path, std::vector<std::uint8_t>& out) override {
        return read_into(path, out);
    }

    bool read_text(std::string_view path, std::string& out) override {
        return read_into(path, out);
    }

    bool write_file(std::string_view path, std::span<const std::uint8_t> data) override {
        std::error_code root_ec;
        std::filesystem::create_directories(root_, root_ec);
        const CacheReset reset{*this};
        const auto real = resolve(path, true);
        if (!real) return false;
        std::FILE* file = open(*real, "wb");
        if (file == nullptr) return false;
        const bool wrote = data.empty() ||
                           std::fwrite(data.data(), 1, data.size(), file) == data.size();
        return (std::fclose(file) == 0) && wrote;
    }

    bool remove_file(std::string_view path) override {
        const CacheReset reset{*this};
        const auto real = resolve(path, false);
        if (!real) return false;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*real, ec) || ec) return false;
        return std::filesystem::remove(*real, ec) && !ec;
    }

    bool rename_file(std::string_view from, std::string_view to) override {
        const CacheReset reset{*this};
        const auto src = resolve(from, false);
        const auto dst = resolve(to, true);
        if (!src || !dst) return false;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*src, ec) || ec) return false;
        // Replaces an existing `dst` atomically (MoveFileExW with
        // MOVEFILE_REPLACE_EXISTING on Windows).
        std::filesystem::rename(*src, *dst, ec);
        return !ec;
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

    bool file_size(std::string_view path, std::uint64_t& size) override {
        const auto real = resolve(path, false);
        if (!real) return false;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(*real, ec) || ec) return false;
        const auto n = std::filesystem::file_size(*real, ec);
        if (ec) return false;
        size = n;
        return true;
    }

    bool is_dir(std::string_view path) override {
        const auto real = resolve(path, false);
        std::error_code ec;
        return real && std::filesystem::is_directory(*real, ec) && !ec;
    }

    bool make_dir(std::string_view path) override {
        std::error_code root_ec;
        std::filesystem::create_directories(root_, root_ec);
        const CacheReset reset{*this};
        const auto real = resolve(path, true);
        if (!real) return false;
        std::error_code ec;
        return std::filesystem::create_directory(*real, ec) && !ec;
    }

    bool remove_dir(std::string_view path) override {
        const CacheReset reset{*this};
        const auto real = resolve(path, false);
        if (!real) return false;
        std::error_code ec;
        if (!std::filesystem::is_directory(*real, ec) || ec) return false;
        return std::filesystem::remove(*real, ec) && !ec;
    }

    // Number of directory listings read for case-insensitive lookups since
    // construction (for tests and profiling).
    std::size_t directory_scans() const noexcept { return directory_scans_; }

private:
    // `Out` is a byte vector or a string; both are filled without a second
    // buffer. `out` is left empty on failure.
    template <typename Out>
    bool read_into(std::string_view path, Out& out) const {
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
        if (!ok) out.clear();
        return ok;
    }

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
            if (auto match = find_case_insensitive(current, part)) {
                current = std::move(*match);
                continue;
            }
            if (for_create && i + 1 == parts->size()) {
                return inside_root(exact) ? std::optional(exact) : std::nullopt;
            }
            return std::nullopt;
        }
        if (!inside_root(current)) return std::nullopt;
        return current;
    }

    struct DirListing {
        std::filesystem::file_time_type mtime{};
        std::filesystem::path dir;  // spelling the listing was read through
        std::vector<std::filesystem::path::string_type> aliases;  // proven-equal spellings
        std::unordered_map<std::string, std::filesystem::path> by_lower;
    };

    // ASCII-folded host path. On a case-insensitive volume (NTFS, default APFS,
    // casefolded ext4/f2fs) `resolve` keeps the caller's spelling of a
    // directory that exists, so `Textures` and `textures` must share one index.
    // On a case-sensitive volume (or a case-sensitive NTFS directory) they can
    // be two directories; `find_case_insensitive` tells them apart by checking
    // that a hit's recorded spelling is the same directory.
    static std::filesystem::path::string_type cache_key(const std::filesystem::path& dir) {
        std::filesystem::path::string_type key = dir.native();
        using Ch = std::filesystem::path::value_type;
        for (auto& ch : key) {
            if (ch >= Ch('A') && ch <= Ch('Z')) {
                ch = static_cast<Ch>(ch - Ch('A') + Ch('a'));
            }
        }
        return key;
    }

    // True when `listing` was built for `dir`: same spelling, a spelling already
    // proven equivalent, or (checked once, then remembered) the same directory.
    bool listing_matches(DirListing& listing, const std::filesystem::path& dir) const {
        const auto& native = dir.native();
        if (listing.dir.native() == native) return true;
        if (std::find(listing.aliases.begin(), listing.aliases.end(), native) !=
            listing.aliases.end()) {
            return true;
        }
        std::error_code ec;
        if (!std::filesystem::equivalent(listing.dir, dir, ec) || ec) return false;
        if (listing.aliases.size() < 8U) listing.aliases.push_back(native);
        return true;
    }

    // Looks `name` up in `dir`'s cached lower-case index, listing `dir` when
    // there is no index or the directory's last-write time moved. A listing
    // that fails part-way is used for this lookup but not cached.
    std::optional<std::filesystem::path> find_case_insensitive(
        const std::filesystem::path& dir, std::string_view name) const {
        const std::string wanted = ascii_lower(name);
        const auto key = cache_key(dir);
        std::error_code time_ec;
        const auto mtime = std::filesystem::last_write_time(dir, time_ec);
        if (!time_ec) {
            const auto cached = dir_cache_.find(key);
            if (cached != dir_cache_.end() && cached->second.mtime == mtime &&
                listing_matches(cached->second, dir)) {
                const auto hit = cached->second.by_lower.find(wanted);
                if (hit == cached->second.by_lower.end()) return std::nullopt;
                return hit->second;
            }
        }
        DirListing listing;
        listing.mtime = mtime;
        listing.dir = dir;
        std::error_code ec;
        std::filesystem::directory_iterator it(dir, ec);
        ++directory_scans_;
        for (const std::filesystem::directory_iterator end; !ec && it != end;
             it.increment(ec)) {
            const std::filesystem::path& entry = it->path();
            auto [pos, inserted] =
                listing.by_lower.try_emplace(ascii_lower(entry.filename().string()), entry);
            if (!inserted && entry.filename().native() < pos->second.filename().native()) {
                pos->second = entry;
            }
        }
        std::optional<std::filesystem::path> result;
        if (const auto hit = listing.by_lower.find(wanted); hit != listing.by_lower.end()) {
            result = hit->second;
        }
        if (!ec && !time_ec) dir_cache_.insert_or_assign(key, std::move(listing));
        return result;
    }

    // Component checks are lexical, so a symbolic link inside the tree could
    // still point outside it. Resolve links on both sides and require the
    // target to stay under the root.
    bool inside_root(const std::filesystem::path& target) const {
        std::error_code ec;
        if (canonical_root_.empty()) return false;
        const std::filesystem::path& root = canonical_root_;
        const auto real = std::filesystem::weakly_canonical(target, ec);
        if (ec) return false;
        auto r = root.begin();
        auto t = real.begin();
        for (; r != root.end(); ++r, ++t) {
            if (r->empty()) break;  // trailing separator on the root
            if (t == real.end() || *t != *r) return false;
        }
        return true;
    }

    std::filesystem::path root_;
    std::filesystem::path canonical_root_;  // empty when it could not be resolved

    // Drops every cached listing when a mutating call returns, after the
    // change it made (resolve() inside the call may have cached the old state).
    struct CacheReset {
        const HostVfs& vfs;
        ~CacheReset() { vfs.dir_cache_.clear(); }
    };

    // Keyed by the ASCII-folded host path; see `cache_key`.
    mutable std::unordered_map<std::filesystem::path::string_type, DirListing> dir_cache_;
    mutable std::size_t directory_scans_ = 0;
};

}  // namespace wse::platform
