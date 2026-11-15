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

// Rewrites the 15-byte NUL-terminated ELF `.dynstr` DT_NEEDED string `"libhardware.so\0"`
// to `"libpanvk_hw.so\0"` (exact same 14-character byte length, preserving all ELF
// offsets and string table indices). PanVK links `libhardware.so` (a VNDK-SP library
// inaccessible to app namespaces on Android); redirecting it to our bundled
// `libpanvk_hw.so` routes `hw_get_module` cleanly into PanVK's supported `-ENOENT`
// fallback path (`docs/PLATFORM_PLAN.md` §P4).
[[nodiscard]] inline std::size_t patch_icd_libhardware_needed(
    std::span<std::uint8_t> elf_bytes) noexcept {
    static constexpr char kFrom[] = "libhardware.so";
    static constexpr char kTo[] = "libpanvk_hw.so";
    static constexpr std::size_t kNeedleLen = sizeof(kFrom);  // includes trailing NUL
    static_assert(sizeof(kFrom) == sizeof(kTo), "replacement SONAME length must match");

    if (elf_bytes.size() < kNeedleLen) {
        return 0;
    }

    std::size_t patched = 0;
    for (std::size_t i = 0; i + kNeedleLen <= elf_bytes.size(); ++i) {
        if (std::memcmp(elf_bytes.data() + i, kFrom, kNeedleLen) == 0) {
            std::memcpy(elf_bytes.data() + i, kTo, kNeedleLen);
            ++patched;
            i += kNeedleLen - 1U;
        }
    }
    return patched;
}

// Validates `meta_json`, patches any `"libhardware.so\0"` reference inside `icd_bytes`
// into `"libpanvk_hw.so\0"`, and writes both `meta.json` and `<libraryName>` under
// `stage_dir` through `vfs`.
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
        const BufferHandle id = next_handle_++;
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
        const TextureHandle id = next_handle_++;
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
        const PipelineHandle id = next_handle_++;
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
        for (std::size_t i = 0; i < pixel_count; ++i) {
            out_rgba8[i * 4U + 0U] = pass.clear_r;
            out_rgba8[i * 4U + 1U] = pass.clear_g;
            out_rgba8[i * 4U + 2U] = pass.clear_b;
            out_rgba8[i * 4U + 3U] = pass.clear_a;
        }
        std::vector<float> depth_buf(pixel_count, pass.clear_depth);

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

            const std::size_t num_verts = vb_it->second.bytes.size() / sizeof(MeshVertex);
            const std::size_t num_indices = ib_it->second.bytes.size() / sizeof(std::uint16_t);
            if (draw.index_count > num_indices || (draw.index_count % 3U) != 0U) {
                return rhi_detail::set_error(error_out, "invalid draw index count");
            }

            std::vector<MeshVertex> verts(num_verts);
            if (num_verts > 0U) {
                std::memcpy(verts.data(), vb_it->second.bytes.data(),
                            num_verts * sizeof(MeshVertex));
            }
            std::vector<std::uint16_t> indices(num_indices);
            if (num_indices > 0U) {
                std::memcpy(indices.data(), ib_it->second.bytes.data(),
                            num_indices * sizeof(std::uint16_t));
            }

            rasterize_draw(pass, pip_it->second, draw.mvp, verts, indices, draw.index_count, tex,
                           out_rgba8, depth_buf);
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

    struct ScreenVert {
        float sx = 0.0f;
        float sy = 0.0f;
        float z_ndc = 0.0f;
        float inv_w = 1.0f;
        float u_over_w = 0.0f;
        float v_over_w = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
        bool valid = false;
    };

    static ScreenVert project_vertex(const MeshVertex& v,
                                     const float m[16],
                                     std::uint32_t width,
                                     std::uint32_t height) noexcept {
        ScreenVert sv{};
        const float cx = m[0] * v.px + m[4] * v.py + m[8] * v.pz + m[12];
        const float cy = m[1] * v.px + m[5] * v.py + m[9] * v.pz + m[13];
        const float cz = m[2] * v.px + m[6] * v.py + m[10] * v.pz + m[14];
        const float cw = m[3] * v.px + m[7] * v.py + m[11] * v.pz + m[15];
        if (!(cw > 1.0e-5f) || !std::isfinite(cw)) {
            return sv;
        }
        const float inv_w = 1.0f / cw;
        const float ndc_x = cx * inv_w;
        const float ndc_y = cy * inv_w;
        const float ndc_z = cz * inv_w;
        if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y) || !std::isfinite(ndc_z)) {
            return sv;
        }
        sv.sx = (ndc_x * 0.5f + 0.5f) * static_cast<float>(width);
        sv.sy = (ndc_y * 0.5f + 0.5f) * static_cast<float>(height);
        sv.z_ndc = ndc_z;
        sv.inv_w = inv_w;
        sv.u_over_w = v.u * inv_w;
        sv.v_over_w = v.v * inv_w;
        sv.r = static_cast<float>(v.color_rgba & 0xFFU) / 255.0f;
        sv.g = static_cast<float>((v.color_rgba >> 8U) & 0xFFU) / 255.0f;
        sv.b = static_cast<float>((v.color_rgba >> 16U) & 0xFFU) / 255.0f;
        sv.a = static_cast<float>((v.color_rgba >> 24U) & 0xFFU) / 255.0f;
        sv.valid = true;
        return sv;
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

    static void rasterize_draw(const RenderPassDesc& pass,
                               const PipelineDesc& pip,
                               const float mvp[16],
                               const std::vector<MeshVertex>& verts,
                               const std::vector<std::uint16_t>& indices,
                               std::uint32_t index_count,
                               const TextureRec* tex,
                               std::vector<std::uint8_t>& color_buf,
                               std::vector<float>& depth_buf) noexcept {
        const auto edge_fn = [](const ScreenVert& a, const ScreenVert& b, float px, float py) noexcept {
            return (px - a.sx) * (b.sy - a.sy) - (py - a.sy) * (b.sx - a.sx);
        };

        for (std::uint32_t idx = 0; idx + 2U < index_count; idx += 3U) {
            const std::uint16_t i0 = indices[idx + 0U];
            const std::uint16_t i1 = indices[idx + 1U];
            const std::uint16_t i2 = indices[idx + 2U];
            if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) {
                continue;
            }
            const ScreenVert v0 = project_vertex(verts[i0], mvp, pass.width, pass.height);
            const ScreenVert v1 = project_vertex(verts[i1], mvp, pass.width, pass.height);
            const ScreenVert v2 = project_vertex(verts[i2], mvp, pass.width, pass.height);
            if (!v0.valid || !v1.valid || !v2.valid) {
                continue;
            }

            // Screen space has +Y down: area > 0 corresponds to visual clockwise
            // (or standard CCW in math coordinates before Vulkan Y-flip).
            const float area = edge_fn(v0, v1, v2.sx, v2.sy);
            if (std::fabs(area) < 1.0e-5f) {
                continue;
            }
            if (pip.cull_mode == CullMode::back && area < 0.0f) {
                continue;
            }
            if (pip.cull_mode == CullMode::front && area > 0.0f) {
                continue;
            }

            const float inv_area = 1.0f / area;
            const float min_xf = std::min({v0.sx, v1.sx, v2.sx});
            const float max_xf = std::max({v0.sx, v1.sx, v2.sx});
            const float min_yf = std::min({v0.sy, v1.sy, v2.sy});
            const float max_yf = std::max({v0.sy, v1.sy, v2.sy});

            const int x_begin = std::max(0, static_cast<int>(std::floor(min_xf)));
            const int x_end = std::min(static_cast<int>(pass.width) - 1,
                                       static_cast<int>(std::ceil(max_xf)));
            const int y_begin = std::max(0, static_cast<int>(std::floor(min_yf)));
            const int y_end = std::min(static_cast<int>(pass.height) - 1,
                                       static_cast<int>(std::ceil(max_yf)));

            for (int py = y_begin; py <= y_end; ++py) {
                const float cy = static_cast<float>(py) + 0.5f;
                for (int px = x_begin; px <= x_end; ++px) {
                    const float cx = static_cast<float>(px) + 0.5f;
                    const float w0 = edge_fn(v1, v2, cx, cy) * inv_area;
                    const float w1 = edge_fn(v2, v0, cx, cy) * inv_area;
                    const float w2 = edge_fn(v0, v1, cx, cy) * inv_area;
                    if (w0 < -1.0e-4f || w1 < -1.0e-4f || w2 < -1.0e-4f) {
                        continue;
                    }

                    const float z = w0 * v0.z_ndc + w1 * v1.z_ndc + w2 * v2.z_ndc;
                    if (z < 0.0f || z > 1.0f) {
                        continue;
                    }

                    const std::size_t pix =
                        static_cast<std::size_t>(py) * pass.width + static_cast<std::size_t>(px);
                    if (pip.depth_test && z >= depth_buf[pix]) {
                        continue;
                    }
                    if (pip.depth_write) {
                        depth_buf[pix] = z;
                    }

                    const float inv_w = w0 * v0.inv_w + w1 * v1.inv_w + w2 * v2.inv_w;
                    const float u = (inv_w > 1.0e-6f)
                        ? (w0 * v0.u_over_w + w1 * v1.u_over_w + w2 * v2.u_over_w) / inv_w
                        : 0.0f;
                    const float v = (inv_w > 1.0e-6f)
                        ? (w0 * v0.v_over_w + w1 * v1.v_over_w + w2 * v2.v_over_w) / inv_w
                        : 0.0f;

                    std::uint8_t tr = 255U;
                    std::uint8_t tg = 255U;
                    std::uint8_t tb = 255U;
                    std::uint8_t ta = 255U;
                    sample_texture(tex, u, v, tr, tg, tb, ta);

                    const float vr = std::clamp(w0 * v0.r + w1 * v1.r + w2 * v2.r, 0.0f, 1.0f);
                    const float vg = std::clamp(w0 * v0.g + w1 * v1.g + w2 * v2.g, 0.0f, 1.0f);
                    const float vb = std::clamp(w0 * v0.b + w1 * v1.b + w2 * v2.b, 0.0f, 1.0f);
                    const float va = std::clamp(w0 * v0.a + w1 * v1.a + w2 * v2.a, 0.0f, 1.0f);

                    color_buf[pix * 4U + 0U] = static_cast<std::uint8_t>(std::lround(tr * vr));
                    color_buf[pix * 4U + 1U] = static_cast<std::uint8_t>(std::lround(tg * vg));
                    color_buf[pix * 4U + 2U] = static_cast<std::uint8_t>(std::lround(tb * vb));
                    color_buf[pix * 4U + 3U] = static_cast<std::uint8_t>(std::lround(ta * va));
                }
            }
        }
    }

    std::string device_name_;
    DriverMode mode_;
    std::uint32_t api_version_;
    std::uint32_t next_handle_ = 1;
    std::unordered_map<BufferHandle, BufferRec> buffers_{};
    std::unordered_map<TextureHandle, TextureRec> textures_{};
    std::unordered_map<PipelineHandle, PipelineDesc> pipelines_{};
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

    const std::vector<MeshVertex> verts = build_rhi_vertices_from_brf(mesh);
    const std::vector<std::uint16_t> indices = build_rhi_indices_from_brf(mesh);
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
