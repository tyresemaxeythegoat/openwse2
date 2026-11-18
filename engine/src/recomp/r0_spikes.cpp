// R0 feasibility spikes. See `r0_spikes.h` and `docs/RECOMP_PLAN.md` ("R0", "Risks").
//
// (a) tries three ways to run code generated in the app, in order, and reports each:
//   1. `dlopen` of the linked `.so` written to app storage and made read-only (the plan's
//      default; Android's W^X rules may forbid mapping app-data files executable),
//   2. `dlopen` from a `memfd` (`android_dlopen_ext` + `ANDROID_DLEXT_USE_LIBRARY_FD`),
//   3. our own ELF loader: segments copied into anonymous memory, `R_*_RELATIVE` relocations,
//      then `mprotect` to read+execute (the plan's fallback).
// Later steps load their code with the first method that worked.

#include "r0_spikes.h"

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <android/dlext.h>
#endif

#include "recomp_llvm.h"
#include "wse/recomp/pdb_reader.h"
#include "discover.h"
#include "lift_r1.h"
#include "wse/recomp/pe_image.h"
#include "wse/recomp/r0_inputs.h"
#include "wse/recomp/x86_interp.h"
#include "wse/recomp/x86_synth.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace wse::recomp {
namespace {

constexpr std::uint32_t kRelocAArch64Relative = 1027;
constexpr std::uint32_t kRelocX8664Relative = 8;
// SizeOfImage of the pinned exe (`tools/r0_pc_reference.cpp` output), used for (b) when the
// exe is not available.
constexpr std::uint32_t kV1155SizeOfImage = 8232960;
constexpr std::uint64_t kSyntheticVa = 0x1000ULL;

// Each line also goes to `<work_dir>/r0/report.txt` at once, so a run that crashes inside
// translated code still leaves the report up to that point (the launcher shows it next time).
class Report {
public:
    explicit Report(const std::string& path) : file_(std::fopen(path.c_str(), "w")) {}
    ~Report() {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
    }
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;

    [[gnu::format(printf, 2, 3)]] void line(const char* fmt, ...) {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        text_ += buf;
        text_ += '\n';
        if (file_ != nullptr) {
            std::fputs(buf, file_);
            std::fputc('\n', file_);
            std::fflush(file_);
        }
    }
    [[nodiscard]] const std::string& text() const noexcept { return text_; }

private:
    std::FILE* file_ = nullptr;
    std::string text_;
};

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::string errno_text() { return std::strerror(errno); }

bool write_file(const std::string& path, const void* data, std::size_t size, std::string* err) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        *err = "open " + path + ": " + errno_text();
        return false;
    }
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    if (!ok) {
        *err = "write " + path + " failed";
    }
    return ok;
}

std::vector<std::uint8_t> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

enum class LoadMethod { None, DlopenFile, DlopenMemfd, OwnLoader };

const char* method_name(LoadMethod m) {
    switch (m) {
    case LoadMethod::DlopenFile: return "dlopen(read-only file)";
    case LoadMethod::DlopenMemfd: return "dlopen(memfd)";
    case LoadMethod::OwnLoader: return "own ELF loader (anonymous memory)";
    case LoadMethod::None: break;
    }
    return "none";
}

class LoadedCode {
public:
    LoadedCode() = default;
    LoadedCode(const LoadedCode&) = delete;
    LoadedCode& operator=(const LoadedCode&) = delete;
    ~LoadedCode() {
        if (handle_ != nullptr) {
            dlclose(handle_);
        }
        if (map_ != nullptr) {
            munmap(map_, map_size_);
        }
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    [[nodiscard]] void* sym(const std::string& name) const {
        if (handle_ != nullptr) {
            return dlsym(handle_, name.c_str());
        }
        const auto it = syms_.find(name);
        return it == syms_.end() ? nullptr : reinterpret_cast<void*>(it->second);
    }

    bool load_file(const std::string& so_path, std::string* err) {
        if (chmod(so_path.c_str(), 0444) != 0) {
            *err = "chmod 0444: " + errno_text();
            return false;
        }
        handle_ = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle_ == nullptr) {
            const char* e = dlerror();
            *err = e != nullptr ? e : "dlopen failed";
            return false;
        }
        return true;
    }

    bool load_memfd(const std::vector<std::uint8_t>& so, const std::string& name, std::string* err) {
        fd_ = static_cast<int>(syscall(__NR_memfd_create, name.c_str(), MFD_CLOEXEC));
        if (fd_ < 0) {
            *err = "memfd_create: " + errno_text();
            return false;
        }
        if (write(fd_, so.data(), so.size()) != static_cast<ssize_t>(so.size())) {
            *err = "memfd write: " + errno_text();
            return false;
        }
#if defined(__ANDROID__)
        android_dlextinfo info{};
        info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
        info.library_fd = fd_;
        handle_ = android_dlopen_ext(name.c_str(), RTLD_NOW | RTLD_LOCAL, &info);
#else
        const std::string path = "/proc/self/fd/" + std::to_string(fd_);
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (handle_ == nullptr) {
            const char* e = dlerror();
            *err = e != nullptr ? e : "dlopen(memfd) failed";
            return false;
        }
        return true;
    }

    // Minimal loader for the shared objects our LLD produces: PT_LOAD segments, RELATIVE
    // relocations, `.dynsym` symbols. Anything else (imports, TLS) is refused.
    bool load_own(const std::vector<std::uint8_t>& so, std::string* err) {
        if (so.size() < sizeof(Elf64_Ehdr)) {
            *err = "ELF too small";
            return false;
        }
        Elf64_Ehdr eh{};
        std::memcpy(&eh, so.data(), sizeof(eh));
        if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
            eh.e_type != ET_DYN || eh.e_phentsize != sizeof(Elf64_Phdr) ||
            eh.e_phoff + static_cast<std::uint64_t>(eh.e_phnum) * sizeof(Elf64_Phdr) > so.size()) {
            *err = "not a 64-bit ELF shared object";
            return false;
        }
        std::vector<Elf64_Phdr> ph(eh.e_phnum);
        std::memcpy(ph.data(), so.data() + eh.e_phoff, ph.size() * sizeof(Elf64_Phdr));
        const std::uint64_t page = static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
        std::uint64_t hi = 0;
        for (const Elf64_Phdr& p : ph) {
            if (p.p_type == PT_LOAD) {
                hi = std::max<std::uint64_t>(hi, p.p_vaddr + p.p_memsz);
            }
        }
        map_size_ = static_cast<std::size_t>((hi + page - 1) & ~(page - 1));
        map_ = mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (map_ == MAP_FAILED) {
            map_ = nullptr;
            *err = "mmap: " + errno_text();
            return false;
        }
        auto* base = static_cast<std::uint8_t*>(map_);
        const Elf64_Phdr* dyn = nullptr;
        for (const Elf64_Phdr& p : ph) {
            if (p.p_type == PT_DYNAMIC) {
                dyn = &p;
            }
            if (p.p_type != PT_LOAD) {
                continue;
            }
            if (p.p_offset + p.p_filesz > so.size()) {
                *err = "segment past end of file";
                return false;
            }
            std::memcpy(base + p.p_vaddr, so.data() + p.p_offset, p.p_filesz);
        }
        if (dyn != nullptr) {
            std::uint64_t rela = 0;
            std::uint64_t relasz = 0;
            for (std::uint64_t off = dyn->p_vaddr; off + sizeof(Elf64_Dyn) <= hi; off += sizeof(Elf64_Dyn)) {
                Elf64_Dyn d{};
                std::memcpy(&d, base + off, sizeof(d));
                if (d.d_tag == DT_NULL) {
                    break;
                }
                if (d.d_tag == DT_RELA) {
                    rela = d.d_un.d_ptr;
                } else if (d.d_tag == DT_RELASZ) {
                    relasz = d.d_un.d_val;
                } else if (d.d_tag == DT_NEEDED) {
                    *err = "imports (DT_NEEDED) are not supported";
                    return false;
                }
            }
            for (std::uint64_t r = 0; r + sizeof(Elf64_Rela) <= relasz; r += sizeof(Elf64_Rela)) {
                Elf64_Rela rel{};
                std::memcpy(&rel, base + rela + r, sizeof(rel));
                const auto type = static_cast<std::uint32_t>(ELF64_R_TYPE(rel.r_info));
                if (type != kRelocAArch64Relative && type != kRelocX8664Relative) {
                    *err = "unsupported relocation type " + std::to_string(type);
                    return false;
                }
                const std::uint64_t v = reinterpret_cast<std::uint64_t>(base) +
                                        static_cast<std::uint64_t>(rel.r_addend);
                std::memcpy(base + rel.r_offset, &v, 8);
            }
        }
        // Symbols from the section headers (`.dynsym` + its string table).
        if (eh.e_shoff != 0 && eh.e_shentsize == sizeof(Elf64_Shdr) &&
            eh.e_shoff + static_cast<std::uint64_t>(eh.e_shnum) * sizeof(Elf64_Shdr) <= so.size()) {
            std::vector<Elf64_Shdr> sh(eh.e_shnum);
            std::memcpy(sh.data(), so.data() + eh.e_shoff, sh.size() * sizeof(Elf64_Shdr));
            for (const Elf64_Shdr& s : sh) {
                if (s.sh_type != SHT_DYNSYM || s.sh_link >= sh.size()) {
                    continue;
                }
                const Elf64_Shdr& str = sh[s.sh_link];
                for (std::uint64_t o = 0; o + sizeof(Elf64_Sym) <= s.sh_size; o += sizeof(Elf64_Sym)) {
                    if (s.sh_offset + o + sizeof(Elf64_Sym) > so.size()) {
                        break;
                    }
                    Elf64_Sym sym{};
                    std::memcpy(&sym, so.data() + s.sh_offset + o, sizeof(sym));
                    if (sym.st_shndx == SHN_UNDEF || sym.st_name >= str.sh_size ||
                        str.sh_offset + sym.st_name >= so.size()) {
                        continue;
                    }
                    const char* name = reinterpret_cast<const char*>(so.data() + str.sh_offset + sym.st_name);
                    syms_[std::string(name, strnlen(name, so.size() - (str.sh_offset + sym.st_name)))] =
                        reinterpret_cast<std::uintptr_t>(base) + sym.st_value;
                }
            }
        }
        for (const Elf64_Phdr& p : ph) {
            if (p.p_type != PT_LOAD) {
                continue;
            }
            const std::uint64_t start = p.p_vaddr & ~(page - 1);
            const std::uint64_t end = (p.p_vaddr + p.p_memsz + page - 1) & ~(page - 1);
            int prot = PROT_READ;
            if ((p.p_flags & PF_X) != 0) {
                prot |= PROT_EXEC;
            } else if ((p.p_flags & PF_W) != 0) {
                prot |= PROT_WRITE;
            }
            if (mprotect(base + start, end - start, prot) != 0) {
                *err = std::string("mprotect ") + ((prot & PROT_EXEC) != 0 ? "R+X" : "R/RW") +
                       ": " + errno_text();
                return false;
            }
            if ((prot & PROT_EXEC) != 0) {
                __builtin___clear_cache(reinterpret_cast<char*>(base + start),
                                        reinterpret_cast<char*>(base + end));
            }
        }
        return true;
    }

private:
    void* handle_ = nullptr;
    void* map_ = nullptr;
    std::size_t map_size_ = 0;
    int fd_ = -1;
    std::map<std::string, std::uintptr_t> syms_;
};

struct Workspace {
    std::string dir;
    int serial = 0;
    std::string next(const char* stem, const char* ext) {
        return dir + "/" + stem + "_" + std::to_string(getpid()) + "_" + std::to_string(serial++) + ext;
    }
};

// Emits, links and loads with `method`. `so_bytes_out` receives the linked file.
bool build_and_load(Translator& tr, int opt, Workspace& ws, LoadMethod method, LoadedCode& code,
                    TranslateTimings* timings, double* link_ms, std::size_t* so_bytes,
                    std::string* err) {
    std::vector<char> obj;
    if (!tr.emit_object(opt, obj, timings, err)) {
        return false;
    }
    const std::string obj_path = ws.next("fn", ".o");
    const std::string so_path = ws.next("fn", ".so");
    if (!write_file(obj_path, obj.data(), obj.size(), err)) {
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::string log;
    const bool linked = link_shared_object({obj_path}, so_path, &log);
    if (link_ms != nullptr) {
        *link_ms = ms_since(t0);
    }
    unlink(obj_path.c_str());
    if (!linked) {
        *err = "lld: " + log;
        return false;
    }
    const std::vector<std::uint8_t> so = read_file(so_path);
    if (so_bytes != nullptr) {
        *so_bytes = so.size();
    }
    switch (method) {
    case LoadMethod::DlopenFile:
        return code.load_file(so_path, err);
    case LoadMethod::DlopenMemfd:
        unlink(so_path.c_str());
        return code.load_memfd(so, "wse_r0_fn", err);
    case LoadMethod::OwnLoader:
        unlink(so_path.c_str());
        return code.load_own(so, err);
    case LoadMethod::None:
        break;
    }
    *err = "no working load method";
    return false;
}

using GuestFn = void (*)(CpuState*);
using ProbeFn = std::int32_t (*)(std::int32_t, std::int32_t, std::int32_t);

std::uint64_t run_synthetic(GuestFn fn, const DecodedFunction* reference, const std::uint8_t* buf,
                            std::uint64_t seed, bool* ok) {
    GuestStack stack;
    CpuState s;
    stack.prepare(s);
    s.rip = kSyntheticVa;
    s.gpr[static_cast<int>(Reg::rcx)] = reinterpret_cast<std::uintptr_t>(buf);
    s.gpr[static_cast<int>(Reg::rdx)] = seed;
    if (fn != nullptr) {
        fn(&s);
    } else {
        Interpreter interp;
        std::string err;
        (void)interp.run(s, GuestStack::kReturnSentinel,
                         [&](std::uint64_t va) { return reference->at(va); }, 1U << 24, &err);
    }
    *ok = s.rip == GuestStack::kReturnSentinel;
    return s.gpr[0];
}

std::string to_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

}  // namespace

// The R1 coverage meter (`docs/RECOMP_PLAN.md` "R1 design"). Entries: non-chained `.pdata`
// begins, PDB functions in `.text`, then direct call targets to a fixed point. A first pass only
// collects entries; the second discovers and lifts each function and drops its body, so the
// decoded image is never held in memory at once. Lifting goes in batches whose LLVM module is
// discarded.
static void run_r1_coverage(Report& rep, const PeImage& img, const std::vector<PdbPublic>& publics,
                            const std::uint8_t* image) {
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t text_b = 0;
    std::uint64_t text_e = 0;
    for (const PeSection& s : img.sections) {
        if (s.name == ".text") {
            text_b = img.image_base + s.virtual_address;
            text_e = text_b + s.virtual_size;
        }
    }
    ImageView view(image, img.image_base, img.size_of_image, text_b, text_e);
    std::set<std::uint64_t> entries;
    for (const PeFunctionRange& r : img.functions) {
        if (r.unwind < img.size_of_image && ((image[r.unwind] >> 3) & 0x4) == 0) {
            entries.insert(img.image_base + r.begin);
        }
    }
    for (const PdbPublic& p : publics) {
        if (p.section == 0 || p.section > img.sections.size() || (p.flags & 2U) == 0) {
            continue;  // flag 2: a function (CVPSF_FUNCTION)
        }
        const std::uint64_t va = img.image_base + img.sections[p.section - 1].virtual_address + p.offset;
        if (view.in_text(va)) {
            entries.insert(va);
        }
    }
    Discoverer disc(view);
    std::vector<std::uint64_t> work(entries.begin(), entries.end());
    std::set<std::uint64_t> seen;
    while (!work.empty()) {
        const std::uint64_t e = work.back();
        work.pop_back();
        if (!seen.insert(e).second) {
            continue;
        }
        const DFunction f = disc.discover(e, entries);
        for (const std::uint64_t t : f.call_targets) {
            if (view.in_text(t) && entries.insert(t).second) {
                work.push_back(t);
            }
        }
    }
    const double discover_ms = ms_since(t0);

    ImageInfo info;
    info.base = img.image_base;
    R1Translator tr(info);
    const std::vector<std::uint64_t> all(entries.begin(), entries.end());
    constexpr std::size_t kBatch = 400;
    std::size_t complete = 0;
    std::uint64_t insns = 0;
    std::map<std::string, std::size_t> discover_errors;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (i % kBatch == 0) {
            tr.discard();
            tr.declare(all);
        }
        const DFunction f = disc.discover(all[i], entries);
        insns += f.insns.size();
        if (!f.error.empty()) {
            discover_errors[f.error.substr(0, f.error.find(" at "))]++;
            continue;
        }
        if (f.unresolved_indirect_jumps.empty()) {
            ++complete;
        }
        std::string why;
        (void)tr.lift(f, &why);
    }
    tr.discard();
    const LiftReport& r = tr.report();
    rep.line("  functions %zu, bodies complete %zu, instructions %llu (discovery %.0f ms)", all.size(), complete,
             static_cast<unsigned long long>(insns), discover_ms);
    for (const auto& [what, n] : discover_errors) {
        rep.line("    not decoded: %-30s %zu", what.c_str(), n);
    }
    rep.line("  lifted %zu/%zu (%.2f%%) in %.0f ms", r.lifted, r.attempted,
             r.attempted == 0 ? 0.0 : 100.0 * static_cast<double>(r.lifted) / static_cast<double>(r.attempted),
             tr.total_lift_ms());
    std::vector<std::pair<std::size_t, std::string>> top;
    for (const auto& [what, n] : r.unsupported) {
        top.emplace_back(n, what);
    }
    std::sort(top.rbegin(), top.rend());
    rep.line("  first unsupported instruction per failing function (top %zu of %zu):",
             std::min<std::size_t>(top.size(), 20), top.size());
    for (std::size_t k = 0; k < top.size() && k < 20; ++k) {
        rep.line("    %-36s %zu", top[k].second.c_str(), top[k].first);
    }
}

R0SpikeResult run_r0_spikes(const R0SpikeInput& input) {
    R0SpikeResult result;
    const auto t_all = std::chrono::steady_clock::now();
    Workspace ws{input.work_dir + "/r0"};
    mkdir(input.work_dir.c_str(), 0700);
    mkdir(ws.dir.c_str(), 0700);
    Report rep(ws.dir + "/report.txt");

    Translator tr;
    const unsigned cores = std::max(1U, std::thread::hardware_concurrency());
    rep.line("WSE2 R0 spikes (docs/RECOMP_PLAN.md)");
    rep.line("target %s, %u CPU cores", tr.triple().c_str(), cores);
    if (input.apk_bytes != 0) {
        rep.line("APK %.1f MB", static_cast<double>(input.apk_bytes) / 1e6);
    }
    if (!tr.init_error().empty()) {
        rep.line("LLVM init FAILED: %s", tr.init_error().c_str());
        result.report = rep.text();
        return result;
    }

    // --- (a) ---------------------------------------------------------------------------
    rep.line("");
    rep.line("(a) generated .so loads and runs");
    LoadMethod method = LoadMethod::None;
    {
        std::vector<char> obj;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        if (!emit_probe_object("wse_r0_probe", obj, &err)) {
            rep.line("  emit FAILED: %s", err.c_str());
        } else {
            const double emit_ms = ms_since(t0);
            const std::string obj_path = ws.next("probe", ".o");
            const std::string so_path = ws.next("probe", ".so");
            std::string log;
            const auto t1 = std::chrono::steady_clock::now();
            const bool linked = write_file(obj_path, obj.data(), obj.size(), &err) &&
                                link_shared_object({obj_path}, so_path, &log);
            const double link_ms = ms_since(t1);
            unlink(obj_path.c_str());
            if (!linked) {
                rep.line("  link FAILED: %s %s", err.c_str(), log.c_str());
            } else {
                const std::vector<std::uint8_t> so = read_file(so_path);
                rep.line("  LLVM emit %.1f ms, LLD link %.1f ms, %zu-byte .so", emit_ms, link_ms,
                         so.size());
                const LoadMethod order[] = {LoadMethod::DlopenFile, LoadMethod::DlopenMemfd,
                                            LoadMethod::OwnLoader};
                for (const LoadMethod m : order) {
                    LoadedCode code;
                    std::string lerr;
                    bool loaded = false;
                    if (m == LoadMethod::DlopenFile) {
                        const std::string copy = ws.next("probe_ro", ".so");
                        loaded = write_file(copy, so.data(), so.size(), &lerr) &&
                                 code.load_file(copy, &lerr);
                    } else if (m == LoadMethod::DlopenMemfd) {
                        loaded = code.load_memfd(so, "wse_r0_probe", &lerr);
                    } else {
                        loaded = code.load_own(so, &lerr);
                    }
                    auto* fn = loaded ? reinterpret_cast<ProbeFn>(code.sym("wse_r0_probe")) : nullptr;
                    if (fn == nullptr) {
                        rep.line("  %-34s FAIL: %s", method_name(m),
                                 loaded ? "symbol not found" : lerr.c_str());
                        continue;
                    }
                    const std::int32_t v = fn(6, 7, 5);
                    rep.line("  %-34s %s (6*7+5 = %d)", method_name(m), v == 47 ? "OK" : "WRONG", v);
                    if (v == 47 && method == LoadMethod::None) {
                        method = m;
                    }
                }
            }
        }
    }
    result.a_loads = method != LoadMethod::None;
    rep.line("  => %s%s", result.a_loads ? "PASS, using " : "FAIL", result.a_loads ? method_name(method) : "");

    // --- exe -----------------------------------------------------------------------------
    rep.line("");
    // Any WSE2 x64 build is accepted when its own PDB (shipped next to it) matches it: functions
    // are found by name, not by a pinned address.
    PeImage img;
    std::vector<PdbPublic> publics;
    bool exe_ok = false;
    std::uint64_t index_of_va = 0;
    if (input.exe.empty()) {
        rep.line("exe: not provided (%s)", input.exe_error.empty() ? "no install" : input.exe_error.c_str());
    } else {
        std::string err;
        PeCodeView cv;
        PdbGuidAge ga;
        const std::string sha = to_lower(input.exe_sha256);
        const char* label = sha == kWse2V1155ExeSha256   ? "v1.1.5.5"
                            : sha == kWse2V1156ExeSha256 ? "v1.1.5.6"
                                                         : "unknown build";
        rep.line("exe: %zu bytes, SHA-256 %.16s... (%s)", input.exe.size(), sha.c_str(), label);
        if (!parse_pe(input.exe.data(), input.exe.size(), img, &err)) {
            rep.line("  PE: %s", err.c_str());
        } else if (!read_pe_codeview(input.exe.data(), input.exe.size(), img, cv, &err)) {
            rep.line("  no PDB link in the exe: %s", err.c_str());
        } else if (input.pdb.empty()) {
            rep.line("  PDB: not provided (%s)", input.pdb_error.empty() ? "missing" : input.pdb_error.c_str());
        } else if (!read_pdb_publics(input.pdb.data(), input.pdb.size(), publics, &ga, &err)) {
            rep.line("  PDB: %s", err.c_str());
        } else if (ga.guid != cv.guid) {
            rep.line("  PDB does not belong to this exe (GUID differs; exe age %u, PDB age %u)", cv.age, ga.age);
        } else if (const auto va = pdb_public_va(publics, kR0IndexOfSymbol, img.sections, img.image_base); !va) {
            rep.line("  PDB matches (age %u, %zu publics) but has no %s", ga.age, publics.size(), kR0IndexOfSymbol);
        } else {
            index_of_va = *va;
            exe_ok = true;
            rep.line("  PDB matches (GUID, age %u), %zu publics; rglString::indexOf at 0x%llx", ga.age,
                     publics.size(), static_cast<unsigned long long>(index_of_va));
        }
    }

    // --- (b) ---------------------------------------------------------------------------
    rep.line("");
    rep.line("(b) map the image at 0x140000000");
    const std::uint64_t image_base = 0x140000000ULL;
    const std::size_t image_size = exe_ok ? img.size_of_image : kV1155SizeOfImage;
    void* image = mmap(reinterpret_cast<void*>(image_base), image_size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (image == MAP_FAILED) {
        rep.line("  mmap(MAP_FIXED_NOREPLACE, %zu bytes) FAILED: %s", image_size, errno_text().c_str());
        image = nullptr;
    } else if (image != reinterpret_cast<void*>(image_base)) {
        rep.line("  kernel ignored MAP_FIXED_NOREPLACE: got %p", image);
        munmap(image, image_size);
        image = nullptr;
    } else {
        result.b_maps = true;
        rep.line("  OK: %zu bytes at %p", image_size, image);
        if (exe_ok) {
            const bool mapped = map_pe_sections(input.exe.data(), input.exe.size(), img,
                                                static_cast<std::uint8_t*>(image));
            rep.line("  sections copied: %s (%zu sections, %zu .pdata functions)", mapped ? "yes" : "FAILED",
                     img.sections.size(), img.functions.size());
            exe_ok = mapped;
        }
    }
    rep.line("  => %s", result.b_maps ? "PASS" : "FAIL");
    if (image == nullptr) {
        exe_ok = false;
    }

    // --- (c) ---------------------------------------------------------------------------
    rep.line("");
    rep.line("(c) translation speed (single thread)");
    std::uint64_t text_bytes = 5242293;  // v1.1.5.5 .text virtual size
    if (exe_ok) {
        for (const PeSection& s : img.sections) {
            if (s.name == ".text") {
                text_bytes = s.virtual_size;
            }
        }
    }
    double synthetic_rate = 0.0;  // guest bytes per second at O2, lift..link
    if (result.a_loads) {
        const std::vector<std::uint8_t> code = make_synthetic_function(input.synthetic_blocks);
        const DecodedFunction fn = decode_function(code.data(), kSyntheticVa, kSyntheticVa + code.size());
        for (const int opt : {2, 1}) {
            std::string err;
            TranslateTimings t;
            double link_ms = 0.0;
            std::size_t so_bytes = 0;
            LoadedCode loaded;
            const bool added = tr.add_function(fn, "wse_synth", &err);
            const bool ok = added && build_and_load(tr, opt, ws, method, loaded, &t, &link_ms,
                                                    &so_bytes, &err);
            if (!ok) {
                rep.line("  synthetic O%d FAILED: %s", opt, err.c_str());
                continue;
            }
            const double total = t.lift_ms + t.optimize_ms + t.codegen_ms + link_ms;
            const double rate = static_cast<double>(code.size()) / (total / 1000.0);
            rep.line("  synthetic %zu B (%zu insns) O%d: lift %.1f + opt %.1f + codegen %.1f + link %.1f = %.1f ms (%.0f KB/s, %zu-byte .so)",
                     code.size(), fn.insns.size(), opt, t.lift_ms, t.optimize_ms, t.codegen_ms,
                     link_ms, total, rate / 1024.0, so_bytes);
            // The translated code must agree with the interpreter.
            auto* gfn = reinterpret_cast<GuestFn>(loaded.sym("wse_synth"));
            std::vector<std::uint8_t> buf(256);
            bool same = gfn != nullptr;
            for (std::uint64_t seed = 0; same && seed < 16; ++seed) {
                for (std::size_t k = 0; k < buf.size(); ++k) {
                    buf[k] = static_cast<std::uint8_t>((k * 131U + seed * 17U) ^ (k >> 3));
                }
                bool ok_n = false;
                bool ok_i = false;
                const std::uint64_t n = run_synthetic(gfn, nullptr, buf.data(), seed * 0x9E3779B9ULL, &ok_n);
                const std::uint64_t i = run_synthetic(nullptr, &fn, buf.data(), seed * 0x9E3779B9ULL, &ok_i);
                same = ok_n && ok_i && n == i;
            }
            rep.line("  synthetic O%d translated == interpreter on 16 inputs: %s", opt, same ? "yes" : "NO");
            if (opt == 2 && same) {
                synthetic_rate = rate;
                result.c_measured = true;
            }
        }
    }
    double retail_rate = 0.0;
    if (exe_ok && result.a_loads) {
        const auto t0 = std::chrono::steady_clock::now();
        std::size_t decoded = 0;
        std::uint64_t decoded_bytes = 0;
        std::vector<DecodedFunction> fns;
        for (const PeFunctionRange& r : img.functions) {
            DecodedFunction fn = decode_function(static_cast<const std::uint8_t*>(image) + r.begin,
                                                 image_base + r.begin, image_base + r.end);
            if (fn.error.empty()) {
                ++decoded;
                decoded_bytes += r.end - r.begin;
                fns.push_back(std::move(fn));
            }
        }
        const double decode_ms = ms_since(t0);
        rep.line("  retail decode of %zu .pdata functions: %.1f ms; R0 subset decodes %zu (%llu bytes)",
                 img.functions.size(), decode_ms, decoded, static_cast<unsigned long long>(decoded_bytes));
        std::string err;
        std::size_t lifted = 0;
        for (const DecodedFunction& fn : fns) {
            char name[32];
            std::snprintf(name, sizeof(name), "wse_fn_%llx", static_cast<unsigned long long>(fn.start));
            if (tr.add_function(fn, name, &err)) {
                ++lifted;
            }
        }
        TranslateTimings t;
        double link_ms = 0.0;
        std::size_t so_bytes = 0;
        LoadedCode loaded;
        if (lifted > 0 && build_and_load(tr, 2, ws, method, loaded, &t, &link_ms, &so_bytes, &err)) {
            const double total = t.lift_ms + t.optimize_ms + t.codegen_ms + link_ms;
            retail_rate = static_cast<double>(decoded_bytes) / (total / 1000.0);
            rep.line("  retail %zu functions O2: lift %.1f + opt %.1f + codegen %.1f + link %.1f = %.1f ms (%.0f KB/s, %zu-byte .so)",
                     lifted, t.lift_ms, t.optimize_ms, t.codegen_ms, link_ms, total,
                     retail_rate / 1024.0, so_bytes);
        } else {
            rep.line("  retail batch FAILED: %s", err.c_str());
        }
    }
    if (synthetic_rate > 0.0) {
        const double s1 = static_cast<double>(text_bytes) / synthetic_rate;
        rep.line("  whole .text (%llu bytes) at the synthetic O2 rate: %.0f s on 1 core, ~%.0f s on %u cores",
                 static_cast<unsigned long long>(text_bytes), s1, s1 / cores, cores);
    }
    if (retail_rate > 0.0) {
        const double s1 = static_cast<double>(text_bytes) / retail_rate;
        rep.line("  whole .text at the retail-subset O2 rate: %.0f s on 1 core, ~%.0f s on %u cores", s1,
                 s1 / cores, cores);
    }
    rep.line("  => %s", result.c_measured ? "MEASURED" : "FAIL");

    // --- (d) ---------------------------------------------------------------------------
    rep.line("");
    rep.line("(d) rglString::indexOf (found through the PDB) translated vs PC");
    if (!exe_ok || !result.a_loads) {
        rep.line("  skipped: %s", !result.a_loads ? "no load method works" : "no exe with a matching PDB");
    } else {
        const PeFunctionRange* range = nullptr;
        for (const PeFunctionRange& r : img.functions) {
            if (image_base + r.begin == index_of_va) {
                range = &r;
            }
        }
        std::string err;
        LoadedCode loaded;
        TranslateTimings t;
        double link_ms = 0.0;
        DecodedFunction fn;
        if (range != nullptr) {
            fn = decode_function(static_cast<const std::uint8_t*>(image) + range->begin,
                                 image_base + range->begin, image_base + range->end);
        }
        if (range == nullptr) {
            rep.line("  FAILED: no .pdata entry at the PDB address of indexOf");
        } else if (!tr.add_function(fn, "wse_fn_index_of", &err) ||
                   !build_and_load(tr, 2, ws, method, loaded, &t, &link_ms, nullptr, &err)) {
            rep.line("  FAILED: %s", err.c_str());
        } else {
            auto* gfn = reinterpret_cast<GuestFn>(loaded.sym("wse_fn_index_of"));
            const std::vector<R0IndexOfCase> cases = make_r0_index_of_cases();
            std::vector<std::int32_t> translated;
            std::vector<std::int32_t> interpreted;
            std::vector<std::int32_t> model;
            GuestStack stack;
            Interpreter interp;
            bool returned = gfn != nullptr;
            for (const R0IndexOfCase& c : cases) {
                if (!returned) {
                    break;
                }
                RglStringFake hay(c.haystack);
                RglStringFake needle(c.needle);
                for (int pass = 0; pass < 2; ++pass) {
                    CpuState s;
                    stack.prepare(s);
                    s.rip = index_of_va;
                    s.gpr[static_cast<int>(Reg::rcx)] = reinterpret_cast<std::uintptr_t>(hay.bytes);
                    s.gpr[static_cast<int>(Reg::rdx)] = reinterpret_cast<std::uintptr_t>(needle.bytes);
                    s.gpr[static_cast<int>(Reg::r8)] = static_cast<std::uint32_t>(c.start);
                    if (pass == 0) {
                        gfn(&s);
                    } else {
                        (void)interp.run(s, GuestStack::kReturnSentinel,
                                         [&](std::uint64_t va) { return fn.at(va); }, 1U << 20, &err);
                    }
                    returned = returned && s.rip == GuestStack::kReturnSentinel;
                    (pass == 0 ? translated : interpreted).push_back(static_cast<std::int32_t>(s.gpr[0]));
                }
                model.push_back(r0_index_of_model(c));
            }
            const std::uint64_t ht = fnv1a_results(translated);
            rep.line("  translated in %.1f ms (lift %.1f, opt %.1f, codegen %.1f, link %.1f), %zu cases",
                     t.lift_ms + t.optimize_ms + t.codegen_ms + link_ms, t.lift_ms, t.optimize_ms,
                     t.codegen_ms, link_ms, cases.size());
            rep.line("  translated  hash 0x%016llx", static_cast<unsigned long long>(ht));
            rep.line("  interpreter hash 0x%016llx", static_cast<unsigned long long>(fnv1a_results(interpreted)));
            rep.line("  PC native   hash 0x%016llx (tools/r0_pc_reference.cpp)",
                     static_cast<unsigned long long>(kR0IndexOfPcHash));
            result.d_matches = returned && ht == kR0IndexOfPcHash && translated == interpreted &&
                               translated == model;
        }
    }
    rep.line("  => %s", result.d_matches ? "PASS" : ((!exe_ok || !result.a_loads) ? "SKIPPED" : "FAIL"));

    // --- R1 coverage ------------------------------------------------------------------------
    rep.line("");
    rep.line("R1 coverage: every function of this exe decoded and lifted (no codegen)");
    if (!exe_ok || !result.b_maps || image == nullptr) {
        rep.line("  skipped: %s", !exe_ok ? "no exe with a matching PDB" : "image not mapped");
    } else {
        run_r1_coverage(rep, img, publics, static_cast<const std::uint8_t*>(image));
    }

    if (image != nullptr) {
        munmap(image, image_size);
    }
    rep.line("");
    rep.line("summary: a=%s b=%s c=%s d=%s (%.1f s)", result.a_loads ? "PASS" : "FAIL",
             result.b_maps ? "PASS" : "FAIL", result.c_measured ? "MEASURED" : "FAIL",
             result.d_matches ? "PASS" : (exe_ok ? "FAIL" : "SKIPPED"), ms_since(t_all) / 1000.0);
    result.report = rep.text();
    return result;
}

}  // namespace wse::recomp
