// Platform seam: minimal cross-platform RHI, deterministic software reference
// rasterizer, and Android custom Vulkan driver package helpers.
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.5 layout
// and carries no PE/PDB citations (consumes decoded `wse::asset::BrfMesh` and
// `wse::asset::DdsImageView` data; `docs/PLATFORM_PLAN.md`, slice P4).
//
// Zero `<vulkan/vulkan.h>` dependency in this header so standalone header
// compilation checks (`wse_header_checks`) succeed with only `engine/include`.

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/asset/dds.h"
#include "wse/core/json.h"
#include "wse/platform/vfs.h"

namespace wse::platform {

// User-selectable Vulkan driver source at runtime (`docs/PLATFORM_PLAN.md` §P4).
enum class DriverMode : std::uint8_t {
    system = 0,
    turnip = 1,
    panvk = 2,
    custom_icd = 3,
};

[[nodiscard]] inline std::string_view driver_mode_name(DriverMode mode) noexcept {
    switch (mode) {
        case DriverMode::system:
            return "system";
        case DriverMode::turnip:
            return "turnip";
        case DriverMode::panvk:
            return "panvk";
        case DriverMode::custom_icd:
            return "custom_icd";
    }
    return "system";
}

[[nodiscard]] inline bool parse_driver_mode(std::string_view text, DriverMode& out) {
    const std::string lower = ascii_lower(text);
    if (lower == "system" || lower == "system vulkan") {
        out = DriverMode::system;
        return true;
    }
    if (lower == "turnip" || lower.find("turnip") != std::string::npos ||
        lower.find("freedreno") != std::string::npos) {
        out = DriverMode::turnip;
        return true;
    }
    if (lower == "panvk" || lower.find("panvk") != std::string::npos ||
        lower.find("panfrost") != std::string::npos) {
        out = DriverMode::panvk;
        return true;
    }
    if (lower == "custom" || lower == "custom_icd" || lower == "custom icd") {
        out = DriverMode::custom_icd;
        return true;
    }
    return false;
}

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

inline bool set_error(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
    return false;
}

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

inline std::uint32_t pack_rgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept {
    return static_cast<std::uint32_t>(r) |
           (static_cast<std::uint32_t>(g) << 8U) |
           (static_cast<std::uint32_t>(b) << 16U) |
           (static_cast<std::uint32_t>(a) << 24U);
}

}  // namespace rhi_detail

// Validates that a package `libraryName` is a single safe shared-library file name
// with no directory separators, drive letters, parent-traversal segments, or NUL bytes.
[[nodiscard]] inline bool is_safe_driver_library_name(std::string_view name) {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    if (name.find('/') != std::string_view::npos ||
        name.find('\\') != std::string_view::npos ||
        name.find(':') != std::string_view::npos ||
        name.find('\0') != std::string_view::npos ||
        name.find("..") != std::string_view::npos) {
        return false;
    }
    return rhi_detail::ends_with_ci(name, ".so") || rhi_detail::ends_with_ci(name, ".dll");
}

// Parses an AdrenoTools / Winlator driver `meta.json` payload.
[[nodiscard]] inline bool parse_driver_meta_json(std::string_view json_text,
                                                 DriverPackageMeta& out,
                                                 std::string* error_out = nullptr) {
    out = DriverPackageMeta{};
    std::map<std::string, std::string> fields;
    if (!wse::core::wse_parse_json_object(json_text, fields)) {
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

// Validates `meta_json`, rejects a `.so` payload that is not an ELF shared object,
// rewrites its `libhardware.so` dependency to `libpanvk_hw.so`
// (`patch_icd_libhardware_needed`), and writes both `meta.json` and
// `<libraryName>` under `stage_dir` through `vfs`.
[[nodiscard]] inline bool stage_driver_package_to_vfs(
    Vfs& vfs,
    std::string_view stage_dir,
    std::string_view meta_json,
    std::span<const std::uint8_t> icd_bytes,
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
    if (!stage_dir.empty() && !vfs.is_dir(stage_dir)) {
        // Best-effort creation of single-level or nested stage directories.
        std::size_t pos = 0;
        while (pos < stage_dir.size()) {
            const std::size_t sep = stage_dir.find_first_of("/\\", pos);
            const std::string_view prefix =
                (sep == std::string_view::npos) ? stage_dir : stage_dir.substr(0, sep);
            if (!prefix.empty() && !vfs.is_dir(prefix)) {
                (void)vfs.make_dir(prefix);
            }
            if (sep == std::string_view::npos) {
                break;
            }
            pos = sep + 1U;
        }
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

    std::vector<std::uint8_t> patched_icd(icd_bytes.begin(), icd_bytes.end());
    const std::size_t patched_count = patch_icd_libhardware_needed(patched_icd);
    if (out_patched_refs != nullptr) {
        *out_patched_refs = patched_count;
    }

    const std::string meta_path = join_stage("meta.json");
    const std::string icd_path = join_stage(out_meta.library_name);
    if (!vfs.write_text(meta_path, meta_json)) {
        return rhi_detail::set_error(error_out, "failed writing driver meta.json to Vfs");
    }
    if (!vfs.write_file(icd_path, patched_icd)) {
        return rhi_detail::set_error(error_out, "failed writing driver ICD binary to Vfs");
    }

    out_staged_icd_path = icd_path;
    return true;
}

// -----------------------------------------------------------------------------
// Minimal RHI interface & types
// -----------------------------------------------------------------------------

using BufferHandle = std::uint32_t;
using TextureHandle = std::uint32_t;
using PipelineHandle = std::uint32_t;

enum class BufferUsage : std::uint8_t {
    vertex = 0,
    index = 1,
    uniform = 2,
};

enum class CullMode : std::uint8_t {
    none = 0,
    back = 1,
    front = 2,
};

// Interleaved vertex format fed to the minimal RHI.
// `color_rgba` uses little-endian packed RGBA bytes: R=(c&0xFF), G=((c>>8)&0xFF),
// B=((c>>16)&0xFF), A=((c>>24)&0xFF).
struct MeshVertex {
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 1.0f;
    float u = 0.0f;
    float v = 0.0f;
    std::uint32_t color_rgba = 0xFFFFFFFFU;
};

struct PipelineDesc {
    bool depth_test = true;
    bool depth_write = true;
    CullMode cull_mode = CullMode::back;
};

struct RenderPassDesc {
    std::uint32_t width = 64;
    std::uint32_t height = 64;
    std::uint8_t clear_r = 18;
    std::uint8_t clear_g = 22;
    std::uint8_t clear_b = 30;
    std::uint8_t clear_a = 255;
    float clear_depth = 1.0f;
};

// `mvp` is a column-major 4x4 matrix (column `c` at `mvp[c*4 + r]`) mapping object
// space into Vulkan clip space (`x,y in [-w, w]` with `-Y` up, `z in [0, w]`).
struct MeshDrawCall {
    PipelineHandle pipeline = 0;
    BufferHandle vertex_buffer = 0;
    BufferHandle index_buffer = 0;
    TextureHandle texture = 0;
    std::uint32_t index_count = 0;
    float mvp[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
};

class RhiDevice {
public:
    virtual ~RhiDevice() = default;

    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;
    [[nodiscard]] virtual std::string device_name() const = 0;
    [[nodiscard]] virtual DriverMode driver_mode() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t api_version() const noexcept = 0;

    [[nodiscard]] virtual BufferHandle create_buffer(BufferUsage usage,
                                                     std::span<const std::uint8_t> data,
                                                     std::string* error_out = nullptr) = 0;
    virtual void destroy_buffer(BufferHandle handle) = 0;

    [[nodiscard]] virtual TextureHandle create_texture_rgba8(
        std::uint32_t width,
        std::uint32_t height,
        std::span<const std::uint8_t> rgba8,
        std::string* error_out = nullptr) = 0;
    virtual void destroy_texture(TextureHandle handle) = 0;

    [[nodiscard]] virtual PipelineHandle create_pipeline(const PipelineDesc& desc,
                                                         std::string* error_out = nullptr) = 0;
    virtual void destroy_pipeline(PipelineHandle handle) = 0;

    [[nodiscard]] virtual bool render_pass_and_readback(
        const RenderPassDesc& pass,
        std::span<const MeshDrawCall> draws,
        std::vector<std::uint8_t>& out_rgba8,
        std::string* error_out = nullptr) = 0;
};

// Deterministic CPU reference implementation of `RhiDevice`.
// Used both as the host-only golden reference and as the deterministic readback
// rasterizer inside `VulkanRhiDevice` for P4 headless verification until P5
// introduces compiled SPIR-V shaders.
class SoftwareRhiDevice final : public RhiDevice {
public:
    explicit SoftwareRhiDevice(std::string dev_name = "WSE2 Software Reference Rasterizer",
                               DriverMode mode = DriverMode::system,
                               std::uint32_t api_ver = 0x00401000U)
        : device_name_(std::move(dev_name)), mode_(mode), api_version_(api_ver) {}

    [[nodiscard]] std::string_view backend_name() const noexcept override { return "software"; }
    [[nodiscard]] std::string device_name() const override { return device_name_; }
    [[nodiscard]] DriverMode driver_mode() const noexcept override { return mode_; }
    [[nodiscard]] std::uint32_t api_version() const noexcept override { return api_version_; }

    void set_identity(std::string dev_name, DriverMode mode, std::uint32_t api_ver) {
        device_name_ = std::move(dev_name);
        mode_ = mode;
        api_version_ = api_ver;
    }

    [[nodiscard]] BufferHandle create_buffer(BufferUsage usage,
                                             std::span<const std::uint8_t> data,
                                             std::string* error_out = nullptr) override {
        if (data.empty()) {
            (void)rhi_detail::set_error(error_out, "cannot create empty RHI buffer");
            return 0;
        }
        const BufferHandle id = allocate_handle();
        BufferRec rec{};
        rec.usage = usage;
        rec.bytes.assign(data.begin(), data.end());
        buffers_.emplace(id, std::move(rec));
        return id;
    }

    void destroy_buffer(BufferHandle handle) override {
        if (handle != 0) {
            buffers_.erase(handle);
        }
    }

    [[nodiscard]] TextureHandle create_texture_rgba8(
        std::uint32_t width,
        std::uint32_t height,
        std::span<const std::uint8_t> rgba8,
        std::string* error_out = nullptr) override {
        if (width == 0 || height == 0) {
            (void)rhi_detail::set_error(error_out, "texture dimensions must be non-zero");
            return 0;
        }
        const std::size_t expected_bytes = static_cast<std::size_t>(width) * height * 4U;
        if (rgba8.size() != expected_bytes) {
            (void)rhi_detail::set_error(error_out, "RGBA8 texture payload size mismatch");
            return 0;
        }
        const TextureHandle id = allocate_handle();
        TextureRec rec{};
        rec.width = width;
        rec.height = height;
        rec.rgba8.assign(rgba8.begin(), rgba8.end());
        textures_.emplace(id, std::move(rec));
        return id;
    }

    void destroy_texture(TextureHandle handle) override {
        if (handle != 0) {
            textures_.erase(handle);
        }
    }

    [[nodiscard]] PipelineHandle create_pipeline(const PipelineDesc& desc,
                                                 std::string* error_out = nullptr) override {
        (void)error_out;
        const PipelineHandle id = allocate_handle();
        pipelines_.emplace(id, desc);
        return id;
    }

    void destroy_pipeline(PipelineHandle handle) override {
        if (handle != 0) {
            pipelines_.erase(handle);
        }
    }

    [[nodiscard]] bool render_pass_and_readback(
        const RenderPassDesc& pass,
        std::span<const MeshDrawCall> draws,
        std::vector<std::uint8_t>& out_rgba8,
        std::string* error_out = nullptr) override {
        out_rgba8.clear();
        if (pass.width == 0 || pass.height == 0 || pass.width > 4096U || pass.height > 4096U) {
            return rhi_detail::set_error(error_out, "invalid render pass dimensions");
        }

        const std::size_t pixel_count = static_cast<std::size_t>(pass.width) * pass.height;
        out_rgba8.resize(pixel_count * 4U);
        const std::array<std::uint8_t, 4> clear{pass.clear_r, pass.clear_g, pass.clear_b,
                                                pass.clear_a};
        for (std::size_t i = 0; i < pixel_count; ++i) {
            std::memcpy(out_rgba8.data() + i * 4U, clear.data(), 4U);
        }
        depth_scratch_.assign(pixel_count, pass.clear_depth);

        for (const MeshDrawCall& draw : draws) {
            const auto pip_it = pipelines_.find(draw.pipeline);
            const auto vb_it = buffers_.find(draw.vertex_buffer);
            const auto ib_it = buffers_.find(draw.index_buffer);
            if (pip_it == pipelines_.end() || vb_it == buffers_.end() || ib_it == buffers_.end()) {
                return rhi_detail::set_error(error_out, "draw call references invalid RHI handle");
            }
            if (vb_it->second.usage != BufferUsage::vertex ||
                ib_it->second.usage != BufferUsage::index) {
                return rhi_detail::set_error(error_out, "draw call buffer usage mismatch");
            }
            const TextureRec* tex = nullptr;
            if (draw.texture != 0) {
                const auto tex_it = textures_.find(draw.texture);
                if (tex_it == textures_.end()) {
                    return rhi_detail::set_error(error_out, "draw call references invalid texture");
                }
                tex = &tex_it->second;
            }

            const std::vector<std::uint8_t>& vb = vb_it->second.bytes;
            const std::vector<std::uint8_t>& ib = ib_it->second.bytes;
            const std::size_t num_verts = vb.size() / sizeof(MeshVertex);
            const std::size_t num_indices = ib.size() / sizeof(std::uint16_t);
            if (draw.index_count > num_indices || (draw.index_count % 3U) != 0U) {
                return rhi_detail::set_error(error_out, "invalid draw index count");
            }

            // Vertex stage: transform each vertex once (indexed meshes share
            // vertices between triangles), reading the stored bytes in place.
            clip_scratch_.resize(num_verts);
            for (std::size_t i = 0; i < num_verts; ++i) {
                MeshVertex mv{};
                std::memcpy(&mv, vb.data() + i * sizeof(MeshVertex), sizeof(MeshVertex));
                clip_scratch_[i] = to_clip(mv, draw.mvp);
            }

            for (std::uint32_t idx = 0; idx + 2U < draw.index_count; idx += 3U) {
                std::uint16_t tri[3]{};
                std::memcpy(tri, ib.data() + static_cast<std::size_t>(idx) * 2U, sizeof(tri));
                if (tri[0] >= num_verts || tri[1] >= num_verts || tri[2] >= num_verts) {
                    continue;
                }
                const ClipVert& a = clip_scratch_[tri[0]];
                const ClipVert& b = clip_scratch_[tri[1]];
                const ClipVert& c = clip_scratch_[tri[2]];
                if (!a.finite || !b.finite || !c.finite) {
                    continue;
                }
                clip_and_rasterize(pass, pip_it->second, a, b, c, tex, out_rgba8, depth_scratch_);
            }
        }

        return true;
    }

private:
    struct BufferRec {
        BufferUsage usage = BufferUsage::vertex;
        std::vector<std::uint8_t> bytes{};
    };

    struct TextureRec {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba8{};
    };

    // Clip-space vertex plus the varyings interpolated across a triangle.
    struct ClipVert {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
        float u = 0.0f;
        float v = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
        bool finite = true;
    };

    // Screen-space vertex: position snapped to 1/256-pixel fixed point so edge
    // functions are exact integers (no cracks or double hits on shared edges),
    // varyings pre-divided by w for perspective-correct interpolation.
    struct ScreenVert {
        std::int64_t fx = 0;
        std::int64_t fy = 0;
        float z_ndc = 0.0f;
        float inv_w = 1.0f;
        float u_over_w = 0.0f;
        float v_over_w = 0.0f;
        float r_over_w = 0.0f;
        float g_over_w = 0.0f;
        float b_over_w = 0.0f;
        float a_over_w = 0.0f;
    };

    static constexpr std::int64_t kSubpixelBits = 8;
    static constexpr std::int64_t kSubpixel = std::int64_t{1} << kSubpixelBits;
    // Guard band: |x|,|y| <= kGuardBand * w after clipping keeps fixed-point
    // screen coordinates below 2^23 subpixels at 4096 px, so edge products fit int64.
    static constexpr float kGuardBand = 8.0f;
    static constexpr float kMinW = 1.0e-5f;
    static constexpr int kClipPlanes = 6;
    static constexpr std::size_t kMaxClipVerts = 3U + kClipPlanes;

    static ClipVert to_clip(const MeshVertex& v, const float m[16]) noexcept {
        ClipVert cv{};
        cv.x = m[0] * v.px + m[4] * v.py + m[8] * v.pz + m[12];
        cv.y = m[1] * v.px + m[5] * v.py + m[9] * v.pz + m[13];
        cv.z = m[2] * v.px + m[6] * v.py + m[10] * v.pz + m[14];
        cv.w = m[3] * v.px + m[7] * v.py + m[11] * v.pz + m[15];
        cv.finite = std::isfinite(cv.x) && std::isfinite(cv.y) && std::isfinite(cv.z) &&
                    std::isfinite(cv.w) && std::isfinite(v.u) && std::isfinite(v.v);
        cv.u = v.u;
        cv.v = v.v;
        cv.r = static_cast<float>(v.color_rgba & 0xFFU) / 255.0f;
        cv.g = static_cast<float>((v.color_rgba >> 8U) & 0xFFU) / 255.0f;
        cv.b = static_cast<float>((v.color_rgba >> 16U) & 0xFFU) / 255.0f;
        cv.a = static_cast<float>((v.color_rgba >> 24U) & 0xFFU) / 255.0f;
        return cv;
    }

    // Signed distance to clip plane `plane` (inside when >= 0): Vulkan near plane
    // `z >= 0`, a minimum `w`, and the four guard-band side planes.
    static float plane_distance(const ClipVert& v, int plane) noexcept {
        switch (plane) {
            case 0:
                return v.z;
            case 1:
                return v.w - kMinW;
            case 2:
                return kGuardBand * v.w - v.x;
            case 3:
                return kGuardBand * v.w + v.x;
            case 4:
                return kGuardBand * v.w - v.y;
            default:
                return kGuardBand * v.w + v.y;
        }
    }

    static ClipVert lerp_clip(const ClipVert& p, const ClipVert& q, float t) noexcept {
        const auto mix = [t](float a, float b) noexcept { return a + (b - a) * t; };
        ClipVert o{};
        o.x = mix(p.x, q.x);
        o.y = mix(p.y, q.y);
        o.z = mix(p.z, q.z);
        o.w = mix(p.w, q.w);
        o.u = mix(p.u, q.u);
        o.v = mix(p.v, q.v);
        o.r = mix(p.r, q.r);
        o.g = mix(p.g, q.g);
        o.b = mix(p.b, q.b);
        o.a = mix(p.a, q.a);
        return o;
    }

    static ScreenVert to_screen(const ClipVert& v, std::uint32_t width, std::uint32_t height) noexcept {
        ScreenVert s{};
        const float inv_w = 1.0f / v.w;
        const float sx = (v.x * inv_w * 0.5f + 0.5f) * static_cast<float>(width);
        const float sy = (v.y * inv_w * 0.5f + 0.5f) * static_cast<float>(height);
        s.fx = static_cast<std::int64_t>(std::llround(sx * static_cast<float>(kSubpixel)));
        s.fy = static_cast<std::int64_t>(std::llround(sy * static_cast<float>(kSubpixel)));
        s.z_ndc = v.z * inv_w;
        s.inv_w = inv_w;
        s.u_over_w = v.u * inv_w;
        s.v_over_w = v.v * inv_w;
        s.r_over_w = v.r * inv_w;
        s.g_over_w = v.g * inv_w;
        s.b_over_w = v.b * inv_w;
        s.a_over_w = v.a * inv_w;
        return s;
    }

    static float wrap_uv_unit(float coord) noexcept {
        if (coord >= 0.0f && coord <= 1.0f) {
            return coord;
        }
        const float wrapped = coord - std::floor(coord);
        return std::clamp(wrapped, 0.0f, 1.0f);
    }

    static void sample_texture(const TextureRec* tex,
                               float u,
                               float v,
                               std::uint8_t& out_r,
                               std::uint8_t& out_g,
                               std::uint8_t& out_b,
                               std::uint8_t& out_a) noexcept {
        if (tex == nullptr || tex->width == 0 || tex->height == 0) {
            out_r = out_g = out_b = out_a = 255U;
            return;
        }
        const float su = wrap_uv_unit(u);
        const float sv = wrap_uv_unit(v);
        const std::uint32_t tx = std::min(
            static_cast<std::uint32_t>(su * static_cast<float>(tex->width)), tex->width - 1U);
        const std::uint32_t ty = std::min(
            static_cast<std::uint32_t>(sv * static_cast<float>(tex->height)), tex->height - 1U);
        const std::size_t off = (static_cast<std::size_t>(ty) * tex->width + tx) * 4U;
        out_r = tex->rgba8[off + 0U];
        out_g = tex->rgba8[off + 1U];
        out_b = tex->rgba8[off + 2U];
        out_a = tex->rgba8[off + 3U];
    }

    // Clips one triangle against the near/w/guard-band planes (Sutherland-Hodgman)
    // and rasterizes the resulting convex polygon as a triangle fan. Triangles
    // crossing the near plane are cut instead of dropped.
    static void clip_and_rasterize(const RenderPassDesc& pass,
                                   const PipelineDesc& pip,
                                   const ClipVert& a,
                                   const ClipVert& b,
                                   const ClipVert& c,
                                   const TextureRec* tex,
                                   std::vector<std::uint8_t>& color_buf,
                                   std::vector<float>& depth_buf) noexcept {
        std::array<ClipVert, kMaxClipVerts> poly{a, b, c};
        std::size_t count = 3U;

        bool all_inside = true;
        for (int plane = 0; plane < kClipPlanes && all_inside; ++plane) {
            all_inside = plane_distance(a, plane) >= 0.0f && plane_distance(b, plane) >= 0.0f &&
                         plane_distance(c, plane) >= 0.0f;
        }
        if (!all_inside) {
            std::array<ClipVert, kMaxClipVerts> next{};
            for (int plane = 0; plane < kClipPlanes && count >= 3U; ++plane) {
                std::size_t out_count = 0;
                for (std::size_t i = 0; i < count; ++i) {
                    const ClipVert& cur = poly[i];
                    const ClipVert& nxt = poly[(i + 1U) % count];
                    const float dc = plane_distance(cur, plane);
                    const float dn = plane_distance(nxt, plane);
                    if (dc >= 0.0f && out_count < kMaxClipVerts) {
                        next[out_count++] = cur;
                    }
                    if (((dc >= 0.0f) != (dn >= 0.0f)) && out_count < kMaxClipVerts) {
                        next[out_count++] = lerp_clip(cur, nxt, dc / (dc - dn));
                    }
                }
                poly = next;
                count = out_count;
            }
            if (count < 3U) {
                return;
            }
        }

        std::array<ScreenVert, kMaxClipVerts> screen{};
        for (std::size_t i = 0; i < count; ++i) {
            screen[i] = to_screen(poly[i], pass.width, pass.height);
        }
        for (std::size_t i = 1; i + 1U < count; ++i) {
            rasterize_triangle(pass, pip, screen[0], screen[i], screen[i + 1U], tex, color_buf,
                               depth_buf);
        }
    }

    static std::int64_t floor_div(std::int64_t num, std::int64_t den) noexcept {
        std::int64_t q = num / den;
        if ((num % den != 0) && ((num < 0) != (den < 0))) {
            --q;
        }
        return q;
    }

    static void rasterize_triangle(const RenderPassDesc& pass,
                                   const PipelineDesc& pip,
                                   const ScreenVert& s0,
                                   ScreenVert s1,
                                   ScreenVert s2,
                                   const TextureRec* tex,
                                   std::vector<std::uint8_t>& color_buf,
                                   std::vector<float>& depth_buf) noexcept {
        // E(a, b, p) = (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x).
        const auto edge = [](const ScreenVert& ea, const ScreenVert& eb, std::int64_t px,
                             std::int64_t py) noexcept {
            return (px - ea.fx) * (eb.fy - ea.fy) - (py - ea.fy) * (eb.fx - ea.fx);
        };

        // Screen space has +Y down: area > 0 corresponds to visual clockwise
        // (or standard CCW in math coordinates before Vulkan Y-flip).
        std::int64_t area = edge(s0, s1, s2.fx, s2.fy);
        if (area == 0) {
            return;
        }
        if (pip.cull_mode == CullMode::back && area < 0) {
            return;
        }
        if (pip.cull_mode == CullMode::front && area > 0) {
            return;
        }
        if (area < 0) {
            std::swap(s1, s2);
            area = -area;
        }

        // Top-left fill rule: a pixel centre exactly on an edge belongs to the
        // triangle only for a left edge (dy > 0) or a top edge (dy == 0, dx < 0).
        const auto bias = [](const ScreenVert& ea, const ScreenVert& eb) noexcept -> std::int64_t {
            const std::int64_t dy = eb.fy - ea.fy;
            const std::int64_t dx = eb.fx - ea.fx;
            return (dy > 0 || (dy == 0 && dx < 0)) ? 0 : -1;
        };
        const std::int64_t bias0 = bias(s1, s2);
        const std::int64_t bias1 = bias(s2, s0);
        const std::int64_t bias2 = bias(s0, s1);

        const std::int64_t half = kSubpixel / 2;
        const std::int64_t min_fx = std::min({s0.fx, s1.fx, s2.fx});
        const std::int64_t max_fx = std::max({s0.fx, s1.fx, s2.fx});
        const std::int64_t min_fy = std::min({s0.fy, s1.fy, s2.fy});
        const std::int64_t max_fy = std::max({s0.fy, s1.fy, s2.fy});
        const std::int64_t x_begin = std::max<std::int64_t>(0, floor_div(min_fx - half + kSubpixel - 1, kSubpixel));
        const std::int64_t x_end = std::min<std::int64_t>(static_cast<std::int64_t>(pass.width) - 1,
                                                          floor_div(max_fx - half, kSubpixel));
        const std::int64_t y_begin = std::max<std::int64_t>(0, floor_div(min_fy - half + kSubpixel - 1, kSubpixel));
        const std::int64_t y_end = std::min<std::int64_t>(static_cast<std::int64_t>(pass.height) - 1,
                                                          floor_div(max_fy - half, kSubpixel));
        if (x_begin > x_end || y_begin > y_end) {
            return;
        }

        const float inv_area = 1.0f / static_cast<float>(area);
        // Per-pixel steps of each edge function (exact integers).
        const std::int64_t step_x0 = (s2.fy - s1.fy) * kSubpixel;
        const std::int64_t step_x1 = (s0.fy - s2.fy) * kSubpixel;
        const std::int64_t step_x2 = (s1.fy - s0.fy) * kSubpixel;

        for (std::int64_t py = y_begin; py <= y_end; ++py) {
            const std::int64_t cy = py * kSubpixel + half;
            const std::int64_t cx0 = x_begin * kSubpixel + half;
            std::int64_t e0 = edge(s1, s2, cx0, cy);
            std::int64_t e1 = edge(s2, s0, cx0, cy);
            std::int64_t e2 = edge(s0, s1, cx0, cy);
            const std::size_t row = static_cast<std::size_t>(py) * pass.width;
            for (std::int64_t px = x_begin; px <= x_end;
                 ++px, e0 += step_x0, e1 += step_x1, e2 += step_x2) {
                if (e0 + bias0 < 0 || e1 + bias1 < 0 || e2 + bias2 < 0) {
                    continue;
                }
                const float w0 = static_cast<float>(e0) * inv_area;
                const float w1 = static_cast<float>(e1) * inv_area;
                const float w2 = static_cast<float>(e2) * inv_area;

                const float z = w0 * s0.z_ndc + w1 * s1.z_ndc + w2 * s2.z_ndc;
                if (z < 0.0f || z > 1.0f) {
                    continue;
                }
                const std::size_t pix = row + static_cast<std::size_t>(px);
                if (pip.depth_test && z >= depth_buf[pix]) {
                    continue;
                }
                if (pip.depth_write) {
                    depth_buf[pix] = z;
                }

                const float inv_w = w0 * s0.inv_w + w1 * s1.inv_w + w2 * s2.inv_w;
                const float persp = (inv_w > 1.0e-12f) ? 1.0f / inv_w : 0.0f;
                const auto interp = [&](float a0, float a1, float a2) noexcept {
                    return (w0 * a0 + w1 * a1 + w2 * a2) * persp;
                };
                const float u = interp(s0.u_over_w, s1.u_over_w, s2.u_over_w);
                const float v = interp(s0.v_over_w, s1.v_over_w, s2.v_over_w);

                std::uint8_t tr = 255U;
                std::uint8_t tg = 255U;
                std::uint8_t tb = 255U;
                std::uint8_t ta = 255U;
                sample_texture(tex, u, v, tr, tg, tb, ta);

                const float vr = std::clamp(interp(s0.r_over_w, s1.r_over_w, s2.r_over_w), 0.0f, 1.0f);
                const float vg = std::clamp(interp(s0.g_over_w, s1.g_over_w, s2.g_over_w), 0.0f, 1.0f);
                const float vb = std::clamp(interp(s0.b_over_w, s1.b_over_w, s2.b_over_w), 0.0f, 1.0f);
                const float va = std::clamp(interp(s0.a_over_w, s1.a_over_w, s2.a_over_w), 0.0f, 1.0f);

                std::uint8_t* dst = color_buf.data() + pix * 4U;
                dst[0] = static_cast<std::uint8_t>(std::lround(tr * vr));
                dst[1] = static_cast<std::uint8_t>(std::lround(tg * vg));
                dst[2] = static_cast<std::uint8_t>(std::lround(tb * vb));
                dst[3] = static_cast<std::uint8_t>(std::lround(ta * va));
            }
        }
    }

    // Handle 0 is reserved for "invalid"; skip it if the counter ever wraps.
    std::uint32_t allocate_handle() noexcept {
        if (next_handle_ == 0) {
            ++next_handle_;
        }
        return next_handle_++;
    }

    std::string device_name_;
    DriverMode mode_;
    std::uint32_t api_version_;
    std::uint32_t next_handle_ = 1;
    std::unordered_map<BufferHandle, BufferRec> buffers_{};
    std::unordered_map<TextureHandle, TextureRec> textures_{};
    std::unordered_map<PipelineHandle, PipelineDesc> pipelines_{};
    // Per-pass scratch reused across frames (no per-draw allocations once warm).
    std::vector<ClipVert> clip_scratch_{};
    std::vector<float> depth_scratch_{};
};

// Converts D3DCOLOR (`0xAARRGGBB`) into little-endian RGBA8 packed uint32 (`R | G<<8 | B<<16 | A<<24`).
[[nodiscard]] inline std::uint32_t argb_to_rgba8(std::uint32_t argb) noexcept {
    const std::uint8_t a = static_cast<std::uint8_t>((argb >> 24U) & 0xFFU);
    const std::uint8_t r = static_cast<std::uint8_t>((argb >> 16U) & 0xFFU);
    const std::uint8_t g = static_cast<std::uint8_t>((argb >> 8U) & 0xFFU);
    const std::uint8_t b = static_cast<std::uint8_t>(argb & 0xFFU);
    return rhi_detail::pack_rgba8(r, g, b, a);
}

// Extracts interleaved `MeshVertex` array from a decoded `BrfMesh`.
[[nodiscard]] inline std::vector<MeshVertex> build_rhi_vertices_from_brf(
    const wse::asset::BrfMesh& mesh, std::size_t frame_index = 0) {
    const wse::asset::BrfVertexKeyFrame* key =
        (frame_index < mesh.vertex_keys.size()) ? &mesh.vertex_keys[frame_index] : nullptr;

    std::vector<MeshVertex> verts;
    verts.reserve(mesh.face_corners.size());
    for (const wse::asset::BrfFaceCorner& corner : mesh.face_corners) {
        wse::asset::Vec3f pos{};
        if (corner.vertex_index >= 0) {
            const std::size_t pidx = static_cast<std::size_t>(corner.vertex_index);
            if (key != nullptr && pidx < key->positions.size()) {
                pos = key->positions[pidx];
            } else if (pidx < mesh.positions.size()) {
                pos = mesh.positions[pidx];
            }
        }
        verts.push_back(MeshVertex{
            pos.x,
            pos.y,
            pos.z,
            corner.normal.x,
            corner.normal.y,
            corner.normal.z,
            corner.uv.x,
            corner.uv.y,
            argb_to_rgba8(corner.color),
        });
    }
    return verts;
}

// Flattens `BrfMesh::faces` into a 16-bit triangle index buffer.
[[nodiscard]] inline std::vector<std::uint16_t> build_rhi_indices_from_brf(
    const wse::asset::BrfMesh& mesh) {
    std::vector<std::uint16_t> indices;
    indices.reserve(mesh.faces.size() * 3U);
    for (const wse::asset::BrfMeshFace& face : mesh.faces) {
        indices.push_back(face.corners[0]);
        indices.push_back(face.corners[1]);
        indices.push_back(face.corners[2]);
    }
    return indices;
}

// Builds a de-duplicated indexed stream from a decoded `BrfMesh`: face corners
// with byte-identical attributes (position, normal, UV, colour) share one vertex,
// and corners no face references are dropped. `.brf` stores one corner per face
// vertex, so this typically cuts the vertex stream (and GPU vertex shading and
// bandwidth) by 2-6x. Faces naming a corner past `face_corners` are skipped.
inline void build_rhi_mesh_from_brf(const wse::asset::BrfMesh& mesh,
                                    std::vector<MeshVertex>& out_verts,
                                    std::vector<std::uint16_t>& out_indices,
                                    std::size_t frame_index = 0) {
    static_assert(sizeof(MeshVertex) == 9U * 4U, "MeshVertex must have no padding bytes");
    out_verts.clear();
    out_indices.clear();
    const std::vector<MeshVertex> corners = build_rhi_vertices_from_brf(mesh, frame_index);

    struct CornerHash {
        const std::vector<MeshVertex>* verts;
        std::size_t operator()(std::uint32_t i) const noexcept {
            const auto* p = reinterpret_cast<const std::uint8_t*>(&(*verts)[i]);
            std::uint64_t h = 14695981039346656037ULL;
            for (std::size_t k = 0; k < sizeof(MeshVertex); ++k) {
                h = (h ^ p[k]) * 1099511628211ULL;
            }
            return static_cast<std::size_t>(h);
        }
    };
    struct CornerEq {
        const std::vector<MeshVertex>* verts;
        bool operator()(std::uint32_t a, std::uint32_t b) const noexcept {
            return std::memcmp(&(*verts)[a], &(*verts)[b], sizeof(MeshVertex)) == 0;
        }
    };
    std::unordered_map<std::uint32_t, std::uint16_t, CornerHash, CornerEq> unique(
        corners.size(), CornerHash{&corners}, CornerEq{&corners});
    constexpr std::uint32_t kUnmapped = 0xFFFFFFFFU;
    std::vector<std::uint32_t> remap(corners.size(), kUnmapped);

    out_verts.reserve(corners.size());
    out_indices.reserve(mesh.faces.size() * 3U);
    for (const wse::asset::BrfMeshFace& face : mesh.faces) {
        if (face.corners[0] >= corners.size() || face.corners[1] >= corners.size() ||
            face.corners[2] >= corners.size()) {
            continue;
        }
        for (const std::uint16_t corner : face.corners) {
            if (remap[corner] == kUnmapped) {
                const auto [it, inserted] = unique.try_emplace(
                    corner, static_cast<std::uint16_t>(out_verts.size()));
                if (inserted) {
                    out_verts.push_back(corners[corner]);
                }
                remap[corner] = it->second;
            }
            out_indices.push_back(static_cast<std::uint16_t>(remap[corner]));
        }
    }
}

// Builds a column-major 4x4 perspective * view matrix targeting the origin `(0, 0, 0)`
// using Vulkan `[0, 1]` clip-space depth and `-Y` up screen coordinates.
inline void make_orbit_mvp_matrix(float aspect,
                                  float distance,
                                  float yaw_rad,
                                  float pitch_rad,
                                  float out_mvp[16]) noexcept {
    const float safe_aspect = (aspect > 0.05f) ? aspect : 1.0f;
    const float safe_dist = std::max(distance, 0.25f);

    const float cp = std::cos(pitch_rad);
    const float sp = std::sin(pitch_rad);
    const float cy = std::cos(yaw_rad);
    const float sy = std::sin(yaw_rad);

    // Camera eye position looking at origin with world up (0, 1, 0).
    const float eye_x = safe_dist * cp * sy;
    const float eye_y = safe_dist * sp;
    const float eye_z = safe_dist * cp * cy;

    auto normalize3 = [](float& x, float& y, float& z) noexcept {
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len > 1.0e-6f) {
            x /= len;
            y /= len;
            z /= len;
        }
    };

    // View basis: f points from eye to origin; r = normalize(cross(f, up)); u = cross(r, f).
    float fx = -eye_x;
    float fy = -eye_y;
    float fz = -eye_z;
    normalize3(fx, fy, fz);

    float rx = -fz;  // cross(f, (0,1,0)) = (-fz, 0, fx)
    float ry = 0.0f;
    float rz = fx;
    if (rx * rx + rz * rz < 1.0e-6f) {
        rx = 1.0f;
        rz = 0.0f;
    } else {
        normalize3(rx, ry, rz);
    }

    const float ux = ry * fz - rz * fy;
    const float uy = rz * fx - rx * fz;
    const float uz = rx * fy - ry * fx;

    const float tx = -(rx * eye_x + ry * eye_y + rz * eye_z);
    const float ty = -(ux * eye_x + uy * eye_y + uz * eye_z);
    const float tz = (fx * eye_x + fy * eye_y + fz * eye_z);  // -dot(-f, eye)

    // Column-major view matrix (RH camera looking down -Z_view).
    const float view[16] = {
        rx, ux, -fx, 0.0f,
        ry, uy, -fy, 0.0f,
        rz, uz, -fz, 0.0f,
        tx, ty,  tz, 1.0f,
    };

    // Vulkan RH perspective [0, 1] Z, Y-flipped projection.
    const float f = 1.7320508f;  // cot(60 deg / 2)
    const float znear = 0.1f;
    const float zfar = 100.0f;
    const float a = zfar / (znear - zfar);
    const float b = (zfar * znear) / (znear - zfar);

    const float proj[16] = {
        f / safe_aspect, 0.0f, 0.0f, 0.0f,
        0.0f, -f, 0.0f, 0.0f,
        0.0f, 0.0f, a, -1.0f,
        0.0f, 0.0f, b, 0.0f,
    };

    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += proj[k * 4 + row] * view[col * 4 + k];
            }
            out_mvp[col * 4 + row] = sum;
        }
    }
}

// End-to-end textured static `.brf` mesh render helper through any `RhiDevice`.
[[nodiscard]] inline bool render_textured_brf_mesh_frame(
    RhiDevice& rhi,
    const wse::asset::BrfMesh& mesh,
    const wse::asset::DdsImageView& dds,
    std::uint32_t width,
    std::uint32_t height,
    const float mvp[16],
    std::vector<std::uint8_t>& out_rgba8,
    std::string* error_out = nullptr,
    CullMode cull_mode = CullMode::back) {
    out_rgba8.clear();
    if (mesh.faces.empty() || mesh.face_corners.empty()) {
        return rhi_detail::set_error(error_out, "cannot render empty BrfMesh");
    }

    std::vector<MeshVertex> verts;
    std::vector<std::uint16_t> indices;
    build_rhi_mesh_from_brf(mesh, verts, indices);
    if (verts.empty() || indices.empty()) {
        return rhi_detail::set_error(error_out, "BrfMesh produced empty vertex or index stream");
    }

    std::vector<std::uint8_t> tex_rgba8;
    if (!wse::asset::decode_dds_mip_rgba8(dds, 0U, 0U, tex_rgba8, error_out)) {
        return false;
    }

    const std::span<const std::uint8_t> vb_bytes(
        reinterpret_cast<const std::uint8_t*>(verts.data()), verts.size() * sizeof(MeshVertex));
    const std::span<const std::uint8_t> ib_bytes(
        reinterpret_cast<const std::uint8_t*>(indices.data()),
        indices.size() * sizeof(std::uint16_t));

    const BufferHandle vb = rhi.create_buffer(BufferUsage::vertex, vb_bytes, error_out);
    if (vb == 0) {
        return false;
    }
    const BufferHandle ib = rhi.create_buffer(BufferUsage::index, ib_bytes, error_out);
    if (ib == 0) {
        rhi.destroy_buffer(vb);
        return false;
    }
    const TextureHandle tex = rhi.create_texture_rgba8(dds.width, dds.height, tex_rgba8, error_out);
    if (tex == 0) {
        rhi.destroy_buffer(ib);
        rhi.destroy_buffer(vb);
        return false;
    }
    PipelineDesc pip_desc{};
    pip_desc.depth_test = true;
    pip_desc.depth_write = true;
    pip_desc.cull_mode = cull_mode;
    const PipelineHandle pip = rhi.create_pipeline(pip_desc, error_out);
    if (pip == 0) {
        rhi.destroy_texture(tex);
        rhi.destroy_buffer(ib);
        rhi.destroy_buffer(vb);
        return false;
    }

    MeshDrawCall draw{};
    draw.pipeline = pip;
    draw.vertex_buffer = vb;
    draw.index_buffer = ib;
    draw.texture = tex;
    draw.index_count = static_cast<std::uint32_t>(indices.size());
    std::memcpy(draw.mvp, mvp, sizeof(draw.mvp));

    RenderPassDesc pass{};
    pass.width = width;
    pass.height = height;
    const std::array<MeshDrawCall, 1> draws{draw};
    const bool ok = rhi.render_pass_and_readback(pass, draws, out_rgba8, error_out);

    rhi.destroy_pipeline(pip);
    rhi.destroy_texture(tex);
    rhi.destroy_buffer(ib);
    rhi.destroy_buffer(vb);
    return ok;
}

// Deterministic 64-bit FNV-1a pixel checksum for host/on-device viewer regression tests.
[[nodiscard]] inline std::uint64_t hash_rgba8_frame(
    std::span<const std::uint8_t> rgba8) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    for (const std::uint8_t byte : rgba8) {
        h ^= static_cast<std::uint64_t>(byte);
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace wse::platform
