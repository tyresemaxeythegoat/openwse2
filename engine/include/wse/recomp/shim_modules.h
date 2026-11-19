// Modules and resources for translated code (Phase 5 R4, `docs/RECOMP_PLAN.md` "R4 design",
// slice 2 and family "Modules and resources"; v1.1.5.6 call sites: GetProcAddress 52,
// GetModuleHandleW/A/ExW 37, LoadLibrary* 14, GetModuleFileNameA/W 7, the rest 1-4 each):
//
//   GetModuleHandleA GetModuleHandleW GetModuleHandleExA GetModuleHandleExW GetProcAddress
//   LoadLibraryA LoadLibraryW LoadLibraryExA LoadLibraryExW FreeLibrary DisableThreadLibraryCalls
//   GetModuleFileNameA GetModuleFileNameW RtlPcToFileHeader
//   FindResourceA FindResourceW FindResourceExA FindResourceExW LoadResource LockResource
//   SizeofResource
//
// The module registry of one process. Two kinds of module:
//   translated  a PE image mapped at its base (after `apply_pe_relocations` when it moved) whose
//               code was translated: the exe and its DLLs (`lua5164`). Its handle is its base; its
//               exports, resources and headers are read from the mapped image.
//   system      a DLL served by shims (KERNEL32.dll): its handle is a small fake image (an MZ/PE
//               header and nothing else); `GetProcAddress` returns an address inside it that runs
//               the shim registered as `dll!name` when called (`Runtime::dispatch_fallback`).
//               KERNEL32, KERNELBASE and ntdll are always loaded, and every DLL an image imports;
//               `LoadLibrary` loads another only when shims exist for it. API sets
//               ("api-ms-win-core-synch-l1-2-0") resolve to KERNEL32.
// `link` binds each import of a translated module to the exporting module's code (forwarders
// followed), and `attach_dlls` runs each statically imported DLL's entry point with
// DLL_PROCESS_ATTACH, dependencies first, as the loader does before the exe's entry point.
//
// Deviations (documented in `engine/README.md`): modules never unload (FreeLibrary only counts);
// no DLL_THREAD_ATTACH/DETACH or DLL_PROCESS_DETACH calls; `LoadLibraryEx` data-file flags return
// the ordinary handle; module names compare by file name only (a path's directory is ignored);
// FindResource picks the neutral language, then en-US, then the first present. Header-only.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/pe_image.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/shim_crt.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::shim {

inline constexpr std::uint32_t kDllProcessAttach = 1;
inline constexpr std::uint32_t kErrorResourceHandle = 6;  // ERROR_INVALID_HANDLE (LoadResource, SizeofResource)
inline constexpr std::uint32_t kErrorModNotFound = 126;
inline constexpr std::uint32_t kErrorProcNotFound = 127;
inline constexpr std::uint32_t kErrorResourceDataNotFound = 1812;
inline constexpr std::uint32_t kErrorResourceTypeNotFound = 1813;
inline constexpr std::uint32_t kErrorResourceNameNotFound = 1814;
inline constexpr std::uint32_t kErrorResourceLangNotFound = 1815;
inline constexpr std::uint32_t kModuleHandleExPin = 1;
inline constexpr std::uint32_t kModuleHandleExUnchangedRefcount = 2;
inline constexpr std::uint32_t kModuleHandleExFromAddress = 4;

// A module's name as the loader compares it: the file name after the last `\` or `/`, lower-case,
// with ".dll" appended when it has no extension (a trailing '.' means "no extension" and is
// dropped).
inline std::string canonical_module_name(const std::string& path) {
    std::size_t start = path.find_last_of("\\/");
    start = start == std::string::npos ? 0 : start + 1;
    std::string n = path.substr(start);
    for (char& c : n) {
        c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (!n.empty() && n.back() == '.') {
        n.pop_back();
    } else if (n.find('.') == std::string::npos) {
        n += ".dll";
    }
    return n;
}
inline bool is_api_set_name(const std::string& canonical) {
    return canonical.rfind("api-ms-win-", 0) == 0 || canonical.rfind("ext-ms-win-", 0) == 0;
}

struct Module {
    std::string name;  // canonical (`canonical_module_name`)
    std::string path;  // what GetModuleFileName reports
    std::uint64_t base = 0;
    std::uint64_t size = 0;
    bool translated = false;
    bool is_exe = false;
    bool attached = false;
    int refs = 1;
    // translated
    std::uint64_t entry = 0;  // AddressOfEntryPoint as a VA, 0 when none
    PeExports exports;
    std::vector<PeImport> imports;
    std::vector<std::string> depends;  // canonical names of the DLLs it imports, in import order
    // system
    std::vector<std::string> shim_dlls;  // the DLL spellings whose shims it exports, in order
    std::unique_ptr<std::uint8_t[]> storage;
    std::map<std::string, std::uint64_t> thunks;                          // export name -> VA
    std::map<std::uint64_t, std::pair<std::string, std::string>> thunk_of;  // VA -> (shim DLL, name)
};

class Modules {
public:
    static constexpr std::uint64_t kSystemImageSize = 0x10000;
    static constexpr std::uint64_t kThunkStart = 0x1000;
    static constexpr std::uint64_t kThunkStride = 16;

    explicit Modules(Runtime& rt) : rt_(rt) {
        rt_.dispatch_fallback = [this](CpuState& s, std::uint64_t va) { return call_thunk(s, va); };
        (void)system_module("KERNEL32.dll");
        (void)system_module("KERNELBASE.dll");
        (void)system_module("ntdll.dll");
    }
    ~Modules() { rt_.dispatch_fallback = nullptr; }
    Modules(const Modules&) = delete;
    Modules& operator=(const Modules&) = delete;

    // Where system DLLs report living (GetModuleFileName).
    std::string system_dir = "C:\\Windows\\System32\\";

    // Registers a translated image mapped at `img.image_base` (relocated already, if it moved),
    // `path` being the Windows path it was loaded from. The exe comes first. Reads its exports and
    // imports and names the imports in the runtime. Null with `error` when the image cannot be read.
    Module* add_image(const std::string& path, const PeImage& img, const std::uint8_t* mapped, bool is_exe,
                      std::string* error) {
        auto m = std::make_unique<Module>();
        m->name = canonical_module_name(path);
        m->path = path;
        m->base = img.image_base;
        m->size = img.size_of_image;
        m->translated = true;
        m->is_exe = is_exe;
        m->entry = img.entry_rva != 0 ? img.image_base + img.entry_rva : 0;
        if (!read_pe_exports(mapped, img.size_of_image, m->exports, error) ||
            !read_pe_imports(mapped, img.size_of_image, img.image_base, m->imports, error)) {
            return nullptr;
        }
        for (const PeImport& imp : m->imports) {
            const std::string dep = canonical_module_name(imp.dll);
            if (m->depends.empty() || m->depends.back() != dep) {
                m->depends.push_back(dep);
            }
        }
        rt_.name_imports(m->imports);
        const std::lock_guard<std::recursive_mutex> lock(m_);
        modules_.push_back(std::move(m));
        return modules_.back().get();
    }

    // Binds every translated module's imports from other translated modules to their exports and
    // loads a system module for each other DLL imported. Returns the number of slots bound to
    // translated code; `unresolved` gets "dll!name" for each import a translated module does not
    // export. Call after every `add_image`, then `Runtime::fill_iat`.
    std::size_t link(std::vector<std::string>* unresolved = nullptr) {
        const std::lock_guard<std::recursive_mutex> lock(m_);
        std::size_t n = 0;
        for (std::size_t i = 0; i < modules_.size(); ++i) {
            Module* m = modules_[i].get();
            if (!m->translated) {
                continue;
            }
            for (const PeImport& imp : m->imports) {
                Module* from = find(imp.dll, false);  // an API set finds KERNEL32
                if (from == nullptr) {
                    (void)system_module(imp.dll);
                    continue;
                }
                if (!from->translated) {
                    continue;
                }
                const std::uint64_t va = proc_address(*from, imp.name, imp.name.empty() ? imp.ordinal : 0, 0);
                if (va == 0) {
                    if (unresolved != nullptr) {
                        unresolved->push_back(imp.dll + "!" + (imp.name.empty() ? "#" + std::to_string(imp.ordinal) : imp.name));
                    }
                    continue;
                }
                rt_.bind_import_to(imp.slot_va, va);
                ++n;
            }
        }
        return n;
    }

    // Runs DllMain(base, DLL_PROCESS_ATTACH, non-null: a static load) for every translated DLL the
    // exe depends on, each after its own dependencies. False when an entry point returned FALSE or
    // faulted (`failed` names the DLL).
    bool attach_dlls(CpuState& s, std::string* failed = nullptr) {
        std::vector<Module*> order;
        {
            const std::lock_guard<std::recursive_mutex> lock(m_);
            std::vector<std::string> visiting;
            for (const auto& m : modules_) {
                if (m->is_exe) {
                    post_order(*m, order, visiting);
                }
            }
        }
        for (Module* m : order) {
            if (!attach(s, *m, 1)) {
                if (failed != nullptr) {
                    *failed = m->name;
                }
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] const Module* exe() const {
        const std::lock_guard<std::recursive_mutex> lock(m_);
        for (const auto& m : modules_) {
            if (m->is_exe) {
                return m.get();
            }
        }
        return nullptr;
    }
    // The module whose handle (base) is `h`, or null.
    Module* by_handle(std::uint64_t h) {
        const std::lock_guard<std::recursive_mutex> lock(m_);
        for (const auto& m : modules_) {
            if (m->base == h) {
                return m.get();
            }
        }
        return nullptr;
    }
    // The module containing `va`, or null.
    Module* by_address(std::uint64_t va) {
        const std::lock_guard<std::recursive_mutex> lock(m_);
        for (const auto& m : modules_) {
            if (va >= m->base && va - m->base < m->size) {
                return m.get();
            }
        }
        return nullptr;
    }
    // GetModuleHandle (`load` false) or LoadLibrary (`load` true) by name.
    Module* find(const std::string& name, bool load) {
        const std::lock_guard<std::recursive_mutex> lock(m_);
        const std::string canon = canonical_module_name(name);
        if (is_api_set_name(canon)) {
            return find("KERNEL32.dll", false);
        }
        for (const auto& m : modules_) {
            if (m->name == canon) {
                return m.get();
            }
        }
        if (load && rt_.has_shims_for(canon)) {
            return &system_module(name);
        }
        return nullptr;
    }
    // GetProcAddress: by `name`, or by `ordinal` when `name` is empty. Follows forwarders. 0 when
    // absent.
    std::uint64_t proc_address(Module& m, const std::string& name, std::uint16_t ordinal, int depth) {
        const std::lock_guard<std::recursive_mutex> lock(m_);
        if (!m.translated) {
            if (name.empty()) {  // by ordinal: a shim registered as "#n"
                return ordinal != 0 ? proc_address(m, "#" + std::to_string(ordinal), 0, depth) : 0;
            }
            const auto it = m.thunks.find(name);
            if (it != m.thunks.end()) {
                return it->second;
            }
            for (const std::string& dll : m.shim_dlls) {
                if (rt_.named_shim(dll, name) != nullptr) {
                    return add_thunk(m, dll, name);
                }
            }
            return 0;
        }
        for (const PeExport& e : m.exports.entries) {
            if (name.empty() ? e.ordinal != ordinal : e.name != name) {
                continue;
            }
            if (e.forwarder.empty()) {
                return m.base + e.rva;
            }
            const std::size_t dot = e.forwarder.rfind('.');
            if (dot == std::string::npos || depth >= 8) {
                return 0;
            }
            Module* to = find(e.forwarder.substr(0, dot), true);
            const std::string what = e.forwarder.substr(dot + 1);
            if (to == nullptr) {
                return 0;
            }
            if (!what.empty() && what[0] == '#') {
                return proc_address(*to, {}, static_cast<std::uint16_t>(std::strtoul(what.c_str() + 1, nullptr, 10)),
                                    depth + 1);
            }
            return proc_address(*to, what, 0, depth + 1);
        }
        return 0;
    }

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* name, Runtime::ImportShim shim) {
            n += rt_.set_import("KERNEL32.dll", name, shim);
            n += rt_.set_import("KERNELBASE.dll", name, shim);
        };
        using crt_detail::get;
        using crt_detail::put;
        bind("GetModuleHandleA", [this](CpuState& s) { get_handle(s, narrow(win64_arg(s, 0))); });
        bind("GetModuleHandleW", [this](CpuState& s) { get_handle(s, wide(win64_arg(s, 0))); });
        bind("GetModuleHandleExA", [this](CpuState& s) { get_handle_ex(s, false); });
        bind("GetModuleHandleExW", [this](CpuState& s) { get_handle_ex(s, true); });
        bind("GetProcAddress", [this](CpuState& s) {
            Module* m = handle_or_exe(win64_arg(s, 0));
            const std::uint64_t p = win64_arg(s, 1);
            if (m == nullptr) {
                return fail(s, kErrorModNotFound);
            }
            const std::uint64_t va = p < 0x10000 ? proc_address(*m, {}, static_cast<std::uint16_t>(p), 0)
                                                 : proc_address(*m, crt_detail::guest_string(p), 0, 0);
            if (va == 0) {
                return fail(s, kErrorProcNotFound);
            }
            win64_return(s, va);
        });
        bind("LoadLibraryA", [this](CpuState& s) { load(s, narrow(win64_arg(s, 0)), win64_arg(s, 0) != 0); });
        bind("LoadLibraryW", [this](CpuState& s) { load(s, wide(win64_arg(s, 0)), win64_arg(s, 0) != 0); });
        bind("LoadLibraryExA", [this](CpuState& s) { load(s, narrow(win64_arg(s, 0)), win64_arg(s, 0) != 0); });
        bind("LoadLibraryExW", [this](CpuState& s) { load(s, wide(win64_arg(s, 0)), win64_arg(s, 0) != 0); });
        bind("FreeLibrary", [this](CpuState& s) {
            Module* m = by_handle(win64_arg(s, 0));
            if (m == nullptr) {
                return fail(s, kErrorModNotFound);
            }
            {
                const std::lock_guard<std::recursive_mutex> lock(m_);
                m->refs = m->refs > 1 ? m->refs - 1 : 1;  // modules never unload
            }
            win64_return(s, 1);
        });
        bind("DisableThreadLibraryCalls", [this](CpuState& s) {
            win64_return(s, by_handle(win64_arg(s, 0)) != nullptr ? 1 : 0);
        });
        bind("GetModuleFileNameA", [this](CpuState& s) { file_name(s, false); });
        bind("GetModuleFileNameW", [this](CpuState& s) { file_name(s, true); });
        for (const char* dll : {"KERNEL32.dll", "ntdll.dll"}) {
            n += rt_.set_import(dll, "RtlPcToFileHeader", [this](CpuState& s) {
                const Module* m = by_address(win64_arg(s, 0));
                const std::uint64_t base = m != nullptr ? m->base : 0;
                if (win64_arg(s, 1) != 0) {
                    put<std::uint64_t>(win64_arg(s, 1), base);
                }
                win64_return(s, base);
            });
        }
        // FindResource(hModule, lpName, lpType); FindResourceEx(hModule, lpType, lpName, wLanguage).
        bind("FindResourceA", [this](CpuState& s) {
            find_resource(s, win64_arg(s, 0), key(win64_arg(s, 2), false), key(win64_arg(s, 1), false), -1);
        });
        bind("FindResourceW", [this](CpuState& s) {
            find_resource(s, win64_arg(s, 0), key(win64_arg(s, 2), true), key(win64_arg(s, 1), true), -1);
        });
        bind("FindResourceExA", [this](CpuState& s) {
            find_resource(s, win64_arg(s, 0), key(win64_arg(s, 1), false), key(win64_arg(s, 2), false),
                          language(win64_arg(s, 3)));
        });
        bind("FindResourceExW", [this](CpuState& s) {
            find_resource(s, win64_arg(s, 0), key(win64_arg(s, 1), true), key(win64_arg(s, 2), true),
                          language(win64_arg(s, 3)));
        });
        bind("LoadResource", [this](CpuState& s) {
            const std::uint64_t r = win64_arg(s, 1);
            const Module* m = r != 0 ? by_address(r) : nullptr;
            if (m == nullptr) {
                return fail(s, kErrorResourceHandle);
            }
            win64_return(s, m->base + get<std::uint32_t>(r));  // IMAGE_RESOURCE_DATA_ENTRY.OffsetToData
        });
        bind("LockResource", [](CpuState& s) { win64_return(s, win64_arg(s, 0)); });
        bind("SizeofResource", [this](CpuState& s) {
            const std::uint64_t r = win64_arg(s, 1);
            if (r == 0 || by_address(r) == nullptr) {
                return fail(s, kErrorResourceHandle);
            }
            win64_return(s, get<std::uint32_t>(r + 4));
        });
        return n;
    }

private:
    static void fail(CpuState& s, std::uint32_t error) {
        teb::set_error(s, error);
        win64_return(s, 0);
    }
    static std::string narrow(std::uint64_t va) { return crt_detail::guest_string(va); }
    static std::string wide(std::uint64_t va) { return crt_detail::utf8(crt_detail::guest_wstring(va)); }
    static int language(std::uint64_t w) {
        const auto lang = static_cast<std::uint16_t>(w);
        return lang == 0 ? -1 : lang;  // LANG_NEUTRAL: the thread's choice, as FindResource
    }
    // A resource type or name argument: MAKEINTRESOURCE(id), "#123", or a string.
    static PeResourceKey key(std::uint64_t p, bool wide_chars) {
        PeResourceKey k;
        if (p < 0x10000) {
            k.id = static_cast<std::uint32_t>(p);
            return k;
        }
        k.name = wide_chars ? crt_detail::guest_wstring(p) : crt_detail::utf16(crt_detail::guest_string(p));
        if (!k.name.empty() && k.name[0] == u'#') {
            std::uint32_t id = 0;
            for (std::size_t i = 1; i < k.name.size() && k.name[i] >= u'0' && k.name[i] <= u'9'; ++i) {
                id = id * 10 + static_cast<std::uint32_t>(k.name[i] - u'0');
            }
            k.name.clear();
            k.id = id;
        }
        return k;
    }

    Module* handle_or_exe(std::uint64_t h) { return h == 0 ? const_cast<Module*>(exe()) : by_handle(h); }

    Module& system_module(const std::string& spelled) {
        const std::lock_guard<std::recursive_mutex> lock(m_);
        const std::string canon = canonical_module_name(spelled);
        for (const auto& m : modules_) {
            if (m->name == canon) {
                return *m;
            }
        }
        auto m = std::make_unique<Module>();
        m->name = canon;
        const std::size_t slash = spelled.find_last_of("\\/");
        std::string file = slash == std::string::npos ? spelled : spelled.substr(slash + 1);
        if (file.find('.') == std::string::npos) {
            file += ".dll";
        }
        m->path = system_dir + file;
        m->size = kSystemImageSize;
        if (canon == "kernel32.dll") {
            m->shim_dlls = {"kernel32.dll", "kernelbase.dll"};
        } else if (canon == "kernelbase.dll") {
            m->shim_dlls = {"kernelbase.dll", "kernel32.dll"};
        } else {
            m->shim_dlls = {canon};
        }
        // A 64 KiB-aligned fake image: MZ and PE32+ headers (no sections, no directories), then
        // one 16-byte thunk per export handed out, filled with int3.
        m->storage = std::make_unique<std::uint8_t[]>(2 * kSystemImageSize);
        const auto raw = reinterpret_cast<std::uintptr_t>(m->storage.get());
        m->base = (static_cast<std::uint64_t>(raw) + kSystemImageSize - 1) & ~(kSystemImageSize - 1);
        auto* p = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(m->base));
        std::memset(p, 0xCC, kSystemImageSize);
        std::memset(p, 0, kThunkStart);
        auto put16 = [&](std::size_t off, std::uint16_t v) { std::memcpy(p + off, &v, 2); };
        auto put32 = [&](std::size_t off, std::uint32_t v) { std::memcpy(p + off, &v, 4); };
        put16(0, 0x5A4D);
        put32(0x3C, 0x40);
        put32(0x40, 0x00004550);
        put16(0x44, 0x8664);
        put16(0x54, 0xF0);    // SizeOfOptionalHeader
        put16(0x56, 0x2022);  // EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE | DLL
        put16(0x58, 0x20B);   // PE32+
        std::memcpy(p + 0x58 + 24, &m->base, 8);
        put32(0x58 + 56, static_cast<std::uint32_t>(kSystemImageSize));
        put32(0x58 + 60, static_cast<std::uint32_t>(kThunkStart));
        put32(0x58 + 108, 16);  // NumberOfRvaAndSizes (all empty)
        modules_.push_back(std::move(m));
        return *modules_.back();
    }

    std::uint64_t add_thunk(Module& m, const std::string& dll, const std::string& name) {
        const std::uint64_t va = m.base + kThunkStart + kThunkStride * m.thunks.size();
        if (va + kThunkStride > m.base + kSystemImageSize) {
            return 0;
        }
        m.thunks[name] = va;
        m.thunk_of[va] = {dll, name};
        return va;
    }

    // `Runtime::dispatch_fallback`: a call to a system module's export.
    bool call_thunk(CpuState& s, std::uint64_t va) {
        const Runtime::ImportShim* shim = nullptr;
        {
            const std::lock_guard<std::recursive_mutex> lock(m_);
            for (const auto& m : modules_) {
                if (!m->translated && va >= m->base && va - m->base < m->size) {
                    const auto it = m->thunk_of.find(va);
                    if (it != m->thunk_of.end()) {
                        shim = rt_.named_shim(it->second.first, it->second.second);
                    }
                    break;
                }
            }
        }
        if (shim == nullptr) {
            return false;
        }
        if (rt_.before_import && rt_.before_import(s)) {
            return true;
        }
        (*shim)(s);
        return true;
    }

    void post_order(Module& m, std::vector<Module*>& order, std::vector<std::string>& visiting) {
        visiting.push_back(m.name);
        for (const std::string& dep : m.depends) {
            Module* d = find(dep, false);
            if (d == nullptr || !d->translated || d->is_exe) {
                continue;
            }
            bool seen = false;
            for (const std::string& v : visiting) {
                seen = seen || v == dep;
            }
            for (const Module* o : order) {
                seen = seen || o == d;
            }
            if (!seen) {
                post_order(*d, order, visiting);
            }
        }
        if (!m.is_exe) {
            order.push_back(&m);
        }
    }

    // DllMain(base, DLL_PROCESS_ATTACH, reserved) once per translated DLL.
    bool attach(CpuState& s, Module& m, std::uint64_t reserved) {
        {
            const std::lock_guard<std::recursive_mutex> lock(m_);
            if (m.attached || !m.translated || m.is_exe) {
                return true;
            }
            m.attached = true;  // set first: a DllMain that loads itself again does not recurse
        }
        if (m.entry == 0) {
            return true;
        }
        bool ok = false;
        const std::uint64_t r = rt_.call_guest(s, m.entry, {m.base, kDllProcessAttach, reserved}, &ok);
        return ok && static_cast<std::uint32_t>(r) != 0;
    }

    void get_handle(CpuState& s, const std::string& name) {
        Module* m = win64_arg(s, 0) == 0 ? const_cast<Module*>(exe()) : find(name, false);
        if (m == nullptr) {
            return fail(s, kErrorModNotFound);
        }
        win64_return(s, m->base);
    }
    // GetModuleHandleEx(dwFlags, lpModuleName, *phModule).
    void get_handle_ex(CpuState& s, bool wide_name) {
        const auto flags = static_cast<std::uint32_t>(win64_arg(s, 0));
        const std::uint64_t arg = win64_arg(s, 1);
        const std::uint64_t out = win64_arg(s, 2);
        if (out == 0) {
            return fail(s, teb::kErrorInvalidParameter);
        }
        Module* m = nullptr;
        if ((flags & kModuleHandleExFromAddress) != 0) {
            m = by_address(arg);
        } else if (arg == 0) {
            m = const_cast<Module*>(exe());
        } else {
            m = find(wide_name ? wide(arg) : narrow(arg), false);
        }
        crt_detail::put<std::uint64_t>(out, m != nullptr ? m->base : 0);
        if (m == nullptr) {
            return fail(s, kErrorModNotFound);
        }
        if ((flags & (kModuleHandleExUnchangedRefcount | kModuleHandleExPin)) == 0) {
            const std::lock_guard<std::recursive_mutex> lock(m_);
            ++m->refs;
        }
        win64_return(s, 1);
    }
    void load(CpuState& s, const std::string& name, bool given) {
        if (!given) {
            return fail(s, teb::kErrorInvalidParameter);
        }
        Module* m = find(name, true);
        if (m == nullptr) {
            return fail(s, kErrorModNotFound);
        }
        {
            const std::lock_guard<std::recursive_mutex> lock(m_);
            ++m->refs;
        }
        // A translated DLL nothing imported statically: attached now, with reserved = NULL.
        if (!attach(s, *m, 0)) {
            return fail(s, kErrorModNotFound);
        }
        win64_return(s, m->base);
    }
    // GetModuleFileName(hModule, lpFilename, nSize): a buffer too small gets nSize - 1 characters
    // and the terminator, returns nSize, and sets ERROR_INSUFFICIENT_BUFFER (Windows Vista and later).
    void file_name(CpuState& s, bool wide_chars) {
        const Module* m = handle_or_exe(win64_arg(s, 0));
        const std::uint64_t buf = win64_arg(s, 1);
        const auto size = static_cast<std::uint32_t>(win64_arg(s, 2));
        if (m == nullptr) {
            return fail(s, kErrorModNotFound);
        }
        const std::u16string w = crt_detail::utf16(m->path);
        const std::size_t len = wide_chars ? w.size() : m->path.size();
        if (size == 0) {
            return fail(s, kErrorInsufficientBuffer);
        }
        const std::size_t copy = len < size ? len : size - 1;
        for (std::size_t i = 0; i < copy; ++i) {
            if (wide_chars) {
                crt_detail::put<char16_t>(buf + 2 * i, w[i]);
            } else {
                crt_detail::put<char>(buf + i, m->path[i]);
            }
        }
        if (wide_chars) {
            crt_detail::put<char16_t>(buf + 2 * copy, 0);
        } else {
            crt_detail::put<char>(buf + copy, 0);
        }
        if (len >= size) {
            teb::set_error(s, kErrorInsufficientBuffer);
            return win64_return(s, size);
        }
        win64_return(s, len);
    }
    void find_resource(CpuState& s, std::uint64_t h, const PeResourceKey& type, const PeResourceKey& name, int lang) {
        const Module* m = handle_or_exe(h);
        if (m == nullptr || !m->translated) {
            return fail(s, kErrorResourceDataNotFound);
        }
        PeResourceMiss miss = PeResourceMiss::none;
        const std::uint32_t rva = find_pe_resource(reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(m->base)),
                                                   static_cast<std::size_t>(m->size), type, name, lang, &miss);
        switch (miss) {
        case PeResourceMiss::none: return win64_return(s, m->base + rva);
        case PeResourceMiss::no_directory: return fail(s, kErrorResourceDataNotFound);
        case PeResourceMiss::type: return fail(s, kErrorResourceTypeNotFound);
        case PeResourceMiss::name: return fail(s, kErrorResourceNameNotFound);
        case PeResourceMiss::language: return fail(s, kErrorResourceLangNotFound);
        }
    }

    Runtime& rt_;
    mutable std::recursive_mutex m_;
    std::vector<std::unique_ptr<Module>> modules_;
};

}  // namespace wse::recomp::shim
