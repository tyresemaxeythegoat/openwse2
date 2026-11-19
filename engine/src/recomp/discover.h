// Function discovery for the R1 lifter (`docs/RECOMP_PLAN.md` "R1 design"): Zydis decoding,
// entry collection and control-flow following, MSVC jump tables, tail calls.
//
// Guest memory is the mapped image (`image` holds `size_of_image` bytes for VA `base`). Nothing
// here needs LLVM, so it builds and runs on the PC against the user's exe.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <Zydis.h>

#include "wse/recomp/pdb_reader.h"
#include "wse/recomp/pe_image.h"

namespace wse::recomp {

struct DInsn {
    std::uint64_t va = 0;
    ZydisDecodedInstruction ins{};
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
    [[nodiscard]] std::uint64_t next() const noexcept { return va + ins.length; }
};

struct JumpTable {
    std::uint64_t jmp_va = 0;                 // the indirect `jmp`
    std::vector<std::uint64_t> targets;       // in table order (duplicates kept)
    bool by_function_range = false;           // bounded by the .pdata function, not a compare (R4)
};

struct DFunction {
    std::uint64_t entry = 0;
    std::map<std::uint64_t, DInsn> insns;     // every reachable instruction
    std::set<std::uint64_t> block_starts;
    std::set<std::uint64_t> call_targets;     // direct `call imm`
    std::set<std::uint64_t> tail_targets;     // `jmp imm` to another function entry
    std::vector<JumpTable> jump_tables;
    std::vector<std::uint64_t> unresolved_indirect_jumps;  // switch-shaped, table not found
    std::vector<std::uint64_t> indirect_tail_jumps;        // dispatched at run time
    std::vector<std::uint64_t> noreturn_calls; // calls followed by undecodable bytes
    std::string error;                         // empty when the body decoded completely

    [[nodiscard]] const DInsn* at(std::uint64_t va) const {
        const auto it = insns.find(va);
        return it == insns.end() ? nullptr : &it->second;
    }
};

class ImageView {
public:
    ImageView(const std::uint8_t* image, std::uint64_t base, std::size_t size,
              std::uint64_t text_begin, std::uint64_t text_end)
        : image_(image), base_(base), size_(size), text_begin_(text_begin), text_end_(text_end) {}

    [[nodiscard]] bool in_text(std::uint64_t va) const noexcept {
        return va >= text_begin_ && va < text_end_;
    }
    [[nodiscard]] bool readable(std::uint64_t va, std::size_t n) const noexcept {
        return va >= base_ && va - base_ <= size_ && n <= size_ - (va - base_);
    }
    [[nodiscard]] const std::uint8_t* ptr(std::uint64_t va) const noexcept { return image_ + (va - base_); }
    [[nodiscard]] std::uint64_t base() const noexcept { return base_; }
    bool read32(std::uint64_t va, std::uint32_t& out) const noexcept;
    bool read8(std::uint64_t va, std::uint8_t& out) const noexcept;

private:
    const std::uint8_t* image_;
    std::uint64_t base_;
    std::size_t size_;
    std::uint64_t text_begin_;
    std::uint64_t text_end_;
};

class Discoverer {
public:
    explicit Discoverer(const ImageView& view);

    bool decode(std::uint64_t va, DInsn& out) const;

    // Follows control flow from `entry`. `entries` is the set of known function entries: a
    // direct `jmp` to one of them (other than `entry`) is a tail call. `extra_starts` (R3) are
    // more blocks of this function that no branch reaches, such as catch continuations
    // (`find_eh_continuations`); they are followed like branch targets.
    DFunction discover(std::uint64_t entry, const std::set<std::uint64_t>& entries,
                       const std::set<std::uint64_t>* extra_starts = nullptr) const;

    // R4: the image's `.pdata` ranges (sorted by begin). With them, an MSVC jump table that no
    // `cmp`/`ja` or mask bounds (the CRT's `memset`/`memcpy` index by `size >> 4`) is read while
    // its entries land inside the `.pdata` function holding the `jmp` (`JumpTable::by_function_range`).
    // Off by default: the R1 coverage meter and the R2 selection keep their recorded rule.
    void set_function_ranges(const std::vector<PeFunctionRange>* ranges) { ranges_ = ranges; }

private:
    bool resolve_jump_table(const DFunction& fn, const DInsn& jmp, JumpTable& out) const;

    const ImageView& view_;
    ZydisDecoder decoder_{};
    const std::vector<PeFunctionRange>* ranges_ = nullptr;
};

// Short Intel-syntax text of an instruction (diagnostics).
std::string format_insn(const DInsn& d);

// A deliberate stop: `int3` (MSVC puts it after no-return calls), `int n` (`int 0x29` is
// __fastfail), `ud0`-`ud2`, `hlt`.
bool insn_is_trap(const DInsn& d);
// The extension an instruction needs when the runtime's fixed CPU profile does not advertise it
// (`Runtime::hook_cpuid`): VEX/EVEX/XOP/3DNow! encodings, SSE4.2, SSE4a, `xgetbv`; else null.
const char* insn_gated_ext(const DInsn& d);

// Every function entry of an image: non-chained `.pdata` begins, PDB functions in `.text`, then
// direct call targets to a fixed point (each function is discovered once and dropped, so the
// decoded image is never held in memory). `image` is the image mapped at `img.image_base`.
std::set<std::uint64_t> collect_function_entries(const ImageView& view, const PeImage& img,
                                                 const std::uint8_t* image,
                                                 const std::vector<PdbPublic>& publics);

// R2 (`docs/RECOMP_PLAN.md` "R2 design"): a function the differential rig may call on its own
// with generated inputs. Its body decoded completely, and it has no call, tail call, indirect jump
// other than a resolved jump table, `fs:`/`gs:` operand, write through a `rip`-relative or
// absolute address, trap, `cpuid`/`rdtsc`, x87, system or cpuid-gated instruction. The PC recorder
// and the phone use this one rule, so both pick the same set.
bool is_self_contained(const DFunction& f);

// R3 (`docs/RECOMP_PLAN.md` "R3 design"): the catch continuations of an image. MSVC C++ `catch`
// blocks are funclets (their own `.pdata` functions) that return the address to continue at
// (`lea rax, [rip + continuation]`); the continuation is reached by no branch, only by the unwind
// (`RtlUnwindEx` consolidation), so function discovery would never decode it. Every `.pdata`
// function whose handler data starts with the RVA of a C++ `FuncInfo` (magic 0x19930520-22, as
// `__CxxFrameHandler3` and `__GSHandlerCheck_EH` read it) contributes its try-block map; each
// catch handler is discovered and every `lea rax, [rip + X]` in it whose X lies inside a
// `.pdata` function (not at an entry) names a continuation of that function (chained fragments
// resolve to their primary function). The handler routine is not identified by name, so the
// rule works for any build.
struct EhContinuations {
    std::map<std::uint64_t, std::set<std::uint64_t>> by_function;  // function entry -> continuations
    std::size_t func_infos = 0;        // distinct FuncInfo records read
    std::size_t catch_handlers = 0;    // distinct catch funclets scanned
    std::size_t handlers_without = 0;  // catch funclets in which no continuation was found
};
EhContinuations find_eh_continuations(const ImageView& view, const PeImage& img,
                                      const std::set<std::uint64_t>& entries);

// R4 (`docs/RECOMP_PLAN.md` "R4 design"): code reached only through a table of 16-bit offsets
// from a function's start, which the program turns into addresses at run time and jumps through
// (LuaJIT's interpreter: `lj_dispatch_init` adds each `lj_bc_ofs` entry to the VM's start and fills
// its dispatch table; the bytecode handlers are reached by no branch). Found from the code that
// builds the addresses: a 16-bit load from a table (`lea rT, [rip + T]` then `movzx r, word
// [rT (+ i * 2)]`, or `movzx r, word [rip + X]`) added to a register holding a `.pdata` function's
// start (`lea rB, [rip + B]`, `add r, rB`) within 8 instructions. Each such table is read from its
// lowest address until a value no longer lands inside that function; every `B + value` is an entry.
// Not part of `collect_function_entries` (the R2 selection must stay as the PC recorded it).
struct OffsetTables {
    struct Table {
        std::uint64_t table = 0;     // VA of the first entry
        std::uint64_t function = 0;  // the base the offsets are added to
        std::size_t count = 0;
    };
    std::vector<Table> tables;
    std::set<std::uint64_t> entries;
};
OffsetTables find_offset_table_entries(const ImageView& view, const PeImage& img, const std::set<std::uint64_t>& entries);

// The primary function entry (VA) whose `.pdata` range holds `va`, following chained unwind info;
// 0 when no `.pdata` range holds it.
std::uint64_t pdata_owner(const ImageView& view, const PeImage& img, std::uint64_t va);

// The `.text` range of a parsed image as {begin VA, end VA} (zeros when there is none).
std::pair<std::uint64_t, std::uint64_t> text_range(const PeImage& img);

}  // namespace wse::recomp
