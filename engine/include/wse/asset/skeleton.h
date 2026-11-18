// Clean-room v1.1.5.5 skeletal pose evaluation and CPU linear-blend skinning.
//
// Reconstructed from v1.1.5.5 PE + PDB evidence:
// - rglSkeletonModel::readStream (0x14030dff0): rest-world bone chain + origins
// - rglSkeleton::updateBonePositions (0x1403098c0)
// - rglSkeleton::getBoneTransformForAnimation (0x14030b530)
// - rglQuaternion::interpolate (0x1402d6e20)
// - rglTransform::setRotation (0x1402b6840)
// - rglBone::getSkinningTransform (0x140258980)
// - rglSkeleton::skinMesh (0x14030c6b0)
// - rglMesh::updateSkinData (0x1402b9ba0)
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/core/math.h"

namespace wse::asset {

inline constexpr std::size_t kMaxVertexSkinInfluences = 4U;
inline constexpr float kMinSkinWeightThreshold = 1.0e-5f;  // __real@3727c5ac at 0x1402b9c9d

[[nodiscard]] inline Quatf normalize_quat(const Quatf& q) noexcept {
    const float norm2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (!std::isfinite(norm2) || norm2 <= 1.0e-12f) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    const float inv = 1.0f / std::sqrt(norm2);
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// Mirrors rglTransform::setRotation(const rglQuaternion&) (0x1402b6840), where
// rglQuaternion stores {w, x, y, z} in memory and Quatf exposes {x, y, z, w}.
inline void set_rotation_quat(core::WseTransform& out, const Quatf& raw_q) noexcept {
    const Quatf q = normalize_quat(raw_q);
    const float xx = 2.0f * q.x * q.x;
    const float yy = 2.0f * q.y * q.y;
    const float zz = 2.0f * q.z * q.z;
    const float xy = 2.0f * q.x * q.y;
    const float xz = 2.0f * q.x * q.z;
    const float yz = 2.0f * q.y * q.z;
    const float wx = 2.0f * q.w * q.x;
    const float wy = 2.0f * q.w * q.y;
    const float wz = 2.0f * q.w * q.z;

    out.s = {1.0f - (yy + zz), xy + wz, xz - wy, 0.0f};
    out.f = {xy - wz, 1.0f - (xx + zz), yz + wx, 0.0f};
    out.u = {xz + wy, yz - wx, 1.0f - (xx + yy), 0.0f};
}

[[nodiscard]] inline core::WseTransform transform_from_skeleton_bone(
    const BrfSkeletonBone& bone) noexcept {
    core::WseTransform tf{};
    tf.s = {bone.rot_s.x, bone.rot_s.y, bone.rot_s.z, 0.0f};
    tf.f = {bone.rot_f.x, bone.rot_f.y, bone.rot_f.z, 0.0f};
    tf.u = {bone.rot_u.x, bone.rot_u.y, bone.rot_u.z, 0.0f};
    tf.o = {bone.origin.x, bone.origin.y, bone.origin.z, 1.0f};
    return tf;
}

[[nodiscard]] inline core::WseVec4 transform_point(const core::WseTransform& tf,
                                                   const Vec3f& p) noexcept {
    return {
        tf.s.x * p.x + tf.f.x * p.y + tf.u.x * p.z + tf.o.x,
        tf.s.y * p.x + tf.f.y * p.y + tf.u.y * p.z + tf.o.y,
        tf.s.z * p.x + tf.f.z * p.y + tf.u.z * p.z + tf.o.z,
        1.0f,
    };
}

[[nodiscard]] inline core::WseVec4 transform_dir(const core::WseTransform& tf,
                                                 const Vec3f& d) noexcept {
    return {
        tf.s.x * d.x + tf.f.x * d.y + tf.u.x * d.z,
        tf.s.y * d.x + tf.f.y * d.y + tf.u.y * d.z,
        tf.s.z * d.x + tf.f.z * d.y + tf.u.z * d.z,
        0.0f,
    };
}

// Mirrors rglQuaternion::interpolate (0x1402d6e20) shortest-arc linear blend,
// normalized so rigid bone transforms preserve unit scale on coarse keys.
[[nodiscard]] inline Quatf interpolate_quat(const Quatf& q0,
                                            const Quatf& q1,
                                            float t) noexcept {
    const float clamped_t = std::isfinite(t) ? std::clamp(t, 0.0f, 1.0f) : 0.0f;
    const float dot = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
    const float w0 = 1.0f - clamped_t;
    const float w1 = (dot < 0.0f) ? -clamped_t : clamped_t;
    return normalize_quat({
        q0.x * w0 + q1.x * w1,
        q0.y * w0 + q1.y * w1,
        q0.z * w0 + q1.z * w1,
        q0.w * w0 + q1.w * w1,
    });
}

// Binary-searches bracketing keys in [keys.front().time_frame .. back().time_frame]
// matching rglIpo::getKeyForTime (0x1401035e0).
template <typename KeyT>
inline void find_ipo_bracket(std::span<const KeyT> keys,
                             float frame_time,
                             std::size_t& out_i0,
                             std::size_t& out_i1,
                             float& out_alpha) noexcept {
    if (keys.size() <= 1U || frame_time <= static_cast<float>(keys.front().time_frame)) {
        out_i0 = 0U;
        out_i1 = 0U;
        out_alpha = 0.0f;
        return;
    }
    if (frame_time >= static_cast<float>(keys.back().time_frame)) {
        out_i0 = keys.size() - 1U;
        out_i1 = keys.size() - 1U;
        out_alpha = 0.0f;
        return;
    }
    const auto it = std::upper_bound(
        keys.begin(), keys.end(), frame_time,
        [](float t, const KeyT& k) { return t < static_cast<float>(k.time_frame); });
    const std::size_t upper = static_cast<std::size_t>(it - keys.begin());
    out_i0 = (upper > 0U) ? (upper - 1U) : 0U;
    out_i1 = std::min(out_i0 + 1U, keys.size() - 1U);
    const float t0 = static_cast<float>(keys[out_i0].time_frame);
    const float t1 = static_cast<float>(keys[out_i1].time_frame);
    const float span = t1 - t0;
    out_alpha = (span > 1.0e-6f) ? std::clamp((frame_time - t0) / span, 0.0f, 1.0f) : 0.0f;
}

[[nodiscard]] inline Quatf sample_quat_ipo(std::span<const BrfQuatKey> keys,
                                           float frame_time) noexcept {
    if (keys.empty()) {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    std::size_t i0 = 0;
    std::size_t i1 = 0;
    float alpha = 0.0f;
    find_ipo_bracket(keys, frame_time, i0, i1, alpha);
    if (i0 == i1) {
        return normalize_quat(keys[i0].rotation);
    }
    return interpolate_quat(keys[i0].rotation, keys[i1].rotation, alpha);
}

[[nodiscard]] inline Vec3f sample_pos_ipo(std::span<const BrfPosKey> keys,
                                          float frame_time) noexcept {
    if (keys.empty()) {
        return {};
    }
    std::size_t i0 = 0;
    std::size_t i1 = 0;
    float alpha = 0.0f;
    find_ipo_bracket(keys, frame_time, i0, i1, alpha);
    const Vec3f& a = keys[i0].position;
    if (i0 == i1) {
        return a;
    }
    const Vec3f& b = keys[i1].position;
    const float w0 = 1.0f - alpha;
    return {
        a.x * w0 + b.x * alpha,
        a.y * w0 + b.y * alpha,
        a.z * w0 + b.z * alpha,
    };
}

// Precomputed rest-pose bone hierarchy matching rglSkeletonModel (0x14030dff0).
struct SkeletonBindPose {
    std::vector<std::int32_t> parent_indices{};
    std::vector<core::WseTransform> local_rest{};
    std::vector<core::WseTransform> world_rest{};
};

namespace skeleton_detail {

template <typename EvalFn>
inline void evaluate_bone_hierarchy(std::span<const std::int32_t> parents,
                                    EvalFn&& eval_local,
                                    std::vector<core::WseTransform>& out_world) {
    const std::size_t n = parents.size();
    out_world.assign(n, core::WseTransform::identity());
    std::vector<std::uint8_t> state(n, 0U);  // 0=unvisited, 1=visiting, 2=done

    auto solve = [&](auto& self, std::size_t i) -> void {
        if (state[i] == 2U || state[i] == 1U) {
            return;
        }
        state[i] = 1U;
        const core::WseTransform local = eval_local(i);
        const std::int32_t p = parents[i];
        if (p >= 0 && static_cast<std::size_t>(p) < n &&
            static_cast<std::size_t>(p) != i &&
            state[static_cast<std::size_t>(p)] != 1U) {
            self(self, static_cast<std::size_t>(p));
            out_world[i] = out_world[static_cast<std::size_t>(p)].transform_to_parent(local);
        } else {
            out_world[i] = local;
        }
        state[i] = 2U;
    };

    for (std::size_t i = 0; i < n; ++i) {
        solve(solve, i);
    }
}

}  // namespace skeleton_detail

// Mirrors rglSkeletonModel::readStream (0x14030e100..0x14030e2ca) hierarchy build.
[[nodiscard]] inline SkeletonBindPose build_skeleton_bind_pose(const BrfSkeleton& skel) {
    SkeletonBindPose pose{};
    const std::size_t n = skel.bones.size();
    pose.parent_indices.reserve(n);
    pose.local_rest.reserve(n);
    for (const BrfSkeletonBone& bone : skel.bones) {
        pose.parent_indices.push_back(bone.parent_index);
        pose.local_rest.push_back(transform_from_skeleton_bone(bone));
    }
    skeleton_detail::evaluate_bone_hierarchy(
        pose.parent_indices,
        [&](std::size_t i) noexcept { return pose.local_rest[i]; },
        pose.world_rest);
    return pose;
}

// Evaluates world-space bone transforms at normalized progress in [0, 1] over
// [start_frame .. end_frame] (rglSkeleton::getBoneTransformForAnimation 0x14030b530,
// root motion 0x14030b72b..0x14030b756; rglSkeleton::updateBonePositions 0x1403098c0,
// root motion 0x14030a892..0x14030a8cb: m_positionIpo adds directly to root bone[0].m_transform.o).
// If `anim == nullptr`, returns the bind rest pose (`pose.world_rest`).
inline void evaluate_skeleton_pose(const SkeletonBindPose& pose,
                                   const BrfSkeletonAnimation* anim,
                                   std::int32_t start_frame,
                                   std::int32_t end_frame,
                                   float progress,
                                   std::vector<core::WseTransform>& out_bone_world) {
    if (anim == nullptr) {
        out_bone_world = pose.world_rest;
        return;
    }
    const float clamped_p = std::isfinite(progress) ? std::clamp(progress, 0.0f, 1.0f) : 0.0f;
    const float frame_time = static_cast<float>(start_frame) +
                             static_cast<float>(end_frame - start_frame) * clamped_p;
    const Vec3f root_delta = sample_pos_ipo(anim->root_positions, frame_time);

    skeleton_detail::evaluate_bone_hierarchy(
        pose.parent_indices,
        [&](std::size_t i) noexcept {
            core::WseTransform local = pose.local_rest[i];
            if (i < anim->bone_rotations.size() && !anim->bone_rotations[i].empty()) {
                set_rotation_quat(local, sample_quat_ipo(anim->bone_rotations[i], frame_time));
            }
            if (i == 0U) {
                local.o.x += root_delta.x;
                local.o.y += root_delta.y;
                local.o.z += root_delta.z;
            }
            return local;
        },
        out_bone_world);
}

// Computes rigid skinning transforms `anim_world[i] * inverse(rest_world[i])`,
// equivalent to rglBone::getSkinningTransform (0x140258980) + rest-origin offset
// applied by rglSkeleton::skinMesh (0x14030c8c7).
inline void compute_skinning_transforms(const SkeletonBindPose& pose,
                                        std::span<const core::WseTransform> bone_world,
                                        std::vector<core::WseTransform>& out_skin) {
    const std::size_t n = pose.world_rest.size();
    out_skin.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const core::WseTransform& b = pose.world_rest[i];
        const core::WseTransform& w =
            (i < bone_world.size()) ? bone_world[i] : pose.world_rest[i];

        // Transpose of 3x3 bind basis (orthonormal rigid inverse rotation).
        core::WseTransform inv_b_rot{};
        inv_b_rot.s = {b.s.x, b.f.x, b.u.x, 0.0f};
        inv_b_rot.f = {b.s.y, b.f.y, b.u.y, 0.0f};
        inv_b_rot.u = {b.s.z, b.f.z, b.u.z, 0.0f};

        core::WseTransform skin = w.transform_to_parent(inv_b_rot);
        const Vec3f rest_origin{b.o.x, b.o.y, b.o.z};
        const core::WseVec4 rotated_rest = transform_dir(skin, rest_origin);
        skin.o = {
            w.o.x - rotated_rest.x,
            w.o.y - rotated_rest.y,
            w.o.z - rotated_rest.z,
            1.0f,
        };
        out_skin[i] = skin;
    }
}

struct VertexSkinWeights {
    std::array<std::uint8_t, kMaxVertexSkinInfluences> bones{};
    std::array<float, kMaxVertexSkinInfluences> weights{};
};

// Extracts up to 4 per-vertex bone weights from `BrfMesh::vertex_groups`
// (rglMesh::updateSkinData 0x1402b9ba0). Safety deviation: a vertex with more than 4
// influences keeps its 4 largest weights (replacing the smallest held one), independent
// of vertex-group order; `skin_brf_mesh` renormalizes what is kept.
[[nodiscard]] inline std::vector<VertexSkinWeights> build_mesh_skin_data(
    const BrfMesh& mesh,
    std::size_t num_positions) {
    std::vector<VertexSkinWeights> skin_data(num_positions);
    for (const BrfVertexGroup& group : mesh.vertex_groups) {
        for (const BrfVertexWeight& pw : group.pairs) {
            if (pw.vertex_index < 0 ||
                static_cast<std::size_t>(pw.vertex_index) >= num_positions ||
                !std::isfinite(pw.weight) || pw.weight <= kMinSkinWeightThreshold) {
                continue;
            }
            VertexSkinWeights& dst = skin_data[static_cast<std::size_t>(pw.vertex_index)];
            std::size_t smallest = 0;
            for (std::size_t slot = 1; slot < kMaxVertexSkinInfluences; ++slot) {
                if (dst.weights[slot] < dst.weights[smallest]) {
                    smallest = slot;
                }
            }
            if (pw.weight > dst.weights[smallest]) {
                dst.bones[smallest] = group.bone_no;
                dst.weights[smallest] = pw.weight;
            }
        }
    }
    return skin_data;
}

// Evaluates CPU linear-blend skinning for one rest position and optional rest normal
// across up to 4 weighted bone influences, renormalizing kept weights (same math used
// by `skin_brf_mesh` and `BrfSceneRenderer::update_skinned_mesh`). Pass `out_pos ==
// nullptr` to skin only the normal (no position transforms are evaluated then).
inline void evaluate_skinned_vertex(const Vec3f& rest_pos,
                                    const Vec3f* rest_normal,
                                    const VertexSkinWeights& sw,
                                    std::span<const core::WseTransform> skin_transforms,
                                    Vec3f* out_pos,
                                    Vec3f* out_normal) noexcept {
    const bool want_pos = (out_pos != nullptr);
    const bool want_nrm = (rest_normal != nullptr && out_normal != nullptr);
    Vec3f pos_acc{};
    float pos_wsum = 0.0f;
    Vec3f nrm_acc{};
    float nrm_wsum = 0.0f;
    for (std::size_t k = 0; k < kMaxVertexSkinInfluences; ++k) {
        const float w = sw.weights[k];
        if (w <= kMinSkinWeightThreshold || sw.bones[k] >= skin_transforms.size()) {
            continue;
        }
        const core::WseTransform& xf = skin_transforms[sw.bones[k]];
        if (want_pos) {
            const core::WseVec4 tp = transform_point(xf, rest_pos);
            pos_acc.x += tp.x * w;
            pos_acc.y += tp.y * w;
            pos_acc.z += tp.z * w;
            pos_wsum += w;
        }
        if (want_nrm) {
            const core::WseVec4 tn = transform_dir(xf, *rest_normal);
            nrm_acc.x += tn.x * w;
            nrm_acc.y += tn.y * w;
            nrm_acc.z += tn.z * w;
            nrm_wsum += w;
        }
    }
    // Divide by the kept weight sum so dropped influences or weights that do not add up
    // to 1 do not pull the vertex toward the bone-space origin.
    if (want_pos) {
        if (pos_wsum > kMinSkinWeightThreshold) {
            const float inv_wsum = 1.0f / pos_wsum;
            *out_pos = {pos_acc.x * inv_wsum, pos_acc.y * inv_wsum, pos_acc.z * inv_wsum};
        } else {
            *out_pos = rest_pos;
        }
    }
    if (want_nrm) {
        if (nrm_wsum > kMinSkinWeightThreshold) {
            const float len2 = nrm_acc.x * nrm_acc.x + nrm_acc.y * nrm_acc.y + nrm_acc.z * nrm_acc.z;
            if (len2 > 1.0e-12f) {
                const float inv = 1.0f / std::sqrt(len2);
                *out_normal = {nrm_acc.x * inv, nrm_acc.y * inv, nrm_acc.z * inv};
            } else {
                *out_normal = *rest_normal;
            }
        } else {
            *out_normal = *rest_normal;
        }
    }
}

// Applies CPU linear-blend skinning (rglSkeleton::skinMesh 0x14030c6b0) to a
// `BrfMesh`, producing a pose-deformed copy suitable for `build_rhi_mesh_from_brf`.
// Unweighted vertices or out-of-range bone indices fall back safely to bind pose.
inline void skin_brf_mesh(const BrfMesh& src,
                          std::span<const core::WseTransform> skin_transforms,
                          BrfMesh& out,
                          std::size_t frame_index = 0U) {
    out = src;
    if (src.vertex_groups.empty() || skin_transforms.empty()) {
        return;
    }
    const std::vector<Vec3f>& base_pos =
        (frame_index < src.vertex_keys.size() && !src.vertex_keys[frame_index].positions.empty())
            ? src.vertex_keys[frame_index].positions
            : src.positions;
    const std::size_t nv = base_pos.size();
    const std::vector<VertexSkinWeights> weights = build_mesh_skin_data(src, nv);

    out.vertex_keys.clear();
    out.positions.resize(nv);
    for (std::size_t vi = 0; vi < nv; ++vi) {
        evaluate_skinned_vertex(base_pos[vi], nullptr, weights[vi], skin_transforms,
                                &out.positions[vi], nullptr);
    }

    for (std::size_t ci = 0; ci < src.face_corners.size(); ++ci) {
        const std::int32_t vidx = src.face_corners[ci].vertex_index;
        if (vidx < 0 || static_cast<std::size_t>(vidx) >= nv) {
            continue;
        }
        evaluate_skinned_vertex(base_pos[static_cast<std::size_t>(vidx)],
                                &src.face_corners[ci].normal,
                                weights[static_cast<std::size_t>(vidx)],
                                skin_transforms,
                                nullptr,
                                &out.face_corners[ci].normal);
    }
}

}  // namespace wse::asset
