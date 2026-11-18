// R0 (d) PC reference (Windows x64, local only: it executes the user's retail code).
//
//   zig c++ -std=c++20 -O2 -target x86_64-windows-gnu -I engine/include \
//       -o r0_pc_reference.exe tools/r0_pc_reference.cpp
//   r0_pc_reference.exe <mb_warband_wse2_x64.exe> <mb_warband_wse2_x64.pdb>
//
// Maps the exe at its preferred base, finds `rglString::indexOf` by name in the PDB
// (`pdb_reader.h`; the PDB must match the exe's RSDS GUID), calls it natively on the
// `r0_inputs.h` cases, runs the same cases through the
// reference interpreter and the C++ model, and prints the FNV-1a hash of each. The phone's
// "R0 spikes" report prints the hash of the translated function for comparison. It also
// reports how many `.pdata` functions the R0 decoder subset decodes completely.
// Never run in CI and never committed with any output.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "wse/recomp/pdb_reader.h"
#include "wse/recomp/pe_image.h"
#include "wse/recomp/r0_inputs.h"
#include "wse/recomp/x86_interp.h"

using namespace wse::recomp;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: r0_pc_reference <exe> <pdb>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    std::ifstream pin(argv[2], std::ios::binary);
    const std::vector<std::uint8_t> pdb((std::istreambuf_iterator<char>(pin)),
                                        std::istreambuf_iterator<char>());
    PeImage img;
    std::string err;
    if (!parse_pe(file.data(), file.size(), img, &err)) {
        std::fprintf(stderr, "PE: %s\n", err.c_str());
        return 2;
    }
    PeCodeView cv;
    std::vector<PdbPublic> publics;
    PdbGuidAge ga;
    if (!read_pe_codeview(file.data(), file.size(), img, cv, &err) ||
        !read_pdb_publics(pdb.data(), pdb.size(), publics, &ga, &err) || ga.guid != cv.guid) {
        std::fprintf(stderr, "PDB: %s\n", err.empty() ? "GUID does not match the exe" : err.c_str());
        return 2;
    }
    const auto found = pdb_public_va(publics, kR0IndexOfSymbol, img.sections, img.image_base);
    if (!found) {
        std::fprintf(stderr, "PDB has no %s\n", kR0IndexOfSymbol);
        return 2;
    }
    const std::uint64_t index_of_va = *found;
    std::printf("exe %zu bytes, PDB age %u, %zu publics, indexOf at 0x%llx\n", file.size(), ga.age,
                publics.size(), static_cast<unsigned long long>(index_of_va));
    auto* base = static_cast<std::uint8_t*>(
        VirtualAlloc(reinterpret_cast<void*>(img.image_base), img.size_of_image,
                     MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (base != reinterpret_cast<std::uint8_t*>(img.image_base)) {
        std::fprintf(stderr, "could not map at 0x%llx\n",
                     static_cast<unsigned long long>(img.image_base));
        return 2;
    }
    if (!map_pe_sections(file.data(), file.size(), img, base)) {
        std::fprintf(stderr, "section mapping failed\n");
        return 2;
    }
    std::printf("mapped %u bytes at 0x%llx, %zu .pdata functions\n", img.size_of_image,
                static_cast<unsigned long long>(img.image_base), img.functions.size());

    // Coverage of the R0 decoder subset over every `.pdata` function.
    std::size_t decoded = 0;
    std::uint64_t decoded_bytes = 0;
    std::uint64_t total_bytes = 0;
    DecodedFunction index_of;
    for (const PeFunctionRange& r : img.functions) {
        const DecodedFunction fn =
            decode_function(base + r.begin, img.image_base + r.begin, img.image_base + r.end);
        total_bytes += r.end - r.begin;
        if (fn.error.empty()) {
            ++decoded;
            decoded_bytes += r.end - r.begin;
        }
        if (img.image_base + r.begin == index_of_va) {
            index_of = fn;
        }
    }
    std::printf("decoder subset: %zu/%zu functions, %llu/%llu bytes decode completely\n",
                decoded, img.functions.size(), static_cast<unsigned long long>(decoded_bytes),
                static_cast<unsigned long long>(total_bytes));
    if (index_of.insns.empty() || !index_of.error.empty()) {
        std::fprintf(stderr, "indexOf did not decode: %s\n", index_of.error.c_str());
        return 1;
    }

    using IndexOf = int (*)(const void*, const void*, int);
    const auto native = reinterpret_cast<IndexOf>(static_cast<std::uintptr_t>(index_of_va));
    const std::vector<R0IndexOfCase> cases = make_r0_index_of_cases();
    std::vector<std::int32_t> native_r;
    std::vector<std::int32_t> interp_r;
    std::vector<std::int32_t> model_r;
    GuestStack stack;
    Interpreter interp;
    for (const R0IndexOfCase& c : cases) {
        RglStringFake hay(c.haystack);
        RglStringFake needle(c.needle);
        native_r.push_back(native(hay.bytes, needle.bytes, c.start));
        CpuState s;
        stack.prepare(s);
        s.gpr[static_cast<int>(Reg::rcx)] = reinterpret_cast<std::uintptr_t>(hay.bytes);
        s.gpr[static_cast<int>(Reg::rdx)] = reinterpret_cast<std::uintptr_t>(needle.bytes);
        s.gpr[static_cast<int>(Reg::r8)] = static_cast<std::uint32_t>(c.start);
        s.rip = index_of_va;
        if (!interp.run(s, GuestStack::kReturnSentinel,
                        [&](std::uint64_t va) { return index_of.at(va); }, 1U << 20, &err)) {
            std::fprintf(stderr, "interpreter: %s\n", err.c_str());
            return 1;
        }
        interp_r.push_back(static_cast<std::int32_t>(s.gpr[0]));
        model_r.push_back(r0_index_of_model(c));
    }
    const std::uint64_t hn = fnv1a_results(native_r);
    std::printf("indexOf cases: %zu\n", cases.size());
    std::printf("native      hash 0x%016llx\n", static_cast<unsigned long long>(hn));
    std::printf("interpreter hash 0x%016llx\n",
                static_cast<unsigned long long>(fnv1a_results(interp_r)));
    std::printf("model       hash 0x%016llx\n",
                static_cast<unsigned long long>(fnv1a_results(model_r)));
    std::printf("first results:");
    for (std::size_t i = 0; i < 12; ++i) {
        std::printf(" %d", native_r[i]);
    }
    std::printf("\n");
    const bool ok = native_r == interp_r && native_r == model_r;
    std::printf("%s\n", ok ? "R0 (d) PC reference: native == interpreter == model"
                           : "R0 (d) PC reference: MISMATCH");
    return ok ? 0 : 1;
}
