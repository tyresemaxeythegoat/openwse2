// R0 (d) test vectors: one retail function checked against the PC (`docs/RECOMP_PLAN.md`).
//
// The function is `rglString::indexOf(const rglString&, int) const` (`0x1403103f0`), a leaf
// that reads two strings. The same seeded cases run on the PC (native call into the user's
// mapped exe, `tools/r0_pc_reference.cpp`, local only) and on the phone (the translated
// function); both print the FNV-1a hash of the results, and the hashes must match.
//
// `rglString` layout (PDB TPI `0x35f5`, 72 bytes, `docs/ARCHITECTURE.md`): `m_cursor` int32
// at `0x0`, `m_buffer` `char*` at `0x8`, `m_bufferLength` int32 at `0x10`, `m_strLength`
// int32 at `0x14`, `m_staticBuffer` `char[48]` at `0x18`. `indexOf` reads only `m_buffer`
// and `m_strLength`. Cases keep `start` in `[0, length]`: retail does not check it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "wse/recomp/x86_interp.h"

namespace wse::recomp {

// Located by name through the install's own PDB (`pdb_reader.h`), so any WSE2 build works
// (its address per build is in `docs/ARCHITECTURE.md`).
inline constexpr const char* kR0IndexOfSymbol = "?indexOf@rglString@@QEBAHAEBV1@H@Z";
inline constexpr std::size_t kRglStringSize = 72;
inline constexpr std::size_t kRglStringBufferOffset = 0x8;
inline constexpr std::size_t kRglStringBufferLengthOffset = 0x10;
inline constexpr std::size_t kRglStringLengthOffset = 0x14;
inline constexpr std::size_t kR0CaseCount = 512;


struct R0IndexOfCase {
    std::string haystack;
    std::string needle;
    std::int32_t start = 0;
};

// Fake `rglString` objects in host memory, laid out as the PDB says.
struct RglStringFake {
    alignas(8) std::uint8_t bytes[kRglStringSize] = {};
    std::vector<char> storage;

    explicit RglStringFake(const std::string& s) : storage(s.begin(), s.end()) {
        storage.push_back('\0');
        char* buf = storage.data();
        const auto len = static_cast<std::int32_t>(s.size());
        const auto cap = static_cast<std::int32_t>(storage.size());
        std::memcpy(bytes + kRglStringBufferOffset, &buf, sizeof(buf));
        std::memcpy(bytes + kRglStringBufferLengthOffset, &cap, 4);
        std::memcpy(bytes + kRglStringLengthOffset, &len, 4);
    }
    RglStringFake(const RglStringFake&) = delete;
    RglStringFake& operator=(const RglStringFake&) = delete;
};

inline std::vector<R0IndexOfCase> make_r0_index_of_cases() {
    std::uint64_t x = 0x9E3779B97F4A7C15ULL;
    const auto next = [&x]() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        return x;
    };
    static constexpr char kAlphabet[] = "abca b_";
    std::vector<R0IndexOfCase> cases;
    cases.reserve(kR0CaseCount);
    // Fixed edge cases first: empty needle, empty haystack, needle longer than haystack,
    // a match at the very end, and a start past the only match.
    cases.push_back({"", "", 0});
    cases.push_back({"abc", "", 1});
    cases.push_back({"", "a", 0});
    cases.push_back({"ab", "abc", 0});
    cases.push_back({"xxxxab", "ab", 0});
    cases.push_back({"abxxxx", "ab", 1});
    cases.push_back({"aaaa", "aa", 3});
    cases.push_back({"aaaa", "aa", 2});
    while (cases.size() < kR0CaseCount) {
        R0IndexOfCase c;
        const std::size_t hl = static_cast<std::size_t>(next() % 41U);
        const std::size_t nl = static_cast<std::size_t>(next() % 5U);
        for (std::size_t i = 0; i < hl; ++i) {
            c.haystack.push_back(kAlphabet[next() % (sizeof(kAlphabet) - 1)]);
        }
        for (std::size_t i = 0; i < nl; ++i) {
            c.needle.push_back(kAlphabet[next() % (sizeof(kAlphabet) - 1)]);
        }
        c.start = static_cast<std::int32_t>(next() % (hl + 1U));
        cases.push_back(std::move(c));
    }
    return cases;
}

// What `indexOf` is expected to compute (from its disassembly): the first position
// `p >= start` with `p <= hay_len - needle_len` where the needle matches, else -1.
inline std::int32_t r0_index_of_model(const R0IndexOfCase& c) noexcept {
    const auto hl = static_cast<std::int64_t>(c.haystack.size());
    const auto nl = static_cast<std::int64_t>(c.needle.size());
    for (std::int64_t p = c.start; p < hl - nl + 1; ++p) {
        if (c.haystack.compare(static_cast<std::size_t>(p), static_cast<std::size_t>(nl), c.needle) == 0) {
            return static_cast<std::int32_t>(p);
        }
    }
    return -1;
}

inline std::uint64_t fnv1a_results(const std::vector<std::int32_t>& results) noexcept {
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const std::int32_t r : results) {
        for (int b = 0; b < 4; ++b) {
            h = (h ^ ((static_cast<std::uint32_t>(r) >> (8 * b)) & 0xFFU)) * 0x100000001b3ULL;
        }
    }
    return h;
}

// A Win64 guest stack for one call: at entry `rsp % 16 == 8`, `[rsp]` holds `sentinel` (the
// return address `ret` pops), and the 32-byte home area above it is writable.
class GuestStack {
public:
    static constexpr std::uint64_t kReturnSentinel = 0x00007FFEDEAD0000ULL;

    explicit GuestStack(std::size_t bytes = 64 * 1024) : mem_(bytes + 64, 0) {}

    void prepare(CpuState& s) {
        s = CpuState{};
        auto top = reinterpret_cast<std::uintptr_t>(mem_.data() + mem_.size() - 64);
        top &= ~static_cast<std::uintptr_t>(15);
        const std::uintptr_t rsp = top - 8;
        std::memcpy(reinterpret_cast<void*>(rsp), &kReturnSentinel, 8);
        s.gpr[static_cast<int>(Reg::rsp)] = rsp;
    }

private:
    std::vector<std::uint8_t> mem_;
};

}  // namespace wse::recomp
