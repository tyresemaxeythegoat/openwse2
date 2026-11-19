// Minimal PE32+ reader for the recompiler: headers, sections, `.pdata` and image mapping.
//
// Reads the user's own exe from memory (it is never bundled). Only what R0 needs: the
// preferred base and size (the image maps at its preferred base, `docs/RECOMP_PLAN.md`), section
// copies, the `.pdata` function ranges used for function discovery, (R3) the import and TLS
// directories, and (R4) exports, base relocations and resources. Every offset is bounds-checked; a
// malformed file fails with a message instead of reading out of range.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace wse::recomp {

struct PeSection {
    std::string name;
    std::uint32_t virtual_address = 0;
    std::uint32_t virtual_size = 0;
    std::uint32_t raw_offset = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t characteristics = 0;
};

struct PeFunctionRange {
    std::uint32_t begin = 0;  // RVA
    std::uint32_t end = 0;    // RVA, exclusive
    std::uint32_t unwind = 0; // RVA of UNWIND_INFO
    std::uint32_t entry = 0;  // RVA of this RUNTIME_FUNCTION in `.pdata` (0 when synthetic)
};

struct PeImage {
    std::uint64_t image_base = 0;
    std::uint32_t size_of_image = 0;
    std::uint32_t size_of_headers = 0;
    std::uint32_t entry_rva = 0;
    std::uint32_t debug_rva = 0;
    std::uint32_t debug_size = 0;
    std::vector<PeSection> sections;
    std::vector<PeFunctionRange> functions;  // `.pdata`, sorted by `begin`

    // Byte range of `rva .. rva + n` inside the file, or null when it is not file-backed.
    [[nodiscard]] const std::uint8_t* file_ptr(const std::uint8_t* file, std::size_t file_size,
                                               std::uint32_t rva, std::size_t n) const noexcept {
        for (const PeSection& s : sections) {
            if (rva >= s.virtual_address && rva - s.virtual_address < s.raw_size) {
                const std::uint64_t off = static_cast<std::uint64_t>(s.raw_offset) +
                                          (rva - s.virtual_address);
                const std::uint64_t avail = s.raw_size - (rva - s.virtual_address);
                if (n <= avail && off + n <= file_size) {
                    return file + off;
                }
                return nullptr;
            }
        }
        return nullptr;
    }
};

namespace pe_detail {

inline bool rd16(const std::uint8_t* f, std::size_t n, std::size_t off, std::uint16_t& v) noexcept {
    if (off + 2 > n) {
        return false;
    }
    v = static_cast<std::uint16_t>(f[off] | (f[off + 1] << 8));
    return true;
}
inline bool rd32(const std::uint8_t* f, std::size_t n, std::size_t off, std::uint32_t& v) noexcept {
    if (off + 4 > n) {
        return false;
    }
    std::memcpy(&v, f + off, 4);
    return true;
}
inline bool rd64(const std::uint8_t* f, std::size_t n, std::size_t off, std::uint64_t& v) noexcept {
    if (off + 8 > n) {
        return false;
    }
    std::memcpy(&v, f + off, 8);
    return true;
}

}  // namespace pe_detail

inline bool parse_pe(const std::uint8_t* f, std::size_t n, PeImage& out, std::string* error) {
    using namespace pe_detail;
    out = PeImage{};
    const auto fail = [&](const char* m) {
        if (error != nullptr) {
            *error = m;
        }
        return false;
    };
    std::uint16_t mz = 0;
    std::uint32_t pe_off = 0;
    if (!rd16(f, n, 0, mz) || mz != 0x5A4D || !rd32(f, n, 0x3C, pe_off)) {
        return fail("not an MZ executable");
    }
    std::uint32_t sig = 0;
    std::uint16_t machine = 0;
    std::uint16_t nsec = 0;
    std::uint16_t opt_size = 0;
    if (!rd32(f, n, pe_off, sig) || sig != 0x00004550 || !rd16(f, n, pe_off + 4, machine) ||
        !rd16(f, n, pe_off + 6, nsec) || !rd16(f, n, pe_off + 20, opt_size)) {
        return fail("no PE header");
    }
    if (machine != 0x8664) {
        return fail("not an x86-64 image");
    }
    const std::size_t opt = pe_off + 24;
    std::uint16_t magic = 0;
    if (!rd16(f, n, opt, magic) || magic != 0x20B) {
        return fail("not PE32+");
    }
    std::uint32_t ndirs = 0;
    std::uint32_t exc_rva = 0;
    std::uint32_t exc_size = 0;
    if (!rd32(f, n, opt + 16, out.entry_rva) || !rd64(f, n, opt + 24, out.image_base) ||
        !rd32(f, n, opt + 56, out.size_of_image) || !rd32(f, n, opt + 60, out.size_of_headers) ||
        !rd32(f, n, opt + 108, ndirs)) {
        return fail("truncated optional header");
    }
    if (ndirs > 3 && (!rd32(f, n, opt + 112 + 3 * 8, exc_rva) ||
                      !rd32(f, n, opt + 112 + 3 * 8 + 4, exc_size))) {
        return fail("truncated data directories");
    }
    if (ndirs > 6 && (opt_size < 112U + 7U * 8U ||
                      !rd32(f, n, opt + 112 + 6 * 8, out.debug_rva) ||
                      !rd32(f, n, opt + 112 + 6 * 8 + 4, out.debug_size))) {
        return fail("truncated data directories");
    }
    const std::size_t sec0 = opt + opt_size;
    for (std::uint16_t i = 0; i < nsec; ++i) {
        const std::size_t s = sec0 + static_cast<std::size_t>(i) * 40;
        if (s + 40 > n) {
            return fail("truncated section table");
        }
        PeSection sec;
        char name[9] = {};
        std::memcpy(name, f + s, 8);
        sec.name = name;
        (void)rd32(f, n, s + 8, sec.virtual_size);
        (void)rd32(f, n, s + 12, sec.virtual_address);
        (void)rd32(f, n, s + 16, sec.raw_size);
        (void)rd32(f, n, s + 20, sec.raw_offset);
        (void)rd32(f, n, s + 36, sec.characteristics);
        if (static_cast<std::uint64_t>(sec.raw_offset) + sec.raw_size > n) {
            return fail("section data past the end of the file");
        }
        out.sections.push_back(sec);
    }
    if (exc_rva != 0 && exc_size >= 12) {
        const std::uint8_t* p = out.file_ptr(f, n, exc_rva, exc_size);
        if (p == nullptr) {
            return fail(".pdata not file-backed");
        }
        out.functions.reserve(exc_size / 12);
        for (std::uint32_t off = 0; off + 12 <= exc_size; off += 12) {
            PeFunctionRange r;
            std::memcpy(&r.begin, p + off, 4);
            std::memcpy(&r.end, p + off + 4, 4);
            std::memcpy(&r.unwind, p + off + 8, 4);
            r.entry = exc_rva + off;
            if (r.end > r.begin) {
                out.functions.push_back(r);
            }
        }
    }
    return true;
}

// Copies headers and every section into `dest` (`size_of_image` writable bytes, normally
// mapped at `image_base`). Uninitialized tails stay zero. No relocations, no imports.
inline bool map_pe_sections(const std::uint8_t* f, std::size_t n, const PeImage& img,
                            std::uint8_t* dest) noexcept {
    std::memset(dest, 0, img.size_of_image);
    if (img.size_of_headers > n || img.size_of_headers > img.size_of_image) {
        return false;
    }
    std::memcpy(dest, f, img.size_of_headers);
    for (const PeSection& s : img.sections) {
        const std::uint32_t copy = s.raw_size < s.virtual_size || s.virtual_size == 0
                                       ? s.raw_size
                                       : s.virtual_size;
        if (static_cast<std::uint64_t>(s.virtual_address) + copy > img.size_of_image) {
            return false;
        }
        std::memcpy(dest + s.virtual_address, f + s.raw_offset, copy);
    }
    return true;
}

// RSDS record of the debug directory (IMAGE_DEBUG_TYPE_CODEVIEW = 2): GUID, age, PDB path.
// One import address table slot of a mapped image (R3 "import bridge": shims register by name).
struct PeImport {
    std::uint64_t slot_va = 0;   // the IAT slot the exe calls through (`call [rip + slot]`)
    std::string dll;             // as written in the exe, e.g. "KERNEL32.dll"
    std::string name;            // empty when imported by ordinal
    std::uint16_t ordinal = 0;   // by-ordinal imports only
};

// Reads the import directory of an image mapped at `base` (`image` holds `size` bytes from the
// base; every read is bounds-checked). Order: descriptor order, then thunk order.
inline bool read_pe_imports(const std::uint8_t* image, std::size_t size, std::uint64_t base,
                            std::vector<PeImport>& out, std::string* error) {
    auto fail = [&](const char* what) {
        if (error != nullptr) {
            *error = what;
        }
        return false;
    };
    using pe_detail::rd32;
    using pe_detail::rd64;
    std::uint32_t pe = 0;
    std::uint32_t dir_rva = 0;
    std::uint32_t dir_size = 0;
    if (!rd32(image, size, 0x3C, pe) || !rd32(image, size, std::size_t{pe} + 24 + 112 + 8, dir_rva) ||
        !rd32(image, size, std::size_t{pe} + 24 + 112 + 12, dir_size)) {
        return fail("truncated headers");
    }
    out.clear();
    if (dir_rva == 0) {
        return true;  // no imports
    }
    auto c_string = [&](std::uint32_t rva, std::string& s) {
        s.clear();
        for (std::size_t i = rva; i < size; ++i) {
            if (image[i] == 0) {
                return true;
            }
            s.push_back(static_cast<char>(image[i]));
            if (s.size() > 4096) {
                return false;
            }
        }
        return false;
    };
    for (std::size_t d = dir_rva;; d += 20) {
        std::uint32_t oft = 0;
        std::uint32_t name_rva = 0;
        std::uint32_t ft = 0;
        if (!rd32(image, size, d, oft) || !rd32(image, size, d + 12, name_rva) || !rd32(image, size, d + 16, ft)) {
            return fail("truncated import directory");
        }
        if (oft == 0 && name_rva == 0 && ft == 0) {
            break;
        }
        std::string dll;
        if (!c_string(name_rva, dll)) {
            return fail("bad import DLL name");
        }
        const std::uint32_t names = oft != 0 ? oft : ft;
        for (std::size_t i = 0;; ++i) {
            std::uint64_t thunk = 0;
            if (!rd64(image, size, names + 8 * i, thunk)) {
                return fail("truncated import lookup table");
            }
            if (thunk == 0) {
                break;
            }
            PeImport imp;
            imp.slot_va = base + ft + 8 * i;
            imp.dll = dll;
            if ((thunk >> 63) != 0) {
                imp.ordinal = static_cast<std::uint16_t>(thunk & 0xFFFF);
            } else if (!c_string(static_cast<std::uint32_t>(thunk & 0x7FFFFFFF) + 2, imp.name)) {
                return fail("bad import name");
            }
            out.push_back(std::move(imp));
            if (i > 65536) {
                return fail("import lookup table too long");
            }
        }
    }
    return true;
}

// The TLS directory of a mapped image (IMAGE_TLS_DIRECTORY64, data directory 9; R3 static TLS,
// `teb.h`). Its fields are VAs (they carry base relocations), not RVAs.
struct PeTls {
    bool present = false;
    std::uint64_t raw_begin = 0;   // the template copied into every thread's block
    std::uint64_t raw_end = 0;
    std::uint64_t index_va = 0;    // `_tls_index`: the loader writes this module's slot number
    std::uint32_t zero_fill = 0;   // bytes after the template, zeroed
    std::uint32_t alignment = 0;   // block alignment from Characteristics bits 20-23 (0 = default)
    std::vector<std::uint64_t> callbacks;  // PIMAGE_TLS_CALLBACK list (null-terminated in the image)
};

// Reads the TLS directory of an image mapped at `base` (`image` holds `size` bytes). A missing
// directory is not an error (`out.present` stays false); every VA must lie inside the image.
inline bool read_pe_tls(const std::uint8_t* image, std::size_t size, std::uint64_t base, PeTls& out,
                        std::string* error) {
    auto fail = [&](const char* what) {
        if (error != nullptr) {
            *error = what;
        }
        return false;
    };
    using pe_detail::rd32;
    using pe_detail::rd64;
    out = PeTls{};
    std::uint32_t pe = 0;
    std::uint32_t dir = 0;
    std::uint32_t dir_size = 0;
    if (!rd32(image, size, 0x3C, pe) || !rd32(image, size, std::size_t{pe} + 24 + 112 + 9 * 8, dir) ||
        !rd32(image, size, std::size_t{pe} + 24 + 112 + 9 * 8 + 4, dir_size)) {
        return fail("truncated headers");
    }
    if (dir == 0) {
        return true;
    }
    std::uint64_t callbacks = 0;
    std::uint32_t characteristics = 0;
    if (dir_size < 40 || !rd64(image, size, dir, out.raw_begin) || !rd64(image, size, dir + 8ULL, out.raw_end) ||
        !rd64(image, size, dir + 16ULL, out.index_va) || !rd64(image, size, dir + 24ULL, callbacks) ||
        !rd32(image, size, dir + 32ULL, out.zero_fill) || !rd32(image, size, dir + 36ULL, characteristics)) {
        return fail("truncated TLS directory");
    }
    auto inside = [&](std::uint64_t va, std::uint64_t n) {
        return va >= base && va - base <= size && n <= size - (va - base);
    };
    if (out.raw_end < out.raw_begin || (out.raw_begin != 0 && !inside(out.raw_begin, out.raw_end - out.raw_begin)) ||
        (out.index_va != 0 && !inside(out.index_va, 4)) || out.zero_fill > (1U << 24)) {
        return fail("TLS directory points outside the image");
    }
    const std::uint32_t align_code = (characteristics >> 20) & 0xF;
    out.alignment = align_code == 0 ? 0 : 1U << (align_code - 1);
    for (std::uint64_t at = callbacks; at != 0; at += 8) {
        std::uint64_t cb = 0;
        if (!inside(at, 8) || !rd64(image, size, at - base, cb)) {
            return fail("TLS callback list runs outside the image");
        }
        if (cb == 0) {
            break;
        }
        if (!inside(cb, 1) || out.callbacks.size() >= 256) {
            return fail("bad TLS callback");
        }
        out.callbacks.push_back(cb);
    }
    out.present = true;
    return true;
}

struct PeCodeView {
    std::array<std::uint8_t, 16> guid{};
    std::uint32_t age = 0;
    std::string pdb_path;
};

// Reads the first CodeView (RSDS) debug directory record from a PE32+ file buffer.
inline bool read_pe_codeview(const std::uint8_t* file, std::size_t size, const PeImage& image,
                             PeCodeView& out, std::string* error) {
    using namespace pe_detail;
    out = PeCodeView{};
    const auto fail = [&](const char* m) {
        if (error != nullptr) {
            *error = m;
        }
        return false;
    };
    if (file == nullptr || size == 0U) {
        return fail("empty PE buffer");
    }
    if (image.debug_rva == 0U || image.debug_size == 0U) {
        return fail("PE has no debug directory");
    }
    if (image.debug_size < 28U) {
        return fail("PE debug directory too small");
    }
    const std::uint8_t* dir = image.file_ptr(file, size, image.debug_rva, image.debug_size);
    if (dir == nullptr) {
        return fail("PE debug directory not file-backed");
    }
    constexpr std::uint32_t kImageDebugTypeCodeView = 2U;
    for (std::uint64_t off = 0; off + 28ULL <= image.debug_size; off += 28ULL) {
        std::uint32_t type = 0;
        std::uint32_t raw_size = 0;
        std::uint32_t raw_off = 0;
        (void)rd32(dir, image.debug_size, static_cast<std::size_t>(off) + 12U, type);
        (void)rd32(dir, image.debug_size, static_cast<std::size_t>(off) + 16U, raw_size);
        (void)rd32(dir, image.debug_size, static_cast<std::size_t>(off) + 24U, raw_off);
        if (type != kImageDebugTypeCodeView) {
            continue;
        }
        if (static_cast<std::uint64_t>(raw_off) > size ||
            static_cast<std::uint64_t>(raw_size) > static_cast<std::uint64_t>(size) - raw_off) {
            return fail("CodeView debug data extends past end of file");
        }
        const std::uint8_t* cv = file + raw_off;
        if (raw_size < 4U) {
            return fail("truncated CodeView record");
        }
        if (std::memcmp(cv, "RSDS", 4) != 0) {
            return fail("unsupported CodeView signature (expected RSDS)");
        }
        if (raw_size < 25U) {
            return fail("truncated RSDS CodeView record");
        }
        std::memcpy(out.guid.data(), cv + 4, 16);
        (void)rd32(cv, raw_size, 20U, out.age);
        const void* nul = std::memchr(cv + 24, '\0', static_cast<std::size_t>(raw_size) - 24U);
        if (nul == nullptr) {
            return fail("unterminated RSDS PDB path");
        }
        const std::size_t path_len =
            static_cast<std::size_t>(static_cast<const std::uint8_t*>(nul) - (cv + 24));
        out.pdb_path.assign(reinterpret_cast<const char*>(cv + 24), path_len);
        return true;
    }
    return fail("no CodeView entry in PE debug directory");
}

// --- R4 multi-module runtime (`docs/RECOMP_PLAN.md` "R4 design", slice 2) ----------------------
// Readers over a *mapped* image (`image` holds `size` bytes from its base), like `read_pe_imports`.

namespace pe_detail {

// Data directory `index` of a mapped image (RVA and size); false when the headers are truncated.
inline bool data_directory(const std::uint8_t* image, std::size_t size, unsigned index, std::uint32_t& rva,
                           std::uint32_t& dir_size) noexcept {
    std::uint32_t pe = 0;
    std::uint32_t ndirs = 0;
    rva = 0;
    dir_size = 0;
    if (!rd32(image, size, 0x3C, pe) || !rd32(image, size, std::size_t{pe} + 24 + 108, ndirs)) {
        return false;
    }
    if (index >= ndirs) {
        return true;
    }
    return rd32(image, size, std::size_t{pe} + 24 + 112 + 8 * std::size_t{index}, rva) &&
           rd32(image, size, std::size_t{pe} + 24 + 112 + 8 * std::size_t{index} + 4, dir_size);
}

inline bool c_string(const std::uint8_t* image, std::size_t size, std::uint32_t rva, std::string& s) {
    s.clear();
    for (std::size_t i = rva; i < size; ++i) {
        if (image[i] == 0) {
            return true;
        }
        s.push_back(static_cast<char>(image[i]));
        if (s.size() > 4096) {
            return false;
        }
    }
    return false;
}

}  // namespace pe_detail

// One exported function (export address table entry). `name` is empty for an ordinal-only export;
// `forwarder` ("NTDLL.RtlUnwind") is set when the entry points into the export directory itself.
struct PeExport {
    std::string name;
    std::uint16_t ordinal = 0;  // biased (the number `GetProcAddress(h, MAKEINTRESOURCE(n))` takes)
    std::uint32_t rva = 0;
    std::string forwarder;
};

struct PeExports {
    std::string dll_name;  // the directory's Name field
    std::vector<PeExport> entries;  // address-table order; an entry with several names appears once per name
};

// Reads the export directory (data directory 0). A missing directory is not an error.
inline bool read_pe_exports(const std::uint8_t* image, std::size_t size, PeExports& out, std::string* error) {
    using pe_detail::rd16;
    using pe_detail::rd32;
    auto fail = [&](const char* what) {
        if (error != nullptr) {
            *error = what;
        }
        return false;
    };
    out = PeExports{};
    std::uint32_t dir = 0;
    std::uint32_t dir_size = 0;
    if (!pe_detail::data_directory(image, size, 0, dir, dir_size)) {
        return fail("truncated headers");
    }
    if (dir == 0) {
        return true;
    }
    std::uint32_t name_rva = 0;
    std::uint32_t ord_base = 0;
    std::uint32_t n_funcs = 0;
    std::uint32_t n_names = 0;
    std::uint32_t funcs = 0;
    std::uint32_t names = 0;
    std::uint32_t ords = 0;
    if (!rd32(image, size, std::size_t{dir} + 0x0C, name_rva) || !rd32(image, size, std::size_t{dir} + 0x10, ord_base) ||
        !rd32(image, size, std::size_t{dir} + 0x14, n_funcs) || !rd32(image, size, std::size_t{dir} + 0x18, n_names) ||
        !rd32(image, size, std::size_t{dir} + 0x1C, funcs) || !rd32(image, size, std::size_t{dir} + 0x20, names) ||
        !rd32(image, size, std::size_t{dir} + 0x24, ords)) {
        return fail("truncated export directory");
    }
    if (n_funcs > 65536 || n_names > 65536) {
        return fail("export directory too large");
    }
    if (name_rva != 0 && !pe_detail::c_string(image, size, name_rva, out.dll_name)) {
        return fail("bad export DLL name");
    }
    std::vector<std::vector<std::string>> named(n_funcs);
    for (std::uint32_t i = 0; i < n_names; ++i) {
        std::uint32_t nr = 0;
        std::uint16_t index = 0;
        std::string nm;
        if (!rd32(image, size, std::size_t{names} + 4 * std::size_t{i}, nr) ||
            !rd16(image, size, std::size_t{ords} + 2 * std::size_t{i}, index) ||
            !pe_detail::c_string(image, size, nr, nm)) {
            return fail("bad export name");
        }
        if (index >= n_funcs) {
            return fail("export name ordinal out of range");
        }
        named[index].push_back(std::move(nm));
    }
    for (std::uint32_t i = 0; i < n_funcs; ++i) {
        PeExport e;
        if (!rd32(image, size, std::size_t{funcs} + 4 * std::size_t{i}, e.rva)) {
            return fail("truncated export address table");
        }
        if (e.rva == 0) {
            continue;  // an unused ordinal
        }
        e.ordinal = static_cast<std::uint16_t>(ord_base + i);
        if (e.rva >= dir && e.rva - dir < dir_size && !pe_detail::c_string(image, size, e.rva, e.forwarder)) {
            return fail("bad export forwarder");
        }
        if (named[i].empty()) {
            out.entries.push_back(e);
        }
        for (std::string& nm : named[i]) {
            e.name = std::move(nm);
            out.entries.push_back(e);
        }
    }
    return true;
}

// Moves a mapped image to `new_base`: applies its base relocations (data directory 5; types
// ABSOLUTE, HIGHLOW and DIR64, the only ones an x64 linker emits) and rewrites the header's
// ImageBase, as the Windows loader does. A move of an image with relocations stripped
// (IMAGE_FILE_RELOCS_STRIPPED, e.g. `lld-link -fixed`) fails; `applied` counts the fixups.
inline bool apply_pe_relocations(std::uint8_t* image, std::size_t size, std::uint64_t new_base, std::size_t* applied,
                                 std::string* error) {
    using pe_detail::rd16;
    using pe_detail::rd32;
    using pe_detail::rd64;
    auto fail = [&](const char* what) {
        if (error != nullptr) {
            *error = what;
        }
        return false;
    };
    if (applied != nullptr) {
        *applied = 0;
    }
    std::uint32_t pe = 0;
    std::uint16_t characteristics = 0;
    std::uint64_t old_base = 0;
    if (!rd32(image, size, 0x3C, pe) || !rd16(image, size, std::size_t{pe} + 22, characteristics) ||
        !rd64(image, size, std::size_t{pe} + 24 + 24, old_base)) {
        return fail("truncated headers");
    }
    const std::uint64_t delta = new_base - old_base;
    if (delta == 0) {
        return true;
    }
    std::uint32_t dir = 0;
    std::uint32_t dir_size = 0;
    if (!pe_detail::data_directory(image, size, 5, dir, dir_size)) {
        return fail("truncated headers");
    }
    if ((characteristics & 1U) != 0) {
        return fail("the image has its relocations stripped and cannot move");
    }
    std::size_t n = 0;
    for (std::size_t block = dir; dir != 0 && block + 8 <= std::size_t{dir} + dir_size;) {
        std::uint32_t page = 0;
        std::uint32_t block_size = 0;
        if (!rd32(image, size, block, page) || !rd32(image, size, block + 4, block_size)) {
            return fail("truncated relocation block");
        }
        if (block_size < 8 || block + block_size > std::size_t{dir} + dir_size) {
            return fail("bad relocation block size");
        }
        for (std::size_t e = block + 8; e + 2 <= block + block_size; e += 2) {
            std::uint16_t entry = 0;
            (void)rd16(image, size, e, entry);
            const unsigned type = entry >> 12;
            const std::size_t at = std::size_t{page} + (entry & 0xFFFU);
            if (type == 0) {
                continue;  // IMAGE_REL_BASED_ABSOLUTE: padding
            }
            if (type == 3) {  // IMAGE_REL_BASED_HIGHLOW
                std::uint32_t v = 0;
                if (!rd32(image, size, at, v)) {
                    return fail("relocation outside the image");
                }
                v += static_cast<std::uint32_t>(delta);
                std::memcpy(image + at, &v, 4);
            } else if (type == 10) {  // IMAGE_REL_BASED_DIR64
                std::uint64_t v = 0;
                if (!rd64(image, size, at, v)) {
                    return fail("relocation outside the image");
                }
                v += delta;
                std::memcpy(image + at, &v, 8);
            } else {
                return fail("unsupported relocation type");
            }
            ++n;
        }
        block += block_size;
    }
    std::memcpy(image + pe + 24 + 24, &new_base, 8);
    if (applied != nullptr) {
        *applied = n;
    }
    return true;
}

// The values of every DIR64 base relocation of a mapped image (after `apply_pe_relocations`): the
// absolute addresses the image stores in its data (function-pointer tables, vtables, CRT init
// tables, callbacks). R4 function discovery takes those inside `.text` as function entries when no
// PDB names the functions. Empty when the image has no relocations.
inline std::vector<std::uint64_t> read_pe_dir64_values(const std::uint8_t* image, std::size_t size) {
    using pe_detail::rd16;
    using pe_detail::rd32;
    using pe_detail::rd64;
    std::vector<std::uint64_t> out;
    std::uint32_t dir = 0;
    std::uint32_t dir_size = 0;
    if (!pe_detail::data_directory(image, size, 5, dir, dir_size) || dir == 0) {
        return out;
    }
    for (std::size_t block = dir; block + 8 <= std::size_t{dir} + dir_size;) {
        std::uint32_t page = 0;
        std::uint32_t block_size = 0;
        if (!rd32(image, size, block, page) || !rd32(image, size, block + 4, block_size) || block_size < 8) {
            break;
        }
        for (std::size_t e = block + 8; e + 2 <= block + block_size; e += 2) {
            std::uint16_t entry = 0;
            std::uint64_t v = 0;
            if (rd16(image, size, e, entry) && (entry >> 12) == 10 && rd64(image, size, std::size_t{page} + (entry & 0xFFFU), v)) {
                out.push_back(v);
            }
        }
        block += block_size;
    }
    return out;
}

// A resource type, name or language key: an integer ID, or a string (compared case-insensitively,
// as `FindResource` compares: resource compilers store names upper-case).
struct PeResourceKey {
    std::uint32_t id = 0;
    std::u16string name;  // empty: the key is `id`
};

enum class PeResourceMiss { none, no_directory, type, name, language };

namespace pe_detail {

inline char16_t upper16(char16_t c) noexcept {
    return c >= u'a' && c <= u'z' ? static_cast<char16_t>(c - u'a' + u'A') : c;
}

// One level of the resource tree: the entry whose key matches; returns its OffsetToData (with the
// subdirectory bit) or 0.
inline std::uint32_t resource_entry(const std::uint8_t* image, std::size_t size, std::uint32_t root,
                                    std::uint32_t table, const PeResourceKey* key, std::uint32_t* first_id) {
    std::uint16_t n_named = 0;
    std::uint16_t n_id = 0;
    if (!rd16(image, size, std::size_t{table} + 12, n_named) || !rd16(image, size, std::size_t{table} + 14, n_id)) {
        return 0;
    }
    const unsigned total = unsigned{n_named} + n_id;
    for (unsigned i = 0; i < total && i < 4096; ++i) {
        const std::size_t e = std::size_t{table} + 16 + 8 * std::size_t{i};
        std::uint32_t name = 0;
        std::uint32_t data = 0;
        if (!rd32(image, size, e, name) || !rd32(image, size, e + 4, data)) {
            return 0;
        }
        if (key == nullptr) {  // the first entry
            if (first_id != nullptr) {
                *first_id = (name & 0x80000000U) != 0 ? 0 : name;
            }
            return data;
        }
        if ((name & 0x80000000U) != 0) {
            if (key->name.empty()) {
                continue;
            }
            const std::size_t s = std::size_t{root} + (name & 0x7FFFFFFFU);
            std::uint16_t len = 0;
            if (!rd16(image, size, s, len) || len != key->name.size() || s + 2 + 2 * std::size_t{len} > size) {
                continue;
            }
            bool same = true;
            for (std::size_t k = 0; k < len && same; ++k) {
                std::uint16_t ch = 0;
                (void)rd16(image, size, s + 2 + 2 * k, ch);
                same = upper16(static_cast<char16_t>(ch)) == upper16(key->name[k]);
            }
            if (same) {
                return data;
            }
        } else if (key->name.empty() && name == key->id) {
            return data;
        }
    }
    return 0;
}

}  // namespace pe_detail

// `FindResourceExW` over the resource directory (data directory 2): the RVA of the
// IMAGE_RESOURCE_DATA_ENTRY for `type` / `name` / `language`, or 0 with `miss` saying which level
// failed. `language` < 0 asks for `FindResource`'s choice: neutral (0), then en-US (0x409), then the
// first language present (the runtime's locale is en-US: `shim_strings.h`).
inline std::uint32_t find_pe_resource(const std::uint8_t* image, std::size_t size, const PeResourceKey& type,
                                      const PeResourceKey& name, int language, PeResourceMiss* miss) {
    auto missed = [&](PeResourceMiss m) {
        if (miss != nullptr) {
            *miss = m;
        }
        return std::uint32_t{0};
    };
    std::uint32_t root = 0;
    std::uint32_t root_size = 0;
    if (!pe_detail::data_directory(image, size, 2, root, root_size) || root == 0) {
        return missed(PeResourceMiss::no_directory);
    }
    const std::uint32_t t = pe_detail::resource_entry(image, size, root, root, &type, nullptr);
    if ((t & 0x80000000U) == 0) {
        return missed(PeResourceMiss::type);
    }
    const std::uint32_t nm = pe_detail::resource_entry(image, size, root, root + (t & 0x7FFFFFFFU), &name, nullptr);
    if ((nm & 0x80000000U) == 0) {
        return missed(PeResourceMiss::name);
    }
    const std::uint32_t langs = root + (nm & 0x7FFFFFFFU);
    std::uint32_t leaf = 0;
    if (language >= 0) {
        PeResourceKey k;
        k.id = static_cast<std::uint32_t>(language);
        leaf = pe_detail::resource_entry(image, size, root, langs, &k, nullptr);
    } else {
        for (const std::uint32_t want : {0U, 0x409U}) {
            PeResourceKey k;
            k.id = want;
            leaf = pe_detail::resource_entry(image, size, root, langs, &k, nullptr);
            if (leaf != 0) {
                break;
            }
        }
        if (leaf == 0) {
            leaf = pe_detail::resource_entry(image, size, root, langs, nullptr, nullptr);
        }
    }
    if (leaf == 0 || (leaf & 0x80000000U) != 0 || std::size_t{root} + leaf + 16 > size) {
        return missed(PeResourceMiss::language);
    }
    if (miss != nullptr) {
        *miss = PeResourceMiss::none;
    }
    return root + leaf;
}

}  // namespace wse::recomp
