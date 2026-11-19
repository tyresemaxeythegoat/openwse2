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
    // direct `jmp` to one of them (other than `entry`) is a tail call.
    DFunction discover(std::uint64_t entry, const std::set<std::uint64_t>& entries) const;

private:
    bool resolve_jump_table(const DFunction& fn, const DInsn& jmp, JumpTable& out) const;

    const ImageView& view_;
    ZydisDecoder decoder_{};
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

// The `.text` range of a parsed image as {begin VA, end VA} (zeros when there is none).
std::pair<std::uint64_t, std::uint64_t> text_range(const PeImage& img);

}  // namespace wse::recomp
