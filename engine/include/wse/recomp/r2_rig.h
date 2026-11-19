// Phase 5 R2 differential rig: inputs and result hashing shared by the PC recorder
// (`tools/r2_pc_record.cpp`, native retail code) and the phone (`r0_spikes.cpp`, translated code),
// so both compute the same hash for the same behaviour (`docs/RECOMP_PLAN.md` "R2 design").
//
// Header-only, no OS or LLVM. Pointers differ between the two sides, so inputs describe pointers
// as (buffer, offset) and outputs replace any value pointing into a buffer or the stack by its
// region and offset before hashing.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wse::recomp::r2 {

inline constexpr int kCases = 16;
inline constexpr int kBuffers = 4;
inline constexpr std::size_t kBufferBytes = 256 * 1024;
// Where argument and stored pointers point: far enough in for small negative offsets, leaving
// most of the buffer for positive ones (`[rcx + 0x13e70]` is common).
inline constexpr std::int64_t kPointerOffset = 4096;
inline constexpr int kStackArgs = 8;  // arguments 5..12 at [rsp + 0x28 ..] after the call

// Fixed addresses, the same on the PC and the phone, so pointer arithmetic (a pointer stored in
// 32 bits, a pointer added to data) gives the same bits on both sides. Each buffer sits between
// no-access guard regions of `kBufferBytes`; the guest stack is `kStackBytes` below `kStackTop`.
inline constexpr std::uint64_t kRegionBase = 0x210000000ULL;
inline constexpr std::uint64_t kRegionBytes = kBufferBytes * (2 * kBuffers + 1);
inline constexpr std::uint64_t buffer_address(int b) noexcept { return kRegionBase + kBufferBytes * (2 * b + 1); }
inline constexpr std::uint64_t kStackBase = 0x220000000ULL;
inline constexpr std::uint64_t kStackBytes = 1 << 20;
// The guest rsp at entry (the return address sits here; `rsp + 8` is 16-aligned).
inline constexpr std::uint64_t kGuestRsp = kStackBase + kStackBytes - 0x2000 - 8;
// The return address every call returns to (some functions read `[rsp]`). On the PC a stub there
// jumps back into the recorder; the phone only compares the popped `rip` with it.
inline constexpr std::uint64_t kReturnVa = 0x230000000ULL;
// Zeroed before every case, so reads of uninitialized stack agree.
inline constexpr std::uint64_t kStackClearLo = kGuestRsp - 0x8000;
inline constexpr std::uint64_t kStackClearHi = kGuestRsp + 0x1000;

// A guest argument: a pointer into buffer `buffer` at `offset`, or `literal` when buffer < 0.
struct Arg {
    int buffer = -1;
    std::int64_t offset = 0;
    std::uint64_t literal = 0;
};

struct CaseInputs {
    Arg gpr[4];                 // rcx rdx r8 r9
    Arg stack[kStackArgs];      // arguments 5..12
    std::uint8_t xmm[4][16]{};  // xmm0..xmm3
};

// Where the side under test put things (all ranges half-open).
struct Regions {
    std::uint64_t buffer[kBuffers]{};
    std::uint64_t stack_lo = 0;
    std::uint64_t stack_hi = 0;
};

// The regions of the fixed layout.
inline Regions fixed_regions() noexcept {
    Regions r;
    for (int b = 0; b < kBuffers; ++b) {
        r.buffer[b] = buffer_address(b);
    }
    r.stack_lo = kStackBase;
    r.stack_hi = kStackBase + kStackBytes;
    return r;
}

inline std::uint64_t splitmix(std::uint64_t& s) noexcept {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

namespace detail {

inline Arg pick_arg(std::uint64_t& s) noexcept {
    static constexpr std::uint64_t kEdges[] = {0,          1,          2,          0xFF,
                                               0x7FFF,     0xFFFF,     0x7FFFFFFF, 0x80000000ULL,
                                               0xFFFFFFFFULL, 0x7FFFFFFFFFFFFFFFULL, 0x8000000000000000ULL,
                                               ~0ULL};
    const std::uint64_t r = splitmix(s);
    Arg a;
    switch (r % 4) {
    case 0:
    case 1:  // a pointer 4 KiB into a buffer (+-256 bytes, 8-aligned): structs reach far forward
        a.buffer = static_cast<int>((r >> 8) % kBuffers);
        a.offset = kPointerOffset + (static_cast<std::int64_t>((r >> 16) % 64) - 32) * 8;
        break;
    case 2:
        a.literal = (r >> 8) % 256;
        break;
    default:
        a.literal = ((r >> 8) % 2) == 0 ? kEdges[(r >> 16) % (sizeof(kEdges) / sizeof(kEdges[0]))] : splitmix(s);
        break;
    }
    return a;
}

inline std::uint32_t pick_float_bits(std::uint64_t& s) noexcept {
    static constexpr std::uint32_t kNice[] = {0x00000000U, 0x3F800000U, 0xBF800000U, 0x3F000000U,
                                              0x40000000U, 0x42C80000U, 0xC0500000U, 0x80000000U};
    const std::uint64_t r = splitmix(s);
    // Finite only: clearing the exponent's top bit keeps |x| < 2^1 for random patterns.
    return (r % 2) == 0 ? kNice[(r >> 8) % 8] : static_cast<std::uint32_t>(r >> 16) & 0xBFFFFFFFU;
}

}  // namespace detail

// The inputs of case `k` for the function at `va` (the same on every side).
inline CaseInputs make_case(std::uint64_t va, int k) noexcept {
    std::uint64_t s = va * 0x100000001B3ULL + static_cast<std::uint64_t>(k) * 0xC2B2AE3D27D4EB4FULL;
    CaseInputs c;
    for (Arg& a : c.gpr) {
        a = detail::pick_arg(s);
    }
    for (Arg& a : c.stack) {
        a = detail::pick_arg(s);
    }
    for (auto& x : c.xmm) {
        for (int lane = 0; lane < 4; ++lane) {
            const std::uint32_t f = detail::pick_float_bits(s);
            std::memcpy(x + 4 * lane, &f, 4);
        }
    }
    return c;
}

inline std::uint64_t materialize(const Arg& a, const Regions& r) noexcept {
    return a.buffer < 0 ? a.literal : r.buffer[a.buffer] + static_cast<std::uint64_t>(a.offset);
}

// Fills the buffers once per function (every case of a function starts from the same contents;
// `restore_buffers` puts them back between cases): random bytes, with every even aligned word a
// pointer into a buffer, so pointer-chasing code mostly reads real memory instead of faulting.
inline void fill_buffers(std::uint64_t va, std::uint8_t* const buffers[kBuffers], const Regions& r) noexcept {
    std::uint64_t s = va ^ 0x5DEECE66DULL;
    for (int b = 0; b < kBuffers; ++b) {
        for (std::size_t w = 0; w < kBufferBytes / 8; ++w) {
            std::uint64_t v = splitmix(s);
            if (w % 2 == 0) {
                const int target = static_cast<int>(v % kBuffers);
                const std::uint64_t off = kPointerOffset + ((v >> 8) % (kBufferBytes / 2 / 8)) * 8;
                v = r.buffer[target] + off;
            }
            std::memcpy(buffers[b] + 8 * w, &v, 8);
        }
    }
}

// Copies the function's starting contents back after a case.
inline void restore_buffers(std::uint8_t* const buffers[kBuffers], const std::uint8_t* const pristine[kBuffers]) noexcept {
    for (int b = 0; b < kBuffers; ++b) {
        std::memcpy(buffers[b], pristine[b], kBufferBytes);
    }
}

// A pointer into a buffer or the stack becomes its region and offset; anything else stays.
inline std::uint64_t normalize(std::uint64_t v, const Regions& r) noexcept {
    for (int b = 0; b < kBuffers; ++b) {
        if (v >= r.buffer[b] && v - r.buffer[b] < kBufferBytes) {
            return 0xB000000000000000ULL | (static_cast<std::uint64_t>(b) << 32) | (v - r.buffer[b]);
        }
    }
    if (v >= r.stack_lo && v < r.stack_hi) {
        return 0x5000000000000000ULL | (r.stack_hi - v);
    }
    return v;
}

// FNV-1a over 64-bit words (with a fold so high bits reach the low ones): 256 KiB of buffers per
// case, so byte-wise FNV would be eight times slower on the phone.
struct Hasher {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    void add(std::uint64_t v) noexcept {
        h ^= v;
        h *= 0x100000001B3ULL;
        h ^= h >> 29;
    }
};

// Adds one case's outcome: `rax`, the low half of `xmm0`, and every buffer word the call changed
// (its position and new value). A faulting case contributes only the fault: how far a side got
// before faulting depends on store ordering the translator may change.
inline void hash_case(Hasher& out, bool faulted, std::uint64_t rax, std::uint64_t xmm0_lo,
                      const std::uint8_t* const buffers[kBuffers], const std::uint8_t* const pristine[kBuffers],
                      const Regions& r) noexcept {
    if (faulted) {
        out.add(0xFA017ULL);
        return;
    }
    out.add(normalize(rax, r));
    out.add(xmm0_lo);
    constexpr std::size_t kChunk = 4096;
    for (int b = 0; b < kBuffers; ++b) {
        for (std::size_t c = 0; c < kBufferBytes; c += kChunk) {
            if (std::memcmp(buffers[b] + c, pristine[b] + c, kChunk) == 0) {
                continue;
            }
            for (std::size_t w = c / 8; w < (c + kChunk) / 8; ++w) {
                std::uint64_t now = 0;
                std::uint64_t was = 0;
                std::memcpy(&now, buffers[b] + 8 * w, 8);
                std::memcpy(&was, pristine[b] + 8 * w, 8);
                if (now != was) {
                    out.add((static_cast<std::uint64_t>(b) << 32) | w);
                    out.add(normalize(now, r));
                }
            }
        }
    }
}

}  // namespace wse::recomp::r2
