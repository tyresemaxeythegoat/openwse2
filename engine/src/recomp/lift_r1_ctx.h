// Internal context of the R1 lifter, shared by the instruction-family files
// (`lift_r1_int.cpp`, `lift_r1_sse.cpp`, `lift_r1_string.cpp`). Includes LLVM headers.
//
// Conventions every family follows:
// - Operands are read and written through `read()` / `write()` with Zydis operand sizes; a
//   32-bit GPR write zero-extends, 8/16-bit writes merge, AH..BH are bits 8..15.
// - Flags live in i1 slots (`flag()` / `set_flag()`); helpers compute them exactly as x86 does.
//   AF is computed by the arithmetic helpers; families that leave a flag undefined by the SDM
//   may leave it unchanged.
// - XMM registers are `<16 x i8>`-sized i128 slots (`xmm()` / `set_xmm()`); MXCSR is in state.
// - A family returns `Lifted::yes` after emitting the instruction, `Lifted::not_mine` when it
//   does not handle the mnemonic (the next family is tried), or `Lifted::unsupported` with a
//   reason in `why` (the instruction becomes a call to the `unsupported` hook).
// - Families never emit terminators; control flow belongs to `lift_r1.cpp`.

#pragma once

#include <cstdint>
#include <map>
#include <string>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>

#include "discover.h"
#include "lift_r1.h"
#include "wse/recomp/cpu_state.h"

namespace wse::recomp::r1 {

enum class Lifted { yes, not_mine, unsupported };

enum Flag : int { CF = 0, PF, AF, ZF, SF, OF, DF, kFlagCount };

class Ctx {
public:
    Ctx(llvm::Module& m, llvm::Function* f, const DFunction& fn, const ImageInfo& image);

    llvm::LLVMContext& C;
    llvm::Module& M;
    llvm::IRBuilder<> B;
    llvm::Function* F;
    const DFunction& fn;
    const ImageInfo& image;
    llvm::Value* state = nullptr;
    std::string why;  // reason for Lifted::unsupported

    // --- types ---------------------------------------------------------------------------
    llvm::IntegerType* ity(unsigned bits) { return B.getIntNTy(bits); }
    llvm::Type* i128() { return B.getInt128Ty(); }
    llvm::Type* ptr() { return llvm::PointerType::getUnqual(C); }

    // --- registers -----------------------------------------------------------------------
    // GPR of any width (AL..R15Q, AH..BH). Returns an iN of the register's width.
    llvm::Value* gpr(ZydisRegister r);
    void set_gpr(ZydisRegister r, llvm::Value* v);
    llvm::Value* gpr64(int index);
    void set_gpr64(int index, llvm::Value* v);
    static int gpr_index(ZydisRegister r);  // 0..15, or -1 when not a GPR
    llvm::Value* xmm(int index);             // i128
    void set_xmm(int index, llvm::Value* v); // i128
    static int xmm_index(ZydisRegister r);   // 0..15, or -1

    // --- flags ---------------------------------------------------------------------------
    llvm::Value* flag(Flag f);
    void set_flag(Flag f, llvm::Value* v);
    void set_szp(llvm::Value* result);  // ZF SF PF of a result of any width
    void set_add_flags(llvm::Value* a, llvm::Value* b, llvm::Value* r, llvm::Value* carry_in = nullptr);
    void set_sub_flags(llvm::Value* a, llvm::Value* b, llvm::Value* r, llvm::Value* borrow_in = nullptr);
    void set_logic_flags(llvm::Value* r);  // CF = OF = 0, AF unchanged (undefined), SZP
    // x86 condition of a Jcc / SETcc / CMOVcc mnemonic (the condition code in the opcode).
    llvm::Value* condition(ZydisMnemonic m);

    // --- operands ------------------------------------------------------------------------
    llvm::Value* address(const DInsn& d, const ZydisDecodedOperand& o);  // i64 effective address
    llvm::Value* host_ptr(llvm::Value* addr64);
    llvm::Value* load(llvm::Value* addr64, unsigned bits);
    void store(llvm::Value* addr64, llvm::Value* v);
    // Operand `i` as an iN of its size (immediates are truncated/sign-extended per Zydis).
    llvm::Value* read(const DInsn& d, int i);
    // Writes `v` (any int width, truncated/extended to the operand's size) to operand `i`.
    void write(const DInsn& d, int i, llvm::Value* v);
    unsigned bits(const DInsn& d, int i) const { return d.ops[i].size; }

    // --- state and hooks ------------------------------------------------------------------
    llvm::Value* state_ptr(std::size_t offset);
    void flush();   // slots -> CpuState (before anything that may read or change the state)
    void reload();  // CpuState -> slots (after it)
    // Calls hook `offset` of `RuntimeHooks` (`offsetof`) with (state, extra args...).
    llvm::CallInst* call_hook(std::size_t offset, llvm::ArrayRef<llvm::Value*> extra,
                              llvm::ArrayRef<llvm::Type*> extra_types, llvm::Type* ret = nullptr);
    void emit_unsupported(std::uint64_t va, const std::string& what);  // hook + unreachable

    // Slots, created in the entry block.
    llvm::AllocaInst* gpr_slot[16] = {};
    llvm::AllocaInst* flag_slot[kFlagCount] = {};
    llvm::AllocaInst* xmm_slot[16] = {};
};

// Instruction families (each tries one decoded instruction).
Lifted lift_int(Ctx& c, const DInsn& d);
Lifted lift_sse(Ctx& c, const DInsn& d);
Lifted lift_string(Ctx& c, const DInsn& d);

}  // namespace wse::recomp::r1
