// Win64 memory for translated code (Phase 5 R4, `docs/RECOMP_PLAN.md` "R4 design", family
// "Memory"; v1.1.5.6: Heap* ~30 call sites, Global*/Local* 14, VirtualAlloc & co. from lua5164):
//
//   GetProcessHeap GetProcessHeaps HeapCreate HeapDestroy HeapAlloc HeapReAlloc HeapFree HeapSize
//   HeapValidate HeapCompact HeapLock HeapUnlock HeapWalk HeapSetInformation
//   LocalAlloc LocalReAlloc LocalFree LocalSize GlobalAlloc GlobalReAlloc GlobalFree GlobalSize
//   GlobalLock GlobalUnlock
//   VirtualAlloc VirtualFree VirtualProtect VirtualQuery
//
// Guest memory is host memory. Heap blocks come from the host allocator with a 16-byte header
// (size, owning heap), 16-byte aligned as on Windows x64; Local/Global memory is fixed (the handle
// is the pointer, so `GlobalLock` returns it). Virtual memory is real host address space
// (mmap / VirtualAlloc), reserved and committed read-write; the protection a guest asks for is
// recorded and reported by `VirtualQuery`, not enforced, and a request for a fixed address outside
// a region already reserved fails. Header-only.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/teb.h"

#if defined(_WIN32)
extern "C" __declspec(dllimport) void* __stdcall VirtualAlloc(void* address, std::size_t size, unsigned long type,
                                                              unsigned long protect);
extern "C" __declspec(dllimport) int __stdcall VirtualFree(void* address, std::size_t size, unsigned long type);
#else
#include <sys/mman.h>
#endif

namespace wse::recomp::shim {

inline constexpr std::uint32_t kHeapZeroMemory = 0x8;
inline constexpr std::uint32_t kHeapReallocInPlaceOnly = 0x10;
inline constexpr std::uint32_t kGmemZeroInit = 0x40;  // GMEM_ZEROINIT / LMEM_ZEROINIT
inline constexpr std::uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000,
                               kMemRelease = 0x8000, kMemFree = 0x10000, kMemPrivate = 0x20000;
inline constexpr std::uint32_t kErrorNotEnoughMemory = 8;
inline constexpr std::uint32_t kErrorInvalidAddress = 487;
inline constexpr std::uint32_t kErrorNoMoreItems = 259;
inline constexpr std::uint64_t kVirtualGranularity = 65536;

namespace mem_detail {
template <class T>
void put(std::uint64_t va, T v) noexcept {
    std::memcpy(reinterpret_cast<void*>(va), &v, sizeof(T));
}
inline void* host_reserve(std::size_t n) noexcept {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, n, 0x3000, 0x04);  // MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE
#else
    void* p = mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}
inline void host_release(void* p, std::size_t n) noexcept {
#if defined(_WIN32)
    (void)n;
    (void)VirtualFree(p, 0, 0x8000);
#else
    (void)munmap(p, n);
#endif
}
}  // namespace mem_detail

class Memory {
public:
    explicit Memory(Runtime& rt) : rt_(rt) { heaps_.insert(kProcessHeap); }
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;
    ~Memory() {
        for (auto& [base, r] : regions_) {
            mem_detail::host_release(reinterpret_cast<void*>(static_cast<std::uintptr_t>(base)), r.size);
        }
    }

    static constexpr std::uint64_t kProcessHeap = 0x0000000000A10000ULL;  // GetProcessHeap's handle

    // Host-side allocation as HeapAlloc does (used by other shims, e.g. FormatMessage's buffer).
    std::uint64_t alloc(std::uint64_t heap, std::uint64_t size, bool zero) {
        auto* raw = static_cast<std::uint8_t*>(std::malloc(size + kHeader));
        if (raw == nullptr) {
            return 0;
        }
        if (zero) {
            std::memset(raw + kHeader, 0, size);
        }
        Header h{size, heap ^ kMagic};
        std::memcpy(raw, &h, sizeof(h));
        return reinterpret_cast<std::uintptr_t>(raw) + kHeader;
    }
    // Frees a block from `alloc`; false when it is not one.
    bool free(std::uint64_t p) {
        if (p == 0) {
            return true;
        }
        Header h{};
        if (!header(p, h)) {
            return false;
        }
        h.owner = 0;
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p - kHeader)), &h, sizeof(h));
        std::free(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p - kHeader)));
        return true;
    }
    // The size of a block from `alloc`, or ~0 when it is not one.
    std::uint64_t size_of(std::uint64_t p) const {
        Header h{};
        return header(p, h) ? h.size : ~std::uint64_t{0};
    }

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* name, Runtime::ImportShim shim) {
            for (const char* dll : {"KERNEL32.dll", "KERNELBASE.dll"}) {
                n += rt_.set_import(dll, name, shim);
            }
        };
        bind("GetProcessHeap", [](CpuState& s) { win64_return(s, kProcessHeap); });
        bind("GetProcessHeaps", [this](CpuState& s) {
            std::lock_guard<std::mutex> lock(m_);
            const auto count = static_cast<std::uint32_t>(win64_arg(s, 0));
            std::uint32_t i = 0;
            for (const std::uint64_t h : heaps_) {
                if (i < count) {
                    mem_detail::put<std::uint64_t>(win64_arg(s, 1) + 8ULL * i, h);
                }
                ++i;
            }
            win64_return(s, i);
        });
        bind("HeapCreate", [this](CpuState& s) {
            std::lock_guard<std::mutex> lock(m_);
            next_heap_ += 0x10000;
            heaps_.insert(next_heap_);
            win64_return(s, next_heap_);
        });
        bind("HeapDestroy", [this](CpuState& s) {
            std::lock_guard<std::mutex> lock(m_);
            const std::uint64_t h = win64_arg(s, 0);
            const bool ok = h != kProcessHeap && heaps_.erase(h) != 0;  // its blocks are not reclaimed
            if (!ok) teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, ok ? 1 : 0);
        });
        bind("HeapAlloc", [this](CpuState& s) {
            const std::uint64_t p = alloc(win64_arg(s, 0), win64_arg(s, 2), (win64_arg(s, 1) & kHeapZeroMemory) != 0);
            if (p == 0) teb::set_error(s, kErrorNotEnoughMemory);
            win64_return(s, p);
        });
        bind("HeapReAlloc", [this](CpuState& s) {
            win64_return(s, realloc(s, win64_arg(s, 0), static_cast<std::uint32_t>(win64_arg(s, 1)), win64_arg(s, 2),
                                    win64_arg(s, 3)));
        });
        bind("HeapFree", [this](CpuState& s) {
            const bool ok = free(win64_arg(s, 2));
            if (!ok) teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, ok ? 1 : 0);
        });
        bind("HeapSize", [this](CpuState& s) { win64_return(s, size_of(win64_arg(s, 2))); });
        bind("HeapValidate", [this](CpuState& s) {
            const std::uint64_t p = win64_arg(s, 2);
            win64_return(s, p == 0 || size_of(p) != ~std::uint64_t{0} ? 1 : 0);
        });
        bind("HeapCompact", [](CpuState& s) { win64_return(s, 1U << 20); });  // largest free block
        bind("HeapLock", [](CpuState& s) { win64_return(s, 1); });
        bind("HeapUnlock", [](CpuState& s) { win64_return(s, 1); });
        bind("HeapSetInformation", [](CpuState& s) { win64_return(s, 1); });
        bind("HeapWalk", [](CpuState& s) {  // the heap is not enumerable: an empty walk
            teb::set_error(s, kErrorNoMoreItems);
            win64_return(s, 0);
        });
        // Local / Global memory: fixed; flags at 0, size at 1 (Alloc) or handle at 0 (others).
        auto local_alloc = [this](CpuState& s) {
            const std::uint64_t p = alloc(kProcessHeap, win64_arg(s, 1), (win64_arg(s, 0) & kGmemZeroInit) != 0);
            if (p == 0) teb::set_error(s, kErrorNotEnoughMemory);
            win64_return(s, p);
        };
        auto local_free = [this](CpuState& s) {
            const std::uint64_t p = win64_arg(s, 0);
            if (free(p)) {
                win64_return(s, 0);  // NULL on success
            } else {
                teb::set_error(s, kErrorInvalidHandle);
                win64_return(s, p);
            }
        };
        auto local_realloc = [this](CpuState& s) {
            const auto flags = static_cast<std::uint32_t>(win64_arg(s, 2));
            win64_return(s, realloc(s, kProcessHeap, (flags & kGmemZeroInit) != 0 ? kHeapZeroMemory : 0, win64_arg(s, 0),
                                    win64_arg(s, 1)));
        };
        auto local_size = [this](CpuState& s) {
            const std::uint64_t n = size_of(win64_arg(s, 0));
            win64_return(s, n == ~std::uint64_t{0} ? 0 : n);
        };
        bind("LocalAlloc", local_alloc);
        bind("GlobalAlloc", local_alloc);
        bind("LocalFree", local_free);
        bind("GlobalFree", local_free);
        bind("LocalReAlloc", local_realloc);
        bind("GlobalReAlloc", local_realloc);
        bind("LocalSize", local_size);
        bind("GlobalSize", local_size);
        bind("GlobalLock", [](CpuState& s) { win64_return(s, win64_arg(s, 0)); });
        bind("GlobalUnlock", [](CpuState& s) {
            teb::set_error(s, 0);  // NO_ERROR: the (fixed) block is unlocked
            win64_return(s, 0);
        });
        bind("VirtualAlloc", [this](CpuState& s) { virtual_alloc(s); });
        bind("VirtualFree", [this](CpuState& s) { virtual_free(s); });
        bind("VirtualProtect", [this](CpuState& s) { virtual_protect(s); });
        bind("VirtualQuery", [this](CpuState& s) { virtual_query(s); });
        return n;
    }

private:
    static constexpr std::uint64_t kHeader = 16;
    static constexpr std::uint64_t kMagic = 0x5E4A11C0C0FFEE00ULL;
    struct Header {
        std::uint64_t size;
        std::uint64_t owner;  // heap ^ kMagic while allocated
    };
    bool header(std::uint64_t p, Header& h) const {
        if (p < kHeader || (p & 15) != 0) {
            return false;
        }
        std::memcpy(&h, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(p - kHeader)), sizeof(h));
        std::lock_guard<std::mutex> lock(m_);
        return heaps_.count(h.owner ^ kMagic) != 0 || (h.owner ^ kMagic) == kProcessHeap;
    }

    std::uint64_t realloc(CpuState& s, std::uint64_t heap, std::uint32_t flags, std::uint64_t p, std::uint64_t n) {
        const std::uint64_t old = size_of(p);
        if (old == ~std::uint64_t{0}) {
            teb::set_error(s, teb::kErrorInvalidParameter);
            return 0;
        }
        if (n <= old) {
            if ((flags & kHeapZeroMemory) == 0 || n == old) {
                // Shrinking in place: the recorded size follows (HeapSize reports the new size).
                Header h{n, heap ^ kMagic};
                std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p - kHeader)), &h, sizeof(h));
                return p;
            }
        }
        if ((flags & kHeapReallocInPlaceOnly) != 0) {
            teb::set_error(s, kErrorNotEnoughMemory);
            return 0;
        }
        const std::uint64_t q = alloc(heap, n, false);
        if (q == 0) {
            teb::set_error(s, kErrorNotEnoughMemory);
            return 0;
        }
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(q)),
                    reinterpret_cast<const void*>(static_cast<std::uintptr_t>(p)), old < n ? old : n);
        if (n > old && (flags & kHeapZeroMemory) != 0) {
            std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(q + old)), 0, n - old);
        }
        (void)free(p);
        return q;
    }

    struct Region {
        std::size_t size = 0;
        std::uint32_t protect = 0x04;
        std::uint32_t state = kMemCommit;
    };
    static constexpr std::uint32_t kErrorInvalidHandle = 6;

    // The region holding [va, va + n), or null.
    std::map<std::uint64_t, Region>::iterator region_of(std::uint64_t va, std::uint64_t n) {
        auto it = regions_.upper_bound(va);
        if (it == regions_.begin()) {
            return regions_.end();
        }
        --it;
        return va - it->first + n <= it->second.size ? it : regions_.end();
    }

    // VirtualAlloc(address, size, type, protect).
    void virtual_alloc(CpuState& s) {
        const std::uint64_t want = win64_arg(s, 0);
        const std::uint64_t size = win64_arg(s, 1);
        const auto type = static_cast<std::uint32_t>(win64_arg(s, 2));
        const auto protect = static_cast<std::uint32_t>(win64_arg(s, 3));
        std::lock_guard<std::mutex> lock(m_);
        if (size == 0 || (type & (kMemCommit | kMemReserve)) == 0) {
            teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, 0);
            return;
        }
        if (want != 0) {
            // Committing (part of) a region reserved earlier: already read-write and zero.
            const auto it = region_of(want & ~std::uint64_t{0xFFF}, size);
            if (it == regions_.end()) {
                teb::set_error(s, kErrorInvalidAddress);
                win64_return(s, 0);
                return;
            }
            if ((type & kMemCommit) != 0) {
                it->second.state = kMemCommit;
                it->second.protect = protect;
            }
            win64_return(s, want & ~std::uint64_t{0xFFF});
            return;
        }
        const std::size_t n = static_cast<std::size_t>((size + kVirtualGranularity - 1) & ~(kVirtualGranularity - 1));
        void* p = mem_detail::host_reserve(n);
        if (p == nullptr) {
            teb::set_error(s, kErrorNotEnoughMemory);
            win64_return(s, 0);
            return;
        }
        Region r;
        r.size = n;
        r.protect = protect;
        r.state = (type & kMemCommit) != 0 ? kMemCommit : kMemReserve;
        const auto va = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p));
        regions_[va] = r;
        win64_return(s, va);
    }

    // VirtualFree(address, size, type): MEM_RELEASE frees the whole region (size 0), MEM_DECOMMIT
    // zeroes the range.
    void virtual_free(CpuState& s) {
        const std::uint64_t va = win64_arg(s, 0);
        const std::uint64_t size = win64_arg(s, 1);
        const auto type = static_cast<std::uint32_t>(win64_arg(s, 2));
        std::lock_guard<std::mutex> lock(m_);
        const auto it = regions_.find(va);
        if (type == kMemRelease) {
            if (it == regions_.end() || size != 0) {
                teb::set_error(s, teb::kErrorInvalidParameter);
                win64_return(s, 0);
                return;
            }
            mem_detail::host_release(reinterpret_cast<void*>(static_cast<std::uintptr_t>(va)), it->second.size);
            regions_.erase(it);
            win64_return(s, 1);
            return;
        }
        const auto r = region_of(va, size == 0 ? 1 : size);
        if (type != kMemDecommit || r == regions_.end()) {
            teb::set_error(s, teb::kErrorInvalidParameter);
            win64_return(s, 0);
            return;
        }
        const std::uint64_t n = size == 0 ? r->second.size - (va - r->first) : size;
        std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(va)), 0, static_cast<std::size_t>(n));
        win64_return(s, 1);
    }

    // VirtualProtect(address, size, newProtect, *oldProtect).
    void virtual_protect(CpuState& s) {
        const std::uint64_t va = win64_arg(s, 0);
        const std::uint64_t size = win64_arg(s, 1);
        const std::uint64_t old = win64_arg(s, 3);
        std::lock_guard<std::mutex> lock(m_);
        const auto r = region_of(va, size == 0 ? 1 : size);
        std::uint32_t before = 0x04;
        if (r != regions_.end()) {
            before = r->second.protect;
            r->second.protect = static_cast<std::uint32_t>(win64_arg(s, 2));
        } else {
            before = 0x40;  // image or heap memory: PAGE_EXECUTE_READWRITE (nothing is enforced)
        }
        if (old == 0) {
            teb::set_error(s, 998);  // ERROR_NOACCESS: Windows requires the out pointer
            win64_return(s, 0);
            return;
        }
        mem_detail::put<std::uint32_t>(old, before);
        win64_return(s, 1);
    }

    // VirtualQuery(address, *MEMORY_BASIC_INFORMATION, length) -> bytes written (48).
    void virtual_query(CpuState& s) {
        const std::uint64_t va = win64_arg(s, 0);
        const std::uint64_t mbi = win64_arg(s, 1);
        if (win64_arg(s, 2) < 48) {
            teb::set_error(s, 24);  // ERROR_BAD_LENGTH
            win64_return(s, 0);
            return;
        }
        std::lock_guard<std::mutex> lock(m_);
        std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(mbi)), 0, 48);
        const auto r = region_of(va, 1);
        const std::uint64_t page = va & ~std::uint64_t{0xFFF};
        if (r != regions_.end()) {
            mem_detail::put<std::uint64_t>(mbi + 0, page);                      // BaseAddress
            mem_detail::put<std::uint64_t>(mbi + 8, r->first);                  // AllocationBase
            mem_detail::put<std::uint32_t>(mbi + 16, r->second.protect);        // AllocationProtect
            mem_detail::put<std::uint64_t>(mbi + 24, r->first + r->second.size - page);  // RegionSize
            mem_detail::put<std::uint32_t>(mbi + 32, r->second.state);          // State
            mem_detail::put<std::uint32_t>(mbi + 36, r->second.protect);        // Protect
            mem_detail::put<std::uint32_t>(mbi + 40, kMemPrivate);              // Type
        } else {
            // Memory this object did not allocate (the image, heaps, stacks): committed read-write.
            mem_detail::put<std::uint64_t>(mbi + 0, page);
            mem_detail::put<std::uint64_t>(mbi + 8, page);
            mem_detail::put<std::uint32_t>(mbi + 16, 0x04);
            mem_detail::put<std::uint64_t>(mbi + 24, 0x1000);
            mem_detail::put<std::uint32_t>(mbi + 32, kMemCommit);
            mem_detail::put<std::uint32_t>(mbi + 36, 0x04);
            mem_detail::put<std::uint32_t>(mbi + 40, kMemPrivate);
        }
        win64_return(s, 48);
    }

    Runtime& rt_;
    mutable std::mutex m_;
    std::set<std::uint64_t> heaps_;
    std::uint64_t next_heap_ = kProcessHeap;
    std::map<std::uint64_t, Region> regions_;
};

}  // namespace wse::recomp::shim
