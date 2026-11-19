// Host runtime of R1-translated code (`docs/RECOMP_PLAN.md` "R1 design"): the `RuntimeHooks`
// that translated functions call, and the VA -> function dispatcher over the `wse_fn_table`
// each translated module exports. No LLVM: the `recomp` host suite tests it directly.
//
// Faults (an unknown dispatch VA, a callee that returned elsewhere, code the translator could
// not lift, an import with no shim) go to `on_fault`. The default prints the fault and calls
// `std::abort()`: there is no translation during play, so a missing function is a loud error
// with its address. A test may install a handler that records and returns; the runtime then
// sets `rip` to `kFaultRip`, so the caller's return check fails too rather than continuing
// silently.

#pragma once

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/pe_image.h"

namespace wse::recomp {

using GuestFn = void (*)(CpuState*);

// One row of a module's `wse_fn_table` (the layout `R1Translator::emit_object` emits).
struct FnRow {
    std::uint64_t va;
    GuestFn fn;
};
static_assert(sizeof(FnRow) == 16);

enum class FaultKind { missing_function, bad_return, unsupported, missing_import, segment };

struct Fault {
    FaultKind kind = FaultKind::missing_function;
    std::uint64_t va = 0;       // target / expected return / instruction / IAT slot
    std::uint64_t rip = 0;      // state->rip when the fault was raised
    std::string what;           // unsupported: the translator's reason
};

[[nodiscard]] inline const char* fault_name(FaultKind k) noexcept {
    switch (k) {
    case FaultKind::missing_function: return "dispatch to an untranslated VA";
    case FaultKind::bad_return: return "callee returned to the wrong VA";
    case FaultKind::unsupported: return "untranslated instruction";
    case FaultKind::missing_import: return "import with no shim";
    case FaultKind::segment: return "segment base not provided";
    }
    return "fault";
}

class Runtime {
public:
    static constexpr std::uint64_t kFaultRip = 0xFA017FA017FA0170ULL;
    // Segment numbers passed to `segment_base` (x86 encoding of FS and GS).
    static constexpr int kSegFs = 4;
    static constexpr int kSegGs = 5;

    // An import shim runs with the return VA on top of the guest stack, like the real callee;
    // it must pop it into `state->rip` (see `return_from_import`).
    using ImportShim = std::function<void(CpuState&)>;

    Runtime() {
        hooks_.dispatch = &Runtime::hook_dispatch;
        hooks_.import_call = &Runtime::hook_import;
        hooks_.bad_return = &Runtime::hook_bad_return;
        hooks_.unsupported = &Runtime::hook_unsupported;
        hooks_.segment_base = &Runtime::hook_segment;
        hooks_.div128 = &Runtime::hook_div128;
        hooks_.cpuid = &Runtime::hook_cpuid;
        hooks_.context = this;
        on_fault = [](const Fault& f) {
            std::fprintf(stderr, "wse recomp: %s at 0x%llx (rip 0x%llx)%s%s\n", fault_name(f.kind),
                         static_cast<unsigned long long>(f.va), static_cast<unsigned long long>(f.rip),
                         f.what.empty() ? "" : ": ", f.what.c_str());
            std::abort();
        };
    }
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] const RuntimeHooks* hooks() const noexcept { return &hooks_; }

    // Adds a module's table (rows need not be sorted; a VA already present keeps its first fn).
    void add_table(const FnRow* rows, std::size_t count) {
        table_.insert(table_.end(), rows, rows + count);
        std::stable_sort(table_.begin(), table_.end(),
                         [](const FnRow& a, const FnRow& b) { return a.va < b.va; });
        table_.erase(std::unique(table_.begin(), table_.end(),
                                 [](const FnRow& a, const FnRow& b) { return a.va == b.va; }),
                     table_.end());
    }
    [[nodiscard]] std::size_t function_count() const noexcept { return table_.size(); }

    [[nodiscard]] GuestFn find(std::uint64_t va) const noexcept {
        const auto it = std::lower_bound(table_.begin(), table_.end(), va,
                                         [](const FnRow& r, std::uint64_t v) { return r.va < v; });
        return it != table_.end() && it->va == va ? it->fn : nullptr;
    }

    void set_import(std::uint64_t slot_va, ImportShim shim) { imports_[slot_va] = std::move(shim); }

    // Names every IAT slot (`read_pe_imports`), so shims register by name and a missing one is
    // reported as "KERNEL32.dll!TlsAlloc" (R3 import bridge). A slot whose name already has a shim
    // (`set_import` ran first, e.g. for a module added later: R4 `modules.h`) is bound at once.
    void name_imports(const std::vector<PeImport>& imports) {
        for (const PeImport& imp : imports) {
            import_names_[imp.slot_va] = imp.dll + "!" + (imp.name.empty() ? "#" + std::to_string(imp.ordinal) : imp.name);
            import_list_.push_back(imp);
            const auto it = named_.find(shim_key(imp.dll, import_key(imp)));
            if (it != named_.end()) {
                imports_[imp.slot_va] = it->second;
            }
        }
    }
    // Registers `shim` as `dll!name` and binds it to every slot importing it (DLL compared
    // case-insensitively; a `name` of "#n" is the export with ordinal n, for imports by ordinal).
    // Returns the number of slots bound now (0 when no image imports it yet; `name_imports` binds
    // later slots, and `named_shim` finds it for `GetProcAddress`).
    std::size_t set_import(const std::string& dll, const std::string& name, const ImportShim& shim) {
        named_[shim_key(dll, name)] = shim;
        std::size_t n = 0;
        for (const PeImport& imp : import_list_) {
            if (import_key(imp) == name && same_dll(imp.dll, dll)) {
                imports_[imp.slot_va] = shim;
                ++n;
            }
        }
        return n;
    }
    // The shim registered as `dll!name`, or null.
    [[nodiscard]] const ImportShim* named_shim(const std::string& dll, const std::string& name) const {
        const auto it = named_.find(shim_key(dll, name));
        return it == named_.end() ? nullptr : &it->second;
    }
    // True when any shim is registered for `dll` (lower-case comparison).
    [[nodiscard]] bool has_shims_for(const std::string& dll) const {
        const std::string prefix = lower(dll) + "!";
        const auto it = named_.lower_bound(prefix);
        return it != named_.end() && it->first.compare(0, prefix.size(), prefix) == 0;
    }
    // Binds an IAT slot to translated code at `target` (an export of another translated module,
    // R4): the slot holds `target`, and a call through it dispatches there (the return address is
    // already pushed, as for a real call through the IAT).
    void bind_import_to(std::uint64_t slot_va, std::uint64_t target) {
        imports_[slot_va] = [target](CpuState& s) { s.hooks->dispatch(&s, target); };
        iat_values_[slot_va] = target;
    }
    // Makes `va` (host memory no image or table uses) run `shim` when guest code calls it through a
    // pointer: the COM methods of the R4 recording devices (`shim_d3d9.h`), whose vtables hold
    // such addresses. Register before guest threads run: lookups take no lock.
    void add_callable(std::uint64_t va, ImportShim shim) { callables_[va] = std::move(shim); }
    [[nodiscard]] const std::vector<PeImport>& import_list() const noexcept { return import_list_; }
    // Writes every named IAT slot's value into the slot, as the loader writes the imported
    // function's address: the slot's own VA for a shim (code that loads an import into a register
    // and calls it later reaches `dispatch` with the slot VA, which runs the import: R3), or the
    // export's VA for a slot bound to translated code (`bind_import_to`). Guest memory must be
    // mapped at the slot VAs (the image at its base).
    void fill_iat() const {
        for (const PeImport& imp : import_list_) {
            const auto it = iat_values_.find(imp.slot_va);
            const std::uint64_t v = it != iat_values_.end() ? it->second : imp.slot_va;
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(imp.slot_va)), &v, 8);
        }
    }
    [[nodiscard]] std::string import_name(std::uint64_t slot_va) const {
        const auto it = import_names_.find(slot_va);
        return it == import_names_.end() ? std::string() : it->second;
    }
    void set_segment_base(int segment, std::uint64_t base) {
        (segment == kSegFs ? fs_base_ : gs_base_) = base;
        (segment == kSegFs ? fs_set_ : gs_set_) = true;
    }

    // Prepares `state` to run on this runtime.
    void attach(CpuState& state) const noexcept { state.hooks = &hooks_; }

    // Runs the function at `va` as a guest call from outside (the caller has pushed the return
    // VA). Returns false, after a fault, when no function is there.
    bool run(CpuState& state, std::uint64_t va) {
        attach(state);
        GuestFn fn = find(va);
        if (fn == nullptr) {
            fault(state, FaultKind::missing_function, va, {});
            return false;
        }
        fn(&state);
        return true;
    }

    // Pops the return VA into `rip` (an import shim's `ret`; `stack_bytes` as in `ret n`).
    static void return_from_import(CpuState& state, std::uint64_t stack_bytes = 0) noexcept {
        std::uint64_t ret = 0;
        std::memcpy(&ret, reinterpret_cast<const void*>(state.gpr[4]), 8);
        state.gpr[4] += 8 + stack_bytes;
        state.rip = ret;
    }

    // Where a host-to-guest call returns (`call_guest`); never executed.
    static constexpr std::uint64_t kCallbackReturn = 0xCA11BAC000000000ULL;

    // Calls the guest function at `va` from host code (a shim calling a window procedure, a thread
    // starting at its entry point) with up to 16 integer arguments by the Win64 convention: the
    // first four in rcx rdx r8 r9, the rest above a 32-byte shadow area. It runs below the guest's
    // current `rsp` and restores every register but `rax` afterwards, so a shim can call back and
    // then continue. Returns `rax`; `ok` is false when the call faulted.
    std::uint64_t call_guest(CpuState& s, std::uint64_t va, std::initializer_list<std::uint64_t> args,
                             bool* ok = nullptr) {
        const CpuState saved = s;
        const std::size_t n = args.size();
        const std::uint64_t frame = 0x20 + 8 * (n > 4 ? n - 4 : 0);
        std::uint64_t rsp = (s.gpr[4] - 0x40 - frame) & ~std::uint64_t{15};  // 16-aligned at the call
        static constexpr int kArgReg[4] = {1, 2, 8, 9};
        std::size_t i = 0;
        for (const std::uint64_t a : args) {
            if (i < 4) {
                s.gpr[kArgReg[i]] = a;
            } else {
                std::memcpy(reinterpret_cast<void*>(rsp + 0x20 + 8 * (i - 4)), &a, 8);
            }
            ++i;
        }
        rsp -= 8;
        std::memcpy(reinterpret_cast<void*>(rsp), &kCallbackReturn, 8);
        s.gpr[4] = rsp;
        const bool ran = run(s, va) && s.rip == kCallbackReturn;
        if (s.unwind_rsp != 0) {
            // An exception is unwinding past this call (R3): leave the target's context in place
            // and let the host caller return too.
            if (ok != nullptr) {
                *ok = false;
            }
            return 0;
        }
        const std::uint64_t rax = s.gpr[0];
        s = saved;
        s.gpr[0] = rax;
        if (ok != nullptr) {
            *ok = ran;
        }
        return rax;
    }

    std::function<void(const Fault&)> on_fault;
    std::atomic<std::size_t> fault_count{0};  // guest threads fault concurrently (R3 `threads.h`)
    // Runs before every import shim; returning true skips the shim (the state was already set,
    // e.g. a thread that `TerminateThread` marked starts its exit: `threads.h`).
    std::function<bool(CpuState&)> before_import;
    // Consulted when `dispatch` reaches a VA that is neither a translated function nor an import
    // slot (R4 `modules.h`: a system DLL export that `GetProcAddress` returned, called through a
    // pointer). Returns true when it ran the call; false faults as before. Set before guest code
    // runs; it must be safe to call from every guest thread.
    std::function<bool(CpuState&, std::uint64_t)> dispatch_fallback;
    // Called with the IAT slot before every import (bring-up tracing, `tools/r4_run.cpp`); empty in
    // normal runs. It runs on whichever guest thread makes the call.
    std::function<void(std::uint64_t)> trace_import;

private:
    static Runtime& of(CpuState* s) noexcept { return *static_cast<Runtime*>(s->hooks->context); }

    static std::string lower(std::string s) {
        for (char& ch : s) {
            ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        }
        return s;
    }
    static std::string shim_key(const std::string& dll, const std::string& name) { return lower(dll) + "!" + name; }
    static std::string import_key(const PeImport& imp) {
        return imp.name.empty() ? "#" + std::to_string(imp.ordinal) : imp.name;
    }
    static bool same_dll(const std::string& a, const std::string& b) { return lower(a) == lower(b); }

    void fault(CpuState& s, FaultKind kind, std::uint64_t va, std::string what) {
        ++fault_count;
        Fault f;
        f.kind = kind;
        f.va = va;
        f.rip = s.rip;
        f.what = std::move(what);
        on_fault(f);
        s.rip = kFaultRip;
    }

    static void hook_dispatch(CpuState* s, std::uint64_t va) {
        Runtime& rt = of(s);
        GuestFn fn = rt.find(va);
        if (fn == nullptr) {
            // An import called through a register (`mov rsi, [rip + IAT]` ... `call rsi`): the
            // slot holds its own VA (`fill_iat`).
            if (rt.import_names_.count(va) != 0) {
                hook_import(s, va);
                return;
            }
            if (const auto c = rt.callables_.find(va); c != rt.callables_.end()) {
                c->second(*s);
                return;
            }
            if (rt.dispatch_fallback && rt.dispatch_fallback(*s, va)) {
                return;
            }
            rt.fault(*s, FaultKind::missing_function, va, {});
            return;
        }
        fn(s);
    }
    static void hook_import(CpuState* s, std::uint64_t slot_va) {
        Runtime& rt = of(s);
        if (rt.trace_import) {
            rt.trace_import(slot_va);
        }
        if (rt.before_import && rt.before_import(*s)) {
            return;
        }
        const auto it = rt.imports_.find(slot_va);
        if (it != rt.imports_.end()) {
            it->second(*s);
            return;
        }
        rt.fault(*s, FaultKind::missing_import, slot_va, rt.import_name(slot_va));
    }
    static void hook_bad_return(CpuState* s, std::uint64_t expected) {
        Runtime& rt = of(s);
        if (s->rip == kFaultRip) {
            return;  // already reported where it happened
        }
        rt.fault(*s, FaultKind::bad_return, expected, {});
    }
    static void hook_unsupported(CpuState* s, std::uint64_t va, const char* what) {
        of(s).fault(*s, FaultKind::unsupported, va, what != nullptr ? what : "");
    }
    // The CPU translated code is told it runs on: what the R1 lifter supports and nothing more,
    // so code behind a feature check (AVX, FMA, SSE4.2: lifted as faults) is never taken.
    // Vendor "WSE2AArch64 ", family 6; leaf 1 advertises SSE SSE2 SSE3 SSSE3 SSE4.1 POPCNT CMOV
    // FPU; extended leaf 0x80000001 advertises long mode and NX. Every other leaf reads zero.
    static void hook_cpuid(CpuState* s) {
        const auto leaf = static_cast<std::uint32_t>(s->gpr[0]);
        std::uint32_t r[4] = {0, 0, 0, 0};  // eax ebx ecx edx
        switch (leaf) {
        case 0:
            r[0] = 1;  // highest basic leaf
            r[1] = 0x32455357U;  // "WSE2"
            r[3] = 0x63724141U;  // "AArc"
            r[2] = 0x20343668U;  // "h64 "
            break;
        case 1:
            r[0] = 0x00000600U;  // family 6, model 0, stepping 0
            r[1] = 0x00010800U;  // 1 logical processor, CLFLUSH line 8 * 8 bytes
            r[2] = (1U << 0) | (1U << 9) | (1U << 19) | (1U << 23);  // SSE3 SSSE3 SSE4.1 POPCNT
            r[3] = (1U << 0) | (1U << 15) | (1U << 25) | (1U << 26);  // FPU CMOV SSE SSE2
            break;
        case 0x80000000U:
            r[0] = 0x80000001U;
            break;
        case 0x80000001U:
            r[3] = (1U << 20) | (1U << 29);  // NX, long mode
            break;
        default:
            break;
        }
        s->gpr[0] = r[0];
        s->gpr[3] = r[1];
        s->gpr[1] = r[2];
        s->gpr[2] = r[3];
    }
    static int hook_div128(CpuState* s, std::uint64_t divisor, int is_signed) {
        if (divisor == 0) {
            return 0;
        }
        const unsigned __int128 n = (static_cast<unsigned __int128>(s->gpr[2]) << 64) | s->gpr[0];
        if (is_signed != 0) {
            const auto sn = static_cast<__int128>(n);
            const auto sd = static_cast<__int128>(static_cast<std::int64_t>(divisor));
            const __int128 q = sn / sd;
            if (q < INT64_MIN || q > INT64_MAX) {
                return 0;
            }
            s->gpr[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(q));
            s->gpr[2] = static_cast<std::uint64_t>(static_cast<std::int64_t>(sn % sd));
            return 1;
        }
        const unsigned __int128 q = n / divisor;
        if ((q >> 64) != 0) {
            return 0;
        }
        s->gpr[0] = static_cast<std::uint64_t>(q);
        s->gpr[2] = static_cast<std::uint64_t>(n % divisor);
        return 1;
    }
    static std::uint64_t hook_segment(CpuState* s, int segment) {
        Runtime& rt = of(s);
        if (segment == kSegFs && rt.fs_set_) return rt.fs_base_;
        if (segment == kSegGs && s->teb != 0) return s->teb;  // this thread's TEB (R3 `teb.h`)
        if (segment == kSegGs && rt.gs_set_) return rt.gs_base_;
        const std::uint64_t rip = s->rip;
        rt.fault(*s, FaultKind::segment, static_cast<std::uint64_t>(segment), {});
        s->rip = rip;  // the instruction continues with base 0; the fault was the report
        return 0;
    }

    RuntimeHooks hooks_;
    std::vector<FnRow> table_;
    std::map<std::uint64_t, ImportShim> imports_;
    std::map<std::uint64_t, std::string> import_names_;
    std::vector<PeImport> import_list_;
    std::map<std::string, ImportShim> named_;           // "kernel32.dll!TlsAlloc" -> shim
    std::map<std::uint64_t, std::uint64_t> iat_values_;  // slot -> translated export (`bind_import_to`)
    std::map<std::uint64_t, ImportShim> callables_;     // COM method thunks (`add_callable`)
    std::uint64_t fs_base_ = 0;
    std::uint64_t gs_base_ = 0;
    bool fs_set_ = false;
    bool gs_set_ = false;
};

// Win64 arguments as an import shim sees them: `rsp` points at the return address, so arguments 5
// and up start at `rsp + 0x28` (after the 32-byte shadow area).
[[nodiscard]] inline std::uint64_t win64_arg(const CpuState& s, int i) noexcept {
    static constexpr int kArgReg[4] = {1, 2, 8, 9};
    if (i < 4) {
        return s.gpr[kArgReg[i]];
    }
    std::uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(s.gpr[4] + 0x28 + 8 * static_cast<std::uint64_t>(i - 4)), 8);
    return v;
}
[[nodiscard]] inline double win64_arg_f64(const CpuState& s, int i) noexcept {
    double v = 0;
    std::memcpy(&v, s.xmm[i], 8);
    return v;
}
[[nodiscard]] inline float win64_arg_f32(const CpuState& s, int i) noexcept {
    float v = 0;
    std::memcpy(&v, s.xmm[i], 4);
    return v;
}
// Returns from an import shim with `rax` (a shim returning a float writes `xmm0` first).
inline void win64_return(CpuState& s, std::uint64_t rax) noexcept {
    s.gpr[0] = rax;
    Runtime::return_from_import(s);
}

}  // namespace wse::recomp
