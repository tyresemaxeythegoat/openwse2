// Host test of the LLVM translator (`wse_recomp`, `WSE_ENABLE_LLVM=ON`; CI job `recomp-host`
// against Ubuntu's LLVM 18). Synthetic code only (`recomp_test.cpp` functions and
// `x86_synth.h`): lifts, optimizes, links with the embedded LLD, loads the result and checks
// it against the reference interpreter; runs the R1 differential corpus (our C compiled for
// Win64: native vs translated); then runs the R0 spikes (a)-(c) as on the phone.

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../src/recomp/discover.h"
#include "../src/recomp/lift_r1.h"
#include "../src/recomp/r0_spikes.h"
#include "../src/recomp/recomp_llvm.h"
#include "wse/recomp/pe_image.h"
#include "wse/recomp/r0_inputs.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/x86_interp.h"
#include "wse/recomp/x86_synth.h"

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

using namespace wse::recomp;

constexpr std::uint64_t kBase = 0x140001000ULL;

const std::vector<std::uint8_t> kSumBytes = {0x31, 0xc0, 0x85, 0xd2, 0x7e, 0x0e, 0x44,
                                             0x0f, 0xb6, 0x01, 0x44, 0x01, 0xc0, 0x48,
                                             0xff, 0xc1, 0xff, 0xca, 0x75, 0xf2, 0xc3};
const std::vector<std::uint8_t> kSmax = {0x89, 0xc8, 0x39, 0xd1, 0x7d, 0x02, 0x89, 0xd0, 0xc3};
const std::vector<std::uint8_t> kHomeArea = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x53, 0x48,
                                             0x8b, 0x44, 0x24, 0x10, 0x5b, 0xc3};

std::uint64_t call(GuestFn native, const DecodedFunction& fn, std::uint64_t rcx, std::uint64_t rdx,
                   std::uint64_t rbx, bool* same) {
    GuestStack stack;
    CpuState a;
    stack.prepare(a);
    a.rip = fn.start;
    a.gpr[static_cast<int>(Reg::rcx)] = rcx;
    a.gpr[static_cast<int>(Reg::rdx)] = rdx;
    a.gpr[static_cast<int>(Reg::rbx)] = rbx;
    CpuState b = a;
    native(&a);
    Interpreter interp;
    std::string err;
    (void)interp.run(b, GuestStack::kReturnSentinel, [&](std::uint64_t va) { return fn.at(va); },
                     1U << 24, &err);
    bool eq = a.rip == GuestStack::kReturnSentinel && b.rip == a.rip;
    for (int r = 0; r < 16; ++r) {
        eq = eq && a.gpr[r] == b.gpr[r];
    }
    eq = eq && a.zf == b.zf && a.sf == b.sf && a.of == b.of && a.cf == b.cf && a.pf == b.pf;
    *same = eq;
    return a.gpr[0];
}

void test_translate_small_functions(const std::string& dir) {
    Translator tr;
    expect(tr.init_error().empty(), "LLVM target initializes");
    const DecodedFunction sum = decode_function(kSumBytes.data(), kBase, kBase + kSumBytes.size());
    const DecodedFunction smax = decode_function(kSmax.data(), kBase + 0x100, kBase + 0x100 + kSmax.size());
    const DecodedFunction home = decode_function(kHomeArea.data(), kBase + 0x200, kBase + 0x200 + kHomeArea.size());
    std::string err;
    expect(tr.add_function(sum, "t_sum", &err) && tr.add_function(smax, "t_smax", &err) &&
               tr.add_function(home, "t_home", &err) && tr.function_count() == 3,
           "lift three synthetic functions");
    std::vector<char> obj;
    TranslateTimings t;
    expect(tr.emit_object(2, obj, &t, &err) && !obj.empty(), "emit an O2 object");
    const std::string o = dir + "/small.o";
    const std::string so = dir + "/small.so";
    std::ofstream(o, std::ios::binary).write(obj.data(), static_cast<std::streamsize>(obj.size()));
    std::string log;
    expect(link_shared_object({o}, so, &log), "LLD links the object into a shared object");
    void* h = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    expect(h != nullptr, "dlopen the translated shared object");
    if (h == nullptr) {
        std::printf("  dlopen: %s\n", dlerror());
        return;
    }
    auto* f_sum = reinterpret_cast<GuestFn>(dlsym(h, "t_sum"));
    auto* f_smax = reinterpret_cast<GuestFn>(dlsym(h, "t_smax"));
    auto* f_home = reinterpret_cast<GuestFn>(dlsym(h, "t_home"));
    expect(f_sum != nullptr && f_smax != nullptr && f_home != nullptr, "symbols resolve");
    if (f_sum != nullptr && f_smax != nullptr && f_home != nullptr) {
        const std::uint8_t data[] = {1, 2, 3, 250, 7};
        bool s1 = false;
        bool s2 = false;
        bool s3 = false;
        bool s4 = false;
        bool s5 = false;
        expect(call(f_sum, sum, reinterpret_cast<std::uintptr_t>(data), 5, 0, &s1) == 263 && s1,
               "translated sum_bytes == interpreter (registers and flags)");
        expect(call(f_sum, sum, 0, 0, 0, &s2) == 0 && s2, "translated sum_bytes n = 0");
        expect(call(f_smax, smax, static_cast<std::uint32_t>(-7), 4, 0, &s3) == 4 && s3 &&
                   call(f_smax, smax, 9, static_cast<std::uint32_t>(-2), 0, &s4) == 9 && s4,
               "translated smax == interpreter");
        expect(call(f_home, home, 0, 0, 0x1234567890ULL, &s5) == 0x1234567890ULL && s5,
               "translated home-area push/pop == interpreter");
    }
    dlclose(h);
}

// --- R1 differential corpus (`engine/tests/recomp_corpus/*_corpus.c`) ----------------------
// Our own C, compiled by clang for Win64 into a PE at 0x140000000 (CMake). The PE is mapped at
// its base; every export runs natively (ms_abi) and translated on the same inputs.

#if defined(__x86_64__)
using NativeFn = std::uint64_t(__attribute__((ms_abi)) *)(std::uint64_t, std::uint64_t,
                                                          std::uint64_t, std::uint64_t);
constexpr bool kCanRunNative = true;
#else
constexpr bool kCanRunNative = false;  // arm64 runner: `--check` against x86-recorded results
#endif

struct Export {
    std::string name;
    std::uint64_t va = 0;
};

std::uint32_t rd32m(const std::uint8_t* p, std::size_t off) {
    std::uint32_t v = 0;
    std::memcpy(&v, p + off, 4);
    return v;
}

std::vector<Export> read_exports(const std::uint8_t* mapped, std::size_t size, std::uint64_t base) {
    std::vector<Export> out;
    const std::uint32_t pe = rd32m(mapped, 0x3C);
    const std::uint32_t dir = rd32m(mapped, pe + 24 + 112);  // PE32+ data directory 0
    if (dir == 0 || dir + 40 > size) {
        return out;
    }
    const std::uint32_t n_names = rd32m(mapped, dir + 0x18);
    const std::uint32_t funcs = rd32m(mapped, dir + 0x1C);
    const std::uint32_t names = rd32m(mapped, dir + 0x20);
    const std::uint32_t ords = rd32m(mapped, dir + 0x24);
    for (std::uint32_t i = 0; i < n_names; ++i) {
        const std::uint32_t name_rva = rd32m(mapped, names + 4 * i);
        std::uint16_t ord = 0;
        std::memcpy(&ord, mapped + ords + 2 * i, 2);
        Export e;
        e.name = reinterpret_cast<const char*>(mapped + name_rva);
        e.va = base + rd32m(mapped, funcs + 4U * ord);
        out.push_back(e);
    }
    return out;
}

struct Rng {
    std::uint64_t s = 0x9E3779B97F4A7C15ULL;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
};

std::uint64_t pick(Rng& r) {
    static const std::uint64_t kEdges[] = {0,
                                           1,
                                           2,
                                           7,
                                           0xFF,
                                           0x100,
                                           0x7FFF,
                                           0x8000,
                                           0xFFFF,
                                           0x7FFFFFFF,
                                           0x80000000ULL,
                                           0xFFFFFFFFULL,
                                           0x100000000ULL,
                                           0x7FFFFFFFFFFFFFFFULL,
                                           0x8000000000000000ULL,
                                           ~0ULL,
                                           ~0ULL - 1};
    const std::uint64_t k = r.next();
    switch (k % 4) {
    case 0: return kEdges[(k >> 8) % (sizeof(kEdges) / sizeof(kEdges[0]))];
    case 1: return (k >> 8) & 0xFFF;  // small: loop counts, shift counts, indices
    default: return r.next();
    }
}

// `--record <file>` writes each export's native result hash (x86 CI); `--check <pe> <file>` runs
// only the translated side and compares with such a file (the arm64 CI runner, which cannot run
// the x86 code natively).
const char* g_record_path = nullptr;
bool g_check_mode = false;
// Per export, per run: {rax, memory hash} recorded natively on x86.
std::map<std::string, std::vector<std::pair<std::uint64_t, std::uint64_t>>> g_expected;

void mix(std::uint64_t& h, std::uint64_t v) {
    h ^= v;
    h *= 0x100000001B3ULL;
    h ^= h >> 29;
}

void mix_bytes(std::uint64_t& h, const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i + 8 <= n; i += 8) {
        std::uint64_t v = 0;
        std::memcpy(&v, p + i, 8);
        mix(h, v);
    }
}

void test_corpus(const char* path) {
    if (!kCanRunNative && !g_check_mode) {
        std::printf("  corpus skipped: not an x86-64 host and no --check results\n");
        return;
    }
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    PeImage img;
    std::string err;
    expect(!file.empty() && parse_pe(file.data(), file.size(), img, &err), "corpus: the Win64 PE parses");
    if (file.empty() || img.size_of_image == 0) {
        std::printf("  corpus %s: %s\n", path, err.c_str());
        return;
    }
    const std::size_t map_size = (img.size_of_image + 0xFFFU) & ~static_cast<std::size_t>(0xFFF);
    void* at = mmap(reinterpret_cast<void*>(img.image_base), map_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    expect(at == reinterpret_cast<void*>(img.image_base), "corpus: the PE maps at its base");
    if (at != reinterpret_cast<void*>(img.image_base)) {
        return;
    }
    struct Unmap {  // R0 (b) maps 0x140000000 again later in this process
        void* p;
        std::size_t n;
        ~Unmap() { munmap(p, n); }
    } unmap{at, map_size};
    auto* mapped = static_cast<std::uint8_t*>(at);
    expect(map_pe_sections(file.data(), file.size(), img, mapped), "corpus: sections copy");

    std::uint64_t text_b = 0;
    std::uint64_t text_e = 0;
    for (const PeSection& s : img.sections) {
        if (s.name == ".text") {
            text_b = img.image_base + s.virtual_address;
            text_e = text_b + s.virtual_size;
        }
    }
    const std::vector<Export> exports = read_exports(mapped, img.size_of_image, img.image_base);
    expect(exports.size() >= 30, "corpus: exports are found");

    // Entries: exports, non-chained .pdata begins, then direct call targets to a fixed point.
    std::set<std::uint64_t> entries;
    for (const Export& e : exports) {
        entries.insert(e.va);
    }
    for (const PeFunctionRange& r : img.functions) {
        if (((mapped[r.unwind] >> 3) & 0x4) == 0) {
            entries.insert(img.image_base + r.begin);
        }
    }
    ImageView view(mapped, img.image_base, img.size_of_image, text_b, text_e);
    Discoverer disc(view);
    std::map<std::uint64_t, DFunction> fns;
    std::vector<std::uint64_t> work(entries.begin(), entries.end());
    while (!work.empty()) {
        const std::uint64_t e = work.back();
        work.pop_back();
        if (fns.count(e) != 0) {
            continue;
        }
        DFunction f = disc.discover(e, entries);
        for (const std::uint64_t t : f.call_targets) {
            if (view.in_text(t) && entries.insert(t).second) {
                work.push_back(t);
            }
        }
        fns.emplace(e, std::move(f));
    }
    std::size_t complete = 0;
    for (const auto& [va, f] : fns) {
        if (f.error.empty() && f.unresolved_indirect_jumps.empty()) {
            ++complete;
        } else {
            std::printf("  discover 0x%llx: %s\n", static_cast<unsigned long long>(va), f.error.c_str());
        }
    }
    expect(complete == fns.size(), "corpus: every function body decodes completely");

    ImageInfo info;
    info.base = img.image_base;
    R1Translator tr(info);
    expect(tr.init_error().empty(), "corpus: the R1 translator initializes");
    tr.declare(std::vector<std::uint64_t>(entries.begin(), entries.end()));
    std::set<std::uint64_t> lifted;
    for (const auto& [va, f] : fns) {
        std::string why;
        if (tr.lift(f, &why)) {
            lifted.insert(va);
        } else {
            std::printf("  lift 0x%llx: %s\n", static_cast<unsigned long long>(va), why.c_str());
        }
    }
    const LiftReport rep = tr.report();
    std::printf("  coverage: %zu/%zu functions lifted\n", rep.lifted, rep.attempted);
    for (const auto& [what, n] : rep.unsupported) {
        std::printf("    unsupported %-32s %zu\n", what.c_str(), n);
    }
    expect(rep.lifted == fns.size(), "corpus: every corpus function lifts");
    std::printf("  lifted as run-time faults: %zu traps, %zu AVX instructions\n", rep.traps,
                rep.gated.count("AVX") != 0 ? rep.gated.at("AVX") : 0);
    expect(rep.traps >= 1 && rep.gated.count("AVX") != 0,
           "corpus: int3 and AVX on cold paths lift as run-time faults (cold_paths)");

    std::vector<char> obj;
    TranslateTimings t;
    expect(tr.emit_object(2, obj, &t, &err) && !obj.empty(), "corpus: emit the O2 object");
    char tmpl[] = "/tmp/wse_r1_corpus_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (dir == nullptr || obj.empty()) {
        std::printf("  emit: %s\n", err.c_str());
        return;
    }
    const std::string o = std::string(dir) + "/corpus.o";
    const std::string so = std::string(dir) + "/corpus.so";
    std::ofstream(o, std::ios::binary).write(obj.data(), static_cast<std::streamsize>(obj.size()));
    std::string log;
    expect(link_shared_object({o}, so, &log), "corpus: LLD links the translated module");
    void* h = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    expect(h != nullptr, "corpus: dlopen the translated module");
    if (h == nullptr) {
        std::printf("  link: %s dlopen: %s\n", log.c_str(), dlerror());
        return;
    }
    const auto* rows = static_cast<const FnRow*>(dlsym(h, "wse_fn_table"));
    const auto* count = static_cast<const std::uint64_t*>(dlsym(h, "wse_fn_table_count"));
    expect(rows != nullptr && count != nullptr && *count == lifted.size(),
           "corpus: the module exports its dispatch table (defined functions only)");
    if (rows == nullptr || count == nullptr) {
        return;
    }
    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    rt.add_table(rows, static_cast<std::size_t>(*count));
    std::printf("  translated %zu functions (lift %.1f ms, opt %.1f ms, codegen %.1f ms, %zu B)\n",
                rt.function_count(), t.lift_ms, t.optimize_ms, t.codegen_ms, obj.size());

    constexpr int kRuns = 200;
    constexpr int kCalleeSaved[] = {3, 5, 6, 7, 12, 13, 14, 15};
    alignas(16) static std::uint8_t buf_native[4096];
    alignas(16) static std::uint8_t buf_trans[4096];
    Rng rng;
    std::size_t checked = 0;
    std::FILE* record = g_record_path != nullptr ? std::fopen(g_record_path, "w") : nullptr;
    for (const Export& e : exports) {
        if (lifted.count(e.va) == 0) {
            continue;
        }
        const bool is_mem = e.name.rfind("m_", 0) == 0;
        int bad = 0;
        const auto expected = g_expected.find(e.name);
        int check_bad = 0;
        // Every run happens even after a mismatch, so the input sequence never depends on results.
        for (int run = 0; run < kRuns; ++run) {
            std::uint64_t arg[4] = {pick(rng), pick(rng), pick(rng), pick(rng)};
            for (std::uint8_t& b : buf_native) {
                b = static_cast<std::uint8_t>(rng.next());
            }
            std::memcpy(buf_trans, buf_native, sizeof(buf_trans));
            if (is_mem) {
                arg[0] = reinterpret_cast<std::uintptr_t>(buf_native);
            }
            std::uint64_t want = 0;
            if (!g_check_mode) {
#if defined(__x86_64__)
                want = reinterpret_cast<NativeFn>(e.va)(arg[0], arg[1], arg[2], arg[3]);
#endif
                if (record != nullptr) {
                    std::uint64_t mh = 0xCBF29CE484222325ULL;
                    mix_bytes(mh, buf_native, sizeof(buf_native));
                    std::fprintf(record, "%s %d %016llx %016llx\n", e.name.c_str(), run,
                                 static_cast<unsigned long long>(want), static_cast<unsigned long long>(mh));
                }
            }

            GuestStack stack;
            CpuState s;
            stack.prepare(s);
            for (int r = 0; r < 16; ++r) {
                if (r != 4) {
                    s.gpr[r] = rng.next();
                }
                for (int k = 0; k < 16; ++k) {
                    s.xmm[r][k] = static_cast<std::uint8_t>(rng.next());
                }
            }
            s.gpr[1] = is_mem ? reinterpret_cast<std::uintptr_t>(buf_trans) : arg[0];
            s.gpr[2] = arg[1];
            s.gpr[8] = arg[2];
            s.gpr[9] = arg[3];
            const CpuState before = s;
            faults.clear();
            const bool ran = rt.run(s, e.va);
            if (g_check_mode) {
                std::uint64_t mh = 0xCBF29CE484222325ULL;
                mix_bytes(mh, buf_trans, sizeof(buf_trans));
                const bool have = expected != g_expected.end() && static_cast<std::size_t>(run) < expected->second.size();
                if (!have || expected->second[static_cast<std::size_t>(run)].first != s.gpr[0] ||
                    expected->second[static_cast<std::size_t>(run)].second != mh) {
                    if (++check_bad <= 3) {
                        std::printf("  MISMATCH %s run %d (0x%llx, 0x%llx, 0x%llx, 0x%llx): x86 rax 0x%llx memory %s, "
                                    "translated rax 0x%llx\n",
                                    e.name.c_str(), run, static_cast<unsigned long long>(arg[0]),
                                    static_cast<unsigned long long>(arg[1]), static_cast<unsigned long long>(arg[2]),
                                    static_cast<unsigned long long>(arg[3]),
                                    static_cast<unsigned long long>(have ? expected->second[static_cast<std::size_t>(run)].first : 0),
                                    !have ? "missing" : (expected->second[static_cast<std::size_t>(run)].second == mh ? "same" : "differs"),
                                    static_cast<unsigned long long>(s.gpr[0]));
                    }
                }
            }
            const bool same_mem = g_check_mode || std::memcmp(buf_native, buf_trans, sizeof(buf_trans)) == 0;
            bool ok = ran && faults.empty() && (g_check_mode || s.gpr[0] == want) &&
                      s.rip == GuestStack::kReturnSentinel && s.gpr[4] == before.gpr[4] + 8 && same_mem;
            for (const int r : kCalleeSaved) {
                ok = ok && s.gpr[r] == before.gpr[r];
            }
            for (int x = 6; x < 16; ++x) {  // xmm6..xmm15 are callee-saved in Win64
                ok = ok && std::memcmp(s.xmm[x], before.xmm[x], 16) == 0;
            }
            ok = ok && !s.df;  // the Win64 ABI returns with DF clear
            if (!ok && ++bad <= 3) {
                std::printf("  MISMATCH %s(0x%llx, 0x%llx, 0x%llx, 0x%llx): native 0x%llx translated 0x%llx "
                            "rip 0x%llx faults %zu memory %s\n",
                            e.name.c_str(), static_cast<unsigned long long>(arg[0]),
                            static_cast<unsigned long long>(arg[1]), static_cast<unsigned long long>(arg[2]),
                            static_cast<unsigned long long>(arg[3]), static_cast<unsigned long long>(want),
                            static_cast<unsigned long long>(s.gpr[0]), static_cast<unsigned long long>(s.rip),
                            faults.size(), same_mem ? "same" : "differs");
                for (const Fault& f : faults) {
                    std::printf("    fault: %s at 0x%llx %s\n", fault_name(f.kind),
                                static_cast<unsigned long long>(f.va), f.what.c_str());
                }
            }
        }
        bad += check_bad;
        if (bad == 0) {
            ++checked;
        }
    }
    if (record != nullptr) {
        std::fclose(record);
    }
    std::printf("  differential: %zu/%zu exports match %s on %d inputs each\n", checked, exports.size(),
                g_check_mode ? "the recorded x86 results" : "native", kRuns);
    expect(checked == exports.size(), "corpus: every export matches native (rax, memory, callee-saved, rsp)");
    dlclose(h);
}

}  // namespace

int main(int argc, char** argv) {
    const char* corpus = nullptr;
#ifdef WSE_RECOMP_CORPUS
    corpus = WSE_RECOMP_CORPUS;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--record" && i + 1 < argc) {
            g_record_path = argv[++i];
        } else if (a == "--check" && i + 2 < argc) {
            corpus = argv[++i];
            g_check_mode = true;
            std::ifstream in(argv[++i]);
            std::string name;
            int run = 0;
            std::string rax;
            std::string mem;
            while (in >> name >> run >> rax >> mem) {
                auto& v = g_expected[name];
                if (static_cast<std::size_t>(run) >= v.size()) {
                    v.resize(static_cast<std::size_t>(run) + 1);
                }
                v[static_cast<std::size_t>(run)] = {std::stoull(rax, nullptr, 16), std::stoull(mem, nullptr, 16)};
            }
            expect(!g_expected.empty(), "check: the recorded results file is not empty");
        }
    }
    char tmpl[] = "/tmp/wse_recomp_llvm_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (dir == nullptr) {
        std::printf("recomp_llvm tests: FAIL (mkdtemp)\n");
        return 1;
    }
    test_translate_small_functions(dir);
    if (corpus != nullptr) {
        test_corpus(corpus);
    } else {
        std::printf("  (no Win64 corpus: clang/lld-link were not found at configure time)\n");
    }

    R0SpikeInput in;
    in.work_dir = dir;
    in.exe_error = "host test (synthetic only)";
    in.synthetic_blocks = 500;
    const R0SpikeResult r = run_r0_spikes(in);
    std::fputs(r.report.c_str(), stdout);
    expect(r.a_loads, "R0 (a): a generated shared object loads and runs");
    expect(r.b_maps, "R0 (b): 0x140000000 maps");
    expect(r.c_measured, "R0 (c): synthetic translation measured and matches the interpreter");
    expect(!r.d_matches, "R0 (d) is skipped without an exe");

    if (g_failures == 0) {
        std::printf("recomp_llvm tests: PASS\n");
        return 0;
    }
    std::printf("recomp_llvm tests: FAIL (%d)\n", g_failures);
    return 1;
}
