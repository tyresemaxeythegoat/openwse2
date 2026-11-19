// The small system DLLs for translated code (Phase 5 R4, `docs/RECOMP_PLAN.md` "R4 design"; each
// has 1-8 imports in v1.1.5.6):
//
//   dbghelp   SymInitialize SymCleanup StackWalk64 SymFunctionTableAccess64 SymGetModuleBase64
//             SymGetSymFromAddr64 EnumerateLoadedModules64 MiniDumpWriteDump
//   ole32     CoInitializeEx CoUninitialize CoInitializeSecurity CoCreateInstance CoSetProxyBlanket
//   OLEAUT32  SysAllocString(#2) SysFreeString(#6) VariantClear(#9) SysStringByteLen(#149)
//             SysAllocStringByteLen(#150)   (imported by ordinal)
//   SHLWAPI   PathRemoveExtensionA PathStripPathA UrlEscapeA UrlUnescapeA
//   SHELL32   ShellExecuteA                   PSAPI GetProcessMemoryInfo
//   XINPUT1_3 XInputGetState(#2) XInputSetState(#3) XInputEnable(#5)
//   KERNEL32  CreateProcessA WriteProcessMemory FreeLibraryWhenCallbackReturns
//
// Stack walks and crash dumps report "unavailable" (the symbol functions fail with
// ERROR_NOT_SUPPORTED); COM starts but creates no objects (REGDB_E_CLASSNOTREG); BSTRs are real
// (length prefix, terminator); no controller is connected; no other program can be started
// (ShellExecuteA and CreateProcessA are reported to the embedder and fail). Header-only.

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/shim_crt.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::shim {

inline constexpr std::uint32_t kErrorNotSupported = 50, kErrorDeviceNotConnected = 1167;
inline constexpr std::uint32_t kRegdbEClassNotReg = 0x80040154U, kRpcEChangedMode = 0x80010106U, kSFalse = 1;
inline constexpr std::uint32_t kEPointer = 0x80004003U, kEInvalidArg = 0x80070057U;
inline constexpr std::uint32_t kSeErrNoAssoc = 31;

class SystemDlls {
public:
    explicit SystemDlls(Runtime& rt) : rt_(rt) {}
    SystemDlls(const SystemDlls&) = delete;
    SystemDlls& operator=(const SystemDlls&) = delete;

    // ShellExecuteA / CreateProcessA: what the program tried to start.
    std::function<void(const std::string&)> on_launch;

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* dll, const std::string& name, Runtime::ImportShim shim) { n += rt_.set_import(dll, name, shim); };
        using crt_detail::get;
        using crt_detail::put;
        auto fails = [](std::uint32_t e) {
            return [e](CpuState& s) {
                teb::set_error(s, e);
                win64_return(s, 0);
            };
        };

        // dbghelp
        bind("dbghelp.dll", "SymInitialize", [](CpuState& s) { win64_return(s, 1); });
        bind("dbghelp.dll", "SymCleanup", [](CpuState& s) { win64_return(s, 1); });
        bind("dbghelp.dll", "EnumerateLoadedModules64", [](CpuState& s) { win64_return(s, 1); });
        for (const char* f : {"StackWalk64", "SymGetSymFromAddr64", "MiniDumpWriteDump"}) {
            bind("dbghelp.dll", f, fails(kErrorNotSupported));
        }
        bind("dbghelp.dll", "SymFunctionTableAccess64", [](CpuState& s) { win64_return(s, 0); });
        bind("dbghelp.dll", "SymGetModuleBase64", [](CpuState& s) { win64_return(s, 0); });

        // ole32: the first CoInitializeEx on a thread succeeds, later ones return S_FALSE.
        bind("ole32.dll", "CoInitializeEx", [](CpuState& s) {
            thread_local int depth = 0;
            win64_return(s, depth++ == 0 ? 0 : kSFalse);
        });
        bind("ole32.dll", "CoUninitialize", [](CpuState& s) { win64_return(s, 0); });
        bind("ole32.dll", "CoInitializeSecurity", [](CpuState& s) { win64_return(s, 0); });
        // CoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv)
        bind("ole32.dll", "CoCreateInstance", [](CpuState& s) {
            const std::uint64_t ppv = win64_arg(s, 4);
            if (ppv == 0) return win64_return(s, kEPointer);
            put<std::uint64_t>(ppv, 0);
            win64_return(s, kRegdbEClassNotReg);
        });
        bind("ole32.dll", "CoSetProxyBlanket", [](CpuState& s) { win64_return(s, 0); });

        // OLEAUT32 BSTRs: [u32 byte length][UTF-16 chars][u16 0], the BSTR points at the chars.
        bind("OLEAUT32.dll", "#2", [this](CpuState& s) {  // SysAllocString(const OLECHAR*)
            const std::uint64_t p = win64_arg(s, 0);
            if (p == 0) return win64_return(s, 0);
            const std::u16string w = crt_detail::guest_wstring(p);
            win64_return(s, alloc_bstr(reinterpret_cast<const std::uint8_t*>(w.data()), w.size() * 2));
        });
        bind("OLEAUT32.dll", "#150", [this](CpuState& s) {  // SysAllocStringByteLen(LPCSTR, UINT)
            const std::uint64_t p = win64_arg(s, 0);
            const auto len = static_cast<std::uint32_t>(win64_arg(s, 1));
            std::string bytes(len, '\0');
            if (p != 0) std::memcpy(bytes.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(p)), len);
            win64_return(s, alloc_bstr(reinterpret_cast<const std::uint8_t*>(bytes.data()), len));
        });
        bind("OLEAUT32.dll", "#6", [this](CpuState& s) {  // SysFreeString(BSTR)
            free_bstr(win64_arg(s, 0));
            win64_return(s, 0);
        });
        bind("OLEAUT32.dll", "#149", [](CpuState& s) {  // SysStringByteLen(BSTR)
            const std::uint64_t b = win64_arg(s, 0);
            win64_return(s, b == 0 ? 0 : get<std::uint32_t>(b - 4));
        });
        bind("OLEAUT32.dll", "#9", [this](CpuState& s) {  // VariantClear(VARIANTARG*): vt at 0, value at 8
            const std::uint64_t v = win64_arg(s, 0);
            if (v == 0) return win64_return(s, kEInvalidArg);
            if (get<std::uint16_t>(v) == 8) free_bstr(get<std::uint64_t>(v + 8));  // VT_BSTR
            put<std::uint16_t>(v, 0);  // VT_EMPTY
            win64_return(s, 0);
        });

        // SHLWAPI
        bind("SHLWAPI.dll", "PathRemoveExtensionA", [](CpuState& s) {
            const std::uint64_t p = win64_arg(s, 0);
            if (p == 0) return win64_return(s, 0);
            std::string path = crt_detail::guest_string(p);
            const std::size_t slash = path.find_last_of("\\/");
            const std::size_t dot = path.rfind('.');
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) put<char>(p + dot, 0);
            win64_return(s, 0);
        });
        bind("SHLWAPI.dll", "PathStripPathA", [](CpuState& s) {
            const std::uint64_t p = win64_arg(s, 0);
            if (p == 0) return win64_return(s, 0);
            std::string path = crt_detail::guest_string(p);
            std::size_t slash = path.find_last_of("\\/");
            if (slash != std::string::npos && slash + 1 == path.size()) {  // "dir\" keeps "dir\"
                slash = path.find_last_of("\\/", slash - (slash > 0 ? 1 : 0));
            }
            if (slash != std::string::npos) {
                const std::string rest = path.substr(slash + 1);
                std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p)), rest.c_str(), rest.size() + 1);
            }
            win64_return(s, 0);
        });
        // UrlEscapeA(pszUrl, pszEscaped, pcchEscaped, dwFlags) / UrlUnescapeA(pszUrl, pszUnescaped, pcchUnescaped, dwFlags)
        bind("SHLWAPI.dll", "UrlEscapeA", [](CpuState& s) {
            url_result(s, escape(crt_detail::guest_string(win64_arg(s, 0))), win64_arg(s, 1), win64_arg(s, 2), false);
        });
        bind("SHLWAPI.dll", "UrlUnescapeA", [](CpuState& s) {
            const bool in_place = (static_cast<std::uint32_t>(win64_arg(s, 3)) & 0x00100000U) != 0;  // URL_UNESCAPE_INPLACE
            url_result(s, unescape(crt_detail::guest_string(win64_arg(s, 0))), in_place ? win64_arg(s, 0) : win64_arg(s, 1),
                       win64_arg(s, 2), in_place);
        });

        // SHELL32 / KERNEL32: no other program runs here.
        bind("SHELL32.dll", "ShellExecuteA", [this](CpuState& s) {
            if (on_launch) on_launch(crt_detail::guest_string(win64_arg(s, 1)) + " " + crt_detail::guest_string(win64_arg(s, 2)));
            win64_return(s, kSeErrNoAssoc);
        });
        bind("KERNEL32.dll", "CreateProcessA", [this](CpuState& s) {
            if (on_launch) on_launch(crt_detail::guest_string(win64_arg(s, 0)) + " " + crt_detail::guest_string(win64_arg(s, 1)));
            teb::set_error(s, kErrorNotSupported);
            win64_return(s, 0);
        });
        bind("KERNEL32.dll", "WriteProcessMemory", fails(kErrorNotSupported));
        bind("KERNEL32.dll", "FreeLibraryWhenCallbackReturns", [](CpuState& s) { win64_return(s, 0); });  // modules never unload

        // PSAPI GetProcessMemoryInfo(hProcess, PROCESS_MEMORY_COUNTERS*, cb): zeros, cb filled in.
        bind("PSAPI.DLL", "GetProcessMemoryInfo", [](CpuState& s) {
            const std::uint64_t pmc = win64_arg(s, 1);
            const auto cb = static_cast<std::uint32_t>(win64_arg(s, 2));
            if (pmc == 0 || cb < 72) {
                teb::set_error(s, teb::kErrorInvalidParameter);
                return win64_return(s, 0);
            }
            std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(pmc)), 0, 72);
            put<std::uint32_t>(pmc, 72);
            win64_return(s, 1);
        });

        // XINPUT1_3: no controller.
        bind("XINPUT1_3.dll", "#2", [](CpuState& s) { win64_return(s, kErrorDeviceNotConnected); });  // XInputGetState
        bind("XINPUT1_3.dll", "#3", [](CpuState& s) { win64_return(s, kErrorDeviceNotConnected); });  // XInputSetState
        bind("XINPUT1_3.dll", "#5", [](CpuState& s) { win64_return(s, 0); });                         // XInputEnable
        return n;
    }

private:
    std::uint64_t alloc_bstr(const std::uint8_t* bytes, std::size_t len) {
        auto* p = static_cast<std::uint8_t*>(std::malloc(4 + len + 2));
        if (p == nullptr) return 0;
        const auto n = static_cast<std::uint32_t>(len);
        std::memcpy(p, &n, 4);
        if (len != 0) std::memcpy(p + 4, bytes, len);
        p[4 + len] = 0;
        p[5 + len] = 0;
        const auto b = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p + 4));
        const std::lock_guard<std::mutex> lock(m_);
        bstrs_.insert(b);
        return b;
    }
    void free_bstr(std::uint64_t b) {
        if (b == 0) return;
        {
            const std::lock_guard<std::mutex> lock(m_);
            if (bstrs_.erase(b) == 0) return;  // not ours: ignore rather than corrupt the heap
        }
        std::free(reinterpret_cast<void*>(static_cast<std::uintptr_t>(b - 4)));
    }
    // RFC 3986 unreserved characters and the URL delimiters stay; everything else becomes %XX.
    static std::string escape(const std::string& in) {
        static constexpr char kHex[] = "0123456789ABCDEF";
        std::string out;
        for (const char ch : in) {
            const auto c = static_cast<unsigned char>(ch);
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                std::strchr("-_.~/:?#[]@!$&'()*+,;=%", c) != nullptr) {
                out.push_back(ch);
            } else {
                out.push_back('%');
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 15]);
            }
        }
        return out;
    }
    static std::string unescape(const std::string& in) {
        std::string out;
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
                out.push_back(static_cast<char>(std::strtoul(in.substr(i + 1, 2).c_str(), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(in[i]);
            }
        }
        return out;
    }
    // Writes a URL result: S_OK with the length (excluding the terminator), or E_POINTER with the
    // size needed (including it) when the buffer is too small.
    static void url_result(CpuState& s, const std::string& text, std::uint64_t buf, std::uint64_t pcch, bool in_place) {
        if (pcch == 0 && !in_place) return win64_return(s, kEInvalidArg);
        const std::uint32_t have = pcch != 0 ? crt_detail::get<std::uint32_t>(pcch) : 0xFFFFFFFFU;
        if (!in_place && have <= text.size()) {
            crt_detail::put<std::uint32_t>(pcch, static_cast<std::uint32_t>(text.size() + 1));
            return win64_return(s, kEPointer);
        }
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(buf)), text.c_str(), text.size() + 1);
        if (pcch != 0) crt_detail::put<std::uint32_t>(pcch, static_cast<std::uint32_t>(text.size()));
        win64_return(s, 0);
    }

    Runtime& rt_;
    std::mutex m_;
    std::set<std::uint64_t> bstrs_;
};

}  // namespace wse::recomp::shim
