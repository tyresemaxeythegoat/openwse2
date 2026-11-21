// LLVM lifter, optimizer, code generator and embedded LLD for the Phase 5 translator.
// See `recomp_llvm.h`. Flag rules mirror `Interpreter::execute` (`x86_interp.h`) exactly;
// unread flags are removed by LLVM's dead-code elimination.
//
// R0 deviation (documented in `engine/README.md`): guest loads and stores are plain LLVM
// loads and stores. The TSO lowering (`LDAPR`/`STLR`, LSE atomics) of `docs/RECOMP_PLAN.md`
// lands with R3; R0 only runs a single-threaded leaf function.

#include "recomp_llvm.h"
#include "llvm_common.h"

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include <llvm/ADT/StringMap.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <lld/Common/Driver.h>

LLD_HAS_DRIVER(elf)

namespace wse::recomp {

static_assert(offsetof(CpuState, gpr) == 0);
static_assert(offsetof(CpuState, rip) == 128);
static_assert(offsetof(CpuState, cf) == 136);
static_assert(offsetof(CpuState, pf) == 137);
static_assert(offsetof(CpuState, zf) == 138);
static_assert(offsetof(CpuState, sf) == 139);
static_assert(offsetof(CpuState, of) == 140);

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void init_llvm_once() {
    static std::once_flag once;
    std::call_once(once, [] {
        llvm::InitializeAllTargetInfos();
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
    });
}

using llvm_common::TargetSetup;

std::mutex g_target_m;
bool g_target_set = false;
CodeTarget g_target;

// The target this process translates for: the one `set_code_target` chose, else on an AArch64
// device with LSE and RCpc the portable phone target, else the process's own CPU (below).
bool chosen_target(CodeTarget& out) {
    std::lock_guard<std::mutex> lock(g_target_m);
    if (!g_target_set) {
        g_target_set = true;
        g_target = {};
        const std::string process = llvm::sys::getProcessTriple();
        if (process.rfind("aarch64", 0) == 0 && process.find("android") != std::string::npos) {
            llvm::StringMap<bool> host;
            if (llvm::sys::getHostCPUFeatures(host) && host.lookup("lse") && host.lookup("rcpc")) {
                g_target = portable_android_target();
            }
        }
    }
    out = g_target;
    return !g_target.triple.empty();
}

TargetSetup make_target_impl(int opt_level) {
    init_llvm_once();
    TargetSetup t;
    CodeTarget chosen;
    const bool fixed = chosen_target(chosen);
    t.triple = fixed ? chosen.triple : llvm::sys::getProcessTriple();
    std::string err;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(t.triple, err);
    if (target == nullptr) {
        t.error = "no LLVM target for " + t.triple + ": " + err;
        return t;
    }
    llvm::CodeGenOptLevel level = llvm::CodeGenOptLevel::Default;
    if (opt_level <= 0) {
        level = llvm::CodeGenOptLevel::None;
    } else if (opt_level == 1) {
        level = llvm::CodeGenOptLevel::Less;
    } else if (opt_level >= 3) {
        level = llvm::CodeGenOptLevel::Aggressive;
    }
    llvm::TargetOptions options;
    // AArch64 "generic" has FRINT*, so llvm.floor/roundeven stay inline. On an x86-64 host
    // (CI tests) "generic" lacks ROUNDSS and LLVM would call libm, giving the translated module
    // imports; x86-64-v2 (SSE4.1) keeps it import-free.
    // On AArch64 the code is translated on the device that runs it, so it targets that CPU:
    // with RCpc an acquire load is LDAPR instead of the stronger LDAR (R3 memory ordering), and
    // with LSE `lock` instructions become single atomic instructions instead of LL/SC loops.
    const bool x86_host = t.triple.rfind("x86_64", 0) == 0;
    std::string cpu = x86_host ? "x86-64-v2" : "generic";
    std::string features;
    if (fixed) {
        cpu = chosen.cpu;
        features = chosen.features;
    } else if (t.triple.rfind("aarch64", 0) == 0) {
        cpu = llvm::sys::getHostCPUName().str();
        llvm::StringMap<bool> host;
        if (llvm::sys::getHostCPUFeatures(host)) {
            for (const auto& f : host) {
                features += (features.empty() ? "" : ",");
                features += (f.getValue() ? "+" : "-");
                features += f.getKey().str();
            }
        }
    }
    t.cpu = cpu;
    t.features = features;
    t.tm.reset(target->createTargetMachine(t.triple, cpu, features, options, llvm::Reloc::PIC_,
                                           llvm::CodeModel::Small, level));
    if (!t.tm) {
        t.error = "createTargetMachine failed for " + t.triple;
    }
    return t;
}

// Emits one guest function.
class FunctionLifter {
public:
    FunctionLifter(llvm::Module& m, const DecodedFunction& fn) : m_(m), c_(m.getContext()), b_(c_), fn_(fn) {}

    bool lift(const std::string& symbol, std::string* error) {
        llvm::Type* ptr = llvm::PointerType::getUnqual(c_);
        auto* fty = llvm::FunctionType::get(llvm::Type::getVoidTy(c_), {ptr}, false);
        f_ = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, symbol, m_);
        f_->addFnAttr(llvm::Attribute::NoUnwind);
        state_ = f_->getArg(0);

        llvm::BasicBlock* entry = llvm::BasicBlock::Create(c_, "entry", f_);
        b_.SetInsertPoint(entry);
        for (int r = 0; r < 16; ++r) {
            regs_[r] = b_.CreateAlloca(i64(), nullptr, "r" + std::to_string(r));
        }
        for (int k = 0; k < 5; ++k) {
            flags_[k] = b_.CreateAlloca(i1(), nullptr, "f" + std::to_string(k));
        }
        for (int r = 0; r < 16; ++r) {
            b_.CreateStore(b_.CreateLoad(i64(), state_ptr(8 * r)), regs_[r]);
        }
        for (int k = 0; k < 5; ++k) {
            llvm::Value* byte = b_.CreateLoad(b_.getInt8Ty(), state_ptr(136 + k));
            b_.CreateStore(b_.CreateICmpNE(byte, b_.getInt8(0)), flags_[k]);
        }
        for (const std::uint64_t va : fn_.block_starts) {
            blocks_[va] = llvm::BasicBlock::Create(c_, "b" + hex(va), f_);
        }
        b_.CreateBr(blocks_.at(fn_.start));

        for (std::size_t bi = 0; bi < fn_.block_starts.size(); ++bi) {
            const std::uint64_t start = fn_.block_starts[bi];
            b_.SetInsertPoint(blocks_.at(start));
            const Insn* insn = fn_.at(start);
            while (insn != nullptr) {
                if (!emit(*insn, error)) {
                    return false;
                }
                if (insn->ends_block()) {
                    break;
                }
                const std::uint64_t next = insn->next_va();
                if (blocks_.count(next) != 0) {
                    b_.CreateBr(blocks_.at(next));
                    break;
                }
                insn = fn_.at(next);
                if (insn == nullptr) {
                    if (error != nullptr) {
                        *error = "fall-through into undecoded bytes at 0x" + hex(next);
                    }
                    return false;
                }
            }
        }
        std::string verify;
        llvm::raw_string_ostream os(verify);
        if (llvm::verifyFunction(*f_, &os)) {
            if (error != nullptr) {
                *error = "IR verification failed: " + os.str();
            }
            return false;
        }
        return true;
    }

private:
    enum Flag { CF = 0, PF = 1, ZF = 2, SF = 3, OF = 4 };

    static std::string hex(std::uint64_t v) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
        return buf;
    }
    llvm::Type* i64() { return b_.getInt64Ty(); }
    llvm::Type* i1() { return b_.getInt1Ty(); }
    llvm::IntegerType* ity(std::uint8_t size) { return b_.getIntNTy(8U * size); }

    llvm::Value* state_ptr(int offset) {
        return b_.CreateConstInBoundsGEP1_64(b_.getInt8Ty(), state_, static_cast<std::uint64_t>(offset));
    }
    llvm::Value* reg64(Reg r) { return b_.CreateLoad(i64(), regs_[static_cast<int>(r)]); }
    void set_reg64(Reg r, llvm::Value* v) { b_.CreateStore(v, regs_[static_cast<int>(r)]); }
    llvm::Value* flag(Flag f) { return b_.CreateLoad(i1(), flags_[f]); }
    void set_flag(Flag f, llvm::Value* v) { b_.CreateStore(v, flags_[f]); }

    llvm::Value* address(const Insn& i, const Operand& m) {
        llvm::Value* a = b_.getInt64(static_cast<std::uint64_t>(static_cast<std::int64_t>(m.disp)));
        if (m.base == Reg::rip) {
            a = b_.CreateAdd(a, b_.getInt64(i.next_va()));
        } else if (m.base != Reg::none) {
            a = b_.CreateAdd(a, reg64(m.base));
        }
        if (m.index != Reg::none) {
            a = b_.CreateAdd(a, b_.CreateMul(reg64(m.index), b_.getInt64(m.scale)));
        }
        return a;
    }
    llvm::Value* host_ptr(llvm::Value* addr) {
        return b_.CreateIntToPtr(addr, llvm::PointerType::getUnqual(c_));
    }

    llvm::Value* read(const Insn& i, const Operand& o) {
        switch (o.kind) {
        case Operand::Kind::Reg:
            return b_.CreateTrunc(reg64(o.reg), ity(o.size));
        case Operand::Kind::Mem: {
            llvm::LoadInst* ld = b_.CreateLoad(ity(o.size), host_ptr(address(i, o)));
            ld->setAlignment(llvm::Align(1));
            return ld;
        }
        case Operand::Kind::Imm: {
            const std::uint64_t mask = o.size >= 8 ? ~0ULL : ((1ULL << (8U * o.size)) - 1ULL);
            return llvm::ConstantInt::get(ity(o.size), static_cast<std::uint64_t>(o.imm) & mask);
        }
        case Operand::Kind::None:
            break;
        }
        return llvm::ConstantInt::get(ity(o.size == 0 ? 8 : o.size), 0);
    }

    void write(const Insn& i, const Operand& o, llvm::Value* v) {
        if (o.kind == Operand::Kind::Reg) {
            if (o.size >= 4) {
                set_reg64(o.reg, b_.CreateZExt(v, i64()));
            } else {
                const std::uint64_t mask = (1ULL << (8U * o.size)) - 1ULL;
                llvm::Value* old = b_.CreateAnd(reg64(o.reg), b_.getInt64(~mask));
                set_reg64(o.reg, b_.CreateOr(old, b_.CreateZExt(v, i64())));
            }
        } else if (o.kind == Operand::Kind::Mem) {
            llvm::StoreInst* st = b_.CreateStore(v, host_ptr(address(i, o)));
            st->setAlignment(llvm::Align(1));
        }
    }

    void set_szp(llvm::Value* r) {
        set_flag(ZF, b_.CreateICmpEQ(r, llvm::ConstantInt::get(r->getType(), 0)));
        set_flag(SF, b_.CreateICmpSLT(r, llvm::ConstantInt::get(r->getType(), 0)));
        llvm::Value* low = b_.CreateTrunc(r, b_.getInt8Ty());
        llvm::Value* pop = b_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, low);
        set_flag(PF, b_.CreateICmpEQ(b_.CreateAnd(pop, b_.getInt8(1)), b_.getInt8(0)));
    }

    llvm::Value* cond(Cond c) {
        switch (c) {
        case Cond::O: return flag(OF);
        case Cond::NO: return b_.CreateNot(flag(OF));
        case Cond::B: return flag(CF);
        case Cond::AE: return b_.CreateNot(flag(CF));
        case Cond::E: return flag(ZF);
        case Cond::NE: return b_.CreateNot(flag(ZF));
        case Cond::BE: return b_.CreateOr(flag(CF), flag(ZF));
        case Cond::A: return b_.CreateNot(b_.CreateOr(flag(CF), flag(ZF)));
        case Cond::S: return flag(SF);
        case Cond::NS: return b_.CreateNot(flag(SF));
        case Cond::P: return flag(PF);
        case Cond::NP: return b_.CreateNot(flag(PF));
        case Cond::L: return b_.CreateICmpNE(flag(SF), flag(OF));
        case Cond::GE: return b_.CreateICmpEQ(flag(SF), flag(OF));
        case Cond::LE: return b_.CreateOr(flag(ZF), b_.CreateICmpNE(flag(SF), flag(OF)));
        case Cond::G: return b_.CreateAnd(b_.CreateNot(flag(ZF)), b_.CreateICmpEQ(flag(SF), flag(OF)));
        }
        return b_.getFalse();
    }

    void write_back_and_return(llvm::Value* rip) {
        b_.CreateStore(rip, state_ptr(128));
        for (int r = 0; r < 16; ++r) {
            b_.CreateStore(b_.CreateLoad(i64(), regs_[r]), state_ptr(8 * r));
        }
        for (int k = 0; k < 5; ++k) {
            b_.CreateStore(b_.CreateZExt(b_.CreateLoad(i1(), flags_[k]), b_.getInt8Ty()),
                           state_ptr(136 + k));
        }
        b_.CreateRetVoid();
    }

    bool emit(const Insn& i, std::string* error) {
        const std::uint8_t sz = i.dst.size;
        switch (i.op) {
        case Op::Nop:
            return true;
        case Op::Mov:
            write(i, i.dst, read(i, i.src));
            return true;
        case Op::Movzx:
            write(i, i.dst, b_.CreateZExt(read(i, i.src), ity(sz)));
            return true;
        case Op::Movsx:
            write(i, i.dst, b_.CreateSExt(read(i, i.src), ity(sz)));
            return true;
        case Op::Lea:
            write(i, i.dst, b_.CreateTrunc(address(i, i.src), ity(sz)));
            return true;
        case Op::Push: {
            llvm::Value* v = read(i, i.dst);
            llvm::Value* rsp = b_.CreateSub(reg64(Reg::rsp), b_.getInt64(8));
            set_reg64(Reg::rsp, rsp);
            b_.CreateStore(v, host_ptr(rsp))->setAlignment(llvm::Align(1));
            return true;
        }
        case Op::Pop: {
            llvm::Value* rsp = reg64(Reg::rsp);
            llvm::LoadInst* v = b_.CreateLoad(i64(), host_ptr(rsp));
            v->setAlignment(llvm::Align(1));
            set_reg64(Reg::rsp, b_.CreateAdd(rsp, b_.getInt64(8)));
            write(i, i.dst, v);
            return true;
        }
        case Op::Ret: {
            llvm::Value* rsp = reg64(Reg::rsp);
            llvm::LoadInst* ra = b_.CreateLoad(i64(), host_ptr(rsp));
            ra->setAlignment(llvm::Align(1));
            set_reg64(Reg::rsp, b_.CreateAdd(rsp, b_.getInt64(8)));
            write_back_and_return(ra);
            return true;
        }
        case Op::Jmp:
            b_.CreateBr(blocks_.at(i.target));
            return true;
        case Op::Jcc: {
            const auto t = blocks_.find(i.target);
            const auto f = blocks_.find(i.next_va());
            if (t == blocks_.end() || f == blocks_.end()) {
                if (error != nullptr) {
                    *error = "branch at 0x" + hex(i.va) + " has no block";
                }
                return false;
            }
            b_.CreateCondBr(cond(i.cond), t->second, f->second);
            return true;
        }
        case Op::Inc:
        case Op::Dec: {
            llvm::Value* a = read(i, i.dst);
            llvm::Value* one = llvm::ConstantInt::get(a->getType(), 1);
            llvm::Value* r = (i.op == Op::Inc) ? b_.CreateAdd(a, one) : b_.CreateSub(a, one);
            llvm::Value* smin = llvm::ConstantInt::get(a->getType(), llvm::APInt::getSignedMinValue(8U * sz));
            set_flag(OF, b_.CreateICmpEQ(i.op == Op::Inc ? r : a, smin));
            set_szp(r);
            write(i, i.dst, r);
            return true;
        }
        case Op::Add:
        case Op::Sub:
        case Op::Cmp:
        case Op::And:
        case Op::Or:
        case Op::Xor:
        case Op::Test: {
            llvm::Value* a = read(i, i.dst);
            llvm::Value* bv = read(i, i.src);
            llvm::Value* zero = llvm::ConstantInt::get(a->getType(), 0);
            llvm::Value* r = nullptr;
            switch (i.op) {
            case Op::Add:
                r = b_.CreateAdd(a, bv);
                set_flag(CF, b_.CreateICmpULT(r, a));
                set_flag(OF, b_.CreateICmpSLT(b_.CreateAnd(b_.CreateNot(b_.CreateXor(a, bv)),
                                                           b_.CreateXor(a, r)), zero));
                break;
            case Op::Sub:
            case Op::Cmp:
                r = b_.CreateSub(a, bv);
                set_flag(CF, b_.CreateICmpULT(a, bv));
                set_flag(OF, b_.CreateICmpSLT(b_.CreateAnd(b_.CreateXor(a, bv), b_.CreateXor(a, r)),
                                              zero));
                break;
            case Op::And:
            case Op::Test:
                r = b_.CreateAnd(a, bv);
                break;
            case Op::Or:
                r = b_.CreateOr(a, bv);
                break;
            default:
                r = b_.CreateXor(a, bv);
                break;
            }
            if (i.op != Op::Add && i.op != Op::Sub && i.op != Op::Cmp) {
                set_flag(CF, b_.getFalse());
                set_flag(OF, b_.getFalse());
            }
            set_szp(r);
            if (i.op != Op::Cmp && i.op != Op::Test) {
                write(i, i.dst, r);
            }
            return true;
        }
        }
        if (error != nullptr) {
            *error = "unhandled op at 0x" + hex(i.va);
        }
        return false;
    }

    llvm::Module& m_;
    llvm::LLVMContext& c_;
    llvm::IRBuilder<> b_;
    const DecodedFunction& fn_;
    llvm::Function* f_ = nullptr;
    llvm::Value* state_ = nullptr;
    llvm::AllocaInst* regs_[16] = {};
    llvm::AllocaInst* flags_[5] = {};
    std::map<std::uint64_t, llvm::BasicBlock*> blocks_;
};

bool optimize_and_emit_impl(llvm::Module& m, llvm::TargetMachine& tm, int opt_level,
                       std::vector<char>& object, TranslateTimings* timings, std::string* error) {
    m.setTargetTriple(tm.getTargetTriple().str());
    m.setDataLayout(tm.createDataLayout());
    auto t0 = std::chrono::steady_clock::now();
    // Debugging aid: `WSE_R1_DUMP_FN=<symbol>` prints that function's IR before and after the
    // optimizer to stderr.
    const char* dump = std::getenv("WSE_R1_DUMP_FN");
    llvm::Function* dump_fn = dump != nullptr ? m.getFunction(dump) : nullptr;
    if (dump_fn != nullptr) {
        llvm::errs() << "=== " << dump << " before optimization\n";
        dump_fn->print(llvm::errs());
    }
    if (opt_level > 0) {
        llvm::LoopAnalysisManager lam;
        llvm::FunctionAnalysisManager fam;
        llvm::CGSCCAnalysisManager cgam;
        llvm::ModuleAnalysisManager mam;
        llvm::PassBuilder pb(&tm);
        pb.registerModuleAnalyses(mam);
        pb.registerCGSCCAnalyses(cgam);
        pb.registerFunctionAnalyses(fam);
        pb.registerLoopAnalyses(lam);
        pb.crossRegisterProxies(lam, fam, cgam, mam);
        llvm::OptimizationLevel level = llvm::OptimizationLevel::O2;
        if (opt_level == 1) {
            level = llvm::OptimizationLevel::O1;
        } else if (opt_level >= 3) {
            level = llvm::OptimizationLevel::O3;
        }
        llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(level);
        mpm.run(m, mam);
    }
    if (dump_fn != nullptr) {
        llvm::errs() << "=== " << dump << " after optimization\n";
        dump_fn->print(llvm::errs());
    }
    if (timings != nullptr) {
        timings->optimize_ms = ms_since(t0);
    }
    t0 = std::chrono::steady_clock::now();
    llvm::SmallVector<char, 0> buffer;
    llvm::raw_svector_ostream os(buffer);
    llvm::legacy::PassManager cg;
    if (tm.addPassesToEmitFile(cg, os, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        if (error != nullptr) {
            *error = "the target cannot emit object files";
        }
        return false;
    }
    cg.run(m);
    object.assign(buffer.begin(), buffer.end());
    if (timings != nullptr) {
        timings->codegen_ms = ms_since(t0);
        timings->object_bytes = object.size();
    }
    return true;
}

}  // namespace

namespace llvm_common {

TargetSetup make_target(int opt_level) { return make_target_impl(opt_level); }

bool optimize_and_emit(llvm::Module& m, llvm::TargetMachine& tm, int opt_level,
                       std::vector<char>& object, TranslateTimings* timings, std::string* error) {
    return optimize_and_emit_impl(m, tm, opt_level, object, timings, error);
}

}  // namespace llvm_common

struct Translator::Impl {
    std::unique_ptr<llvm::LLVMContext> context = std::make_unique<llvm::LLVMContext>();
    std::unique_ptr<llvm::Module> module;
    std::string triple;
    std::string init_error;
    std::size_t count = 0;
    double lift_ms = 0.0;

    void reset_module() {
        module = std::make_unique<llvm::Module>("wse_recomp", *context);
        count = 0;
        lift_ms = 0.0;
    }
};

Translator::Translator() : impl_(std::make_unique<Impl>()) {
    const TargetSetup t = make_target_impl(2);
    impl_->triple = t.triple;
    impl_->init_error = t.error;
    impl_->reset_module();
}

Translator::~Translator() = default;

const std::string& Translator::triple() const noexcept { return impl_->triple; }
const std::string& Translator::init_error() const noexcept { return impl_->init_error; }
std::size_t Translator::function_count() const noexcept { return impl_->count; }

bool Translator::add_function(const DecodedFunction& fn, const std::string& symbol,
                              std::string* error) {
    if (!fn.error.empty()) {
        if (error != nullptr) {
            *error = fn.error;
        }
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    FunctionLifter lifter(*impl_->module, fn);
    const bool ok = lifter.lift(symbol, error);
    impl_->lift_ms += ms_since(t0);
    if (ok) {
        ++impl_->count;
    } else if (llvm::Function* f = impl_->module->getFunction(symbol)) {
        f->eraseFromParent();
    }
    return ok;
}

bool Translator::emit_object(int opt_level, std::vector<char>& object, TranslateTimings* timings,
                             std::string* error) {
    const TargetSetup t = make_target_impl(opt_level);
    if (!t.tm) {
        if (error != nullptr) {
            *error = t.error;
        }
        return false;
    }
    if (timings != nullptr) {
        *timings = TranslateTimings{};
        timings->lift_ms = impl_->lift_ms;
    }
    const bool ok = optimize_and_emit_impl(*impl_->module, *t.tm, opt_level, object, timings, error);
    impl_->reset_module();
    return ok;
}

bool emit_probe_object(const std::string& symbol, std::vector<char>& object, std::string* error) {
    const TargetSetup t = make_target_impl(2);
    if (!t.tm) {
        if (error != nullptr) {
            *error = t.error;
        }
        return false;
    }
    llvm::LLVMContext ctx;
    llvm::Module m("wse_r0_probe", ctx);
    llvm::IRBuilder<> b(ctx);
    llvm::Type* i32 = b.getInt32Ty();
    auto* fty = llvm::FunctionType::get(i32, {i32, i32, i32}, false);
    auto* f = llvm::Function::Create(fty, llvm::Function::ExternalLinkage, symbol, m);
    b.SetInsertPoint(llvm::BasicBlock::Create(ctx, "entry", f));
    b.CreateRet(b.CreateAdd(b.CreateMul(f->getArg(0), f->getArg(1)), f->getArg(2)));
    return optimize_and_emit_impl(m, *t.tm, 2, object, nullptr, error);
}

CodeTarget portable_android_target() {
    return {"aarch64-unknown-linux-android", "generic", "+v8.2a,+rcpc"};
}

void set_code_target(const CodeTarget& target) {
    std::lock_guard<std::mutex> lock(g_target_m);
    g_target_set = true;
    g_target = target;
}

std::string translator_fingerprint() {
    CodeTarget t;
    if (!chosen_target(t)) {
        // This process's own CPU: the same choice make_target_impl makes.
        t.triple = llvm::sys::getProcessTriple();
        t.cpu = t.triple.rfind("aarch64", 0) == 0 ? llvm::sys::getHostCPUName().str()
                                                    : (t.triple.rfind("x86_64", 0) == 0 ? "x86-64-v2" : "generic");
        t.features = "host";
    }
#ifndef WSE_LIFTER_SOURCES_HASH
#define WSE_LIFTER_SOURCES_HASH "unknown"
#endif
    return std::string("lifter ") + WSE_LIFTER_SOURCES_HASH + " llvm " + LLVM_VERSION_STRING + " target " + t.triple +
           " " + t.cpu + " " + t.features;
}

bool link_shared_object(const std::vector<std::string>& objects, const std::string& output,
                        std::string* log) {
    static std::mutex lld_mutex;  // LLD keeps global state while it runs
    std::lock_guard<std::mutex> lock(lld_mutex);
    // `-z defs`: a translated module must import nothing (every call out goes through
    // `RuntimeHooks`), so any undefined symbol, such as a libm or libatomic call LLVM chose
    // to emit, fails the link instead of the load.
    std::vector<const char*> args = {"ld.lld", "-shared", "--hash-style=gnu", "-z", "noexecstack",
                                     "-z", "now", "-z", "defs", "-o", output.c_str()};
    for (const std::string& o : objects) {
        args.push_back(o.c_str());
    }
    std::string out_text;
    std::string err_text;
    llvm::raw_string_ostream out_os(out_text);
    llvm::raw_string_ostream err_os(err_text);
    const lld::Result r = lld::lldMain(args, out_os, err_os, {{lld::Gnu, &lld::elf::link}});
    out_os.flush();
    err_os.flush();
    if (log != nullptr) {
        *log = out_text + err_text;
        if (!r.canRunAgain) {
            *log += " (lld: cannot run again in this process)";
        }
    }
    return r.retCode == 0;
}

}  // namespace wse::recomp
