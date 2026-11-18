// Minimal PE32+ reader for the recompiler: headers, sections, `.pdata` and image mapping.
//
// Reads the user's own exe from memory (it is never bundled). Only what R0 needs: the
// preferred base and size (the image maps at its preferred base, `docs/RECOMP_PLAN.md`), section
// copies, and the `.pdata` function ranges used for function discovery. Every offset is
// bounds-checked; a malformed file fails with a message instead of reading out of range.

#pragma once

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
};

struct PeImage {
    std::uint64_t image_base = 0;
    std::uint32_t size_of_image = 0;
    std::uint32_t size_of_headers = 0;
    std::uint32_t entry_rva = 0;
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

}  // namespace wse::recomp
