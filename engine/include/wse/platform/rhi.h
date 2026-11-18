// Platform seam: minimal cross-platform RHI and the deterministic software
// reference rasterizer (`docs/PLATFORM_PLAN.md`, slice P4).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.6 layout
// and carries no PE/PDB citations.
//
// Standard library only: no `<vulkan/vulkan.h>`, no asset decoders, no Vfs.
// Driver package import lives in `driver_package.h`; `.brf`/`.dds` helpers in
// `rhi_brf.h`.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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

// Parses a stable driver-mode id (`driver_mode_name` output, ASCII
// case-insensitive, surrounding blanks ignored) plus the exact aliases
// `freedreno` / `panfrost` / `custom`. Display labels and free text are
// rejected: substring matching once turned "not turnip" into Turnip.
[[nodiscard]] inline bool parse_driver_mode(std::string_view text, DriverMode& out) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    std::string lower(text);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    if (lower == "system") {
        out = DriverMode::system;
        return true;
    }
    if (lower == "turnip" || lower == "freedreno") {
        out = DriverMode::turnip;
        return true;
    }
    if (lower == "panvk" || lower == "panfrost") {
        out = DriverMode::panvk;
        return true;
    }
    if (lower == "custom_icd" || lower == "custom") {
        out = DriverMode::custom_icd;
        return true;
    }
    return false;
}

// Presentation strategy chosen by the Vulkan driver loader for WSI vs direct
// custom ICD `dlopen` (`docs/PLATFORM_PLAN.md` §P5):
// - System `libvulkan.so` (or a WSI-capable driver exposing `VK_KHR_surface` +
//   `VK_KHR_android_surface`) presents directly to an `ANativeWindow` surface.
// - Bare Adreno Turnip / PanVK `.so` files loaded directly via `dlopen` bypass
//   Android's loader and lack WSI extensions, so frames render offscreen into a
//   GPU image / `AHardwareBuffer` bridge for Android Surface presentation.
enum class PresentationPath : std::uint8_t {
    android_surface_wsi = 0,
    offscreen_hardware_buffer_bridge = 1,
};

[[nodiscard]] inline std::string_view presentation_path_name(
    PresentationPath path) noexcept {
    switch (path) {
        case PresentationPath::android_surface_wsi:
            return "android_surface_wsi";
        case PresentationPath::offscreen_hardware_buffer_bridge:
            return "offscreen_hardware_buffer_bridge";
    }
    return "offscreen_hardware_buffer_bridge";
}

namespace rhi_detail {

inline bool set_error(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
    return false;
}

inline std::uint32_t pack_rgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept {
    return static_cast<std::uint32_t>(r) |
           (static_cast<std::uint32_t>(g) << 8U) |
           (static_cast<std::uint32_t>(b) << 16U) |
           (static_cast<std::uint32_t>(a) << 24U);
}

}  // namespace rhi_detail

// D3D9 -> Vulkan math & color helpers (`docs/PLATFORM_PLAN.md` §P5):
// - Both D3D9 (`D3DXMatrix*RH`) and Vulkan natively use [0, 1] NDC depth (unlike
//   OpenGL's [-1, 1]), so only optional Vulkan Y-flip and the D3D9 half-pixel
//   screen offset differ.
// - Column-major 4x4 matrices (`col * 4 + row`), matching `MeshDrawCall::mvp`.

inline void make_d3d9_perspective_off_center_rh(float left,
                                                float right,
                                                float bottom,
                                                float top,
                                                float z_near,
                                                float z_far,
                                                float out_m[16],
                                                bool vulkan_flip_y = false) noexcept {
    std::memset(out_m, 0, sizeof(float) * 16U);
    const float inv_w = 1.0f / (right - left);
    const float inv_h = 1.0f / (top - bottom);
    const float inv_d = 1.0f / (z_near - z_far);
    const float y_sign = vulkan_flip_y ? -1.0f : 1.0f;

    out_m[0] = 2.0f * z_near * inv_w;
    out_m[8] = (left + right) * inv_w;
    out_m[5] = y_sign * 2.0f * z_near * inv_h;
    out_m[9] = y_sign * (top + bottom) * inv_h;
    out_m[10] = z_far * inv_d;
    out_m[11] = -1.0f;
    out_m[14] = (z_near * z_far) * inv_d;
}

inline void make_d3d9_ortho_off_center_rh(float left,
                                          float right,
                                          float bottom,
                                          float top,
                                          float z_near,
                                          float z_far,
                                          float out_m[16],
                                          bool vulkan_flip_y = false) noexcept {
    std::memset(out_m, 0, sizeof(float) * 16U);
    const float inv_w = 1.0f / (right - left);
    const float inv_h = 1.0f / (top - bottom);
    const float inv_d = 1.0f / (z_near - z_far);
    const float y_sign = vulkan_flip_y ? -1.0f : 1.0f;

    out_m[0] = 2.0f * inv_w;
    out_m[12] = -(right + left) * inv_w;
    out_m[5] = y_sign * 2.0f * inv_h;
    out_m[13] = -y_sign * (top + bottom) * inv_h;
    out_m[10] = inv_d;
    out_m[14] = z_near * inv_d;
    out_m[15] = 1.0f;
}

// Converts D3D9 pixel-center coordinates `(0, 0) .. (W-1, H-1)` (whose quads
// are pre-shifted by `(-0.5, -0.5)` in D3D9) into Vulkan pixel-center
// coordinates `(0.5, 0.5)` in screen pixels or clip-space NDC.
inline void apply_d3d9_half_pixel_offset_px(float x_px,
                                            float y_px,
                                            float& out_x_px,
                                            float& out_y_px) noexcept {
    out_x_px = x_px + 0.5f;
    out_y_px = y_px + 0.5f;
}

// The same `(+0.5, +0.5)`-pixel shift applied to a clip-space position (before the
// perspective divide): half a pixel is `1/W` (`1/H`) in NDC, i.e. `w/W` (`w/H`) in
// clip space. `ndc_y_up` selects the NDC convention of `y_clip`: Vulkan NDC is
// `+Y` down (screen-down shift is `+w/H`); D3D9 NDC is `+Y` up (`-w/H`).
inline void apply_d3d9_half_pixel_offset_clip(float x_clip,
                                              float y_clip,
                                              float w_clip,
                                              std::uint32_t width,
                                              std::uint32_t height,
                                              float& out_x_clip,
                                              float& out_y_clip,
                                              bool ndc_y_up = false) noexcept {
    const float dx = (width > 0U) ? (w_clip / static_cast<float>(width)) : 0.0f;
    const float dy = (height > 0U) ? (w_clip / static_cast<float>(height)) : 0.0f;
    out_x_clip = x_clip + dx;
    out_y_clip = ndc_y_up ? (y_clip - dy) : (y_clip + dy);
}

// Clamps to [0, 1]; NaN maps to 0 (std::clamp would pass NaN through).
[[nodiscard]] inline float unit_clamp(float c) noexcept {
    return (c > 0.0f) ? ((c < 1.0f) ? c : 1.0f) : 0.0f;
}

// [0, 1] float to UNORM8 with round-to-nearest; NaN-safe.
[[nodiscard]] inline std::uint8_t unorm_to_u8(float c) noexcept {
    return static_cast<std::uint8_t>(unit_clamp(c) * 255.0f + 0.5f);
}

// IEC 61966-2-1 sRGB <-> linear float conversions matching D3D9 `D3DRS_SRGBWRITEENABLE`
// and `D3DSAMP_SRGBTEXTURE`.
[[nodiscard]] inline float srgb_to_linear_f32(float c) noexcept {
    const float clamped = unit_clamp(c);
    if (clamped <= 0.04045f) {
        return clamped / 12.92f;
    }
    return std::pow((clamped + 0.055f) / 1.055f, 2.4f);
}

[[nodiscard]] inline float linear_to_srgb_f32(float c) noexcept {
    const float clamped = unit_clamp(c);
    if (clamped <= 0.0031308f) {
        return clamped * 12.92f;
    }
    return 1.055f * std::pow(clamped, 1.0f / 2.4f) - 0.055f;
}

[[nodiscard]] inline std::uint8_t linear_to_srgb_u8(float c) noexcept {
    return unorm_to_u8(linear_to_srgb_f32(c));
}

// Exact `srgb_to_linear_f32(b / 255)` for every byte, from a table built once.
[[nodiscard]] inline float srgb_u8_to_linear_f32(std::uint8_t b) noexcept {
    static const std::array<float, 256> kTable = [] {
        std::array<float, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            t[i] = srgb_to_linear_f32(static_cast<float>(i) / 255.0f);
        }
        return t;
    }();
    return kTable[b];
}

// -----------------------------------------------------------------------------
// Minimal RHI interface & types
// -----------------------------------------------------------------------------

using BufferHandle = std::uint32_t;
using TextureHandle = std::uint32_t;
using ShaderModuleHandle = std::uint32_t;
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

// Winding that marks a front face, in Vulkan framebuffer terms (`+Y` down;
// `VkFrontFace`). D3D9 content such as Warband `.brf` meshes is authored with
// clockwise front faces, so pass `clockwise` for it once a pipeline culls.
enum class FrontFace : std::uint8_t {
    counter_clockwise = 0,
    clockwise = 1,
};

enum class RhiShaderStage : std::uint8_t {
    vertex = 0,
    fragment = 1,
};

struct ShaderModuleDesc {
    RhiShaderStage stage = RhiShaderStage::vertex;
    std::string entry_point = "main";
    std::string name{};
    std::vector<std::uint32_t> spirv_words{};
};

[[nodiscard]] inline bool is_valid_spirv_module(
    std::span<const std::uint32_t> words) noexcept {
    return words.size() >= 5U && words[0] == 0x07230203U && words[3] > 0U;
}

// SPIR-V `ExecutionModel` for an RHI stage (Vertex = 0, Fragment = 4).
[[nodiscard]] inline constexpr std::uint32_t spirv_execution_model(RhiShaderStage stage) noexcept {
    return (stage == RhiShaderStage::fragment) ? 4U : 0U;
}

// True when every instruction's word count fits the module and some `OpEntryPoint`
// has `execution_model` and the literal name `entry_point`.
[[nodiscard]] inline bool spirv_has_entry_point(std::span<const std::uint32_t> words,
                                                std::uint32_t execution_model,
                                                std::string_view entry_point) noexcept {
    if (!is_valid_spirv_module(words) || entry_point.empty()) {
        return false;
    }
    bool found = false;
    std::size_t idx = 5U;
    while (idx < words.size()) {
        const std::uint32_t wc = words[idx] >> 16U;
        if (wc == 0U || idx + wc > words.size()) {
            return false;
        }
        if ((words[idx] & 0xFFFFU) == 15U && wc >= 4U && words[idx + 1U] == execution_model) {
            // Literal name starts at word idx + 3, NUL-terminated within the instruction.
            const std::size_t max_bytes = static_cast<std::size_t>(wc - 3U) * 4U;
            std::size_t len = 0;
            bool terminated = false;
            for (; len < max_bytes; ++len) {
                const std::uint32_t w = words[idx + 3U + len / 4U];
                const char c = static_cast<char>((w >> (8U * (len % 4U))) & 0xFFU);
                if (c == '\0') {
                    terminated = true;
                    break;
                }
                if (len >= entry_point.size() || c != entry_point[len]) {
                    break;
                }
            }
            if (terminated && len == entry_point.size()) {
                found = true;
            }
        }
        idx += wc;
    }
    return found;
}

enum class BlendMode : std::uint8_t {
    opaque = 0,
    alpha = 1,          // SRCALPHA / INVSRCALPHA
    additive_alpha = 2, // SRCALPHA / ONE
    multiply = 3,       // ZERO / SRCCOLOR
    additive = 4,       // ONE / ONE
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

// Keep `depth_test`, `depth_write`, `cull_mode` first so existing aggregate
// initializers `PipelineDesc{true, true, CullMode::back}` continue to compile.
struct PipelineDesc {
    bool depth_test = true;
    bool depth_write = true;
    CullMode cull_mode = CullMode::back;
    ShaderModuleHandle vertex_shader = 0;
    ShaderModuleHandle fragment_shader = 0;
    BlendMode blend_mode = BlendMode::opaque;
    bool alpha_test = false;
    std::uint8_t alpha_ref = 0;
    bool srgb_texture = false;
    bool srgb_framebuffer = false;
    bool enable_lighting = false;
    bool use_normal_map = false;
    bool use_specular_map = false;
    bool compensate_d3d9_half_pixel = false;
    float specular_coefficient = 0.0f;
    float specular_power = 16.0f;
    // Last, so positional aggregate initializers of the fields above still compile.
    FrontFace front_face = FrontFace::counter_clockwise;
};

struct RenderPassDesc {
    std::uint32_t width = 64;
    std::uint32_t height = 64;
    std::uint8_t clear_r = 18;
    std::uint8_t clear_g = 22;
    std::uint8_t clear_b = 30;
    std::uint8_t clear_a = 255;
    float clear_depth = 1.0f;  // must be finite and within [0, 1] (Vulkan rule)
};

// `mvp` is a column-major 4x4 matrix (column `c` at `mvp[c*4 + r]`) mapping object
// space into Vulkan clip space (`x,y in [-w, w]` with `-Y` up, `z in [0, w]`).
struct MeshDrawCall {
    PipelineHandle pipeline = 0;
    BufferHandle vertex_buffer = 0;
    BufferHandle index_buffer = 0;
    TextureHandle texture = 0;
    std::uint32_t index_count = 0;
    // First index read from `index_buffer` (in indices, not bytes), so several draws can share
    // one index buffer (`vkCmdDrawIndexed` `firstIndex`). `first_index + index_count` must not
    // exceed the buffer's index count.
    std::uint32_t first_index = 0;
    float mvp[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    TextureHandle normal_texture = 0;
    TextureHandle specular_texture = 0;
    // Per-draw colour multiplied into every vertex colour, packed like
    // `MeshVertex::color_rgba` (R in the low byte). Mirrors the per-mesh colour that
    // `rglImageWidget::setColor` / `setAlpha` (`0x1402a6ec0` / `0x1402a6f00`) write into the
    // widget mesh for `overlay_set_color` / `overlay_set_alpha`. White leaves draws unchanged.
    std::uint32_t tint_rgba = 0xFFFFFFFFU;
    // Column-major object -> world matrix used only to bring normals and tangents
    // into the (world-space) lighting frame; positions use `mvp`. Normals use its
    // inverse transpose, tangents its upper 3x3.
    float model[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    // Optional scissor clip rectangle in the UI render's virtual coordinates (`UiRenderConfig::virtual_*`,
    // +x right, +y up like the UI projection).
    bool has_clip = false;
    float clip_x0 = 0.0f;
    float clip_y0 = 0.0f;
    float clip_x1 = 0.0f;
    float clip_y1 = 0.0f;
};

namespace rhi_detail {

// Half-open framebuffer-pixel scissor rectangle `[x0, x1) x [y0, y1)` (+y down).
struct ScissorRect {
    std::int64_t x0 = 0;
    std::int64_t y0 = 0;
    std::int64_t x1 = 0;
    std::int64_t y1 = 0;
};

[[nodiscard]] inline bool invert_mat4_f64(const float src[16], double out[16]) noexcept {
    double m[16];
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(src[i])) {
            return false;
        }
        m[i] = static_cast<double>(src[i]);
    }
    double inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!(std::fabs(det) > 1.0e-18) || !std::isfinite(det)) {
        return false;
    }
    const double inv_det = 1.0 / det;
    for (int i = 0; i < 16; ++i) {
        out[i] = inv[i] * inv_det;
    }
    return true;
}

// Projects `draw`'s UI-virtual clip rectangle through `P = draw.mvp * inv(draw.model)` into
// framebuffer pixels `[x0, x1) x [y0, y1)` within `(vp_x, vp_y, vp_w, vp_h)` and `fb_w x fb_h`.
[[nodiscard]] inline ScissorRect compute_draw_scissor(const MeshDrawCall& draw,
                                                      float vp_x,
                                                      float vp_y,
                                                      float vp_w,
                                                      float vp_h,
                                                      std::uint32_t fb_w,
                                                      std::uint32_t fb_h) noexcept {
    if (!draw.has_clip) {
        return ScissorRect{0, 0, static_cast<std::int64_t>(fb_w),
                           static_cast<std::int64_t>(fb_h)};
    }
    if (vp_w <= 0.0f || vp_h <= 0.0f || fb_w == 0U || fb_h == 0U ||
        !std::isfinite(draw.clip_x0) || !std::isfinite(draw.clip_y0) ||
        !std::isfinite(draw.clip_x1) || !std::isfinite(draw.clip_y1) ||
        !(draw.clip_x1 > draw.clip_x0) || !(draw.clip_y1 > draw.clip_y0)) {
        return ScissorRect{};
    }

    bool model_identity = true;
    for (int col = 0; col < 4 && model_identity; ++col) {
        for (int row = 0; row < 4; ++row) {
            const float expected = (col == row) ? 1.0f : 0.0f;
            if (draw.model[col * 4 + row] != expected) {
                model_identity = false;
                break;
            }
        }
    }

    double proj[16]{};
    if (model_identity) {
        for (int i = 0; i < 16; ++i) {
            if (!std::isfinite(draw.mvp[i])) {
                return ScissorRect{};
            }
            proj[i] = static_cast<double>(draw.mvp[i]);
        }
    } else {
        double inv_model[16]{};
        if (!invert_mat4_f64(draw.model, inv_model)) {
            return ScissorRect{};
        }
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                double sum = 0.0;
                for (int k = 0; k < 4; ++k) {
                    sum += static_cast<double>(draw.mvp[k * 4 + row]) *
                           inv_model[col * 4 + k];
                }
                if (!std::isfinite(sum)) {
                    return ScissorRect{};
                }
                proj[col * 4 + row] = sum;
            }
        }
    }

    const double x0 = static_cast<double>(draw.clip_x0);
    const double y0 = static_cast<double>(draw.clip_y0);
    const double x1 = static_cast<double>(draw.clip_x1);
    const double y1 = static_cast<double>(draw.clip_y1);
    const double cx0 = proj[0] * x0 + proj[4] * y0 + proj[12];
    const double cy0 = proj[1] * x0 + proj[5] * y0 + proj[13];
    const double cw0 = proj[3] * x0 + proj[7] * y0 + proj[15];
    const double cx1 = proj[0] * x1 + proj[4] * y1 + proj[12];
    const double cy1 = proj[1] * x1 + proj[5] * y1 + proj[13];
    const double cw1 = proj[3] * x1 + proj[7] * y1 + proj[15];
    if (!(cw0 > 1.0e-6) || !(cw1 > 1.0e-6) || !std::isfinite(cx0) || !std::isfinite(cy0) ||
        !std::isfinite(cx1) || !std::isfinite(cy1)) {
        return ScissorRect{};
    }

    const double px0 =
        static_cast<double>(vp_x) + ((cx0 / cw0) * 0.5 + 0.5) * static_cast<double>(vp_w);
    const double py0 =
        static_cast<double>(vp_y) + ((cy0 / cw0) * 0.5 + 0.5) * static_cast<double>(vp_h);
    const double px1 =
        static_cast<double>(vp_x) + ((cx1 / cw1) * 0.5 + 0.5) * static_cast<double>(vp_w);
    const double py1 =
        static_cast<double>(vp_y) + ((cy1 / cw1) * 0.5 + 0.5) * static_cast<double>(vp_h);

    const double min_bx = std::max(0.0, static_cast<double>(vp_x));
    const double max_bx =
        std::min(static_cast<double>(fb_w), static_cast<double>(vp_x) + static_cast<double>(vp_w));
    const double min_by = std::max(0.0, static_cast<double>(vp_y));
    const double max_by =
        std::min(static_cast<double>(fb_h), static_cast<double>(vp_y) + static_cast<double>(vp_h));
    if (!(max_bx > min_bx) || !(max_by > min_by)) {
        return ScissorRect{};
    }

    const double fx0 = std::clamp(std::min(px0, px1), min_bx, max_bx);
    const double fx1 = std::clamp(std::max(px0, px1), min_bx, max_bx);
    const double fy0 = std::clamp(std::min(py0, py1), min_by, max_by);
    const double fy1 = std::clamp(std::max(py0, py1), min_by, max_by);

    const auto ix0 = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::ceil(fx0 - 0.5)), 0,
                                              static_cast<std::int64_t>(fb_w));
    const auto ix1 = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::ceil(fx1 - 0.5)), 0,
                                              static_cast<std::int64_t>(fb_w));
    const auto iy0 = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::ceil(fy0 - 0.5)), 0,
                                              static_cast<std::int64_t>(fb_h));
    const auto iy1 = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::ceil(fy1 - 0.5)), 0,
                                              static_cast<std::int64_t>(fb_h));
    if (ix1 <= ix0 || iy1 <= iy0) {
        return ScissorRect{};
    }
    return ScissorRect{ix0, iy0, ix1, iy1};
}

}  // namespace rhi_detail

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
    // Replaces the whole contents of a live buffer (any non-empty size; the usage stays), so
    // per-frame geometry can reuse one buffer instead of a create / destroy pair. Safe between
    // `render_pass_and_readback` calls, which are synchronous. False (contents unchanged) for
    // an unknown handle or empty data.
    [[nodiscard]] virtual bool update_buffer(BufferHandle handle,
                                             std::span<const std::uint8_t> data,
                                             std::string* error_out = nullptr) = 0;

    [[nodiscard]] virtual TextureHandle create_texture_rgba8(
        std::uint32_t width,
        std::uint32_t height,
        std::span<const std::uint8_t> rgba8,
        std::string* error_out = nullptr) = 0;
    virtual void destroy_texture(TextureHandle handle) = 0;

    [[nodiscard]] virtual ShaderModuleHandle create_shader_module(
        const ShaderModuleDesc& desc,
        std::string* error_out = nullptr) = 0;
    virtual void destroy_shader_module(ShaderModuleHandle handle) = 0;

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
// rasterizer inside `VulkanRhiDevice` for P4/P5 headless verification.
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

    [[nodiscard]] bool update_buffer(BufferHandle handle,
                                     std::span<const std::uint8_t> data,
                                     std::string* error_out = nullptr) override {
        const auto it = buffers_.find(handle);
        if (it == buffers_.end()) {
            return rhi_detail::set_error(error_out, "update_buffer references invalid RHI buffer");
        }
        if (data.empty()) {
            return rhi_detail::set_error(error_out, "cannot update RHI buffer with empty data");
        }
        it->second.bytes.assign(data.begin(), data.end());
        return true;
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

    [[nodiscard]] ShaderModuleHandle create_shader_module(
        const ShaderModuleDesc& desc,
        std::string* error_out = nullptr) override {
        if (!is_valid_spirv_module(desc.spirv_words)) {
            (void)rhi_detail::set_error(error_out, "invalid SPIR-V shader module payload");
            return 0;
        }
        if (!spirv_has_entry_point(desc.spirv_words, spirv_execution_model(desc.stage),
                                   desc.entry_point)) {
            (void)rhi_detail::set_error(
                error_out, "SPIR-V module has no entry point '" + desc.entry_point +
                               "' for the requested shader stage");
            return 0;
        }
        const ShaderModuleHandle id = allocate_handle();
        ShaderModuleRec rec{};
        rec.stage = desc.stage;
        rec.entry_point = desc.entry_point;
        rec.name = desc.name;
        rec.word_count = desc.spirv_words.size();
        shader_modules_.emplace(id, std::move(rec));
        return id;
    }

    void destroy_shader_module(ShaderModuleHandle handle) override {
        if (handle != 0) {
            shader_modules_.erase(handle);
        }
    }

    [[nodiscard]] PipelineHandle create_pipeline(const PipelineDesc& desc,
                                                 std::string* error_out = nullptr) override {
        // Modules are only needed at pipeline creation: like `VkShaderModule`, they
        // may be destroyed afterwards without affecting the pipeline.
        if (desc.vertex_shader != 0) {
            const auto it = shader_modules_.find(desc.vertex_shader);
            if (it == shader_modules_.end()) {
                (void)rhi_detail::set_error(error_out, "pipeline references invalid vertex shader");
                return 0;
            }
            if (it->second.stage != RhiShaderStage::vertex) {
                (void)rhi_detail::set_error(error_out, "pipeline vertex shader is not a vertex-stage module");
                return 0;
            }
        }
        if (desc.fragment_shader != 0) {
            const auto it = shader_modules_.find(desc.fragment_shader);
            if (it == shader_modules_.end()) {
                (void)rhi_detail::set_error(error_out, "pipeline references invalid fragment shader");
                return 0;
            }
            if (it->second.stage != RhiShaderStage::fragment) {
                (void)rhi_detail::set_error(error_out,
                                            "pipeline fragment shader is not a fragment-stage module");
                return 0;
            }
        }
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
        // Every failure leaves `out_rgba8` empty, never a partly drawn frame
        // that a caller ignoring the result could mistake for output.
        const auto fail = [&](const char* message) {
            out_rgba8.clear();
            return rhi_detail::set_error(error_out, message);
        };
        if (pass.width == 0 || pass.height == 0 || pass.width > 4096U || pass.height > 4096U) {
            return fail("invalid render pass dimensions");
        }
        // A NaN clear depth would make every depth test pass (`z >= NaN` is false).
        if (!(pass.clear_depth >= 0.0f && pass.clear_depth <= 1.0f)) {
            return fail("render pass clear depth must be within [0, 1]");
        }

        const std::size_t pixel_count = static_cast<std::size_t>(pass.width) * pass.height;
        const std::size_t byte_count = pixel_count * 4U;
        out_rgba8.resize(byte_count);
        // Clear by doubling: one 4-byte pattern, then memcpy of the filled prefix.
        const std::array<std::uint8_t, 4> clear{pass.clear_r, pass.clear_g, pass.clear_b,
                                                pass.clear_a};
        std::memcpy(out_rgba8.data(), clear.data(), 4U);
        for (std::size_t filled = 4U; filled < byte_count;) {
            const std::size_t chunk = std::min(filled, byte_count - filled);
            std::memcpy(out_rgba8.data() + filled, out_rgba8.data(), chunk);
            filled += chunk;
        }
        depth_scratch_.assign(pixel_count, pass.clear_depth);

        for (const MeshDrawCall& draw : draws) {
            const auto pip_it = pipelines_.find(draw.pipeline);
            const auto vb_it = buffers_.find(draw.vertex_buffer);
            const auto ib_it = buffers_.find(draw.index_buffer);
            if (pip_it == pipelines_.end() || vb_it == buffers_.end() || ib_it == buffers_.end()) {
                return fail("draw call references invalid RHI handle");
            }
            if (vb_it->second.usage != BufferUsage::vertex ||
                ib_it->second.usage != BufferUsage::index) {
                return fail("draw call buffer usage mismatch");
            }
            const auto find_tex = [&](TextureHandle h, const char* err_msg,
                                      const TextureRec*& out_rec) -> bool {
                out_rec = nullptr;
                if (h == 0) {
                    return true;
                }
                const auto it = textures_.find(h);
                if (it == textures_.end()) {
                    return fail(err_msg);
                }
                out_rec = &it->second;
                return true;
            };
            const TextureRec* tex = nullptr;
            const TextureRec* normal_tex = nullptr;
            const TextureRec* spec_tex = nullptr;
            if (!find_tex(draw.texture, "draw call references invalid texture", tex) ||
                !find_tex(draw.normal_texture, "draw call references invalid normal texture",
                          normal_tex) ||
                !find_tex(draw.specular_texture, "draw call references invalid specular texture",
                          spec_tex)) {
                return false;
            }

            const std::vector<std::uint8_t>& vb = vb_it->second.bytes;
            const std::vector<std::uint8_t>& ib = ib_it->second.bytes;
            const std::size_t num_verts = vb.size() / sizeof(MeshVertex);
            const std::size_t num_indices = ib.size() / sizeof(std::uint16_t);
            if (draw.first_index > num_indices || draw.index_count > num_indices - draw.first_index ||
                (draw.index_count % 3U) != 0U) {
                return fail("invalid draw index count");
            }

            // Vertex stage: transform each vertex once (indexed meshes share
            // vertices between triangles), reading the stored bytes in place.
            const PipelineDesc& pip = pip_it->second;
            const bool lit = pip.enable_lighting;
            const bool need_tangent = lit && pip.use_normal_map && normal_tex != nullptr;
            std::array<float, 9> normal_m{};
            if (lit) {
                normal_m = normal_matrix_from_model(draw.model);
            }
            clip_scratch_.resize(num_verts);
            // `tint_rgba` is constant per draw, so scaling each vertex colour by it equals
            // scaling the interpolated colour per pixel.
            const bool tinted = draw.tint_rgba != 0xFFFFFFFFU;
            const float tint_r = static_cast<float>(draw.tint_rgba & 0xFFU) / 255.0f;
            const float tint_g = static_cast<float>((draw.tint_rgba >> 8U) & 0xFFU) / 255.0f;
            const float tint_b = static_cast<float>((draw.tint_rgba >> 16U) & 0xFFU) / 255.0f;
            const float tint_a = static_cast<float>((draw.tint_rgba >> 24U) & 0xFFU) / 255.0f;
            for (std::size_t i = 0; i < num_verts; ++i) {
                MeshVertex mv{};
                std::memcpy(&mv, vb.data() + i * sizeof(MeshVertex), sizeof(MeshVertex));
                ClipVert cv = to_clip(mv, draw.mvp, lit ? normal_m.data() : nullptr);
                if (tinted) {
                    cv.r *= tint_r;
                    cv.g *= tint_g;
                    cv.b *= tint_b;
                    cv.a *= tint_a;
                }
                clip_scratch_[i] = cv;
            }

            const std::uint32_t idx_end = draw.first_index + draw.index_count;
            const rhi_detail::ScissorRect scissor =
                draw.has_clip
                    ? rhi_detail::compute_draw_scissor(
                          draw, 0.0f, 0.0f, static_cast<float>(pass.width),
                          static_cast<float>(pass.height), pass.width, pass.height)
                    : rhi_detail::ScissorRect{0, 0, static_cast<std::int64_t>(pass.width),
                                              static_cast<std::int64_t>(pass.height)};
            if (scissor.x0 >= scissor.x1 || scissor.y0 >= scissor.y1) {
                continue;
            }
            for (std::uint32_t idx = draw.first_index; idx + 2U < idx_end; idx += 3U) {
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
                TriangleFrame frame{};
                if (need_tangent) {
                    MeshVertex v0{};
                    MeshVertex v1{};
                    MeshVertex v2{};
                    std::memcpy(&v0, vb.data() + tri[0] * sizeof(MeshVertex), sizeof(MeshVertex));
                    std::memcpy(&v1, vb.data() + tri[1] * sizeof(MeshVertex), sizeof(MeshVertex));
                    std::memcpy(&v2, vb.data() + tri[2] * sizeof(MeshVertex), sizeof(MeshVertex));
                    frame = triangle_uv_frame(v0, v1, v2, draw.model);
                }
                clip_and_rasterize(pass, pip, scissor, a, b, c, frame, tex, normal_tex, spec_tex,
                                   out_rgba8, depth_scratch_);
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

    struct ShaderModuleRec {
        RhiShaderStage stage = RhiShaderStage::vertex;
        std::string entry_point{};
        std::string name{};
        std::size_t word_count = 0;
    };

    // Clip-space vertex plus the varyings interpolated across a triangle.
    struct ClipVert {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
        float u = 0.0f;
        float v = 0.0f;
        float nx = 0.0f;
        float ny = 0.0f;
        float nz = 1.0f;
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
        float nx_over_w = 0.0f;
        float ny_over_w = 0.0f;
        float nz_over_w = 0.0f;
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

    // Per-triangle tangent frame derived from object positions and UVs (MeshVertex
    // carries no tangents), in the lighting frame. `valid` is false for degenerate UVs.
    struct TriangleFrame {
        float tx = 1.0f;
        float ty = 0.0f;
        float tz = 0.0f;
        float bx = 0.0f;
        float by = 1.0f;
        float bz = 0.0f;
        bool valid = false;
    };

    // Inverse transpose of the model matrix's upper 3x3, column-major; falls back to
    // the upper 3x3 itself when it is singular.
    static std::array<float, 9> normal_matrix_from_model(const float m[16]) noexcept {
        const float a = m[0], b = m[4], c = m[8];
        const float d = m[1], e = m[5], f = m[9];
        const float g = m[2], h = m[6], i = m[10];
        const float A = e * i - f * h;
        const float B = -(d * i - f * g);
        const float C = d * h - e * g;
        const float det = a * A + b * B + c * C;
        if (!(std::fabs(det) > 1.0e-12f) || !std::isfinite(det)) {
            return {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
        }
        const float inv = 1.0f / det;
        // Cofactor matrix / det == inverse transpose; stored column-major.
        const float D = -(b * i - c * h);
        const float E = a * i - c * g;
        const float F = -(a * h - b * g);
        const float G = b * f - c * e;
        const float H = -(a * f - c * d);
        const float I = a * e - b * d;
        // Row r, column c of the inverse transpose is cofactor(r, c) / det, where
        // A..C, D..F, G..I are cofactor rows 0, 1, 2; emitted column by column.
        return {A * inv, D * inv, G * inv, B * inv, E * inv, H * inv, C * inv, F * inv, I * inv};
    }

    static TriangleFrame triangle_uv_frame(const MeshVertex& v0,
                                           const MeshVertex& v1,
                                           const MeshVertex& v2,
                                           const float model[16]) noexcept {
        TriangleFrame fr{};
        const float e1x = v1.px - v0.px, e1y = v1.py - v0.py, e1z = v1.pz - v0.pz;
        const float e2x = v2.px - v0.px, e2y = v2.py - v0.py, e2z = v2.pz - v0.pz;
        const float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
        const float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;
        const float det = du1 * dv2 - du2 * dv1;
        if (!(std::fabs(det) > 1.0e-12f) || !std::isfinite(det)) {
            return fr;
        }
        const float r = 1.0f / det;
        const float tx = (e1x * dv2 - e2x * dv1) * r;
        const float ty = (e1y * dv2 - e2y * dv1) * r;
        const float tz = (e1z * dv2 - e2z * dv1) * r;
        const float bx = (e2x * du1 - e1x * du2) * r;
        const float by = (e2y * du1 - e1y * du2) * r;
        const float bz = (e2z * du1 - e1z * du2) * r;
        fr.tx = model[0] * tx + model[4] * ty + model[8] * tz;
        fr.ty = model[1] * tx + model[5] * ty + model[9] * tz;
        fr.tz = model[2] * tx + model[6] * ty + model[10] * tz;
        fr.bx = model[0] * bx + model[4] * by + model[8] * bz;
        fr.by = model[1] * bx + model[5] * by + model[9] * bz;
        fr.bz = model[2] * bx + model[6] * by + model[10] * bz;
        fr.valid = std::isfinite(fr.tx) && std::isfinite(fr.ty) && std::isfinite(fr.tz) &&
                   std::isfinite(fr.bx) && std::isfinite(fr.by) && std::isfinite(fr.bz);
        return fr;
    }

    static ClipVert to_clip(const MeshVertex& v, const float m[16], const float* normal_m) noexcept {
        ClipVert cv{};
        cv.x = m[0] * v.px + m[4] * v.py + m[8] * v.pz + m[12];
        cv.y = m[1] * v.px + m[5] * v.py + m[9] * v.pz + m[13];
        cv.z = m[2] * v.px + m[6] * v.py + m[10] * v.pz + m[14];
        cv.w = m[3] * v.px + m[7] * v.py + m[11] * v.pz + m[15];
        cv.finite = std::isfinite(cv.x) && std::isfinite(cv.y) && std::isfinite(cv.z) &&
                    std::isfinite(cv.w) && std::isfinite(v.u) && std::isfinite(v.v);
        cv.u = v.u;
        cv.v = v.v;
        if (normal_m != nullptr) {
            cv.nx = normal_m[0] * v.nx + normal_m[3] * v.ny + normal_m[6] * v.nz;
            cv.ny = normal_m[1] * v.nx + normal_m[4] * v.ny + normal_m[7] * v.nz;
            cv.nz = normal_m[2] * v.nx + normal_m[5] * v.ny + normal_m[8] * v.nz;
        } else {
            cv.nx = v.nx;
            cv.ny = v.ny;
            cv.nz = v.nz;
        }
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
        o.nx = mix(p.nx, q.nx);
        o.ny = mix(p.ny, q.ny);
        o.nz = mix(p.nz, q.nz);
        o.r = mix(p.r, q.r);
        o.g = mix(p.g, q.g);
        o.b = mix(p.b, q.b);
        o.a = mix(p.a, q.a);
        return o;
    }

    static ScreenVert to_screen(const ClipVert& v,
                                std::uint32_t width,
                                std::uint32_t height,
                                bool compensate_d3d9_half_pixel) noexcept {
        ScreenVert s{};
        const float inv_w = 1.0f / v.w;
        float sx = (v.x * inv_w * 0.5f + 0.5f) * static_cast<float>(width);
        float sy = (v.y * inv_w * 0.5f + 0.5f) * static_cast<float>(height);
        if (compensate_d3d9_half_pixel) {
            apply_d3d9_half_pixel_offset_px(sx, sy, sx, sy);
        }
        s.fx = static_cast<std::int64_t>(std::llround(sx * static_cast<float>(kSubpixel)));
        s.fy = static_cast<std::int64_t>(std::llround(sy * static_cast<float>(kSubpixel)));
        s.z_ndc = v.z * inv_w;
        s.inv_w = inv_w;
        s.u_over_w = v.u * inv_w;
        s.v_over_w = v.v * inv_w;
        s.nx_over_w = v.nx * inv_w;
        s.ny_over_w = v.ny * inv_w;
        s.nz_over_w = v.nz * inv_w;
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
                                   const rhi_detail::ScissorRect& scissor,
                                   const ClipVert& a,
                                   const ClipVert& b,
                                   const ClipVert& c,
                                   const TriangleFrame& frame,
                                   const TextureRec* tex,
                                   const TextureRec* normal_tex,
                                   const TextureRec* spec_tex,
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
            screen[i] = to_screen(poly[i], pass.width, pass.height,
                                  pip.compensate_d3d9_half_pixel);
        }
        for (std::size_t i = 1; i + 1U < count; ++i) {
            rasterize_triangle(pass, pip, scissor, screen[0], screen[i], screen[i + 1U], frame, tex,
                               normal_tex, spec_tex, color_buf, depth_buf);
        }
    }

    // Key light normalized from (0.45, 0.70, 0.55); Blinn half vector with view (0, 0, 1),
    // precomputed: normalize(L + V).
    static constexpr float kLightX = 0.451129f;
    static constexpr float kLightY = 0.701757f;
    static constexpr float kLightZ = 0.551380f;
    static constexpr float kHalfX = 0.256110f;
    static constexpr float kHalfY = 0.398394f;
    static constexpr float kHalfZ = 0.880733f;

    static void normalize3(float& x, float& y, float& z) noexcept {
        const float len = std::sqrt(x * x + y * y + z * z);
        if (len > 1.0e-6f) {
            x /= len;
            y /= len;
            z /= len;
        } else {
            x = 0.0f;
            y = 0.0f;
            z = 1.0f;
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
                                   const rhi_detail::ScissorRect& scissor,
                                   const ScreenVert& s0,
                                   ScreenVert s1,
                                   ScreenVert s2,
                                   const TriangleFrame& frame,
                                   const TextureRec* tex,
                                   const TextureRec* normal_tex,
                                   const TextureRec* spec_tex,
                                   std::vector<std::uint8_t>& color_buf,
                                   std::vector<float>& depth_buf) noexcept {
        // E(a, b, p) = (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x).
        const auto edge = [](const ScreenVert& ea, const ScreenVert& eb, std::int64_t px,
                             std::int64_t py) noexcept {
            return (px - ea.fx) * (eb.fy - ea.fy) - (py - ea.fy) * (eb.fx - ea.fx);
        };

        // E(s0, s1, s2) equals twice Vulkan's framebuffer-space signed area
        // (`-1/2 * sum(x_i * y_{i+1} - x_{i+1} * y_i)`, `+Y` down), so area > 0 is
        // counter-clockwise in `VkFrontFace` terms: drawn clockwise on screen
        // appears as area < 0.
        std::int64_t area = edge(s0, s1, s2.fx, s2.fy);
        if (area == 0) {
            return;
        }
        const bool front = (pip.front_face == FrontFace::counter_clockwise) ? (area > 0) : (area < 0);
        if (pip.cull_mode == CullMode::back && !front) {
            return;
        }
        if (pip.cull_mode == CullMode::front && front) {
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
        const std::int64_t x_begin =
            std::max<std::int64_t>(scissor.x0, floor_div(min_fx - half + kSubpixel - 1, kSubpixel));
        const std::int64_t x_end =
            std::min<std::int64_t>(scissor.x1 - 1, floor_div(max_fx - half, kSubpixel));
        const std::int64_t y_begin =
            std::max<std::int64_t>(scissor.y0, floor_div(min_fy - half + kSubpixel - 1, kSubpixel));
        const std::int64_t y_end =
            std::min<std::int64_t>(scissor.y1 - 1, floor_div(max_fy - half, kSubpixel));
        if (x_begin > x_end || y_begin > y_end) {
            return;
        }

        const float inv_area = 1.0f / static_cast<float>(area);
        // Per-pixel steps of each edge function (exact integers).
        const std::int64_t step_x0 = (s2.fy - s1.fy) * kSubpixel;
        const std::int64_t step_x1 = (s0.fy - s2.fy) * kSubpixel;
        const std::int64_t step_x2 = (s1.fy - s0.fy) * kSubpixel;
        const bool fast_p4_path = !pip.alpha_test && !pip.srgb_texture && !pip.srgb_framebuffer &&
                                  !pip.enable_lighting && pip.blend_mode == BlendMode::opaque;
        const float spec_power = std::max(1.0f, pip.specular_power);

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

                const float src_a = (static_cast<float>(ta) / 255.0f) * va;
                const std::uint8_t out_a_u8 = static_cast<std::uint8_t>(std::lround(ta * va));
                if (pip.alpha_test && out_a_u8 < pip.alpha_ref) {
                    continue;
                }
                if (pip.depth_write) {
                    depth_buf[pix] = z;
                }

                std::uint8_t* dst = color_buf.data() + pix * 4U;
                if (fast_p4_path) {
                    dst[0] = static_cast<std::uint8_t>(std::lround(tr * vr));
                    dst[1] = static_cast<std::uint8_t>(std::lround(tg * vg));
                    dst[2] = static_cast<std::uint8_t>(std::lround(tb * vb));
                    dst[3] = out_a_u8;
                    continue;
                }

                float sr = static_cast<float>(tr) / 255.0f;
                float sg = static_cast<float>(tg) / 255.0f;
                float sb = static_cast<float>(tb) / 255.0f;
                if (pip.srgb_texture) {
                    sr = srgb_u8_to_linear_f32(tr);
                    sg = srgb_u8_to_linear_f32(tg);
                    sb = srgb_u8_to_linear_f32(tb);
                }
                sr *= vr;
                sg *= vg;
                sb *= vb;

                if (pip.enable_lighting) {
                    float nx = interp(s0.nx_over_w, s1.nx_over_w, s2.nx_over_w);
                    float ny = interp(s0.ny_over_w, s1.ny_over_w, s2.ny_over_w);
                    float nz = interp(s0.nz_over_w, s1.nz_over_w, s2.nz_over_w);
                    normalize3(nx, ny, nz);

                    if (pip.use_normal_map && normal_tex != nullptr) {
                        std::uint8_t nr = 128U, ng = 128U, nb = 255U, na = 255U;
                        sample_texture(normal_tex, u, v, nr, ng, nb, na);
                        const float tnx = (static_cast<float>(nr) / 255.0f) * 2.0f - 1.0f;
                        const float tny = (static_cast<float>(ng) / 255.0f) * 2.0f - 1.0f;
                        const float tnz = (static_cast<float>(nb) / 255.0f) * 2.0f - 1.0f;
                        float tx = 0.0f;
                        float ty = 0.0f;
                        float tz = 0.0f;
                        float handed = 1.0f;
                        bool have_t = false;
                        if (frame.valid) {
                            // Gram-Schmidt the triangle's UV tangent against the
                            // interpolated normal; the UV bitangent sets handedness.
                            const float ndt = nx * frame.tx + ny * frame.ty + nz * frame.tz;
                            tx = frame.tx - nx * ndt;
                            ty = frame.ty - ny * ndt;
                            tz = frame.tz - nz * ndt;
                            const float len2 = tx * tx + ty * ty + tz * tz;
                            if (len2 > 1.0e-12f) {
                                const float il = 1.0f / std::sqrt(len2);
                                tx *= il;
                                ty *= il;
                                tz *= il;
                                const float cx = ny * tz - nz * ty;
                                const float cy = nz * tx - nx * tz;
                                const float cz = nx * ty - ny * tx;
                                handed = (cx * frame.bx + cy * frame.by + cz * frame.bz < 0.0f)
                                             ? -1.0f
                                             : 1.0f;
                                have_t = true;
                            }
                        }
                        if (!have_t) {
                            // Degenerate UVs: any tangent perpendicular to the normal.
                            const float ux = (std::fabs(ny) < 0.99f) ? 0.0f : 1.0f;
                            const float uy = (std::fabs(ny) < 0.99f) ? 1.0f : 0.0f;
                            tx = uy * nz;
                            ty = -ux * nz;
                            tz = ux * ny - uy * nx;
                            normalize3(tx, ty, tz);
                        }
                        const float bx = handed * (ny * tz - nz * ty);
                        const float by = handed * (nz * tx - nx * tz);
                        const float bz = handed * (nx * ty - ny * tx);
                        const float mx = tx * tnx + bx * tny + nx * tnz;
                        const float my = ty * tnx + by * tny + ny * tnz;
                        const float mz = tz * tnx + bz * tny + nz * tnz;
                        nx = mx;
                        ny = my;
                        nz = mz;
                        normalize3(nx, ny, nz);
                    }

                    const float ndotl =
                        std::max(0.0f, nx * kLightX + ny * kLightY + nz * kLightZ);
                    const float diffuse = 0.28f + 0.72f * ndotl;

                    float spec = 0.0f;
                    if (pip.specular_coefficient > 0.0f) {
                        const float ndoth =
                            std::max(0.0f, nx * kHalfX + ny * kHalfY + nz * kHalfZ);
                        float spec_mask = 1.0f;
                        if (pip.use_specular_map && spec_tex != nullptr) {
                            std::uint8_t mr = 255U, mg = 255U, mb = 255U, ma = 255U;
                            sample_texture(spec_tex, u, v, mr, mg, mb, ma);
                            spec_mask = static_cast<float>(mr) / 255.0f;
                        }
                        spec = pip.specular_coefficient * spec_mask *
                               std::pow(ndoth, spec_power);
                    }

                    sr = sr * diffuse + spec;
                    sg = sg * diffuse + spec;
                    sb = sb * diffuse + spec;
                }

                float out_r = sr;
                float out_g = sg;
                float out_b = sb;
                const float dst_a = static_cast<float>(dst[3]) / 255.0f;
                float out_a = src_a;
                if (pip.blend_mode != BlendMode::opaque) {
                    // The destination is only read (and sRGB-decoded) when blending.
                    float dst_r = static_cast<float>(dst[0]) / 255.0f;
                    float dst_g = static_cast<float>(dst[1]) / 255.0f;
                    float dst_b = static_cast<float>(dst[2]) / 255.0f;
                    if (pip.srgb_framebuffer) {
                        dst_r = srgb_u8_to_linear_f32(dst[0]);
                        dst_g = srgb_u8_to_linear_f32(dst[1]);
                        dst_b = srgb_u8_to_linear_f32(dst[2]);
                    }
                    // D3D9 without D3DRS_SEPARATEALPHABLENDENABLE: alpha uses the
                    // same source/destination factors as colour.
                    switch (pip.blend_mode) {
                        case BlendMode::opaque:
                            break;
                        case BlendMode::alpha:  // SRCALPHA / INVSRCALPHA
                            out_r = sr * src_a + dst_r * (1.0f - src_a);
                            out_g = sg * src_a + dst_g * (1.0f - src_a);
                            out_b = sb * src_a + dst_b * (1.0f - src_a);
                            out_a = src_a * src_a + dst_a * (1.0f - src_a);
                            break;
                        case BlendMode::additive_alpha:  // SRCALPHA / ONE
                            out_r = dst_r + sr * src_a;
                            out_g = dst_g + sg * src_a;
                            out_b = dst_b + sb * src_a;
                            out_a = dst_a + src_a * src_a;
                            break;
                        case BlendMode::multiply:  // ZERO / SRCCOLOR
                            out_r = sr * dst_r;
                            out_g = sg * dst_g;
                            out_b = sb * dst_b;
                            out_a = src_a * dst_a;
                            break;
                        case BlendMode::additive:  // ONE / ONE
                            out_r = dst_r + sr;
                            out_g = dst_g + sg;
                            out_b = dst_b + sb;
                            out_a = dst_a + src_a;
                            break;
                    }
                }

                if (pip.srgb_framebuffer) {
                    dst[0] = linear_to_srgb_u8(out_r);
                    dst[1] = linear_to_srgb_u8(out_g);
                    dst[2] = linear_to_srgb_u8(out_b);
                } else {
                    dst[0] = unorm_to_u8(out_r);
                    dst[1] = unorm_to_u8(out_g);
                    dst[2] = unorm_to_u8(out_b);
                }
                dst[3] = (pip.blend_mode == BlendMode::opaque) ? out_a_u8 : unorm_to_u8(out_a);
            }
        }
    }

    // Handle 0 is reserved for "invalid". After the 32-bit counter wraps, skip
    // 0 and every value still owned by a live buffer, texture, shader module, or
    // pipeline so a new object never aliases an old one.
    std::uint32_t allocate_handle() noexcept {
        for (;;) {
            const std::uint32_t id = next_handle_++;
            if (id == 0) {
                continue;
            }
            if (!wrapped_) {
                if (next_handle_ == 0) {
                    wrapped_ = true;
                }
                return id;
            }
            if (buffers_.count(id) == 0 && textures_.count(id) == 0 &&
                shader_modules_.count(id) == 0 && pipelines_.count(id) == 0) {
                return id;
            }
        }
    }

  public:
    // Test hook: continues handle allocation from `next` as if the counter had
    // already wrapped once (exercises the reuse guard without 2^32 allocations).
    void debug_set_next_handle(std::uint32_t next) noexcept {
        next_handle_ = next;
        wrapped_ = true;
    }

  private:

    std::string device_name_;
    DriverMode mode_;
    std::uint32_t api_version_;
    std::uint32_t next_handle_ = 1;
    bool wrapped_ = false;
    std::unordered_map<BufferHandle, BufferRec> buffers_{};
    std::unordered_map<TextureHandle, TextureRec> textures_{};
    std::unordered_map<ShaderModuleHandle, ShaderModuleRec> shader_modules_{};
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


// Builds a column-major 4x4 perspective * view matrix targeting the origin `(0, 0, 0)`
// using Vulkan `[0, 1]` clip-space depth and `-Y` up screen coordinates.
inline void make_orbit_mvp_matrix(float aspect,
                                  float distance,
                                  float yaw_rad,
                                  float pitch_rad,
                                  float out_mvp[16]) noexcept {
    // Non-finite inputs fall back to defaults instead of filling the matrix with NaN.
    const float safe_aspect = (std::isfinite(aspect) && aspect > 0.05f) ? aspect : 1.0f;
    const float safe_dist =
        (std::isfinite(distance) && distance > 0.25f) ? std::min(distance, 1.0e6f) : 0.25f;
    if (!std::isfinite(yaw_rad)) {
        yaw_rad = 0.0f;
    }
    if (!std::isfinite(pitch_rad)) {
        pitch_rad = 0.0f;
    }

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
    // The far plane grows with the orbit distance so the target (and a mesh of
    // radius up to ~distance around it) never falls behind it; the near plane
    // scales with it to keep depth precision.
    const float znear = std::max(0.1f, safe_dist * 1.0e-3f);
    const float zfar = std::max(100.0f, safe_dist * 4.0f);
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
