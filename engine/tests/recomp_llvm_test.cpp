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
#include "wse/recomp/seh.h"
#include "wse/platform/vfs.h"
#include "wse/recomp/shim_crt.h"
#include "wse/recomp/shim_files.h"
#include "wse/recomp/shim_memory.h"
#include "wse/recomp/shim_modules.h"
#include "wse/recomp/shim_strings.h"
#include "wse/recomp/shim_time.h"
#include "wse/recomp/teb.h"
#include "wse/recomp/threadpool.h"
#include "wse/recomp/threads.h"
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
    std::string name;  // "#42" for an export by ordinal only
    std::uint64_t va = 0;
};

// The image's exported code (forwarders, which name another DLL's function, are left out).
std::vector<Export> read_exports(const std::uint8_t* mapped, std::size_t size, std::uint64_t base) {
    std::vector<Export> out;
    PeExports ex;
    std::string err;
    if (!read_pe_exports(mapped, size, ex, &err)) {
        std::printf("  exports: %s\n", err.c_str());
        return out;
    }
    for (const PeExport& e : ex.entries) {
        if (e.forwarder.empty()) {
            out.push_back({e.name.empty() ? "#" + std::to_string(e.ordinal) : e.name, base + e.rva});
        }
    }
    return out;
}

// One mapped PE image of the corpus.
struct Mapped {
    PeImage img;
    std::uint8_t* mapped = nullptr;
    std::size_t map_size = 0;
    std::vector<Export> exports;
    Mapped() = default;
    Mapped(const Mapped&) = delete;
    Mapped& operator=(const Mapped&) = delete;
    ~Mapped() {
        if (mapped != nullptr) {
            munmap(mapped, map_size);  // R0 (b) maps 0x140000000 again later in this process
        }
    }
};

// Reads `path` and maps it at `base` (0: its preferred base), applying base relocations when it
// moves. False (after reporting) when the file is missing or the range is taken.
bool map_image(const std::string& path, std::uint64_t base, Mapped& m, std::size_t* relocations) {
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    if (file.empty() || !parse_pe(file.data(), file.size(), m.img, &err)) {
        std::printf("  corpus %s: %s\n", path.c_str(), file.empty() ? "missing" : err.c_str());
        return false;
    }
    const std::uint64_t at_base = base != 0 ? base : m.img.image_base;
    m.map_size = (m.img.size_of_image + 0xFFFU) & ~static_cast<std::size_t>(0xFFF);
    void* at = mmap(reinterpret_cast<void*>(at_base), m.map_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (at != reinterpret_cast<void*>(at_base)) {
        if (at != MAP_FAILED) {
            munmap(at, m.map_size);
        }
        std::printf("  corpus %s: 0x%llx is not free\n", path.c_str(), static_cast<unsigned long long>(at_base));
        return false;
    }
    m.mapped = static_cast<std::uint8_t*>(at);
    if (!map_pe_sections(file.data(), file.size(), m.img, m.mapped) ||
        !apply_pe_relocations(m.mapped, m.img.size_of_image, at_base, relocations, &err)) {
        std::printf("  corpus %s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    m.img.image_base = at_base;
    m.exports = read_exports(m.mapped, m.img.size_of_image, at_base);
    return true;
}

// One image translated into a loaded shared object.
struct Translated {
    void* handle = nullptr;
    const FnRow* rows = nullptr;
    std::size_t count = 0;
    std::set<std::uint64_t> lifted;
    TranslateTimings t;
    std::size_t obj_size = 0;
};

// Discovers, lifts, emits and links one mapped image (`tag` names its files and checks). The exe
// also gets the checks that only its corpus units satisfy (catch continuations, run-time faults).
bool translate_image(const char* tag, Mapped& m, bool is_exe, const char* work_dir, Translated& out) {
    const PeImage& img = m.img;
    std::uint8_t* mapped = m.mapped;
    std::string err;
    std::uint64_t text_b = 0;
    std::uint64_t text_e = 0;
    for (const PeSection& s : img.sections) {
        if (s.name == ".text") {
            text_b = img.image_base + s.virtual_address;
            text_e = text_b + s.virtual_size;
        }
    }
    // Entries: exports, the entry point, non-chained .pdata begins, then direct call targets to a
    // fixed point.
    std::set<std::uint64_t> entries;
    for (const Export& e : m.exports) {
        entries.insert(e.va);
    }
    if (img.entry_rva != 0) {
        entries.insert(img.image_base + img.entry_rva);
    }
    for (const PeFunctionRange& r : img.functions) {
        if (((mapped[r.unwind] >> 3) & 0x4) == 0) {
            entries.insert(img.image_base + r.begin);
        }
    }
    ImageView view(mapped, img.image_base, img.size_of_image, text_b, text_e);
    // R3: catch continuations, reached only through a catch funclet (`t_cxx_catch`).
    const EhContinuations eh = find_eh_continuations(view, img, entries);
    if (is_exe) {
        std::uint64_t cxx = 0;
        for (const Export& e : m.exports) {
            cxx = e.name == "t_cxx_catch" ? e.va : cxx;
        }
        const auto it = eh.by_function.find(cxx);
        // Three FuncInfo: t_cxx_catch, t_cxx_rethrow (continuations) and seh_rethrow_inner, whose
        // catch only rethrows.
        expect(eh.func_infos == 3 && eh.catch_handlers == 3 && eh.handlers_without == 1 &&
                   eh.by_function.size() == 2 && it != eh.by_function.end() && it->second.size() == 1,
               "corpus: the catch funclets name t_cxx_catch's and t_cxx_rethrow's continuations");
        expect(pdata_owner(view, img, cxx + 1) == cxx && pdata_owner(view, img, img.image_base) == 0,
               "corpus: pdata_owner maps an address to its function");
    }
    Discoverer disc(view);
    std::map<std::uint64_t, DFunction> fns;
    std::vector<std::uint64_t> work(entries.begin(), entries.end());
    while (!work.empty()) {
        const std::uint64_t e = work.back();
        work.pop_back();
        if (fns.count(e) != 0) {
            continue;
        }
        const auto cont = eh.by_function.find(e);
        DFunction f = disc.discover(e, entries, cont == eh.by_function.end() ? nullptr : &cont->second);
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
            std::printf("  discover %s 0x%llx: %s\n", tag, static_cast<unsigned long long>(va), f.error.c_str());
        }
    }
    expect(complete == fns.size(), "corpus: every function body decodes completely");

    // R3: imports (the corpus calls KERNEL32), and the functions that can catch.
    std::vector<PeImport> imports;
    expect(read_pe_imports(mapped, img.size_of_image, img.image_base, imports, &err), "corpus: the import table reads");
    ImageInfo info;
    info.base = img.image_base;
    for (const PeImport& imp : imports) {
        info.iat_begin = info.iat_begin == 0 ? imp.slot_va : std::min(info.iat_begin, imp.slot_va);
        info.iat_end = std::max(info.iat_end, imp.slot_va + 8);
    }
    for (const PeFunctionRange& r : img.functions) {
        if (((mapped[r.unwind] >> 3) & 0x3) != 0) {  // UNW_FLAG_EHANDLER | UNW_FLAG_UHANDLER
            info.resumable.insert(img.image_base + r.begin);
        }
    }
    R1Translator tr(info);
    expect(tr.init_error().empty(), "corpus: the R1 translator initializes");
    tr.declare(std::vector<std::uint64_t>(entries.begin(), entries.end()));
    for (const auto& [va, f] : fns) {
        std::string why;
        if (tr.lift(f, &why)) {
            out.lifted.insert(va);
        } else {
            std::printf("  lift %s 0x%llx: %s\n", tag, static_cast<unsigned long long>(va), why.c_str());
        }
    }
    const LiftReport rep = tr.report();
    std::printf("  %s coverage: %zu/%zu functions lifted\n", tag, rep.lifted, rep.attempted);
    for (const auto& [what, n] : rep.unsupported) {
        std::printf("    unsupported %-32s %zu\n", what.c_str(), n);
    }
    expect(rep.lifted == fns.size(), "corpus: every corpus function lifts");
    if (is_exe) {
        std::printf("  lifted as run-time faults: %zu traps, %zu AVX instructions\n", rep.traps,
                    rep.gated.count("AVX") != 0 ? rep.gated.at("AVX") : 0);
        expect(rep.traps >= 1 && rep.gated.count("AVX") != 0,
               "corpus: int3 and AVX on cold paths lift as run-time faults (cold_paths)");
    }

    if (const char* name = std::getenv("WSE_R1_DUMP_EXPORT")) {
        for (const Export& e : m.exports) {
            if (e.name == name) {
                setenv("WSE_R1_DUMP_FN", r1_symbol(e.va).c_str(), 1);
            }
        }
    }
    std::vector<char> obj;
    expect(tr.emit_object(2, obj, &out.t, &err) && !obj.empty(), "corpus: emit the O2 object");
    if (obj.empty()) {
        std::printf("  emit %s: %s\n", tag, err.c_str());
        return false;
    }
    out.obj_size = obj.size();
    const std::string o = std::string(work_dir) + "/" + tag + ".o";
    const std::string so = std::string(work_dir) + "/" + tag + ".so";
    std::ofstream(o, std::ios::binary).write(obj.data(), static_cast<std::streamsize>(obj.size()));
    std::string log;
    expect(link_shared_object({o}, so, &log), "corpus: LLD links the translated module");
    out.handle = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    expect(out.handle != nullptr, "corpus: dlopen the translated module");
    if (out.handle == nullptr) {
        std::printf("  link %s: %s dlopen: %s\n", tag, log.c_str(), dlerror());
        return false;
    }
    out.rows = static_cast<const FnRow*>(dlsym(out.handle, "wse_fn_table"));
    const auto* count = static_cast<const std::uint64_t*>(dlsym(out.handle, "wse_fn_table_count"));
    expect(out.rows != nullptr && count != nullptr && *count == out.lifted.size(),
           "corpus: the module exports its dispatch table (defined functions only)");
    if (out.rows == nullptr || count == nullptr) {
        return false;
    }
    out.count = static_cast<std::size_t>(*count);
    return true;
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

// The corpus DLL maps here, not at its preferred 0x180000000, so its base relocations apply (and
// the x86 recording and the arm64 check see the same addresses).
constexpr std::uint64_t kCorpusDllBase = 0x190000000ULL;

void test_corpus(const char* path) {
    if (!kCanRunNative && !g_check_mode) {
        std::printf("  corpus skipped: not an x86-64 host and no --check results\n");
        return;
    }
    Mapped exe;
    expect(map_image(path, 0, exe, nullptr), "corpus: the Win64 PE maps at its base");
    if (exe.mapped == nullptr) {
        return;
    }
    expect(exe.exports.size() >= 30, "corpus: exports are found");
    // R4: the DLL the exe imports, from beside it (`dll_corpus.c`).
    std::string dll_path = path;
    const std::size_t slash = dll_path.find_last_of('/');
    dll_path = (slash == std::string::npos ? std::string() : dll_path.substr(0, slash + 1)) + "corpus_dll.dll";
    Mapped dll;
    std::size_t relocations = 0;
    expect(map_image(dll_path, kCorpusDllBase, dll, &relocations) && relocations >= 4,
           "corpus: corpus_dll.dll maps at another base and its relocations apply");
    if (dll.mapped == nullptr) {
        return;
    }
    char tmpl[] = "/tmp/wse_r1_corpus_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (dir == nullptr) {
        return;
    }
    Translated tx;
    Translated td;
    if (!translate_image("corpus", exe, true, dir, tx) || !translate_image("corpus_dll", dll, false, dir, td)) {
        return;
    }
    const PeImage& img = exe.img;
    std::uint8_t* mapped = exe.mapped;
    const std::vector<Export>& exports = exe.exports;
    std::set<std::uint64_t> lifted = tx.lifted;
    lifted.insert(td.lifted.begin(), td.lifted.end());
    std::string err;

    Runtime rt;
    std::vector<Fault> faults;
    rt.on_fault = [&](const Fault& f) { faults.push_back(f); };
    rt.add_table(tx.rows, tx.count);
    rt.add_table(td.rows, td.count);
    // R4: the module registry names both images' imports and links the exe's corpus_dll imports
    // to the DLL's code.
    shim::Modules modules(rt);
    expect(modules.add_image("C:\\WSE2\\corpus.exe", img, mapped, true, &err) != nullptr &&
               modules.add_image("C:\\WSE2\\corpus_dll.dll", dll.img, dll.mapped, false, &err) != nullptr,
           "corpus: both images register as modules");
    std::vector<std::string> unresolved;
    const std::size_t cross = modules.link(&unresolved);
    expect(cross >= 5 && unresolved.empty(), "corpus: the exe's corpus_dll imports bind to the DLL's exports");
    for (const std::string& u : unresolved) {
        std::printf("  unresolved import %s\n", u.c_str());
    }
    rt.fill_iat();  // imports called through a register (clang hoists `__imp_*` loads out of loops)
    seh::Exceptions exceptions(rt, img, mapped);
    exceptions.add_image(dll.img, dll.mapped);
    const std::size_t shims = exceptions.install();
    expect(shims >= 7, "corpus: the SEH shims bind to the corpus imports (both modules)");
    // R3 thread environment: the main thread's TEB and static TLS; the TLS callbacks run once
    // with DLL_PROCESS_ATTACH, as the loader runs them (`t_tls_callback` checks).
    teb::Process process(rt, img, mapped);
    expect(process.init(&err) && process.tls_directory().present && process.install() >= 11,
           "corpus: the TLS directory reads and the TEB shims bind");
    teb::GuestThread& main_thread = process.create_thread(0, 0);
    // R3 threads and sync: translated entry points on host threads (`t_thread_*`).
    kernel::Kernel kern(rt);
    threads::Threads thr(rt, process, kern);
    const std::size_t kernel_shims = kern.install();
    const std::size_t thread_shims = thr.install();
    expect(kernel_shims >= 9 && thread_shims >= 5, "corpus: the kernel and thread shims bind");
    threadpool::Pool pool(rt, process, kern);  // `t_pool_*`, `t_register_wait`
    expect(pool.install() >= 9, "corpus: the thread-pool shims bind");
    // R4: time, CRT support and system information (`crt_corpus.c`).
    shim::Time time_shims(rt);
    shim::Crt crt_shims(rt, shim::ProcessConfig{});
    const std::size_t time_bound = time_shims.install();
    const std::size_t crt_bound = crt_shims.install();
    expect(time_bound >= 6 && crt_bound >= 8, "corpus: the time and CRT shims bind");
    // R4: strings, locale and memory (`str_corpus.c`).
    shim::Memory memory_shims(rt);
    shim::Strings string_shims(rt, memory_shims);
    const std::size_t memory_bound = memory_shims.install();
    const std::size_t string_bound = string_shims.install();
    expect(memory_bound >= 10 && string_bound >= 8, "corpus: the memory and string shims bind");
    // R4: files (`file_corpus.c`): an empty install and an in-memory writable layer.
    wse::platform::MemoryVfs install_vfs;
    wse::platform::MemoryVfs user_vfs;
    shim::FilesConfig files_cfg;
    files_cfg.install = &install_vfs;
    files_cfg.user = &user_vfs;
    shim::Files file_shims(rt, kern, files_cfg);
    expect(file_shims.install() >= 20, "corpus: the file shims bind");
    // R4: modules and resources (`mod_corpus.c`).
    expect(modules.install() >= 15, "corpus: the module shims bind");
    {
        GuestStack stack;
        CpuState s;
        stack.prepare(s);
        rt.attach(s);
        teb::Process::attach(s, main_thread);
        expect(process.run_tls_callbacks(s, teb::kDllProcessAttach) && process.tls_directory().callbacks.size() == 1,
               "corpus: the TLS callback runs at process attach");
        // The loader's order: DLLs attach before the exe runs; a static load passes lpReserved != NULL.
        std::string failed;
        expect(modules.attach_dlls(s, &failed) && faults.empty(), "corpus: corpus_dll's DllMain runs at process attach");
        std::uint64_t info_va = 0;
        for (const Export& e : dll.exports) {
            info_va = e.name == "dt_attach_info" ? e.va : info_va;
        }
        bool ok = false;
        const std::uint64_t info = rt.call_guest(s, info_va, {}, &ok);
        expect(ok && info == 0x1111, "corpus: DllMain saw its handle, DLL_PROCESS_ATTACH, once, static load");
        if (info != 0x1111) {
            std::printf("  dt_attach_info 0x%llx (%s)\n", static_cast<unsigned long long>(info), failed.c_str());
        }
    }
    std::printf("  translated %zu functions (corpus: lift %.1f ms, opt %.1f ms, codegen %.1f ms, %zu B; "
                "corpus_dll: %zu B)\n",
                rt.function_count(), tx.t.lift_ms, tx.t.optimize_ms, tx.t.codegen_ms, tx.obj_size, td.obj_size);

    constexpr int kRuns = 200;
    constexpr int kCalleeSaved[] = {3, 5, 6, 7, 12, 13, 14, 15};
    alignas(16) static std::uint8_t buf_native[4096];
    alignas(16) static std::uint8_t buf_trans[4096];
    Rng rng;
    std::size_t checked = 0;
    std::size_t runnable = 0;
    std::FILE* record = g_record_path != nullptr ? std::fopen(g_record_path, "w") : nullptr;
    // The exe's exports, then the DLL's `d_*` (pure code at the relocated base; its `dt_*` run only
    // inside the exe's `t_mod_*` tests).
    std::vector<Export> runs = exports;
    for (const Export& e : dll.exports) {
        if (e.name.rfind("d_", 0) == 0) {
            runs.push_back(e);
        }
    }
    for (const Export& e : runs) {
        // `seh_*` / `teb_*` / `mod_*` are the R3/R4 corpora's handlers, callbacks and helpers: they
        // only run inside `t_*` tests.
        if (e.name.rfind("seh_", 0) == 0 || e.name.rfind("teb_", 0) == 0 || e.name.rfind("mod_", 0) == 0) {
            continue;
        }
        ++runnable;
        if (lifted.count(e.va) == 0) {
            continue;
        }
        const bool is_mem = e.name.rfind("m_", 0) == 0;
        // `t_*` call KERNEL32 (no native run on Linux): self-checking, 0x600D means correct.
        const bool self_check = e.name.rfind("t_", 0) == 0;
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
            std::uint64_t want = self_check ? 0x600D : 0;
            if (!g_check_mode && !self_check) {
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
            teb::Process::attach(s, main_thread);
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
            if (g_check_mode && !self_check) {
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
            bool ok = ran && faults.empty() && ((g_check_mode && !self_check) || s.gpr[0] == want) &&
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
    std::printf("  differential: %zu/%zu exports match %s on %d inputs each\n", checked, runnable,
                g_check_mode ? "the recorded x86 results" : "native", kRuns);
    expect(checked == runnable, "corpus: every export matches native (rax, memory, callee-saved, rsp)");
    dlclose(td.handle);
    dlclose(tx.handle);
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
