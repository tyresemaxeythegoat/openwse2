// R1 string family: movs/stos/lods/cmps/scas (8/16/32/64-bit), with or without rep/repe/repne,
// honouring DF. Conventions are in `lift_r1_ctx.h`; semantics follow the Intel SDM.

#include "lift_r1_ctx.h"

#include <algorithm>

namespace wse::recomp::r1 {
namespace {

using llvm::Value;


enum class StringOp { movs, stos, lods, cmps, scas };

struct StringSpec {
    StringOp op;
    unsigned bits;
};

bool classify_string_op(const DInsn& d, StringSpec& out) {
    // MOVSD and CMPSD mnemonics are shared with SSE2; Zydis puts only the string forms in
    // the STRINGOP category (Intel SDM Vol. 2).
    if (d.ins.meta.category != ZYDIS_CATEGORY_STRINGOP || d.ins.encoding != ZYDIS_INSTRUCTION_ENCODING_LEGACY) {
        return false;
    }
    switch (d.ins.mnemonic) {
    case ZYDIS_MNEMONIC_MOVSB: out = {StringOp::movs, 8}; return true;
    case ZYDIS_MNEMONIC_MOVSW: out = {StringOp::movs, 16}; return true;
    case ZYDIS_MNEMONIC_MOVSD: out = {StringOp::movs, 32}; return true;
    case ZYDIS_MNEMONIC_MOVSQ: out = {StringOp::movs, 64}; return true;
    case ZYDIS_MNEMONIC_STOSB: out = {StringOp::stos, 8}; return true;
    case ZYDIS_MNEMONIC_STOSW: out = {StringOp::stos, 16}; return true;
    case ZYDIS_MNEMONIC_STOSD: out = {StringOp::stos, 32}; return true;
    case ZYDIS_MNEMONIC_STOSQ: out = {StringOp::stos, 64}; return true;
    case ZYDIS_MNEMONIC_LODSB: out = {StringOp::lods, 8}; return true;
    case ZYDIS_MNEMONIC_LODSW: out = {StringOp::lods, 16}; return true;
    case ZYDIS_MNEMONIC_LODSD: out = {StringOp::lods, 32}; return true;
    case ZYDIS_MNEMONIC_LODSQ: out = {StringOp::lods, 64}; return true;
    case ZYDIS_MNEMONIC_CMPSB: out = {StringOp::cmps, 8}; return true;
    case ZYDIS_MNEMONIC_CMPSW: out = {StringOp::cmps, 16}; return true;
    case ZYDIS_MNEMONIC_CMPSD: out = {StringOp::cmps, 32}; return true;
    case ZYDIS_MNEMONIC_CMPSQ: out = {StringOp::cmps, 64}; return true;
    case ZYDIS_MNEMONIC_SCASB: out = {StringOp::scas, 8}; return true;
    case ZYDIS_MNEMONIC_SCASW: out = {StringOp::scas, 16}; return true;
    case ZYDIS_MNEMONIC_SCASD: out = {StringOp::scas, 32}; return true;
    case ZYDIS_MNEMONIC_SCASQ: out = {StringOp::scas, 64}; return true;
    default: return false;
    }
}

ZydisRegister rax_for_bits(unsigned bits) {
    switch (bits) {
    case 8: return ZYDIS_REGISTER_AL;
    case 16: return ZYDIS_REGISTER_AX;
    case 32: return ZYDIS_REGISTER_EAX;
    default: return ZYDIS_REGISTER_RAX;
    }
}

// Emits one element step of `spec` using `delta` (+size if DF=0, -size if DF=1).
void emit_step(Ctx& c, StringSpec spec, ZydisRegister reg_src, ZydisRegister reg_dst, Value* delta) {
    auto& B = c.B;
    auto addr64 = [&](Value* p) { return B.CreateZExt(p, B.getInt64Ty()); };
    const ZydisRegister reg_ax = rax_for_bits(spec.bits);
    switch (spec.op) {
    case StringOp::movs: {
        Value* src = c.gpr(reg_src);
        Value* dst = c.gpr(reg_dst);
        Value* v = c.load(addr64(src), spec.bits);
        c.store(addr64(dst), v);
        c.set_gpr(reg_src, B.CreateAdd(src, delta));
        c.set_gpr(reg_dst, B.CreateAdd(dst, delta));
        return;
    }
    case StringOp::stos: {
        Value* dst = c.gpr(reg_dst);
        c.store(addr64(dst), c.gpr(reg_ax));
        c.set_gpr(reg_dst, B.CreateAdd(dst, delta));
        return;
    }
    case StringOp::lods: {
        Value* src = c.gpr(reg_src);
        Value* v = c.load(addr64(src), spec.bits);
        // 8/16-bit writes merge into AL/AX; 32-bit zero-extends into RAX (Intel SDM).
        c.set_gpr(reg_ax, v);
        c.set_gpr(reg_src, B.CreateAdd(src, delta));
        return;
    }
    case StringOp::cmps: {
        // Intel SDM: CMPS computes [RSI] - [RDI] (SRC1 - SRC2) for status flags.
        Value* src = c.gpr(reg_src);
        Value* dst = c.gpr(reg_dst);
        Value* a = c.load(addr64(src), spec.bits);
        Value* b = c.load(addr64(dst), spec.bits);
        c.set_sub_flags(a, b, B.CreateSub(a, b));
        c.set_gpr(reg_src, B.CreateAdd(src, delta));
        c.set_gpr(reg_dst, B.CreateAdd(dst, delta));
        return;
    }
    case StringOp::scas: {
        // Intel SDM: SCAS computes rAX - [RDI] for status flags.
        Value* dst = c.gpr(reg_dst);
        Value* a = c.gpr(reg_ax);
        Value* b = c.load(addr64(dst), spec.bits);
        c.set_sub_flags(a, b, B.CreateSub(a, b));
        c.set_gpr(reg_dst, B.CreateAdd(dst, delta));
        return;
    }
    }
}

}  // namespace

Lifted lift_string(Ctx& c, const DInsn& d) {
    StringSpec spec{};
    if (!classify_string_op(d, spec)) {
        return Lifted::not_mine;
    }
    if ((d.ins.attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0) {
        c.why = "lock on string op";
        return Lifted::unsupported;
    }
    const unsigned op_count = std::min<unsigned>(d.ins.operand_count, ZYDIS_MAX_OPERAND_COUNT);
    for (unsigned i = 0; i < op_count; ++i) {
        const ZydisDecodedOperand& o = d.ops[i];
        if (o.type == ZYDIS_OPERAND_TYPE_MEMORY &&
            (o.mem.segment == ZYDIS_REGISTER_FS || o.mem.segment == ZYDIS_REGISTER_GS)) {
            c.why = "segmented string op";
            return Lifted::unsupported;
        }
    }
    if (d.ins.address_width != 64 && d.ins.address_width != 32) {
        c.why = "16-bit address string op";
        return Lifted::unsupported;
    }

    const unsigned addr_bits = d.ins.address_width;
    const ZydisRegister reg_cnt = addr_bits == 32 ? ZYDIS_REGISTER_ECX : ZYDIS_REGISTER_RCX;
    const ZydisRegister reg_src = addr_bits == 32 ? ZYDIS_REGISTER_ESI : ZYDIS_REGISTER_RSI;
    const ZydisRegister reg_dst = addr_bits == 32 ? ZYDIS_REGISTER_EDI : ZYDIS_REGISTER_RDI;

    auto& B = c.B;
    llvm::Type* addr_ty = c.ity(addr_bits);
    Value* step_pos = llvm::ConstantInt::get(addr_ty, spec.bits / 8U);
    Value* delta = B.CreateSelect(c.flag(DF), B.CreateNeg(step_pos), step_pos);

    const bool has_rep = (d.ins.attributes & ZYDIS_ATTRIB_HAS_REP) != 0;
    const bool has_repe = (d.ins.attributes & ZYDIS_ATTRIB_HAS_REPE) != 0;
    const bool has_repne = (d.ins.attributes & ZYDIS_ATTRIB_HAS_REPNE) != 0;
    if (!has_rep && !has_repe && !has_repne) {
        emit_step(c, spec, reg_src, reg_dst, delta);
        return Lifted::yes;
    }

    // Intel SDM: when rCX == 0 at entry, a REP* string op executes 0 iterations and leaves
    // registers, memory, and all flags unchanged.
    Value* zero = llvm::ConstantInt::get(addr_ty, 0);
    Value* one = llvm::ConstantInt::get(addr_ty, 1);
    Value* cnt0 = c.gpr(reg_cnt);
    Value* nz = B.CreateICmpNE(cnt0, zero);

    llvm::BasicBlock* loop_bb = llvm::BasicBlock::Create(c.C, "rep_loop", c.F);
    llvm::BasicBlock* done_bb = llvm::BasicBlock::Create(c.C, "rep_done", c.F);

    // One element per iteration. No `llvm.memmove`/`memset` fast path: LLVM lowers those to
    // libc calls, and a translated module imports nothing (`cpu_state.h`).
    B.CreateCondBr(nz, loop_bb, done_bb);

    B.SetInsertPoint(loop_bb);
    emit_step(c, spec, reg_src, reg_dst, delta);
    Value* cnt_next = B.CreateSub(c.gpr(reg_cnt), one);
    c.set_gpr(reg_cnt, cnt_next);
    Value* keep = B.CreateICmpNE(cnt_next, zero);
    if (spec.op == StringOp::cmps || spec.op == StringOp::scas) {
        if (has_repne) {
            keep = B.CreateAnd(keep, B.CreateNot(c.flag(ZF)));
        } else {
            // REPE / REPZ (including if Zydis emits HAS_REP for 0xF3 on cmps/scas).
            keep = B.CreateAnd(keep, c.flag(ZF));
        }
    }
    B.CreateCondBr(keep, loop_bb, done_bb);

    B.SetInsertPoint(done_bb);
    return Lifted::yes;
}

}  // namespace wse::recomp::r1
