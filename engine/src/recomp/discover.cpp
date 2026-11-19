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
    if (limit < 0 && ranges_ != nullptr && !table_relative && byte_table == 0) {
        // R4: no bound in the code. Read entries while each lands inside the .pdata function
        // that holds the jmp (at most 256); a table must have two entries to count.
        const auto rva = static_cast<std::uint32_t>(jmp.va - base);
        const auto it2 = std::upper_bound(ranges_->begin(), ranges_->end(), rva,
                                          [](std::uint32_t v, const PeFunctionRange& r) { return v < r.begin; });
        if (it2 == ranges_->begin() || rva >= std::prev(it2)->end) {
            return false;
        }
        const std::uint64_t lo = base + std::prev(it2)->begin;
        const std::uint64_t hi = base + std::prev(it2)->end;
        out.jmp_va = jmp.va;
        for (std::uint32_t k = 0; k < 256; ++k) {
            std::uint32_t e = 0;
            if (!view_.read32(table + 4ULL * k, e) || base + e < lo || base + e >= hi) {
                break;
            }
            out.targets.push_back(base + e);
        }
        out.by_function_range = true;
        return out.targets.size() >= 2;
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

DFunction Discoverer::discover(std::uint64_t entry, const std::set<std::uint64_t>& entries,
                               const std::set<std::uint64_t>* extra_starts) const {
    DFunction fn;
    fn.entry = entry;
    fn.block_starts.insert(entry);
    std::vector<std::uint64_t> work{entry};
    if (extra_starts != nullptr) {
        for (const std::uint64_t x : *extra_starts) {
            if (x != entry && fn.block_starts.insert(x).second) {
                work.push_back(x);
            }
        }
    }
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

bool insn_is_trap(const DInsn& d) {
    const auto mn = d.ins.mnemonic;
    return mn == ZYDIS_MNEMONIC_INT3 || mn == ZYDIS_MNEMONIC_INT || mn == ZYDIS_MNEMONIC_INT1 ||
           mn == ZYDIS_MNEMONIC_INTO || mn == ZYDIS_MNEMONIC_UD0 || mn == ZYDIS_MNEMONIC_UD1 ||
           mn == ZYDIS_MNEMONIC_UD2 || mn == ZYDIS_MNEMONIC_HLT;
}

const char* insn_gated_ext(const DInsn& d) {
    switch (d.ins.encoding) {
    case ZYDIS_INSTRUCTION_ENCODING_VEX: return "AVX";
    case ZYDIS_INSTRUCTION_ENCODING_EVEX: return "AVX-512";
    case ZYDIS_INSTRUCTION_ENCODING_XOP: return "XOP";
    case ZYDIS_INSTRUCTION_ENCODING_3DNOW: return "3DNow!";
    default: break;
    }
    if (d.ins.meta.isa_set == ZYDIS_ISA_SET_SSE42) return "SSE4.2";
    if (d.ins.meta.isa_set == ZYDIS_ISA_SET_SSE4A) return "SSE4a";
    if (d.ins.mnemonic == ZYDIS_MNEMONIC_XGETBV) return "OSXSAVE";
    return nullptr;
}

bool is_self_contained(const DFunction& f) {
    if (!f.error.empty() || !f.unresolved_indirect_jumps.empty() || !f.indirect_tail_jumps.empty() ||
        !f.tail_targets.empty() || !f.call_targets.empty() || !f.noreturn_calls.empty() || f.insns.empty()) {
        return false;
    }
    for (const auto& [va, d] : f.insns) {
        const auto cat = d.ins.meta.category;
        const auto mn = d.ins.mnemonic;
        if (cat == ZYDIS_CATEGORY_CALL || cat == ZYDIS_CATEGORY_SYSTEM || cat == ZYDIS_CATEGORY_SYSCALL ||
            cat == ZYDIS_CATEGORY_X87_ALU || d.ins.meta.isa_ext == ZYDIS_ISA_EXT_X87 || insn_is_trap(d) ||
            insn_gated_ext(d) != nullptr || mn == ZYDIS_MNEMONIC_CPUID || mn == ZYDIS_MNEMONIC_RDTSC ||
            mn == ZYDIS_MNEMONIC_RDTSCP) {
            return false;
        }
        if (cat == ZYDIS_CATEGORY_UNCOND_BR && d.ops[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            bool table = false;
            for (const JumpTable& t : f.jump_tables) {
                table = table || t.jmp_va == va;
            }
            if (!table) {
                return false;
            }
        }
        for (unsigned i = 0; i < d.ins.operand_count; ++i) {
            const ZydisDecodedOperand& o = d.ops[i];
            if (o.type != ZYDIS_OPERAND_TYPE_MEMORY) {
                continue;
            }
            if (o.mem.segment == ZYDIS_REGISTER_FS || o.mem.segment == ZYDIS_REGISTER_GS) {
                return false;
            }
            const bool writes = (o.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
            const bool absolute = o.mem.base == ZYDIS_REGISTER_NONE && o.mem.index == ZYDIS_REGISTER_NONE;
            if (writes && (o.mem.base == ZYDIS_REGISTER_RIP || absolute)) {
                return false;
            }
        }
    }
    return true;
}

namespace {

// UNWIND_INFO: byte 0 = version | flags << 3, byte 2 = count of 2-byte unwind codes (padded to an
// even count), then the handler RVA and its data (UNW_FLAG_EHANDLER 1 / UHANDLER 2) or a chained
// RUNTIME_FUNCTION (UNW_FLAG_CHAININFO 4).
std::uint64_t unwind_tail(const ImageView& view, std::uint64_t info, std::uint8_t& flags) {
    std::uint8_t b0 = 0;
    std::uint8_t count = 0;
    if (!view.read8(info, b0) || !view.read8(info + 2, count)) {
        return 0;
    }
    flags = static_cast<std::uint8_t>(b0 >> 3);
    return info + 4 + 2ULL * ((count + 1U) & ~1U);
}

const PeFunctionRange* range_at(const PeImage& img, std::uint32_t rva) {
    const auto it = std::upper_bound(img.functions.begin(), img.functions.end(), rva,
                                     [](std::uint32_t v, const PeFunctionRange& r) { return v < r.begin; });
    if (it == img.functions.begin()) {
        return nullptr;
    }
    const PeFunctionRange& r = *(it - 1);
    return rva < r.end ? &r : nullptr;
}

constexpr std::uint32_t kMaxEhEntries = 1U << 16;  // bounds a corrupt FuncInfo

}  // namespace

std::uint64_t pdata_owner(const ImageView& view, const PeImage& img, std::uint64_t va) {
    if (va < img.image_base || va - img.image_base >= img.size_of_image) {
        return 0;
    }
    const PeFunctionRange* r = range_at(img, static_cast<std::uint32_t>(va - img.image_base));
    for (int depth = 0; r != nullptr && depth < 32; ++depth) {
        std::uint8_t flags = 0;
        const std::uint64_t tail = unwind_tail(view, img.image_base + r->unwind, flags);
        if (tail == 0) {
            return 0;
        }
        if ((flags & 0x4) == 0) {
            return img.image_base + r->begin;
        }
        std::uint32_t parent = 0;  // the chained RUNTIME_FUNCTION's BeginAddress
        if (!view.read32(tail, parent)) {
            return 0;
        }
        r = range_at(img, parent);
    }
    return 0;
}

EhContinuations find_eh_continuations(const ImageView& view, const PeImage& img,
                                      const std::set<std::uint64_t>& entries) {
    EhContinuations out;
    const std::uint64_t base = img.image_base;
    std::set<std::uint32_t> func_infos;
    for (const PeFunctionRange& r : img.functions) {
        std::uint8_t flags = 0;
        const std::uint64_t tail = unwind_tail(view, base + r.unwind, flags);
        std::uint32_t fi = 0;
        // Handler data follows the handler RVA; its first word is the FuncInfo RVA.
        if (tail == 0 || (flags & 0x3) == 0 || (flags & 0x4) != 0 || !view.read32(tail + 4, fi)) {
            continue;
        }
        std::uint32_t magic = 0;
        if (fi == 0 || !view.read32(base + fi, magic) || magic < 0x19930520U || magic > 0x19930522U) {
            continue;
        }
        func_infos.insert(fi);
    }
    out.func_infos = func_infos.size();

    std::set<std::uint64_t> funclets;
    for (const std::uint32_t fi : func_infos) {
        // FuncInfo (x64): magic, maxState, dispUnwindMap, nTryBlocks, dispTryBlockMap, ...
        std::uint32_t n_try = 0;
        std::uint32_t try_map = 0;
        if (!view.read32(base + fi + 12, n_try) || !view.read32(base + fi + 16, try_map) ||
            n_try > kMaxEhEntries) {
            continue;
        }
        for (std::uint32_t t = 0; t < n_try; ++t) {
            // TryBlockMapEntry (20 bytes): tryLow, tryHigh, catchHigh, nCatches, dispHandlerArray.
            const std::uint64_t e = base + try_map + 20ULL * t;
            std::uint32_t n_catch = 0;
            std::uint32_t handlers = 0;
            if (!view.read32(e + 12, n_catch) || !view.read32(e + 16, handlers) || n_catch > kMaxEhEntries) {
                break;
            }
            for (std::uint32_t h = 0; h < n_catch; ++h) {
                // HandlerType (20 bytes): adjectives, dispType, dispCatchObj, dispOfHandler, dispFrame.
                std::uint32_t funclet = 0;
                if (view.read32(base + handlers + 20ULL * h + 12, funclet) && funclet != 0 &&
                    view.in_text(base + funclet)) {
                    funclets.insert(base + funclet);
                }
            }
        }
    }

    Discoverer disc(view);
    for (const std::uint64_t f : funclets) {
        ++out.catch_handlers;
        const DFunction fn = disc.discover(f, entries);
        bool found = false;
        for (const auto& [va, d] : fn.insns) {
            if (d.ins.mnemonic != ZYDIS_MNEMONIC_LEA || d.ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                d.ops[0].reg.value != ZYDIS_REGISTER_RAX || d.ops[1].mem.base != ZYDIS_REGISTER_RIP) {
                continue;
            }
            const std::uint64_t x = d.next() + static_cast<std::uint64_t>(d.ops[1].mem.disp.value);
            if (!view.in_text(x) || entries.count(x) != 0) {
                continue;
            }
            const std::uint64_t owner = pdata_owner(view, img, x);
            if (owner != 0 && entries.count(owner) != 0) {
                out.by_function[owner].insert(x);
                found = true;
            }
        }
        if (!found) {
            ++out.handlers_without;
        }
    }
    return out;
}

OffsetTables find_offset_table_entries(const ImageView& view, const PeImage& img, const std::set<std::uint64_t>& entries) {
    OffsetTables out;
    const std::uint64_t base = img.image_base;
    // .pdata begins -> end, for "B is a function start" and "B + v stays inside it".
    std::map<std::uint64_t, std::uint64_t> ranges;
    for (const PeFunctionRange& r : img.functions) {
        ranges[base + r.begin] = base + r.end;
    }
    std::set<std::pair<std::uint64_t, std::uint64_t>> tables;  // (table, function)
    Discoverer disc(view);
    for (const std::uint64_t e : entries) {
        const DFunction fn = disc.discover(e, entries);
        std::map<ZydisRegister, std::uint64_t> lea;  // 64-bit register -> rip-relative address it holds
        ZydisRegister loaded = ZYDIS_REGISTER_NONE;  // destination of the last 16-bit table load
        std::uint64_t loaded_table = 0;
        int since_load = 0;
        for (const auto& [va, d] : fn.insns) {
            (void)va;
            const auto mn = d.ins.mnemonic;
            const ZydisDecodedOperand& o0 = d.ops[0];
            const ZydisDecodedOperand& o1 = d.ops[1];
            if (mn == ZYDIS_MNEMONIC_MOVZX && o0.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                o1.type == ZYDIS_OPERAND_TYPE_MEMORY && o1.size == 16) {
                std::uint64_t t = 0;
                if (o1.mem.base == ZYDIS_REGISTER_RIP && o1.mem.index == ZYDIS_REGISTER_NONE) {
                    t = d.next() + static_cast<std::uint64_t>(o1.mem.disp.value);  // a single entry
                } else if (const auto it = lea.find(widen(o1.mem.base)); it != lea.end() && o1.mem.disp.value == 0 &&
                                                                         (o1.mem.index == ZYDIS_REGISTER_NONE || o1.mem.scale == 2)) {
                    t = it->second;
                }
                if (t != 0) {
                    lea.erase(widen(o0.reg.value));
                    loaded = widen(o0.reg.value);
                    loaded_table = t;
                    since_load = 0;
                    continue;
                }
            }
            if (mn == ZYDIS_MNEMONIC_ADD && loaded != ZYDIS_REGISTER_NONE && o0.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                o1.type == ZYDIS_OPERAND_TYPE_REGISTER && widen(o0.reg.value) == loaded) {
                const auto it = lea.find(widen(o1.reg.value));
                if (it != lea.end() && ranges.count(it->second) != 0) {
                    tables.insert({loaded_table, it->second});
                }
            }
            if (++since_load > 8) {
                loaded = ZYDIS_REGISTER_NONE;
            }
            if (mn == ZYDIS_MNEMONIC_LEA && o0.type == ZYDIS_OPERAND_TYPE_REGISTER && o1.mem.base == ZYDIS_REGISTER_RIP &&
                o1.mem.index == ZYDIS_REGISTER_NONE) {
                lea[widen(o0.reg.value)] = d.next() + static_cast<std::uint64_t>(o1.mem.disp.value);
                continue;
            }
            // Any other write to a register forgets what it held.
            for (std::uint8_t k = 0; k < d.ins.operand_count; ++k) {
                const ZydisDecodedOperand& o = d.ops[k];
                if (o.type == ZYDIS_OPERAND_TYPE_REGISTER && (o.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0) {
                    lea.erase(widen(o.reg.value));
                }
            }
        }
    }
    // A table whose single entries were read one by one ("movzx [rip + X]") is the same table read
    // from its start: merge each such entry into the table it lies in.
    std::map<std::uint64_t, std::set<std::uint64_t>> by_function;
    for (const auto& [t, f] : tables) {
        by_function[f].insert(t);
    }
    for (const auto& [f, ts] : by_function) {
        const std::uint64_t start = *ts.begin();  // the lowest address read is the table's start
        const std::uint64_t size = ranges[f] - f;
        std::size_t n = 0;
        for (std::uint64_t at = start; n < 4096; at += 2, ++n) {
            if (!view.readable(at, 2)) {
                break;
            }
            std::uint16_t v = 0;
            std::memcpy(&v, view.ptr(at), 2);
            if (v >= size) {
                break;  // past the table: the next value does not land in the function
            }
            out.entries.insert(f + v);
        }
        out.tables.push_back({start, f, n});
    }
    return out;
}

std::pair<std::uint64_t, std::uint64_t> text_range(const PeImage& img) {
    for (const PeSection& s : img.sections) {
        if (s.name == ".text") {
            const std::uint64_t b = img.image_base + s.virtual_address;
            return {b, b + s.virtual_size};
        }
    }
    return {0, 0};
}

std::set<std::uint64_t> collect_function_entries(const ImageView& view, const PeImage& img,
                                                 const std::uint8_t* image,
                                                 const std::vector<PdbPublic>& publics) {
    std::set<std::uint64_t> entries;
    for (const PeFunctionRange& r : img.functions) {
        // UNWIND_INFO flag 4 (UNW_FLAG_CHAININFO): a fragment of another function.
        if (r.unwind < img.size_of_image && ((image[r.unwind] >> 3) & 0x4) == 0) {
            entries.insert(img.image_base + r.begin);
        }
    }
    for (const PdbPublic& p : publics) {
        if (p.section == 0 || p.section > img.sections.size() || (p.flags & 2U) == 0) {
            continue;  // flag 2: a function (CVPSF_FUNCTION)
        }
        const std::uint64_t va = img.image_base + img.sections[p.section - 1].virtual_address + p.offset;
        if (view.in_text(va)) {
            entries.insert(va);
        }
    }
    // Catch continuations are code too: their calls name more entries (R3).
    const EhContinuations eh = find_eh_continuations(view, img, entries);
    Discoverer disc(view);
    std::vector<std::uint64_t> work(entries.begin(), entries.end());
    std::set<std::uint64_t> seen;
    while (!work.empty()) {
        const std::uint64_t e = work.back();
        work.pop_back();
        if (!seen.insert(e).second) {
            continue;
        }
        const auto cont = eh.by_function.find(e);
        const DFunction f = disc.discover(e, entries, cont == eh.by_function.end() ? nullptr : &cont->second);
        for (const std::uint64_t t : f.call_targets) {
            if (view.in_text(t) && entries.insert(t).second) {
                work.push_back(t);
            }
        }
    }
    return entries;
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
