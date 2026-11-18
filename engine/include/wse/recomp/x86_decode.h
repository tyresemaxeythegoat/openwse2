// x86-64 decoder for the Phase 5 recompiler (R0 spike subset, `docs/RECOMP_PLAN.md`).
//
// Decodes one instruction into a small operation/operand record that both the reference
// interpreter (`x86_interp.h`) and the LLVM lifter (`recomp/src/lifter_llvm.cpp`) consume, so
// the two always agree on what an instruction means. The subset is the integer core that
// leaf functions such as `rglString::indexOf` (`0x14030f630`) use: moves, sign/zero
// extension, `lea`, `push`/`pop`, the eight ALU group operations except `adc`/`sbb`, `test`,
// `inc`/`dec`, conditional and unconditional near branches, `ret` and `nop`. Anything else
// fails with the opcode bytes in the message; R1 replaces the opcode table with Zydis.
//
// Not supported yet (decode error): legacy prefixes other than REX (`66`, `F2`, `F3`, `lock`,
// segment overrides), `AH`..`BH` byte registers, SSE, calls and indirect branches.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace wse::recomp {

// General-purpose register numbers in x86 encoding order; `kRip` marks RIP-relative bases.
enum class Reg : std::uint8_t {
    rax = 0, rcx, rdx, rbx, rsp, rbp, rsi, rdi,
    r8, r9, r10, r11, r12, r13, r14, r15,
    rip = 16,
    none = 0xff,
};

enum class Op : std::uint8_t {
    Mov,      // dst = src (zero-extends a 32-bit register destination)
    Movsx,    // dst = sign-extend(src)   (`movsxd`)
    Movzx,    // dst = zero-extend(src)   (`movzx`)
    Lea,      // dst = address of src
    Push,     // rsp -= 8; [rsp] = dst
    Pop,      // dst = [rsp]; rsp += 8
    Add, Or, And, Sub, Xor, Cmp,  // ALU group (flags); `Cmp` writes no register
    Test,     // flags of dst & src
    Inc, Dec, // dst +/- 1; CF unchanged
    Jcc,      // if cond: rip = target
    Jmp,      // rip = target
    Ret,      // rip = [rsp]; rsp += 8
    Nop,
};

// Condition codes in encoding order (`0x70 + cc`).
enum class Cond : std::uint8_t {
    O = 0, NO, B, AE, E, NE, BE, A, S, NS, P, NP, L, GE, LE, G,
};

struct Operand {
    enum class Kind : std::uint8_t { None, Reg, Mem, Imm };
    Kind kind = Kind::None;
    std::uint8_t size = 0;  // bytes: 1, 2, 4 or 8
    Reg reg = Reg::none;    // Kind::Reg
    Reg base = Reg::none;   // Kind::Mem (`Reg::rip` for RIP-relative)
    Reg index = Reg::none;  // Kind::Mem
    std::uint8_t scale = 1; // Kind::Mem
    std::int32_t disp = 0;  // Kind::Mem
    std::int64_t imm = 0;   // Kind::Imm, already sign-extended to 64 bits
};

struct Insn {
    std::uint64_t va = 0;
    std::uint8_t length = 0;
    Op op = Op::Nop;
    Cond cond = Cond::O;
    Operand dst{};
    Operand src{};
    std::uint64_t target = 0;  // Jcc / Jmp

    [[nodiscard]] std::uint64_t next_va() const noexcept { return va + length; }
    [[nodiscard]] bool ends_block() const noexcept {
        return op == Op::Jcc || op == Op::Jmp || op == Op::Ret;
    }
};

namespace decode_detail {

struct Cursor {
    const std::uint8_t* p = nullptr;
    std::size_t avail = 0;
    std::size_t pos = 0;
    bool ok = true;

    std::uint8_t u8() noexcept {
        if (pos >= avail) {
            ok = false;
            return 0;
        }
        return p[pos++];
    }
    std::int32_t s8() noexcept { return static_cast<std::int8_t>(u8()); }
    std::int32_t s32() noexcept {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        }
        return static_cast<std::int32_t>(v);
    }
    std::int64_t s64() noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        return static_cast<std::int64_t>(v);
    }
};

struct Rex {
    bool present = false;
    bool w = false;
    bool r = false;
    bool x = false;
    bool b = false;
};

struct ModRm {
    std::uint8_t mod = 0;
    std::uint8_t reg = 0;  // with REX.R
    Operand rm{};          // register or memory; size filled in by the caller
};

inline Reg gpr(std::uint8_t n) noexcept { return static_cast<Reg>(n & 15U); }

inline bool read_modrm(Cursor& c, const Rex& rex, ModRm& out) noexcept {
    const std::uint8_t m = c.u8();
    out.mod = static_cast<std::uint8_t>(m >> 6);
    out.reg = static_cast<std::uint8_t>(((m >> 3) & 7U) | (rex.r ? 8U : 0U));
    const std::uint8_t rm = m & 7U;
    if (out.mod == 3) {
        out.rm.kind = Operand::Kind::Reg;
        out.rm.reg = gpr(static_cast<std::uint8_t>(rm | (rex.b ? 8U : 0U)));
        return c.ok;
    }
    out.rm.kind = Operand::Kind::Mem;
    if (rm == 4) {
        const std::uint8_t sib = c.u8();
        const std::uint8_t ss = static_cast<std::uint8_t>(sib >> 6);
        const std::uint8_t idx = static_cast<std::uint8_t>(((sib >> 3) & 7U) | (rex.x ? 8U : 0U));
        const std::uint8_t base = sib & 7U;
        out.rm.scale = static_cast<std::uint8_t>(1U << ss);
        out.rm.index = (idx == 4) ? Reg::none : gpr(idx);
        if (base == 5 && out.mod == 0) {
            out.rm.base = Reg::none;
            out.rm.disp = c.s32();
            return c.ok;
        }
        out.rm.base = gpr(static_cast<std::uint8_t>(base | (rex.b ? 8U : 0U)));
    } else if (rm == 5 && out.mod == 0) {
        out.rm.base = Reg::rip;
        out.rm.disp = c.s32();
        return c.ok;
    } else {
        out.rm.base = gpr(static_cast<std::uint8_t>(rm | (rex.b ? 8U : 0U)));
    }
    if (out.mod == 1) {
        out.rm.disp = c.s8();
    } else if (out.mod == 2) {
        out.rm.disp = c.s32();
    }
    return c.ok;
}

inline Operand reg_operand(std::uint8_t n, std::uint8_t size) noexcept {
    Operand o{};
    o.kind = Operand::Kind::Reg;
    o.reg = gpr(n);
    o.size = size;
    return o;
}

inline Operand imm_operand(std::int64_t v, std::uint8_t size) noexcept {
    Operand o{};
    o.kind = Operand::Kind::Imm;
    o.imm = v;
    o.size = size;
    return o;
}

// `AH`..`BH` are encoded as registers 4..7 of a byte operand without a REX prefix.
inline bool high_byte_reg(const Operand& o, const Rex& rex) noexcept {
    return o.kind == Operand::Kind::Reg && o.size == 1 && !rex.present &&
           static_cast<std::uint8_t>(o.reg) >= 4 && static_cast<std::uint8_t>(o.reg) <= 7;
}

}  // namespace decode_detail

// Decodes the instruction at `bytes` (at most `avail` bytes readable) whose address is `va`.
// On failure returns false and, when `error` is set, describes the unsupported encoding.
inline bool decode_insn(const std::uint8_t* bytes,
                        std::size_t avail,
                        std::uint64_t va,
                        Insn& out,
                        std::string* error = nullptr) {
    using namespace decode_detail;
    Cursor c{bytes, avail, 0, true};
    out = Insn{};
    out.va = va;
    const auto fail = [&](const char* what) {
        if (error != nullptr) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "0x%llx: %s (bytes", static_cast<unsigned long long>(va),
                          what);
            std::string msg = buf;
            for (std::size_t i = 0; i < 6 && i < avail; ++i) {
                std::snprintf(buf, sizeof(buf), " %02x", bytes[i]);
                msg += buf;
            }
            msg += ")";
            *error = msg;
        }
        return false;
    };

    Rex rex{};
    std::uint8_t b = c.u8();
    if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 || b == 0x2E || b == 0x26 ||
        b == 0x36 || b == 0x3E || b == 0x64 || b == 0x65) {
        return fail("unsupported legacy prefix");
    }
    if ((b & 0xF0U) == 0x40U) {
        rex.present = true;
        rex.w = (b & 8U) != 0;
        rex.r = (b & 4U) != 0;
        rex.x = (b & 2U) != 0;
        rex.b = (b & 1U) != 0;
        b = c.u8();
    }
    const std::uint8_t osize = rex.w ? 8 : 4;
    ModRm mr{};

    const auto finish = [&]() {
        if (!c.ok) {
            return fail("truncated instruction");
        }
        if (high_byte_reg(out.dst, rex) || high_byte_reg(out.src, rex)) {
            return fail("AH/CH/DH/BH byte register");
        }
        out.length = static_cast<std::uint8_t>(c.pos);
        return true;
    };
    const auto rel_target = [&](std::int64_t rel) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(va + c.pos) + rel);
    };

    // ALU group 00..3D: add, or, adc, sbb, and, sub, xor, cmp.
    if (b < 0x40 && (b & 7U) <= 5U) {
        static constexpr Op kAlu[8] = {Op::Add, Op::Or, Op::Nop, Op::Nop,
                                       Op::And, Op::Sub, Op::Xor, Op::Cmp};
        const std::uint8_t group = static_cast<std::uint8_t>(b >> 3);
        if (group == 2 || group == 3) {
            return fail("adc/sbb");
        }
        out.op = kAlu[group];
        const std::uint8_t form = b & 7U;
        const std::uint8_t size = (form == 0 || form == 2 || form == 4) ? 1 : osize;
        if (form <= 3) {
            if (!read_modrm(c, rex, mr)) {
                return fail("truncated ModRM");
            }
            mr.rm.size = size;
            const Operand r = reg_operand(mr.reg, size);
            if (form <= 1) {
                out.dst = mr.rm;
                out.src = r;
            } else {
                out.dst = r;
                out.src = mr.rm;
            }
        } else {
            out.dst = reg_operand(0, size);
            out.src = imm_operand(size == 1 ? c.s8() : c.s32(), size);
        }
        return finish();
    }
    if (b >= 0x50 && b <= 0x5F) {
        out.op = (b < 0x58) ? Op::Push : Op::Pop;
        out.dst = reg_operand(static_cast<std::uint8_t>((b & 7U) | (rex.b ? 8U : 0U)), 8);
        return finish();
    }
    if (b == 0x63) {
        if (!rex.w) {
            return fail("movsxd without REX.W");
        }
        if (!read_modrm(c, rex, mr)) {
            return fail("truncated ModRM");
        }
        out.op = Op::Movsx;
        out.dst = reg_operand(mr.reg, 8);
        out.src = mr.rm;
        out.src.size = 4;
        return finish();
    }
    if (b >= 0x70 && b <= 0x7F) {
        out.op = Op::Jcc;
        out.cond = static_cast<Cond>(b - 0x70);
        const std::int32_t rel = c.s8();
        out.target = rel_target(rel);
        return finish();
    }
    if (b == 0x80 || b == 0x81 || b == 0x83) {
        if (!read_modrm(c, rex, mr)) {
            return fail("truncated ModRM");
        }
        static constexpr Op kAlu[8] = {Op::Add, Op::Or, Op::Nop, Op::Nop,
                                       Op::And, Op::Sub, Op::Xor, Op::Cmp};
        const std::uint8_t ext = mr.reg & 7U;
        if (ext == 2 || ext == 3) {
            return fail("adc/sbb immediate");
        }
        out.op = kAlu[ext];
        const std::uint8_t size = (b == 0x80) ? 1 : osize;
        mr.rm.size = size;
        out.dst = mr.rm;
        const std::int64_t imm = (b == 0x81) ? c.s32() : c.s8();
        out.src = imm_operand(imm, size);
        return finish();
    }
    if (b == 0x84 || b == 0x85) {
        if (!read_modrm(c, rex, mr)) {
            return fail("truncated ModRM");
        }
        const std::uint8_t size = (b == 0x84) ? 1 : osize;
        out.op = Op::Test;
        mr.rm.size = size;
        out.dst = mr.rm;
        out.src = reg_operand(mr.reg, size);
        return finish();
    }
    if (b >= 0x88 && b <= 0x8B) {
        if (!read_modrm(c, rex, mr)) {
            return fail("truncated ModRM");
        }
        const std::uint8_t size = (b == 0x88 || b == 0x8A) ? 1 : osize;
        out.op = Op::Mov;
        mr.rm.size = size;
        const Operand r = reg_operand(mr.reg, size);
        if (b <= 0x89) {
            out.dst = mr.rm;
            out.src = r;
        } else {
            out.dst = r;
            out.src = mr.rm;
        }
        return finish();
    }
    if (b == 0x8D) {
        if (!read_modrm(c, rex, mr) || mr.rm.kind != Operand::Kind::Mem) {
            return fail("lea with register operand");
        }
        out.op = Op::Lea;
        out.dst = reg_operand(mr.reg, osize);
        out.src = mr.rm;
        out.src.size = osize;
        return finish();
    }
    if (b == 0x90 && !rex.b) {
        out.op = Op::Nop;
        return finish();
    }
    if (b >= 0xB8 && b <= 0xBF) {
        out.op = Op::Mov;
        out.dst = reg_operand(static_cast<std::uint8_t>((b & 7U) | (rex.b ? 8U : 0U)), osize);
        out.src = imm_operand(rex.w ? c.s64() : static_cast<std::int64_t>(static_cast<std::uint32_t>(c.s32())),
                              osize);
        return finish();
    }
    if (b == 0xC3) {
        out.op = Op::Ret;
        return finish();
    }
    if (b == 0xC6 || b == 0xC7) {
        if (!read_modrm(c, rex, mr) || (mr.reg & 7U) != 0) {
            return fail("C6/C7 with non-zero /reg");
        }
        const std::uint8_t size = (b == 0xC6) ? 1 : osize;
        out.op = Op::Mov;
        mr.rm.size = size;
        out.dst = mr.rm;
        out.src = imm_operand(size == 1 ? c.s8() : c.s32(), size);
        return finish();
    }
    if (b == 0xE9 || b == 0xEB) {
        out.op = Op::Jmp;
        const std::int32_t rel = (b == 0xEB) ? c.s8() : c.s32();
        out.target = rel_target(rel);
        return finish();
    }
    if (b == 0xF6 || b == 0xF7) {
        if (!read_modrm(c, rex, mr) || (mr.reg & 7U) != 0) {
            return fail("F6/F7 other than test");
        }
        const std::uint8_t size = (b == 0xF6) ? 1 : osize;
        out.op = Op::Test;
        mr.rm.size = size;
        out.dst = mr.rm;
        out.src = imm_operand(size == 1 ? c.s8() : c.s32(), size);
        return finish();
    }
    if (b == 0xFE || b == 0xFF) {
        if (!read_modrm(c, rex, mr)) {
            return fail("truncated ModRM");
        }
        const std::uint8_t ext = mr.reg & 7U;
        if (ext > 1) {
            return fail("FE/FF other than inc/dec (call/jmp/push)");
        }
        out.op = (ext == 0) ? Op::Inc : Op::Dec;
        mr.rm.size = (b == 0xFE) ? 1 : osize;
        out.dst = mr.rm;
        return finish();
    }
    if (b == 0x0F) {
        const std::uint8_t b2 = c.u8();
        if (b2 >= 0x80 && b2 <= 0x8F) {
            out.op = Op::Jcc;
            out.cond = static_cast<Cond>(b2 - 0x80);
            const std::int32_t rel = c.s32();
            out.target = rel_target(rel);
            return finish();
        }
        if (b2 == 0xB6 || b2 == 0xB7) {
            if (!read_modrm(c, rex, mr)) {
                return fail("truncated ModRM");
            }
            out.op = Op::Movzx;
            out.dst = reg_operand(mr.reg, osize);
            out.src = mr.rm;
            out.src.size = (b2 == 0xB6) ? 1 : 2;
            return finish();
        }
        if (b2 == 0xBE || b2 == 0xBF) {
            if (!read_modrm(c, rex, mr)) {
                return fail("truncated ModRM");
            }
            out.op = Op::Movsx;
            out.dst = reg_operand(mr.reg, osize);
            out.src = mr.rm;
            out.src.size = (b2 == 0xBE) ? 1 : 2;
            return finish();
        }
        if (b2 == 0x1F) {
            if (!read_modrm(c, rex, mr)) {
                return fail("truncated ModRM");
            }
            out.op = Op::Nop;
            return finish();
        }
        return fail("unsupported 0F opcode");
    }
    return fail("unsupported opcode");
}

}  // namespace wse::recomp
