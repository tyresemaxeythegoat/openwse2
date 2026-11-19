// Win64 thread environment for translated code (Phase 5 R3, `docs/RECOMP_PLAN.md` "R3 design"):
// the TEB and PEB in guest memory, thread-local storage, and the KERNEL32 calls built on them.
//
//   gs:            each guest thread's `CpuState::teb` is its TEB; `gs:` reads go through
//                  `RuntimeHooks::segment_base` (v1.1.5.6 reads only `gs:[0x58]` and `gs:[0x10]`)
//   static TLS     the exe's TLS directory: `_tls_index` = 0, every thread gets a copy of the
//                  template plus the zero fill, reached through `ThreadLocalStoragePointer`
//   TLS callbacks  run with DLL_PROCESS_ATTACH / DLL_THREAD_ATTACH / DLL_THREAD_DETACH
//   TlsAlloc TlsFree TlsGetValue TlsSetValue           slots in the TEB (64 + 1024 expansion)
//   FlsAlloc FlsFree FlsGetValue FlsSetValue           per-thread values with callbacks
//   GetLastError SetLastError                          `LastErrorValue` in the TEB
//   GetCurrentThreadId GetCurrentProcessId GetCurrentThread GetCurrentProcess
//
// Guest memory is host memory (translated code dereferences guest addresses directly), so a TEB
// is a host allocation whose address is its guest VA. Offsets are the documented x64 TEB/PEB
// ones (winternl.h / the public NT_TIB layout). Thread creation (`CreateThread`) builds on this
// in the next R3 step. Header-only.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/pe_image.h"
#include "wse/recomp/runtime.h"

namespace wse::recomp::teb {

// TEB (x64).
namespace tebo {
inline constexpr std::size_t stack_base = 0x08, stack_limit = 0x10, self = 0x30, client_pid = 0x40,
                             client_tid = 0x48, tls_pointer = 0x58, peb = 0x60, last_error = 0x68,
                             tls_slots = 0x1480, tls_expansion = 0x1780;
// Past the real TEB (0x1838 bytes on current Windows): the host `GuestThread*`, never read by
// guest code.
inline constexpr std::size_t host_thread = 0x1FF8;
inline constexpr std::size_t size = 0x2000;
}  // namespace tebo
// PEB (x64).
namespace pebo {
inline constexpr std::size_t image_base = 0x10, size = 0x1000;
}

inline constexpr std::uint32_t kTlsMinimumAvailable = 64;     // slots in the TEB itself
inline constexpr std::uint32_t kTlsExpansionSlots = 1024;
inline constexpr std::uint32_t kTlsSlots = kTlsMinimumAvailable + kTlsExpansionSlots;
inline constexpr std::uint32_t kFlsMaximumAvailable = 4080;
inline constexpr std::uint32_t kOutOfIndexes = 0xFFFFFFFFU;   // TLS_OUT_OF_INDEXES / FLS_OUT_OF_INDEXES
inline constexpr std::uint32_t kErrorInvalidParameter = 87;
inline constexpr std::uint32_t kErrorNoMoreItems = 259;
inline constexpr std::uint32_t kDllProcessAttach = 1;
inline constexpr std::uint32_t kDllThreadAttach = 2;
inline constexpr std::uint32_t kDllThreadDetach = 3;
inline constexpr std::uint32_t kProcessId = 0x1000;
inline constexpr std::uint64_t kCurrentProcess = ~std::uint64_t{0};      // (HANDLE)-1
inline constexpr std::uint64_t kCurrentThread = ~std::uint64_t{0} - 1;   // (HANDLE)-2

namespace detail {
struct AlignedFree {
    std::size_t align = 16;
    void operator()(std::uint8_t* p) const noexcept { ::operator delete(p, std::align_val_t{align}); }
};
using Block = std::unique_ptr<std::uint8_t, AlignedFree>;
inline Block zeroed(std::size_t n, std::size_t align) {
    auto* p = static_cast<std::uint8_t*>(::operator new(n == 0 ? 1 : n, std::align_val_t{align}));
    std::memset(p, 0, n == 0 ? 1 : n);
    return Block(p, AlignedFree{align});
}
inline std::uint64_t va(const Block& b) noexcept { return reinterpret_cast<std::uintptr_t>(b.get()); }
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
}  // namespace detail

// One guest thread's environment (owned by `Process`).
struct GuestThread {
    std::uint64_t teb = 0;  // guest VA of the TEB (= `CpuState::teb`)
    std::uint32_t id = 0;   // `ClientId.UniqueThread`
    std::vector<std::uint64_t> fls;  // FLS values by index (grown on demand)
    detail::Block teb_mem;
    detail::Block tls_block;   // static TLS: template + zero fill
    detail::Block tls_array;   // ThreadLocalStoragePointer[] (one module: index 0)
    detail::Block tls_expansion;  // TlsExpansionSlots, allocated on the first set of a slot >= 64
};

class Process {
public:
    // `image` is the exe mapped at `img.image_base` (writable: the loader writes `_tls_index`).
    Process(Runtime& rt, const PeImage& img, std::uint8_t* image) : rt_(rt), img_(img), image_(image) {}
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Reads the TLS directory, writes `_tls_index` (0: the exe is module 0) and builds the PEB.
    bool init(std::string* error) {
        if (!read_pe_tls(image_, img_.size_of_image, img_.image_base, tls_, error)) {
            return false;
        }
        if (tls_.present && tls_.index_va != 0) {
            const std::uint32_t zero = 0;
            std::memcpy(image_ + (tls_.index_va - img_.image_base), &zero, 4);
        }
        peb_ = detail::zeroed(pebo::size, 4096);
        detail::put<std::uint64_t>(detail::va(peb_) + pebo::image_base, img_.image_base);
        return true;
    }

    // Binds the shims to the exe's imports (KERNEL32, and KERNELBASE spellings).
    std::size_t install() {
        std::size_t n = 0;
        for (const char* dll : {"KERNEL32.dll", "KERNELBASE.dll"}) {
            n += rt_.set_import(dll, "TlsAlloc", [this](CpuState& s) { tls_alloc(s); });
            n += rt_.set_import(dll, "TlsFree", [this](CpuState& s) { tls_free(s); });
            n += rt_.set_import(dll, "TlsGetValue", [this](CpuState& s) { tls_get(s); });
            n += rt_.set_import(dll, "TlsSetValue", [this](CpuState& s) { tls_set(s); });
            n += rt_.set_import(dll, "FlsAlloc", [this](CpuState& s) { fls_alloc(s); });
            n += rt_.set_import(dll, "FlsFree", [this](CpuState& s) { fls_free(s); });
            n += rt_.set_import(dll, "FlsGetValue", [this](CpuState& s) { fls_get(s); });
            n += rt_.set_import(dll, "FlsSetValue", [this](CpuState& s) { fls_set(s); });
            n += rt_.set_import(dll, "GetLastError", [this](CpuState& s) {
                if (GuestThread* t = thread_of(s, "GetLastError")) win64_return(s, last_error(*t));
            });
            n += rt_.set_import(dll, "SetLastError", [this](CpuState& s) {
                if (GuestThread* t = thread_of(s, "SetLastError")) {
                    set_last_error(*t, static_cast<std::uint32_t>(win64_arg(s, 0)));
                    win64_return(s, 0);
                }
            });
            n += rt_.set_import(dll, "GetCurrentThreadId", [this](CpuState& s) {
                if (GuestThread* t = thread_of(s, "GetCurrentThreadId")) win64_return(s, t->id);
            });
            n += rt_.set_import(dll, "GetCurrentProcessId", [](CpuState& s) { win64_return(s, kProcessId); });
            n += rt_.set_import(dll, "GetCurrentThread", [](CpuState& s) { win64_return(s, kCurrentThread); });
            n += rt_.set_import(dll, "GetCurrentProcess", [](CpuState& s) { win64_return(s, kCurrentProcess); });
        }
        return n;
    }

    // A new guest thread: its TEB (stack range as given, for `gs:[0x8]`/`gs:[0x10]`) and static
    // TLS block. Attach it to a `CpuState` with `attach`; the TLS callbacks are the caller's
    // (`run_tls_callbacks` with DLL_THREAD_ATTACH, or DLL_PROCESS_ATTACH for the first thread).
    GuestThread& create_thread(std::uint64_t stack_base, std::uint64_t stack_limit) {
        auto t = std::make_unique<GuestThread>();
        t->teb_mem = detail::zeroed(tebo::size, 4096);
        t->teb = detail::va(t->teb_mem);
        const std::uint64_t teb = t->teb;
        detail::put<std::uint64_t>(teb + tebo::stack_base, stack_base);
        detail::put<std::uint64_t>(teb + tebo::stack_limit, stack_limit);
        detail::put<std::uint64_t>(teb + tebo::self, teb);
        detail::put<std::uint64_t>(teb + tebo::client_pid, kProcessId);
        detail::put<std::uint64_t>(teb + tebo::peb, detail::va(peb_));
        if (tls_.present) {
            const std::size_t raw = static_cast<std::size_t>(tls_.raw_end - tls_.raw_begin);
            const std::size_t align = tls_.alignment > 16 ? tls_.alignment : 16;
            t->tls_block = detail::zeroed(raw + tls_.zero_fill, align);
            if (raw != 0) {
                std::memcpy(t->tls_block.get(), image_ + (tls_.raw_begin - img_.image_base), raw);
            }
            t->tls_array = detail::zeroed(8, 16);
            detail::put<std::uint64_t>(detail::va(t->tls_array), detail::va(t->tls_block));
            detail::put<std::uint64_t>(teb + tebo::tls_pointer, detail::va(t->tls_array));
        }
        detail::put<std::uint64_t>(teb + tebo::host_thread, reinterpret_cast<std::uintptr_t>(t.get()));
        std::lock_guard<std::mutex> lock(mutex_);
        next_tid_ += 4;
        t->id = next_tid_;
        detail::put<std::uint64_t>(teb + tebo::client_tid, t->id);
        threads_.push_back(std::move(t));
        return *threads_.back();
    }

    static void attach(CpuState& s, const GuestThread& t) noexcept { s.teb = t.teb; }

    // Calls every TLS callback with (image base, `reason`, 0) on `s`'s thread. False when one faults.
    bool run_tls_callbacks(CpuState& s, std::uint32_t reason) {
        bool all = true;
        for (const std::uint64_t cb : tls_.callbacks) {
            bool ok = false;
            (void)rt_.call_guest(s, cb, {img_.image_base, reason, 0}, &ok);
            all = all && ok;
        }
        return all;
    }

    // A guest thread ends on `s`: its FLS callbacks run for every non-null value, then the TLS
    // callbacks with DLL_THREAD_DETACH; then its memory is freed (`t` is invalid afterwards).
    void exit_thread(CpuState& s, GuestThread& t) {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;  // {callback, value}
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (std::size_t i = 0; i < t.fls.size(); ++i) {
                if (t.fls[i] != 0 && i < fls_callbacks_.size() && fls_callbacks_[i] != 0) {
                    calls.emplace_back(fls_callbacks_[i], t.fls[i]);
                }
            }
            t.fls.clear();
        }
        for (const auto& [cb, value] : calls) {
            (void)rt_.call_guest(s, cb, {value});
        }
        (void)run_tls_callbacks(s, kDllThreadDetach);
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = threads_.begin(); it != threads_.end(); ++it) {
            if (it->get() == &t) {
                threads_.erase(it);
                break;
            }
        }
    }

    [[nodiscard]] const PeTls& tls_directory() const noexcept { return tls_; }
    [[nodiscard]] std::uint64_t peb() const noexcept { return detail::va(peb_); }
    [[nodiscard]] static std::uint32_t last_error(const GuestThread& t) noexcept {
        return detail::get<std::uint32_t>(t.teb + tebo::last_error);
    }
    static void set_last_error(GuestThread& t, std::uint32_t e) noexcept {
        detail::put<std::uint32_t>(t.teb + tebo::last_error, e);
    }

private:
    // The thread a shim runs on; reports and returns null when `s` has no TEB.
    GuestThread* thread_of(CpuState& s, const char* shim) {
        if (s.teb == 0) {
            const std::string what = std::string(shim) + ": the calling CpuState has no TEB (teb::Process::attach)";
            s.hooks->unsupported(&s, s.rip, what.c_str());
            return nullptr;
        }
        return reinterpret_cast<GuestThread*>(static_cast<std::uintptr_t>(
            detail::get<std::uint64_t>(s.teb + tebo::host_thread)));
    }

    // TLS slot `i` of thread `t` (expansion slots allocated when `create`).
    static std::uint64_t tls_slot_va(GuestThread& t, std::uint32_t i, bool create) {
        if (i < kTlsMinimumAvailable) {
            return t.teb + tebo::tls_slots + 8ULL * i;
        }
        if (!t.tls_expansion) {
            if (!create) {
                return 0;
            }
            t.tls_expansion = detail::zeroed(8ULL * kTlsExpansionSlots, 16);
            detail::put<std::uint64_t>(t.teb + tebo::tls_expansion, detail::va(t.tls_expansion));
        }
        return detail::va(t.tls_expansion) + 8ULL * (i - kTlsMinimumAvailable);
    }

    void tls_alloc(CpuState& s) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::uint32_t i = 0; i < kTlsSlots; ++i) {
            if (!tls_used_[i]) {
                tls_used_[i] = true;
                // A new slot reads as null on every thread.
                for (const auto& t : threads_) {
                    if (const std::uint64_t at = tls_slot_va(*t, i, false)) {
                        detail::put<std::uint64_t>(at, 0);
                    }
                }
                win64_return(s, i);
                return;
            }
        }
        if (s.teb != 0) {
            detail::put<std::uint32_t>(s.teb + tebo::last_error, kErrorNoMoreItems);
        }
        win64_return(s, kOutOfIndexes);
    }

    void tls_free(CpuState& s) {
        const auto i = static_cast<std::uint32_t>(win64_arg(s, 0));
        std::lock_guard<std::mutex> lock(mutex_);
        if (i >= kTlsSlots || !tls_used_[i]) {
            fail_invalid(s);
            return;
        }
        tls_used_[i] = false;
        for (const auto& t : threads_) {
            if (const std::uint64_t at = tls_slot_va(*t, i, false)) {
                detail::put<std::uint64_t>(at, 0);
            }
        }
        win64_return(s, 1);
    }

    void tls_get(CpuState& s) {
        GuestThread* t = thread_of(s, "TlsGetValue");
        if (t == nullptr) {
            return;
        }
        const auto i = static_cast<std::uint32_t>(win64_arg(s, 0));
        if (i >= kTlsSlots) {
            fail_invalid(s);
            return;
        }
        const std::uint64_t at = tls_slot_va(*t, i, false);
        set_last_error(*t, 0);  // TlsGetValue clears the last error when it succeeds
        win64_return(s, at == 0 ? 0 : detail::get<std::uint64_t>(at));
    }

    void tls_set(CpuState& s) {
        GuestThread* t = thread_of(s, "TlsSetValue");
        if (t == nullptr) {
            return;
        }
        const auto i = static_cast<std::uint32_t>(win64_arg(s, 0));
        if (i >= kTlsSlots) {
            fail_invalid(s);
            return;
        }
        detail::put<std::uint64_t>(tls_slot_va(*t, i, true), win64_arg(s, 1));
        win64_return(s, 1);
    }

    void fls_alloc(CpuState& s) {
        const std::uint64_t callback = win64_arg(s, 0);
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::uint32_t i = 0; i < kFlsMaximumAvailable; ++i) {
            if (i >= fls_used_.size()) {
                fls_used_.resize(i + 1, false);
                fls_callbacks_.resize(i + 1, 0);
            }
            if (!fls_used_[i]) {
                fls_used_[i] = true;
                fls_callbacks_[i] = callback;
                for (const auto& t : threads_) {
                    if (i < t->fls.size()) {
                        t->fls[i] = 0;
                    }
                }
                win64_return(s, i);
                return;
            }
        }
        if (s.teb != 0) {
            detail::put<std::uint32_t>(s.teb + tebo::last_error, kErrorNoMoreItems);
        }
        win64_return(s, kOutOfIndexes);
    }

    // Runs the slot's callback for every thread's non-null value (on the calling thread, as
    // Windows does), then frees the slot.
    void fls_free(CpuState& s) {
        const auto i = static_cast<std::uint32_t>(win64_arg(s, 0));
        std::vector<std::uint64_t> values;
        std::uint64_t callback = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (i >= fls_used_.size() || !fls_used_[i]) {
                fail_invalid(s);
                return;
            }
            callback = fls_callbacks_[i];
            for (const auto& t : threads_) {
                if (i < t->fls.size() && t->fls[i] != 0) {
                    values.push_back(t->fls[i]);
                    t->fls[i] = 0;
                }
            }
            fls_used_[i] = false;
            fls_callbacks_[i] = 0;
        }
        if (callback != 0) {
            for (const std::uint64_t v : values) {
                (void)rt_.call_guest(s, callback, {v});
                if (s.unwind_rsp != 0) {
                    return;  // the callback raised and an unwind is passing this shim
                }
            }
        }
        win64_return(s, 1);
    }

    void fls_get(CpuState& s) {
        GuestThread* t = thread_of(s, "FlsGetValue");
        if (t == nullptr) {
            return;
        }
        const auto i = static_cast<std::uint32_t>(win64_arg(s, 0));
        std::lock_guard<std::mutex> lock(mutex_);
        if (i >= fls_used_.size() || !fls_used_[i]) {
            fail_invalid(s);
            return;
        }
        win64_return(s, i < t->fls.size() ? t->fls[i] : 0);
    }

    void fls_set(CpuState& s) {
        GuestThread* t = thread_of(s, "FlsSetValue");
        if (t == nullptr) {
            return;
        }
        const auto i = static_cast<std::uint32_t>(win64_arg(s, 0));
        std::lock_guard<std::mutex> lock(mutex_);
        if (i >= fls_used_.size() || !fls_used_[i]) {
            fail_invalid(s);
            return;
        }
        if (i >= t->fls.size()) {
            t->fls.resize(i + 1, 0);
        }
        t->fls[i] = win64_arg(s, 1);
        win64_return(s, 1);
    }

    // FALSE / 0 with ERROR_INVALID_PARAMETER.
    static void fail_invalid(CpuState& s) {
        if (s.teb != 0) {
            detail::put<std::uint32_t>(s.teb + tebo::last_error, kErrorInvalidParameter);
        }
        win64_return(s, 0);
    }

    Runtime& rt_;
    const PeImage& img_;
    std::uint8_t* image_;
    PeTls tls_;
    detail::Block peb_{nullptr, detail::AlignedFree{4096}};
    std::mutex mutex_;
    std::list<std::unique_ptr<GuestThread>> threads_;
    std::uint32_t next_tid_ = kProcessId;
    bool tls_used_[kTlsSlots] = {};
    std::vector<bool> fls_used_;
    std::vector<std::uint64_t> fls_callbacks_;
};

}  // namespace wse::recomp::teb
