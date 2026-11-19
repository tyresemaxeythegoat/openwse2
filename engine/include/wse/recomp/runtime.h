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
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "wse/recomp/cpu_state.h"

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

    void set_import(std::uint64_t slot_va, ImportShim shim) {
        imports_.push_back({slot_va, std::move(shim)});
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

    std::function<void(const Fault&)> on_fault;
    std::size_t fault_count = 0;

private:
    static Runtime& of(CpuState* s) noexcept { return *static_cast<Runtime*>(s->hooks->context); }

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
            rt.fault(*s, FaultKind::missing_function, va, {});
            return;
        }
        fn(s);
    }
    static void hook_import(CpuState* s, std::uint64_t slot_va) {
        Runtime& rt = of(s);
        for (const auto& [slot, shim] : rt.imports_) {
            if (slot == slot_va) {
                shim(*s);
                return;
            }
        }
        rt.fault(*s, FaultKind::missing_import, slot_va, {});
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
        if (segment == kSegGs && rt.gs_set_) return rt.gs_base_;
        const std::uint64_t rip = s->rip;
        rt.fault(*s, FaultKind::segment, static_cast<std::uint64_t>(segment), {});
        s->rip = rip;  // the instruction continues with base 0; the fault was the report
        return 0;
    }

    RuntimeHooks hooks_;
    std::vector<FnRow> table_;
    std::vector<std::pair<std::uint64_t, ImportShim>> imports_;
    std::uint64_t fs_base_ = 0;
    std::uint64_t gs_base_ = 0;
    bool fs_set_ = false;
    bool gs_set_ = false;
};

}  // namespace wse::recomp
