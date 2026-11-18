// Platform seam: Android custom Vulkan driver package import (AdrenoTools-style
// `meta.json` + ICD `.so`), ELF `libhardware.so` rewrite, and staging through a
// `Vfs` (`docs/PLATFORM_PLAN.md`, slice P4).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.6 layout
// and carries no PE/PDB citations.

#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wse/core/json.h"
#include "wse/platform/rhi.h"
#include "wse/platform/vfs.h"

namespace wse::platform {

// Metadata schema used by AdrenoTools driver packages (`The412Banner/Banners-Turnip`
// and `FristOneRR/FristOneRR-Panvk-Driver` `meta.json`).
struct DriverPackageMeta {
    int schema_version = 0;
    std::string name{};
    std::string description{};
    std::string author{};
    std::string package_version{};
    std::string vendor{};
    std::string driver_version{};
    int min_api = 0;
    std::string library_name{};
    DriverMode inferred_mode = DriverMode::custom_icd;
};

namespace rhi_detail {

inline bool parse_exact_int(std::string_view text, int& out) noexcept {
    if (text.empty()) {
        return false;
    }
    int val = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), val);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    out = val;
    return true;
}

inline bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) {
        return false;
    }
    return ascii_lower(s.substr(s.size() - suffix.size())) == ascii_lower(suffix);
}

}  // namespace rhi_detail

// Longest accepted `libraryName` (well under every file system's name limit).
inline constexpr std::size_t kMaxDriverLibraryNameLength = 128;

// Validates that a package `libraryName` is a plain shared-library file name.
// Allow-list: 1..128 characters from `[A-Za-z0-9._+-]`, not starting with `.`
// or `-` (hidden files, option-like names), no `..`, and a `.so` / `.dll`
// suffix with a non-empty stem. Separators, drive letters, spaces, control and
// non-ASCII bytes are all rejected by the allow-list itself.
[[nodiscard]] inline bool is_safe_driver_library_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxDriverLibraryNameLength) {
        return false;
    }
    if (name.front() == '.' || name.front() == '-' || name.find("..") != std::string_view::npos) {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-';
        if (!ok) {
            return false;
        }
    }
    const bool so = rhi_detail::ends_with_ci(name, ".so") && name.size() > 3U;
    const bool dll = rhi_detail::ends_with_ci(name, ".dll") && name.size() > 4U;
    return so || dll;
}

// Parses an AdrenoTools / Winlator driver `meta.json` payload.
[[nodiscard]] inline bool parse_driver_meta_json(std::string_view json_text,
                                                 DriverPackageMeta& out,
                                                 std::string* error_out = nullptr) {
    out = DriverPackageMeta{};
    std::map<std::string, std::string> fields;
    // Packages may carry structured fields (arrays/objects) we do not use; skip
    // them rather than rejecting the whole file.
    if (!wse::core::wse_parse_json_object(json_text, fields, /*skip_nested=*/true)) {
        return rhi_detail::set_error(error_out, "invalid JSON in driver meta.json");
    }

    const auto find_str = [&](std::string_view key) -> std::string {
        const auto it = fields.find(std::string(key));
        return (it != fields.end()) ? it->second : std::string{};
    };

    int schema = 0;
    const auto schema_it = fields.find("schemaVersion");
    if (schema_it == fields.end() || !rhi_detail::parse_exact_int(schema_it->second, schema) ||
        schema != 1) {
        return rhi_detail::set_error(error_out, "driver meta.json requires schemaVersion == 1");
    }
    out.schema_version = schema;

    out.library_name = find_str("libraryName");
    if (!is_safe_driver_library_name(out.library_name)) {
        return rhi_detail::set_error(
            error_out, "driver meta.json contains unsafe or unsupported libraryName");
    }

    out.name = find_str("name");
    out.description = find_str("description");
    out.author = find_str("author");
    out.package_version = find_str("packageVersion");
    out.vendor = find_str("vendor");
    out.driver_version = find_str("driverVersion");

    if (const auto api_it = fields.find("minApi"); api_it != fields.end()) {
        int parsed_api = 0;
        if (!rhi_detail::parse_exact_int(api_it->second, parsed_api) || parsed_api < 0) {
            return rhi_detail::set_error(error_out, "invalid minApi in driver meta.json");
        }
        out.min_api = parsed_api;
    }

    const std::string lib_lower = ascii_lower(out.library_name);
    const std::string name_lower = ascii_lower(out.name);
    if (lib_lower.find("freedreno") != std::string::npos ||
        name_lower.find("turnip") != std::string::npos) {
        out.inferred_mode = DriverMode::turnip;
    } else if (lib_lower.find("panfrost") != std::string::npos ||
               name_lower.find("panvk") != std::string::npos) {
        out.inferred_mode = DriverMode::panvk;
    } else {
        out.inferred_mode = DriverMode::custom_icd;
    }

    return true;
}

// Checks an already-parsed package against the device: `minApi` (when the
// package states one and `device_api_level > 0`) and, for a specific
// `requested` mode (`turnip` / `panvk`), that a package whose kind could be
// inferred is that kind. `custom_icd` packages match any request.
[[nodiscard]] inline bool check_driver_package_compat(const DriverPackageMeta& meta,
                                                      int device_api_level,
                                                      DriverMode requested,
                                                      std::string* error_out = nullptr) {
    if (meta.min_api > 0 && device_api_level > 0 && device_api_level < meta.min_api) {
        return rhi_detail::set_error(
            error_out, "driver package needs Android API " + std::to_string(meta.min_api) +
                           " but this device is API " + std::to_string(device_api_level));
    }
    const bool specific_request =
        requested == DriverMode::turnip || requested == DriverMode::panvk;
    const bool specific_package =
        meta.inferred_mode == DriverMode::turnip || meta.inferred_mode == DriverMode::panvk;
    if (specific_request && specific_package && meta.inferred_mode != requested) {
        return rhi_detail::set_error(
            error_out, "driver package is " + std::string(driver_mode_name(meta.inferred_mode)) +
                           " but " + std::string(driver_mode_name(requested)) + " was requested");
    }
    return true;
}

namespace rhi_detail {

// Little-endian ELF reader with every offset bounds-checked against the input.
struct ElfView {
    std::span<const std::uint8_t> bytes{};
    bool is64 = false;

    [[nodiscard]] bool in_range(std::uint64_t off, std::uint64_t len) const noexcept {
        return off <= bytes.size() && len <= bytes.size() - off;
    }
    [[nodiscard]] std::uint64_t rd(std::uint64_t off, unsigned width) const noexcept {
        std::uint64_t v = 0;
        for (unsigned i = 0; i < width; ++i) {
            v |= static_cast<std::uint64_t>(bytes[static_cast<std::size_t>(off) + i]) << (8U * i);
        }
        return v;
    }
    [[nodiscard]] unsigned word() const noexcept { return is64 ? 8U : 4U; }
};

[[nodiscard]] inline bool open_elf(std::span<const std::uint8_t> bytes, ElfView& out) noexcept {
    if (bytes.size() < 52U || bytes[0] != 0x7FU || bytes[1] != 'E' || bytes[2] != 'L' ||
        bytes[3] != 'F') {
        return false;
    }
    const std::uint8_t ei_class = bytes[4];
    const std::uint8_t ei_data = bytes[5];
    if ((ei_class != 1U && ei_class != 2U) || ei_data != 1U) {
        return false;  // ELF32/ELF64 little-endian only (every Android ABI).
    }
    out.bytes = bytes;
    out.is64 = (ei_class == 2U);
    return out.in_range(0, out.is64 ? 64U : 52U);
}

// Maps a virtual address to a file offset through the PT_LOAD segments.
[[nodiscard]] inline bool elf_vaddr_to_offset(const ElfView& elf,
                                              std::uint64_t vaddr,
                                              std::uint64_t& out_off) noexcept {
    const std::uint64_t phoff = elf.rd(elf.is64 ? 32U : 28U, elf.word());
    const std::uint64_t phentsize = elf.rd(elf.is64 ? 54U : 42U, 2U);
    const std::uint64_t phnum = elf.rd(elf.is64 ? 56U : 44U, 2U);
    const std::uint64_t min_ent = elf.is64 ? 56U : 32U;
    if (phentsize < min_ent) {
        return false;
    }
    for (std::uint64_t i = 0; i < phnum; ++i) {
        const std::uint64_t ph = phoff + i * phentsize;
        if (!elf.in_range(ph, min_ent)) {
            return false;
        }
        if (elf.rd(ph, 4U) != 1U) {  // PT_LOAD
            continue;
        }
        const std::uint64_t p_offset = elf.rd(ph + (elf.is64 ? 8U : 4U), elf.word());
        const std::uint64_t p_vaddr = elf.rd(ph + (elf.is64 ? 16U : 8U), elf.word());
        const std::uint64_t p_filesz = elf.rd(ph + (elf.is64 ? 32U : 16U), elf.word());
        if (vaddr >= p_vaddr && vaddr - p_vaddr < p_filesz) {
            out_off = p_offset + (vaddr - p_vaddr);
            return true;
        }
    }
    return false;
}

}  // namespace rhi_detail

// True when `bytes` starts with a well-formed little-endian ELF32/ELF64 header.
[[nodiscard]] inline bool is_little_endian_elf(std::span<const std::uint8_t> bytes) noexcept {
    rhi_detail::ElfView elf{};
    return rhi_detail::open_elf(bytes, elf);
}

// Rewrites the dynamic-string-table name `"libhardware.so"` to `"libpanvk_hw.so"`
// (same 14-byte length, so no ELF offset moves) wherever the dynamic section
// references it: `DT_NEEDED` entries and `DT_VERNEED` `vn_file` names. Mesa's
// Android Vulkan drivers (PanVK and Turnip) link `libhardware.so`, a VNDK-SP
// library outside an app's linker namespace; the bundled `libpanvk_hw.so` routes
// `hw_get_module` into the drivers' supported `-ENOENT` fallback path
// (`docs/PLATFORM_PLAN.md` §P4).
//
// Only strings reached through `PT_DYNAMIC` -> `DT_STRTAB` are touched, so the
// same text in `.rodata` (log messages, dlopen paths) is left alone. Mirrors
// `tools/patch_panvk.py`. Returns the number of strings rewritten; 0 for
// non-ELF input or an ELF with no `libhardware.so` dependency.
[[nodiscard]] inline std::size_t patch_icd_libhardware_needed(
    std::span<std::uint8_t> elf_bytes) noexcept {
    static constexpr char kFrom[] = "libhardware.so";
    static constexpr char kTo[] = "libpanvk_hw.so";
    static constexpr std::size_t kNeedleLen = sizeof(kFrom);  // includes trailing NUL
    static_assert(sizeof(kFrom) == sizeof(kTo), "replacement SONAME length must match");

    rhi_detail::ElfView elf{};
    if (!rhi_detail::open_elf(elf_bytes, elf)) {
        return 0;
    }

    // Locate PT_DYNAMIC.
    const std::uint64_t phoff = elf.rd(elf.is64 ? 32U : 28U, elf.word());
    const std::uint64_t phentsize = elf.rd(elf.is64 ? 54U : 42U, 2U);
    const std::uint64_t phnum = elf.rd(elf.is64 ? 56U : 44U, 2U);
    const std::uint64_t min_ent = elf.is64 ? 56U : 32U;
    if (phentsize < min_ent) {
        return 0;
    }
    std::uint64_t dyn_off = 0;
    std::uint64_t dyn_size = 0;
    for (std::uint64_t i = 0; i < phnum; ++i) {
        const std::uint64_t ph = phoff + i * phentsize;
        if (!elf.in_range(ph, min_ent)) {
            return 0;
        }
        if (elf.rd(ph, 4U) == 2U) {  // PT_DYNAMIC
            dyn_off = elf.rd(ph + (elf.is64 ? 8U : 4U), elf.word());
            dyn_size = elf.rd(ph + (elf.is64 ? 32U : 16U), elf.word());
            break;
        }
    }
    if (dyn_size == 0 || !elf.in_range(dyn_off, dyn_size)) {
        return 0;
    }

    // Walk the dynamic array once.
    const std::uint64_t ent = 2U * elf.word();
    std::uint64_t strtab_vaddr = 0;
    std::uint64_t strsz = 0;
    std::uint64_t verneed_vaddr = 0;
    std::uint64_t verneed_num = 0;
    bool have_strtab = false;
    std::vector<std::uint64_t> name_offsets;
    for (std::uint64_t pos = dyn_off; pos + ent <= dyn_off + dyn_size; pos += ent) {
        const std::uint64_t tag = elf.rd(pos, elf.word());
        const std::uint64_t val = elf.rd(pos + elf.word(), elf.word());
        if (tag == 0U) {  // DT_NULL
            break;
        }
        switch (tag) {
            case 1U:  // DT_NEEDED
                name_offsets.push_back(val);
                break;
            case 5U:  // DT_STRTAB
                strtab_vaddr = val;
                have_strtab = true;
                break;
            case 10U:  // DT_STRSZ
                strsz = val;
                break;
            case 0x6ffffffeU:  // DT_VERNEED
                verneed_vaddr = val;
                break;
            case 0x6fffffffU:  // DT_VERNEEDNUM
                verneed_num = val;
                break;
            default:
                break;
        }
    }
    std::uint64_t strtab_off = 0;
    if (!have_strtab || strsz == 0 ||
        !rhi_detail::elf_vaddr_to_offset(elf, strtab_vaddr, strtab_off) ||
        !elf.in_range(strtab_off, strsz)) {
        return 0;
    }

    // `Elf32_Verneed` and `Elf64_Verneed` share one 16-byte layout:
    // vn_version u16, vn_cnt u16, vn_file u32, vn_aux u32, vn_next u32.
    std::uint64_t vn_off = 0;
    if (verneed_vaddr != 0U && verneed_num != 0U &&
        rhi_detail::elf_vaddr_to_offset(elf, verneed_vaddr, vn_off)) {
        for (std::uint64_t n = 0; n < verneed_num && n < 4096U; ++n) {
            if (!elf.in_range(vn_off, 16U)) {
                break;
            }
            name_offsets.push_back(elf.rd(vn_off + 4U, 4U));
            const std::uint64_t next = elf.rd(vn_off + 12U, 4U);
            if (next == 0U) {
                break;
            }
            vn_off += next;
        }
    }

    std::sort(name_offsets.begin(), name_offsets.end());
    name_offsets.erase(std::unique(name_offsets.begin(), name_offsets.end()), name_offsets.end());

    std::size_t patched = 0;
    for (const std::uint64_t name : name_offsets) {
        if (name > strsz || kNeedleLen > strsz - name) {
            continue;
        }
        const std::size_t at = static_cast<std::size_t>(strtab_off + name);
        if (std::memcmp(elf_bytes.data() + at, kFrom, kNeedleLen) == 0) {
            std::memcpy(elf_bytes.data() + at, kTo, kNeedleLen);
            ++patched;
        }
    }
    return patched;
}

namespace rhi_detail {

// Writes `data` to `path` without ever truncating an existing `path` in place:
// the bytes go to `<path>.partial`, which then replaces `path` through
// `Vfs::rename_file`. A live mapping of the old file (a driver that is still
// `dlopen`ed, which `RTLD_NODELETE` makes permanent) keeps its own storage, and
// an interrupted write leaves the old file intact. A Vfs without rename support
// gets remove + write, which still gives the new file fresh storage.
[[nodiscard]] inline bool replace_file_atomically(Vfs& vfs,
                                                  const std::string& path,
                                                  std::span<const std::uint8_t> data) {
    const std::string tmp = path + ".partial";
    (void)vfs.remove_file(tmp);
    if (!vfs.write_file(tmp, data)) {
        (void)vfs.remove_file(tmp);
        return false;
    }
    if (vfs.rename_file(tmp, path)) {
        return true;
    }
    (void)vfs.remove_file(tmp);
    if (vfs.is_file(path) && !vfs.remove_file(path)) {
        return false;
    }
    return vfs.write_file(path, data);
}

// Creates `dir` and any missing parents (`/` or `\\` separated). True when
// `dir` exists afterwards.
[[nodiscard]] inline bool make_dirs(Vfs& vfs, std::string_view dir) {
    if (dir.empty() || vfs.is_dir(dir)) {
        return true;
    }
    std::size_t pos = 0;
    while (pos <= dir.size()) {
        const std::size_t sep = dir.find_first_of("/\\", pos);
        const std::string_view prefix = (sep == std::string_view::npos) ? dir : dir.substr(0, sep);
        if (!prefix.empty() && !vfs.is_dir(prefix) && !vfs.make_dir(prefix)) {
            return false;
        }
        if (sep == std::string_view::npos) {
            break;
        }
        pos = sep + 1U;
    }
    return vfs.is_dir(dir);
}

}  // namespace rhi_detail

// Validates `meta_json`, rejects a `.so` payload that is not an ELF shared object,
// rewrites its `libhardware.so` dependency to `libpanvk_hw.so`
// (`patch_icd_libhardware_needed`, in place on `icd_bytes`), and writes
// `<libraryName>` then `meta.json` under `stage_dir` through `vfs`.
//
// Each file is replaced atomically (`replace_file_atomically`), and `meta.json`
// is written last, so a staged directory whose `meta.json` names the library
// always holds the complete library.
[[nodiscard]] inline bool stage_driver_package_to_vfs(
    Vfs& vfs,
    std::string_view stage_dir,
    std::string_view meta_json,
    std::vector<std::uint8_t>&& icd_bytes,
    DriverPackageMeta& out_meta,
    std::string& out_staged_icd_path,
    std::size_t* out_patched_refs = nullptr,
    std::string* error_out = nullptr) {
    out_staged_icd_path.clear();
    if (out_patched_refs != nullptr) {
        *out_patched_refs = 0;
    }

    if (!parse_driver_meta_json(meta_json, out_meta, error_out)) {
        return false;
    }
    if (icd_bytes.empty()) {
        return rhi_detail::set_error(error_out, "driver ICD binary is empty");
    }
    if (rhi_detail::ends_with_ci(out_meta.library_name, ".so") && !is_little_endian_elf(icd_bytes)) {
        return rhi_detail::set_error(
            error_out, "driver ICD " + out_meta.library_name + " is not a little-endian ELF file");
    }

    while (!stage_dir.empty() && (stage_dir.back() == '/' || stage_dir.back() == '\\')) {
        stage_dir.remove_suffix(1);
    }
    if (!rhi_detail::make_dirs(vfs, stage_dir)) {
        return rhi_detail::set_error(error_out,
                                     "cannot create driver stage directory " + std::string(stage_dir));
    }

    const auto join_stage = [&](std::string_view file) -> std::string {
        if (stage_dir.empty()) {
            return std::string(file);
        }
        std::string p(stage_dir);
        p.push_back('/');
        p.append(file);
        return p;
    };

    const std::size_t patched_count = patch_icd_libhardware_needed(icd_bytes);
    if (out_patched_refs != nullptr) {
        *out_patched_refs = patched_count;
    }

    const std::string meta_path = join_stage("meta.json");
    const std::string icd_path = join_stage(out_meta.library_name);
    if (!rhi_detail::replace_file_atomically(vfs, icd_path, icd_bytes)) {
        return rhi_detail::set_error(error_out, "failed writing driver ICD binary to Vfs");
    }
    const std::span<const std::uint8_t> meta_bytes(
        reinterpret_cast<const std::uint8_t*>(meta_json.data()), meta_json.size());
    if (!rhi_detail::replace_file_atomically(vfs, meta_path, meta_bytes)) {
        return rhi_detail::set_error(error_out, "failed writing driver meta.json to Vfs");
    }

    out_staged_icd_path = icd_path;
    return true;
}

// Copying overload for callers that keep their own ICD bytes.
[[nodiscard]] inline bool stage_driver_package_to_vfs(
    Vfs& vfs,
    std::string_view stage_dir,
    std::string_view meta_json,
    std::span<const std::uint8_t> icd_bytes,
    DriverPackageMeta& out_meta,
    std::string& out_staged_icd_path,
    std::size_t* out_patched_refs = nullptr,
    std::string* error_out = nullptr) {
    return stage_driver_package_to_vfs(vfs, stage_dir, meta_json,
                                       std::vector<std::uint8_t>(icd_bytes.begin(), icd_bytes.end()),
                                       out_meta, out_staged_icd_path, out_patched_refs, error_out);
}

}  // namespace wse::platform
