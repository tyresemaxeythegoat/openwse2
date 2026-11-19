// R1 lifter: Zydis-decoded x86-64 functions -> LLVM IR (`docs/RECOMP_PLAN.md` "R1 design").
//
// Public API used by the spike runner, the coverage meter and the tests. LLVM stays inside the
// `.cpp` files; the family files share `lift_r1_ctx.h`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "discover.h"
#include "recomp_llvm.h"

namespace wse::recomp {

struct ImageInfo {
    std::uint64_t base = 0;
    std::uint64_t iat_begin = 0;  // import address table range (calls through it are imports)
    std::uint64_t iat_end = 0;
    // Functions that can catch (a `.pdata` exception or termination handler): they get a resume
    // switch so an unwind can continue them at any of their blocks (`docs/RECOMP_PLAN.md` "R3").
    std::set<std::uint64_t> resumable;
};

struct LiftReport {
    std::size_t attempted = 0;
    std::size_t lifted = 0;
    // mnemonic (or reason) -> number of functions that failed on it first
    std::map<std::string, std::size_t> unsupported;
    // Instructions lifted as a run-time fault rather than failures: traps (`int3` after a
    // no-return call, `int 0x29` fast-fail, `ud2`, `hlt`) and code the fixed CPU profile does
    // not advertise (AVX/FMA/SSE4.2, behind a `cpuid` check), by reason.
    std::size_t traps = 0;
    std::map<std::string, std::size_t> gated;
};

// One module of translated functions. Each function `F` at VA `v` becomes the external symbol
// `wse_fn_<v hex>`; the module also exports `wse_fn_table` (sorted {va, fn} pairs) and
// `wse_fn_table_count` for the runtime dispatcher.
class R1Translator {
public:
    explicit R1Translator(const ImageInfo& image);
    ~R1Translator();
    R1Translator(const R1Translator&) = delete;
    R1Translator& operator=(const R1Translator&) = delete;

    [[nodiscard]] const std::string& init_error() const noexcept;

    // Declares every function that will be in this module (calls between them become direct).
    void declare(const std::vector<std::uint64_t>& entries);
    // Lifts one discovered function; on failure the function body becomes a call to the
    // `unsupported` hook at the first instruction that could not be lifted, and `why` names it.
    bool lift(const DFunction& fn, std::string* why);

    // Emits the dispatch table, optimizes and emits an object (then resets the module).
    bool emit_object(int opt_level, std::vector<char>& object, TranslateTimings* timings,
                     std::string* error);

    [[nodiscard]] const LiftReport& report() const noexcept;
    // Drops the module lifted so far without emitting it (the coverage meter lifts in batches).
    void discard();
    // Time spent in `lift()` since construction (all batches).
    [[nodiscard]] double total_lift_ms() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string r1_symbol(std::uint64_t va);

}  // namespace wse::recomp
