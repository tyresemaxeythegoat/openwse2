// Platform seam: `.brf` mesh / `.dds` texture helpers for the minimal RHI
// (`docs/PLATFORM_PLAN.md`, slice P4). Consumes decoded `wse::asset::BrfMesh`
// and `wse::asset::DdsImageView` data.
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.6 layout
// and carries no PE/PDB citations.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/asset/dds.h"
#include "wse/platform/rhi.h"

namespace wse::platform {

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
// with byte-identical attributes (position, normal, UV, colour; plus source
// `vertex_index` when `out_vert_pos_indices != nullptr` so coincident vertices
// bound to different bones stay distinct) share one vertex, and unreferenced
// corners are dropped. Faces naming a corner past `face_corners` are skipped.
inline void build_rhi_mesh_from_brf_mapped(const wse::asset::BrfMesh& mesh,
                                           std::vector<MeshVertex>& out_verts,
                                           std::vector<std::uint16_t>& out_indices,
                                           std::vector<std::int32_t>* out_vert_pos_indices,
                                           std::size_t frame_index = 0) {
    static_assert(sizeof(MeshVertex) == 9U * 4U, "MeshVertex must have no padding bytes");
    out_verts.clear();
    out_indices.clear();
    if (out_vert_pos_indices != nullptr) {
        out_vert_pos_indices->clear();
    }
    const std::vector<MeshVertex> corners = build_rhi_vertices_from_brf(mesh, frame_index);
    const bool keep_pos_map = (out_vert_pos_indices != nullptr);

    struct CornerHash {
        const std::vector<MeshVertex>* verts;
        const std::vector<wse::asset::BrfFaceCorner>* brf_corners;
        bool check_pos_idx;
        std::size_t operator()(std::uint32_t i) const noexcept {
            const auto* p = reinterpret_cast<const std::uint8_t*>(&(*verts)[i]);
            std::uint64_t h = 14695981039346656037ULL;
            for (std::size_t k = 0; k < sizeof(MeshVertex); ++k) {
                h = (h ^ p[k]) * 1099511628211ULL;
            }
            if (check_pos_idx) {
                const auto vidx = static_cast<std::uint32_t>((*brf_corners)[i].vertex_index);
                h = (h ^ vidx) * 1099511628211ULL;
            }
            return static_cast<std::size_t>(h);
        }
    };
    struct CornerEq {
        const std::vector<MeshVertex>* verts;
        const std::vector<wse::asset::BrfFaceCorner>* brf_corners;
        bool check_pos_idx;
        bool operator()(std::uint32_t a, std::uint32_t b) const noexcept {
            if (check_pos_idx &&
                (*brf_corners)[a].vertex_index != (*brf_corners)[b].vertex_index) {
                return false;
            }
            return std::memcmp(&(*verts)[a], &(*verts)[b], sizeof(MeshVertex)) == 0;
        }
    };
    std::unordered_map<std::uint32_t, std::uint16_t, CornerHash, CornerEq> unique(
        corners.size(), CornerHash{&corners, &mesh.face_corners, keep_pos_map},
        CornerEq{&corners, &mesh.face_corners, keep_pos_map});
    constexpr std::uint32_t kUnmapped = 0xFFFFFFFFU;
    std::vector<std::uint32_t> remap(corners.size(), kUnmapped);

    out_verts.reserve(corners.size());
    if (out_vert_pos_indices != nullptr) {
        out_vert_pos_indices->reserve(corners.size());
    }
    out_indices.reserve(mesh.faces.size() * 3U);
    for (const wse::asset::BrfMeshFace& face : mesh.faces) {
        if (face.corners[0] >= corners.size() || face.corners[1] >= corners.size() ||
            face.corners[2] >= corners.size()) {
            continue;
        }
        for (const std::uint16_t corner : face.corners) {
            if (remap[corner] == kUnmapped) {
                if (out_verts.size() >= 65536U) {
                    out_verts.clear();
                    out_indices.clear();
                    if (out_vert_pos_indices != nullptr) {
                        out_vert_pos_indices->clear();
                    }
                    return;
                }
                const auto [it, inserted] = unique.try_emplace(
                    corner, static_cast<std::uint16_t>(out_verts.size()));
                if (inserted) {
                    out_verts.push_back(corners[corner]);
                    if (out_vert_pos_indices != nullptr) {
                        out_vert_pos_indices->push_back(mesh.face_corners[corner].vertex_index);
                    }
                }
                remap[corner] = it->second;
            }
            out_indices.push_back(static_cast<std::uint16_t>(remap[corner]));
        }
    }
}

inline void build_rhi_mesh_from_brf(const wse::asset::BrfMesh& mesh,
                                    std::vector<MeshVertex>& out_verts,
                                    std::vector<std::uint16_t>& out_indices,
                                    std::size_t frame_index = 0) {
    build_rhi_mesh_from_brf_mapped(mesh, out_verts, out_indices, nullptr, frame_index);
}

// Uploads one textured static `.brf` mesh to an `RhiDevice` once and renders it
// any number of times (a viewer re-renders per orbit step; re-decoding the DDS,
// re-building the de-duplicated streams, and re-creating buffers, texture, and
// pipeline every frame was the dominant cost). Releases its device objects on
// destruction; the device must outlive it.
class BrfMeshRenderer {
public:
    BrfMeshRenderer() = default;
    BrfMeshRenderer(const BrfMeshRenderer&) = delete;
    BrfMeshRenderer& operator=(const BrfMeshRenderer&) = delete;
    ~BrfMeshRenderer() { release(); }

    [[nodiscard]] bool upload(RhiDevice& rhi,
                              const wse::asset::BrfMesh& mesh,
                              const wse::asset::DdsImageView& dds,
                              std::string* error_out = nullptr,
                              CullMode cull_mode = CullMode::back,
                              FrontFace front_face = FrontFace::counter_clockwise) {
        release();
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

        rhi_ = &rhi;
        const std::span<const std::uint8_t> vb_bytes(
            reinterpret_cast<const std::uint8_t*>(verts.data()), verts.size() * sizeof(MeshVertex));
        const std::span<const std::uint8_t> ib_bytes(
            reinterpret_cast<const std::uint8_t*>(indices.data()),
            indices.size() * sizeof(std::uint16_t));
        vb_ = rhi.create_buffer(BufferUsage::vertex, vb_bytes, error_out);
        if (vb_ != 0) {
            ib_ = rhi.create_buffer(BufferUsage::index, ib_bytes, error_out);
        }
        if (ib_ != 0) {
            tex_ = rhi.create_texture_rgba8(dds.width, dds.height, tex_rgba8, error_out);
        }
        if (tex_ != 0) {
            PipelineDesc pip_desc{};
            pip_desc.depth_test = true;
            pip_desc.depth_write = true;
            pip_desc.cull_mode = cull_mode;
            pip_desc.front_face = front_face;
            pip_ = rhi.create_pipeline(pip_desc, error_out);
        }
        if (pip_ == 0) {
            release();
            return false;
        }
        index_count_ = static_cast<std::uint32_t>(indices.size());
        return true;
    }

    [[nodiscard]] bool ready() const noexcept { return pip_ != 0; }

    [[nodiscard]] bool render(std::uint32_t width,
                              std::uint32_t height,
                              const float mvp[16],
                              std::vector<std::uint8_t>& out_rgba8,
                              std::string* error_out = nullptr) const {
        out_rgba8.clear();
        if (!ready()) {
            return rhi_detail::set_error(error_out, "BrfMeshRenderer has no uploaded mesh");
        }
        MeshDrawCall draw{};
        draw.pipeline = pip_;
        draw.vertex_buffer = vb_;
        draw.index_buffer = ib_;
        draw.texture = tex_;
        draw.index_count = index_count_;
        std::memcpy(draw.mvp, mvp, sizeof(draw.mvp));
        RenderPassDesc pass{};
        pass.width = width;
        pass.height = height;
        const std::array<MeshDrawCall, 1> draws{draw};
        return rhi_->render_pass_and_readback(pass, draws, out_rgba8, error_out);
    }

    void release() noexcept {
        if (rhi_ != nullptr) {
            if (pip_ != 0) {
                rhi_->destroy_pipeline(pip_);
            }
            if (tex_ != 0) {
                rhi_->destroy_texture(tex_);
            }
            if (ib_ != 0) {
                rhi_->destroy_buffer(ib_);
            }
            if (vb_ != 0) {
                rhi_->destroy_buffer(vb_);
            }
        }
        rhi_ = nullptr;
        vb_ = ib_ = 0;
        tex_ = 0;
        pip_ = 0;
        index_count_ = 0;
    }

private:
    RhiDevice* rhi_ = nullptr;
    BufferHandle vb_ = 0;
    BufferHandle ib_ = 0;
    TextureHandle tex_ = 0;
    PipelineHandle pip_ = 0;
    std::uint32_t index_count_ = 0;
};

// One-shot textured static `.brf` mesh render through any `RhiDevice` (uploads,
// renders, releases). Use `BrfMeshRenderer` to render the same mesh repeatedly.
[[nodiscard]] inline bool render_textured_brf_mesh_frame(
    RhiDevice& rhi,
    const wse::asset::BrfMesh& mesh,
    const wse::asset::DdsImageView& dds,
    std::uint32_t width,
    std::uint32_t height,
    const float mvp[16],
    std::vector<std::uint8_t>& out_rgba8,
    std::string* error_out = nullptr,
    CullMode cull_mode = CullMode::back,
    FrontFace front_face = FrontFace::counter_clockwise) {
    out_rgba8.clear();
    BrfMeshRenderer renderer;
    if (!renderer.upload(rhi, mesh, dds, error_out, cull_mode, front_face)) {
        return false;
    }
    return renderer.render(width, height, mvp, out_rgba8, error_out);
}

}  // namespace wse::platform
