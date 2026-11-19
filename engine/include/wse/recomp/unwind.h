// Win64 virtual unwinding over a guest image's own `.pdata` / `.xdata` (Phase 5 R3,
// `docs/RECOMP_PLAN.md` "R3 design"): what `RtlLookupFunctionEntry` and `RtlVirtualUnwind` do on
// Windows, for the runtime's exception dispatch and for the exe's own calls to those functions.
//
// Translated code keeps every guest frame in its Win64 layout on the guest stack, so unwinding
// reads guest memory exactly as Windows reads the native stack. The algorithm follows the
// documented x64 unwind format ("x64 exception handling", Microsoft docs): UNWIND_INFO versions 1
// and 2, UNWIND_CODEs, chained info, a partly executed prolog (only codes at or before the offset
// apply), an epilog detected from its instructions (`add rsp`/`lea rsp`, pops, `ret`/`jmp`), and
// machine frames. A function with no `.pdata` entry is a leaf: its return address is at `[rsp]`.
// Header-only; guest memory is host memory.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/pe_image.h"

namespace wse::recomp::unwind {

// UNWIND_INFO flags.
inline constexpr std::uint8_t kFlagExceptionHandler = 1;
inline constexpr std::uint8_t kFlagTerminationHandler = 2;
inline constexpr std::uint8_t kFlagChainInfo = 4;

enum Op : std::uint8_t {
    kPushNonvol = 0,
    kAllocLarge = 1,
    kAllocSmall = 2,
    kSetFpreg = 3,
    kSaveNonvol = 4,
    kSaveNonvolFar = 5,
    kEpilog = 6,  // version 2 only; describes epilogs, ignored when unwinding
    kSpareCode = 7,
    kSaveXmm128 = 8,
    kSaveXmm128Far = 9,
    kPushMachframe = 10,
};

// The image the unwinder reads: mapped at `base`, `size` bytes, `.pdata` sorted by begin RVA.
struct Image {
    const std::uint8_t* image = nullptr;
    std::size_t size = 0;
    std::uint64_t base = 0;
    const std::vector<PeFunctionRange>* functions = nullptr;
};

// `RtlLookupFunctionEntry`: the `.pdata` entry containing `pc`, or null.
inline const PeFunctionRange* lookup_function(const Image& img, std::uint64_t pc) noexcept {
    if (img.functions == nullptr || pc < img.base || pc - img.base >= img.size) {
        return nullptr;
    }
    const auto rva = static_cast<std::uint32_t>(pc - img.base);
    const auto& f = *img.functions;
    auto it = std::upper_bound(f.begin(), f.end(), rva,
                               [](std::uint32_t v, const PeFunctionRange& r) { return v < r.begin; });
    if (it == f.begin()) {
        return nullptr;
    }
    --it;
    return rva >= it->begin && rva < it->end ? &*it : nullptr;
}

struct Result {
    bool ok = false;
    std::string error;
    std::uint64_t establisher_frame = 0;   // rsp (or frame register base) of the primary frame
    std::uint64_t handler = 0;             // language handler VA, 0 when none of the requested kind
    std::uint64_t handler_data = 0;        // VA of the handler data after the handler RVA
};

namespace detail {

inline std::uint64_t rd64(std::uint64_t va) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(va), 8);
    return v;
}

inline bool image_byte(const Image& img, std::uint64_t va, std::uint8_t& out) noexcept {
    if (va < img.base || va - img.base >= img.size) {
        return false;
    }
    out = img.image[va - img.base];
    return true;
}

// Version-1 epilog detection from the instructions at `pc` (the documented epilog forms):
// an optional `add rsp, imm` / `lea rsp, [frame + disp]`, then `pop` of nonvolatile registers,
// then `ret`, `ret n`, `rep ret`, or a `jmp` leaving the function. Emulates it into `ctx` and
// returns true when `pc` is inside one.
inline bool unwind_epilog(const Image& img, std::uint64_t pc, const PeFunctionRange& fn, std::uint8_t frame_reg,
                          CpuState& ctx) {
    std::uint8_t b[16] = {};
    for (std::uint64_t i = 0; i < 16; ++i) {
        if (!image_byte(img, pc + i, b[i])) {
            b[i] = 0;
        }
    }
    std::uint64_t rsp = ctx.gpr[4];
    std::size_t p = 0;
    // add rsp, imm8 / imm32
    if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xC4) {
        rsp += static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(b[3])));
        p = 4;
    } else if (b[0] == 0x48 && b[1] == 0x81 && b[2] == 0xC4) {
        std::int32_t imm = 0;
        std::memcpy(&imm, b + 3, 4);
        rsp += static_cast<std::uint64_t>(static_cast<std::int64_t>(imm));
        p = 7;
    } else if ((b[0] & 0xFE) == 0x48 && b[1] == 0x8D && frame_reg != 0) {
        // lea rsp, [frame_reg + disp8/32] (REX.B selects r8..r15)
        const std::uint8_t modrm = b[2];
        const int reg = ((b[0] & 1) << 3) | (modrm & 7);
        if (((modrm >> 3) & 7) != 4 || reg != frame_reg) {
            return false;
        }
        if ((modrm >> 6) == 1) {
            rsp = ctx.gpr[reg] + static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int8_t>(b[3])));
            p = 4;
        } else if ((modrm >> 6) == 2) {
            std::int32_t d = 0;
            std::memcpy(&d, b + 3, 4);
            rsp = ctx.gpr[reg] + static_cast<std::uint64_t>(static_cast<std::int64_t>(d));
            p = 7;
        } else {
            return false;
        }
    }
    // Pops of nonvolatile registers, then the return.
    std::uint64_t regs[16];
    std::memcpy(regs, ctx.gpr, sizeof(regs));
    std::uint64_t sp = rsp;
    for (int guard = 0; guard < 16 && p < 15; ++guard) {
        if (b[p] >= 0x58 && b[p] <= 0x5F) {
            regs[b[p] - 0x58] = rd64(sp);
            sp += 8;
            p += 1;
        } else if (b[p] == 0x41 && b[p + 1] >= 0x58 && b[p + 1] <= 0x5F) {
            regs[8 + b[p + 1] - 0x58] = rd64(sp);
            sp += 8;
            p += 2;
        } else {
            break;
        }
    }
    const bool ret = b[p] == 0xC3 || b[p] == 0xC2 || (b[p] == 0xF3 && b[p + 1] == 0xC3);
    bool jmp_out = false;
    if (b[p] == 0xE9) {
        std::int32_t rel = 0;
        std::memcpy(&rel, b + p + 1, 4);
        const std::uint64_t target = pc + p + 5 + static_cast<std::uint64_t>(static_cast<std::int64_t>(rel));
        jmp_out = target < img.base + fn.begin || target >= img.base + fn.end;
    } else if (b[p] == 0xFF && b[p + 1] == 0x25) {
        jmp_out = true;  // jmp [rip + disp]: an import or a tail call
    } else if (b[p] == 0x48 && b[p + 1] == 0xFF && (b[p + 2] & 0x38) == 0x20) {
        jmp_out = true;  // rex.w jmp r/m64
    }
    if (!ret && !jmp_out) {
        return false;
    }
    std::memcpy(ctx.gpr, regs, sizeof(regs));
    ctx.rip = rd64(sp);
    ctx.gpr[4] = sp + 8;
    return true;
}

inline std::size_t code_slots(std::uint8_t op, std::uint8_t info) noexcept {
    switch (op) {
    case kAllocLarge: return info == 0 ? 2 : 3;
    case kSaveNonvol:
    case kSaveXmm128: return 2;
    case kSaveNonvolFar:
    case kSaveXmm128Far: return 3;
    default: return 1;
    }
}

}  // namespace detail

// `RtlVirtualUnwind`: turns `ctx` (rip, rsp, nonvolatile registers of a frame) into its caller's
// context. `handler_mask` selects which handler to return (kFlagExceptionHandler while
// dispatching, kFlagTerminationHandler while unwinding, 0 for none).
inline Result virtual_unwind(const Image& img, CpuState& ctx, std::uint8_t handler_mask) {
    Result r;
    const PeFunctionRange* fn = lookup_function(img, ctx.rip);
    if (fn == nullptr) {  // a leaf: nothing saved, the return address is at [rsp]
        r.establisher_frame = ctx.gpr[4];
        ctx.rip = detail::rd64(ctx.gpr[4]);
        ctx.gpr[4] += 8;
        r.ok = true;
        return r;
    }
    const PeFunctionRange* primary = fn;
    std::uint64_t offset = ctx.rip - (img.base + fn->begin);
    bool machine_frame = false;
    bool first = true;
    std::uint64_t handler_rva = 0;
    std::uint64_t handler_data = 0;
    for (int chain = 0; chain < 32; ++chain) {
        const std::uint64_t ui = fn->unwind;
        if (ui + 4 > img.size) {
            r.error = "UNWIND_INFO out of the image";
            return r;
        }
        const std::uint8_t* u = img.image + ui;
        const std::uint8_t version = u[0] & 7;
        const std::uint8_t flags = u[0] >> 3;
        const std::uint8_t prolog = u[1];
        const std::uint8_t count = u[2];
        const std::uint8_t frame_reg = u[3] & 15;
        const std::uint8_t frame_off = u[3] >> 4;
        if ((version != 1 && version != 2) || ui + 4 + 2ULL * ((count + 1U) & ~1U) > img.size) {
            r.error = "bad UNWIND_INFO";
            return r;
        }
        const std::uint8_t* codes = u + 4;
        // In an epilog of the primary function: emulate it (no handler runs there). The
        // establisher frame is the frame's `rsp` before the epilog runs, not after.
        const std::uint64_t frame_rsp = ctx.gpr[4];
        if (first && offset >= prolog && detail::unwind_epilog(img, ctx.rip, *fn, frame_reg, ctx)) {
            r.establisher_frame = frame_rsp;
            r.ok = true;
            return r;
        }
        // The frame base the save offsets are relative to.
        std::uint64_t frame = ctx.gpr[4];
        if (frame_reg != 0) {
            bool fp_set = offset >= prolog;
            for (std::size_t i = 0; i < count && !fp_set; i += detail::code_slots(codes[2 * i + 1] & 15, codes[2 * i + 1] >> 4)) {
                fp_set = (codes[2 * i + 1] & 15) == kSetFpreg && codes[2 * i] <= offset;
            }
            if (fp_set) {
                frame = ctx.gpr[frame_reg] - 16ULL * frame_off;
            }
        }
        r.establisher_frame = frame;
        for (std::size_t i = 0; i < count;) {
            const std::uint8_t at = codes[2 * i];
            const std::uint8_t op = codes[2 * i + 1] & 15;
            const std::uint8_t info = codes[2 * i + 1] >> 4;
            const std::size_t slots = detail::code_slots(op, info);
            if (i + slots > count) {
                r.error = "truncated UNWIND_CODE";
                return r;
            }
            auto u16 = [&](std::size_t k) {
                std::uint16_t v = 0;
                std::memcpy(&v, codes + 2 * k, 2);
                return v;
            };
            auto u32 = [&](std::size_t k) {
                std::uint32_t v = 0;
                std::memcpy(&v, codes + 2 * k, 4);
                return v;
            };
            if (at <= offset || !first) {
                switch (op) {
                case kPushNonvol:
                    ctx.gpr[info] = detail::rd64(ctx.gpr[4]);
                    ctx.gpr[4] += 8;
                    break;
                case kAllocLarge:
                    ctx.gpr[4] += info == 0 ? 8ULL * u16(i + 1) : u32(i + 1);
                    break;
                case kAllocSmall:
                    ctx.gpr[4] += 8ULL * info + 8;
                    break;
                case kSetFpreg:
                    ctx.gpr[4] = ctx.gpr[frame_reg] - 16ULL * frame_off;
                    break;
                case kSaveNonvol:
                    ctx.gpr[info] = detail::rd64(frame + 8ULL * u16(i + 1));
                    break;
                case kSaveNonvolFar:
                    ctx.gpr[info] = detail::rd64(frame + u32(i + 1));
                    break;
                case kSaveXmm128:
                    std::memcpy(ctx.xmm[info], reinterpret_cast<const void*>(frame + 16ULL * u16(i + 1)), 16);
                    break;
                case kSaveXmm128Far:
                    std::memcpy(ctx.xmm[info], reinterpret_cast<const void*>(frame + u32(i + 1)), 16);
                    break;
                case kPushMachframe:
                    if (info != 0) {
                        ctx.gpr[4] += 8;  // an error code was pushed
                    }
                    ctx.rip = detail::rd64(ctx.gpr[4]);
                    ctx.gpr[4] = detail::rd64(ctx.gpr[4] + 24);
                    machine_frame = true;
                    break;
                case kEpilog:
                case kSpareCode:
                default:
                    break;
                }
            }
            i += slots;
        }
        // Version 2 adds kEpilog codes describing epilogs; they are skipped above (an epilog is
        // detected from its instructions instead), and the prolog codes apply as in version 1.
        const std::size_t after = 4 + 2ULL * ((count + 1U) & ~1U);
        if ((flags & kFlagChainInfo) != 0) {
            if (ui + after + 12 > img.size) {
                r.error = "truncated chained RUNTIME_FUNCTION";
                return r;
            }
            static thread_local PeFunctionRange parent;
            std::memcpy(&parent.begin, u + after, 4);
            std::memcpy(&parent.end, u + after + 4, 4);
            std::memcpy(&parent.unwind, u + after + 8, 4);
            fn = &parent;
            primary = fn;
            offset = ~0ULL;  // the parent's prolog ran in full before this fragment
            first = false;
            continue;
        }
        if ((flags & handler_mask & (kFlagExceptionHandler | kFlagTerminationHandler)) != 0) {
            std::uint32_t h = 0;
            std::memcpy(&h, u + after, 4);
            handler_rva = h;
            handler_data = img.base + ui + after + 4;
        }
        break;
    }
    (void)primary;
    if (!machine_frame) {
        ctx.rip = detail::rd64(ctx.gpr[4]);
        ctx.gpr[4] += 8;
    }
    if (handler_rva != 0) {
        r.handler = img.base + handler_rva;
        r.handler_data = handler_data;
    }
    r.ok = true;
    return r;
}

}  // namespace wse::recomp::unwind
