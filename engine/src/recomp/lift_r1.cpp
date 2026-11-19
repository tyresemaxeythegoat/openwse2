// R1 lifter core: context helpers, control flow, the translator module. See `lift_r1.h` and
// the conventions in `lift_r1_ctx.h`.

#include "lift_r1.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include "lift_r1_ctx.h"
#include "llvm_common.h"

namespace wse::recomp {

std::string r1_symbol(std::uint64_t va) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "wse_fn_%llx", static_cast<unsigned long long>(va));
    return buf;
}

namespace r1 {
namespace {

constexpr std::size_t kFlagOffset[kFlagCount] = {cpu_offset::cf, cpu_offset::pf, cpu_offset::af,
                                                 cpu_offset::zf, cpu_offset::sf, cpu_offset::of,
                                                 cpu_offset::df};

bool is_high_byte(ZydisRegister r) {
    return r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_CH || r == ZYDIS_REGISTER_DH ||
           r == ZYDIS_REGISTER_BH;
}

unsigned reg_bits(ZydisRegister r) {
    return static_cast<unsigned>(ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, r));
}

}  // namespace

Ctx::Ctx(llvm::Module& m, llvm::Function* f, const DFunction& fn_in, const ImageInfo& image_in)
    : C(m.getContext()), M(m), B(m.getContext()), F(f), fn(fn_in), image(image_in) {
    state = F->getArg(0);  // slots are created by the caller once the entry block exists
}

int Ctx::gpr_index(ZydisRegister r) {
    if (ZydisRegisterGetClass(r) != ZYDIS_REGCLASS_GPR8 && ZydisRegisterGetClass(r) != ZYDIS_REGCLASS_GPR16 &&
        ZydisRegisterGetClass(r) != ZYDIS_REGCLASS_GPR32 && ZydisRegisterGetClass(r) != ZYDIS_REGCLASS_GPR64) {
        return -1;
    }
    const ZydisRegister w = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
    if (w < ZYDIS_REGISTER_RAX || w > ZYDIS_REGISTER_R15) {
        return -1;
    }
    return static_cast<int>(w - ZYDIS_REGISTER_RAX);
}

int Ctx::xmm_index(ZydisRegister r) {
    if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM15) {
        return static_cast<int>(r - ZYDIS_REGISTER_XMM0);
    }
    return -1;
}

llvm::Value* Ctx::gpr64(int index) { return B.CreateLoad(B.getInt64Ty(), gpr_slot[index]); }
void Ctx::set_gpr64(int index, llvm::Value* v) { B.CreateStore(B.CreateZExtOrTrunc(v, B.getInt64Ty()), gpr_slot[index]); }

llvm::Value* Ctx::gpr(ZydisRegister r) {
    const int idx = gpr_index(r);
    llvm::Value* v = gpr64(idx);
    if (is_high_byte(r)) {
        return B.CreateTrunc(B.CreateLShr(v, 8), B.getInt8Ty());
    }
    return B.CreateTrunc(v, ity(reg_bits(r)));
}

void Ctx::set_gpr(ZydisRegister r, llvm::Value* v) {
    const int idx = gpr_index(r);
    const unsigned w = is_high_byte(r) ? 8 : reg_bits(r);
    v = B.CreateZExtOrTrunc(v, ity(w));
    if (w == 64 || w == 32) {
        set_gpr64(idx, B.CreateZExt(v, B.getInt64Ty()));  // 32-bit writes zero the upper half
        return;
    }
    llvm::Value* old = gpr64(idx);
    const unsigned shift = is_high_byte(r) ? 8 : 0;
    const std::uint64_t mask = ((w == 8 ? 0xFFULL : 0xFFFFULL) << shift);
    llvm::Value* merged = B.CreateOr(B.CreateAnd(old, B.getInt64(~mask)),
                                     B.CreateShl(B.CreateZExt(v, B.getInt64Ty()), shift));
    set_gpr64(idx, merged);
}

llvm::Value* Ctx::xmm(int index) { return B.CreateLoad(i128(), xmm_slot[index]); }
void Ctx::set_xmm(int index, llvm::Value* v) {
    if (v->getType()->isVectorTy()) {
        v = B.CreateBitCast(v, i128());
    }
    B.CreateStore(B.CreateZExtOrTrunc(v, i128()), xmm_slot[index]);
}

llvm::Value* Ctx::flag(Flag f) { return B.CreateLoad(B.getInt1Ty(), flag_slot[f]); }
void Ctx::set_flag(Flag f, llvm::Value* v) { B.CreateStore(B.CreateTrunc(v, B.getInt1Ty()), flag_slot[f]); }

void Ctx::set_szp(llvm::Value* r) {
    llvm::Type* t = r->getType();
    set_flag(ZF, B.CreateICmpEQ(r, llvm::ConstantInt::get(t, 0)));
    set_flag(SF, B.CreateICmpSLT(r, llvm::ConstantInt::get(t, 0)));
    llvm::Value* low = B.CreateTrunc(r, B.getInt8Ty());
    llvm::Value* pop = B.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, low);
    set_flag(PF, B.CreateICmpEQ(B.CreateAnd(pop, B.getInt8(1)), B.getInt8(0)));
}

void Ctx::set_add_flags(llvm::Value* a, llvm::Value* b, llvm::Value* r, llvm::Value* cin) {
    const unsigned n = a->getType()->getIntegerBitWidth();
    llvm::Type* wide = ity(n + 1);
    llvm::Value* s = B.CreateAdd(B.CreateZExt(a, wide), B.CreateZExt(b, wide));
    if (cin != nullptr) {
        s = B.CreateAdd(s, B.CreateZExt(cin, wide));
    }
    set_flag(CF, B.CreateTrunc(B.CreateLShr(s, n), B.getInt1Ty()));
    llvm::Value* zero = llvm::ConstantInt::get(a->getType(), 0);
    set_flag(OF, B.CreateICmpSLT(B.CreateAnd(B.CreateXor(a, r), B.CreateXor(b, r)), zero));
    set_flag(AF, B.CreateICmpNE(B.CreateAnd(B.CreateXor(B.CreateXor(a, b), r),
                                            llvm::ConstantInt::get(a->getType(), 0x10)),
                                zero));
    set_szp(r);
}

void Ctx::set_sub_flags(llvm::Value* a, llvm::Value* b, llvm::Value* r, llvm::Value* bin) {
    const unsigned n = a->getType()->getIntegerBitWidth();
    llvm::Type* wide = ity(n + 1);
    llvm::Value* d = B.CreateSub(B.CreateZExt(a, wide), B.CreateZExt(b, wide));
    if (bin != nullptr) {
        d = B.CreateSub(d, B.CreateZExt(bin, wide));
    }
    set_flag(CF, B.CreateTrunc(B.CreateLShr(d, n), B.getInt1Ty()));
    llvm::Value* zero = llvm::ConstantInt::get(a->getType(), 0);
    set_flag(OF, B.CreateICmpSLT(B.CreateAnd(B.CreateXor(a, b), B.CreateXor(a, r)), zero));
    set_flag(AF, B.CreateICmpNE(B.CreateAnd(B.CreateXor(B.CreateXor(a, b), r),
                                            llvm::ConstantInt::get(a->getType(), 0x10)),
                                zero));
    set_szp(r);
}

void Ctx::set_logic_flags(llvm::Value* r) {
    set_flag(CF, B.getFalse());
    set_flag(OF, B.getFalse());
    set_szp(r);
}

llvm::Value* Ctx::condition(ZydisMnemonic m) {
    std::string s = ZydisMnemonicGetString(m);
    for (const char* p : {"cmov", "set", "j"}) {
        if (s.rfind(p, 0) == 0) {
            s = s.substr(std::strlen(p));
            break;
        }
    }
    auto ne = [&](llvm::Value* v) { return B.CreateNot(v); };
    if (s == "o") return flag(OF);
    if (s == "no") return ne(flag(OF));
    if (s == "b") return flag(CF);
    if (s == "nb") return ne(flag(CF));
    if (s == "z") return flag(ZF);
    if (s == "nz") return ne(flag(ZF));
    if (s == "be") return B.CreateOr(flag(CF), flag(ZF));
    if (s == "nbe") return ne(B.CreateOr(flag(CF), flag(ZF)));
    if (s == "s") return flag(SF);
    if (s == "ns") return ne(flag(SF));
    if (s == "p") return flag(PF);
    if (s == "np") return ne(flag(PF));
    if (s == "l") return B.CreateICmpNE(flag(SF), flag(OF));
    if (s == "nl") return B.CreateICmpEQ(flag(SF), flag(OF));
    if (s == "le") return B.CreateOr(flag(ZF), B.CreateICmpNE(flag(SF), flag(OF)));
    if (s == "nle") return B.CreateAnd(ne(flag(ZF)), B.CreateICmpEQ(flag(SF), flag(OF)));
    return nullptr;
}

llvm::Value* Ctx::state_ptr(std::size_t offset) {
    return B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), state, offset);
}

llvm::Value* Ctx::address(const DInsn& d, const ZydisDecodedOperand& o) {
    llvm::Value* a = B.getInt64(o.mem.disp.has_displacement ? static_cast<std::uint64_t>(o.mem.disp.value) : 0);
    if (o.mem.base == ZYDIS_REGISTER_RIP) {
        a = B.CreateAdd(a, B.getInt64(d.next()));
    } else if (o.mem.base != ZYDIS_REGISTER_NONE) {
        a = B.CreateAdd(a, B.CreateZExt(gpr(o.mem.base), B.getInt64Ty()));
    }
    if (o.mem.index != ZYDIS_REGISTER_NONE) {
        a = B.CreateAdd(a, B.CreateMul(B.CreateZExt(gpr(o.mem.index), B.getInt64Ty()), B.getInt64(o.mem.scale)));
    }
    if (d.ins.address_width == 32) {
        a = B.CreateAnd(a, B.getInt64(0xFFFFFFFFULL));
    }
    if (o.mem.segment == ZYDIS_REGISTER_FS || o.mem.segment == ZYDIS_REGISTER_GS) {
        flush();
        llvm::Value* base = call_hook(offsetof(RuntimeHooks, segment_base),
                                      {B.getInt32(o.mem.segment == ZYDIS_REGISTER_FS ? 4 : 5)},
                                      {B.getInt32Ty()}, B.getInt64Ty());
        reload();
        a = B.CreateAdd(a, base);
    }
    return a;
}

llvm::Value* Ctx::host_ptr(llvm::Value* addr64) { return B.CreateIntToPtr(addr64, ptr()); }

llvm::Value* Ctx::load(llvm::Value* addr64, unsigned bits_in) {
    llvm::LoadInst* l = B.CreateLoad(ity(bits_in), host_ptr(addr64));
    l->setAlignment(llvm::Align(1));
    return l;
}

void Ctx::store(llvm::Value* addr64, llvm::Value* v) {
    llvm::StoreInst* s = B.CreateStore(v, host_ptr(addr64));
    s->setAlignment(llvm::Align(1));
}

llvm::Value* Ctx::read(const DInsn& d, int i) {
    const ZydisDecodedOperand& o = d.ops[i];
    switch (o.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER: {
        const int x = xmm_index(o.reg.value);
        if (x >= 0) {
            return B.CreateTrunc(xmm(x), ity(o.size));
        }
        return gpr(o.reg.value);
    }
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return load(address(d, o), o.size);
    case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
        unsigned n = o.size == 0 ? 64 : o.size;
        // A signed immediate narrower than the destination (`mov r64, imm32`, `add r64, imm8`)
        // is sign-extended to it; Zydis already sign-extends `imm.value` to 64 bits.
        if (i > 0 && o.imm.is_signed && d.ops[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE &&
            d.ops[0].size > n) {
            n = d.ops[0].size;
        }
        const std::uint64_t mask = n >= 64 ? ~0ULL : ((1ULL << n) - 1ULL);
        return llvm::ConstantInt::get(ity(n), o.imm.value.u & mask);
    }
    default:
        break;
    }
    return llvm::ConstantInt::get(ity(o.size == 0 ? 64 : o.size), 0);
}

void Ctx::write(const DInsn& d, int i, llvm::Value* v) {
    const ZydisDecodedOperand& o = d.ops[i];
    if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        const int x = xmm_index(o.reg.value);
        if (x >= 0) {
            if (o.size >= 128) {
                set_xmm(x, v);
            } else {
                // Narrow XMM writes merge into the low bits (families decide when to zero).
                const std::uint64_t n = o.size;
                llvm::Value* low = B.CreateZExt(B.CreateZExtOrTrunc(v, ity(o.size)), i128());
                llvm::Value* keep = B.CreateShl(B.CreateLShr(xmm(x), n), n);
                set_xmm(x, B.CreateOr(keep, low));
            }
            return;
        }
        set_gpr(o.reg.value, v);
        return;
    }
    if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
        if (v->getType()->isVectorTy()) {
            v = B.CreateBitCast(v, ity(o.size));
        }
        store(address(d, o), B.CreateZExtOrTrunc(v, ity(o.size)));
    }
}

void Ctx::flush() {
    for (int r = 0; r < 16; ++r) {
        B.CreateStore(B.CreateLoad(B.getInt64Ty(), gpr_slot[r]), state_ptr(cpu_offset::gpr + 8U * r));
        llvm::StoreInst* s = B.CreateStore(B.CreateLoad(i128(), xmm_slot[r]), state_ptr(cpu_offset::xmm + 16U * r));
        s->setAlignment(llvm::Align(16));
    }
    for (int k = 0; k < kFlagCount; ++k) {
        B.CreateStore(B.CreateZExt(B.CreateLoad(B.getInt1Ty(), flag_slot[k]), B.getInt8Ty()),
                      state_ptr(kFlagOffset[k]));
    }
}

void Ctx::reload() {
    for (int r = 0; r < 16; ++r) {
        B.CreateStore(B.CreateLoad(B.getInt64Ty(), state_ptr(cpu_offset::gpr + 8U * r)), gpr_slot[r]);
        llvm::LoadInst* x = B.CreateLoad(i128(), state_ptr(cpu_offset::xmm + 16U * r));
        x->setAlignment(llvm::Align(16));
        B.CreateStore(x, xmm_slot[r]);
    }
    for (int k = 0; k < kFlagCount; ++k) {
        llvm::Value* byte = B.CreateLoad(B.getInt8Ty(), state_ptr(kFlagOffset[k]));
        B.CreateStore(B.CreateICmpNE(byte, B.getInt8(0)), flag_slot[k]);
    }
}

llvm::CallInst* Ctx::call_hook(std::size_t offset, llvm::ArrayRef<llvm::Value*> extra,
                               llvm::ArrayRef<llvm::Type*> extra_types, llvm::Type* ret) {
    llvm::Value* hooks = B.CreateLoad(ptr(), state_ptr(cpu_offset::hooks));
    llvm::Value* slot = B.CreateConstInBoundsGEP1_64(B.getInt8Ty(), hooks, offset);
    llvm::Value* fp = B.CreateLoad(ptr(), slot);
    std::vector<llvm::Type*> params{ptr()};
    params.insert(params.end(), extra_types.begin(), extra_types.end());
    auto* fty = llvm::FunctionType::get(ret != nullptr ? ret : B.getVoidTy(), params, false);
    std::vector<llvm::Value*> args{state};
    args.insert(args.end(), extra.begin(), extra.end());
    return B.CreateCall(fty, fp, args);
}

void Ctx::emit_unsupported(std::uint64_t va, const std::string& what) {
    flush();
    B.CreateStore(B.getInt64(va), state_ptr(cpu_offset::rip));
    llvm::Constant* text = B.CreateGlobalStringPtr(what, "why");
    call_hook(offsetof(RuntimeHooks, unsupported), {B.getInt64(va), text}, {B.getInt64Ty(), ptr()});
    B.CreateRetVoid();  // the hook does not return in practice; returning keeps the IR valid
}

}  // namespace r1

// ---------------------------------------------------------------------------------------------

struct R1Translator::Impl {
    ImageInfo image;
    std::unique_ptr<llvm::LLVMContext> context = std::make_unique<llvm::LLVMContext>();
    std::unique_ptr<llvm::Module> module;
    std::map<std::uint64_t, llvm::Function*> functions;  // declared (some defined)
    std::string init_error;
    LiftReport report;
    double lift_ms = 0.0;
    double total_ms = 0.0;  // lift time of discarded / emitted modules

    void reset() {
        module = std::make_unique<llvm::Module>("wse_r1", *context);
        functions.clear();
        lift_ms = 0.0;
    }

    llvm::Function* function(std::uint64_t va) {
        auto it = functions.find(va);
        if (it != functions.end()) {
            return it->second;
        }
        auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(*context),
                                            {llvm::PointerType::getUnqual(*context)}, false);
        auto* f = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, r1_symbol(va), *module);
        // Exported, but bound inside the module: calls between translated functions are direct (no
        // PLT) and `wse_fn_table` needs only relative relocations (the own ELF loader's subset).
        f->setVisibility(llvm::GlobalValue::ProtectedVisibility);
        f->setDSOLocal(true);
        f->addFnAttr(llvm::Attribute::NoUnwind);
        functions.emplace(va, f);
        return f;
    }
};

R1Translator::R1Translator(const ImageInfo& image) : impl_(std::make_unique<Impl>()) {
    impl_->image = image;
    impl_->reset();
    const llvm_common::TargetSetup t = llvm_common::make_target(2);
    impl_->init_error = t.error;
}

R1Translator::~R1Translator() = default;

const std::string& R1Translator::init_error() const noexcept { return impl_->init_error; }
const LiftReport& R1Translator::report() const noexcept { return impl_->report; }
void R1Translator::discard() {
    impl_->total_ms += impl_->lift_ms;
    impl_->reset();
}
double R1Translator::total_lift_ms() const noexcept { return impl_->total_ms + impl_->lift_ms; }

void R1Translator::declare(const std::vector<std::uint64_t>& entries) {
    for (const std::uint64_t va : entries) {
        (void)impl_->function(va);
    }
}

namespace {

using r1::Ctx;
using r1::Lifted;

// Pushes `ret_va`, runs `emit_call` (which must leave the state flushed and call the target),
// reloads, and reports a callee that did not return to `ret_va`.
template <typename Emit>
void guest_call(Ctx& c, std::uint64_t ret_va, Emit&& emit_call) {
    llvm::Value* rsp = c.B.CreateSub(c.gpr64(4), c.B.getInt64(8));
    c.set_gpr64(4, rsp);
    c.store(rsp, c.B.getInt64(ret_va));
    c.flush();
    emit_call();
    c.reload();
    llvm::Value* rip = c.B.CreateLoad(c.B.getInt64Ty(), c.state_ptr(cpu_offset::rip));
    llvm::BasicBlock* bad = llvm::BasicBlock::Create(c.C, "bad_ret", c.F);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(c.C, "ret_ok", c.F);
    c.B.CreateCondBr(c.B.CreateICmpEQ(rip, c.B.getInt64(ret_va)), ok, bad);
    c.B.SetInsertPoint(bad);
    c.call_hook(offsetof(RuntimeHooks, bad_return), {c.B.getInt64(ret_va)}, {c.B.getInt64Ty()});
    c.B.CreateBr(ok);
    c.B.SetInsertPoint(ok);
}

void call_dispatch(Ctx& c, llvm::Value* target) {
    c.call_hook(offsetof(RuntimeHooks, dispatch), {target}, {c.B.getInt64Ty()});
}

}  // namespace

bool R1Translator::lift(const DFunction& fn, std::string* why) {
    const auto t0 = std::chrono::steady_clock::now();
    ++impl_->report.attempted;
    llvm::Function* f = impl_->function(fn.entry);
    if (!f->empty()) {
        if (why != nullptr) {
            *why = "already lifted";
        }
        return false;
    }
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*impl_->context, "entry", f);
    Ctx c(*impl_->module, f, fn, impl_->image);
    c.B.SetInsertPoint(entry);
    for (int r = 0; r < 16; ++r) {
        c.gpr_slot[r] = c.B.CreateAlloca(c.B.getInt64Ty(), nullptr, "r" + std::to_string(r));
        c.xmm_slot[r] = c.B.CreateAlloca(c.i128(), nullptr, "x" + std::to_string(r));
    }
    for (int k = 0; k < r1::kFlagCount; ++k) {
        c.flag_slot[k] = c.B.CreateAlloca(c.B.getInt1Ty(), nullptr, "f" + std::to_string(k));
    }
    c.reload();

    std::map<std::uint64_t, llvm::BasicBlock*> blocks;
    for (const std::uint64_t b : fn.block_starts) {
        char name[24];
        std::snprintf(name, sizeof(name), "b%llx", static_cast<unsigned long long>(b));
        blocks[b] = llvm::BasicBlock::Create(*impl_->context, name, f);
    }
    c.B.CreateBr(blocks.at(fn.entry));

    std::string first_failure;
    auto fail = [&](std::uint64_t va, const std::string& what) {
        if (first_failure.empty()) {
            first_failure = what;
        }
        c.emit_unsupported(va, what);
    };
    auto tail_call = [&](std::uint64_t target) {
        c.flush();
        if (impl_->functions.count(target) != 0) {
            c.B.CreateCall(impl_->function(target), {c.state});
        } else {
            call_dispatch(c, c.B.getInt64(target));
        }
        c.B.CreateRetVoid();
    };

    for (const std::uint64_t start : fn.block_starts) {
        c.B.SetInsertPoint(blocks.at(start));
        std::uint64_t va = start;
        while (true) {
            const DInsn* dp = fn.at(va);
            if (dp == nullptr) {
                fail(va, "undecoded instruction");
                break;
            }
            const DInsn& d = *dp;
            const auto cat = d.ins.meta.category;
            const auto mn = d.ins.mnemonic;
            bool terminated = false;

            if (cat == ZYDIS_CATEGORY_RET) {
                llvm::Value* rsp = c.gpr64(4);
                llvm::Value* ret = c.load(rsp, 64);
                std::uint64_t extra = d.ins.operand_count_visible > 0 ? d.ops[0].imm.value.u : 0;
                c.set_gpr64(4, c.B.CreateAdd(rsp, c.B.getInt64(8 + extra)));
                c.flush();
                c.B.CreateStore(ret, c.state_ptr(cpu_offset::rip));
                c.B.CreateRetVoid();
                terminated = true;
            } else if (cat == ZYDIS_CATEGORY_COND_BR) {
                ZyanU64 target = 0;
                ZydisCalcAbsoluteAddress(&d.ins, &d.ops[0], d.va, &target);
                llvm::Value* cond = nullptr;
                if (mn == ZYDIS_MNEMONIC_JRCXZ || mn == ZYDIS_MNEMONIC_JECXZ || mn == ZYDIS_MNEMONIC_JCXZ) {
                    const unsigned w = mn == ZYDIS_MNEMONIC_JRCXZ ? 64 : (mn == ZYDIS_MNEMONIC_JECXZ ? 32 : 16);
                    cond = c.B.CreateICmpEQ(c.B.CreateTrunc(c.gpr64(1), c.ity(w)), llvm::ConstantInt::get(c.ity(w), 0));
                } else if (mn == ZYDIS_MNEMONIC_LOOP || mn == ZYDIS_MNEMONIC_LOOPE || mn == ZYDIS_MNEMONIC_LOOPNE) {
                    llvm::Value* rcx = c.B.CreateSub(c.gpr64(1), c.B.getInt64(1));
                    c.set_gpr64(1, rcx);
                    cond = c.B.CreateICmpNE(rcx, c.B.getInt64(0));
                    if (mn == ZYDIS_MNEMONIC_LOOPE) cond = c.B.CreateAnd(cond, c.flag(r1::ZF));
                    if (mn == ZYDIS_MNEMONIC_LOOPNE) cond = c.B.CreateAnd(cond, c.B.CreateNot(c.flag(r1::ZF)));
                } else {
                    cond = c.condition(mn);
                }
                if (cond == nullptr || blocks.count(target) == 0 || blocks.count(d.next()) == 0) {
                    fail(d.va, std::string("conditional branch ") + ZydisMnemonicGetString(mn));
                    break;
                }
                c.B.CreateCondBr(cond, blocks.at(target), blocks.at(d.next()));
                terminated = true;
            } else if (cat == ZYDIS_CATEGORY_UNCOND_BR) {
                if (d.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    ZyanU64 target = 0;
                    ZydisCalcAbsoluteAddress(&d.ins, &d.ops[0], d.va, &target);
                    if (fn.tail_targets.count(target) != 0) {
                        tail_call(target);
                    } else if (blocks.count(target) != 0) {
                        c.B.CreateBr(blocks.at(target));
                    } else {
                        fail(d.va, "jmp to an unknown block");
                        break;
                    }
                } else {
                    const JumpTable* jt = nullptr;
                    for (const JumpTable& t : fn.jump_tables) {
                        if (t.jmp_va == d.va) {
                            jt = &t;
                        }
                    }
                    llvm::Value* target = c.B.CreateZExt(c.read(d, 0), c.B.getInt64Ty());
                    if (jt != nullptr) {
                        llvm::BasicBlock* dflt = llvm::BasicBlock::Create(c.C, "jt_default", f);
                        llvm::SwitchInst* sw = c.B.CreateSwitch(target, dflt);
                        std::set<std::uint64_t> seen;
                        for (const std::uint64_t t : jt->targets) {
                            if (seen.insert(t).second) {
                                sw->addCase(c.B.getInt64(t), blocks.at(t));
                            }
                        }
                        c.B.SetInsertPoint(dflt);
                        c.emit_unsupported(d.va, "jump table target outside the table");
                    } else {
                        // Indirect tail call (or a switch whose table was not found: the
                        // dispatcher aborts with its VA if the target is no function).
                        c.flush();
                        call_dispatch(c, target);
                        c.B.CreateRetVoid();
                    }
                }
                terminated = true;
            } else if (cat == ZYDIS_CATEGORY_CALL) {
                const std::uint64_t ret_va = d.next();
                const ZydisDecodedOperand& o = d.ops[0];
                if (o.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    ZyanU64 target = 0;
                    ZydisCalcAbsoluteAddress(&d.ins, &o, d.va, &target);
                    guest_call(c, ret_va, [&] {
                        if (impl_->functions.count(target) != 0) {
                            c.B.CreateCall(impl_->function(target), {c.state});
                        } else {
                            call_dispatch(c, c.B.getInt64(target));
                        }
                    });
                } else if (o.type == ZYDIS_OPERAND_TYPE_MEMORY && o.mem.base == ZYDIS_REGISTER_RIP &&
                           o.mem.index == ZYDIS_REGISTER_NONE) {
                    const std::uint64_t slot = d.next() + static_cast<std::uint64_t>(o.mem.disp.value);
                    if (slot >= impl_->image.iat_begin && slot < impl_->image.iat_end) {
                        guest_call(c, ret_va, [&] {
                            c.call_hook(offsetof(RuntimeHooks, import_call), {c.B.getInt64(slot)}, {c.B.getInt64Ty()});
                        });
                    } else {
                        llvm::Value* target = c.read(d, 0);
                        guest_call(c, ret_va, [&] { call_dispatch(c, target); });
                    }
                } else {
                    llvm::Value* target = c.B.CreateZExt(c.read(d, 0), c.B.getInt64Ty());
                    guest_call(c, ret_va, [&] { call_dispatch(c, target); });
                }
            } else if (insn_is_trap(d)) {
                // A deliberate stop (MSVC puts `int3` after no-return calls; `int 0x29` is
                // __fastfail): reaching it at run time is a fault with its address, not missing work.
                ++impl_->report.traps;
                c.emit_unsupported(d.va, ZydisMnemonicGetString(mn));
                break;
            } else if (const char* ext = insn_gated_ext(d)) {
                // Only reachable when `cpuid` advertises the feature, which the runtime's fixed
                // CPU profile does not (`Runtime::hook_cpuid`): a run-time fault if it is reached.
                impl_->report.gated[ext]++;
                c.emit_unsupported(d.va, std::string("needs ") + ext + " (not advertised by cpuid)");
                break;
            } else if (mn == ZYDIS_MNEMONIC_CPUID) {
                c.flush();
                c.call_hook(offsetof(RuntimeHooks, cpuid), {}, {});
                c.reload();
            } else {
                Lifted r = r1::lift_int(c, d);
                if (r == Lifted::not_mine) {
                    r = r1::lift_sse(c, d);
                }
                if (r == Lifted::not_mine) {
                    r = r1::lift_string(c, d);
                }
                if (r != Lifted::yes) {
                    fail(d.va, r == Lifted::unsupported && !c.why.empty() ? c.why
                                                                          : std::string(ZydisMnemonicGetString(mn)));
                    break;
                }
            }
            if (terminated) {
                break;
            }
            const std::uint64_t next = d.next();
            if (fn.tail_targets.count(next) != 0) {
                tail_call(next);
                break;
            }
            if (blocks.count(next) != 0) {
                c.B.CreateBr(blocks.at(next));
                break;
            }
            va = next;
        }
    }

    impl_->lift_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string verify;
    llvm::raw_string_ostream os(verify);
    if (llvm::verifyFunction(*f, &os)) {
        f->deleteBody();
        if (why != nullptr) {
            *why = "IR verification failed: " + os.str().substr(0, 300);
        }
        impl_->report.unsupported["(IR verification)"]++;
        return false;
    }
    if (!first_failure.empty()) {
        impl_->report.unsupported[first_failure]++;
        if (why != nullptr) {
            *why = first_failure;
        }
        return false;
    }
    ++impl_->report.lifted;
    return true;
}

bool R1Translator::emit_object(int opt_level, std::vector<char>& object, TranslateTimings* timings,
                               std::string* error) {
    llvm::LLVMContext& ctx = *impl_->context;
    llvm::Module& m = *impl_->module;
    llvm::IRBuilder<> b(ctx);
    // Declared but not defined: forward to the dispatcher (another module, or a missing target).
    // Only defined functions go in the table, or a forwarder would dispatch to itself.
    std::set<std::uint64_t> defined;
    for (auto& [va, f] : impl_->functions) {
        if (!f->empty()) {
            defined.insert(va);
            continue;
        }
        b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "fwd", f));
        llvm::Value* hooks = b.CreateLoad(b.getPtrTy(), b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), f->getArg(0), cpu_offset::hooks));
        llvm::Value* fp = b.CreateLoad(b.getPtrTy(), b.CreateConstInBoundsGEP1_64(b.getInt8Ty(), hooks, offsetof(RuntimeHooks, dispatch)));
        auto* fty = llvm::FunctionType::get(b.getVoidTy(), {b.getPtrTy(), b.getInt64Ty()}, false);
        b.CreateCall(fty, fp, {f->getArg(0), b.getInt64(va)});
        b.CreateRetVoid();
    }
    // Dispatch table: sorted {va, fn} of everything in this module.
    std::vector<llvm::Constant*> rows;
    auto* row_ty = llvm::StructType::get(ctx, {b.getInt64Ty(), b.getPtrTy()});
    for (auto& [va, f] : impl_->functions) {
        if (defined.count(va) == 0) {
            continue;
        }
        rows.push_back(llvm::ConstantStruct::get(row_ty, {b.getInt64(va), f}));
    }
    auto* arr_ty = llvm::ArrayType::get(row_ty, rows.size());
    new llvm::GlobalVariable(m, arr_ty, true, llvm::GlobalValue::ExternalLinkage,
                             llvm::ConstantArray::get(arr_ty, rows), "wse_fn_table");
    new llvm::GlobalVariable(m, b.getInt64Ty(), true, llvm::GlobalValue::ExternalLinkage,
                             b.getInt64(rows.size()), "wse_fn_table_count");

    const llvm_common::TargetSetup t = llvm_common::make_target(opt_level);
    if (!t.tm) {
        if (error != nullptr) {
            *error = t.error;
        }
        return false;
    }
    const double lift_ms = impl_->lift_ms;
    impl_->total_ms += lift_ms;
    const bool ok = llvm_common::optimize_and_emit(m, *t.tm, opt_level, object, timings, error);
    if (timings != nullptr) {
        timings->lift_ms = lift_ms;
    }
    impl_->reset();
    return ok;
}

}  // namespace wse::recomp
