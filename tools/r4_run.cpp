// R4 bring-up run (`docs/RECOMP_PLAN.md` "R4 design", "Bring-up tool"). LOCAL ONLY: it reads the
// user's own install and runs retail code translated on this machine; never run in CI, and
// nothing it reads or writes is committed. Linux x86-64 (WSL, `tools/wsl_gate.sh` builds it).
//
//   r4_run <install dir> [--work <dir>] [--retranslate] [--batch <n>] [--opt <0-3>] [--jobs <n>] [--reuse] [--quiet]
//
// Translates `mb_warband_wse2_x64.exe` (entries from its PDB and `.pdata`) and `lua5164.dll`
// (entries from `.pdata`, exports and its entry point) in batches of shared objects under
// `<work>/so` (on `--jobs` threads, default 2), each reused while the image, the options and the translator library are unchanged; maps both images at their
// preferred bases; links them through `shim::Modules`; binds every shim; then on the main guest
// thread runs the TLS callbacks, each DLL's DllMain and the exe's entry point. Every import's first
// call is logged in call order. `--reuse` keeps every batch already built whatever its key, for
// when only a non-lifter file of the translator library changed (`r0_spikes.cpp` includes
// `runtime.h`, so a shim change relinks the library and moves the key). A fault (a missing import names its DLL and function: the next
// thing to implement) ends the run with the fault and the last imports called.

#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../engine/src/recomp/discover.h"
#include "../engine/src/recomp/lift_r1.h"
#include "../engine/src/recomp/recomp_llvm.h"
#include "wse/platform/vfs.h"
#include "wse/recomp/kernel.h"
#include "wse/recomp/pdb_reader.h"
#include "wse/recomp/pe_image.h"
#include "wse/recomp/r0_inputs.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/seh.h"
#include "wse/recomp/shim_advapi.h"
#include "wse/recomp/shim_crt.h"
#include "wse/recomp/shim_d3d9.h"
#include "wse/recomp/shim_d3dx.h"
#include "wse/recomp/shim_files.h"
#include "wse/recomp/shim_fmod_steam.h"
#include "wse/recomp/shim_memory.h"
#include "wse/recomp/shim_modules.h"
#include "wse/recomp/shim_net.h"
#include "wse/recomp/shim_strings.h"
#include "wse/recomp/shim_system.h"
#include "wse/recomp/shim_time.h"
#include "wse/recomp/shim_user32.h"
#include "wse/recomp/teb.h"
#include "wse/recomp/threadpool.h"
#include "wse/recomp/threads.h"

using namespace wse::recomp;
namespace fs = std::filesystem;

namespace {

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::vector<std::uint8_t> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A file in `dir` whose name matches `name` case-insensitively (Windows installs mix cases).
fs::path find_ci(const fs::path& dir, const std::string& name) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string a = e.path().filename().string();
        std::string b = name;
        for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (a == b) return e.path();
    }
    return dir / name;
}

std::uint64_t fnv64(const std::vector<std::uint8_t>& b) {
    std::uint64_t h = 0xCBF29CE484222325ULL;
    for (const std::uint8_t x : b) {
        h = (h ^ x) * 0x100000001B3ULL;
    }
    return h;
}

// What a translated batch depends on: the image bytes, the options, and the translator library
// (`WSE_RECOMP_LIB`, its path at build time). Rebuilding r4_run for a new shim keeps the cache.
std::string cache_key(const std::vector<std::uint8_t>& file, int opt, std::size_t batch) {
    struct stat st {};
#ifdef WSE_RECOMP_LIB
    (void)stat(WSE_RECOMP_LIB, &st);
#endif
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%016llx %zu opt%d batch%zu lifter %lld/%lld", static_cast<unsigned long long>(fnv64(file)),
                  file.size(), opt, batch, static_cast<long long>(st.st_size), static_cast<long long>(st.st_mtime));
    return buf;
}

struct Image {
    std::string tag;
    std::vector<std::uint8_t> file;
    PeImage img;
    std::uint8_t* mapped = nullptr;
};

bool map_image(Image& m, std::string* err) {
    if (m.file.empty() || !parse_pe(m.file.data(), m.file.size(), m.img, err)) {
        if (m.file.empty()) *err = "missing";
        return false;
    }
    const std::size_t size = (m.img.size_of_image + 0xFFFU) & ~std::size_t{0xFFF};
    void* at = mmap(reinterpret_cast<void*>(m.img.image_base), size, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (at != reinterpret_cast<void*>(m.img.image_base)) {
        *err = "its preferred base is taken";
        return false;
    }
    m.mapped = static_cast<std::uint8_t*>(at);
    std::size_t relocs = 0;
    return map_pe_sections(m.file.data(), m.file.size(), m.img, m.mapped) &&
           apply_pe_relocations(m.mapped, m.img.size_of_image, m.img.image_base, &relocs, err);
}

// Entries of one image: PDB publics and `.pdata` (collect_function_entries), the exports and the
// entry point, then direct call targets of those to a fixed point.
std::set<std::uint64_t> entries_of(const Image& m, const ImageView& view, const std::vector<PdbPublic>& publics) {
    std::set<std::uint64_t> entries = collect_function_entries(view, m.img, m.mapped, publics);
    std::deque<std::uint64_t> work;
    PeExports ex;
    std::string err;
    if (read_pe_exports(m.mapped, m.img.size_of_image, ex, &err)) {
        for (const PeExport& e : ex.entries) {
            const std::uint64_t va = m.img.image_base + e.rva;
            if (e.forwarder.empty() && view.in_text(va) && entries.insert(va).second) work.push_back(va);
        }
    }
    if (m.img.entry_rva != 0 && entries.insert(m.img.image_base + m.img.entry_rva).second) {
        work.push_back(m.img.image_base + m.img.entry_rva);
    }
    // Code reached only through a stored pointer (CRT init tables, callbacks, vtables; leaf
    // functions have no .pdata entry, and the PDB's publics miss static functions such as
    // dynamic initializers): every relocated pointer into .text.
    {
        std::size_t added = 0;
        for (const std::uint64_t v : read_pe_dir64_values(m.mapped, m.img.size_of_image)) {
            if (view.in_text(v) && entries.insert(v).second) {
                work.push_back(v);
                ++added;
            }
        }
        std::printf("%s: %zu entries from relocated pointers into .text\n", m.tag.c_str(), added);
    }
    // Code reached only through a run-time table of 16-bit offsets (LuaJIT's bytecode handlers).
    const OffsetTables ot = find_offset_table_entries(view, m.img, entries);
    for (const OffsetTables::Table& t : ot.tables) {
        std::printf("%s: offset table at 0x%llx, %zu entries from 0x%llx\n", m.tag.c_str(), static_cast<unsigned long long>(t.table),
                    t.count, static_cast<unsigned long long>(t.function));
    }
    for (const std::uint64_t e : ot.entries) {
        if (entries.insert(e).second) work.push_back(e);
    }
    Discoverer disc(view);
    while (!work.empty()) {
        const std::uint64_t e = work.front();
        work.pop_front();
        for (const std::uint64_t t : disc.discover(e, entries).call_targets) {
            if (view.in_text(t) && entries.insert(t).second) work.push_back(t);
        }
    }
    // Code whose address a function takes with a rip-relative `lea` (callbacks, tables filled at
    // run time) and that no .pdata range covers.
    const std::set<std::uint64_t> taken = find_code_address_entries(view, m.img, entries);
    entries.insert(taken.begin(), taken.end());
    std::printf("%s: %zu entries from code addresses taken with lea\n", m.tag.c_str(), taken.size());
    return entries;
}

struct Loaded {
    std::vector<void*> handles;
    std::vector<std::pair<const FnRow*, std::size_t>> tables;
    std::size_t functions = 0;
    std::size_t not_lifted = 0;
};

// Translates `m` in batches on `jobs` worker threads (a batch whose `.so` and key file match is
// reused, so an interrupted run resumes) and loads every batch.
bool translate(Image& m, const std::vector<PdbPublic>& publics, const fs::path& so_dir, bool retranslate, bool reuse,
               std::size_t batch, int opt, unsigned jobs, Loaded& out) {
    const auto t0 = std::chrono::steady_clock::now();
    const auto [text_b, text_e] = text_range(m.img);
    ImageView view(m.mapped, m.img.image_base, m.img.size_of_image, text_b, text_e);
    const std::string key = cache_key(m.file, opt, batch);
    const std::set<std::uint64_t> entries = entries_of(m, view, publics);
    const EhContinuations eh = find_eh_continuations(view, m.img, entries);
    std::vector<PeImport> imports;
    std::string err;
    (void)read_pe_imports(m.mapped, m.img.size_of_image, m.img.image_base, imports, &err);
    ImageInfo info;
    info.base = m.img.image_base;
    for (const PeImport& imp : imports) {
        info.iat_begin = info.iat_begin == 0 ? imp.slot_va : std::min(info.iat_begin, imp.slot_va);
        info.iat_end = std::max(info.iat_end, imp.slot_va + 8);
    }
    for (const PeFunctionRange& r : m.img.functions) {
        const std::uint8_t f = r.unwind < m.img.size_of_image ? static_cast<std::uint8_t>(m.mapped[r.unwind] >> 3) : 0;
        if ((f & 0x3) != 0 && (f & 0x4) == 0) info.resumable.insert(m.img.image_base + r.begin);
    }
    const std::vector<std::uint64_t> all(entries.begin(), entries.end());
    // Batches are address windows of `batch` KiB of .text (named by window), so a new entry only
    // changes its own window's batch; empty windows have no batch.
    const std::uint64_t window = static_cast<std::uint64_t>(batch) * 1024;
    std::map<std::uint64_t, std::pair<std::size_t, std::size_t>> windows;  // window -> [first, end) in `all`
    for (std::size_t i = 0; i < all.size(); ++i) {
        const std::uint64_t w = (all[i] - text_b) / window;
        auto [it, fresh] = windows.try_emplace(w, i, i + 1);
        if (!fresh) it->second.second = i + 1;
    }
    std::vector<std::uint64_t> batch_window;
    std::vector<std::pair<std::size_t, std::size_t>> batch_range;
    for (const auto& [w, r] : windows) {
        batch_window.push_back(w);
        batch_range.push_back(r);
    }
    const std::size_t n_batches = batch_window.size();
    std::vector<fs::path> sos(n_batches);
    std::vector<std::size_t> todo;
    // How each function was discovered (jump tables, their targets, unresolved jumps): a batch's
    // translation depends on it as much as on which functions the batch holds.
    std::map<std::uint64_t, std::array<std::size_t, 4>> sig;  // tables, targets, unresolved, function-bounded tables
    {
        Discoverer d0(view);
        d0.set_function_ranges(&m.img.functions);
        for (const std::uint64_t e : all) {
            const auto cont = eh.by_function.find(e);
            const DFunction f = d0.discover(e, entries, cont == eh.by_function.end() ? nullptr : &cont->second);
            std::array<std::size_t, 4> v{f.jump_tables.size(), 0, f.unresolved_indirect_jumps.size(), 0};
            for (const JumpTable& jt : f.jump_tables) {
                v[1] += jt.targets.size();
                v[3] += jt.by_function_range ? 1 : 0;
            }
            sig[e] = v;
        }
    }
    // Each batch's key file holds the key, then a hash of the batch's functions and how they were
    // discovered: a batch is reused only when both match (`--reuse` waives the key, never the
    // functions). A key file without the second line (written before it existed) is kept unless the
    // batch has a jump table bounded by its function (`Discoverer::set_function_ranges`), and its
    // hash line is written then.
    auto batch_hash = [&](std::size_t b) {
        std::uint64_t h = 0xCBF29CE484222325ULL;
        for (std::size_t i = batch_range[b].first; i < batch_range[b].second; ++i) {
            h = (h ^ all[i]) * 0x100000001B3ULL;
            for (const std::size_t x : sig[all[i]]) h = (h ^ x) * 0x100000001B3ULL;
        }
        return std::to_string(h);
    };
    // The first hash line (2026-11-20) covered the function list only.
    auto list_hash = [&](std::size_t b) {
        std::uint64_t h = 0xCBF29CE484222325ULL;
        for (std::size_t i = batch_range[b].first; i < batch_range[b].second; ++i) h = (h ^ all[i]) * 0x100000001B3ULL;
        return std::to_string(h);
    };
    auto legacy_ok = [&](std::size_t b) {
        for (std::size_t i = batch_range[b].first; i < batch_range[b].second; ++i) {
            if (sig[all[i]][3] != 0) return false;
        }
        return true;
    };
    for (std::size_t b = 0; b < n_batches; ++b) {
        sos[b] = so_dir / (m.tag + "_w" + std::to_string(batch_window[b]) + ".so");
        const fs::path key_file = so_dir / (m.tag + "_w" + std::to_string(batch_window[b]) + ".key");
        std::ifstream in(key_file);
        std::string line;
        std::string fns;
        const bool have = std::getline(in, line) && fs::exists(sos[b]);
        const bool legacy = have && (!std::getline(in, fns) || fns == list_hash(b));
        const bool same_fns = legacy ? legacy_ok(b) : fns == batch_hash(b);
        if (retranslate || !have || !same_fns || (line != key && !reuse)) {
            todo.push_back(b);
        } else if (legacy) {
            in.close();
            std::ofstream(key_file) << line << "\n" << batch_hash(b) << "\n";
        }
    }
    std::printf("%s: %zu functions, %zu resumable, catch continuations in %zu functions; %zu batches (%zu KiB windows), %zu to translate on %u threads\n",
                m.tag.c_str(), entries.size(), info.resumable.size(), eh.by_function.size(), n_batches, batch, todo.size(), jobs);
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> not_lifted{0};
    std::atomic<bool> failed{false};
    std::mutex print_m;
    auto worker = [&]() {
        Discoverer disc(view);
        disc.set_function_ranges(&m.img.functions);
        for (std::size_t i = next++; i < todo.size() && !failed; i = next++) {
            const std::size_t b = todo[i];
            const std::vector<std::uint64_t> vas(all.begin() + static_cast<std::ptrdiff_t>(batch_range[b].first),
                                                 all.begin() + static_cast<std::ptrdiff_t>(batch_range[b].second));
            R1Translator tr(info);
            std::string why;
            std::string e = tr.init_error();
            tr.declare(vas);
            for (const std::uint64_t va : vas) {
                const auto cont = eh.by_function.find(va);
                if (e.empty() && !tr.lift(disc.discover(va, entries, cont == eh.by_function.end() ? nullptr : &cont->second), &why)) {
                    ++not_lifted;
                }
            }
            std::vector<char> obj;
            std::string log;
            const fs::path o = so_dir / (m.tag + "_w" + std::to_string(batch_window[b]) + ".o");
            if (e.empty() && !tr.emit_object(opt, obj, nullptr, &e) && e.empty()) e = "emit failed";
            if (e.empty()) {
                std::ofstream(o, std::ios::binary).write(obj.data(), static_cast<std::streamsize>(obj.size()));
                if (!link_shared_object({o.string()}, sos[b].string(), &log)) e = "link failed: " + log;
                fs::remove(o);
            }
            const std::lock_guard<std::mutex> lock(print_m);
            if (!e.empty()) {
                std::printf("%s batch %zu: %s\n", m.tag.c_str(), b, e.c_str());
                failed = true;
                return;
            }
            std::ofstream(so_dir / (m.tag + "_w" + std::to_string(batch_window[b]) + ".key")) << key << "\n" << batch_hash(b) << "\n";
            std::printf("  %s batch %zu done (%zu/%zu, %.0f s)\n", m.tag.c_str(), b, ++done, todo.size(), ms_since(t0) / 1000.0);
        }
    };
    std::vector<std::thread> pool;
    for (unsigned j = 0; j < jobs; ++j) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
    if (failed) return false;
    out.not_lifted += not_lifted;
    for (const fs::path& so : sos) {
        void* h = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
        const auto* rows = h != nullptr ? static_cast<const FnRow*>(dlsym(h, "wse_fn_table")) : nullptr;
        const auto* count = h != nullptr ? static_cast<const std::uint64_t*>(dlsym(h, "wse_fn_table_count")) : nullptr;
        if (rows == nullptr || count == nullptr) {
            std::printf("%s: cannot load %s: %s (rerun with --retranslate)\n", m.tag.c_str(), so.c_str(), dlerror());
            return false;
        }
        out.handles.push_back(h);
        out.tables.emplace_back(rows, static_cast<std::size_t>(*count));
        out.functions += static_cast<std::size_t>(*count);
    }
    std::printf("%s: %zu translated functions loaded from %zu objects (%.1f s)\n", m.tag.c_str(), out.functions,
                sos.size(), ms_since(t0) / 1000.0);
    return true;
}

// Import tracing: first calls in order, and the last few calls of the run.
std::mutex g_trace_m;
std::set<std::uint64_t> g_seen;
std::vector<std::uint64_t> g_first;
std::deque<std::uint64_t> g_recent;
std::atomic<std::uint64_t> g_calls{0};
Runtime* g_rt = nullptr;
bool g_quiet = false;
std::string g_last_raise;  // the last RaiseException's report (under g_trace_m)

void print_recent() {
    std::printf("last imports called (oldest first):\n");
    for (const std::uint64_t slot : g_recent) {
        std::printf("  %s\n", g_rt->import_name(slot).c_str());
    }
    std::printf("%llu import calls, %zu distinct\n", static_cast<unsigned long long>(g_calls.load()), g_first.size());
}

void on_signal(int sig) {
    std::printf("\nhost signal %d in translated code or a shim\n", sig);
    print_recent();
    std::fflush(stdout);
    std::_Exit(3);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <install dir> [--work <dir>] [--retranslate] [--batch <n>] [--opt <0-3>] [--jobs <n>] [--reuse] [--quiet]\n",
                     argv[0]);
        return 2;
    }
    const fs::path install = argv[1];
    fs::path work = "/root/wse/r4";
    bool retranslate = false;
    bool reuse = false;
    std::size_t batch = 64;  // KiB of .text per translated object
    int opt = 2;
    unsigned jobs = 2;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--work" && i + 1 < argc) work = argv[++i];
        else if (a == "--retranslate") retranslate = true;
        else if (a == "--batch" && i + 1 < argc) batch = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--opt" && i + 1 < argc) opt = std::atoi(argv[++i]);
        else if (a == "--jobs" && i + 1 < argc) jobs = static_cast<unsigned>(std::max(1, std::atoi(argv[++i])));
        else if (a == "--reuse") reuse = true;
        else if (a == "--quiet") g_quiet = true;
    }
    fs::create_directories(work / "so");
    fs::create_directories(work / "user");
    setvbuf(stdout, nullptr, _IOLBF, 0);

    Image exe{"exe", read_file(find_ci(install, "mb_warband_wse2_x64.exe")), {}, nullptr};
    Image lua{"lua5164", read_file(find_ci(install, "lua5164.dll")), {}, nullptr};
    const std::vector<std::uint8_t> pdb = read_file(find_ci(install, "mb_warband_wse2_x64.pdb"));
    std::string err;
    if (!map_image(exe, &err)) {
        std::printf("exe: %s\n", err.c_str());
        return 1;
    }
    if (!map_image(lua, &err)) {
        std::printf("lua5164.dll: %s\n", err.c_str());
        return 1;
    }
    std::vector<PdbPublic> publics;
    PdbGuidAge ga;
    PeCodeView cv;
    if (!read_pe_codeview(exe.file.data(), exe.file.size(), exe.img, cv, &err) ||
        !read_pdb_publics(pdb.data(), pdb.size(), publics, &ga, &err) || ga.guid != cv.guid) {
        std::printf("PDB: %s\n", err.empty() ? "does not belong to this exe" : err.c_str());
        return 1;
    }
    std::printf("exe at 0x%llx (%u bytes), lua5164 at 0x%llx, PDB matches (%zu publics)\n",
                static_cast<unsigned long long>(exe.img.image_base), exe.img.size_of_image,
                static_cast<unsigned long long>(lua.img.image_base), publics.size());

    Loaded lx;
    Loaded ll;
    if (!translate(exe, publics, work / "so", retranslate, reuse, batch, opt, jobs, lx) ||
        !translate(lua, {}, work / "so", retranslate, reuse, batch, opt, jobs, ll)) {
        return 1;
    }

    Runtime rt;
    g_rt = &rt;
    std::atomic<bool> faulted{false};
    rt.on_fault = [&](const Fault& f) {
        if (faulted.exchange(true)) return;
        std::printf("\nFAULT: %s at 0x%llx (rip 0x%llx)%s%s\n", fault_name(f.kind), static_cast<unsigned long long>(f.va),
                    static_cast<unsigned long long>(f.rip), f.what.empty() ? "" : ": ", f.what.c_str());
        const std::lock_guard<std::mutex> lock(g_trace_m);
        if (f.what == "unhandled guest exception" && !g_last_raise.empty()) {
            std::printf("%s", g_last_raise.c_str());
        }
        print_recent();
        std::fflush(stdout);
        std::_Exit(2);
    };
    rt.trace_import = [&](std::uint64_t slot) {
        ++g_calls;
        const std::lock_guard<std::mutex> lock(g_trace_m);
        if (g_seen.insert(slot).second) {
            g_first.push_back(slot);
            if (!g_quiet) std::printf("  import #%zu %s\n", g_first.size(), rt.import_name(slot).c_str());
        }
        g_recent.push_back(slot);
        if (g_recent.size() > 12) g_recent.pop_front();
    };
    for (const auto& [rows, n] : lx.tables) rt.add_table(rows, n);
    for (const auto& [rows, n] : ll.tables) rt.add_table(rows, n);

    shim::Modules modules(rt);
    if (modules.add_image("C:\\WSE2\\mb_warband_wse2_x64.exe", exe.img, exe.mapped, true, &err) == nullptr ||
        modules.add_image("C:\\WSE2\\lua5164.dll", lua.img, lua.mapped, false, &err) == nullptr) {
        std::printf("modules: %s\n", err.c_str());
        return 1;
    }
    std::vector<std::string> unresolved;
    const std::size_t cross = modules.link(&unresolved);
    std::printf("linked %zu imports across modules%s\n", cross, unresolved.empty() ? "" : "; unresolved:");
    for (const std::string& u : unresolved) std::printf("  %s\n", u.c_str());
    rt.fill_iat();
    // LuaJIT's JIT compiles Lua to x86 code at run time, which nothing here can run (and translating
    // during play is not allowed): every state the exe creates gets luaJIT_setmode(L, 0,
    // LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF) and runs in the interpreter.
    if (shim::Module* lm = modules.find("lua5164.dll", false)) {
        const std::uint64_t newstate = modules.proc_address(*lm, "luaL_newstate", 0, 0);
        const std::uint64_t setmode = modules.proc_address(*lm, "luaJIT_setmode", 0, 0);
        std::size_t wrapped = 0;
        for (const PeImport& imp : rt.import_list()) {
            if (imp.name == "luaL_newstate" && imp.slot_va >= exe.img.image_base &&
                imp.slot_va < exe.img.image_base + exe.img.size_of_image && newstate != 0 && setmode != 0) {
                rt.set_import(imp.slot_va, [&rt, newstate, setmode](CpuState& s) {
                    bool ok = false;
                    const std::uint64_t L = rt.call_guest(s, newstate, {}, &ok);
                    if (s.unwind_rsp != 0) return;
                    if (L != 0) (void)rt.call_guest(s, setmode, {L, 0, 0}, &ok);
                    if (s.unwind_rsp == 0) win64_return(s, L);
                });
                ++wrapped;
            }
        }
        std::printf("LuaJIT: the JIT is switched off in every new state (%zu luaL_newstate imports wrapped)\n", wrapped);
    }

    seh::Exceptions exceptions(rt, exe.img, exe.mapped);
    exceptions.add_image(lua.img, lua.mapped);
    teb::Process process(rt, exe.img, exe.mapped);
    if (!process.init(&err)) {
        std::printf("TEB/TLS: %s\n", err.c_str());
        return 1;
    }
    kernel::Kernel kern(rt);
    threads::Threads thr(rt, process, kern);
    threadpool::Pool pool(rt, process, kern);
    shim::Time time_shims(rt);
    shim::Crt crt(rt, shim::ProcessConfig{});
    crt.on_exit = [](std::uint32_t code) {
        std::printf("\nExitProcess(%u)\n", code);
        print_recent();
        std::fflush(stdout);
        std::_Exit(static_cast<int>(code & 0xFF));
    };
    crt.on_debug_string = [](const std::string& text) { std::printf("  debug: %s", text.c_str()); };
    shim::Memory memory(rt);
    shim::Strings strings(rt, memory);
    wse::platform::HostVfs install_vfs(install);
    wse::platform::HostVfs user_vfs(work / "user");
    shim::FilesConfig files_cfg;
    files_cfg.install = &install_vfs;
    files_cfg.user = &user_vfs;
    shim::Files files(rt, kern, files_cfg);
    shim::Advapi advapi(rt, &user_vfs);
    shim::User32 user32(rt);
    user32.on_message_box = [](const std::string& caption, const std::string& text) {
        std::printf("  MessageBox [%s] %s\n", caption.c_str(), text.c_str());
    };
    shim::Net net(rt);
    shim::D3dxMath d3dx(rt);
    shim::D3d9 d3d9(rt);
    shim::FmodSteam fmod_steam(rt);
    // D3DX reads texture headers: a guest path in the install ("C:\WSE2\..." or relative).
    d3d9.read_file = [&](const std::string& path, std::vector<std::uint8_t>& bytes) {
        std::string p = path;
        static const std::string kRoot = "c:\\wse2\\";
        std::string low = p;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (low.rfind(kRoot, 0) == 0) p = p.substr(kRoot.size());
        for (char& c : p) c = c == '\\' ? '/' : c;
        return install_vfs.read_file(p, bytes) || user_vfs.read_file(p, bytes);
    };
    d3d9.on_first_call = [](const std::string& m) {
        if (!g_quiet) std::printf("  recorded %s\n", m.c_str());
    };
    shim::SystemDlls sysdlls(rt);
    sysdlls.on_launch = [](const std::string& what) { std::printf("  launch refused: %s\n", what.c_str()); };
    const std::size_t bound = exceptions.install() + process.install() + kern.install() + thr.install() + pool.install() +
                              time_shims.install() + crt.install() + memory.install() + strings.install() +
                              files.install() + modules.install() + advapi.install() + user32.install() + net.install() +
                              sysdlls.install() + d3dx.install() + d3d9.install() + fmod_steam.install();
    std::size_t total = 0;
    std::size_t missing = 0;
    std::map<std::string, std::vector<std::string>> missing_by_dll;
    for (const PeImport& imp : rt.import_list()) {
        ++total;
        const std::string key = imp.name.empty() ? "#" + std::to_string(imp.ordinal) : imp.name;
        if (rt.named_shim(imp.dll, key) == nullptr && imp.dll != "lua5164.dll") {
            ++missing;
            missing_by_dll[imp.dll].push_back(imp.name.empty() ? "#" + std::to_string(imp.ordinal) : imp.name);
        }
    }
    std::printf("%zu import slots bound to shims (%zu slots, %zu with no shim yet in %zu DLLs)\n", bound, total, missing,
                missing_by_dll.size());
    if (missing <= 32) {
        for (const auto& [dll, names] : missing_by_dll) {
            for (const std::string& n : names) std::printf("  no shim: %s!%s\n", dll.c_str(), n.c_str());
        }
    }

    // Diagnostics for a guest exception nobody handles: RaiseException records its code and
    // parameters, the exe return addresses on the guest stack (named by the nearest PDB public),
    // and the printable strings in the raising frames; on_fault prints them.
    std::uint64_t main_stack_top = 0;  // set once the main stack is prepared
    std::map<std::uint64_t, std::string> names;
    for (const PdbPublic& p : publics) {
        if (p.section >= 1 && p.section <= exe.img.sections.size()) {
            names[exe.img.image_base + exe.img.sections[p.section - 1].virtual_address + p.offset] = p.name;
        }
    }
    const std::uint64_t exe_text_b = text_range(exe.img).first;
    const std::uint64_t exe_text_e = text_range(exe.img).second;
    auto name_of = [&](std::uint64_t va) {
        auto it = names.upper_bound(va);
        if (it == names.begin()) return std::string("?");
        --it;
        char off[24];
        std::snprintf(off, sizeof(off), "+0x%llx", static_cast<unsigned long long>(va - it->first));
        return it->first == va ? it->second : it->second + off;
    };
    if (const Runtime::ImportShim* raise = rt.named_shim("KERNEL32.dll", "RaiseException")) {
        const Runtime::ImportShim original = *raise;
        rt.set_import("KERNEL32.dll", "RaiseException", [&, original](CpuState& st) {
            std::string r;
            char line[512];
            const std::uint64_t args = win64_arg(st, 3);
            const auto nargs = static_cast<std::uint32_t>(win64_arg(st, 2));
            std::snprintf(line, sizeof(line), "last RaiseException: code 0x%08llx flags 0x%llx, %u parameters",
                          static_cast<unsigned long long>(win64_arg(st, 0) & 0xFFFFFFFFULL),
                          static_cast<unsigned long long>(win64_arg(st, 1)), nargs);
            r += line;
            for (std::uint32_t i = 0; i < nargs && i < 4 && args != 0; ++i) {
                std::uint64_t v = 0;
                std::memcpy(&v, reinterpret_cast<const void*>(args + 8ULL * i), 8);
                std::snprintf(line, sizeof(line), " 0x%llx", static_cast<unsigned long long>(v));
                r += line;
            }
            r += "\n  guest stack (exe return addresses):\n";
            const std::uint64_t rsp = st.gpr[4];
            // The scan stays inside the thread's stack: the TEB's StackBase (guest threads), else
            // the main stack's top; with neither above rsp, nothing is read.
            std::uint64_t top = 0;
            if (st.teb != 0) std::memcpy(&top, reinterpret_cast<const void*>(st.teb + teb::tebo::stack_base), 8);
            if (top <= rsp) top = main_stack_top > rsp ? main_stack_top : rsp;
            int shown = 0;
            for (std::uint64_t q = rsp; q + 8 <= top && q < rsp + 0x8000 && shown < 24; q += 8) {
                std::uint64_t v = 0;
                std::memcpy(&v, reinterpret_cast<const void*>(q), 8);
                if (v < exe_text_b + 6 || v >= exe_text_e) continue;
                const std::uint8_t* c = exe.mapped + (v - exe.img.image_base);
                const bool after_call = c[-5] == 0xE8 || (c[-6] == 0xFF && (c[-5] & 0x38) == 0x10) ||
                                        (c[-2] == 0xFF && (c[-1] & 0x38) == 0x10) || (c[-3] == 0xFF && (c[-2] & 0x38) == 0x10);
                if (!after_call) continue;
                std::snprintf(line, sizeof(line), "    [rsp+0x%llx] 0x%llx %s\n", static_cast<unsigned long long>(q - rsp),
                              static_cast<unsigned long long>(v), name_of(v).c_str());
                r += line;
                ++shown;
            }
            r += "  strings in the raising frames:\n";
            std::string run;
            int strings = 0;
            for (std::uint64_t q = rsp; q < top && q < rsp + 0x4000 && strings < 12; ++q) {
                const char ch = *reinterpret_cast<const char*>(q);
                if (ch >= 0x20 && ch < 0x7F) {
                    run += ch;
                    continue;
                }
                if (run.size() >= 12) {
                    r += "    \"" + run.substr(0, 300) + "\"\n";
                    ++strings;
                }
                run.clear();
            }
            {
                const std::lock_guard<std::mutex> lock(g_trace_m);
                g_last_raise = r;
            }
            original(st);
        });
    }

    for (const int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) signal(sig, &on_signal);
    GuestStack stack(8 * 1024 * 1024);
    CpuState s;
    stack.prepare(s);
    main_stack_top = s.gpr[4] + 8;  // prepare leaves rsp 8 below the aligned top
    rt.attach(s);
    teb::GuestThread& main_thread = process.create_thread(0, 0);
    teb::Process::attach(s, main_thread);
    std::printf("\n-- TLS callbacks, DllMain, entry point --\n");
    if (!process.run_tls_callbacks(s, teb::kDllProcessAttach)) std::printf("TLS callbacks failed\n");
    std::string failed;
    if (!modules.attach_dlls(s, &failed)) {
        std::printf("DllMain of %s failed\n", failed.c_str());
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    // Heartbeat: a steady-state loop prints no first calls, so every 20 s report the call count,
    // the frames presented and the latest imports (a stall shows what it waits on).
    std::thread([&]() {
        std::uint64_t last_calls = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(20));
            const std::lock_guard<std::mutex> lock(g_trace_m);
            const std::uint64_t calls = g_calls.load();
            std::printf("\n[%.0f s] %llu import calls (+%llu), %llu frames presented; latest:", ms_since(t0) / 1000.0,
                        static_cast<unsigned long long>(calls), static_cast<unsigned long long>(calls - last_calls),
                        static_cast<unsigned long long>(d3d9.frames()));
            for (const std::uint64_t slot : g_recent) std::printf(" %s", g_rt->import_name(slot).c_str());
            std::printf("\n");
            std::fflush(stdout);
            last_calls = calls;
        }
    }).detach();
    const bool ran = rt.run(s, exe.img.image_base + exe.img.entry_rva);
    std::printf("\nentry point returned (%s) rax 0x%llx after %.1f s, %llu frames presented\n", ran ? "ok" : "fault",
                static_cast<unsigned long long>(s.gpr[0]), ms_since(t0) / 1000.0, static_cast<unsigned long long>(d3d9.frames()));
    print_recent();
    return 0;
}
