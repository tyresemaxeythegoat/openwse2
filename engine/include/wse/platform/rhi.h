// Platform seam: minimal cross-platform RHI and the deterministic software
// reference rasterizer (`docs/PLATFORM_PLAN.md`, slice P4).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.5 layout
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

// Winding that marks a front face, in Vulkan framebuffer terms (`+Y` down;
// `VkFrontFace`). D3D9 content such as Warband `.brf` meshes is authored with
// clockwise front faces, so pass `clockwise` for it once a pipeline culls.
enum class FrontFace : std::uint8_t {
    counter_clockwise = 0,
    clockwise = 1,
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
            const TextureRec* tex = nullptr;
            if (draw.texture != 0) {
                const auto tex_it = textures_.find(draw.texture);
                if (tex_it == textures_.end()) {
                    return fail("draw call references invalid texture");
                }
                tex = &tex_it->second;
            }

            const std::vector<std::uint8_t>& vb = vb_it->second.bytes;
            const std::vector<std::uint8_t>& ib = ib_it->second.bytes;
            const std::size_t num_verts = vb.size() / sizeof(MeshVertex);
            const std::size_t num_indices = ib.size() / sizeof(std::uint16_t);
            if (draw.index_count > num_indices || (draw.index_count % 3U) != 0U) {
                return fail("invalid draw index count");
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

    // Handle 0 is reserved for "invalid". After the 32-bit counter wraps, skip
    // 0 and every value still owned by a live buffer, texture, or pipeline so a
    // new object never aliases an old one.
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
            if (buffers_.count(id) == 0 && textures_.count(id) == 0 && pipelines_.count(id) == 0) {
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
