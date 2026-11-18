// R0 (d) PC reference (Windows x64, local only: it executes the user's retail code).
//
//   zig c++ -std=c++20 -O2 -target x86_64-windows-gnu -I engine/include \
//       -o r0_pc_reference.exe tools/r0_pc_reference.cpp
//   r0_pc_reference.exe <path to mb_warband_wse2_x64.exe v1.1.5.5>
//
// Maps the exe at its preferred base (`0x140000000`), calls `rglString::indexOf`
// (`0x14030f630`) natively on the `r0_inputs.h` cases, runs the same cases through the
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

#include "wse/recomp/pe_image.h"
#include "wse/recomp/r0_inputs.h"
#include "wse/recomp/x86_interp.h"

using namespace wse::recomp;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: r0_pc_reference <mb_warband_wse2_x64.exe>\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    if (file.size() != kWse2V1155ExeSize) {
        std::fprintf(stderr, "expected the v1.1.5.5 exe (%llu bytes), got %zu bytes\n",
                     static_cast<unsigned long long>(kWse2V1155ExeSize), file.size());
        return 2;
    }
    PeImage img;
    std::string err;
    if (!parse_pe(file.data(), file.size(), img, &err)) {
        std::fprintf(stderr, "PE: %s\n", err.c_str());
        return 2;
    }
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
        if (img.image_base + r.begin == kR0IndexOfVa) {
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
    const auto native = reinterpret_cast<IndexOf>(static_cast<std::uintptr_t>(kR0IndexOfVa));
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
        s.rip = kR0IndexOfVa;
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
