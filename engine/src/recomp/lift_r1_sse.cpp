// R1 sse family: legacy SSE/SSE2/SSE3/SSSE3/SSE4.1 scalar and packed instructions.
// Conventions are in `lift_r1_ctx.h`. Semantics follow the Intel SDM; VEX/EVEX encodings
// return `Lifted::not_mine`, MMX operands return `unsupported`/`not_mine`, and unhandled
// legacy instructions with an XMM operand return `Lifted::unsupported` with the mnemonic.

#include "lift_r1_ctx.h"

#include <array>

namespace wse::recomp::r1 {
namespace {

using llvm::Type;
using llvm::Value;

llvm::FixedVectorType* v4f_ty(Ctx& c) { return llvm::FixedVectorType::get(c.B.getFloatTy(), 4); }
llvm::FixedVectorType* v2d_ty(Ctx& c) { return llvm::FixedVectorType::get(c.B.getDoubleTy(), 2); }
llvm::FixedVectorType* v16i8_ty(Ctx& c) { return llvm::FixedVectorType::get(c.B.getInt8Ty(), 16); }
llvm::FixedVectorType* v8i16_ty(Ctx& c) { return llvm::FixedVectorType::get(c.B.getInt16Ty(), 8); }
llvm::FixedVectorType* v4i32_ty(Ctx& c) { return llvm::FixedVectorType::get(c.B.getInt32Ty(), 4); }
llvm::FixedVectorType* v2i64_ty(Ctx& c) { return llvm::FixedVectorType::get(c.B.getInt64Ty(), 2); }

bool has_xmm_op(const DInsn& d) {
    for (std::uint8_t i = 0; i < d.ins.operand_count; ++i) {
        if (d.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER && Ctx::xmm_index(d.ops[i].reg.value) >= 0) {
            return true;
        }
    }
    return false;
}

bool has_mmx_op(const DInsn& d) {
    for (std::uint8_t i = 0; i < d.ins.operand_count; ++i) {
        if (d.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ZydisRegisterGetClass(d.ops[i].reg.value) == ZYDIS_REGCLASS_MMX) {
            return true;
        }
    }
    return false;
}

Lifted fail_insn(Ctx& c, const DInsn& d) {
    c.why = ZydisMnemonicGetString(d.ins.mnemonic);
    return Lifted::unsupported;
}

Value* splat_vec(Ctx& c, Value* scalar, unsigned n) {
    auto* vty = llvm::FixedVectorType::get(scalar->getType(), n);
    Value* v = c.B.CreateInsertElement(llvm::PoisonValue::get(vty), scalar, std::uint64_t{0});
    std::array<int, 16> zeros{};
    return c.B.CreateShuffleVector(v, llvm::PoisonValue::get(vty),
                                   llvm::ArrayRef<int>(zeros.data(), n));
}

// Reads a 128-bit XMM/m128 operand as `i128` or bitcast to `vec_ty`.
Value* read_i128(Ctx& c, const DInsn& d, int op_idx, bool* ok = nullptr) {
    const ZydisDecodedOperand& o = d.ops[op_idx];
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int idx = Ctx::xmm_index(o.reg.value);
        if (idx < 0) {
            if (ok != nullptr) *ok = false;
            return nullptr;
        }
        return c.xmm(idx);
    }
    if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        return c.load(c.address(d, o), 128);
    }
    if (ok != nullptr) *ok = false;
    return nullptr;
}

Value* read_vec(Ctx& c, const DInsn& d, int op_idx, Type* vec_ty, bool* ok = nullptr) {
    Value* v = read_i128(c, d, op_idx, ok);
    return v != nullptr ? c.B.CreateBitCast(v, vec_ty) : nullptr;
}

Value* read_scalar_fp(Ctx& c, const DInsn& d, int op_idx, bool is_double, bool* ok = nullptr) {
    const ZydisDecodedOperand& o = d.ops[op_idx];
    Type* fty = is_double ? c.B.getDoubleTy() : c.B.getFloatTy();
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int idx = Ctx::xmm_index(o.reg.value);
        if (idx < 0) {
            if (ok != nullptr) *ok = false;
            return nullptr;
        }
        Value* vec = c.B.CreateBitCast(c.xmm(idx), is_double ? v2d_ty(c) : v4f_ty(c));
        return c.B.CreateExtractElement(vec, std::uint64_t{0});
    }
    if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        return c.B.CreateBitCast(c.load(c.address(d, o), is_double ? 64U : 32U), fty);
    }
    if (ok != nullptr) *ok = false;
    return nullptr;
}

// Writes `scalar_val` (float/double/i32/i64) into the low bits of `dst_idx`, keeping upper bits.
void write_scalar_merge(Ctx& c, int dst_idx, Value* scalar_val) {
    Type* st = scalar_val->getType();
    const bool w64 = st->isDoubleTy() || st->isIntegerTy(64);
    Type* vty = llvm::FixedVectorType::get(st, w64 ? 2U : 4U);
    Value* cur = c.B.CreateBitCast(c.xmm(dst_idx), vty);
    c.set_xmm(dst_idx, c.B.CreateInsertElement(cur, scalar_val, std::uint64_t{0}));
}

// Reads MXCSR.RC (bits 13..14: 0 nearest-even, 1 down, 2 up, 3 toward zero).
Value* mxcsr_rc(Ctx& c) {
    Value* mxcsr = c.B.CreateLoad(c.B.getInt32Ty(), c.state_ptr(cpu_offset::mxcsr));
    return c.B.CreateAnd(c.B.CreateLShr(mxcsr, 13), c.B.getInt32(3));
}

Value* round_with_mode(Ctx& c, Value* x, Value* rc_i32) {
    Value* r0 = c.B.CreateUnaryIntrinsic(llvm::Intrinsic::roundeven, x);
    Value* r1 = c.B.CreateUnaryIntrinsic(llvm::Intrinsic::floor, x);
    Value* r2 = c.B.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, x);
    Value* r3 = c.B.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x);
    Value* r = c.B.CreateSelect(c.B.CreateICmpEQ(rc_i32, c.B.getInt32(1)), r1, r0);
    r = c.B.CreateSelect(c.B.CreateICmpEQ(rc_i32, c.B.getInt32(2)), r2, r);
    return c.B.CreateSelect(c.B.CreateICmpEQ(rc_i32, c.B.getInt32(3)), r3, r);
}

Value* round_with_imm(Ctx& c, Value* x, unsigned mode) {
    switch (mode & 3U) {
    case 0: return c.B.CreateUnaryIntrinsic(llvm::Intrinsic::roundeven, x);
    case 1: return c.B.CreateUnaryIntrinsic(llvm::Intrinsic::floor, x);
    case 2: return c.B.CreateUnaryIntrinsic(llvm::Intrinsic::ceil, x);
    default: return c.B.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x);
    }
}

// Converts scalar or vector float/double to signed i32/i64. Out-of-range or NaN inputs return
// the Intel integer indefinite value (`0x80000000` / `0x8000000000000000`) instead of poison.
Value* fp_to_si_checked(Ctx& c, Value* x, Type* dst_int_ty, bool trunc_mode) {
    Value* rx = trunc_mode ? c.B.CreateUnaryIntrinsic(llvm::Intrinsic::trunc, x)
                           : round_with_mode(c, x, mxcsr_rc(c));
    Type* src_ty = x->getType();
    const unsigned int_bits = dst_int_ty->getScalarSizeInBits();
    const double lo_lim = int_bits == 64 ? -0x1p63 : -0x1p31;
    const double hi_lim = int_bits == 64 ? 0x1p63 : 0x1p31;
    Value* in_lo = c.B.CreateFCmpOGE(rx, llvm::ConstantFP::get(src_ty, lo_lim));
    Value* in_hi = c.B.CreateFCmpOLT(rx, llvm::ConstantFP::get(src_ty, hi_lim));
    Value* in_range = c.B.CreateAnd(in_lo, in_hi);
    Value* safe_rx = c.B.CreateSelect(in_range, rx, llvm::ConstantFP::get(src_ty, 0.0));
    Value* conv = c.B.CreateFPToSI(safe_rx, dst_int_ty);
    const std::uint64_t indef_val = int_bits == 64 ? 0x8000000000000000ULL : 0x80000000ULL;
    return c.B.CreateSelect(in_range, conv, llvm::ConstantInt::get(dst_int_ty, indef_val, false));
}

// Signed integer -> float/double honoring all 4 MXCSR rounding modes via exact magnitude bitops.
Value* si64_to_fp_mxcsr(Ctx& c, Value* x64, Type* fp_ty) {
    const unsigned mant_bits = fp_ty->isDoubleTy() ? 53U : 24U;
    Value* rne = c.B.CreateSIToFP(x64, fp_ty);
    Value* rc = mxcsr_rc(c);
    Value* is_rne = c.B.CreateICmpEQ(rc, c.B.getInt32(0));
    Value* zero = c.B.getInt64(0);
    Value* neg = c.B.CreateICmpSLT(x64, zero);
    Value* ax = c.B.CreateSelect(neg, c.B.CreateNeg(x64), x64);
    Value* lz = c.B.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, ax, c.B.getFalse());
    Value* bits = c.B.CreateSub(c.B.getInt64(64), lz);
    Value* p_val = c.B.getInt64(mant_bits);
    Value* drop = c.B.CreateSelect(c.B.CreateICmpULE(bits, p_val), zero, c.B.CreateSub(bits, p_val));
    Value* step = c.B.CreateShl(c.B.getInt64(1), drop);
    Value* low_mask = c.B.CreateSub(step, c.B.getInt64(1));
    Value* rem = c.B.CreateICmpNE(c.B.CreateAnd(ax, low_mask), zero);
    Value* trunc_mag = c.B.CreateAnd(ax, c.B.CreateNot(low_mask));
    Value* is_down = c.B.CreateICmpEQ(rc, c.B.getInt32(1));
    Value* is_up = c.B.CreateICmpEQ(rc, c.B.getInt32(2));
    Value* away = c.B.CreateAnd(
        rem, c.B.CreateOr(c.B.CreateAnd(is_down, neg), c.B.CreateAnd(is_up, c.B.CreateNot(neg))));
    Value* adj_mag = c.B.CreateAdd(trunc_mag, c.B.CreateSelect(away, step, zero));
    Value* mag_fp = c.B.CreateUIToFP(adj_mag, fp_ty);
    Value* dir = c.B.CreateSelect(neg, c.B.CreateFNeg(mag_fp), mag_fp);
    return c.B.CreateSelect(is_rne, rne, dir);
}

Value* v4i32_to_v4f_mxcsr(Ctx& c, Value* v) {
    auto* v4i = v4i32_ty(c);
    auto* v4f = v4f_ty(c);
    Value* rne = c.B.CreateSIToFP(v, v4f);
    Value* rc = mxcsr_rc(c);
    Value* is_rne = c.B.CreateICmpEQ(rc, c.B.getInt32(0));
    Value* zero = llvm::ConstantAggregateZero::get(v4i);
    Value* ones = llvm::ConstantInt::get(v4i, 1, false);
    Value* neg = c.B.CreateICmpSLT(v, zero);
    Value* ax = c.B.CreateSelect(neg, c.B.CreateNeg(v), v);
    Value* lz = c.B.CreateBinaryIntrinsic(llvm::Intrinsic::ctlz, ax, c.B.getFalse());
    Value* bits = c.B.CreateSub(llvm::ConstantInt::get(v4i, 32, false), lz);
    Value* p_val = llvm::ConstantInt::get(v4i, 24, false);
    Value* drop = c.B.CreateSelect(c.B.CreateICmpULE(bits, p_val), zero, c.B.CreateSub(bits, p_val));
    Value* step = c.B.CreateShl(ones, drop);
    Value* low_mask = c.B.CreateSub(step, ones);
    Value* rem = c.B.CreateICmpNE(c.B.CreateAnd(ax, low_mask), zero);
    Value* trunc_mag = c.B.CreateAnd(ax, c.B.CreateNot(low_mask));
    Value* is_down = splat_vec(c, c.B.CreateICmpEQ(rc, c.B.getInt32(1)), 4);
    Value* is_up = splat_vec(c, c.B.CreateICmpEQ(rc, c.B.getInt32(2)), 4);
    Value* away = c.B.CreateAnd(
        rem, c.B.CreateOr(c.B.CreateAnd(is_down, neg), c.B.CreateAnd(is_up, c.B.CreateNot(neg))));
    Value* adj_mag = c.B.CreateAdd(trunc_mag, c.B.CreateSelect(away, step, zero));
    Value* mag_fp = c.B.CreateUIToFP(adj_mag, v4f);
    Value* dir = c.B.CreateSelect(neg, c.B.CreateFNeg(mag_fp), mag_fp);
    return c.B.CreateSelect(is_rne, rne, dir);
}

llvm::CmpInst::Predicate fp_cmp_pred(unsigned imm) {
    switch (imm & 7U) {
    case 0: return llvm::CmpInst::FCMP_OEQ;
    case 1: return llvm::CmpInst::FCMP_OLT;
    case 2: return llvm::CmpInst::FCMP_OLE;
    case 3: return llvm::CmpInst::FCMP_UNO;
    case 4: return llvm::CmpInst::FCMP_UNE;
    case 5: return llvm::CmpInst::FCMP_UGE;
    case 6: return llvm::CmpInst::FCMP_UGT;
    default: return llvm::CmpInst::FCMP_ORD;
    }
}

Lifted lift_mov_scalar(Ctx& c, const DInsn& d, unsigned bits) {
    // movss / movsd: reg<-reg merges low bits; reg<-mem zeroes upper bits; mem<-reg stores low bits.
    if (d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        if (d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const int src = Ctx::xmm_index(d.ops[1].reg.value);
            if (src < 0) return fail_insn(c, d);
            Value* low = c.B.CreateTrunc(c.xmm(src), c.ity(bits));
            write_scalar_merge(c, dst, low);
            return Lifted::yes;
        }
        if (d.ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            Value* v = c.load(c.address(d, d.ops[1]), bits);
            c.set_xmm(dst, c.B.CreateZExt(v, c.i128()));
            return Lifted::yes;
        }
        return fail_insn(c, d);
    }
    if (d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int src = Ctx::xmm_index(d.ops[1].reg.value);
        if (src < 0) return fail_insn(c, d);
        c.store(c.address(d, d.ops[0]), c.B.CreateTrunc(c.xmm(src), c.ity(bits)));
        return Lifted::yes;
    }
    return fail_insn(c, d);
}

Lifted lift_movdq(Ctx& c, const DInsn& d, unsigned bits) {
    // movd (32-bit) / movq (64-bit): any write into XMM (including xmm<-xmm) zeroes upper bits.
    if (d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int dst_xmm = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst_xmm >= 0) {
            Value* v = nullptr;
            if (d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                const int src_xmm = Ctx::xmm_index(d.ops[1].reg.value);
                if (src_xmm >= 0) {
                    v = c.B.CreateTrunc(c.xmm(src_xmm), c.ity(bits));
                } else if (Ctx::gpr_index(d.ops[1].reg.value) >= 0) {
                    v = c.B.CreateZExtOrTrunc(c.gpr(d.ops[1].reg.value), c.ity(bits));
                }
            } else if (d.ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
                v = c.load(c.address(d, d.ops[1]), bits);
            }
            if (v == nullptr) return fail_insn(c, d);
            c.set_xmm(dst_xmm, c.B.CreateZExt(v, c.i128()));
            return Lifted::yes;
        }
        if (Ctx::gpr_index(d.ops[0].reg.value) >= 0 && d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const int src_xmm = Ctx::xmm_index(d.ops[1].reg.value);
            if (src_xmm < 0) return fail_insn(c, d);
            c.set_gpr(d.ops[0].reg.value, c.B.CreateTrunc(c.xmm(src_xmm), c.ity(bits)));
            return Lifted::yes;
        }
        return fail_insn(c, d);
    }
    if (d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int src_xmm = Ctx::xmm_index(d.ops[1].reg.value);
        if (src_xmm < 0) return fail_insn(c, d);
        c.store(c.address(d, d.ops[0]), c.B.CreateTrunc(c.xmm(src_xmm), c.ity(bits)));
        return Lifted::yes;
    }
    return fail_insn(c, d);
}

Lifted lift_mov_half64(Ctx& c, const DInsn& d, unsigned lane) {
    // movlps/movlpd (lane 0) and movhps/movhpd (lane 1): preserve the other 64-bit half of dst.
    auto* v2i = v2i64_ty(c);
    if (d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && d.ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Value* cur = c.B.CreateBitCast(c.xmm(dst), v2i);
        Value* val = c.load(c.address(d, d.ops[1]), 64);
        c.set_xmm(dst, c.B.CreateInsertElement(cur, val, static_cast<std::uint64_t>(lane)));
        return Lifted::yes;
    }
    if (d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int src = Ctx::xmm_index(d.ops[1].reg.value);
        if (src < 0) return fail_insn(c, d);
        Value* vec = c.B.CreateBitCast(c.xmm(src), v2i);
        c.store(c.address(d, d.ops[0]), c.B.CreateExtractElement(vec, static_cast<std::uint64_t>(lane)));
        return Lifted::yes;
    }
    return fail_insn(c, d);
}

Lifted lift_movmsk(Ctx& c, const DInsn& d, unsigned lanes, unsigned elem_bits) {
    if (d.ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER || Ctx::gpr_index(d.ops[0].reg.value) < 0 ||
        d.ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER) {
        return fail_insn(c, d);
    }
    const int src = Ctx::xmm_index(d.ops[1].reg.value);
    if (src < 0) return fail_insn(c, d);
    auto* vty = llvm::FixedVectorType::get(c.ity(elem_bits), lanes);
    Value* vec = c.B.CreateBitCast(c.xmm(src), vty);
    Value* signs = c.B.CreateICmpSLT(vec, llvm::ConstantAggregateZero::get(vty));
    Value* mask = c.B.CreateZExt(c.B.CreateBitCast(signs, c.ity(lanes)), c.B.getInt32Ty());
    c.set_gpr(d.ops[0].reg.value, mask);
    return Lifted::yes;
}

Lifted lift_scalar_fp_bin(Ctx& c, const DInsn& d, bool is_double) {
    const int dst = Ctx::xmm_index(d.ops[0].reg.value);
    if (dst < 0) return fail_insn(c, d);
    bool ok = true;
    Value* a = read_scalar_fp(c, d, 0, is_double, &ok);
    Value* b = read_scalar_fp(c, d, 1, is_double, &ok);
    if (!ok) return fail_insn(c, d);
    Value* r = nullptr;
    switch (d.ins.mnemonic) {
    case ZYDIS_MNEMONIC_ADDSS:
    case ZYDIS_MNEMONIC_ADDSD:
        // Note: on two NaN inputs, plain IEEE/ARM64 fadd may pick differently than x86's first-operand NaN rule (accepted per R1 spec).
        r = c.B.CreateFAdd(a, b);
        break;
    case ZYDIS_MNEMONIC_SUBSS:
    case ZYDIS_MNEMONIC_SUBSD:
        r = c.B.CreateFSub(a, b);
        break;
    case ZYDIS_MNEMONIC_MULSS:
    case ZYDIS_MNEMONIC_MULSD:
        r = c.B.CreateFMul(a, b);
        break;
    case ZYDIS_MNEMONIC_DIVSS:
    case ZYDIS_MNEMONIC_DIVSD:
        r = c.B.CreateFDiv(a, b);
        break;
    case ZYDIS_MNEMONIC_MINSS:
    case ZYDIS_MNEMONIC_MINSD:
        // Intel SDM: a < b ? a : b (second operand returned on NaN or +/-0.0).
        r = c.B.CreateSelect(c.B.CreateFCmpOLT(a, b), a, b);
        break;
    case ZYDIS_MNEMONIC_MAXSS:
    case ZYDIS_MNEMONIC_MAXSD:
        r = c.B.CreateSelect(c.B.CreateFCmpOGT(a, b), a, b);
        break;
    default:
        return fail_insn(c, d);
    }
    write_scalar_merge(c, dst, r);
    return Lifted::yes;
}

Lifted lift_packed_fp_bin(Ctx& c, const DInsn& d, bool is_double) {
    const int dst = Ctx::xmm_index(d.ops[0].reg.value);
    if (dst < 0) return fail_insn(c, d);
    Type* vty = is_double ? static_cast<Type*>(v2d_ty(c)) : static_cast<Type*>(v4f_ty(c));
    bool ok = true;
    Value* a = read_vec(c, d, 0, vty, &ok);
    Value* b = read_vec(c, d, 1, vty, &ok);
    if (!ok) return fail_insn(c, d);
    Value* r = nullptr;
    switch (d.ins.mnemonic) {
    case ZYDIS_MNEMONIC_ADDPS:
    case ZYDIS_MNEMONIC_ADDPD:
        // Note: on two NaN inputs, plain IEEE/ARM64 fadd may pick differently than x86's first-operand NaN rule (accepted per R1 spec).
        r = c.B.CreateFAdd(a, b);
        break;
    case ZYDIS_MNEMONIC_SUBPS:
    case ZYDIS_MNEMONIC_SUBPD:
        r = c.B.CreateFSub(a, b);
        break;
    case ZYDIS_MNEMONIC_MULPS:
    case ZYDIS_MNEMONIC_MULPD:
        r = c.B.CreateFMul(a, b);
        break;
    case ZYDIS_MNEMONIC_DIVPS:
    case ZYDIS_MNEMONIC_DIVPD:
        r = c.B.CreateFDiv(a, b);
        break;
    case ZYDIS_MNEMONIC_MINPS:
    case ZYDIS_MNEMONIC_MINPD:
        r = c.B.CreateSelect(c.B.CreateFCmpOLT(a, b), a, b);
        break;
    case ZYDIS_MNEMONIC_MAXPS:
    case ZYDIS_MNEMONIC_MAXPD:
        r = c.B.CreateSelect(c.B.CreateFCmpOGT(a, b), a, b);
        break;
    default:
        return fail_insn(c, d);
    }
    c.set_xmm(dst, r);
    return Lifted::yes;
}

Lifted lift_comi(Ctx& c, const DInsn& d, bool is_double) {
    bool ok = true;
    Value* a = read_scalar_fp(c, d, 0, is_double, &ok);
    Value* b = read_scalar_fp(c, d, 1, is_double, &ok);
    if (!ok) return fail_insn(c, d);
    c.set_flag(ZF, c.B.CreateFCmpUEQ(a, b));
    c.set_flag(PF, c.B.CreateFCmpUNO(a, b));
    c.set_flag(CF, c.B.CreateFCmpULT(a, b));
    c.set_flag(OF, c.B.getFalse());
    c.set_flag(SF, c.B.getFalse());
    c.set_flag(AF, c.B.getFalse());
    return Lifted::yes;
}

Lifted lift_shift_packed(Ctx& c, const DInsn& d, unsigned lane_bits, bool right, bool arith) {
    const int dst = Ctx::xmm_index(d.ops[0].reg.value);
    if (dst < 0) return fail_insn(c, d);
    const unsigned lanes = 128U / lane_bits;
    auto* vty = llvm::FixedVectorType::get(c.ity(lane_bits), lanes);
    Value* a = c.B.CreateBitCast(c.xmm(dst), vty);
    Value* cnt64 = nullptr;
    if (d.ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        cnt64 = c.B.getInt64(d.ops[1].imm.value.u & 0xFFULL);
    } else if (d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int src = Ctx::xmm_index(d.ops[1].reg.value);
        if (src < 0) return fail_insn(c, d);
        cnt64 = c.B.CreateTrunc(c.xmm(src), c.B.getInt64Ty());
    } else if (d.ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
        cnt64 = c.B.CreateZExtOrTrunc(c.load(c.address(d, d.ops[1]), d.ops[1].size), c.B.getInt64Ty());
    } else {
        return fail_insn(c, d);
    }
    // Intel SDM: count is unsigned 64-bit SRC[63:0]; counts >= lane_bits yield 0 (logical) or sign-fill (arith).
    Value* oob = c.B.CreateICmpUGE(cnt64, c.B.getInt64(lane_bits));
    Value* safe64 = c.B.CreateSelect(oob, c.B.getInt64(lane_bits - 1U), cnt64);
    Value* cnt_vec = splat_vec(c, c.B.CreateTrunc(safe64, c.ity(lane_bits)), lanes);
    Value* res = nullptr;
    if (!right) {
        res = c.B.CreateSelect(oob, llvm::ConstantAggregateZero::get(vty), c.B.CreateShl(a, cnt_vec));
    } else if (!arith) {
        res = c.B.CreateSelect(oob, llvm::ConstantAggregateZero::get(vty), c.B.CreateLShr(a, cnt_vec));
    } else {
        res = c.B.CreateAShr(a, cnt_vec);
    }
    c.set_xmm(dst, res);
    return Lifted::yes;
}

Lifted lift_unpack(Ctx& c, const DInsn& d, unsigned lane_bits, bool high) {
    const int dst = Ctx::xmm_index(d.ops[0].reg.value);
    if (dst < 0) return fail_insn(c, d);
    const unsigned n = 128U / lane_bits;
    auto* vty = llvm::FixedVectorType::get(c.ity(lane_bits), n);
    bool ok = true;
    Value* a = read_vec(c, d, 0, vty, &ok);
    Value* b = read_vec(c, d, 1, vty, &ok);
    if (!ok) return fail_insn(c, d);
    std::array<int, 16> mask{};
    const unsigned start = high ? n / 2U : 0U;
    for (unsigned i = 0; i < n / 2U; ++i) {
        mask[2U * i] = static_cast<int>(start + i);
        mask[2U * i + 1U] = static_cast<int>(n + start + i);
    }
    c.set_xmm(dst, c.B.CreateShuffleVector(a, b, llvm::ArrayRef<int>(mask.data(), n)));
    return Lifted::yes;
}

Lifted lift_pack(Ctx& c, const DInsn& d, unsigned src_bits, bool dst_unsigned) {
    const int dst = Ctx::xmm_index(d.ops[0].reg.value);
    if (dst < 0) return fail_insn(c, d);
    const unsigned half_n = 128U / src_bits;
    const unsigned dst_bits = src_bits / 2U;
    auto* svty = llvm::FixedVectorType::get(c.ity(src_bits), half_n);
    bool ok = true;
    Value* a = read_vec(c, d, 0, svty, &ok);
    Value* b = read_vec(c, d, 1, svty, &ok);
    if (!ok) return fail_insn(c, d);
    const std::int64_t lo = dst_unsigned ? 0 : -(1LL << (dst_bits - 1U));
    const std::int64_t hi = dst_unsigned ? ((1LL << dst_bits) - 1LL) : ((1LL << (dst_bits - 1U)) - 1LL);
    Value* vlo = llvm::ConstantInt::get(svty, static_cast<std::uint64_t>(lo), true);
    Value* vhi = llvm::ConstantInt::get(svty, static_cast<std::uint64_t>(hi), true);
    auto* dvty = llvm::FixedVectorType::get(c.ity(dst_bits), half_n);
    auto sat_narrow = [&](Value* v) {
        Value* clamped = c.B.CreateSelect(c.B.CreateICmpSLT(v, vlo), vlo, v);
        clamped = c.B.CreateSelect(c.B.CreateICmpSGT(clamped, vhi), vhi, clamped);
        return c.B.CreateTrunc(clamped, dvty);
    };
    std::array<int, 16> mask{};
    for (unsigned i = 0; i < 2U * half_n; ++i) {
        mask[i] = static_cast<int>(i);
    }
    c.set_xmm(dst, c.B.CreateShuffleVector(sat_narrow(a), sat_narrow(b),
                                           llvm::ArrayRef<int>(mask.data(), 2U * half_n)));
    return Lifted::yes;
}

Lifted lift_pmovx(Ctx& c, const DInsn& d, unsigned src_bits, unsigned dst_bits, bool sign_ext) {
    const int dst = Ctx::xmm_index(d.ops[0].reg.value);
    if (dst < 0) return fail_insn(c, d);
    const unsigned lanes = 128U / dst_bits;
    const unsigned src_total_bits = lanes * src_bits;
    Value* raw = nullptr;
    if (d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int src = Ctx::xmm_index(d.ops[1].reg.value);
        if (src < 0) return fail_insn(c, d);
        raw = c.B.CreateTrunc(c.xmm(src), c.ity(src_total_bits));
    } else if (d.ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
        raw = c.load(c.address(d, d.ops[1]), src_total_bits);
    } else {
        return fail_insn(c, d);
    }
    auto* svty = llvm::FixedVectorType::get(c.ity(src_bits), lanes);
    auto* dvty = llvm::FixedVectorType::get(c.ity(dst_bits), lanes);
    Value* sv = c.B.CreateBitCast(raw, svty);
    c.set_xmm(dst, sign_ext ? c.B.CreateSExt(sv, dvty) : c.B.CreateZExt(sv, dvty));
    return Lifted::yes;
}

Lifted lift_pextr(Ctx& c, const DInsn& d, unsigned elem_bits) {
    if (d.ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER) return fail_insn(c, d);
    const int src = Ctx::xmm_index(d.ops[1].reg.value);
    if (src < 0) return fail_insn(c, d);
    const unsigned lanes = 128U / elem_bits;
    const unsigned idx = static_cast<unsigned>(d.ops[2].imm.value.u) & (lanes - 1U);
    auto* vty = llvm::FixedVectorType::get(c.ity(elem_bits), lanes);
    Value* elem = c.B.CreateExtractElement(c.B.CreateBitCast(c.xmm(src), vty), static_cast<std::uint64_t>(idx));
    if (d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
        c.store(c.address(d, d.ops[0]), elem);
        return Lifted::yes;
    }
    if (d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && Ctx::gpr_index(d.ops[0].reg.value) >= 0) {
        c.set_gpr(d.ops[0].reg.value, c.B.CreateZExtOrTrunc(elem, c.ity(d.ops[0].size)));
        return Lifted::yes;
    }
    return fail_insn(c, d);
}

Lifted lift_pinsr(Ctx& c, const DInsn& d, unsigned elem_bits) {
    const int dst = Ctx::xmm_index(d.ops[0].reg.value);
    if (dst < 0) return fail_insn(c, d);
    const unsigned lanes = 128U / elem_bits;
    const unsigned idx = static_cast<unsigned>(d.ops[2].imm.value.u) & (lanes - 1U);
    auto* vty = llvm::FixedVectorType::get(c.ity(elem_bits), lanes);
    Value* vec = c.B.CreateBitCast(c.xmm(dst), vty);
    Value* elem = c.B.CreateTrunc(c.read(d, 1), c.ity(elem_bits));
    c.set_xmm(dst, c.B.CreateInsertElement(vec, elem, static_cast<std::uint64_t>(idx)));
    return Lifted::yes;
}

Lifted lift_horiz_fp(Ctx& c, const DInsn& d, bool is_double, bool sub) {
    const int dst = Ctx::xmm_index(d.ops[0].reg.value);
    if (dst < 0) return fail_insn(c, d);
    Type* vty = is_double ? static_cast<Type*>(v2d_ty(c)) : static_cast<Type*>(v4f_ty(c));
    bool ok = true;
    Value* a = read_vec(c, d, 0, vty, &ok);
    Value* b = read_vec(c, d, 1, vty, &ok);
    if (!ok) return fail_insn(c, d);
    Value* even = is_double ? c.B.CreateShuffleVector(a, b, llvm::ArrayRef<int>{0, 2})
                            : c.B.CreateShuffleVector(a, b, llvm::ArrayRef<int>{0, 2, 4, 6});
    Value* odd = is_double ? c.B.CreateShuffleVector(a, b, llvm::ArrayRef<int>{1, 3})
                           : c.B.CreateShuffleVector(a, b, llvm::ArrayRef<int>{1, 3, 5, 7});
    c.set_xmm(dst, sub ? c.B.CreateFSub(even, odd) : c.B.CreateFAdd(even, odd));
    return Lifted::yes;
}

Lifted lift_horiz_int(Ctx& c, const DInsn& d, unsigned elem_bits, bool sub, bool sat) {
    const int dst = Ctx::xmm_index(d.ops[0].reg.value);
    if (dst < 0) return fail_insn(c, d);
    const unsigned n = 128U / elem_bits;
    auto* vty = llvm::FixedVectorType::get(c.ity(elem_bits), n);
    bool ok = true;
    Value* a = read_vec(c, d, 0, vty, &ok);
    Value* b = read_vec(c, d, 1, vty, &ok);
    if (!ok) return fail_insn(c, d);
    std::array<int, 8> me{};
    std::array<int, 8> mo{};
    for (unsigned i = 0; i < n; ++i) {
        me[i] = static_cast<int>(2U * i);
        mo[i] = static_cast<int>(2U * i + 1U);
    }
    Value* even = c.B.CreateShuffleVector(a, b, llvm::ArrayRef<int>(me.data(), n));
    Value* odd = c.B.CreateShuffleVector(a, b, llvm::ArrayRef<int>(mo.data(), n));
    Value* res = sat ? c.B.CreateBinaryIntrinsic(sub ? llvm::Intrinsic::ssub_sat : llvm::Intrinsic::sadd_sat,
                                                 even, odd)
                     : (sub ? c.B.CreateSub(even, odd) : c.B.CreateAdd(even, odd));
    c.set_xmm(dst, res);
    return Lifted::yes;
}

}  // namespace

Lifted lift_sse(Ctx& c, const DInsn& d) {
    if (d.ins.encoding != ZYDIS_INSTRUCTION_ENCODING_LEGACY) {
        return Lifted::not_mine;
    }
    const auto mn = d.ins.mnemonic;
    if (has_mmx_op(d)) {
        return has_xmm_op(d) ? fail_insn(c, d) : Lifted::not_mine;
    }
    // `movsd` and `cmpsd` share mnemonics with 0-operand string instructions.
    if ((mn == ZYDIS_MNEMONIC_MOVSD || mn == ZYDIS_MNEMONIC_CMPSD) &&
        (d.ins.operand_count_visible < 2 || !has_xmm_op(d))) {
        return Lifted::not_mine;
    }

    auto& B = c.B;
    switch (mn) {
    // --- MXCSR --------------------------------------------------------------------------------
    case ZYDIS_MNEMONIC_LDMXCSR:
        if (d.ops[0].type != ZYDIS_OPERAND_TYPE_MEMORY) return fail_insn(c, d);
        B.CreateStore(c.load(c.address(d, d.ops[0]), 32), c.state_ptr(cpu_offset::mxcsr));
        return Lifted::yes;
    case ZYDIS_MNEMONIC_STMXCSR:
        if (d.ops[0].type != ZYDIS_OPERAND_TYPE_MEMORY) return fail_insn(c, d);
        c.store(c.address(d, d.ops[0]), B.CreateLoad(B.getInt32Ty(), c.state_ptr(cpu_offset::mxcsr)));
        return Lifted::yes;

    // --- Moves --------------------------------------------------------------------------------
    case ZYDIS_MNEMONIC_MOVSS:
        return lift_mov_scalar(c, d, 32);
    case ZYDIS_MNEMONIC_MOVSD:
        return lift_mov_scalar(c, d, 64);
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVAPD:
    case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_MOVDQA:
    case ZYDIS_MNEMONIC_MOVDQU:
    case ZYDIS_MNEMONIC_LDDQU:
    case ZYDIS_MNEMONIC_MOVNTDQA: {
        bool ok = true;
        Value* src = read_i128(c, d, 1, &ok);
        if (!ok) return fail_insn(c, d);
        if (d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const int dst = Ctx::xmm_index(d.ops[0].reg.value);
            if (dst < 0) return fail_insn(c, d);
            c.set_xmm(dst, src);
            return Lifted::yes;
        }
        if (d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            c.store(c.address(d, d.ops[0]), src);
            return Lifted::yes;
        }
        return fail_insn(c, d);
    }
    case ZYDIS_MNEMONIC_MOVNTPS:
    case ZYDIS_MNEMONIC_MOVNTPD:
    case ZYDIS_MNEMONIC_MOVNTDQ: {
        if (d.ops[0].type != ZYDIS_OPERAND_TYPE_MEMORY || d.ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER) {
            return fail_insn(c, d);
        }
        const int src = Ctx::xmm_index(d.ops[1].reg.value);
        if (src < 0) return fail_insn(c, d);
        c.store(c.address(d, d.ops[0]), c.xmm(src));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_MOVNTI:
        if (d.ops[0].type != ZYDIS_OPERAND_TYPE_MEMORY || d.ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER) {
            return Lifted::not_mine;
        }
        c.store(c.address(d, d.ops[0]), c.read(d, 1));
        return Lifted::yes;
    case ZYDIS_MNEMONIC_MOVD:
        return lift_movdq(c, d, 32);
    case ZYDIS_MNEMONIC_MOVQ:
        return lift_movdq(c, d, 64);
    case ZYDIS_MNEMONIC_MOVLPS:
    case ZYDIS_MNEMONIC_MOVLPD:
        return lift_mov_half64(c, d, 0);
    case ZYDIS_MNEMONIC_MOVHPS:
    case ZYDIS_MNEMONIC_MOVHPD:
        return lift_mov_half64(c, d, 1);
    case ZYDIS_MNEMONIC_MOVHLPS:
    case ZYDIS_MNEMONIC_MOVLHPS: {
        if (d.ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER || d.ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER) {
            return fail_insn(c, d);
        }
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        const int src = Ctx::xmm_index(d.ops[1].reg.value);
        if (dst < 0 || src < 0) return fail_insn(c, d);
        auto* v2i = v2i64_ty(c);
        Value* dv = B.CreateBitCast(c.xmm(dst), v2i);
        Value* sv = B.CreateBitCast(c.xmm(src), v2i);
        if (mn == ZYDIS_MNEMONIC_MOVHLPS) {
            c.set_xmm(dst, B.CreateInsertElement(dv, B.CreateExtractElement(sv, std::uint64_t{1}),
                                                 std::uint64_t{0}));
        } else {
            c.set_xmm(dst, B.CreateInsertElement(dv, B.CreateExtractElement(sv, std::uint64_t{0}),
                                                 std::uint64_t{1}));
        }
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_MOVMSKPS:
        return lift_movmsk(c, d, 4, 32);
    case ZYDIS_MNEMONIC_MOVMSKPD:
        return lift_movmsk(c, d, 2, 64);
    case ZYDIS_MNEMONIC_PMOVMSKB:
        return lift_movmsk(c, d, 16, 8);
    case ZYDIS_MNEMONIC_MOVDDUP: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Value* lo = nullptr;
        if (d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const int src = Ctx::xmm_index(d.ops[1].reg.value);
            if (src < 0) return fail_insn(c, d);
            lo = B.CreateTrunc(c.xmm(src), B.getInt64Ty());
        } else if (d.ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            lo = c.load(c.address(d, d.ops[1]), 64);
        } else {
            return fail_insn(c, d);
        }
        c.set_xmm(dst, splat_vec(c, lo, 2));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_MOVSLDUP:
    case ZYDIS_MNEMONIC_MOVSHDUP: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_vec(c, d, 1, v4i32_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        // (An ArrayRef of a braced list would dangle after this statement: keep real arrays.)
        static constexpr int kLow[4] = {0, 0, 2, 2};
        static constexpr int kHigh[4] = {1, 1, 3, 3};
        const llvm::ArrayRef<int> mask(mn == ZYDIS_MNEMONIC_MOVSLDUP ? kLow : kHigh, 4);
        c.set_xmm(dst, B.CreateShuffleVector(s, llvm::PoisonValue::get(v4i32_ty(c)), mask));
        return Lifted::yes;
    }

    // --- Scalar float / double ----------------------------------------------------------------
    case ZYDIS_MNEMONIC_ADDSS:
    case ZYDIS_MNEMONIC_SUBSS:
    case ZYDIS_MNEMONIC_MULSS:
    case ZYDIS_MNEMONIC_DIVSS:
    case ZYDIS_MNEMONIC_MINSS:
    case ZYDIS_MNEMONIC_MAXSS:
        return lift_scalar_fp_bin(c, d, false);
    case ZYDIS_MNEMONIC_ADDSD:
    case ZYDIS_MNEMONIC_SUBSD:
    case ZYDIS_MNEMONIC_MULSD:
    case ZYDIS_MNEMONIC_DIVSD:
    case ZYDIS_MNEMONIC_MINSD:
    case ZYDIS_MNEMONIC_MAXSD:
        return lift_scalar_fp_bin(c, d, true);
    case ZYDIS_MNEMONIC_SQRTSS:
    case ZYDIS_MNEMONIC_SQRTSD: {
        const bool is_d = mn == ZYDIS_MNEMONIC_SQRTSD;
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_scalar_fp(c, d, 1, is_d, &ok);
        if (!ok) return fail_insn(c, d);
        write_scalar_merge(c, dst, B.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, s));
        return Lifted::yes;
    }
    // RSQRT/RCP give hardware approximations (relative error <= 1.5 * 2^-12, Intel SDM; the
    // exact bits differ between Intel and AMD). R1 deviation: the exact 1/sqrt(x) and 1/x, which
    // are within that bound; code refining them with a Newton step gets the same result or a
    // closer one.
    case ZYDIS_MNEMONIC_RSQRTSS:
    case ZYDIS_MNEMONIC_RCPSS: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_scalar_fp(c, d, 1, false, &ok);
        if (!ok) return fail_insn(c, d);
        Value* den = mn == ZYDIS_MNEMONIC_RSQRTSS ? B.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, s) : s;
        write_scalar_merge(c, dst, B.CreateFDiv(llvm::ConstantFP::get(B.getFloatTy(), 1.0), den));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_RSQRTPS:
    case ZYDIS_MNEMONIC_RCPPS: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_vec(c, d, 1, v4f_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        Value* den = mn == ZYDIS_MNEMONIC_RSQRTPS ? B.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, s) : s;
        c.set_xmm(dst, B.CreateFDiv(llvm::ConstantFP::get(v4f_ty(c), 1.0), den));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_COMISS:
    case ZYDIS_MNEMONIC_UCOMISS:
        return lift_comi(c, d, false);
    case ZYDIS_MNEMONIC_COMISD:
    case ZYDIS_MNEMONIC_UCOMISD:
        return lift_comi(c, d, true);
    case ZYDIS_MNEMONIC_CMPSS:
    case ZYDIS_MNEMONIC_CMPSD: {
        const bool is_d = mn == ZYDIS_MNEMONIC_CMPSD;
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_scalar_fp(c, d, 0, is_d, &ok);
        Value* b = read_scalar_fp(c, d, 1, is_d, &ok);
        if (!ok) return fail_insn(c, d);
        Value* cmp = B.CreateFCmp(fp_cmp_pred(static_cast<unsigned>(d.ops[2].imm.value.u)), a, b);
        write_scalar_merge(c, dst, B.CreateSExt(cmp, c.ity(is_d ? 64U : 32U)));
        return Lifted::yes;
    }

    // --- Packed float / double ----------------------------------------------------------------
    case ZYDIS_MNEMONIC_ADDPS:
    case ZYDIS_MNEMONIC_SUBPS:
    case ZYDIS_MNEMONIC_MULPS:
    case ZYDIS_MNEMONIC_DIVPS:
    case ZYDIS_MNEMONIC_MINPS:
    case ZYDIS_MNEMONIC_MAXPS:
        return lift_packed_fp_bin(c, d, false);
    case ZYDIS_MNEMONIC_ADDPD:
    case ZYDIS_MNEMONIC_SUBPD:
    case ZYDIS_MNEMONIC_MULPD:
    case ZYDIS_MNEMONIC_DIVPD:
    case ZYDIS_MNEMONIC_MINPD:
    case ZYDIS_MNEMONIC_MAXPD:
        return lift_packed_fp_bin(c, d, true);
    case ZYDIS_MNEMONIC_SQRTPS:
    case ZYDIS_MNEMONIC_SQRTPD: {
        const bool is_d = mn == ZYDIS_MNEMONIC_SQRTPD;
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = is_d ? static_cast<Type*>(v2d_ty(c)) : static_cast<Type*>(v4f_ty(c));
        bool ok = true;
        Value* s = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        c.set_xmm(dst, B.CreateUnaryIntrinsic(llvm::Intrinsic::sqrt, s));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CMPPS:
    case ZYDIS_MNEMONIC_CMPPD: {
        const bool is_d = mn == ZYDIS_MNEMONIC_CMPPD;
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = is_d ? static_cast<Type*>(v2d_ty(c)) : static_cast<Type*>(v4f_ty(c));
        Type* ity = is_d ? static_cast<Type*>(v2i64_ty(c)) : static_cast<Type*>(v4i32_ty(c));
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        Value* cmp = B.CreateFCmp(fp_cmp_pred(static_cast<unsigned>(d.ops[2].imm.value.u)), a, b);
        c.set_xmm(dst, B.CreateSExt(cmp, ity));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_ANDPS:
    case ZYDIS_MNEMONIC_ANDPD:
    case ZYDIS_MNEMONIC_PAND:
    case ZYDIS_MNEMONIC_ANDNPS:
    case ZYDIS_MNEMONIC_ANDNPD:
    case ZYDIS_MNEMONIC_PANDN:
    case ZYDIS_MNEMONIC_ORPS:
    case ZYDIS_MNEMONIC_ORPD:
    case ZYDIS_MNEMONIC_POR:
    case ZYDIS_MNEMONIC_XORPS:
    case ZYDIS_MNEMONIC_XORPD:
    case ZYDIS_MNEMONIC_PXOR: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_i128(c, d, 0, &ok);
        Value* b = read_i128(c, d, 1, &ok);
        if (!ok) return fail_insn(c, d);
        Value* r = nullptr;
        if (mn == ZYDIS_MNEMONIC_ANDPS || mn == ZYDIS_MNEMONIC_ANDPD || mn == ZYDIS_MNEMONIC_PAND) {
            r = B.CreateAnd(a, b);
        } else if (mn == ZYDIS_MNEMONIC_ANDNPS || mn == ZYDIS_MNEMONIC_ANDNPD || mn == ZYDIS_MNEMONIC_PANDN) {
            r = B.CreateAnd(B.CreateNot(a), b);
        } else if (mn == ZYDIS_MNEMONIC_ORPS || mn == ZYDIS_MNEMONIC_ORPD || mn == ZYDIS_MNEMONIC_POR) {
            r = B.CreateOr(a, b);
        } else {
            r = B.CreateXor(a, b);
        }
        c.set_xmm(dst, r);
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_SHUFPS: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_vec(c, d, 0, v4i32_ty(c), &ok);
        Value* b = read_vec(c, d, 1, v4i32_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        const unsigned imm = static_cast<unsigned>(d.ops[2].imm.value.u);
        const int mask[4] = {
            static_cast<int>(imm & 3U),
            static_cast<int>((imm >> 2U) & 3U),
            static_cast<int>(4U + ((imm >> 4U) & 3U)),
            static_cast<int>(4U + ((imm >> 6U) & 3U)),
        };
        c.set_xmm(dst, B.CreateShuffleVector(a, b, llvm::ArrayRef<int>(mask, 4)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_SHUFPD: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_vec(c, d, 0, v2i64_ty(c), &ok);
        Value* b = read_vec(c, d, 1, v2i64_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        const unsigned imm = static_cast<unsigned>(d.ops[2].imm.value.u);
        const int mask[2] = {static_cast<int>(imm & 1U), static_cast<int>(2U + ((imm >> 1U) & 1U))};
        c.set_xmm(dst, B.CreateShuffleVector(a, b, llvm::ArrayRef<int>(mask, 2)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_UNPCKLPS:
        return lift_unpack(c, d, 32, false);
    case ZYDIS_MNEMONIC_UNPCKHPS:
        return lift_unpack(c, d, 32, true);
    case ZYDIS_MNEMONIC_UNPCKLPD:
        return lift_unpack(c, d, 64, false);
    case ZYDIS_MNEMONIC_UNPCKHPD:
        return lift_unpack(c, d, 64, true);
    case ZYDIS_MNEMONIC_HADDPS:
        return lift_horiz_fp(c, d, false, false);
    case ZYDIS_MNEMONIC_HSUBPS:
        return lift_horiz_fp(c, d, false, true);
    case ZYDIS_MNEMONIC_HADDPD:
        return lift_horiz_fp(c, d, true, false);
    case ZYDIS_MNEMONIC_HSUBPD:
        return lift_horiz_fp(c, d, true, true);
    case ZYDIS_MNEMONIC_ADDSUBPS:
    case ZYDIS_MNEMONIC_ADDSUBPD: {
        const bool is_d = mn == ZYDIS_MNEMONIC_ADDSUBPD;
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = is_d ? static_cast<Type*>(v2d_ty(c)) : static_cast<Type*>(v4f_ty(c));
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        Value* sub = B.CreateFSub(a, b);
        Value* add = B.CreateFAdd(a, b);
        c.set_xmm(dst, is_d ? B.CreateShuffleVector(sub, add, llvm::ArrayRef<int>{0, 3})
                            : B.CreateShuffleVector(sub, add, llvm::ArrayRef<int>{0, 5, 2, 7}));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_DPPS:
    case ZYDIS_MNEMONIC_DPPD: {
        const bool is_d = mn == ZYDIS_MNEMONIC_DPPD;
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        const unsigned n = is_d ? 2U : 4U;
        Type* vty = is_d ? static_cast<Type*>(v2d_ty(c)) : static_cast<Type*>(v4f_ty(c));
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        const unsigned imm = static_cast<unsigned>(d.ops[2].imm.value.u);
        Value* prod = B.CreateFMul(a, b);
        Value* zero = llvm::ConstantFP::get(vty, 0.0);
        std::array<int, 4> in_mask{};
        std::array<int, 4> out_mask{};
        for (unsigned k = 0; k < n; ++k) {
            in_mask[k] = static_cast<int>(((imm >> (4U + k)) & 1U) ? k : (n + k));
            out_mask[k] = static_cast<int>(((imm >> k) & 1U) ? k : (n + k));
        }
        Value* masked = B.CreateShuffleVector(prod, zero, llvm::ArrayRef<int>(in_mask.data(), n));
        Value* sum = nullptr;
        if (is_d) {
            sum = B.CreateFAdd(B.CreateExtractElement(masked, std::uint64_t{0}),
                               B.CreateExtractElement(masked, std::uint64_t{1}));
        } else {
            Value* s01 = B.CreateFAdd(B.CreateExtractElement(masked, std::uint64_t{0}),
                                      B.CreateExtractElement(masked, std::uint64_t{1}));
            Value* s23 = B.CreateFAdd(B.CreateExtractElement(masked, std::uint64_t{2}),
                                      B.CreateExtractElement(masked, std::uint64_t{3}));
            sum = B.CreateFAdd(s01, s23);
        }
        c.set_xmm(dst, B.CreateShuffleVector(splat_vec(c, sum, n), zero,
                                             llvm::ArrayRef<int>(out_mask.data(), n)));
        return Lifted::yes;
    }

    // --- Conversions & Rounding ---------------------------------------------------------------
    case ZYDIS_MNEMONIC_CVTSI2SS: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Value* src = B.CreateSExt(c.read(d, 1), B.getInt64Ty());
        write_scalar_merge(c, dst, si64_to_fp_mxcsr(c, src, B.getFloatTy()));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CVTSI2SD: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Value* raw = c.read(d, 1);
        Value* fp = raw->getType()->getIntegerBitWidth() <= 32U
                        ? B.CreateSIToFP(raw, B.getDoubleTy())
                        : si64_to_fp_mxcsr(c, raw, B.getDoubleTy());
        write_scalar_merge(c, dst, fp);
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CVTSS2SD: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_scalar_fp(c, d, 1, false, &ok);
        if (!ok) return fail_insn(c, d);
        write_scalar_merge(c, dst, B.CreateFPExt(s, B.getDoubleTy()));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CVTSD2SS: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_scalar_fp(c, d, 1, true, &ok);
        if (!ok) return fail_insn(c, d);
        // R1 deviation: MXCSR.RC ignored
        write_scalar_merge(c, dst, B.CreateFPTrunc(s, B.getFloatTy()));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CVTTSS2SI:
    case ZYDIS_MNEMONIC_CVTSS2SI:
    case ZYDIS_MNEMONIC_CVTTSD2SI:
    case ZYDIS_MNEMONIC_CVTSD2SI: {
        if (d.ops[0].type != ZYDIS_OPERAND_TYPE_REGISTER || Ctx::gpr_index(d.ops[0].reg.value) < 0) {
            return fail_insn(c, d);
        }
        const bool is_d = (mn == ZYDIS_MNEMONIC_CVTTSD2SI || mn == ZYDIS_MNEMONIC_CVTSD2SI);
        const bool is_trunc = (mn == ZYDIS_MNEMONIC_CVTTSS2SI || mn == ZYDIS_MNEMONIC_CVTTSD2SI);
        bool ok = true;
        Value* s = read_scalar_fp(c, d, 1, is_d, &ok);
        if (!ok) return fail_insn(c, d);
        c.set_gpr(d.ops[0].reg.value, fp_to_si_checked(c, s, c.ity(d.ops[0].size), is_trunc));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CVTDQ2PS: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_vec(c, d, 1, v4i32_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        c.set_xmm(dst, v4i32_to_v4f_mxcsr(c, s));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CVTPS2DQ:
    case ZYDIS_MNEMONIC_CVTTPS2DQ: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_vec(c, d, 1, v4f_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        c.set_xmm(dst, fp_to_si_checked(c, s, v4i32_ty(c), mn == ZYDIS_MNEMONIC_CVTTPS2DQ));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CVTDQ2PD:
    case ZYDIS_MNEMONIC_CVTPS2PD: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Value* lo = nullptr;
        if (d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const int src = Ctx::xmm_index(d.ops[1].reg.value);
            if (src < 0) return fail_insn(c, d);
            lo = B.CreateTrunc(c.xmm(src), B.getInt64Ty());
        } else if (d.ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            lo = c.load(c.address(d, d.ops[1]), 64);
        } else {
            return fail_insn(c, d);
        }
        if (mn == ZYDIS_MNEMONIC_CVTDQ2PD) {
            Value* vi = B.CreateBitCast(lo, llvm::FixedVectorType::get(B.getInt32Ty(), 2));
            c.set_xmm(dst, B.CreateSIToFP(vi, v2d_ty(c)));
        } else {
            Value* vf = B.CreateBitCast(lo, llvm::FixedVectorType::get(B.getFloatTy(), 2));
            c.set_xmm(dst, B.CreateFPExt(vf, v2d_ty(c)));
        }
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CVTPD2DQ:
    case ZYDIS_MNEMONIC_CVTTPD2DQ: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_vec(c, d, 1, v2d_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        auto* v2i32 = llvm::FixedVectorType::get(B.getInt32Ty(), 2);
        Value* low64 = B.CreateBitCast(fp_to_si_checked(c, s, v2i32, mn == ZYDIS_MNEMONIC_CVTTPD2DQ),
                                       B.getInt64Ty());
        // Intel SDM: upper 64 bits of destination are zeroed.
        c.set_xmm(dst, B.CreateZExt(low64, c.i128()));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_CVTPD2PS: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_vec(c, d, 1, v2d_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        auto* v2f32 = llvm::FixedVectorType::get(B.getFloatTy(), 2);
        // R1 deviation: MXCSR.RC ignored
        Value* low64 = B.CreateBitCast(B.CreateFPTrunc(s, v2f32), B.getInt64Ty());
        // Intel SDM: upper 64 bits of destination are zeroed.
        c.set_xmm(dst, B.CreateZExt(low64, c.i128()));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_ROUNDSS:
    case ZYDIS_MNEMONIC_ROUNDSD: {
        const bool is_d = mn == ZYDIS_MNEMONIC_ROUNDSD;
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_scalar_fp(c, d, 1, is_d, &ok);
        if (!ok) return fail_insn(c, d);
        const unsigned imm = static_cast<unsigned>(d.ops[2].imm.value.u);
        Value* r = (imm & 4U) != 0 ? round_with_mode(c, s, mxcsr_rc(c)) : round_with_imm(c, s, imm);
        write_scalar_merge(c, dst, r);
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_ROUNDPS:
    case ZYDIS_MNEMONIC_ROUNDPD: {
        const bool is_d = mn == ZYDIS_MNEMONIC_ROUNDPD;
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = is_d ? static_cast<Type*>(v2d_ty(c)) : static_cast<Type*>(v4f_ty(c));
        bool ok = true;
        Value* s = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        const unsigned imm = static_cast<unsigned>(d.ops[2].imm.value.u);
        Value* r = (imm & 4U) != 0 ? round_with_mode(c, s, mxcsr_rc(c)) : round_with_imm(c, s, imm);
        c.set_xmm(dst, r);
        return Lifted::yes;
    }

    // --- Packed integer -----------------------------------------------------------------------
    case ZYDIS_MNEMONIC_PADDB:
    case ZYDIS_MNEMONIC_PADDW:
    case ZYDIS_MNEMONIC_PADDD:
    case ZYDIS_MNEMONIC_PADDQ:
    case ZYDIS_MNEMONIC_PSUBB:
    case ZYDIS_MNEMONIC_PSUBW:
    case ZYDIS_MNEMONIC_PSUBD:
    case ZYDIS_MNEMONIC_PSUBQ: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = (mn == ZYDIS_MNEMONIC_PADDB || mn == ZYDIS_MNEMONIC_PSUBB) ? v16i8_ty(c)
                  : (mn == ZYDIS_MNEMONIC_PADDW || mn == ZYDIS_MNEMONIC_PSUBW) ? v8i16_ty(c)
                  : (mn == ZYDIS_MNEMONIC_PADDD || mn == ZYDIS_MNEMONIC_PSUBD) ? v4i32_ty(c)
                                                                               : v2i64_ty(c);
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        const bool is_sub = mn == ZYDIS_MNEMONIC_PSUBB || mn == ZYDIS_MNEMONIC_PSUBW ||
                            mn == ZYDIS_MNEMONIC_PSUBD || mn == ZYDIS_MNEMONIC_PSUBQ;
        c.set_xmm(dst, is_sub ? B.CreateSub(a, b) : B.CreateAdd(a, b));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PADDSB:
    case ZYDIS_MNEMONIC_PADDSW:
    case ZYDIS_MNEMONIC_PADDUSB:
    case ZYDIS_MNEMONIC_PADDUSW:
    case ZYDIS_MNEMONIC_PSUBSB:
    case ZYDIS_MNEMONIC_PSUBSW:
    case ZYDIS_MNEMONIC_PSUBUSB:
    case ZYDIS_MNEMONIC_PSUBUSW: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        const bool is_word = mn == ZYDIS_MNEMONIC_PADDSW || mn == ZYDIS_MNEMONIC_PADDUSW ||
                             mn == ZYDIS_MNEMONIC_PSUBSW || mn == ZYDIS_MNEMONIC_PSUBUSW;
        Type* vty = is_word ? static_cast<Type*>(v8i16_ty(c)) : static_cast<Type*>(v16i8_ty(c));
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        llvm::Intrinsic::ID id = llvm::Intrinsic::sadd_sat;
        if (mn == ZYDIS_MNEMONIC_PADDUSB || mn == ZYDIS_MNEMONIC_PADDUSW) id = llvm::Intrinsic::uadd_sat;
        else if (mn == ZYDIS_MNEMONIC_PSUBSB || mn == ZYDIS_MNEMONIC_PSUBSW) id = llvm::Intrinsic::ssub_sat;
        else if (mn == ZYDIS_MNEMONIC_PSUBUSB || mn == ZYDIS_MNEMONIC_PSUBUSW) id = llvm::Intrinsic::usub_sat;
        c.set_xmm(dst, B.CreateBinaryIntrinsic(id, a, b));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PMULLW:
    case ZYDIS_MNEMONIC_PMULLD: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = mn == ZYDIS_MNEMONIC_PMULLW ? static_cast<Type*>(v8i16_ty(c))
                                                : static_cast<Type*>(v4i32_ty(c));
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        c.set_xmm(dst, B.CreateMul(a, b));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PMULHW:
    case ZYDIS_MNEMONIC_PMULHUW: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_vec(c, d, 0, v8i16_ty(c), &ok);
        Value* b = read_vec(c, d, 1, v8i16_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        auto* v8i32 = llvm::FixedVectorType::get(B.getInt32Ty(), 8);
        const bool sgn = mn == ZYDIS_MNEMONIC_PMULHW;
        Value* aw = sgn ? B.CreateSExt(a, v8i32) : B.CreateZExt(a, v8i32);
        Value* bw = sgn ? B.CreateSExt(b, v8i32) : B.CreateZExt(b, v8i32);
        Value* p = B.CreateMul(aw, bw);
        c.set_xmm(dst, B.CreateTrunc(B.CreateLShr(p, 16), v8i16_ty(c)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PMULUDQ: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_vec(c, d, 0, v2i64_ty(c), &ok);
        Value* b = read_vec(c, d, 1, v2i64_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        Value* mask = llvm::ConstantInt::get(v2i64_ty(c), 0xFFFFFFFFULL, false);
        c.set_xmm(dst, B.CreateMul(B.CreateAnd(a, mask), B.CreateAnd(b, mask)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PMADDWD: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_vec(c, d, 0, v8i16_ty(c), &ok);
        Value* b = read_vec(c, d, 1, v8i16_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        auto* v8i32 = llvm::FixedVectorType::get(B.getInt32Ty(), 8);
        Value* p = B.CreateMul(B.CreateSExt(a, v8i32), B.CreateSExt(b, v8i32));
        Value* even = B.CreateShuffleVector(p, llvm::PoisonValue::get(v8i32), llvm::ArrayRef<int>{0, 2, 4, 6});
        Value* odd = B.CreateShuffleVector(p, llvm::PoisonValue::get(v8i32), llvm::ArrayRef<int>{1, 3, 5, 7});
        c.set_xmm(dst, B.CreateAdd(even, odd));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PMULHRSW: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_vec(c, d, 0, v8i16_ty(c), &ok);
        Value* b = read_vec(c, d, 1, v8i16_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        auto* v8i32 = llvm::FixedVectorType::get(B.getInt32Ty(), 8);
        Value* p = B.CreateMul(B.CreateSExt(a, v8i32), B.CreateSExt(b, v8i32));
        Value* rounded = B.CreateAdd(p, llvm::ConstantInt::get(v8i32, 0x4000, false));
        c.set_xmm(dst, B.CreateTrunc(B.CreateAShr(rounded, 15), v8i16_ty(c)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PCMPEQB:
    case ZYDIS_MNEMONIC_PCMPEQW:
    case ZYDIS_MNEMONIC_PCMPEQD:
    case ZYDIS_MNEMONIC_PCMPEQQ:
    case ZYDIS_MNEMONIC_PCMPGTB:
    case ZYDIS_MNEMONIC_PCMPGTW:
    case ZYDIS_MNEMONIC_PCMPGTD:
    case ZYDIS_MNEMONIC_PCMPGTQ: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = (mn == ZYDIS_MNEMONIC_PCMPEQB || mn == ZYDIS_MNEMONIC_PCMPGTB) ? v16i8_ty(c)
                  : (mn == ZYDIS_MNEMONIC_PCMPEQW || mn == ZYDIS_MNEMONIC_PCMPGTW) ? v8i16_ty(c)
                  : (mn == ZYDIS_MNEMONIC_PCMPEQD || mn == ZYDIS_MNEMONIC_PCMPGTD) ? v4i32_ty(c)
                                                                                   : v2i64_ty(c);
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        const bool gt = mn == ZYDIS_MNEMONIC_PCMPGTB || mn == ZYDIS_MNEMONIC_PCMPGTW ||
                        mn == ZYDIS_MNEMONIC_PCMPGTD || mn == ZYDIS_MNEMONIC_PCMPGTQ;
        Value* cmp = gt ? B.CreateICmpSGT(a, b) : B.CreateICmpEQ(a, b);
        c.set_xmm(dst, B.CreateSExt(cmp, vty));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PMINUB:
    case ZYDIS_MNEMONIC_PMAXUB:
    case ZYDIS_MNEMONIC_PMINSB:
    case ZYDIS_MNEMONIC_PMAXSB:
    case ZYDIS_MNEMONIC_PMINUW:
    case ZYDIS_MNEMONIC_PMAXUW:
    case ZYDIS_MNEMONIC_PMINSW:
    case ZYDIS_MNEMONIC_PMAXSW:
    case ZYDIS_MNEMONIC_PMINUD:
    case ZYDIS_MNEMONIC_PMAXUD:
    case ZYDIS_MNEMONIC_PMINSD:
    case ZYDIS_MNEMONIC_PMAXSD: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = (mn == ZYDIS_MNEMONIC_PMINUB || mn == ZYDIS_MNEMONIC_PMAXUB ||
                     mn == ZYDIS_MNEMONIC_PMINSB || mn == ZYDIS_MNEMONIC_PMAXSB)
                        ? static_cast<Type*>(v16i8_ty(c))
                  : (mn == ZYDIS_MNEMONIC_PMINUW || mn == ZYDIS_MNEMONIC_PMAXUW ||
                     mn == ZYDIS_MNEMONIC_PMINSW || mn == ZYDIS_MNEMONIC_PMAXSW)
                        ? static_cast<Type*>(v8i16_ty(c))
                        : static_cast<Type*>(v4i32_ty(c));
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        const bool is_max = mn == ZYDIS_MNEMONIC_PMAXUB || mn == ZYDIS_MNEMONIC_PMAXSB ||
                            mn == ZYDIS_MNEMONIC_PMAXUW || mn == ZYDIS_MNEMONIC_PMAXSW ||
                            mn == ZYDIS_MNEMONIC_PMAXUD || mn == ZYDIS_MNEMONIC_PMAXSD;
        const bool is_signed = mn == ZYDIS_MNEMONIC_PMINSB || mn == ZYDIS_MNEMONIC_PMAXSB ||
                               mn == ZYDIS_MNEMONIC_PMINSW || mn == ZYDIS_MNEMONIC_PMAXSW ||
                               mn == ZYDIS_MNEMONIC_PMINSD || mn == ZYDIS_MNEMONIC_PMAXSD;
        Value* pick_a = is_max ? (is_signed ? B.CreateICmpSGT(a, b) : B.CreateICmpUGT(a, b))
                               : (is_signed ? B.CreateICmpSLT(a, b) : B.CreateICmpULT(a, b));
        c.set_xmm(dst, B.CreateSelect(pick_a, a, b));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PSLLW: return lift_shift_packed(c, d, 16, false, false);
    case ZYDIS_MNEMONIC_PSLLD: return lift_shift_packed(c, d, 32, false, false);
    case ZYDIS_MNEMONIC_PSLLQ: return lift_shift_packed(c, d, 64, false, false);
    case ZYDIS_MNEMONIC_PSRLW: return lift_shift_packed(c, d, 16, true, false);
    case ZYDIS_MNEMONIC_PSRLD: return lift_shift_packed(c, d, 32, true, false);
    case ZYDIS_MNEMONIC_PSRLQ: return lift_shift_packed(c, d, 64, true, false);
    case ZYDIS_MNEMONIC_PSRAW: return lift_shift_packed(c, d, 16, true, true);
    case ZYDIS_MNEMONIC_PSRAD: return lift_shift_packed(c, d, 32, true, true);
    case ZYDIS_MNEMONIC_PSLLDQ:
    case ZYDIS_MNEMONIC_PSRLDQ: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        const unsigned cnt = static_cast<unsigned>(d.ops[1].imm.value.u) & 0xFFU;
        if (cnt >= 16U) {
            c.set_xmm(dst, B.getIntN(128, 0));
        } else if (cnt > 0U) {
            Value* v = c.xmm(dst);
            c.set_xmm(dst, mn == ZYDIS_MNEMONIC_PSLLDQ ? B.CreateShl(v, cnt * 8U)
                                                       : B.CreateLShr(v, cnt * 8U));
        }
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PSHUFD: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_vec(c, d, 1, v4i32_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        const unsigned imm = static_cast<unsigned>(d.ops[2].imm.value.u);
        const int mask[4] = {
            static_cast<int>(imm & 3U),
            static_cast<int>((imm >> 2U) & 3U),
            static_cast<int>((imm >> 4U) & 3U),
            static_cast<int>((imm >> 6U) & 3U),
        };
        c.set_xmm(dst, B.CreateShuffleVector(s, llvm::PoisonValue::get(v4i32_ty(c)),
                                             llvm::ArrayRef<int>(mask, 4)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PSHUFLW:
    case ZYDIS_MNEMONIC_PSHUFHW: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* s = read_vec(c, d, 1, v8i16_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        const unsigned imm = static_cast<unsigned>(d.ops[2].imm.value.u);
        int mask[8] = {0, 1, 2, 3, 4, 5, 6, 7};
        const unsigned base = mn == ZYDIS_MNEMONIC_PSHUFHW ? 4U : 0U;
        for (unsigned k = 0; k < 4U; ++k) {
            mask[base + k] = static_cast<int>(base + ((imm >> (2U * k)) & 3U));
        }
        c.set_xmm(dst, B.CreateShuffleVector(s, llvm::PoisonValue::get(v8i16_ty(c)),
                                             llvm::ArrayRef<int>(mask, 8)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PSHUFB: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_vec(c, d, 0, v16i8_ty(c), &ok);
        Value* ctrl = read_vec(c, d, 1, v16i8_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        Value* idx = B.CreateAnd(ctrl, llvm::ConstantInt::get(v16i8_ty(c), 15, false));
        Value* res = llvm::PoisonValue::get(v16i8_ty(c));
        for (std::uint64_t k = 0; k < 16ULL; ++k) {
            Value* lane = B.CreateZExt(B.CreateExtractElement(idx, k), B.getInt32Ty());
            res = B.CreateInsertElement(res, B.CreateExtractElement(a, lane), k);
        }
        Value* zero = llvm::ConstantAggregateZero::get(v16i8_ty(c));
        c.set_xmm(dst, B.CreateSelect(B.CreateICmpSLT(ctrl, zero), zero, res));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PUNPCKLBW: return lift_unpack(c, d, 8, false);
    case ZYDIS_MNEMONIC_PUNPCKHBW: return lift_unpack(c, d, 8, true);
    case ZYDIS_MNEMONIC_PUNPCKLWD: return lift_unpack(c, d, 16, false);
    case ZYDIS_MNEMONIC_PUNPCKHWD: return lift_unpack(c, d, 16, true);
    case ZYDIS_MNEMONIC_PUNPCKLDQ: return lift_unpack(c, d, 32, false);
    case ZYDIS_MNEMONIC_PUNPCKHDQ: return lift_unpack(c, d, 32, true);
    case ZYDIS_MNEMONIC_PUNPCKLQDQ: return lift_unpack(c, d, 64, false);
    case ZYDIS_MNEMONIC_PUNPCKHQDQ: return lift_unpack(c, d, 64, true);
    case ZYDIS_MNEMONIC_PACKSSWB: return lift_pack(c, d, 16, false);
    case ZYDIS_MNEMONIC_PACKUSWB: return lift_pack(c, d, 16, true);
    case ZYDIS_MNEMONIC_PACKSSDW: return lift_pack(c, d, 32, false);
    case ZYDIS_MNEMONIC_PACKUSDW: return lift_pack(c, d, 32, true);
    case ZYDIS_MNEMONIC_PEXTRB: return lift_pextr(c, d, 8);
    case ZYDIS_MNEMONIC_PEXTRW: return lift_pextr(c, d, 16);
    case ZYDIS_MNEMONIC_PEXTRD: return lift_pextr(c, d, 32);
    case ZYDIS_MNEMONIC_PEXTRQ: return lift_pextr(c, d, 64);
    case ZYDIS_MNEMONIC_PINSRB: return lift_pinsr(c, d, 8);
    case ZYDIS_MNEMONIC_PINSRW: return lift_pinsr(c, d, 16);
    case ZYDIS_MNEMONIC_PINSRD: return lift_pinsr(c, d, 32);
    case ZYDIS_MNEMONIC_PINSRQ: return lift_pinsr(c, d, 64);
    case ZYDIS_MNEMONIC_PMOVZXBW: return lift_pmovx(c, d, 8, 16, false);
    case ZYDIS_MNEMONIC_PMOVZXBD: return lift_pmovx(c, d, 8, 32, false);
    case ZYDIS_MNEMONIC_PMOVZXBQ: return lift_pmovx(c, d, 8, 64, false);
    case ZYDIS_MNEMONIC_PMOVZXWD: return lift_pmovx(c, d, 16, 32, false);
    case ZYDIS_MNEMONIC_PMOVZXWQ: return lift_pmovx(c, d, 16, 64, false);
    case ZYDIS_MNEMONIC_PMOVZXDQ: return lift_pmovx(c, d, 32, 64, false);
    case ZYDIS_MNEMONIC_PMOVSXBW: return lift_pmovx(c, d, 8, 16, true);
    case ZYDIS_MNEMONIC_PMOVSXBD: return lift_pmovx(c, d, 8, 32, true);
    case ZYDIS_MNEMONIC_PMOVSXBQ: return lift_pmovx(c, d, 8, 64, true);
    case ZYDIS_MNEMONIC_PMOVSXWD: return lift_pmovx(c, d, 16, 32, true);
    case ZYDIS_MNEMONIC_PMOVSXWQ: return lift_pmovx(c, d, 16, 64, true);
    case ZYDIS_MNEMONIC_PMOVSXDQ: return lift_pmovx(c, d, 32, 64, true);
    case ZYDIS_MNEMONIC_PALIGNR: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_i128(c, d, 0, &ok);
        Value* b = read_i128(c, d, 1, &ok);
        if (!ok) return fail_insn(c, d);
        const unsigned shift = static_cast<unsigned>(d.ops[2].imm.value.u) & 0xFFU;
        if (shift >= 32U) {
            c.set_xmm(dst, B.getIntN(128, 0));
        } else {
            Type* i256 = c.ity(256);
            Value* concat = B.CreateOr(B.CreateShl(B.CreateZExt(a, i256), 128), B.CreateZExt(b, i256));
            c.set_xmm(dst, B.CreateTrunc(B.CreateLShr(concat, shift * 8U), c.i128()));
        }
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PABSB:
    case ZYDIS_MNEMONIC_PABSW:
    case ZYDIS_MNEMONIC_PABSD: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = mn == ZYDIS_MNEMONIC_PABSB ? static_cast<Type*>(v16i8_ty(c))
                  : mn == ZYDIS_MNEMONIC_PABSW ? static_cast<Type*>(v8i16_ty(c))
                                               : static_cast<Type*>(v4i32_ty(c));
        bool ok = true;
        Value* s = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        Value* zero = llvm::ConstantAggregateZero::get(vty);
        c.set_xmm(dst, B.CreateSelect(B.CreateICmpSLT(s, zero), B.CreateNeg(s), s));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PSIGNB:
    case ZYDIS_MNEMONIC_PSIGNW:
    case ZYDIS_MNEMONIC_PSIGND: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = mn == ZYDIS_MNEMONIC_PSIGNB ? static_cast<Type*>(v16i8_ty(c))
                  : mn == ZYDIS_MNEMONIC_PSIGNW ? static_cast<Type*>(v8i16_ty(c))
                                                : static_cast<Type*>(v4i32_ty(c));
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        Value* zero = llvm::ConstantAggregateZero::get(vty);
        Value* v = B.CreateSelect(B.CreateICmpSLT(b, zero), B.CreateNeg(a), a);
        c.set_xmm(dst, B.CreateSelect(B.CreateICmpEQ(b, zero), zero, v));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PHADDW: return lift_horiz_int(c, d, 16, false, false);
    case ZYDIS_MNEMONIC_PHADDD: return lift_horiz_int(c, d, 32, false, false);
    case ZYDIS_MNEMONIC_PHADDSW: return lift_horiz_int(c, d, 16, false, true);
    case ZYDIS_MNEMONIC_PHSUBW: return lift_horiz_int(c, d, 16, true, false);
    case ZYDIS_MNEMONIC_PHSUBD: return lift_horiz_int(c, d, 32, true, false);
    case ZYDIS_MNEMONIC_PHSUBSW: return lift_horiz_int(c, d, 16, true, true);
    case ZYDIS_MNEMONIC_PTEST: {
        bool ok = true;
        Value* a = read_i128(c, d, 0, &ok);
        Value* b = read_i128(c, d, 1, &ok);
        if (!ok) return fail_insn(c, d);
        Value* z = B.getIntN(128, 0);
        c.set_flag(ZF, B.CreateICmpEQ(B.CreateAnd(a, b), z));
        c.set_flag(CF, B.CreateICmpEQ(B.CreateAnd(B.CreateNot(a), b), z));
        c.set_flag(OF, B.getFalse());
        c.set_flag(SF, B.getFalse());
        c.set_flag(PF, B.getFalse());
        c.set_flag(AF, B.getFalse());
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PAVGB:
    case ZYDIS_MNEMONIC_PAVGW: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        const bool is_w = mn == ZYDIS_MNEMONIC_PAVGW;
        Type* nty = is_w ? static_cast<Type*>(v8i16_ty(c)) : static_cast<Type*>(v16i8_ty(c));
        auto* wty = llvm::FixedVectorType::get(is_w ? B.getInt32Ty() : B.getInt16Ty(), is_w ? 8U : 16U);
        bool ok = true;
        Value* a = read_vec(c, d, 0, nty, &ok);
        Value* b = read_vec(c, d, 1, nty, &ok);
        if (!ok) return fail_insn(c, d);
        Value* sum = B.CreateAdd(B.CreateAdd(B.CreateZExt(a, wty), B.CreateZExt(b, wty)),
                                 llvm::ConstantInt::get(wty, 1, false));
        c.set_xmm(dst, B.CreateTrunc(B.CreateLShr(sum, 1), nty));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_PSADBW: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        bool ok = true;
        Value* a = read_vec(c, d, 0, v16i8_ty(c), &ok);
        Value* b = read_vec(c, d, 1, v16i8_ty(c), &ok);
        if (!ok) return fail_insn(c, d);
        Value* diff8 = B.CreateSelect(B.CreateICmpULT(a, b), B.CreateSub(b, a), B.CreateSub(a, b));
        auto* v16i16 = llvm::FixedVectorType::get(B.getInt16Ty(), 16);
        Value* d16 = B.CreateZExt(diff8, v16i16);
        Value* s1 = B.CreateAdd(
            B.CreateShuffleVector(d16, llvm::PoisonValue::get(v16i16),
                                  llvm::ArrayRef<int>{0, 1, 2, 3, 8, 9, 10, 11}),
            B.CreateShuffleVector(d16, llvm::PoisonValue::get(v16i16),
                                  llvm::ArrayRef<int>{4, 5, 6, 7, 12, 13, 14, 15}));
        auto* v8i16 = v8i16_ty(c);
        Value* s2 = B.CreateAdd(
            B.CreateShuffleVector(s1, llvm::PoisonValue::get(v8i16), llvm::ArrayRef<int>{0, 1, 4, 5}),
            B.CreateShuffleVector(s1, llvm::PoisonValue::get(v8i16), llvm::ArrayRef<int>{2, 3, 6, 7}));
        auto* v4i16 = llvm::FixedVectorType::get(B.getInt16Ty(), 4);
        Value* s3 = B.CreateAdd(
            B.CreateShuffleVector(s2, llvm::PoisonValue::get(v4i16), llvm::ArrayRef<int>{0, 2}),
            B.CreateShuffleVector(s2, llvm::PoisonValue::get(v4i16), llvm::ArrayRef<int>{1, 3}));
        c.set_xmm(dst, B.CreateZExt(s3, v2i64_ty(c)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_EXTRACTPS:
        return lift_pextr(c, d, 32);
    case ZYDIS_MNEMONIC_INSERTPS: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        const unsigned imm = static_cast<unsigned>(d.ops[2].imm.value.u);
        Value* src_elem = nullptr;
        if (d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
            const int src = Ctx::xmm_index(d.ops[1].reg.value);
            if (src < 0) return fail_insn(c, d);
            const unsigned count_s = (imm >> 6U) & 3U;
            src_elem = B.CreateExtractElement(B.CreateBitCast(c.xmm(src), v4i32_ty(c)),
                                              static_cast<std::uint64_t>(count_s));
        } else if (d.ops[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
            // Intel SDM: COUNT_S is ignored for m32 source.
            src_elem = c.load(c.address(d, d.ops[1]), 32);
        } else {
            return fail_insn(c, d);
        }
        const unsigned count_d = (imm >> 4U) & 3U;
        Value* ins = B.CreateInsertElement(B.CreateBitCast(c.xmm(dst), v4i32_ty(c)), src_elem,
                                           static_cast<std::uint64_t>(count_d));
        const int mask[4] = {
            (imm & 1U) != 0 ? 4 : 0,
            (imm & 2U) != 0 ? 5 : 1,
            (imm & 4U) != 0 ? 6 : 2,
            (imm & 8U) != 0 ? 7 : 3,
        };
        c.set_xmm(dst, B.CreateShuffleVector(ins, llvm::ConstantAggregateZero::get(v4i32_ty(c)),
                                             llvm::ArrayRef<int>(mask, 4)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_BLENDPS:
    case ZYDIS_MNEMONIC_BLENDPD:
    case ZYDIS_MNEMONIC_PBLENDW: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        const unsigned n = mn == ZYDIS_MNEMONIC_BLENDPD ? 2U : (mn == ZYDIS_MNEMONIC_BLENDPS ? 4U : 8U);
        Type* vty = n == 2U ? static_cast<Type*>(v2i64_ty(c))
                            : (n == 4U ? static_cast<Type*>(v4i32_ty(c)) : static_cast<Type*>(v8i16_ty(c)));
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        const unsigned imm = static_cast<unsigned>(d.ops[2].imm.value.u);
        std::array<int, 8> mask{};
        for (unsigned k = 0; k < n; ++k) {
            mask[k] = static_cast<int>(((imm >> k) & 1U) != 0 ? (n + k) : k);
        }
        c.set_xmm(dst, B.CreateShuffleVector(a, b, llvm::ArrayRef<int>(mask.data(), n)));
        return Lifted::yes;
    }
    case ZYDIS_MNEMONIC_BLENDVPS:
    case ZYDIS_MNEMONIC_BLENDVPD:
    case ZYDIS_MNEMONIC_PBLENDVB: {
        const int dst = Ctx::xmm_index(d.ops[0].reg.value);
        if (dst < 0) return fail_insn(c, d);
        Type* vty = mn == ZYDIS_MNEMONIC_BLENDVPD ? static_cast<Type*>(v2i64_ty(c))
                  : mn == ZYDIS_MNEMONIC_BLENDVPS ? static_cast<Type*>(v4i32_ty(c))
                                                  : static_cast<Type*>(v16i8_ty(c));
        bool ok = true;
        Value* a = read_vec(c, d, 0, vty, &ok);
        Value* b = read_vec(c, d, 1, vty, &ok);
        if (!ok) return fail_insn(c, d);
        Value* mask_vec = B.CreateBitCast(c.xmm(0), vty);
        Value* pick_b = B.CreateICmpSLT(mask_vec, llvm::ConstantAggregateZero::get(vty));
        c.set_xmm(dst, B.CreateSelect(pick_b, b, a));
        return Lifted::yes;
    }
    default:
        break;
    }
    return has_xmm_op(d) ? fail_insn(c, d) : Lifted::not_mine;
}

}  // namespace wse::recomp::r1
