// Platform seam: Scene, terrain manifold, placed `.sco` prop batching, and
// skeletal-skinned mesh rendering over the minimal RHI (`docs/PLATFORM_PLAN.md`,
// slice P9 Part 1).
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.5 layout
// and carries no PE/PDB citations.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/asset/dds.h"
#include "wse/asset/skeleton.h"
#include "wse/core/math.h"
#include "wse/game/scene_loaders.h"
#include "wse/platform/rhi.h"
#include "wse/platform/rhi_brf.h"

namespace wse::platform {

// -----------------------------------------------------------------------------
// Column-major 4x4 transform & Warband Z-up <-> RHI helpers
// -----------------------------------------------------------------------------

inline void mat4_identity(float out[16]) noexcept {
    std::memset(out, 0, sizeof(float) * 16U);
    out[0] = out[5] = out[10] = out[15] = 1.0f;
}

inline void mat4_mul(const float a[16], const float b[16], float out[16]) noexcept {
    float tmp[16]{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            tmp[col * 4 + row] = sum;
        }
    }
    std::memcpy(out, tmp, sizeof(tmp));
}

// Converts a `WseTransform` + per-axis scale into a column-major 4x4 model matrix.
inline void transform_to_mat4(const wse::core::WseTransform& tf,
                              const wse::core::WseVec4& scale,
                              float out_m[16]) noexcept {
    out_m[0] = tf.s.x * scale.x;
    out_m[1] = tf.s.y * scale.x;
    out_m[2] = tf.s.z * scale.x;
    out_m[3] = 0.0f;

    out_m[4] = tf.f.x * scale.y;
    out_m[5] = tf.f.y * scale.y;
    out_m[6] = tf.f.z * scale.y;
    out_m[7] = 0.0f;

    out_m[8] = tf.u.x * scale.z;
    out_m[9] = tf.u.y * scale.z;
    out_m[10] = tf.u.z * scale.z;
    out_m[11] = 0.0f;

    out_m[12] = tf.o.x;
    out_m[13] = tf.o.y;
    out_m[14] = tf.o.z;
    out_m[15] = 1.0f;
}

// Determinant +1 change-of-basis from Warband Z-up `(x, y, z)` into `make_orbit_mvp_matrix`
// Y-up world `(x, z, -y)` so outward CCW winding stays intact under `CullMode::back`.
inline void make_zup_to_yup_mat4(float out_m[16]) noexcept {
    std::memset(out_m, 0, sizeof(float) * 16U);
    out_m[0] = 1.0f;    // x' = x
    out_m[6] = -1.0f;   // z' = -y
    out_m[9] = 1.0f;    // y' = z
    out_m[15] = 1.0f;
}

// Builds a Vulkan `[0, 1]` Z, `-Y` up screen view-projection matrix directly in
// Warband's Z-up world coordinate system.
inline void make_zup_look_at_vp(float aspect,
                                const wse::core::WseVec4& eye,
                                const wse::core::WseVec4& target,
                                float fov_deg,
                                float z_near,
                                float z_far,
                                float out_vp[16]) noexcept {
    const float safe_aspect = (std::isfinite(aspect) && aspect > 0.05f) ? aspect : 1.0f;
    const float safe_fov = std::clamp(std::isfinite(fov_deg) ? fov_deg : 60.0f, 15.0f, 150.0f);
    const float safe_near = std::max(0.05f, z_near);
    const float safe_far = std::max(safe_near + 1.0f, z_far);

    wse::core::WseVec4 fwd = target - eye;
    fwd.normalize();

    const wse::core::WseVec4 world_up{0.0f, 0.0f, 1.0f, 0.0f};
    wse::core::WseVec4 right = fwd.cross(world_up);
    if (right.length() <= 1.0e-5f) {
        right = {1.0f, 0.0f, 0.0f, 0.0f};
    } else {
        right.normalize();
    }
    const wse::core::WseVec4 up = right.cross(fwd);

    const float view[16] = {
        right.x, up.x, -fwd.x, 0.0f,
        right.y, up.y, -fwd.y, 0.0f,
        right.z, up.z, -fwd.z, 0.0f,
        -right.dot(eye), -up.dot(eye), fwd.dot(eye), 1.0f,
    };

    const float f = 1.0f / std::tan(safe_fov * 0.5f * 0.017453292519943295f);
    const float a = safe_far / (safe_near - safe_far);
    const float b = (safe_far * safe_near) / (safe_near - safe_far);
    const float proj[16] = {
        f / safe_aspect, 0.0f, 0.0f, 0.0f,
        0.0f, -f, 0.0f, 0.0f,
        0.0f, 0.0f, a, -1.0f,
        0.0f, 0.0f, b, 0.0f,
    };
    mat4_mul(proj, view, out_vp);
}

// -----------------------------------------------------------------------------
// Terrain manifold RHI chunk conversion (WorldMapManifold -> 16-bit batches)
// -----------------------------------------------------------------------------

struct RhiMeshChunk {
    std::vector<MeshVertex> vertices{};
    std::vector<std::uint16_t> indices{};
};

namespace scene_rhi_detail {

// Packs little-endian RGBA8 matching `MeshVertex::color_rgba`.
[[nodiscard]] inline constexpr std::uint32_t pack_rgba8(
    std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a = 255U) noexcept {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8U) |
           (static_cast<std::uint32_t>(b) << 16U) | (static_cast<std::uint32_t>(a) << 24U);
}

[[nodiscard]] inline std::uint32_t terrain_material_rgba8(std::uint16_t material_no) noexcept {
    // Ground palette approximations for Native map.txt material IDs (`0..15`).
    switch (material_no & 0x0FU) {
        case 0: return pack_rgba8(64, 118, 176);   // sea / ocean
        case 1: return pack_rgba8(82, 134, 170);   // shallow water / bridge ford
        case 2: return pack_rgba8(138, 136, 128);  // mountain stone
        case 3: return pack_rgba8(152, 146, 102);  // steppe dry grass
        case 4: return pack_rgba8(102, 140, 74);   // plains / meadow
        case 5: return pack_rgba8(226, 232, 238);  // snow
        case 7: return pack_rgba8(90, 128, 168);   // river
        case 8: return pack_rgba8(168, 138, 92);   // mountain pass / rocky dirt
        case 10: return pack_rgba8(204, 178, 124); // desert
        case 11: return pack_rgba8(66, 108, 60);   // forest floor
        case 12: return pack_rgba8(206, 214, 224); // snow forest
        case 13: return pack_rgba8(186, 162, 114); // desert forest
        default: return pack_rgba8(120, 140, 100);
    }
}

[[nodiscard]] inline BufferHandle upload_vertex_buffer(RhiDevice& rhi,
                                                       std::span<const MeshVertex> verts,
                                                       std::string* error_out) {
    return rhi.create_buffer(
        BufferUsage::vertex,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(verts.data()),
                                      verts.size() * sizeof(MeshVertex)),
        error_out);
}

[[nodiscard]] inline BufferHandle upload_index_buffer(RhiDevice& rhi,
                                                      std::span<const std::uint16_t> indices,
                                                      std::string* error_out) {
    return rhi.create_buffer(
        BufferUsage::index,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(indices.data()),
                                      indices.size() * sizeof(std::uint16_t)),
        error_out);
}

}  // namespace scene_rhi_detail

// Triangulates a `WorldMapManifold` into one or more `< 65536`-vertex chunks
// suitable for `RhiDevice` 16-bit indexed drawing, preserving flat material tints
// and outward surface normals.
[[nodiscard]] inline std::vector<RhiMeshChunk> build_terrain_chunks(
    const wse::game::WorldMapManifold& map,
    float uv_scale = 0.05f,
    std::size_t max_verts_per_chunk = 60000U) {
    std::vector<RhiMeshChunk> chunks;
    const std::size_t cap = std::clamp<std::size_t>(max_verts_per_chunk, 3U, 65532U);
    RhiMeshChunk cur{};

    for (const wse::game::WorldMapFace& face : map.faces) {
        if (face.vertex_indices.size() < 3U) {
            continue;
        }
        const std::uint32_t color = scene_rhi_detail::terrain_material_rgba8(face.material_no);
        const auto& v0 = map.vertices[face.vertex_indices[0]];
        for (std::size_t k = 1; k + 1U < face.vertex_indices.size(); ++k) {
            if (cur.vertices.size() + 3U > cap) {
                chunks.push_back(std::move(cur));
                cur = {};
            }
            const auto& v1 = map.vertices[face.vertex_indices[k]];
            const auto& v2 = map.vertices[face.vertex_indices[k + 1U]];

            const float e1x = v1.x - v0.x;
            const float e1y = v1.y - v0.y;
            const float e1z = v1.z - v0.z;
            const float e2x = v2.x - v0.x;
            const float e2y = v2.y - v0.y;
            const float e2z = v2.z - v0.z;
            float nx = e1y * e2z - e1z * e2y;
            float ny = e1z * e2x - e1x * e2z;
            float nz = e1x * e2y - e1y * e2x;
            const float nlen = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nlen > 1.0e-7f) {
                const float inv = 1.0f / nlen;
                nx *= inv;
                ny *= inv;
                nz *= inv;
            } else {
                nx = 0.0f;
                ny = 0.0f;
                nz = 1.0f;
            }

            const std::uint16_t base = static_cast<std::uint16_t>(cur.vertices.size());
            const std::array<const wse::game::WorldMapVertex*, 3> tri{&v0, &v1, &v2};
            for (const wse::game::WorldMapVertex* vp : tri) {
                cur.vertices.push_back(MeshVertex{
                    vp->x,
                    vp->y,
                    vp->z,
                    nx,
                    ny,
                    nz,
                    vp->x * uv_scale,
                    vp->y * uv_scale,
                    color,
                });
            }
            cur.indices.push_back(base);
            cur.indices.push_back(static_cast<std::uint16_t>(base + 1U));
            cur.indices.push_back(static_cast<std::uint16_t>(base + 2U));
        }
    }
    if (!cur.vertices.empty()) {
        chunks.push_back(std::move(cur));
    }
    return chunks;
}

// -----------------------------------------------------------------------------
// Multi-object RHI scene & skinned character renderer (Slice P9 Part 1)
// -----------------------------------------------------------------------------

using SceneMeshId = std::uint32_t;

struct SceneDrawInstance {
    SceneMeshId mesh_id = 0;
    wse::core::WseTransform transform = wse::core::WseTransform::identity();
    wse::core::WseVec4 scale{1.0f, 1.0f, 1.0f, 1.0f};
};

// Manages uploaded terrain chunks, static `.brf`/`.sco` meshes, and dynamically
// re-skinned character meshes over one `RhiDevice`, submitting them in a single
// render pass. The `RhiDevice` must outlive `BrfSceneRenderer`.
class BrfSceneRenderer {
public:
    BrfSceneRenderer() = default;
    BrfSceneRenderer(const BrfSceneRenderer&) = delete;
    BrfSceneRenderer& operator=(const BrfSceneRenderer&) = delete;
    ~BrfSceneRenderer() { release(); }

    [[nodiscard]] bool init(RhiDevice& rhi,
                            CullMode cull_mode = CullMode::back,
                            FrontFace front_face = FrontFace::counter_clockwise,
                            std::string* error_out = nullptr) {
        release();
        rhi_ = &rhi;
        constexpr std::array<std::uint8_t, 4> kWhitePixel{255U, 255U, 255U, 255U};
        default_tex_ = rhi.create_texture_rgba8(1U, 1U, kWhitePixel, error_out);
        if (default_tex_ == 0U) {
            release();
            return false;
        }
        PipelineDesc pdesc{};
        pdesc.depth_test = true;
        pdesc.depth_write = true;
        pdesc.cull_mode = cull_mode;
        pdesc.front_face = front_face;
        pdesc.enable_lighting = true;
        pipeline_ = rhi.create_pipeline(pdesc, error_out);
        if (pipeline_ == 0U) {
            release();
            return false;
        }
        return true;
    }

    [[nodiscard]] SceneMeshId upload_chunk(const RhiMeshChunk& chunk,
                                           TextureHandle tex = 0U,
                                           std::string* error_out = nullptr) {
        if (rhi_ == nullptr || pipeline_ == 0U || chunk.vertices.empty() || chunk.indices.empty()) {
            return 0U;
        }
        BufferHandle vb = scene_rhi_detail::upload_vertex_buffer(*rhi_, chunk.vertices, error_out);
        if (vb == 0U) {
            return 0U;
        }
        BufferHandle ib = scene_rhi_detail::upload_index_buffer(*rhi_, chunk.indices, error_out);
        if (ib == 0U) {
            rhi_->destroy_buffer(vb);
            return 0U;
        }
        GpuSlot slot{};
        slot.vb = vb;
        slot.ib = ib;
        slot.tex = (tex != 0U) ? tex : default_tex_;
        slot.owns_tex = false;
        slot.index_count = static_cast<std::uint32_t>(chunk.indices.size());
        meshes_.push_back(slot);
        return static_cast<SceneMeshId>(meshes_.size());
    }

    [[nodiscard]] SceneMeshId upload_brf_mesh(const wse::asset::BrfMesh& mesh,
                                              const wse::asset::DdsImageView* dds = nullptr,
                                              std::string* error_out = nullptr) {
        if (rhi_ == nullptr || pipeline_ == 0U || mesh.faces.empty() || mesh.face_corners.empty()) {
            return 0U;
        }
        RhiMeshChunk chunk{};
        build_rhi_mesh_from_brf(mesh, chunk.vertices, chunk.indices);
        if (chunk.vertices.empty() || chunk.indices.empty()) {
            return 0U;
        }
        TextureHandle tex = 0U;
        bool owns_tex = false;
        if (dds != nullptr) {
            std::vector<std::uint8_t> rgba;
            if (!wse::asset::decode_dds_mip_rgba8(*dds, 0U, 0U, rgba, error_out)) {
                return 0U;
            }
            tex = rhi_->create_texture_rgba8(dds->width, dds->height, rgba, error_out);
            if (tex == 0U) {
                return 0U;
            }
            owns_tex = true;
        }
        const SceneMeshId id = upload_chunk(chunk, tex, error_out);
        if (id == 0U) {
            if (owns_tex) {
                rhi_->destroy_texture(tex);
            }
            return 0U;
        }
        meshes_[id - 1U].owns_tex = owns_tex;
        return id;
    }

    // Updates an already-uploaded skinned mesh slot cleanly (swaps GPU VB in place).
    [[nodiscard]] bool update_skinned_mesh(SceneMeshId mesh_id,
                                           const wse::asset::BrfMesh& source_mesh,
                                           std::span<const wse::core::WseTransform> skin_transforms,
                                           std::string* error_out = nullptr) {
        if (rhi_ == nullptr || mesh_id == 0U || mesh_id > meshes_.size()) {
            return false;
        }
        wse::asset::BrfMesh deformed;
        wse::asset::skin_brf_mesh(source_mesh, skin_transforms, deformed);
        std::vector<MeshVertex> verts;
        std::vector<std::uint16_t> indices;
        build_rhi_mesh_from_brf(deformed, verts, indices);
        if (verts.empty() || indices.empty()) {
            return false;
        }
        BufferHandle new_vb = scene_rhi_detail::upload_vertex_buffer(*rhi_, verts, error_out);
        BufferHandle new_ib = scene_rhi_detail::upload_index_buffer(*rhi_, indices, error_out);
        if (new_vb == 0U || new_ib == 0U) {
            if (new_vb != 0U) rhi_->destroy_buffer(new_vb);
            if (new_ib != 0U) rhi_->destroy_buffer(new_ib);
            return false;
        }
        GpuSlot& slot = meshes_[mesh_id - 1U];
        rhi_->destroy_buffer(slot.vb);
        rhi_->destroy_buffer(slot.ib);
        slot.vb = new_vb;
        slot.ib = new_ib;
        slot.index_count = static_cast<std::uint32_t>(indices.size());
        return true;
    }

    [[nodiscard]] bool render(std::uint32_t width,
                              std::uint32_t height,
                              const float view_proj[16],
                              std::span<const SceneDrawInstance> instances,
                              std::vector<std::uint8_t>& out_rgba8,
                              std::string* error_out = nullptr) const {
        out_rgba8.clear();
        if (rhi_ == nullptr || pipeline_ == 0U) {
            return false;
        }
        std::vector<MeshDrawCall> draws;
        draws.reserve(instances.size());
        for (const SceneDrawInstance& inst : instances) {
            if (inst.mesh_id == 0U || inst.mesh_id > meshes_.size()) {
                continue;
            }
            const GpuSlot& slot = meshes_[inst.mesh_id - 1U];
            MeshDrawCall draw{};
            draw.pipeline = pipeline_;
            draw.vertex_buffer = slot.vb;
            draw.index_buffer = slot.ib;
            draw.texture = slot.tex;
            draw.index_count = slot.index_count;
            transform_to_mat4(inst.transform, inst.scale, draw.model);
            mat4_mul(view_proj, draw.model, draw.mvp);
            draws.push_back(draw);
        }
        RenderPassDesc pass{};
        pass.width = width;
        pass.height = height;
        return rhi_->render_pass_and_readback(pass, draws, out_rgba8, error_out);
    }

    void release() noexcept {
        if (rhi_ != nullptr) {
            for (const GpuSlot& slot : meshes_) {
                if (slot.vb != 0U) rhi_->destroy_buffer(slot.vb);
                if (slot.ib != 0U) rhi_->destroy_buffer(slot.ib);
                if (slot.owns_tex && slot.tex != 0U) rhi_->destroy_texture(slot.tex);
            }
            if (pipeline_ != 0U) rhi_->destroy_pipeline(pipeline_);
            if (default_tex_ != 0U) rhi_->destroy_texture(default_tex_);
        }
        meshes_.clear();
        pipeline_ = 0U;
        default_tex_ = 0U;
        rhi_ = nullptr;
    }

private:
    struct GpuSlot {
        BufferHandle vb = 0U;
        BufferHandle ib = 0U;
        TextureHandle tex = 0U;
        bool owns_tex = false;
        std::uint32_t index_count = 0U;
    };

    RhiDevice* rhi_ = nullptr;
    TextureHandle default_tex_ = 0U;
    PipelineHandle pipeline_ = 0U;
    std::vector<GpuSlot> meshes_{};
};

}  // namespace wse::platform
