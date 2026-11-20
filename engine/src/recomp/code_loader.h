// Loading a shared object our LLD produced (Phase 5, `docs/RECOMP_PLAN.md` "R0", "Risks"), in
// one of three ways, since Android may refuse executable mappings of app-data files:
//   1. `dlopen` of the linked `.so` in app storage, made read-only first,
//   2. `dlopen` from a `memfd` (`android_dlopen_ext` + `ANDROID_DLEXT_USE_LIBRARY_FD` on Android),
//   3. our own ELF loader: segments copied into anonymous memory, `R_*_RELATIVE` relocations,
//      then `mprotect` to read+execute.
// Used by the R0 spikes and the R4 startup runner (`r4_startup.h`).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#if defined(__ANDROID__)
#include <android/dlext.h>
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

namespace wse::recomp::code_loader_detail {
inline constexpr std::uint32_t kRelocAArch64Relative = 1027;
inline constexpr std::uint32_t kRelocX8664Relative = 8;
inline std::string errno_text() { return std::strerror(errno); }
}  // namespace wse::recomp::code_loader_detail

namespace wse::recomp {

enum class LoadMethod { None, DlopenFile, DlopenMemfd, OwnLoader };

inline const char* method_name(LoadMethod m) {
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

    // Takes over a handle the caller got from `dlopen` (closed with this object).
    void adopt_handle(void* handle) noexcept { handle_ = handle; }

    [[nodiscard]] void* sym(const std::string& name) const {
        if (handle_ != nullptr) {
            return dlsym(handle_, name.c_str());
        }
        const auto it = syms_.find(name);
        return it == syms_.end() ? nullptr : reinterpret_cast<void*>(it->second);
    }

    bool load_file(const std::string& so_path, std::string* err) {
        if (chmod(so_path.c_str(), 0444) != 0) {
            *err = "chmod 0444: " + code_loader_detail::errno_text();
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
            *err = "memfd_create: " + code_loader_detail::errno_text();
            return false;
        }
        if (write(fd_, so.data(), so.size()) != static_cast<ssize_t>(so.size())) {
            *err = "memfd write: " + code_loader_detail::errno_text();
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
            *err = "mmap: " + code_loader_detail::errno_text();
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
                if (type != code_loader_detail::kRelocAArch64Relative && type != code_loader_detail::kRelocX8664Relative) {
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
                       ": " + code_loader_detail::errno_text();
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

}  // namespace wse::recomp
