// Host test of the LLVM translator (`wse_recomp`, `WSE_ENABLE_LLVM=ON`; CI job `recomp-host`
// against Ubuntu's LLVM 18). Synthetic code only (`recomp_test.cpp` functions and
// `x86_synth.h`): lifts, optimizes, links with the embedded LLD, loads the result and checks
// it against the reference interpreter; then runs the R0 spikes (a)-(c) as on the phone.

#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../src/recomp/r0_spikes.h"
#include "../src/recomp/recomp_llvm.h"
#include "wse/recomp/r0_inputs.h"
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
using GuestFn = void (*)(CpuState*);

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

}  // namespace

int main() {
    char tmpl[] = "/tmp/wse_recomp_llvm_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (dir == nullptr) {
        std::printf("recomp_llvm tests: FAIL (mkdtemp)\n");
        return 1;
    }
    test_translate_small_functions(dir);

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
