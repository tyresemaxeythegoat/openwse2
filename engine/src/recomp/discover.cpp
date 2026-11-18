// Function discovery for the R1 lifter. See `discover.h`.

#include "discover.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace wse::recomp {

bool ImageView::read32(std::uint64_t va, std::uint32_t& out) const noexcept {
    if (!readable(va, 4)) {
        return false;
    }
    std::memcpy(&out, ptr(va), 4);
    return true;
}

bool ImageView::read8(std::uint64_t va, std::uint8_t& out) const noexcept {
    if (!readable(va, 1)) {
        return false;
    }
    out = *ptr(va);
    return true;
}

Discoverer::Discoverer(const ImageView& view) : view_(view) {
    ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
}

bool Discoverer::decode(std::uint64_t va, DInsn& out) const {
    if (!view_.in_text(va) || !view_.readable(va, 1)) {
        return false;
    }
    std::size_t avail = 15;
    while (avail > 0 && !view_.readable(va, avail)) {
        --avail;
    }
    out.va = va;
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder_, view_.ptr(va), avail, &out.ins, out.ops));
}

namespace {

bool is_reg(const ZydisDecodedOperand& o, ZydisRegister r) {
    return o.type == ZYDIS_OPERAND_TYPE_REGISTER && o.reg.value == r;
}

// The 64-bit register that contains `r` (e.g. ECX -> RCX).
ZydisRegister widen(ZydisRegister r) {
    return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
}

std::uint64_t branch_target(const DInsn& d) {
    ZyanU64 t = 0;
    if (d.ins.operand_count_visible > 0 &&
        ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&d.ins, &d.ops[0], d.va, &t))) {
        return t;
    }
    return 0;
}

// A register `jmp` preceded (within 6 instructions) by `add reg, reg` of a table entry: the shape
// of an MSVC switch that the resolver could not bound. Anything else is a tail call.
bool looks_like_switch(const DFunction& fn, const DInsn& jmp) {
    if (jmp.ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) {
        return false;
    }
    auto it = fn.insns.find(jmp.va);
    for (int k = 0; k < 6 && it != fn.insns.begin(); ++k) {
        --it;
        const DInsn& d = it->second;
        if (d.ins.mnemonic == ZYDIS_MNEMONIC_ADD && d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            return true;
        }
    }
    return false;
}

}  // namespace

// MSVC x64 switch lowering, in the shapes MSVC emits:
//   cmp   idx, N ; ja default                           (bound)
//   [movzx idx, byte [base + idx2 + byte_rva]]          (optional byte index table)
//   lea   base, [rip + __ImageBase]                     (or mov base, imm = image base)
//   mov   r32, dword [base + idx*4 + table_rva]         (entry = RVA of the case)
//   add   r64, base
//   jmp   r64
// and the clang/LLVM shape, whose entries are signed offsets from the table itself:
//   lea    base, [rip + table]
//   movsxd r64, dword [base + idx*4]
//   add    r64, base
//   jmp    r64
bool Discoverer::resolve_jump_table(const DFunction& fn, const DInsn& jmp, JumpTable& out) const {
    if (jmp.ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER) {
        return false;
    }
    const ZydisRegister target = widen(jmp.ops[0].reg.value);
    // Walk back through the straight-line instructions before the jmp.
    std::vector<const DInsn*> prev;
    auto it = fn.insns.find(jmp.va);
    while (it != fn.insns.begin() && prev.size() < 24) {
        --it;
        if (it->second.next() != (prev.empty() ? jmp.va : prev.back()->va)) {
            break;
        }
        prev.push_back(&it->second);
    }
    const std::uint64_t base = view_.base();
    ZydisRegister base_reg = ZYDIS_REGISTER_NONE;
    ZydisRegister index_reg = ZYDIS_REGISTER_NONE;
    std::uint64_t table = 0;
    std::int64_t table_disp = 0;
    std::uint64_t entry_load_va = 0;
    bool have_entry_load = false;
    bool signed_entries = false;
    bool have_add = false;
    for (const DInsn* p : prev) {
        const auto& i = p->ins;
        if (!have_add && i.mnemonic == ZYDIS_MNEMONIC_ADD && is_reg(p->ops[0], target) &&
            p->ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            base_reg = widen(p->ops[1].reg.value);
            have_add = true;
            continue;
        }
        if (have_add && !have_entry_load &&
            (i.mnemonic == ZYDIS_MNEMONIC_MOV || i.mnemonic == ZYDIS_MNEMONIC_MOVSXD) &&
            p->ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && widen(p->ops[0].reg.value) == target &&
            p->ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY && p->ops[1].mem.scale == 4 &&
            widen(p->ops[1].mem.base) == base_reg) {
            index_reg = widen(p->ops[1].mem.index);
            table_disp = p->ops[1].mem.disp.value;
            signed_entries = i.mnemonic == ZYDIS_MNEMONIC_MOVSXD;
            have_entry_load = true;
            entry_load_va = p->va;
            continue;
        }
    }
    if (!have_entry_load || index_reg == ZYDIS_REGISTER_NONE) {
        return false;
    }
    // The base register must hold the image base. Its load may sit in an earlier block, so
    // search every instruction before the table load (nearest first) for the last write to it
    // (the register may be reused between the `add` and the `jmp`).
    bool base_ok = false;
    bool table_relative = false;  // clang shape: entries are offsets from the table
    for (auto r = fn.insns.rbegin(); r != fn.insns.rend(); ++r) {
        const DInsn* p = &r->second;
        if (p->va >= entry_load_va) {
            continue;
        }
        if (p->ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && widen(p->ops[0].reg.value) == base_reg &&
            (p->ops[0].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0) {
            std::uint64_t v = 0;
            if (p->ins.mnemonic == ZYDIS_MNEMONIC_LEA &&
                ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&p->ins, &p->ops[1], p->va, &v))) {
                base_ok = (v == base);
                if (!base_ok && table_disp == 0 && signed_entries && view_.readable(v, 4)) {
                    base_ok = true;
                    table_relative = true;
                    table = v;
                }
            } else if (p->ins.mnemonic == ZYDIS_MNEMONIC_MOV &&
                       p->ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                base_ok = (p->ops[1].imm.value.u == base);
            }
            break;
        }
    }
    if (!base_ok) {
        return false;
    }
    if (!table_relative) {
        table = base + static_cast<std::uint64_t>(table_disp);
    }
    // Optional byte index table feeding the dword index, and the bound.
    std::uint64_t byte_table = 0;
    ZydisRegister bound_reg = index_reg;
    std::size_t alias_from = 0;  // copies are searched before this position in `prev`
    for (std::size_t k = 0; k < prev.size(); ++k) {
        const DInsn* p = prev[k];
        if (!table_relative && p->ins.mnemonic == ZYDIS_MNEMONIC_MOVZX && p->ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            widen(p->ops[0].reg.value) == index_reg && p->ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
            p->ops[1].size == 8 && widen(p->ops[1].mem.base) == base_reg) {
            byte_table = base + static_cast<std::uint64_t>(p->ops[1].mem.disp.value);
            bound_reg = widen(p->ops[1].mem.index);
            alias_from = k + 1;  // the movzx itself rewrites the register; look before it
            break;
        }
    }
    // The index is often a sign- or zero-extended copy of the compared register
    // (`movsxd rcx, eax`, `mov ecx, eax`, `cdqe`): follow such copies back.
    for (int hop = 0; hop < 3; ++hop) {
        bool moved = false;
        for (std::size_t k = alias_from; k < prev.size(); ++k) {
            const DInsn* p = prev[k];
            const auto mn = p->ins.mnemonic;
            if (mn == ZYDIS_MNEMONIC_CDQE && bound_reg == ZYDIS_REGISTER_RAX) {
                break;  // rax <- eax: same register
            }
            if ((mn == ZYDIS_MNEMONIC_MOVSXD || mn == ZYDIS_MNEMONIC_MOV || mn == ZYDIS_MNEMONIC_MOVZX) &&
                p->ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && widen(p->ops[0].reg.value) == bound_reg &&
                p->ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                const ZydisRegister src = widen(p->ops[1].reg.value);
                if (src != bound_reg) {
                    bound_reg = src;
                    moved = true;
                }
                break;
            }
            if (p->ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && widen(p->ops[0].reg.value) == bound_reg &&
                (p->ops[0].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0) {
                break;  // defined some other way: stop
            }
        }
        if (!moved) {
            break;
        }
    }
    // The bound: `cmp bound_reg, N` followed by `ja`, anywhere in the function before the jmp
    // (the compare may sit in a dominating block).
    std::int64_t limit = -1;
    for (auto r = fn.insns.rbegin(); r != fn.insns.rend(); ++r) {
        const DInsn& d = r->second;
        if (d.va >= jmp.va) {
            continue;
        }
        if (d.ins.mnemonic == ZYDIS_MNEMONIC_CMP && d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            widen(d.ops[0].reg.value) == bound_reg && d.ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            const DInsn* nx = fn.at(d.next());
            if (nx != nullptr && nx->ins.mnemonic == ZYDIS_MNEMONIC_JNBE) {
                limit = d.ops[1].imm.value.s;
                break;
            }
        }
    }
    if (limit < 0) {
        // No `cmp`/`ja`: an index masked by `and idx, 2^k - 1` (clang lowers `switch (x & 7)`
        // this way) is bounded by the mask. It must be the last write to the index register
        // before the table load, possibly in an earlier block (threaded switches reuse it).
        for (auto r = fn.insns.rbegin(); r != fn.insns.rend(); ++r) {
            const DInsn* p = &r->second;
            if (p->ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER || widen(p->ops[0].reg.value) != bound_reg ||
                (p->ops[0].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) == 0 || p->va >= entry_load_va) {
                continue;
            }
            if (p->ins.mnemonic == ZYDIS_MNEMONIC_AND && p->ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                p->ops[1].imm.value.s >= 0) {
                limit = p->ops[1].imm.value.s;
            }
            break;
        }
    }
    if (limit < 0 || limit > 65535) {
        return false;
    }
    std::vector<std::uint32_t> slots;
    if (byte_table != 0) {
        std::uint8_t maxb = 0;
        for (std::int64_t k = 0; k <= limit; ++k) {
            std::uint8_t b = 0;
            if (!view_.read8(byte_table + static_cast<std::uint64_t>(k), b)) {
                return false;
            }
            maxb = std::max(maxb, b);
        }
        for (std::uint32_t k = 0; k <= maxb; ++k) {
            slots.push_back(k);
        }
    } else {
        for (std::int64_t k = 0; k <= limit; ++k) {
            slots.push_back(static_cast<std::uint32_t>(k));
        }
    }
    out.jmp_va = jmp.va;
    for (const std::uint32_t k : slots) {
        std::uint32_t rva = 0;
        if (!view_.read32(table + 4ULL * k, rva)) {
            return false;
        }
        const std::uint64_t t = table_relative
                                    ? table + static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(rva)))
                                    : base + rva;
        if (!view_.in_text(t)) {
            return false;
        }
        out.targets.push_back(t);
    }
    return !out.targets.empty();
}

DFunction Discoverer::discover(std::uint64_t entry, const std::set<std::uint64_t>& entries) const {
    DFunction fn;
    fn.entry = entry;
    fn.block_starts.insert(entry);
    std::vector<std::uint64_t> work{entry};
    std::size_t budget = 400000;  // instructions; bounds pathological inputs
    while (!work.empty()) {
        std::uint64_t va = work.back();
        work.pop_back();
        while (fn.insns.count(va) == 0) {
            if (--budget == 0) {
                fn.error = "instruction budget exceeded";
                return fn;
            }
            DInsn d;
            if (!decode(va, d)) {
                fn.error = "undecodable instruction at 0x" + [&] {
                    char b[20];
                    std::snprintf(b, sizeof(b), "%llx", static_cast<unsigned long long>(va));
                    return std::string(b);
                }();
                break;
            }
            fn.insns.emplace(va, d);
            const auto cat = d.ins.meta.category;
            const auto mn = d.ins.mnemonic;
            if (cat == ZYDIS_CATEGORY_RET || mn == ZYDIS_MNEMONIC_INT3 || mn == ZYDIS_MNEMONIC_UD2 ||
                mn == ZYDIS_MNEMONIC_HLT) {
                break;
            }
            if (cat == ZYDIS_CATEGORY_COND_BR) {
                const std::uint64_t t = branch_target(d);
                fn.block_starts.insert(t);
                fn.block_starts.insert(d.next());
                work.push_back(t);
                va = d.next();
                continue;
            }
            if (cat == ZYDIS_CATEGORY_UNCOND_BR) {
                if (d.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    const std::uint64_t t = branch_target(d);
                    if (t != entry && entries.count(t) != 0) {
                        fn.tail_targets.insert(t);
                    } else {
                        fn.block_starts.insert(t);
                        work.push_back(t);
                    }
                } else {
                    JumpTable jt;
                    if (resolve_jump_table(fn, d, jt)) {
                        for (const std::uint64_t t : jt.targets) {
                            if (fn.block_starts.insert(t).second) {
                                work.push_back(t);
                            }
                        }
                        fn.jump_tables.push_back(std::move(jt));
                    } else if (looks_like_switch(fn, d)) {
                        fn.unresolved_indirect_jumps.push_back(d.va);
                    } else {
                        // `jmp [rip+IAT]`, `jmp [reg+disp]` (vtable) or `jmp reg` after an
                        // epilogue: an indirect tail call, dispatched at run time like a call.
                        fn.indirect_tail_jumps.push_back(d.va);
                    }
                }
                break;
            }
            if (cat == ZYDIS_CATEGORY_CALL) {
                if (d.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    fn.call_targets.insert(branch_target(d));
                }
                // A call that never returns is followed by padding or data: stop quietly when
                // the next bytes do not decode.
                DInsn probe;
                if (!decode(d.next(), probe)) {
                    fn.noreturn_calls.push_back(d.va);
                    break;
                }
            }
            va = d.next();
            if (entries.count(va) != 0 && va != entry) {
                // Falling through into another function's entry: treat as a tail call.
                fn.tail_targets.insert(va);
                break;
            }
        }
    }
    // Every recorded block start must be an instruction we decoded.
    for (const std::uint64_t b : fn.block_starts) {
        if (fn.insns.count(b) == 0 && fn.error.empty()) {
            fn.error = "branch into an undecoded address";
        }
    }
    return fn;
}

std::string format_insn(const DInsn& d) {
    static ZydisFormatter fmt = [] {
        ZydisFormatter f;
        ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
        return f;
    }();
    char buf[256];
    if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&fmt, &d.ins, d.ops, d.ins.operand_count_visible,
                                                      buf, sizeof(buf), d.va, nullptr))) {
        return "?";
    }
    return buf;
}

}  // namespace wse::recomp
