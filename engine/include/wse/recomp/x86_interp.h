// Reference semantics for the recompiler's x86-64 subset (`x86_decode.h`).
//
// `decode_function` decodes one function (a `.pdata` range) into instructions and basic
// blocks; `Interpreter` executes decoded instructions on a `CpuState` against host memory.
// The interpreter is the oracle the LLVM lifter is checked against (R0 (d), R2): both read
// the same `Insn` records, and the flag rules below are the ones the lifter emits.
//
// Guest addresses are host addresses: the image is mapped at its preferred base
// (`docs/RECOMP_PLAN.md` "Runtime model"), and pointer arguments point at host memory.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/x86_decode.h"

namespace wse::recomp {

// The interpreter models the R0 integer subset of `CpuState` (`cpu_state.h`).

struct DecodedFunction {
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::vector<Insn> insns;                 // in address order
    std::vector<std::uint64_t> block_starts; // sorted; the first is `start`
    std::string error;                       // empty when every instruction decoded

    [[nodiscard]] const Insn* at(std::uint64_t va) const noexcept {
        const auto it = std::lower_bound(insns.begin(), insns.end(), va,
                                         [](const Insn& i, std::uint64_t v) { return i.va < v; });
        return (it != insns.end() && it->va == va) ? &*it : nullptr;
    }
};

// Decodes `[start, end)` given its bytes (`code` holds `end - start` bytes). Instructions are
// found by following control flow from `start`, so padding and data after the last `ret`
// are never decoded. A branch leaving the range, or a decode failure, sets `error`.
inline DecodedFunction decode_function(const std::uint8_t* code,
                                       std::uint64_t start,
                                       std::uint64_t end) {
    DecodedFunction fn;
    fn.start = start;
    fn.end = end;
    std::map<std::uint64_t, Insn> found;
    std::set<std::uint64_t> leaders{start};
    std::vector<std::uint64_t> work{start};
    while (!work.empty() && fn.error.empty()) {
        std::uint64_t va = work.back();
        work.pop_back();
        while (fn.error.empty()) {
            if (va < start || va >= end) {
                fn.error = "control flow leaves the function range";
                break;
            }
            if (found.count(va) != 0) {
                break;
            }
            Insn insn{};
            const std::size_t off = static_cast<std::size_t>(va - start);
            if (!decode_insn(code + off, static_cast<std::size_t>(end - va), va, insn, &fn.error)) {
                break;
            }
            found.emplace(va, insn);
            if (insn.op == Op::Jcc || insn.op == Op::Jmp) {
                if (insn.target < start || insn.target >= end) {
                    fn.error = "branch target outside the function (tail call)";
                    break;
                }
                leaders.insert(insn.target);
                work.push_back(insn.target);
            }
            if (insn.op == Op::Jcc) {
                leaders.insert(insn.next_va());
            }
            if (insn.ends_block() && insn.op != Op::Jcc) {
                break;
            }
            va = insn.next_va();
            if (insn.op == Op::Jcc) {
                continue;
            }
        }
    }
    fn.insns.reserve(found.size());
    for (auto& [va, insn] : found) {
        (void)va;
        fn.insns.push_back(insn);
    }
    fn.block_starts.assign(leaders.begin(), leaders.end());
    if (fn.error.empty()) {
        for (const std::uint64_t l : fn.block_starts) {
            if (fn.at(l) == nullptr) {
                fn.error = "branch into the middle of an instruction";
                break;
            }
        }
    }
    return fn;
}

namespace interp_detail {

inline std::uint64_t mask(std::uint8_t size) noexcept {
    return size >= 8 ? ~0ULL : ((1ULL << (8U * size)) - 1ULL);
}
inline bool sign(std::uint64_t v, std::uint8_t size) noexcept {
    return ((v >> (8U * size - 1U)) & 1ULL) != 0;
}
inline bool parity8(std::uint64_t v) noexcept {
    std::uint8_t b = static_cast<std::uint8_t>(v);
    b ^= static_cast<std::uint8_t>(b >> 4);
    b ^= static_cast<std::uint8_t>(b >> 2);
    b ^= static_cast<std::uint8_t>(b >> 1);
    return (b & 1U) == 0;  // PF set when the low byte has an even number of 1 bits
}

}  // namespace interp_detail

[[nodiscard]] inline bool eval_cond(const CpuState& s, Cond c) noexcept {
    switch (c) {
    case Cond::O: return s.of;
    case Cond::NO: return !s.of;
    case Cond::B: return s.cf;
    case Cond::AE: return !s.cf;
    case Cond::E: return s.zf;
    case Cond::NE: return !s.zf;
    case Cond::BE: return s.cf || s.zf;
    case Cond::A: return !s.cf && !s.zf;
    case Cond::S: return s.sf;
    case Cond::NS: return !s.sf;
    case Cond::P: return s.pf;
    case Cond::NP: return !s.pf;
    case Cond::L: return s.sf != s.of;
    case Cond::GE: return s.sf == s.of;
    case Cond::LE: return s.zf || (s.sf != s.of);
    case Cond::G: return !s.zf && (s.sf == s.of);
    }
    return false;
}

class Interpreter {
public:
    // Runs from `s.rip` until a `ret` pops `return_sentinel`, or `max_steps` instructions.
    // `lookup(va)` returns the decoded instruction at `va` or null.
    template <typename Lookup>
    bool run(CpuState& s, std::uint64_t return_sentinel, Lookup&& lookup, std::uint64_t max_steps,
             std::string* error = nullptr) {
        for (std::uint64_t step = 0; step < max_steps; ++step) {
            const Insn* insn = lookup(s.rip);
            if (insn == nullptr) {
                if (error != nullptr) {
                    *error = "no decoded instruction at rip";
                }
                return false;
            }
            execute(s, *insn);
            if (insn->op == Op::Ret && s.rip == return_sentinel) {
                return true;
            }
        }
        if (error != nullptr) {
            *error = "step limit reached";
        }
        return false;
    }

    static void execute(CpuState& s, const Insn& i) noexcept {
        using namespace interp_detail;
        s.rip = i.next_va();
        const std::uint8_t sz = i.dst.size;
        switch (i.op) {
        case Op::Nop:
            return;
        case Op::Mov:
            write(s, i, i.dst, read(s, i, i.src) & mask(sz));
            return;
        case Op::Movzx:
            write(s, i, i.dst, read(s, i, i.src) & mask(i.src.size));
            return;
        case Op::Movsx: {
            const std::uint64_t v = read(s, i, i.src) & mask(i.src.size);
            const std::uint64_t ext = sign(v, i.src.size) ? (v | ~mask(i.src.size)) : v;
            write(s, i, i.dst, ext & mask(sz));
            return;
        }
        case Op::Lea:
            write(s, i, i.dst, address(s, i, i.src) & mask(sz));
            return;
        case Op::Push: {
            std::uint64_t& rsp = s.gpr[static_cast<int>(Reg::rsp)];
            const std::uint64_t v = read(s, i, i.dst);
            rsp -= 8;
            store(rsp, v, 8);
            return;
        }
        case Op::Pop: {
            std::uint64_t& rsp = s.gpr[static_cast<int>(Reg::rsp)];
            const std::uint64_t v = load(rsp, 8);
            rsp += 8;
            write(s, i, i.dst, v);
            return;
        }
        case Op::Ret: {
            std::uint64_t& rsp = s.gpr[static_cast<int>(Reg::rsp)];
            s.rip = load(rsp, 8);
            rsp += 8;
            return;
        }
        case Op::Jmp:
            s.rip = i.target;
            return;
        case Op::Jcc:
            if (eval_cond(s, i.cond)) {
                s.rip = i.target;
            }
            return;
        case Op::Inc:
        case Op::Dec: {
            const std::uint64_t a = read(s, i, i.dst) & mask(sz);
            const std::uint64_t r = (i.op == Op::Inc ? a + 1 : a - 1) & mask(sz);
            const std::uint64_t smin = 1ULL << (8U * sz - 1U);
            s.of = (i.op == Op::Inc) ? (r == smin) : (a == smin);
            set_szp(s, r, sz);
            write(s, i, i.dst, r);
            return;
        }
        case Op::Add:
        case Op::Sub:
        case Op::Cmp:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Test: {
            const std::uint64_t a = read(s, i, i.dst) & mask(sz);
            const std::uint64_t b = read(s, i, i.src) & mask(sz);
            std::uint64_t r = 0;
            switch (i.op) {
            case Op::Add:
                r = (a + b) & mask(sz);
                s.cf = r < a;
                s.of = sign(~(a ^ b) & (a ^ r), sz);
                break;
            case Op::Sub:
            case Op::Cmp:
                r = (a - b) & mask(sz);
                s.cf = a < b;
                s.of = sign((a ^ b) & (a ^ r), sz);
                break;
            case Op::And:
            case Op::Test:
                r = a & b;
                s.cf = s.of = false;
                break;
            case Op::Or:
                r = a | b;
                s.cf = s.of = false;
                break;
            default:
                r = a ^ b;
                s.cf = s.of = false;
                break;
            }
            set_szp(s, r, sz);
            if (i.op != Op::Cmp && i.op != Op::Test) {
                write(s, i, i.dst, r);
            }
            return;
        }
        }
    }

    [[nodiscard]] static std::uint64_t address(const CpuState& s, const Insn& i,
                                               const Operand& m) noexcept {
        std::uint64_t a = static_cast<std::uint64_t>(static_cast<std::int64_t>(m.disp));
        if (m.base == Reg::rip) {
            a += i.next_va();
        } else if (m.base != Reg::none) {
            a += s.gpr[static_cast<int>(m.base)];
        }
        if (m.index != Reg::none) {
            a += s.gpr[static_cast<int>(m.index)] * m.scale;
        }
        return a;
    }

private:
    static void set_szp(CpuState& s, std::uint64_t r, std::uint8_t sz) noexcept {
        s.zf = r == 0;
        s.sf = interp_detail::sign(r, sz);
        s.pf = interp_detail::parity8(r);
    }

    static std::uint64_t load(std::uint64_t addr, std::uint8_t size) noexcept {
        std::uint64_t v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(addr)), size);
        return v;  // little-endian host (x86-64, arm64)
    }
    static void store(std::uint64_t addr, std::uint64_t v, std::uint8_t size) noexcept {
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(addr)), &v, size);
    }

    static std::uint64_t read(const CpuState& s, const Insn& i, const Operand& o) noexcept {
        switch (o.kind) {
        case Operand::Kind::Reg:
            return s.gpr[static_cast<int>(o.reg)] & interp_detail::mask(o.size);
        case Operand::Kind::Mem:
            return load(address(s, i, o), o.size);
        case Operand::Kind::Imm:
            return static_cast<std::uint64_t>(o.imm);
        case Operand::Kind::None:
            break;
        }
        return 0;
    }

    static void write(CpuState& s, const Insn& i, const Operand& o, std::uint64_t v) noexcept {
        if (o.kind == Operand::Kind::Reg) {
            std::uint64_t& r = s.gpr[static_cast<int>(o.reg)];
            if (o.size >= 4) {
                r = v & interp_detail::mask(o.size);  // 32-bit writes zero the upper half
            } else {
                const std::uint64_t m = interp_detail::mask(o.size);
                r = (r & ~m) | (v & m);
            }
        } else if (o.kind == Operand::Kind::Mem) {
            store(address(s, i, o), v, o.size);
        }
    }
};

}  // namespace wse::recomp
