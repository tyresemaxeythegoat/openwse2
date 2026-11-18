// R1 discovery report (local only: reads the user's exe + PDB; prints counts, never code).
//
//   zig c++ -std=c++20 -O2 -target x86_64-windows-gnu -I engine/include -I <zydis dir>
//       -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -o r1_discovery_report.exe
//       tools/r1_discovery_report.cpp engine/src/recomp/discover.cpp zydis.o
//   r1_discovery_report.exe <mb_warband_wse2_x64.exe> <mb_warband_wse2_x64.pdb>
//
// Collects function entries (non-chained `.pdata` begins, PDB publics in `.text`, direct call
// targets, repeated to a fixed point), follows control flow from each with `discover.h`, and
// reports how many bodies decode completely, jump tables, tail calls, and the mnemonics used.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../engine/src/recomp/discover.h"
#include "wse/recomp/pdb_reader.h"
#include "wse/recomp/pe_image.h"

using namespace wse::recomp;

static std::vector<std::uint8_t> slurp(const char* p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: r1_discovery_report <exe> <pdb>\n");
        return 2;
    }
    const auto file = slurp(argv[1]);
    const auto pdb = slurp(argv[2]);
    PeImage img;
    std::string err;
    if (!parse_pe(file.data(), file.size(), img, &err)) {
        std::fprintf(stderr, "PE: %s\n", err.c_str());
        return 2;
    }
    std::vector<std::uint8_t> mapped(img.size_of_image);
    if (!map_pe_sections(file.data(), file.size(), img, mapped.data())) {
        return 2;
    }
    std::uint64_t text_b = 0, text_e = 0;
    for (const auto& s : img.sections) {
        if (s.name == ".text") {
            text_b = img.image_base + s.virtual_address;
            text_e = text_b + s.virtual_size;
        }
    }
    ImageView view(mapped.data(), img.image_base, mapped.size(), text_b, text_e);

    // Entries: .pdata begins whose UNWIND_INFO is not a chained fragment (flag 4).
    std::set<std::uint64_t> entries;
    std::size_t chained = 0;
    for (const auto& r : img.functions) {
        const std::uint8_t ver_flags = mapped[r.unwind];
        if ((ver_flags >> 3) & 0x4) {
            ++chained;
            continue;
        }
        entries.insert(img.image_base + r.begin);
    }
    const std::size_t from_pdata = entries.size();
    std::vector<PdbPublic> publics;
    PdbGuidAge ga;
    std::size_t from_pdb = 0;
    if (read_pdb_publics(pdb.data(), pdb.size(), publics, &ga, &err)) {
        for (const auto& p : publics) {
            if (p.section == 0 || p.section > img.sections.size()) {
                continue;
            }
            const std::uint64_t va = img.image_base + img.sections[p.section - 1].virtual_address + p.offset;
            if (va >= text_b && va < text_e && (p.flags & 2U) != 0 && entries.insert(va).second) {
                ++from_pdb;  // flag 2 = function (CVPSF_FUNCTION)
            }
        }
    }

    Discoverer disc(view);
    std::map<std::uint64_t, DFunction> fns;
    std::vector<std::uint64_t> work(entries.begin(), entries.end());
    std::size_t from_calls = 0;
    while (!work.empty()) {
        const std::uint64_t e = work.back();
        work.pop_back();
        if (fns.count(e) != 0) {
            continue;
        }
        DFunction f = disc.discover(e, entries);
        for (const std::uint64_t t : f.call_targets) {
            if (view.in_text(t) && entries.insert(t).second) {
                ++from_calls;
                work.push_back(t);
            }
        }
        fns.emplace(e, std::move(f));
    }

    std::size_t ok = 0, jt = 0, jt_fns = 0, unresolved = 0, unresolved_fns = 0, noret = 0, tails = 0;
    std::uint64_t insns = 0;
    std::map<std::string, std::size_t> errors;
    std::map<std::string, std::size_t> mnemonics;
    for (const auto& [e, f] : fns) {
        (void)e;
        insns += f.insns.size();
        jt += f.jump_tables.size();
        jt_fns += f.jump_tables.empty() ? 0 : 1;
        unresolved += f.unresolved_indirect_jumps.size();
        unresolved_fns += f.unresolved_indirect_jumps.empty() ? 0 : 1;
        noret += f.noreturn_calls.size();
        tails += f.tail_targets.size();
        if (f.error.empty() && f.unresolved_indirect_jumps.empty()) {
            ++ok;
        } else if (!f.error.empty()) {
            errors[f.error.substr(0, f.error.find(" at "))]++;
        }
        for (const auto& [va, d] : f.insns) {
            (void)va;
            mnemonics[ZydisMnemonicGetString(d.ins.mnemonic)]++;
        }
    }
    std::printf("entries: %zu (.pdata %zu, chained fragments skipped %zu, PDB functions +%zu, call targets +%zu)\n",
                entries.size(), from_pdata, chained, from_pdb, from_calls);
    std::printf("functions discovered: %zu, bodies complete: %zu, instructions reached: %llu\n", fns.size(), ok,
                static_cast<unsigned long long>(insns));
    std::printf("jump tables resolved: %zu in %zu functions; unresolved indirect jmps: %zu in %zu functions\n", jt,
                jt_fns, unresolved, unresolved_fns);
    std::printf("tail calls: %zu, calls treated as noreturn: %zu, distinct mnemonics: %zu\n", tails, noret,
                mnemonics.size());
    for (const auto& [k, v] : errors) {
        std::printf("  error %-40s %zu\n", k.c_str(), v);
    }
    // With a third argument: group the unresolved indirect jumps by the mnemonics of the 6
    // instructions before them (no addresses or operands are printed).
    if (argc > 3) {
        std::map<std::string, std::size_t> shapes;
        for (const auto& [e, f] : fns) {
            (void)e;
            for (const std::uint64_t j : f.unresolved_indirect_jumps) {
                std::vector<std::string> seq;
                auto it = f.insns.find(j);
                for (int k = 0; k < 7 && it != f.insns.end(); ++k) {
                    const DInsn& d = it->second;
                    std::string op;
                    for (int o = 0; o < d.ins.operand_count_visible; ++o) {
                        op += d.ops[o].type == ZYDIS_OPERAND_TYPE_REGISTER ? "r"
                              : d.ops[o].type == ZYDIS_OPERAND_TYPE_MEMORY  ? (d.ops[o].mem.base == ZYDIS_REGISTER_RIP ? "M" : "m")
                                                                            : "i";
                    }
                    seq.insert(seq.begin(), std::string(ZydisMnemonicGetString(d.ins.mnemonic)) + "(" + op + ")");
                    if (it == f.insns.begin()) break;
                    --it;
                }
                std::string key;
                for (const auto& s : seq) key += s + " ";
                shapes[key]++;
            }
        }
        std::vector<std::pair<std::size_t, std::string>> sorted;
        for (const auto& [k, v] : shapes) sorted.emplace_back(v, k);
        std::sort(sorted.rbegin(), sorted.rend());
        for (std::size_t i = 0; i < sorted.size() && i < 25; ++i) {
            std::printf("  %4zu  %s\n", sorted[i].first, sorted[i].second.c_str());
        }
    }
    return 0;
}
