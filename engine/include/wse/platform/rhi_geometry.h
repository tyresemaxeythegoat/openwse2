// `RhiDevice` that records 2D screen-space triangle batches instead of rasterizing, so an app
// shell can hand them to a GPU 2D API (`SDL_RenderGeometry`) and skip the CPU rasterizer.
//
// Port infrastructure (no retail counterpart). Mirrors `SoftwareRhiDevice`'s conventions so
// the two produce the same picture: clip -> pixel mapping `((ndc + 1) / 2) * size` with row 0
// at NDC `y = -1` (the top of the RGBA8 frame), `v = 0` = first texture row, per-draw
// `tint_rgba` multiplied into the vertex colour, texture colour times vertex colour, the same
// front-face / cull test, and the D3D9 half-pixel shift when the pipeline asks for it.
// Not supported (draws using them are reported through `unsupported_draws()` and still
// recorded with what can be expressed): depth testing (draw order decides, as it does for the
// UI, which turns depth off), alpha test, lighting, normal / specular maps, sRGB, and near-plane
// clipping (triangles with any `w <= 1e-6` are dropped; the UI's orthographic projection keeps
// `w = 1`). Texture sampling filter and wrap are the host's choice.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wse/platform/rhi.h"

namespace wse::platform {

// Same layout as `SDL_Vertex` (`SDL_FPoint position; SDL_FColor color; SDL_FPoint
// tex_coord;`), so a host can pass the array straight through; the SDL adapter checks the
// offsets with `static_assert`.
struct GeometryVertex {
    float x = 0.0f;  // framebuffer pixels, +x right, +y down
    float y = 0.0f;
    float r = 1.0f;  // [0, 1], vertex colour times the draw's tint
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
    float u = 0.0f;  // texture coordinates, (0, 0) = first texel of the first row
    float v = 0.0f;
};
static_assert(sizeof(GeometryVertex) == 8U * sizeof(float));

// Consecutive triangles sharing a texture and blend mode. `indices[first_index ..
// first_index + index_count)` index `vertices[first_vertex ..]` (relative to `first_vertex`).
struct GeometryBatch {
    TextureHandle texture = 0;  // 0 = untextured (vertex colour only)
    BlendMode blend = BlendMode::opaque;
    std::uint32_t first_vertex = 0;
    std::uint32_t vertex_count = 0;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
};

struct GeometryFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t viewport_x = 0;
    std::uint32_t viewport_y = 0;
    std::uint32_t viewport_width = 0;
    std::uint32_t viewport_height = 0;
    std::uint8_t clear_r = 0;
    std::uint8_t clear_g = 0;
    std::uint8_t clear_b = 0;
    std::uint8_t clear_a = 255;
    std::vector<GeometryVertex> vertices{};
    std::vector<std::int32_t> indices{};
    std::vector<GeometryBatch> batches{};
};

class GeometryCaptureRhiDevice final : public RhiDevice {
public:
    struct TextureData {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<std::uint8_t> rgba8{};
    };

    [[nodiscard]] std::string_view backend_name() const noexcept override { return "geometry"; }
    [[nodiscard]] std::string device_name() const override { return "WSE2 geometry capture"; }
    [[nodiscard]] DriverMode driver_mode() const noexcept override { return DriverMode::system; }
    [[nodiscard]] std::uint32_t api_version() const noexcept override { return 0U; }

    [[nodiscard]] BufferHandle create_buffer(BufferUsage usage,
                                             std::span<const std::uint8_t> data,
                                             std::string* error_out = nullptr) override {
        if (data.empty()) {
            (void)rhi_detail::set_error(error_out, "cannot create empty RHI buffer");
            return 0;
        }
        const BufferHandle h = next_handle();
        buffers_.emplace(h, BufferRec{usage, std::vector<std::uint8_t>(data.begin(), data.end())});
        return h;
    }
    void destroy_buffer(BufferHandle handle) override { buffers_.erase(handle); }
    [[nodiscard]] bool update_buffer(BufferHandle handle,
                                     std::span<const std::uint8_t> data,
                                     std::string* error_out = nullptr) override {
        const auto it = buffers_.find(handle);
        if (it == buffers_.end() || data.empty()) {
            return rhi_detail::set_error(error_out, "update_buffer: unknown handle or empty data");
        }
        it->second.bytes.assign(data.begin(), data.end());
        return true;
    }

    [[nodiscard]] TextureHandle create_texture_rgba8(std::uint32_t width,
                                                     std::uint32_t height,
                                                     std::span<const std::uint8_t> rgba8,
                                                     std::string* error_out = nullptr) override {
        if (width == 0U || height == 0U ||
            rgba8.size() != static_cast<std::size_t>(width) * height * 4U) {
            (void)rhi_detail::set_error(error_out, "invalid RGBA8 texture data");
            return 0;
        }
        const TextureHandle h = next_handle();
        textures_.emplace(h, TextureData{width, height,
                                         std::vector<std::uint8_t>(rgba8.begin(), rgba8.end())});
        return h;
    }
    void destroy_texture(TextureHandle handle) override {
        if (textures_.erase(handle) != 0U) {
            destroyed_textures_.push_back(handle);
        }
    }

    [[nodiscard]] ShaderModuleHandle create_shader_module(const ShaderModuleDesc&,
                                                          std::string* error_out = nullptr) override {
        (void)rhi_detail::set_error(error_out, "geometry capture has no shader modules");
        return 0;
    }
    void destroy_shader_module(ShaderModuleHandle) override {}

    [[nodiscard]] PipelineHandle create_pipeline(const PipelineDesc& desc,
                                                 std::string* = nullptr) override {
        const PipelineHandle h = next_handle();
        pipelines_.emplace(h, desc);
        return h;
    }
    void destroy_pipeline(PipelineHandle handle) override { pipelines_.erase(handle); }

    // Places subsequent frames' vertices inside `(x, y, width, height)` in window pixels while
    // keeping `GeometryFrame::width` / `height` at the full pass size; zero width or height
    // resets to the full pass.
    void set_viewport(std::uint32_t x,
                      std::uint32_t y,
                      std::uint32_t width,
                      std::uint32_t height) noexcept {
        viewport_x_ = x;
        viewport_y_ = y;
        viewport_width_ = width;
        viewport_height_ = height;
    }

    // Records `draws` into `frame()` and leaves `out_rgba8` empty: there is no readback.
    [[nodiscard]] bool render_pass_and_readback(const RenderPassDesc& pass,
                                                std::span<const MeshDrawCall> draws,
                                                std::vector<std::uint8_t>& out_rgba8,
                                                std::string* error_out = nullptr) override {
        out_rgba8.clear();
        const bool custom_vp = (viewport_width_ > 0U && viewport_height_ > 0U);
        frame_.width = pass.width;
        frame_.height = pass.height;
        frame_.viewport_x = custom_vp ? viewport_x_ : 0U;
        frame_.viewport_y = custom_vp ? viewport_y_ : 0U;
        frame_.viewport_width = custom_vp ? viewport_width_ : pass.width;
        frame_.viewport_height = custom_vp ? viewport_height_ : pass.height;
        frame_.clear_r = pass.clear_r;
        frame_.clear_g = pass.clear_g;
        frame_.clear_b = pass.clear_b;
        frame_.clear_a = pass.clear_a;
        frame_.vertices.clear();
        frame_.indices.clear();
        frame_.batches.clear();
        if (pass.width == 0U || pass.height == 0U) {
            return rhi_detail::set_error(error_out, "invalid render pass dimensions");
        }
        for (const MeshDrawCall& draw : draws) {
            if (!record_draw(draw, error_out)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] const GeometryFrame& frame() const noexcept { return frame_; }
    [[nodiscard]] const TextureData* texture(TextureHandle handle) const noexcept {
        const auto it = textures_.find(handle);
        return it != textures_.end() ? &it->second : nullptr;
    }
    // Handles destroyed since the last call, so a host can free its GPU copies.
    [[nodiscard]] std::vector<TextureHandle> take_destroyed_textures() {
        return std::exchange(destroyed_textures_, {});
    }
    // Draws that used a feature this device ignores (see the header comment), cumulative.
    [[nodiscard]] std::uint64_t unsupported_draws() const noexcept { return unsupported_draws_; }

private:
    struct BufferRec {
        BufferUsage usage = BufferUsage::vertex;
        std::vector<std::uint8_t> bytes{};
    };

    TextureHandle next_handle() noexcept {
        ++handle_counter_;
        if (handle_counter_ == 0U) {
            ++handle_counter_;
        }
        return handle_counter_;
    }

    bool record_draw(const MeshDrawCall& draw, std::string* error_out) {
        const auto pip_it = pipelines_.find(draw.pipeline);
        const auto vb_it = buffers_.find(draw.vertex_buffer);
        const auto ib_it = buffers_.find(draw.index_buffer);
        if (pip_it == pipelines_.end() || vb_it == buffers_.end() || ib_it == buffers_.end()) {
            return rhi_detail::set_error(error_out, "draw call references invalid RHI handle");
        }
        if (vb_it->second.usage != BufferUsage::vertex || ib_it->second.usage != BufferUsage::index) {
            return rhi_detail::set_error(error_out, "draw call buffer usage mismatch");
        }
        if (draw.texture != 0 && textures_.find(draw.texture) == textures_.end()) {
            return rhi_detail::set_error(error_out, "draw call references invalid texture");
        }
        const PipelineDesc& pip = pip_it->second;
        const std::vector<std::uint8_t>& vb = vb_it->second.bytes;
        const std::vector<std::uint8_t>& ib = ib_it->second.bytes;
        const std::size_t num_verts = vb.size() / sizeof(MeshVertex);
        const std::size_t num_indices = ib.size() / sizeof(std::uint16_t);
        if (draw.first_index > num_indices || draw.index_count > num_indices - draw.first_index ||
            (draw.index_count % 3U) != 0U) {
            return rhi_detail::set_error(error_out, "invalid draw index count");
        }
        if (pip.depth_test || pip.alpha_test || pip.enable_lighting || pip.srgb_texture ||
            pip.srgb_framebuffer || draw.normal_texture != 0 || draw.specular_texture != 0) {
            ++unsupported_draws_;
        }

        // Continue the previous batch when texture and blend match (painter's order is kept:
        // only consecutive draws merge).
        if (frame_.batches.empty() || frame_.batches.back().texture != draw.texture ||
            frame_.batches.back().blend != pip.blend_mode) {
            GeometryBatch b{};
            b.texture = draw.texture;
            b.blend = pip.blend_mode;
            b.first_vertex = static_cast<std::uint32_t>(frame_.vertices.size());
            b.first_index = static_cast<std::uint32_t>(frame_.indices.size());
            frame_.batches.push_back(b);
        }
        GeometryBatch& batch = frame_.batches.back();

        const bool tinted = draw.tint_rgba != 0xFFFFFFFFU;
        const float tint[4] = {static_cast<float>(draw.tint_rgba & 0xFFU) / 255.0f,
                               static_cast<float>((draw.tint_rgba >> 8U) & 0xFFU) / 255.0f,
                               static_cast<float>((draw.tint_rgba >> 16U) & 0xFFU) / 255.0f,
                               static_cast<float>((draw.tint_rgba >> 24U) & 0xFFU) / 255.0f};
        const float* m = draw.mvp;
        const float vx = static_cast<float>(frame_.viewport_x);
        const float vy = static_cast<float>(frame_.viewport_y);
        const float fw = static_cast<float>(frame_.viewport_width);
        const float fh = static_cast<float>(frame_.viewport_height);

        // A draw usually reads a small index range of a shared buffer: transform only the
        // vertices it references, once each (`remap_` holds the batch-relative slot; only the
        // entries this draw touched are reset afterwards, so the cost follows the draw's size,
        // not the buffer's).
        if (remap_.size() < num_verts) {
            remap_.resize(num_verts, -1);
        }
        touched_.clear();
        const auto project = [&](std::uint16_t vi) -> std::int32_t {
            if (remap_[vi] != -1) {
                return remap_[vi];
            }
            touched_.push_back(vi);
            MeshVertex mv{};
            std::memcpy(&mv, vb.data() + static_cast<std::size_t>(vi) * sizeof(MeshVertex),
                        sizeof(MeshVertex));
            const float cx = m[0] * mv.px + m[4] * mv.py + m[8] * mv.pz + m[12];
            const float cy = m[1] * mv.px + m[5] * mv.py + m[9] * mv.pz + m[13];
            const float cw = m[3] * mv.px + m[7] * mv.py + m[11] * mv.pz + m[15];
            if (!(cw > 1e-6f) || !std::isfinite(cx) || !std::isfinite(cy) ||
                !std::isfinite(mv.u) || !std::isfinite(mv.v)) {
                remap_[vi] = -2;  // unusable: drop triangles that use it
                return -2;
            }
            GeometryVertex gv{};
            const float inv_w = 1.0f / cw;
            gv.x = vx + (cx * inv_w * 0.5f + 0.5f) * fw;
            gv.y = vy + (cy * inv_w * 0.5f + 0.5f) * fh;
            if (pip.compensate_d3d9_half_pixel) {
                apply_d3d9_half_pixel_offset_px(gv.x, gv.y, gv.x, gv.y);
            }
            gv.r = static_cast<float>(mv.color_rgba & 0xFFU) / 255.0f;
            gv.g = static_cast<float>((mv.color_rgba >> 8U) & 0xFFU) / 255.0f;
            gv.b = static_cast<float>((mv.color_rgba >> 16U) & 0xFFU) / 255.0f;
            gv.a = static_cast<float>((mv.color_rgba >> 24U) & 0xFFU) / 255.0f;
            if (tinted) {
                gv.r *= tint[0];
                gv.g *= tint[1];
                gv.b *= tint[2];
                gv.a *= tint[3];
            }
            gv.u = mv.u;
            gv.v = mv.v;
            const auto slot = static_cast<std::int32_t>(frame_.vertices.size() - batch.first_vertex);
            frame_.vertices.push_back(gv);
            remap_[vi] = slot;
            return slot;
        };

        const std::uint32_t idx_end = draw.first_index + draw.index_count;
        for (std::uint32_t idx = draw.first_index; idx + 2U < idx_end; idx += 3U) {
            std::uint16_t tri[3]{};
            std::memcpy(tri, ib.data() + static_cast<std::size_t>(idx) * 2U, sizeof(tri));
            if (tri[0] >= num_verts || tri[1] >= num_verts || tri[2] >= num_verts) {
                continue;
            }
            const std::int32_t a = project(tri[0]);
            const std::int32_t b = project(tri[1]);
            const std::int32_t c = project(tri[2]);
            if (a < 0 || b < 0 || c < 0) {
                continue;
            }
            if (pip.cull_mode != CullMode::none) {
                // `SoftwareRhiDevice`'s test: twice the framebuffer-space signed area (+y
                // down), front when > 0 for counter-clockwise front faces.
                const GeometryVertex& s0 = frame_.vertices[batch.first_vertex + static_cast<std::uint32_t>(a)];
                const GeometryVertex& s1 = frame_.vertices[batch.first_vertex + static_cast<std::uint32_t>(b)];
                const GeometryVertex& s2 = frame_.vertices[batch.first_vertex + static_cast<std::uint32_t>(c)];
                const float area = (s2.x - s0.x) * (s1.y - s0.y) - (s2.y - s0.y) * (s1.x - s0.x);
                if (area == 0.0f) {
                    continue;
                }
                const bool front =
                    pip.front_face == FrontFace::counter_clockwise ? area > 0.0f : area < 0.0f;
                if ((pip.cull_mode == CullMode::back && !front) ||
                    (pip.cull_mode == CullMode::front && front)) {
                    continue;
                }
            }
            frame_.indices.push_back(a);
            frame_.indices.push_back(b);
            frame_.indices.push_back(c);
        }
        for (const std::uint16_t vi : touched_) {
            remap_[vi] = -1;
        }
        batch.vertex_count = static_cast<std::uint32_t>(frame_.vertices.size()) - batch.first_vertex;
        batch.index_count = static_cast<std::uint32_t>(frame_.indices.size()) - batch.first_index;
        return true;
    }

    std::unordered_map<BufferHandle, BufferRec> buffers_{};
    std::unordered_map<TextureHandle, TextureData> textures_{};
    std::unordered_map<PipelineHandle, PipelineDesc> pipelines_{};
    std::vector<TextureHandle> destroyed_textures_{};
    std::vector<std::int32_t> remap_{};
    std::vector<std::uint16_t> touched_{};
    GeometryFrame frame_{};
    std::uint32_t viewport_x_ = 0U;
    std::uint32_t viewport_y_ = 0U;
    std::uint32_t viewport_width_ = 0U;
    std::uint32_t viewport_height_ = 0U;
    std::uint64_t unsupported_draws_ = 0U;
    std::uint32_t handle_counter_ = 0U;
};

}  // namespace wse::platform
