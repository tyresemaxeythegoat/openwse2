// Phase 5 translator: decoded x86-64 functions -> LLVM IR -> native object -> shared object.
// Built only with `WSE_ENABLE_LLVM=ON` (`docs/RECOMP_PLAN.md`, R0 spikes). Internal header of
// the `wse_recomp` library; LLVM types stay inside `lifter_llvm.cpp`.
//
// Translated functions have the signature `void fn(wse::recomp::CpuState*)`: they read the
// guest registers and flags from the state, run, and write them back at `ret` (with `rip`
// set to the popped return address).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "wse/recomp/x86_interp.h"

namespace wse::recomp {

struct TranslateTimings {
    double lift_ms = 0.0;
    double optimize_ms = 0.0;
    double codegen_ms = 0.0;
    std::size_t object_bytes = 0;
};

// One translation unit: add functions, then emit one object file for all of them.
class Translator {
public:
    Translator();
    ~Translator();
    Translator(const Translator&) = delete;
    Translator& operator=(const Translator&) = delete;

    // Target triple of the code being produced (the process triple).
    [[nodiscard]] const std::string& triple() const noexcept;
    // Empty when the LLVM target could not be created.
    [[nodiscard]] const std::string& init_error() const noexcept;

    // Lifts `fn` into the current module as the external function `symbol`.
    bool add_function(const DecodedFunction& fn, const std::string& symbol, std::string* error);
    [[nodiscard]] std::size_t function_count() const noexcept;

    // Optimizes (`opt_level` 0..3) and emits an ELF object; the module is reset afterwards.
    bool emit_object(int opt_level, std::vector<char>& object, TranslateTimings* timings,
                     std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Builds a minimal `int32_t symbol(int32_t a, int32_t b, int32_t c)` returning `a * b + c` and
// emits it as an object: the R0 (a) "generated in the app" shared object needs no guest code.
bool emit_probe_object(const std::string& symbol, std::vector<char>& object, std::string* error);

// Links ELF objects into a shared object with the embedded LLD (`ld.lld -shared`).
bool link_shared_object(const std::vector<std::string>& objects, const std::string& output,
                        std::string* log);

}  // namespace wse::recomp
