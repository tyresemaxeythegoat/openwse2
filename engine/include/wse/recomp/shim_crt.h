// Win64 CRT support, process and system information for translated code (Phase 5 R4,
// `docs/RECOMP_PLAN.md` "R4 design", families "CRT support" and "System information"; v1.1.5.6
// call sites: DecodePointer 42, EncodePointer 35, the rest 1-4 each):
//
//   EncodePointer DecodePointer IsDebuggerPresent IsProcessorFeaturePresent
//   SetUnhandledExceptionFilter UnhandledExceptionFilter InitOnceExecuteOnce
//   GetCommandLineA GetCommandLineW GetStartupInfoW GetEnvironmentStringsW
//   FreeEnvironmentStringsW SetEnvironmentVariableA SetEnvironmentVariableW
//   ExitProcess TerminateProcess OutputDebugStringA OutputDebugStringW FlushProcessWriteBuffers
//   GetSystemInfo GetNativeSystemInfo GetLogicalProcessorInformation GetProcessAffinityMask
//   GetVersionExW IsWow64Process GetNumaHighestNodeNumber GetCurrentProcessorNumber
//   GlobalMemoryStatusEx SetProcessWorkingSetSize, and ntdll's RtlGetVersion
//
// The process is described by `ProcessConfig` (command line, environment, processor count,
// memory size). System answers match the fixed CPU profile of `Runtime::hook_cpuid` (SSE up to
// SSE4.1, no AVX). `GetVersionExW` reports 6.2 build 9200, what Windows reports to an exe whose
// manifest declares no supported OS (v1.1.5.6's declares none). Header-only.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::shim {

inline constexpr std::uint32_t kErrorInsufficientBuffer = 122;
inline constexpr std::uint32_t kErrorEnvvarNotFound = 203;

namespace crt_detail {
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
inline std::string guest_string(std::uint64_t va) {
    std::string s;
    for (std::size_t i = 0; va != 0 && i < 65536; ++i) {
        const char c = get<char>(va + i);
        if (c == 0) {
            break;
        }
        s.push_back(c);
    }
    return s;
}
inline std::u16string guest_wstring(std::uint64_t va) {
    std::u16string s;
    for (std::size_t i = 0; va != 0 && i < 65536; ++i) {
        const char16_t c = get<char16_t>(va + 2 * i);
        if (c == 0) {
            break;
        }
        s.push_back(c);
    }
    return s;
}
// UTF-8 <-> UTF-16 (invalid UTF-8 bytes become U+FFFD).
inline std::u16string utf16(const std::string& s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0xFFFD;
        std::size_t n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 6 && i + 1 < s.size()) {
            cp = ((c & 0x1FU) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3FU);
            n = 2;
        } else if ((c >> 4) == 14 && i + 2 < s.size()) {
            cp = ((c & 0x0FU) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3FU);
            n = 3;
        } else if ((c >> 3) == 30 && i + 3 < s.size()) {
            cp = ((c & 0x07U) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3FU) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3FU) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3FU);
            n = 4;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += n;
    }
    return out;
}
inline std::string utf8(const std::u16string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        std::uint32_t cp = s[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00U);
            ++i;
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
inline std::uint64_t rotr(std::uint64_t v, unsigned n) noexcept { return n == 0 ? v : (v >> n) | (v << (64 - n)); }
inline std::uint64_t rotl(std::uint64_t v, unsigned n) noexcept { return n == 0 ? v : (v << n) | (v >> (64 - n)); }
}  // namespace crt_detail

struct ProcessConfig {
    std::string command_line = "mb_warband_wse2_x64.exe";
    std::map<std::string, std::string> environment;  // names as written; Windows compares them case-insensitively
    unsigned processors = 0;                         // 0: the host's count (at most 64)
    std::uint64_t memory_bytes = 8ULL << 30;
};

class Crt {
public:
    Crt(Runtime& rt, ProcessConfig config) : rt_(rt), config_(std::move(config)) {
        std::random_device rd;
        cookie_ = (static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^ 0x5DEECE66DULL;
        if (config_.processors == 0) {
            const unsigned n = std::thread::hardware_concurrency();
            config_.processors = n == 0 ? 1 : (n > 64 ? 64 : n);
        }
        command_a_ = config_.command_line;
        command_a_.push_back('\0');
        command_w_ = crt_detail::utf16(config_.command_line);
        command_w_.push_back(u'\0');
    }
    Crt(const Crt&) = delete;
    Crt& operator=(const Crt&) = delete;

    // Called by ExitProcess / TerminateProcess(current) with the exit code; the calling guest
    // thread then unwinds out (`teb::begin_thread_exit`). The embedder stops the other threads.
    std::function<void(std::uint32_t)> on_exit;
    // OutputDebugString text (UTF-8); default: ignored.
    std::function<void(const std::string&)> on_debug_string;
    [[nodiscard]] bool exited() const noexcept { return exited_; }
    [[nodiscard]] std::uint32_t exit_code() const noexcept { return exit_code_; }
    [[nodiscard]] unsigned processors() const noexcept { return config_.processors; }

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* name, Runtime::ImportShim shim) {
            for (const char* dll : {"KERNEL32.dll", "KERNELBASE.dll"}) {
                n += rt_.set_import(dll, name, shim);
            }
        };
        using crt_detail::get;
        using crt_detail::put;
        // RtlEncodePointer: rotate right by the cookie's low 6 bits after XOR with the cookie.
        bind("EncodePointer", [this](CpuState& s) {
            win64_return(s, crt_detail::rotr(win64_arg(s, 0) ^ cookie_, static_cast<unsigned>(cookie_ & 63)));
        });
        bind("DecodePointer", [this](CpuState& s) {
            win64_return(s, crt_detail::rotl(win64_arg(s, 0), static_cast<unsigned>(cookie_ & 63)) ^ cookie_);
        });
        bind("IsDebuggerPresent", [](CpuState& s) { win64_return(s, 0); });
        bind("IsProcessorFeaturePresent", [](CpuState& s) {
            win64_return(s, feature_present(static_cast<std::uint32_t>(win64_arg(s, 0))) ? 1 : 0);
        });
        bind("SetUnhandledExceptionFilter", [this](CpuState& s) {
            win64_return(s, top_filter_.exchange(win64_arg(s, 0)));
        });
        // UnhandledExceptionFilter(EXCEPTION_POINTERS*): runs the top-level filter, if any.
        bind("UnhandledExceptionFilter", [this](CpuState& s) {
            const std::uint64_t filter = top_filter_.load();
            if (filter == 0) {
                win64_return(s, 0);  // EXCEPTION_CONTINUE_SEARCH
                return;
            }
            const std::uint64_t r = rt_.call_guest(s, filter, {win64_arg(s, 0)});
            if (s.unwind_rsp == 0) {
                win64_return(s, r);
            }
        });
        bind("InitOnceExecuteOnce", [this](CpuState& s) { init_once(s); });
        bind("GetCommandLineA", [this](CpuState& s) { win64_return(s, reinterpret_cast<std::uintptr_t>(command_a_.data())); });
        bind("GetCommandLineW", [this](CpuState& s) { win64_return(s, reinterpret_cast<std::uintptr_t>(command_w_.data())); });
        bind("GetStartupInfoW", [](CpuState& s) {
            const std::uint64_t si = win64_arg(s, 0);
            std::memset(reinterpret_cast<void*>(si), 0, 104);
            put<std::uint32_t>(si, 104);  // cb
            win64_return(s, 0);
        });
        bind("GetEnvironmentStringsW", [this](CpuState& s) { win64_return(s, environment_block()); });
        bind("FreeEnvironmentStringsW", [this](CpuState& s) {
            std::lock_guard<std::mutex> lock(m_);
            const auto it = blocks_.find(win64_arg(s, 0));
            const bool ok = it != blocks_.end();
            if (ok) {
                blocks_.erase(it);
            }
            win64_return(s, ok ? 1 : 0);
        });
        bind("SetEnvironmentVariableA", [this](CpuState& s) {
            const std::uint64_t name = win64_arg(s, 0);
            const std::uint64_t value = win64_arg(s, 1);
            set_env(s, name != 0, crt_detail::guest_string(name), value != 0, crt_detail::guest_string(value));
        });
        bind("SetEnvironmentVariableW", [this](CpuState& s) {
            const std::uint64_t name = win64_arg(s, 0);
            const std::uint64_t value = win64_arg(s, 1);
            set_env(s, name != 0, crt_detail::utf8(crt_detail::guest_wstring(name)), value != 0,
                    crt_detail::utf8(crt_detail::guest_wstring(value)));
        });
        bind("ExitProcess", [this](CpuState& s) { exit(s, static_cast<std::uint32_t>(win64_arg(s, 0))); });
        bind("TerminateProcess", [this](CpuState& s) {
            if (win64_arg(s, 0) != teb::kCurrentProcess) {
                teb::set_error(s, 5);  // ERROR_ACCESS_DENIED: no other processes exist
                win64_return(s, 0);
                return;
            }
            exit(s, static_cast<std::uint32_t>(win64_arg(s, 1)));
        });
        bind("OutputDebugStringA", [this](CpuState& s) {
            if (on_debug_string) on_debug_string(crt_detail::guest_string(win64_arg(s, 0)));
            win64_return(s, 0);
        });
        bind("OutputDebugStringW", [this](CpuState& s) {
            if (on_debug_string) on_debug_string(crt_detail::utf8(crt_detail::guest_wstring(win64_arg(s, 0))));
            win64_return(s, 0);
        });
        bind("FlushProcessWriteBuffers", [](CpuState& s) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            win64_return(s, 0);
        });
        bind("GetSystemInfo", [this](CpuState& s) {
            system_info(win64_arg(s, 0));
            win64_return(s, 0);
        });
        bind("GetNativeSystemInfo", [this](CpuState& s) {
            system_info(win64_arg(s, 0));
            win64_return(s, 0);
        });
        bind("GetLogicalProcessorInformation", [this](CpuState& s) { logical_processors(s); });
        bind("GetProcessAffinityMask", [this](CpuState& s) {
            if (const std::uint64_t p = win64_arg(s, 1)) put<std::uint64_t>(p, mask());
            if (const std::uint64_t p = win64_arg(s, 2)) put<std::uint64_t>(p, mask());
            win64_return(s, 1);
        });
        bind("GetVersionExW", [](CpuState& s) {
            const std::uint64_t v = win64_arg(s, 0);
            const auto size = get<std::uint32_t>(v);
            if (size != 276 && size != 284) {  // OSVERSIONINFOW / OSVERSIONINFOEXW
                teb::set_error(s, kErrorInsufficientBuffer);
                win64_return(s, 0);
                return;
            }
            std::memset(reinterpret_cast<void*>(v + 4), 0, size - 4);
            put<std::uint32_t>(v + 4, 6);      // major
            put<std::uint32_t>(v + 8, 2);      // minor
            put<std::uint32_t>(v + 12, 9200);  // build
            put<std::uint32_t>(v + 16, 2);     // VER_PLATFORM_WIN32_NT
            if (size == 284) {
                put<std::uint8_t>(v + 282, 1);  // wProductType VER_NT_WORKSTATION
            }
            win64_return(s, 1);
        });
        // RtlGetVersion ignores the manifest: Windows 10 22H2 (10.0.19045). Checked on Windows
        // 10.0.19045: it never fails whatever dwOSVersionInfoSize says, writes the version,
        // platform and an empty szCSDVersion (its first WCHAR only), and the service pack, suite
        // (VER_SUITE_SINGLEUSERTS) and product type only when the size is exactly 284; wReserved
        // is never written. v1.1.5.6 finds it with GetProcAddress and calls the result without a
        // null check.
        n += rt_.set_import("ntdll.dll", "RtlGetVersion", [](CpuState& s) {
            const std::uint64_t v = win64_arg(s, 0);
            const auto size = get<std::uint32_t>(v);
            put<std::uint32_t>(v + 4, 10);      // major
            put<std::uint32_t>(v + 8, 0);       // minor
            put<std::uint32_t>(v + 12, 19045);  // build
            put<std::uint32_t>(v + 16, 2);      // VER_PLATFORM_WIN32_NT
            put<std::uint16_t>(v + 20, 0);      // szCSDVersion = L""
            if (size == 284) {
                put<std::uint16_t>(v + 276, 0);      // wServicePackMajor
                put<std::uint16_t>(v + 278, 0);      // wServicePackMinor
                put<std::uint16_t>(v + 280, 0x100);  // wSuiteMask
                put<std::uint8_t>(v + 282, 1);       // wProductType VER_NT_WORKSTATION
            }
            win64_return(s, 0);  // STATUS_SUCCESS
        });
        bind("IsWow64Process", [](CpuState& s) {
            if (const std::uint64_t p = win64_arg(s, 1)) put<std::uint32_t>(p, 0);
            win64_return(s, 1);
        });
        bind("GetNumaHighestNodeNumber", [](CpuState& s) {
            if (const std::uint64_t p = win64_arg(s, 0)) put<std::uint32_t>(p, 0);
            win64_return(s, 1);
        });
        bind("GetCurrentProcessorNumber", [](CpuState& s) { win64_return(s, 0); });
        bind("GlobalMemoryStatusEx", [this](CpuState& s) {
            // MEMORYSTATUSEX (64 bytes): dwLength, dwMemoryLoad, then the totals and the free amounts.
            const std::uint64_t m = win64_arg(s, 0);
            if (get<std::uint32_t>(m) != 64) {
                teb::set_error(s, teb::kErrorInvalidParameter);
                win64_return(s, 0);
                return;
            }
            const std::uint64_t total = config_.memory_bytes;
            put<std::uint32_t>(m + 4, 50);
            put<std::uint64_t>(m + 8, total);
            put<std::uint64_t>(m + 16, total / 2);
            put<std::uint64_t>(m + 24, total * 2);
            put<std::uint64_t>(m + 32, total);
            put<std::uint64_t>(m + 40, 0x7FFFFFFE0000ULL);  // user address space
            put<std::uint64_t>(m + 48, 0x7FF000000000ULL);
            put<std::uint64_t>(m + 56, 0);
            win64_return(s, 1);
        });
        bind("SetProcessWorkingSetSize", [](CpuState& s) { win64_return(s, 1); });
        return n;
    }

    // PF_* features consistent with the CPU profile of `Runtime::hook_cpuid`.
    static bool feature_present(std::uint32_t f) noexcept {
        switch (f) {
        case 2:   // PF_COMPARE_EXCHANGE_DOUBLE
        case 3:   // PF_MMX_INSTRUCTIONS_AVAILABLE
        case 6:   // PF_XMMI_INSTRUCTIONS_AVAILABLE (SSE)
        case 8:   // PF_RDTSC_INSTRUCTION_AVAILABLE
        case 10:  // PF_XMMI64_INSTRUCTIONS_AVAILABLE (SSE2)
        case 12:  // PF_NX_ENABLED
        case 13:  // PF_SSE3_INSTRUCTIONS_AVAILABLE
        case 14:  // PF_COMPARE_EXCHANGE128
        case 23:  // PF_FASTFAIL_AVAILABLE
        case 36:  // PF_SSSE3_INSTRUCTIONS_AVAILABLE
        case 37:  // PF_SSE4_1_INSTRUCTIONS_AVAILABLE
            return true;
        default:  // SSE4.2, AVX, AVX2, AVX-512, ... (the profile advertises none)
            return false;
        }
    }

private:
    [[nodiscard]] std::uint64_t mask() const noexcept {
        return config_.processors >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << config_.processors) - 1;
    }

    // SYSTEM_INFO (48 bytes).
    void system_info(std::uint64_t si) const {
        using crt_detail::put;
        std::memset(reinterpret_cast<void*>(si), 0, 48);
        put<std::uint16_t>(si + 0, 9);                      // PROCESSOR_ARCHITECTURE_AMD64
        put<std::uint32_t>(si + 4, 4096);                   // dwPageSize
        put<std::uint64_t>(si + 8, 0x10000);                // lpMinimumApplicationAddress
        put<std::uint64_t>(si + 16, 0x7FFFFFFEFFFFULL);     // lpMaximumApplicationAddress
        put<std::uint64_t>(si + 24, mask());                // dwActiveProcessorMask
        put<std::uint32_t>(si + 32, config_.processors);    // dwNumberOfProcessors
        put<std::uint32_t>(si + 36, 8664);                  // PROCESSOR_AMD_X8664
        put<std::uint32_t>(si + 40, 65536);                 // dwAllocationGranularity
        put<std::uint16_t>(si + 44, 6);                     // wProcessorLevel
        put<std::uint16_t>(si + 46, 0);
    }

    // GetLogicalProcessorInformation(buffer, *length): one RelationProcessorCore entry (32 bytes)
    // per processor.
    void logical_processors(CpuState& s) const {
        using crt_detail::get;
        using crt_detail::put;
        const std::uint64_t buf = win64_arg(s, 0);
        const std::uint64_t len = win64_arg(s, 1);
        const std::uint32_t need = 32 * config_.processors;
        if (len == 0) {
            teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, 0);
            return;
        }
        if (buf == 0 || get<std::uint32_t>(len) < need) {
            put<std::uint32_t>(len, need);
            teb::set_error(s, kErrorInsufficientBuffer);
            win64_return(s, 0);
            return;
        }
        std::memset(reinterpret_cast<void*>(buf), 0, need);
        for (unsigned i = 0; i < config_.processors; ++i) {
            put<std::uint64_t>(buf + 32ULL * i, std::uint64_t{1} << i);  // ProcessorMask
            put<std::uint32_t>(buf + 32ULL * i + 8, 0);                  // RelationProcessorCore
        }
        put<std::uint32_t>(len, need);
        win64_return(s, 1);
    }

    // INIT_ONCE: 0 = not run, 1 = running, 2 = done (other bits free for the context: unused).
    void init_once(CpuState& s) {
        const std::uint64_t once = win64_arg(s, 0);
        const std::uint64_t fn = win64_arg(s, 1);
        const std::uint64_t param = win64_arg(s, 2);
        const std::uint64_t ctx = win64_arg(s, 3);
        auto* word = reinterpret_cast<std::uint64_t*>(static_cast<std::uintptr_t>(once));
        while (true) {
            std::uint64_t cur = __atomic_load_n(word, __ATOMIC_ACQUIRE);
            if ((cur & 3) == 2) {
                if (ctx != 0) crt_detail::put<std::uint64_t>(ctx, 0);
                win64_return(s, 1);
                return;
            }
            std::uint64_t expected = 0;
            if ((cur & 3) == 0 &&
                __atomic_compare_exchange_n(word, &expected, std::uint64_t{1}, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                bool ok = true;
                const std::uint64_t r = rt_.call_guest(s, fn, {once, param, ctx}, &ok);
                if (s.unwind_rsp != 0) {
                    __atomic_store_n(word, std::uint64_t{0}, __ATOMIC_RELEASE);
                    return;
                }
                const bool success = ok && (r & 0xFFFFFFFF) != 0;
                __atomic_store_n(word, success ? std::uint64_t{2} : std::uint64_t{0}, __ATOMIC_RELEASE);
                win64_return(s, success ? 1 : 0);
                return;
            }
            std::this_thread::yield();  // another thread is running the callback
        }
    }

    // A fresh "NAME=VALUE\0...\0\0" UTF-16 block (FreeEnvironmentStringsW releases it).
    std::uint64_t environment_block() {
        std::u16string block;
        std::lock_guard<std::mutex> lock(m_);
        for (const auto& [k, v] : config_.environment) {
            block += crt_detail::utf16(k + "=" + v);
            block.push_back(u'\0');
        }
        block.push_back(u'\0');
        if (block.size() == 1) {
            block.push_back(u'\0');
        }
        auto mem = std::make_unique<char16_t[]>(block.size());
        std::memcpy(mem.get(), block.data(), block.size() * 2);
        const auto va = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mem.get()));
        blocks_[va] = std::move(mem);
        return va;
    }

    // Checked on Windows: NULL or empty lpName fails with ERROR_INVALID_PARAMETER (87); deleting
    // a variable that does not exist (lpValue == NULL) returns TRUE and leaves LastError untouched.
    void set_env(CpuState& s, bool has_name, const std::string& name, bool set, const std::string& value) {
        if (!has_name || name.empty()) {
            teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, 0);
            return;
        }
        std::lock_guard<std::mutex> lock(m_);
        auto same = [&](const std::string& k) {
            if (k.size() != name.size()) return false;
            for (std::size_t i = 0; i < k.size(); ++i) {
                const auto a = static_cast<unsigned char>(k[i]);
                const auto b = static_cast<unsigned char>(name[i]);
                if ((a >= 'a' && a <= 'z' ? a - 32 : a) != (b >= 'a' && b <= 'z' ? b - 32 : b)) return false;
            }
            return true;
        };
        auto it = config_.environment.begin();
        for (; it != config_.environment.end() && !same(it->first); ++it) {
        }
        if (!set) {
            if (it != config_.environment.end()) {
                config_.environment.erase(it);
            }
        } else if (it != config_.environment.end()) {
            it->second = value;
        } else {
            config_.environment[name] = value;
        }
        win64_return(s, 1);
    }

    void exit(CpuState& s, std::uint32_t code) {
        exit_code_ = code;
        exited_ = true;
        if (on_exit) {
            on_exit(code);
        }
        teb::begin_thread_exit(s, code);
    }

    Runtime& rt_;
    ProcessConfig config_;
    std::uint64_t cookie_ = 0;
    std::atomic<std::uint64_t> top_filter_{0};
    std::string command_a_;
    std::u16string command_w_;
    std::mutex m_;
    std::map<std::uint64_t, std::unique_ptr<char16_t[]>> blocks_;
    std::atomic<bool> exited_{false};
    std::atomic<std::uint32_t> exit_code_{0};
};

}  // namespace wse::recomp::shim
