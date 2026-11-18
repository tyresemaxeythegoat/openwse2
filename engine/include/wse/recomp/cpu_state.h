// Guest CPU state of a translated thread (Phase 5 R1, `docs/RECOMP_PLAN.md` "R1 design").
//
// Translated functions are `void fn(CpuState*)`. The layout is fixed: the LLVM lifter addresses
// fields by these byte offsets, and R0's offsets (GPRs, `rip`, CF PF ZF SF OF) are unchanged.
// `hooks` points at the host runtime (`RuntimeHooks`), so a translated shared object needs no
// imported symbols: every call out of guest code goes through this table.

#pragma once

#include <cstddef>
#include <cstdint>

namespace wse::recomp {

struct CpuState;

// Host runtime entry points used by translated code (filled by the loader).
struct RuntimeHooks {
    // Indirect call/jump target `va` (rsp already holds the return address for a call).
    void (*dispatch)(CpuState* state, std::uint64_t va) = nullptr;
    // A call through the import address table slot at `slot_va` (R4 shims plug in here).
    void (*import_call)(CpuState* state, std::uint64_t slot_va) = nullptr;
    // A callee returned to `state->rip` instead of `expected` (stack or ABI corruption).
    void (*bad_return)(CpuState* state, std::uint64_t expected) = nullptr;
    // Code the translator could not lift (`va` is the instruction; `what` a short reason).
    void (*unsupported)(CpuState* state, std::uint64_t va, const char* what) = nullptr;
    // `gs:`/`fs:` segment base (Win64 TEB and TLS: R3).
    std::uint64_t (*segment_base)(CpuState* state, int segment) = nullptr;
    // Owner of this table (`wse::recomp::Runtime` in `runtime.h`); translated code never reads it.
    void* context = nullptr;
};

struct alignas(16) CpuState {
    std::uint64_t gpr[16] = {};  // rax rcx rdx rbx rsp rbp rsi rdi r8..r15
    std::uint64_t rip = 0;
    bool cf = false;  // 136
    bool pf = false;  // 137
    bool zf = false;  // 138
    bool sf = false;  // 139
    bool of = false;  // 140
    bool af = false;  // 141
    bool df = false;  // 142
    std::uint8_t pad0 = 0;
    std::uint8_t pad1[8] = {};
    alignas(16) std::uint8_t xmm[16][16] = {};  // 160
    std::uint32_t mxcsr = 0x1F80;                // 416: all exceptions masked, round to nearest
    std::uint32_t pad2 = 0;
    const RuntimeHooks* hooks = nullptr;         // 424
    std::uint64_t pad3 = 0;
};

namespace cpu_offset {
inline constexpr std::size_t gpr = 0;
inline constexpr std::size_t rip = 128;
inline constexpr std::size_t cf = 136;
inline constexpr std::size_t pf = 137;
inline constexpr std::size_t zf = 138;
inline constexpr std::size_t sf = 139;
inline constexpr std::size_t of = 140;
inline constexpr std::size_t af = 141;
inline constexpr std::size_t df = 142;
inline constexpr std::size_t xmm = 160;
inline constexpr std::size_t mxcsr = 416;
inline constexpr std::size_t hooks = 424;
}  // namespace cpu_offset

static_assert(offsetof(CpuState, gpr) == cpu_offset::gpr);
static_assert(offsetof(CpuState, rip) == cpu_offset::rip);
static_assert(offsetof(CpuState, cf) == cpu_offset::cf);
static_assert(offsetof(CpuState, of) == cpu_offset::of);
static_assert(offsetof(CpuState, af) == cpu_offset::af);
static_assert(offsetof(CpuState, df) == cpu_offset::df);
static_assert(offsetof(CpuState, xmm) == cpu_offset::xmm);
static_assert(offsetof(CpuState, mxcsr) == cpu_offset::mxcsr);
static_assert(offsetof(CpuState, hooks) == cpu_offset::hooks);
static_assert(sizeof(CpuState) == 448);

}  // namespace wse::recomp
