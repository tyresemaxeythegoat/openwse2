// R1 integer family: data movement, ALU with exact flags, shifts/rotates, multiply/divide,
// SETcc/CMOVcc, bit operations, sign extension, and LOCK-prefixed atomics. Conventions are in
// `lift_r1_ctx.h`. Semantics follow the Intel SDM; flags the SDM leaves undefined are left
// unchanged unless noted.

#include "lift_r1_ctx.h"

namespace wse::recomp::r1 {
namespace {

using llvm::Value;

bool is_lock(const DInsn& d) { return (d.ins.attributes & ZYDIS_ATTRIB_HAS_LOCK) != 0; }

// LOCK-prefixed operands are assumed naturally aligned. x86 also locks misaligned ones (a bus
// lock), but LLVM lowers an under-aligned atomic to a libatomic call, which a translated module
// cannot import, and AArch64 atomics need the alignment anyway. R1 deviation: a misaligned LOCK
// access is not atomic-safe on the phone (it faults); R3 revisits memory ordering.
llvm::MaybeAlign atomic_align(const DInsn& d) {
    return llvm::MaybeAlign(d.ops[0].size >= 8 ? d.ops[0].size / 8U : 1U);
}

Value* cint(Ctx&, Value* like, std::uint64_t v) { return llvm::ConstantInt::get(like->getType(), v); }

// Shift count masked to 5 bits (6 for 64-bit operands), as an iN of the destination width.
Value* shift_count(Ctx& c, const DInsn& d, Value* dst) {
    const unsigned n = dst->getType()->getIntegerBitWidth();
    Value* cnt = d.ins.operand_count_visible > 1 ? c.read(d, 1) : c.B.getInt8(1);
    cnt = c.B.CreateZExtOrTrunc(cnt, dst->getType());
    return c.B.CreateAnd(cnt, cint(c, dst, n == 64 ? 63 : 31));
}

// Sets `f` to `v` when `when` is true, else keeps it.
void set_flag_if(Ctx& c, Flag f, Value* when, Value* v) {
    c.set_flag(f, c.B.CreateSelect(when, c.B.CreateTrunc(v, c.B.getInt1Ty()), c.flag(f)));
}

// The ALU group (add, adc, sub, sbb, and, or, xor, cmp, test), optionally LOCKed.
Lifted alu(Ctx& c, const DInsn& d) {
    const auto mn = d.ins.mnemonic;
    Value* a = c.read(d, 0);
    Value* b = c.B.CreateSExtOrTrunc(c.read(d, 1), a->getType());
    Value* cin = (mn == ZYDIS_MNEMONIC_ADC || mn == ZYDIS_MNEMONIC_SBB) ? c.flag(CF) : nullptr;
    Value* r = nullptr;
    if (is_lock(d) && d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && cin == nullptr) {
        // Atomic read-modify-write; flags come from the old value the atomic returns.
        llvm::AtomicRMWInst::BinOp op = llvm::AtomicRMWInst::Add;
        switch (mn) {
        case ZYDIS_MNEMONIC_ADD: op = llvm::AtomicRMWInst::Add; break;
        case ZYDIS_MNEMONIC_SUB: op = llvm::AtomicRMWInst::Sub; break;
        case ZYDIS_MNEMONIC_AND: op = llvm::AtomicRMWInst::And; break;
        case ZYDIS_MNEMONIC_OR: op = llvm::AtomicRMWInst::Or; break;
        case ZYDIS_MNEMONIC_XOR: op = llvm::AtomicRMWInst::Xor; break;
        default: c.why = "lock with this ALU op"; return Lifted::unsupported;
        }
        Value* addr = c.host_ptr(c.address(d, d.ops[0]));
        Value* old = c.B.CreateAtomicRMW(op, addr, b, atomic_align(d), llvm::AtomicOrdering::SequentiallyConsistent);
        a = old;
        switch (mn) {
        case ZYDIS_MNEMONIC_ADD: r = c.B.CreateAdd(a, b); c.set_add_flags(a, b, r); break;
        case ZYDIS_MNEMONIC_SUB: r = c.B.CreateSub(a, b); c.set_sub_flags(a, b, r); break;
        case ZYDIS_MNEMONIC_AND: r = c.B.CreateAnd(a, b); c.set_logic_flags(r); break;
        case ZYDIS_MNEMONIC_OR: r = c.B.CreateOr(a, b); c.set_logic_flags(r); break;
        default: r = c.B.CreateXor(a, b); c.set_logic_flags(r); break;
        }
        return Lifted::yes;
    }
    switch (mn) {
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_ADC:
        r = c.B.CreateAdd(a, b);
        if (cin != nullptr) r = c.B.CreateAdd(r, c.B.CreateZExt(cin, a->getType()));
        c.set_add_flags(a, b, r, cin);
        break;
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_SBB:
    case ZYDIS_MNEMONIC_CMP:
        r = c.B.CreateSub(a, b);
        if (cin != nullptr) r = c.B.CreateSub(r, c.B.CreateZExt(cin, a->getType()));
        c.set_sub_flags(a, b, r, cin);
        break;
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_TEST:
        r = c.B.CreateAnd(a, b);
        c.set_logic_flags(r);
        break;
    case ZYDIS_MNEMONIC_OR:
        r = c.B.CreateOr(a, b);
        c.set_logic_flags(r);
        break;
    default:  // XOR
        r = c.B.CreateXor(a, b);
        c.set_logic_flags(r);
        break;
    }
    if (mn != ZYDIS_MNEMONIC_CMP && mn != ZYDIS_MNEMONIC_TEST) {
        // `xor r32, r32` and friends: the write zero-extends like any 32-bit write.
        c.write(d, 0, r);
    }
    return Lifted::yes;
}

Lifted shift(Ctx& c, const DInsn& d) {
    const auto mn = d.ins.mnemonic;
    Value* a = c.read(d, 0);
    llvm::Type* t = a->getType();
    const unsigned n = t->getIntegerBitWidth();
    Value* cnt = shift_count(c, d, a);
    Value* nz = c.B.CreateICmpNE(cnt, cint(c, a, 0));
    Value* r = nullptr;
    Value* cf = nullptr;
    Value* of = nullptr;
    Value* one = cint(c, a, 1);
    if (n < 32 && (mn == ZYDIS_MNEMONIC_SHL || mn == ZYDIS_MNEMONIC_SHR || mn == ZYDIS_MNEMONIC_SAR)) {
        // 8/16-bit operands: the count (masked to 5 bits) may reach or pass the width, where
        // x86 shifts everything out and LLVM would be poison; work in 64 bits instead.
        llvm::Type* w = c.B.getInt64Ty();
        Value* cw = c.B.CreateZExt(cnt, w);
        Value* aw = mn == ZYDIS_MNEMONIC_SAR ? c.B.CreateSExt(a, w) : c.B.CreateZExt(a, w);
        Value* cm1 = c.B.CreateSub(cw, c.B.getInt64(1));  // only used when cnt != 0
        if (mn == ZYDIS_MNEMONIC_SHL) {
            Value* rw = c.B.CreateShl(aw, cw);
            r = c.B.CreateTrunc(rw, t);
            cf = c.B.CreateTrunc(c.B.CreateAnd(c.B.CreateLShr(rw, n), c.B.getInt64(1)), t);
            of = c.B.CreateXor(c.B.CreateLShr(r, n - 1), cf);
        } else if (mn == ZYDIS_MNEMONIC_SHR) {
            r = c.B.CreateTrunc(c.B.CreateLShr(aw, cw), t);
            cf = c.B.CreateTrunc(c.B.CreateAnd(c.B.CreateLShr(aw, cm1), c.B.getInt64(1)), t);
            of = c.B.CreateLShr(a, n - 1);
        } else {
            r = c.B.CreateTrunc(c.B.CreateAShr(aw, cw), t);
            cf = c.B.CreateTrunc(c.B.CreateAnd(c.B.CreateAShr(aw, cm1), c.B.getInt64(1)), t);
            of = cint(c, a, 0);
        }
        // `cm1` wraps for a zero count; the select below never picks that result, and a
        // freeze keeps the unpicked poison from spreading.
        cf = c.B.CreateFreeze(cf);
    } else switch (mn) {
    case ZYDIS_MNEMONIC_SHL: {
        r = c.B.CreateShl(a, cnt);
        // CF = last bit shifted out = bit (n - cnt) of a (cnt in 1..n-1; cnt >= n leaves 0).
        Value* outbit = c.B.CreateLShr(a, c.B.CreateSub(cint(c, a, n), cnt));
        cf = c.B.CreateAnd(outbit, one);
        of = c.B.CreateXor(c.B.CreateLShr(r, n - 1), cf);  // defined for count 1
        break;
    }
    case ZYDIS_MNEMONIC_SHR:
        r = c.B.CreateLShr(a, cnt);
        cf = c.B.CreateAnd(c.B.CreateLShr(a, c.B.CreateSub(cnt, one)), one);
        of = c.B.CreateLShr(a, n - 1);  // MSB of the original
        break;
    case ZYDIS_MNEMONIC_SAR:
        r = c.B.CreateAShr(a, cnt);
        cf = c.B.CreateAnd(c.B.CreateAShr(a, c.B.CreateSub(cnt, one)), one);
        of = cint(c, a, 0);
        break;
    case ZYDIS_MNEMONIC_ROL: {
        Value* m = c.B.CreateURem(cnt, cint(c, a, n));
        r = c.B.CreateIntrinsic(llvm::Intrinsic::fshl, {t}, {a, a, m});
        cf = c.B.CreateAnd(r, one);
        of = c.B.CreateXor(c.B.CreateLShr(r, n - 1), cf);
        Value* rnz = c.B.CreateICmpNE(cnt, cint(c, a, 0));
        set_flag_if(c, CF, rnz, cf);
        set_flag_if(c, OF, rnz, of);
        c.write(d, 0, r);
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_ROR: {
        Value* m = c.B.CreateURem(cnt, cint(c, a, n));
        r = c.B.CreateIntrinsic(llvm::Intrinsic::fshr, {t}, {a, a, m});
        Value* msb = c.B.CreateLShr(r, n - 1);
        cf = c.B.CreateAnd(msb, one);
        of = c.B.CreateXor(msb, c.B.CreateAnd(c.B.CreateLShr(r, n - 2), one));
        Value* rnz = c.B.CreateICmpNE(cnt, cint(c, a, 0));
        set_flag_if(c, CF, rnz, cf);
        set_flag_if(c, OF, rnz, of);
        c.write(d, 0, r);
        return Lifted::yes;
    }
    default:
        c.why = std::string("shift ") + ZydisMnemonicGetString(mn);
        return Lifted::unsupported;
    }
    // A zero count changes nothing, flags included.
    set_flag_if(c, CF, nz, cf);
    set_flag_if(c, OF, nz, of);
    llvm::Value* old_zf = c.flag(ZF);
    llvm::Value* old_sf = c.flag(SF);
    llvm::Value* old_pf = c.flag(PF);
    c.set_szp(r);
    c.set_flag(ZF, c.B.CreateSelect(nz, c.flag(ZF), old_zf));
    c.set_flag(SF, c.B.CreateSelect(nz, c.flag(SF), old_sf));
    c.set_flag(PF, c.B.CreateSelect(nz, c.flag(PF), old_pf));
    c.write(d, 0, r);
    return Lifted::yes;
}

// SHLD / SHRD dst, src, count.
Lifted double_shift(Ctx& c, const DInsn& d) {
    Value* a = c.read(d, 0);
    Value* s = c.read(d, 1);
    llvm::Type* t = a->getType();
    const unsigned n = t->getIntegerBitWidth();
    Value* cnt = c.B.CreateAnd(c.B.CreateZExtOrTrunc(c.read(d, 2), t), cint(c, a, n == 64 ? 63 : 31));
    Value* nz = c.B.CreateICmpNE(cnt, cint(c, a, 0));
    const bool left = d.ins.mnemonic == ZYDIS_MNEMONIC_SHLD;
    Value* r = left ? c.B.CreateIntrinsic(llvm::Intrinsic::fshl, {t}, {a, s, cnt})
                    : c.B.CreateIntrinsic(llvm::Intrinsic::fshr, {t}, {s, a, cnt});
    Value* one = cint(c, a, 1);
    Value* cf = left ? c.B.CreateAnd(c.B.CreateLShr(a, c.B.CreateSub(cint(c, a, n), cnt)), one)
                     : c.B.CreateAnd(c.B.CreateLShr(a, c.B.CreateSub(cnt, one)), one);
    set_flag_if(c, CF, nz, cf);
    set_flag_if(c, OF, nz, c.B.CreateXor(c.B.CreateLShr(a, n - 1), c.B.CreateLShr(r, n - 1)));
    Value* zf = c.flag(ZF);
    Value* sf = c.flag(SF);
    Value* pf = c.flag(PF);
    c.set_szp(r);
    c.set_flag(ZF, c.B.CreateSelect(nz, c.flag(ZF), zf));
    c.set_flag(SF, c.B.CreateSelect(nz, c.flag(SF), sf));
    c.set_flag(PF, c.B.CreateSelect(nz, c.flag(PF), pf));
    c.write(d, 0, r);
    return Lifted::yes;
}

// One-operand MUL/IMUL, DIV/IDIV on rDX:rAX (AX for 8-bit).
Lifted muldiv(Ctx& c, const DInsn& d) {
    const auto mn = d.ins.mnemonic;
    // Zydis lists the explicit source first (visible), then implicit rAX/rDX.
    Value* src = c.read(d, 0);
    const unsigned n = src->getType()->getIntegerBitWidth();
    llvm::Type* t = src->getType();
    llvm::Type* w = c.ity(2 * n);
    const bool sgn = (mn == ZYDIS_MNEMONIC_IMUL || mn == ZYDIS_MNEMONIC_IDIV);
    auto ext = [&](Value* v) { return sgn ? c.B.CreateSExt(v, w) : c.B.CreateZExt(v, w); };
    Value* lo = n == 8 ? c.gpr(ZYDIS_REGISTER_AL) : c.B.CreateTrunc(c.gpr64(0), t);
    Value* hi = n == 8 ? c.gpr(ZYDIS_REGISTER_AH) : c.B.CreateTrunc(c.gpr64(2), t);
    auto put = [&](Value* low, Value* high) {
        if (n == 8) {
            c.set_gpr(ZYDIS_REGISTER_AX, c.B.CreateOr(c.B.CreateZExt(low, c.ity(16)),
                                                      c.B.CreateShl(c.B.CreateZExt(high, c.ity(16)), 8)));
        } else {
            const ZydisRegister ra = n == 16 ? ZYDIS_REGISTER_AX : (n == 32 ? ZYDIS_REGISTER_EAX : ZYDIS_REGISTER_RAX);
            const ZydisRegister rd = n == 16 ? ZYDIS_REGISTER_DX : (n == 32 ? ZYDIS_REGISTER_EDX : ZYDIS_REGISTER_RDX);
            c.set_gpr(ra, low);
            c.set_gpr(rd, high);
        }
    };
    if (mn == ZYDIS_MNEMONIC_MUL || mn == ZYDIS_MNEMONIC_IMUL) {
        Value* p = c.B.CreateMul(ext(lo), ext(src));
        Value* plo = c.B.CreateTrunc(p, t);
        Value* phi = c.B.CreateTrunc(c.B.CreateLShr(p, n), t);
        put(plo, phi);
        Value* overflow = sgn ? c.B.CreateICmpNE(p, c.B.CreateSExt(plo, w)) : c.B.CreateICmpNE(phi, cint(c, src, 0));
        c.set_flag(CF, overflow);
        c.set_flag(OF, overflow);
        return Lifted::yes;
    }
    // DIV / IDIV: a zero divisor or a quotient that does not fit raises #DE.
    Value* dividend = c.B.CreateOr(c.B.CreateShl(c.B.CreateZExt(hi, w), n), c.B.CreateZExt(lo, w));
    Value* divisor = ext(src);
    Value* zero = c.B.CreateICmpEQ(src, cint(c, src, 0));
    llvm::BasicBlock* de = llvm::BasicBlock::Create(c.C, "div_error", c.F);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(c.C, "div_ok", c.F);
    llvm::BasicBlock* ok2 = llvm::BasicBlock::Create(c.C, "div_fits", c.F);
    c.B.CreateCondBr(zero, de, ok);
    c.B.SetInsertPoint(de);
    c.emit_unsupported(d.va, "#DE divide error");
    c.B.SetInsertPoint(ok);
    Value* q = sgn ? c.B.CreateSDiv(dividend, divisor) : c.B.CreateUDiv(dividend, divisor);
    Value* r = sgn ? c.B.CreateSRem(dividend, divisor) : c.B.CreateURem(dividend, divisor);
    Value* q_t = c.B.CreateTrunc(q, t);
    Value* fits = sgn ? c.B.CreateICmpEQ(c.B.CreateSExt(q_t, w), q) : c.B.CreateICmpEQ(c.B.CreateZExt(q_t, w), q);
    llvm::BasicBlock* de2 = llvm::BasicBlock::Create(c.C, "div_overflow", c.F);
    c.B.CreateCondBr(fits, ok2, de2);
    c.B.SetInsertPoint(de2);
    c.emit_unsupported(d.va, "#DE quotient overflow");
    c.B.SetInsertPoint(ok2);
    put(q_t, c.B.CreateTrunc(r, t));
    return Lifted::yes;
}

Lifted bit_test(Ctx& c, const DInsn& d) {
    const auto mn = d.ins.mnemonic;
    const unsigned n = d.ops[0].size;
    Value* off = c.read(d, 1);
    if (d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        // A register bit offset addresses memory beyond the operand: byte = offset >> 3 (signed).
        Value* off64 = c.B.CreateSExt(off, c.B.getInt64Ty());
        Value* base = c.address(d, d.ops[0]);
        Value* byte_addr = c.B.CreateAdd(base, c.B.CreateAShr(off64, 3));
        Value* bit = c.B.CreateTrunc(c.B.CreateAnd(off64, c.B.getInt64(7)), c.B.getInt8Ty());
        Value* v = c.load(byte_addr, 8);
        c.set_flag(CF, c.B.CreateTrunc(c.B.CreateLShr(v, bit), c.B.getInt1Ty()));
        Value* m = c.B.CreateShl(c.B.getInt8(1), bit);
        Value* nv = mn == ZYDIS_MNEMONIC_BTS ? c.B.CreateOr(v, m)
                    : mn == ZYDIS_MNEMONIC_BTR ? c.B.CreateAnd(v, c.B.CreateNot(m))
                    : mn == ZYDIS_MNEMONIC_BTC ? c.B.CreateXor(v, m)
                                               : nullptr;
        if (nv != nullptr) c.store(byte_addr, nv);
        return Lifted::yes;
    }
    Value* a = c.read(d, 0);
    Value* bit = c.B.CreateURem(c.B.CreateZExtOrTrunc(off, a->getType()), cint(c, a, n));
    c.set_flag(CF, c.B.CreateTrunc(c.B.CreateLShr(a, bit), c.B.getInt1Ty()));
    Value* m = c.B.CreateShl(cint(c, a, 1), bit);
    if (mn == ZYDIS_MNEMONIC_BTS) c.write(d, 0, c.B.CreateOr(a, m));
    if (mn == ZYDIS_MNEMONIC_BTR) c.write(d, 0, c.B.CreateAnd(a, c.B.CreateNot(m)));
    if (mn == ZYDIS_MNEMONIC_BTC) c.write(d, 0, c.B.CreateXor(a, m));
    return Lifted::yes;
}

}  // namespace

Lifted lift_int(Ctx& c, const DInsn& d) {
    const auto mn = d.ins.mnemonic;
    auto& B = c.B;
    switch (mn) {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_PAUSE:
    case ZYDIS_MNEMONIC_PREFETCH:
    case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1:
    case ZYDIS_MNEMONIC_PREFETCHT2:
    case ZYDIS_MNEMONIC_PREFETCHNTA:
    case ZYDIS_MNEMONIC_PREFETCHW:
    case ZYDIS_MNEMONIC_LFENCE:
    case ZYDIS_MNEMONIC_SFENCE:
        return Lifted::yes;
    case ZYDIS_MNEMONIC_MFENCE:
        B.CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
        return Lifted::yes;
    case ZYDIS_MNEMONIC_MOV:
        if (d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && Ctx::gpr_index(d.ops[0].reg.value) < 0) {
            c.why = "mov to a non-GPR register";
            return Lifted::unsupported;
        }
        c.write(d, 0, B.CreateZExtOrTrunc(c.read(d, 1), c.ity(c.bits(d, 0))));
        return Lifted::yes;
    case ZYDIS_MNEMONIC_MOVZX:
        c.write(d, 0, B.CreateZExt(c.read(d, 1), c.ity(c.bits(d, 0))));
        return Lifted::yes;
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD:
        c.write(d, 0, B.CreateSExtOrTrunc(c.read(d, 1), c.ity(c.bits(d, 0))));
        return Lifted::yes;
    case ZYDIS_MNEMONIC_LEA:
        c.write(d, 0, B.CreateTrunc(c.address(d, d.ops[1]), c.ity(c.bits(d, 0))));
        return Lifted::yes;
    case ZYDIS_MNEMONIC_PUSH: {
        Value* v = B.CreateSExtOrTrunc(c.read(d, 0), B.getInt64Ty());
        Value* rsp = B.CreateSub(c.gpr64(4), B.getInt64(8));
        c.set_gpr64(4, rsp);
        c.store(rsp, v);
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_POP: {
        Value* rsp = c.gpr64(4);
        Value* v = c.load(rsp, 64);
        c.set_gpr64(4, B.CreateAdd(rsp, B.getInt64(8)));
        c.write(d, 0, v);  // `pop rsp` ends with the loaded value, as on x86
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_LEAVE: {
        c.set_gpr64(4, c.gpr64(5));
        Value* rsp = c.gpr64(4);
        c.set_gpr64(5, c.load(rsp, 64));
        c.set_gpr64(4, B.CreateAdd(rsp, B.getInt64(8)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_XCHG: {
        if (d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY || d.ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            const int m = d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY ? 0 : 1;
            const int r = 1 - m;
            Value* addr = c.host_ptr(c.address(d, d.ops[m]));
            Value* old = B.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, addr, c.read(d, r), atomic_align(d),
                                           llvm::AtomicOrdering::SequentiallyConsistent);
            c.write(d, r, old);
            return Lifted::yes;
        }
        Value* a = c.read(d, 0);
        Value* b = c.read(d, 1);
        c.write(d, 0, b);
        c.write(d, 1, a);
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_XADD: {
        Value* src = c.read(d, 1);
        Value* old = nullptr;
        if (is_lock(d) && d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            old = B.CreateAtomicRMW(llvm::AtomicRMWInst::Add, c.host_ptr(c.address(d, d.ops[0])), src,
                                    atomic_align(d), llvm::AtomicOrdering::SequentiallyConsistent);
        } else {
            old = c.read(d, 0);
            c.write(d, 0, B.CreateAdd(old, src));
        }
        Value* sum = B.CreateAdd(old, src);
        c.set_add_flags(old, src, sum);
        c.write(d, 1, old);
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CMPXCHG: {
        // cmpxchg dst, src: if rAX == dst { ZF=1; dst = src } else { ZF=0; rAX = dst }.
        const unsigned n = c.bits(d, 0);
        Value* expected = B.CreateTrunc(c.gpr64(0), c.ity(n));
        Value* src = c.read(d, 1);
        Value* cur = nullptr;
        if (d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            llvm::AtomicCmpXchgInst* x = B.CreateAtomicCmpXchg(
                c.host_ptr(c.address(d, d.ops[0])), expected, src, atomic_align(d),
                llvm::AtomicOrdering::SequentiallyConsistent, llvm::AtomicOrdering::SequentiallyConsistent);
            cur = B.CreateExtractValue(x, 0);
        } else {
            cur = c.read(d, 0);
            c.write(d, 0, B.CreateSelect(B.CreateICmpEQ(cur, expected), src, cur));
        }
        c.set_sub_flags(expected, cur, B.CreateSub(expected, cur));
        // rAX is written only on failure (a 32-bit write then zero-extends).
        Value* eq = B.CreateICmpEQ(cur, expected);
        const ZydisRegister ra = n == 8 ? ZYDIS_REGISTER_AL : n == 16 ? ZYDIS_REGISTER_AX
                                 : n == 32 ? ZYDIS_REGISTER_EAX : ZYDIS_REGISTER_RAX;
        Value* rax_old = c.gpr64(0);
        c.set_gpr(ra, cur);
        c.set_gpr64(0, B.CreateSelect(eq, rax_old, c.gpr64(0)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_ADC:
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_SBB:
    case ZYDIS_MNEMONIC_CMP:
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_TEST:
        return alu(c, d);
    case ZYDIS_MNEMONIC_INC:
    case ZYDIS_MNEMONIC_DEC: {
        Value* a = c.read(d, 0);
        Value* one = cint(c, a, 1);
        Value* cf = c.flag(CF);  // INC/DEC keep CF
        Value* r = nullptr;
        if (is_lock(d) && d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            a = B.CreateAtomicRMW(mn == ZYDIS_MNEMONIC_INC ? llvm::AtomicRMWInst::Add : llvm::AtomicRMWInst::Sub,
                                  c.host_ptr(c.address(d, d.ops[0])), one, atomic_align(d),
                                  llvm::AtomicOrdering::SequentiallyConsistent);
            r = mn == ZYDIS_MNEMONIC_INC ? B.CreateAdd(a, one) : B.CreateSub(a, one);
        } else {
            r = mn == ZYDIS_MNEMONIC_INC ? B.CreateAdd(a, one) : B.CreateSub(a, one);
            c.write(d, 0, r);
        }
        if (mn == ZYDIS_MNEMONIC_INC) c.set_add_flags(a, one, r); else c.set_sub_flags(a, one, r);
        c.set_flag(CF, cf);
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_NEG: {
        Value* a = c.read(d, 0);
        Value* zero = cint(c, a, 0);
        Value* r = B.CreateSub(zero, a);
        c.set_sub_flags(zero, a, r);
        c.write(d, 0, r);
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_NOT:
        c.write(d, 0, B.CreateNot(c.read(d, 0)));
        return Lifted::yes;
    case ZYDIS_MNEMONIC_SHL:  // Zydis decodes SAL (same opcode) as SHL
    case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR:
        return shift(c, d);
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD:
        return double_shift(c, d);
    case ZYDIS_MNEMONIC_IMUL:
        if (d.ins.operand_count_visible >= 2) {
            // imul r, r/m [, imm]: truncated signed product; CF = OF = it overflowed.
            const unsigned n = c.bits(d, 0);
            Value* a = c.read(d, d.ins.operand_count_visible == 3 ? 1 : 0);
            Value* b = B.CreateSExtOrTrunc(c.read(d, d.ins.operand_count_visible == 3 ? 2 : 1), a->getType());
            llvm::Type* w = c.ity(2 * n);
            Value* p = B.CreateMul(B.CreateSExt(a, w), B.CreateSExt(b, w));
            Value* r = B.CreateTrunc(p, c.ity(n));
            Value* ovf = B.CreateICmpNE(p, B.CreateSExt(r, w));
            c.set_flag(CF, ovf);
            c.set_flag(OF, ovf);
            c.write(d, 0, r);
            return Lifted::yes;
        }
        return muldiv(c, d);
    case ZYDIS_MNEMONIC_MUL:
    case ZYDIS_MNEMONIC_DIV:
    case ZYDIS_MNEMONIC_IDIV:
        return muldiv(c, d);
    case ZYDIS_MNEMONIC_CBW: c.set_gpr(ZYDIS_REGISTER_AX, B.CreateSExt(c.gpr(ZYDIS_REGISTER_AL), c.ity(16))); return Lifted::yes;
    case ZYDIS_MNEMONIC_CWDE: c.set_gpr(ZYDIS_REGISTER_EAX, B.CreateSExt(c.gpr(ZYDIS_REGISTER_AX), c.ity(32))); return Lifted::yes;
    case ZYDIS_MNEMONIC_CDQE: c.set_gpr64(0, B.CreateSExt(c.gpr(ZYDIS_REGISTER_EAX), c.ity(64))); return Lifted::yes;
    case ZYDIS_MNEMONIC_CWD: c.set_gpr(ZYDIS_REGISTER_DX, B.CreateTrunc(B.CreateAShr(B.CreateSExt(c.gpr(ZYDIS_REGISTER_AX), c.ity(32)), 16), c.ity(16))); return Lifted::yes;
    case ZYDIS_MNEMONIC_CDQ: c.set_gpr(ZYDIS_REGISTER_EDX, B.CreateAShr(c.gpr(ZYDIS_REGISTER_EAX), 31)); return Lifted::yes;
    case ZYDIS_MNEMONIC_CQO: c.set_gpr64(2, B.CreateAShr(c.gpr64(0), 63)); return Lifted::yes;
    case ZYDIS_MNEMONIC_BSWAP: {
        Value* a = c.read(d, 0);
        c.write(d, 0, B.CreateUnaryIntrinsic(llvm::Intrinsic::bswap, a));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_BT:
    case ZYDIS_MNEMONIC_BTS:
    case ZYDIS_MNEMONIC_BTR:
    case ZYDIS_MNEMONIC_BTC:
        return bit_test(c, d);
    case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR: {
        Value* s = c.read(d, 1);
        Value* z = B.CreateICmpEQ(s, cint(c, s, 0));
        c.set_flag(ZF, z);
        const unsigned n = s->getType()->getIntegerBitWidth();
        Value* idx = mn == ZYDIS_MNEMONIC_BSF
                         ? B.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, s, B.getTrue())
                         : B.CreateSub(cint(c, s, n - 1), B.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, s, B.getTrue()));
        // Source zero: the destination is left unchanged (what Intel and AMD do).
        c.write(d, 0, B.CreateSelect(z, c.read(d, 0), idx));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_TZCNT:
    case ZYDIS_MNEMONIC_LZCNT:
    case ZYDIS_MNEMONIC_POPCNT: {
        Value* s = c.read(d, 1);
        Value* r = mn == ZYDIS_MNEMONIC_POPCNT ? B.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, s)
                   : mn == ZYDIS_MNEMONIC_TZCNT ? B.CreateBinaryIntrinsic(llvm::Intrinsic::cttz, s, B.getFalse())
                                                : B.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, s, B.getFalse());
        const unsigned n = s->getType()->getIntegerBitWidth();
        if (mn == ZYDIS_MNEMONIC_POPCNT) {
            c.set_flag(ZF, B.CreateICmpEQ(s, cint(c, s, 0)));
            c.set_flag(CF, B.getFalse());
            c.set_flag(OF, B.getFalse());
            c.set_flag(SF, B.getFalse());
            c.set_flag(PF, B.getFalse());
            c.set_flag(AF, B.getFalse());
        } else {
            c.set_flag(CF, B.CreateICmpEQ(r, cint(c, s, n)));
            c.set_flag(ZF, B.CreateICmpEQ(r, cint(c, s, 0)));
        }
        c.write(d, 0, r);
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CLC: c.set_flag(CF, B.getFalse()); return Lifted::yes;
    case ZYDIS_MNEMONIC_STC: c.set_flag(CF, B.getTrue()); return Lifted::yes;
    case ZYDIS_MNEMONIC_CMC: c.set_flag(CF, B.CreateNot(c.flag(CF))); return Lifted::yes;
    case ZYDIS_MNEMONIC_CLD: c.set_flag(DF, B.getFalse()); return Lifted::yes;
    case ZYDIS_MNEMONIC_STD: c.set_flag(DF, B.getTrue()); return Lifted::yes;
    default:
        break;
    }
    // SETcc / CMOVcc share the condition decoder.
    const std::string name = ZydisMnemonicGetString(mn);
    if (name.rfind("set", 0) == 0) {
        if (Value* cond = c.condition(mn)) {
            c.write(d, 0, B.CreateZExt(cond, c.ity(8)));
            return Lifted::yes;
        }
    }
    if (name.rfind("cmov", 0) == 0) {
        if (Value* cond = c.condition(mn)) {
            Value* src = c.read(d, 1);
            Value* dst = c.read(d, 0);
            // A 32-bit CMOV zero-extends the destination even when the condition is false.
            c.write(d, 0, B.CreateSelect(cond, src, dst));
            return Lifted::yes;
        }
    }
    return Lifted::not_mine;
}

}  // namespace wse::recomp::r1
