// Win64 files and directories for translated code (Phase 5 R4, `docs/RECOMP_PLAN.md` "R4 design",
// family "Files and directories"; v1.1.5.6 call sites: FindFirstFileA 46, FindClose 41,
// CreateDirectoryA 22, ReadFile/WriteFile 16, DeleteFileA 7, GetFullPathNameA 6, the rest 1-4):
//
//   CreateFileA/W ReadFile WriteFile SetFilePointer(Ex) SetEndOfFile FlushFileBuffers GetFileSize(Ex)
//   GetFileType FindFirstFileA/W FindFirstFileExA/W FindNextFileA/W FindClose CreateDirectoryA/W
//   RemoveDirectoryA/W DeleteFileA/W CopyFileA/W MoveFileExA/W GetFileAttributesA/W
//   GetFileAttributesExA/W GetFullPathNameA/W GetCurrentDirectoryA/W SetCurrentDirectoryA/W
//   GetTempPathA/W GetDiskFreeSpaceA/W LockFile(Ex) UnlockFile(Ex) CreateFileMappingA/W
//   OpenFileMappingA/W MapViewOfFile UnmapViewOfFile FlushViewOfFile GetStdHandle SetStdHandle
//   GetConsoleMode WriteConsoleW ReadConsoleW SetConsoleTitleA SetConsoleCtrlHandler
//   SHGetFolderPathA/W (SHELL32)
//
// Decided with the user: one `Vfs`, two backends. Guest paths are Win32 paths, made absolute
// against the current directory (`C:\WSE2` at start) and matched against mounts:
//
//   C:\WSE2                  the install: reads from `install`, writes to `user` under install/
//                            (an overlay: the user's files win, deletions are remembered)
//   C:\Users\Player          the writable layer `user` (Documents, AppData\Roaming, AppData\Local,
//                            AppData\Local\Temp)
//   C:\ProgramData           `user` under ProgramData/
//
// Ancestors of mounts (`C:\`, `C:\Users`) are directories that cannot be written. Lookups are
// case-insensitive (`Vfs` rules); trailing dots and spaces in a component are dropped, as Win32
// does. An open file is a kernel object (`kernel.h`) holding the file's bytes: read when it is
// opened, written back when it was changed and its last handle closes or it is flushed (two
// handles on one file do not see each other's unflushed writes). File mappings copy the file into
// memory; views write back on flush and unmap. The process has no console unless configured: the
// standard handles are null, as for the GUI exe. Header-only.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/platform/vfs.h"
#include "wse/recomp/cpu_state.h"
#include "wse/recomp/kernel.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/shim_crt.h"
#include "wse/recomp/shim_strings.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::shim {

inline constexpr std::uint32_t kErrorFileNotFound = 2, kErrorPathNotFound = 3, kErrorAccessDenied = 5,
                               kErrorNoMoreFiles = 18, kErrorHandleEof = 38, kErrorFileExists = 80,
                               kErrorInvalidName = 123, kErrorNegativeSeek = 131, kErrorDirNotEmpty = 145,
                               kErrorAlreadyExistsFiles = 183;
inline constexpr std::uint32_t kGenericRead = 0x80000000U, kGenericWrite = 0x40000000U;
inline constexpr std::uint32_t kCreateNew = 1, kCreateAlways = 2, kOpenExisting = 3, kOpenAlways = 4,
                               kTruncateExisting = 5;
inline constexpr std::uint32_t kFileAttributeDirectory = 0x10, kFileAttributeArchive = 0x20,
                               kFileAttributeReadonly = 0x1;
inline constexpr std::uint32_t kInvalidFileAttributes = 0xFFFFFFFFU;
inline constexpr std::uint64_t kInvalidHandleValue = ~std::uint64_t{0};
// Every file reports this time (2020-01-01 00:00 UTC): `Vfs` has no timestamps.
inline constexpr std::uint64_t kFixedFileTime = 132223104000000000ULL;

struct FilesConfig {
    platform::Vfs* install = nullptr;  // the user's Warband install (read)
    platform::Vfs* user = nullptr;     // the writable layer
    std::string install_dir = "C:\\WSE2";
    std::string user_profile = "C:\\Users\\Player";
    bool console = false;  // give the process standard handles that write to `on_console`
};

namespace files_detail {
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
inline char16_t upper(char16_t c) { return str_detail::to_upper(c); }
inline std::string upper_utf8(const std::string& s) {
    std::u16string w = crt_detail::utf16(s);
    for (char16_t& c : w) c = upper(c);
    return crt_detail::utf8(w);
}
// An ANSI (code page 1252) guest string as UTF-8, and back.
inline std::string ansi_to_utf8(std::uint64_t va) {
    std::u16string w;
    for (std::size_t i = 0; va != 0 && i < 32768; ++i) {
        const auto b = get<std::uint8_t>(va + i);
        if (b == 0) break;
        w.push_back(str_detail::byte_to_unit(kAnsiCodePage, b));
    }
    return crt_detail::utf8(w);
}
inline std::string wide_to_utf8(std::uint64_t va) { return crt_detail::utf8(crt_detail::guest_wstring(va)); }
inline std::string utf8_to_ansi(const std::string& s) {
    std::string out;
    for (const char16_t c : crt_detail::utf16(s)) {
        const int b = str_detail::unit_to_byte(kAnsiCodePage, c);
        out.push_back(b < 0 ? '?' : static_cast<char>(b));
    }
    return out;
}
// Win32 wildcard match ('*', '?'; case-insensitive; "*.*" matches every name, a trailing '.'
// matches names without an extension).
inline bool wildcard(const std::u16string& pat, std::size_t pi, const std::u16string& name, std::size_t ni) {
    while (pi < pat.size()) {
        const char16_t p = pat[pi];
        if (p == u'*') {
            for (std::size_t k = ni; k <= name.size(); ++k) {
                if (wildcard(pat, pi + 1, name, k)) return true;
            }
            return false;
        }
        if (ni >= name.size()) {
            // Trailing '.' (or '*' / '.' runs) matches the end of a name without an extension.
            return std::all_of(pat.begin() + static_cast<std::ptrdiff_t>(pi), pat.end(),
                               [](char16_t c) { return c == u'.' || c == u'*'; });
        }
        if (p != u'?' && upper(p) != upper(name[ni])) return false;
        ++pi;
        ++ni;
    }
    return ni == name.size();
}
inline bool matches(std::string pattern, const std::string& name) {
    if (pattern == "*.*") pattern = "*";
    const std::u16string p = crt_detail::utf16(pattern);
    const std::u16string n = crt_detail::utf16(name);
    if (!p.empty() && p.back() == u'.' && n.find(u'.') != std::u16string::npos && p.find(u'*') != std::u16string::npos &&
        p != u"*.") {
        return wildcard(p, 0, n, 0);
    }
    if (p == u"*.") return n.find(u'.') == std::u16string::npos;
    return wildcard(p, 0, n, 0);
}
}  // namespace files_detail

class Files {
public:
    Files(Runtime& rt, kernel::Kernel& kernel, FilesConfig config) : rt_(rt), kernel_(kernel), cfg_(std::move(config)) {
        add_mount(cfg_.install_dir, cfg_.install, "", cfg_.user, "install");
        add_mount(cfg_.user_profile, nullptr, "", cfg_.user, "");
        add_mount("C:\\ProgramData", nullptr, "", cfg_.user, "ProgramData");
        cwd_ = canonical(cfg_.install_dir).value_or("C:\\");
        if (cfg_.console) {
            for (int i = 0; i < 3; ++i) {
                auto f = std::make_shared<OpenFile>();
                f->owner = this;
                f->console = true;
                f->readable = i == 0;
                f->writable = i != 0;
                std_handles_[i] = file_handle(std::move(f));
            }
        }
    }
    Files(const Files&) = delete;
    Files& operator=(const Files&) = delete;
    ~Files() {
        std::lock_guard<std::mutex> lock(opened_m_);
        for (const std::weak_ptr<OpenFile>& w : opened_) {
            if (std::shared_ptr<OpenFile> f = w.lock()) {
                std::lock_guard<std::mutex> fl(f->m);
                flush_locked(*f);
                f->owner = nullptr;  // a handle that outlives this object no longer writes back
            }
        }
    }

    // Text written to the standard handles when `console` is set (UTF-8).
    std::function<void(const std::string&)> on_console;

    // `path` (Win32, UTF-8) made absolute and canonical (`C:\a\b`), or nothing when invalid.
    [[nodiscard]] std::optional<std::string> canonical(const std::string& path) const {
        std::string p = path;
        if (p.rfind("\\\\?\\", 0) == 0) p = p.substr(4);
        for (char& c : p) {
            if (c == '/') c = '\\';
        }
        std::string abs;
        if (p.size() >= 2 && p[1] == ':') {
            abs = std::string(1, static_cast<char>(p[0] >= 'a' && p[0] <= 'z' ? p[0] - 32 : p[0])) + ":";
            const std::string rest = p.substr(2);
            abs += rest.empty() || rest[0] != '\\' ? cwd_.substr(2) + "\\" + rest : rest;
        } else if (!p.empty() && p[0] == '\\') {
            abs = cwd_.substr(0, 2) + p;
        } else {
            abs = cwd_ + "\\" + p;
        }
        std::vector<std::string> parts;
        std::size_t i = 2;
        while (i <= abs.size()) {
            std::size_t j = abs.find('\\', i);
            if (j == std::string::npos) j = abs.size();
            std::string part = abs.substr(i, j - i);
            if (part == "..") {
                if (!parts.empty()) parts.pop_back();
            } else if (!part.empty() && part != ".") {
                while (!part.empty() && (part.back() == '.' || part.back() == ' ')) part.pop_back();
                if (part.find_first_of("<>\"|?*") != std::string::npos || part.find(':') != std::string::npos) {
                    return std::nullopt;
                }
                if (!part.empty()) parts.push_back(part);
            }
            i = j + 1;
        }
        std::string out = abs.substr(0, 2);
        for (const std::string& part : parts) out += "\\" + part;
        if (parts.empty()) out += "\\";
        return out;
    }

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* name, Runtime::ImportShim shim) {
            for (const char* dll : {"KERNEL32.dll", "KERNELBASE.dll"}) {
                n += rt_.set_import(dll, name, shim);
            }
        };
        using files_detail::ansi_to_utf8;
        using files_detail::wide_to_utf8;
        auto a = [](CpuState& s, int i) { return ansi_to_utf8(win64_arg(s, i)); };
        auto w = [](CpuState& s, int i) { return wide_to_utf8(win64_arg(s, i)); };
        bind("CreateFileA", [this, a](CpuState& s) { create_file(s, a(s, 0)); });
        bind("CreateFileW", [this, w](CpuState& s) { create_file(s, w(s, 0)); });
        bind("ReadFile", [this](CpuState& s) { read_file(s); });
        bind("WriteFile", [this](CpuState& s) { write_file(s); });
        bind("SetFilePointer", [this](CpuState& s) { set_file_pointer(s); });
        bind("SetFilePointerEx", [this](CpuState& s) { set_file_pointer_ex(s); });
        bind("SetEndOfFile", [this](CpuState& s) { set_end_of_file(s); });
        bind("FlushFileBuffers", [this](CpuState& s) {
            std::shared_ptr<OpenFile> f = file_of(s, win64_arg(s, 0));
            if (!f) return win64_return(s, 0);
            std::lock_guard<std::mutex> fl(f->m);
            flush_locked(*f);
            win64_return(s, 1);
        });
        bind("GetFileSize", [this](CpuState& s) {
            std::shared_ptr<OpenFile> f = file_of(s, win64_arg(s, 0));
            if (!f) {
                win64_return(s, kInvalidFileAttributes);
                return;
            }
            std::lock_guard<std::mutex> fl(f->m);
            const std::uint64_t size = f->data.size();
            if (const std::uint64_t hi = win64_arg(s, 1)) files_detail::put<std::uint32_t>(hi, static_cast<std::uint32_t>(size >> 32));
            teb::set_error(s, 0);
            win64_return(s, static_cast<std::uint32_t>(size));
        });
        bind("GetFileSizeEx", [this](CpuState& s) {
            std::shared_ptr<OpenFile> f = file_of(s, win64_arg(s, 0));
            if (!f) return win64_return(s, 0);
            std::lock_guard<std::mutex> fl(f->m);
            files_detail::put<std::uint64_t>(win64_arg(s, 1), f->data.size());
            win64_return(s, 1);
        });
        bind("GetFileType", [this](CpuState& s) {
            std::shared_ptr<OpenFile> f = file_of(s, win64_arg(s, 0));
            if (!f) {
                win64_return(s, 0);  // FILE_TYPE_UNKNOWN
                return;
            }
            win64_return(s, f->console ? 2 : 1);  // FILE_TYPE_CHAR / FILE_TYPE_DISK
        });
        bind("FindFirstFileA", [this, a](CpuState& s) { find_first(s, a(s, 0), win64_arg(s, 1), false); });
        bind("FindFirstFileW", [this, w](CpuState& s) { find_first(s, w(s, 0), win64_arg(s, 1), true); });
        bind("FindFirstFileExA", [this, a](CpuState& s) { find_first(s, a(s, 0), win64_arg(s, 2), false); });
        bind("FindFirstFileExW", [this, w](CpuState& s) { find_first(s, w(s, 0), win64_arg(s, 2), true); });
        bind("FindNextFileA", [this](CpuState& s) { find_next(s, false); });
        bind("FindNextFileW", [this](CpuState& s) { find_next(s, true); });
        bind("FindClose", [this](CpuState& s) {
            std::lock_guard<std::mutex> lock(m_);
            const bool ok = finds_.erase(win64_arg(s, 0)) != 0;
            if (!ok) teb::set_error(s, kernel::kErrorInvalidHandle);
            win64_return(s, ok ? 1 : 0);
        });
        bind("CreateDirectoryA", [this, a](CpuState& s) { create_directory(s, a(s, 0)); });
        bind("CreateDirectoryW", [this, w](CpuState& s) { create_directory(s, w(s, 0)); });
        bind("RemoveDirectoryA", [this, a](CpuState& s) { remove_directory(s, a(s, 0)); });
        bind("RemoveDirectoryW", [this, w](CpuState& s) { remove_directory(s, w(s, 0)); });
        bind("DeleteFileA", [this, a](CpuState& s) { delete_file(s, a(s, 0)); });
        bind("DeleteFileW", [this, w](CpuState& s) { delete_file(s, w(s, 0)); });
        bind("CopyFileA", [this, a](CpuState& s) { copy_file(s, a(s, 0), a(s, 1), (win64_arg(s, 2) & 0xFFFFFFFF) != 0); });
        bind("CopyFileW", [this, w](CpuState& s) { copy_file(s, w(s, 0), w(s, 1), (win64_arg(s, 2) & 0xFFFFFFFF) != 0); });
        bind("MoveFileExA", [this, a](CpuState& s) { move_file(s, a(s, 0), a(s, 1), static_cast<std::uint32_t>(win64_arg(s, 2))); });
        bind("MoveFileExW", [this, w](CpuState& s) { move_file(s, w(s, 0), w(s, 1), static_cast<std::uint32_t>(win64_arg(s, 2))); });
        bind("GetFileAttributesA", [this, a](CpuState& s) { win64_return(s, attributes(s, a(s, 0))); });
        bind("GetFileAttributesW", [this, w](CpuState& s) { win64_return(s, attributes(s, w(s, 0))); });
        bind("GetFileAttributesExA", [this, a](CpuState& s) { attributes_ex(s, a(s, 0)); });
        bind("GetFileAttributesExW", [this, w](CpuState& s) { attributes_ex(s, w(s, 0)); });
        bind("GetFullPathNameA", [this, a](CpuState& s) { full_path(s, a(s, 0), false); });
        bind("GetFullPathNameW", [this, w](CpuState& s) { full_path(s, w(s, 0), true); });
        bind("GetCurrentDirectoryA", [this](CpuState& s) { current_directory(s, false); });
        bind("GetCurrentDirectoryW", [this](CpuState& s) { current_directory(s, true); });
        bind("SetCurrentDirectoryA", [this, a](CpuState& s) { set_current_directory(s, a(s, 0)); });
        bind("SetCurrentDirectoryW", [this, w](CpuState& s) { set_current_directory(s, w(s, 0)); });
        bind("GetTempPathA", [this](CpuState& s) { temp_path(s, false); });
        bind("GetTempPathW", [this](CpuState& s) { temp_path(s, true); });
        auto disk = [](CpuState& s) {
            const std::uint32_t v[4] = {8, 512, 0x10000000U, 0x20000000U};  // 1 TiB free of 2 TiB
            for (int i = 0; i < 4; ++i) {
                if (const std::uint64_t p = win64_arg(s, i + 1)) files_detail::put<std::uint32_t>(p, v[i]);
            }
            win64_return(s, 1);
        };
        bind("GetDiskFreeSpaceA", disk);
        bind("GetDiskFreeSpaceW", disk);
        for (const char* lk : {"LockFile", "UnlockFile", "LockFileEx", "UnlockFileEx"}) {
            bind(lk, [](CpuState& s) { win64_return(s, 1); });  // one process: nothing to lock out
        }
        bind("CreateFileMappingA", [this](CpuState& s) {
            create_mapping(s, crt_detail::utf16(files_detail::ansi_to_utf8(win64_arg(s, 5))));
        });
        bind("CreateFileMappingW", [this](CpuState& s) { create_mapping(s, crt_detail::guest_wstring(win64_arg(s, 5))); });
        bind("OpenFileMappingA", [this](CpuState& s) {
            open_mapping(s, crt_detail::utf16(files_detail::ansi_to_utf8(win64_arg(s, 2))));
        });
        bind("OpenFileMappingW", [this](CpuState& s) { open_mapping(s, crt_detail::guest_wstring(win64_arg(s, 2))); });
        bind("MapViewOfFile", [this](CpuState& s) { map_view(s); });
        bind("UnmapViewOfFile", [this](CpuState& s) { unmap_view(s, true); });
        bind("FlushViewOfFile", [this](CpuState& s) { unmap_view(s, false); });
        bind("GetStdHandle", [this](CpuState& s) {
            const auto which = static_cast<std::uint32_t>(win64_arg(s, 0));
            const int i = which == 0xFFFFFFF6U ? 0 : (which == 0xFFFFFFF5U ? 1 : (which == 0xFFFFFFF4U ? 2 : -1));
            if (i < 0) {
                teb::set_error(s, kernel::kErrorInvalidHandle);
                win64_return(s, kInvalidHandleValue);
                return;
            }
            std::lock_guard<std::mutex> lock(m_);
            win64_return(s, std_handles_[i]);
        });
        bind("SetStdHandle", [this](CpuState& s) {
            const auto which = static_cast<std::uint32_t>(win64_arg(s, 0));
            const int i = which == 0xFFFFFFF6U ? 0 : (which == 0xFFFFFFF5U ? 1 : (which == 0xFFFFFFF4U ? 2 : -1));
            if (i < 0) {
                teb::set_error(s, kernel::kErrorInvalidHandle);
                win64_return(s, 0);
                return;
            }
            std::lock_guard<std::mutex> lock(m_);
            std_handles_[i] = win64_arg(s, 1);
            win64_return(s, 1);
        });
        bind("GetConsoleMode", [this](CpuState& s) {
            std::shared_ptr<OpenFile> f = console_of(win64_arg(s, 0));
            if (!f) {
                teb::set_error(s, kernel::kErrorInvalidHandle);
                win64_return(s, 0);
                return;
            }
            files_detail::put<std::uint32_t>(win64_arg(s, 1), 3);  // ENABLE_PROCESSED_*: a plain console
            win64_return(s, 1);
        });
        bind("WriteConsoleW", [this](CpuState& s) {
            std::shared_ptr<OpenFile> f = console_of(win64_arg(s, 0));
            const auto n = static_cast<std::uint32_t>(win64_arg(s, 2));
            if (!f) {
                teb::set_error(s, kernel::kErrorInvalidHandle);
                win64_return(s, 0);
                return;
            }
            std::u16string text(n, u'\0');
            std::memcpy(text.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(win64_arg(s, 1))), 2ULL * n);
            if (on_console) on_console(crt_detail::utf8(text));
            if (const std::uint64_t out = win64_arg(s, 3)) files_detail::put<std::uint32_t>(out, n);
            win64_return(s, 1);
        });
        bind("ReadConsoleW", [this](CpuState& s) {
            std::shared_ptr<OpenFile> f = console_of(win64_arg(s, 0));
            if (!f) {
                teb::set_error(s, kernel::kErrorInvalidHandle);
                win64_return(s, 0);
                return;
            }
            if (const std::uint64_t out = win64_arg(s, 3)) files_detail::put<std::uint32_t>(out, 0);  // end of input
            win64_return(s, 1);
        });
        bind("SetConsoleTitleA", [this](CpuState& s) {
            const bool has = cfg_.console;
            if (!has) teb::set_error(s, kernel::kErrorInvalidHandle);
            win64_return(s, has ? 1 : 0);
        });
        bind("SetConsoleCtrlHandler", [](CpuState& s) { win64_return(s, 1); });
        for (const char* dll : {"SHELL32.dll"}) {
            n += rt_.set_import(dll, "SHGetFolderPathA", [this](CpuState& s) { folder_path(s, false); });
            n += rt_.set_import(dll, "SHGetFolderPathW", [this](CpuState& s) { folder_path(s, true); });
        }
        return n;
    }

private:
    struct Mount {
        std::string prefix;        // canonical guest path, e.g. "C:\WSE2"
        std::string prefix_upper;  // for matching
        platform::Vfs* lower = nullptr;
        std::string lower_root;
        platform::Vfs* upper = nullptr;
        std::string upper_root;
    };
    // A guest path resolved to a mount and a `Vfs` path below it ("" = the mount itself).
    struct Location {
        const Mount* mount = nullptr;
        std::string rel;
        std::string guest;  // canonical guest path
        bool virtual_dir = false;  // an ancestor of a mount (C:\, C:\Users)
    };
    struct OpenFile {
        Files* owner = nullptr;
        Location loc;
        std::vector<std::uint8_t> data;
        std::uint64_t pos = 0;
        bool readable = false;
        bool writable = false;
        bool dirty = false;
        bool console = false;
        std::mutex m;
        ~OpenFile() {
            if (owner != nullptr && dirty) {
                owner->write_back(*this);
            }
        }
    };
    struct Mapping {
        std::shared_ptr<OpenFile> file;  // null for memory backed by the page file
        std::uint8_t* mem = nullptr;
        std::size_t size = 0;
        bool writable = false;
        ~Mapping() {
            if (mem != nullptr) ::operator delete(mem, std::align_val_t{65536});
        }
    };
    struct Find {
        std::vector<std::pair<std::string, bool>> entries;  // name, is directory
        std::vector<std::uint64_t> sizes;
        std::size_t next = 0;
        bool wide = false;
    };

    void add_mount(const std::string& prefix, platform::Vfs* lower, std::string lower_root, platform::Vfs* upper,
                   std::string upper_root) {
        Mount m;
        m.prefix = prefix;
        while (m.prefix.size() > 3 && m.prefix.back() == '\\') m.prefix.pop_back();
        m.prefix_upper = files_detail::upper_utf8(m.prefix);
        m.lower = lower;
        m.lower_root = std::move(lower_root);
        m.upper = upper;
        m.upper_root = std::move(upper_root);
        mounts_.push_back(std::move(m));
    }

    static std::string join(const std::string& root, const std::string& rel) {
        if (root.empty()) return rel;
        if (rel.empty()) return root;
        return root + "/" + rel;
    }

    // Resolves a guest path; false (with the Win32 error) when it is invalid or outside every mount.
    bool locate(const std::string& path, Location& out, std::uint32_t& error) const {
        const std::optional<std::string> canon = canonical(path);
        if (!canon) {
            error = kErrorInvalidName;
            return false;
        }
        out.guest = *canon;
        const std::string up = files_detail::upper_utf8(*canon);
        const Mount* best = nullptr;
        for (const Mount& m : mounts_) {
            if (up.rfind(m.prefix_upper, 0) == 0 && (up.size() == m.prefix_upper.size() || up[m.prefix_upper.size()] == '\\') &&
                (best == nullptr || m.prefix_upper.size() > best->prefix_upper.size())) {
                best = &m;
            }
        }
        if (best == nullptr) {
            for (const Mount& m : mounts_) {
                const std::string anc = up.size() == 3 ? up.substr(0, 2) : up;  // "C:\" -> "C:"
                if (m.prefix_upper.rfind(anc + "\\", 0) == 0 || m.prefix_upper == anc) {
                    out.virtual_dir = true;
                    return true;
                }
            }
            error = kErrorPathNotFound;
            return false;
        }
        out.mount = best;
        std::string rel = canon->size() > best->prefix.size() ? canon->substr(best->prefix.size() + 1) : "";
        for (char& c : rel) {
            if (c == '\\') c = '/';
        }
        out.rel = rel;
        return true;
    }

    // Overlay queries (callers hold `m_`).
    bool whiteout(const Location& l) const { return whiteouts_.count(files_detail::upper_utf8(l.guest)) != 0; }
    bool upper_file(const Location& l) const {
        return l.mount->upper != nullptr && l.mount->upper->is_file(join(l.mount->upper_root, l.rel));
    }
    bool lower_file(const Location& l) const {
        return l.mount->lower != nullptr && !whiteout(l) && l.mount->lower->is_file(join(l.mount->lower_root, l.rel));
    }
    bool is_file(const Location& l) const { return !l.virtual_dir && (upper_file(l) || lower_file(l)); }
    bool is_dir(const Location& l) const {
        if (l.virtual_dir) return true;
        if (l.rel.empty()) return true;
        return (l.mount->upper != nullptr && l.mount->upper->is_dir(join(l.mount->upper_root, l.rel))) ||
               (l.mount->lower != nullptr && !whiteout(l) && l.mount->lower->is_dir(join(l.mount->lower_root, l.rel)));
    }
    bool parent_is_dir(const Location& l) const {
        if (l.virtual_dir || l.rel.empty()) return true;
        Location p = l;
        const std::size_t cut = p.rel.rfind('/');
        p.rel = cut == std::string::npos ? "" : p.rel.substr(0, cut);
        const std::size_t gcut = p.guest.rfind('\\');
        p.guest = gcut == std::string::npos ? p.guest : p.guest.substr(0, gcut);
        return is_dir(p);
    }
    bool read_whole(const Location& l, std::vector<std::uint8_t>& out) const {
        if (upper_file(l)) return l.mount->upper->read_file(join(l.mount->upper_root, l.rel), out);
        if (lower_file(l)) return l.mount->lower->read_file(join(l.mount->lower_root, l.rel), out);
        return false;
    }
    bool size_of(const Location& l, std::uint64_t& size) const {
        if (upper_file(l)) return l.mount->upper->file_size(join(l.mount->upper_root, l.rel), size);
        if (lower_file(l)) return l.mount->lower->file_size(join(l.mount->lower_root, l.rel), size);
        return false;
    }
    bool write_whole(const Location& l, const std::vector<std::uint8_t>& data) {
        if (l.mount == nullptr || l.mount->upper == nullptr) return false;
        const std::string up = join(l.mount->upper_root, l.rel);
        if (!platform::make_parent_dirs(*l.mount->upper, up)) return false;
        whiteouts_.erase(files_detail::upper_utf8(l.guest));
        return l.mount->upper->write_file(up, std::span<const std::uint8_t>(data.data(), data.size()));
    }
    bool remove(const Location& l) {
        bool any = false;
        if (upper_file(l)) any = l.mount->upper->remove_file(join(l.mount->upper_root, l.rel));
        if (l.mount->lower != nullptr && !whiteout(l) && l.mount->lower->is_file(join(l.mount->lower_root, l.rel))) {
            whiteouts_.insert(files_detail::upper_utf8(l.guest));  // the install is never modified
            any = true;
        }
        return any;
    }
    // Directory entries of `l` (the overlay's union; ancestors of mounts list their next component).
    std::vector<std::pair<std::string, bool>> list(const Location& l) const {
        std::map<std::string, std::pair<std::string, bool>> merged;  // upper-case name -> entry
        auto add = [&](platform::Vfs* vfs, const std::string& root, bool lower) {
            if (vfs == nullptr) return;
            std::vector<platform::VfsEntry> entries;
            if (!vfs->list_dir(join(root, l.rel), entries)) return;
            for (const platform::VfsEntry& e : entries) {
                const std::string key = files_detail::upper_utf8(e.name);
                if (lower && whiteouts_.count(files_detail::upper_utf8(l.guest + "\\" + e.name)) != 0) continue;
                if (l.rel.empty() && !lower && l.mount->upper_root.empty() && l.mount == &mounts_[1]) {
                    // the profile root also holds the overlay's and ProgramData's storage
                    if (key == "INSTALL" || key == "PROGRAMDATA") continue;
                }
                merged.emplace(key, std::make_pair(e.name, e.is_directory));
            }
        };
        if (l.virtual_dir) {
            const std::string up = files_detail::upper_utf8(l.guest);
            const std::string base = up.size() == 3 ? up : up + "\\";
            for (const Mount& m : mounts_) {
                if (m.prefix_upper.rfind(base, 0) != 0) continue;
                const std::string rest = m.prefix.substr(base.size());
                const std::string first = rest.substr(0, rest.find('\\'));
                merged.emplace(files_detail::upper_utf8(first), std::make_pair(first, true));
            }
        } else {
            add(l.mount->upper, l.mount->upper_root, false);
            add(l.mount->lower, l.mount->lower_root, true);
        }
        std::vector<std::pair<std::string, bool>> out;
        for (auto& [k, v] : merged) out.push_back(v);  // NTFS order: by upper-case name
        return out;
    }

    // --- Handles --------------------------------------------------------------------------------
    std::uint64_t file_handle(std::shared_ptr<OpenFile> f) {
        auto o = std::make_shared<kernel::Object>();
        o->kind = kernel::Kind::file;
        {
            std::lock_guard<std::mutex> lock(opened_m_);
            opened_.push_back(f);
        }
        o->payload = std::move(f);
        return kernel_.add_handle(std::move(o));
    }
    // The open file behind `h`; null (with ERROR_INVALID_HANDLE set) otherwise. The caller returns.
    std::shared_ptr<OpenFile> file_of(CpuState& s, std::uint64_t h) {
        std::shared_ptr<kernel::Object> o = h == 0 || h == kInvalidHandleValue ? nullptr : kernel_.resolve(s, h);
        if (!o || o->kind != kernel::Kind::file) {
            teb::set_error(s, kernel::kErrorInvalidHandle);
            return nullptr;
        }
        return std::static_pointer_cast<OpenFile>(o->payload);
    }
    std::shared_ptr<OpenFile> console_of(std::uint64_t h) {
        CpuState dummy;
        std::shared_ptr<kernel::Object> o = h == 0 || h == kInvalidHandleValue ? nullptr : kernel_.resolve(dummy, h);
        if (!o || o->kind != kernel::Kind::file) return nullptr;
        auto f = std::static_pointer_cast<OpenFile>(o->payload);
        return f->console ? f : nullptr;
    }
    void flush_locked(OpenFile& f) {
        if (f.dirty && !f.console) {
            write_back(f);
            f.dirty = false;
        }
    }
    void write_back(OpenFile& f) {
        std::lock_guard<std::mutex> lock(m_);
        (void)write_whole(f.loc, f.data);
    }

    // CreateFile(name, access, share, security, disposition, flags, template).
    void create_file(CpuState& s, const std::string& path) {
        const auto access = static_cast<std::uint32_t>(win64_arg(s, 1));
        const auto disposition = static_cast<std::uint32_t>(win64_arg(s, 4));
        Location l;
        std::uint32_t error = 0;
        std::unique_lock<std::mutex> lock(m_);
        if (!locate(path, l, error)) {
            lock.unlock();
            return fail_handle(s, error);
        }
        if (l.virtual_dir || is_dir(l)) {
            lock.unlock();
            return fail_handle(s, kErrorAccessDenied);  // directory handles are not supported
        }
        if (!parent_is_dir(l)) {
            lock.unlock();
            return fail_handle(s, kErrorPathNotFound);
        }
        const bool exists = is_file(l);
        const bool write = (access & (kGenericWrite | 0x2U /* FILE_WRITE_DATA */ | 0x10000000U /* GENERIC_ALL */)) != 0;
        auto f = std::make_shared<OpenFile>();
        f->owner = this;
        f->loc = l;
        f->readable = (access & (kGenericRead | 0x1U | 0x10000000U)) != 0;
        f->writable = write;
        std::uint32_t success_error = 0;
        switch (disposition) {
        case kCreateNew:
            if (exists) {
                lock.unlock();
                return fail_handle(s, kErrorFileExists);
            }
            f->dirty = true;
            break;
        case kCreateAlways:
            success_error = exists ? kErrorAlreadyExistsFiles : 0;
            f->dirty = true;
            break;
        case kOpenExisting:
            if (!exists) {
                lock.unlock();
                return fail_handle(s, kErrorFileNotFound);
            }
            (void)read_whole(l, f->data);
            break;
        case kOpenAlways:
            if (exists) {
                success_error = kErrorAlreadyExistsFiles;
                (void)read_whole(l, f->data);
            } else {
                f->dirty = true;
            }
            break;
        case kTruncateExisting:
            if (!exists || !write) {
                lock.unlock();
                return fail_handle(s, !exists ? kErrorFileNotFound : teb::kErrorInvalidParameter);
            }
            f->dirty = true;
            break;
        default:
            lock.unlock();
            return fail_handle(s, teb::kErrorInvalidParameter);
        }
        if ((f->dirty) && (l.mount == nullptr || l.mount->upper == nullptr)) {
            lock.unlock();
            return fail_handle(s, kErrorAccessDenied);
        }
        if (f->dirty) {
            // A new or truncated file exists from now on (CreateFile writes it at once).
            (void)write_whole(l, f->data);
            f->dirty = false;
        }
        lock.unlock();
        const std::uint64_t h = file_handle(std::move(f));
        teb::set_error(s, success_error);
        win64_return(s, h);
    }
    static void fail_handle(CpuState& s, std::uint32_t error) {
        teb::set_error(s, error);
        win64_return(s, kInvalidHandleValue);
    }

    // ReadFile(handle, buffer, count, *read, overlapped).
    void read_file(CpuState& s) {
        std::shared_ptr<OpenFile> f = file_of(s, win64_arg(s, 0));
        if (!f) return win64_return(s, 0);
        const std::uint64_t buf = win64_arg(s, 1);
        const auto n = static_cast<std::uint32_t>(win64_arg(s, 2));
        const std::uint64_t out = win64_arg(s, 3);
        std::lock_guard<std::mutex> fl(f->m);
        if (!f->readable) {
            teb::set_error(s, kErrorAccessDenied);
            win64_return(s, 0);
            return;
        }
        const std::uint64_t avail = f->pos < f->data.size() ? f->data.size() - f->pos : 0;
        const std::uint32_t got = static_cast<std::uint32_t>(std::min<std::uint64_t>(avail, n));
        if (got != 0) {
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(buf)), f->data.data() + f->pos, got);
        }
        f->pos += got;
        if (out != 0) files_detail::put<std::uint32_t>(out, got);
        win64_return(s, 1);
    }

    // WriteFile(handle, buffer, count, *written, overlapped).
    void write_file(CpuState& s) {
        std::shared_ptr<OpenFile> f = file_of(s, win64_arg(s, 0));
        if (!f) return win64_return(s, 0);
        const std::uint64_t buf = win64_arg(s, 1);
        const auto n = static_cast<std::uint32_t>(win64_arg(s, 2));
        const std::uint64_t out = win64_arg(s, 3);
        std::lock_guard<std::mutex> fl(f->m);
        if (!f->writable) {
            teb::set_error(s, kErrorAccessDenied);
            win64_return(s, 0);
            return;
        }
        if (f->console) {
            if (on_console) {
                on_console(std::string(reinterpret_cast<const char*>(static_cast<std::uintptr_t>(buf)), n));
            }
        } else if (n != 0) {
            if (f->data.size() < f->pos + n) f->data.resize(static_cast<std::size_t>(f->pos + n));
            std::memcpy(f->data.data() + f->pos, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(buf)), n);
            f->pos += n;
            f->dirty = true;
        }
        if (out != 0) files_detail::put<std::uint32_t>(out, n);
        win64_return(s, 1);
    }

    // The new position for a move of `distance` from `method` (FILE_BEGIN / CURRENT / END).
    static bool seek(OpenFile& f, std::int64_t distance, std::uint32_t method, std::uint64_t& to) {
        const std::int64_t base = method == 0 ? 0 : (method == 1 ? static_cast<std::int64_t>(f.pos)
                                                                   : static_cast<std::int64_t>(f.data.size()));
        if (method > 2) return false;
        const std::int64_t r = base + distance;
        if (r < 0) return false;
        to = static_cast<std::uint64_t>(r);
        return true;
    }

    // SetFilePointer(handle, distanceLow, *distanceHigh, method) -> new low DWORD.
    void set_file_pointer(CpuState& s) {
        std::shared_ptr<OpenFile> f = file_of(s, win64_arg(s, 0));
        if (!f) return win64_return(s, 0xFFFFFFFFU);
        const std::uint64_t hi_ptr = win64_arg(s, 2);
        std::int64_t distance = static_cast<std::int32_t>(win64_arg(s, 1));
        if (hi_ptr != 0) {
            distance = static_cast<std::int64_t>((static_cast<std::uint64_t>(files_detail::get<std::uint32_t>(hi_ptr)) << 32) |
                                                 static_cast<std::uint32_t>(win64_arg(s, 1)));
        }
        std::lock_guard<std::mutex> fl(f->m);
        std::uint64_t to = 0;
        if (!seek(*f, distance, static_cast<std::uint32_t>(win64_arg(s, 3)), to)) {
            teb::set_error(s, kErrorNegativeSeek);
            win64_return(s, 0xFFFFFFFFU);  // INVALID_SET_FILE_POINTER
            return;
        }
        f->pos = to;
        if (hi_ptr != 0) files_detail::put<std::uint32_t>(hi_ptr, static_cast<std::uint32_t>(to >> 32));
        teb::set_error(s, 0);
        win64_return(s, static_cast<std::uint32_t>(to));
    }

    // SetFilePointerEx(handle, distance, *newPosition, method).
    void set_file_pointer_ex(CpuState& s) {
        std::shared_ptr<OpenFile> f = file_of(s, win64_arg(s, 0));
        if (!f) return win64_return(s, 0);
        std::lock_guard<std::mutex> fl(f->m);
        std::uint64_t to = 0;
        if (!seek(*f, static_cast<std::int64_t>(win64_arg(s, 1)), static_cast<std::uint32_t>(win64_arg(s, 3)), to)) {
            teb::set_error(s, kErrorNegativeSeek);
            win64_return(s, 0);
            return;
        }
        f->pos = to;
        if (const std::uint64_t out = win64_arg(s, 2)) files_detail::put<std::uint64_t>(out, to);
        win64_return(s, 1);
    }

    void set_end_of_file(CpuState& s) {
        std::shared_ptr<OpenFile> f = file_of(s, win64_arg(s, 0));
        if (!f) return win64_return(s, 0);
        std::lock_guard<std::mutex> fl(f->m);
        if (!f->writable) {
            teb::set_error(s, kErrorAccessDenied);
            win64_return(s, 0);
            return;
        }
        f->data.resize(static_cast<std::size_t>(f->pos));
        f->dirty = true;
        win64_return(s, 1);
    }

    // --- Directory search -------------------------------------------------------------------------
    // WIN32_FIND_DATAA (320 bytes) / WIN32_FIND_DATAW (592 bytes).
    static void fill_find(std::uint64_t d, const std::string& name, bool dir, std::uint64_t size, bool wide) {
        std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(d)), 0, wide ? 592 : 320);
        files_detail::put<std::uint32_t>(d, dir ? kFileAttributeDirectory : kFileAttributeArchive);
        for (int i = 0; i < 3; ++i) files_detail::put<std::uint64_t>(d + 4 + 8ULL * i, kFixedFileTime);
        files_detail::put<std::uint32_t>(d + 28, static_cast<std::uint32_t>(size >> 32));
        files_detail::put<std::uint32_t>(d + 32, static_cast<std::uint32_t>(size));
        if (wide) {
            std::u16string w = crt_detail::utf16(name);
            if (w.size() > 259) w.resize(259);
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(d + 44)), w.c_str(), 2 * (w.size() + 1));
        } else {
            std::string a = files_detail::utf8_to_ansi(name);
            if (a.size() > 259) a.resize(259);
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(d + 44)), a.c_str(), a.size() + 1);
        }
    }

    void find_first(CpuState& s, const std::string& spec, std::uint64_t data, bool wide) {
        std::string norm = spec;
        for (char& c : norm) {
            if (c == '/') c = '\\';
        }
        const std::size_t cut = norm.rfind('\\');
        const std::string dir = cut == std::string::npos ? "." : (cut == 0 ? "\\" : norm.substr(0, cut));
        const std::string pattern = cut == std::string::npos ? norm : norm.substr(cut + 1);
        Location l;
        std::uint32_t error = 0;
        std::unique_lock<std::mutex> lock(m_);
        if (pattern.empty() || !locate(dir.size() == 2 && dir[1] == ':' ? dir + "\\" : dir, l, error) || !is_dir(l)) {
            lock.unlock();
            return fail_handle(s, pattern.empty() ? kErrorFileNotFound : (error != 0 ? error : kErrorPathNotFound));
        }
        Find f;
        f.wide = wide;
        const bool root = l.guest.size() == 3;
        if (!root) {
            for (const char* dot : {".", ".."}) {
                if (files_detail::matches(pattern, dot)) {
                    f.entries.emplace_back(dot, true);
                    f.sizes.push_back(0);
                }
            }
        }
        for (const auto& [name, is_directory] : list(l)) {
            if (!files_detail::matches(pattern, name)) continue;
            std::uint64_t size = 0;
            if (!is_directory) {
                Location child = l;
                child.rel = l.rel.empty() ? name : l.rel + "/" + name;
                child.guest = l.guest + (root ? "" : "\\") + name;
                child.virtual_dir = false;
                if (child.mount != nullptr) (void)size_of(child, size);
            }
            f.entries.emplace_back(name, is_directory);
            f.sizes.push_back(size);
        }
        if (f.entries.empty()) {
            lock.unlock();
            return fail_handle(s, kErrorFileNotFound);
        }
        fill_find(data, f.entries[0].first, f.entries[0].second, f.sizes[0], wide);
        f.next = 1;
        next_find_ += 4;
        const std::uint64_t h = next_find_;
        finds_[h] = std::move(f);
        lock.unlock();
        win64_return(s, h);
    }

    void find_next(CpuState& s, bool wide) {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = finds_.find(win64_arg(s, 0));
        if (it == finds_.end()) {
            teb::set_error(s, kernel::kErrorInvalidHandle);
            win64_return(s, 0);
            return;
        }
        Find& f = it->second;
        if (f.next >= f.entries.size()) {
            teb::set_error(s, kErrorNoMoreFiles);
            win64_return(s, 0);
            return;
        }
        fill_find(win64_arg(s, 1), f.entries[f.next].first, f.entries[f.next].second, f.sizes[f.next], wide);
        ++f.next;
        win64_return(s, 1);
    }

    // --- Paths and directories --------------------------------------------------------------------
    void create_directory(CpuState& s, const std::string& path) {
        Location l;
        std::uint32_t error = 0;
        std::lock_guard<std::mutex> lock(m_);
        if (!locate(path, l, error)) return fail_bool(s, error);
        if (l.virtual_dir || is_dir(l) || is_file(l)) return fail_bool(s, kErrorAlreadyExistsFiles);
        if (!parent_is_dir(l)) return fail_bool(s, kErrorPathNotFound);
        if (l.mount->upper == nullptr) return fail_bool(s, kErrorAccessDenied);
        const std::string up = join(l.mount->upper_root, l.rel);
        if (!platform::make_parent_dirs(*l.mount->upper, up + "/x") || (!l.mount->upper->is_dir(up) && !l.mount->upper->make_dir(up))) {
            return fail_bool(s, kErrorAccessDenied);
        }
        whiteouts_.erase(files_detail::upper_utf8(l.guest));
        win64_return(s, 1);
    }

    void remove_directory(CpuState& s, const std::string& path) {
        Location l;
        std::uint32_t error = 0;
        std::lock_guard<std::mutex> lock(m_);
        if (!locate(path, l, error)) return fail_bool(s, error);
        if (l.virtual_dir || l.rel.empty()) return fail_bool(s, kErrorAccessDenied);
        if (!is_dir(l)) return fail_bool(s, is_file(l) ? 267 /* ERROR_DIRECTORY */ : kErrorFileNotFound);
        if (!list(l).empty()) return fail_bool(s, kErrorDirNotEmpty);
        const bool in_lower = l.mount->lower != nullptr && l.mount->lower->is_dir(join(l.mount->lower_root, l.rel));
        if (l.mount->upper != nullptr && l.mount->upper->is_dir(join(l.mount->upper_root, l.rel))) {
            (void)l.mount->upper->remove_dir(join(l.mount->upper_root, l.rel));
        }
        if (in_lower) whiteouts_.insert(files_detail::upper_utf8(l.guest));
        win64_return(s, 1);
    }

    void delete_file(CpuState& s, const std::string& path) {
        Location l;
        std::uint32_t error = 0;
        std::lock_guard<std::mutex> lock(m_);
        if (!locate(path, l, error)) return fail_bool(s, error);
        if (!is_file(l)) return fail_bool(s, is_dir(l) ? kErrorAccessDenied : (parent_is_dir(l) ? kErrorFileNotFound : kErrorPathNotFound));
        if (!remove(l)) return fail_bool(s, kErrorAccessDenied);
        win64_return(s, 1);
    }

    void copy_file(CpuState& s, const std::string& from, const std::string& to, bool fail_if_exists) {
        Location a;
        Location b;
        std::uint32_t error = 0;
        std::lock_guard<std::mutex> lock(m_);
        if (!locate(from, a, error) || !locate(to, b, error)) return fail_bool(s, error);
        std::vector<std::uint8_t> bytes;
        if (!is_file(a) || !read_whole(a, bytes)) return fail_bool(s, kErrorFileNotFound);
        if (b.virtual_dir || is_dir(b)) return fail_bool(s, kErrorAccessDenied);
        if (is_file(b) && fail_if_exists) return fail_bool(s, kErrorFileExists);
        if (!parent_is_dir(b)) return fail_bool(s, kErrorPathNotFound);
        if (!write_whole(b, bytes)) return fail_bool(s, kErrorAccessDenied);
        win64_return(s, 1);
    }

    // MoveFileEx(from, to, flags): MOVEFILE_REPLACE_EXISTING (1); a move is a copy and a delete.
    void move_file(CpuState& s, const std::string& from, const std::string& to, std::uint32_t flags) {
        Location a;
        Location b;
        std::uint32_t error = 0;
        std::lock_guard<std::mutex> lock(m_);
        if (!locate(from, a, error) || !locate(to, b, error)) return fail_bool(s, error);
        std::vector<std::uint8_t> bytes;
        if (!is_file(a) || !read_whole(a, bytes)) return fail_bool(s, kErrorFileNotFound);
        if (is_file(b) && (flags & 1) == 0) return fail_bool(s, kErrorAlreadyExistsFiles);
        if (b.virtual_dir || is_dir(b)) return fail_bool(s, kErrorAccessDenied);
        if (!parent_is_dir(b)) return fail_bool(s, kErrorPathNotFound);
        if (!write_whole(b, bytes) || !remove(a)) return fail_bool(s, kErrorAccessDenied);
        win64_return(s, 1);
    }

    std::uint32_t attributes(CpuState& s, const std::string& path) {
        Location l;
        std::uint32_t error = 0;
        std::lock_guard<std::mutex> lock(m_);
        if (!locate(path, l, error)) {
            teb::set_error(s, error);
            return kInvalidFileAttributes;
        }
        if (is_dir(l)) return kFileAttributeDirectory | (l.virtual_dir ? kFileAttributeReadonly : 0);
        if (is_file(l)) return kFileAttributeArchive;
        teb::set_error(s, parent_is_dir(l) ? kErrorFileNotFound : kErrorPathNotFound);
        return kInvalidFileAttributes;
    }

    // GetFileAttributesEx(name, GetFileExInfoStandard, *WIN32_FILE_ATTRIBUTE_DATA (36 bytes)).
    void attributes_ex(CpuState& s, const std::string& path) {
        const std::uint32_t attr = attributes(s, path);
        if (attr == kInvalidFileAttributes) {
            win64_return(s, 0);
            return;
        }
        std::uint64_t size = 0;
        if ((attr & kFileAttributeDirectory) == 0) {
            Location l;
            std::uint32_t error = 0;
            std::lock_guard<std::mutex> lock(m_);
            if (locate(path, l, error)) (void)size_of(l, size);
        }
        const std::uint64_t d = win64_arg(s, 2);
        files_detail::put<std::uint32_t>(d, attr);
        for (int i = 0; i < 3; ++i) files_detail::put<std::uint64_t>(d + 4 + 8ULL * i, kFixedFileTime);
        files_detail::put<std::uint32_t>(d + 28, static_cast<std::uint32_t>(size >> 32));
        files_detail::put<std::uint32_t>(d + 32, static_cast<std::uint32_t>(size));
        win64_return(s, 1);
    }

    // Writes `text` for an A or W API with a buffer of `cap` characters: the length without the
    // terminator when it fits, else the size needed with it.
    static std::uint64_t write_path(std::uint64_t buf, std::uint32_t cap, const std::string& text, bool wide) {
        if (wide) {
            const std::u16string w = crt_detail::utf16(text);
            if (cap < w.size() + 1) return w.size() + 1;
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(buf)), w.c_str(), 2 * (w.size() + 1));
            return w.size();
        }
        const std::string a = files_detail::utf8_to_ansi(text);
        if (cap < a.size() + 1) return a.size() + 1;
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(buf)), a.c_str(), a.size() + 1);
        return a.size();
    }

    // GetFullPathName(name, cap, buffer, *filePart).
    void full_path(CpuState& s, const std::string& name, bool wide) {
        std::optional<std::string> canon;
        {
            std::lock_guard<std::mutex> lock(m_);
            canon = canonical(name);
        }
        if (!canon || name.empty()) {
            teb::set_error(s, kErrorInvalidName);
            win64_return(s, 0);
            return;
        }
        std::string text = *canon;
        if (!name.empty() && (name.back() == '\\' || name.back() == '/') && text.back() != '\\') text += "\\";
        const std::uint64_t buf = win64_arg(s, 2);
        const auto cap = static_cast<std::uint32_t>(win64_arg(s, 1));
        const std::uint64_t r = write_path(buf, cap, text, wide);
        const std::uint64_t part = win64_arg(s, 3);
        if (part != 0 && r < cap) {  // it fit
            const std::size_t cut = text.rfind('\\');
            const std::string head = text.substr(0, cut + 1);
            const std::size_t chars = wide ? crt_detail::utf16(head).size() : files_detail::utf8_to_ansi(head).size();
            files_detail::put<std::uint64_t>(part, cut + 1 == text.size() ? 0 : buf + (wide ? 2 : 1) * chars);
        }
        win64_return(s, r);
    }

    void current_directory(CpuState& s, bool wide) {
        std::lock_guard<std::mutex> lock(m_);
        win64_return(s, write_path(win64_arg(s, 1), static_cast<std::uint32_t>(win64_arg(s, 0)), cwd_, wide));
    }

    void set_current_directory(CpuState& s, const std::string& path) {
        Location l;
        std::uint32_t error = 0;
        std::lock_guard<std::mutex> lock(m_);
        if (!locate(path, l, error)) return fail_bool(s, error);
        if (!is_dir(l)) return fail_bool(s, is_file(l) ? 267 : kErrorFileNotFound);
        cwd_ = l.guest;
        win64_return(s, 1);
    }

    // GetTempPath(cap, buffer): the profile's AppData\Local\Temp\, created on first use.
    void temp_path(CpuState& s, bool wide) {
        const std::string temp = cfg_.user_profile + "\\AppData\\Local\\Temp\\";
        {
            std::lock_guard<std::mutex> lock(m_);
            if (cfg_.user != nullptr) (void)platform::make_parent_dirs(*cfg_.user, "AppData/Local/Temp/x");
        }
        win64_return(s, write_path(win64_arg(s, 1), static_cast<std::uint32_t>(win64_arg(s, 0)), temp, wide));
    }

    // SHGetFolderPath(hwnd, csidl, token, flags, path[MAX_PATH]).
    void folder_path(CpuState& s, bool wide) {
        const auto csidl = static_cast<std::uint32_t>(win64_arg(s, 1));
        std::string rel;
        std::string base = cfg_.user_profile;
        switch (csidl & 0xFF) {
        case 0x00: rel = "Desktop"; break;                // CSIDL_DESKTOP
        case 0x05: rel = "Documents"; break;              // CSIDL_PERSONAL
        case 0x10: rel = "Desktop"; break;                // CSIDL_DESKTOPDIRECTORY
        case 0x1A: rel = "AppData\\Roaming"; break;       // CSIDL_APPDATA
        case 0x1C: rel = "AppData\\Local"; break;         // CSIDL_LOCAL_APPDATA
        case 0x28: rel = ""; break;                       // CSIDL_PROFILE
        case 0x23: base = "C:\\ProgramData"; rel = ""; break;  // CSIDL_COMMON_APPDATA
        case 0x2E: base = "C:\\Users\\Public\\Documents"; rel = ""; break;  // CSIDL_COMMON_DOCUMENTS
        default:
            win64_return(s, 0x80070057U);  // E_INVALIDARG
            return;
        }
        if (base == "C:\\Users\\Public\\Documents") {
            base = cfg_.user_profile;
            rel = "Documents";  // no public profile: the user's own Documents
        }
        const std::string path = rel.empty() ? base : base + "\\" + rel;
        {
            std::lock_guard<std::mutex> lock(m_);
            Location l;
            std::uint32_t error = 0;
            // The folder always exists here (CSIDL_FLAG_CREATE or not): the layer creates it.
            if (locate(path, l, error) && l.mount != nullptr && l.mount->upper != nullptr) {
                const std::string up = join(l.mount->upper_root, l.rel);
                if (!up.empty()) (void)platform::make_parent_dirs(*l.mount->upper, up + "/x");
            }
        }
        (void)write_path(win64_arg(s, 4), 260, path, wide);
        win64_return(s, 0);  // S_OK
    }

    static void fail_bool(CpuState& s, std::uint32_t error) {
        teb::set_error(s, error);
        win64_return(s, 0);
    }

    // --- File mappings ----------------------------------------------------------------------------
    // CreateFileMapping(file, security, protect, maxHigh, maxLow, name).
    void create_mapping(CpuState& s, const std::u16string& name) {
        const std::uint64_t file = win64_arg(s, 0);
        const auto protect = static_cast<std::uint32_t>(win64_arg(s, 2));
        const std::uint64_t max = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(win64_arg(s, 3))) << 32) |
                                  static_cast<std::uint32_t>(win64_arg(s, 4));
        auto m = std::make_shared<Mapping>();
        m->writable = (protect & 0x44U) != 0;  // PAGE_READWRITE / PAGE_EXECUTE_READWRITE
        std::size_t size = static_cast<std::size_t>(max);
        if (file != kInvalidHandleValue && file != 0) {
            std::shared_ptr<OpenFile> f = file_of(s, file);
            if (!f) return win64_return(s, 0);
            std::lock_guard<std::mutex> fl(f->m);
            if (size == 0) size = f->data.size();
            if (size == 0) {
                teb::set_error(s, 1006);  // ERROR_FILE_INVALID: an empty file cannot be mapped
                win64_return(s, 0);
                return;
            }
            if (m->writable && f->data.size() < size) {
                f->data.resize(size);
                f->dirty = true;
            }
            m->file = f;
        } else if (size == 0) {
            teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, 0);
            return;
        }
        m->size = size;
        m->mem = static_cast<std::uint8_t*>(::operator new(size, std::align_val_t{65536}));
        std::memset(m->mem, 0, size);
        if (m->file) {
            std::lock_guard<std::mutex> fl(m->file->m);
            std::memcpy(m->mem, m->file->data.data(), std::min(size, m->file->data.size()));
        }
        auto o = std::make_shared<kernel::Object>();
        o->kind = kernel::Kind::mapping;
        o->payload = std::move(m);
        win64_return(s, name.empty() ? kernel_.add_handle(std::move(o)) : kernel_.add_named_handle(s, name, std::move(o)));
    }

    // OpenFileMapping(access, inherit, name).
    void open_mapping(CpuState& s, const std::u16string& name) {
        std::shared_ptr<kernel::Object> o = kernel_.find_named(name, kernel::Kind::mapping);
        if (!o) {
            teb::set_error(s, kErrorFileNotFound);
            win64_return(s, 0);
            return;
        }
        win64_return(s, kernel_.add_handle(std::move(o)));
    }

    // MapViewOfFile(mapping, access, offsetHigh, offsetLow, bytes).
    void map_view(CpuState& s) {
        std::shared_ptr<kernel::Object> o = kernel_.resolve(s, win64_arg(s, 0));
        if (!o || o->kind != kernel::Kind::mapping) {
            teb::set_error(s, kernel::kErrorInvalidHandle);
            win64_return(s, 0);
            return;
        }
        auto m = std::static_pointer_cast<Mapping>(o->payload);
        const std::uint64_t offset = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(win64_arg(s, 2))) << 32) |
                                     static_cast<std::uint32_t>(win64_arg(s, 3));
        if (offset >= m->size || (offset & 0xFFFF) != 0) {
            teb::set_error(s, offset >= m->size ? kErrorAccessDenied : 1132 /* ERROR_MAPPED_ALIGNMENT */);
            win64_return(s, 0);
            return;
        }
        const std::uint64_t va = reinterpret_cast<std::uintptr_t>(m->mem) + offset;
        std::lock_guard<std::mutex> lock(m_);
        views_[va] = o;
        win64_return(s, va);
    }

    // UnmapViewOfFile(base) / FlushViewOfFile(base, bytes): a writable file-backed view's bytes go
    // back to the open file (written to storage when that file is flushed or closed).
    void unmap_view(CpuState& s, bool unmap) {
        const std::uint64_t va = win64_arg(s, 0);
        std::shared_ptr<kernel::Object> o;
        {
            std::lock_guard<std::mutex> lock(m_);
            auto it = views_.upper_bound(va);
            if (it != views_.begin()) {
                --it;
                auto m = std::static_pointer_cast<Mapping>(it->second->payload);
                if (va >= it->first && va < reinterpret_cast<std::uintptr_t>(m->mem) + m->size) {
                    o = it->second;
                    if (unmap) {
                        if (va != it->first) o = nullptr;  // UnmapViewOfFile takes the view's base
                        else views_.erase(it);
                    }
                }
            }
        }
        if (!o) {
            teb::set_error(s, 487);  // ERROR_INVALID_ADDRESS
            win64_return(s, 0);
            return;
        }
        auto m = std::static_pointer_cast<Mapping>(o->payload);
        if (m->file && m->writable) {
            std::lock_guard<std::mutex> fl(m->file->m);
            if (m->file->data.size() < m->size) m->file->data.resize(m->size);
            std::memcpy(m->file->data.data(), m->mem, m->size);
            m->file->dirty = true;
            if (!unmap) flush_locked(*m->file);
        }
        win64_return(s, 1);
    }

    Runtime& rt_;
    kernel::Kernel& kernel_;
    FilesConfig cfg_;
    std::vector<Mount> mounts_;
    mutable std::mutex m_;  // the mounts' Vfs objects, the overlay state, searches, views, cwd
    std::string cwd_;
    std::set<std::string> whiteouts_;  // upper-case guest paths removed from the install's view
    std::map<std::uint64_t, Find> finds_;
    std::uint64_t next_find_ = 0x40000000;
    std::map<std::uint64_t, std::shared_ptr<kernel::Object>> views_;
    std::uint64_t std_handles_[3] = {0, 0, 0};
    std::mutex opened_m_;
    std::vector<std::weak_ptr<OpenFile>> opened_;
};

}  // namespace wse::recomp::shim
