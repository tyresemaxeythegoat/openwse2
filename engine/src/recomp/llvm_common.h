// Shared LLVM plumbing of the R0 and R1 translators (internal; includes LLVM headers).

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include "recomp_llvm.h"

namespace wse::recomp::llvm_common {

struct TargetSetup {
    std::string triple;
    std::string cpu;       // the CPU code is generated for (the device's own on AArch64)
    std::string features;  // its feature string
    std::unique_ptr<llvm::TargetMachine> tm;
    std::string error;
};

// The process triple's target machine at `opt_level` (0..3); targets are initialized once.
TargetSetup make_target(int opt_level);

// Sets the module's triple/data layout, runs the new pass manager at `opt_level` (skipped at
// 0) and emits an ELF object into `object`.
bool optimize_and_emit(llvm::Module& m, llvm::TargetMachine& tm, int opt_level,
                       std::vector<char>& object, TranslateTimings* timings, std::string* error);

}  // namespace wse::recomp::llvm_common
