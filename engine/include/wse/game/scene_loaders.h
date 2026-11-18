// Clean-room v1.1.5.6 map manifold, scene object (.sco), and skeleton bodies
// XML loaders via Vfs/TokenReader/BinaryReader.
//
// Reconstructed from v1.1.5.6 PE + PDB evidence:
// - mbGame::readMap (0x1400d9b60), rglManifold::readStream (0x1402b1490),
//   rglManifoldFace::readStream (0x1402b1400), rglManifold::computeBounds (0x1402b1f70)
// - mbSite::readSceneObjFile (0x140210d60), mbMissionObject::readStream (0x140152d60),
//   rglAiMesh::readStream (0x140252780)
// - mbGame::readSkeletons (0x1400cbcd0), mbSkeletonManager::readFile (0x140211d20),
//   mbSkeleton::readXMLElement (0x140211b40), mbSkeletonBone::readXMLElement (0x140211600),
//   rglSkeletonModelBody::readXMLElement (0x14030d7f0)
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wse/core/math.h"
#include "wse/game/module_loader_common.h"
#include "wse/game/world_state.h"
#include "wse/io/binary_stream.h"
#include "wse/io/text_stream.h"
#include "wse/io/xml.h"
#include "wse/platform/vfs.h"

namespace wse::game {

// -----------------------------------------------------------------------------
// World map manifold (`Modules/<mod>/map.txt`: rglManifold 0x1402b1490)
// -----------------------------------------------------------------------------

struct WorldMapVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct WorldMapFace {
    std::uint16_t material_no = 0;  // bottom 8 bits at mbGame::readMap (0x1400d9cfe)
    std::uint16_t extra = 0;        // upper 8 bits when packed >= 0x100 (0x1400d9d08)
    std::uint32_t flags = 0;
    std::vector<std::uint32_t> vertex_indices{};
};

struct WorldMapManifold {
    std::vector<WorldMapVertex> vertices{};
    std::vector<WorldMapFace> faces{};
    WorldMapVertex min_bounds{};
    WorldMapVertex max_bounds{};

    // Mirrors rglManifold::computeBounds (0x1402b1f70).
    void compute_bounds() noexcept {
        if (vertices.empty()) {
            min_bounds = {};
            max_bounds = {};
            return;
        }
        min_bounds = vertices[0];
        max_bounds = vertices[0];
        for (const WorldMapVertex& v : vertices) {
            min_bounds.x = std::min(min_bounds.x, v.x);
            min_bounds.y = std::min(min_bounds.y, v.y);
            min_bounds.z = std::min(min_bounds.z, v.z);
            max_bounds.x = std::max(max_bounds.x, v.x);
            max_bounds.y = std::max(max_bounds.y, v.y);
            max_bounds.z = std::max(max_bounds.z, v.z);
        }
    }

    // Samples ground elevation z and material id over (x, y) by barycentric
    // fan-triangle testing across manifold faces. Returns false outside the mesh.
    [[nodiscard]] bool sample_surface_at(float x,
                                         float y,
                                         float& out_z,
                                         std::uint16_t* out_material = nullptr) const noexcept {
        if (x < min_bounds.x || x > max_bounds.x || y < min_bounds.y || y > max_bounds.y) {
            return false;
        }
        constexpr float kEps = -1.0e-5f;
        for (const WorldMapFace& face : faces) {
            if (face.vertex_indices.size() < 3U) {
                continue;
            }
            const WorldMapVertex& v0 = vertices[face.vertex_indices[0]];
            for (std::size_t k = 1; k + 1U < face.vertex_indices.size(); ++k) {
                const WorldMapVertex& v1 = vertices[face.vertex_indices[k]];
                const WorldMapVertex& v2 = vertices[face.vertex_indices[k + 1U]];

                const float d00 = v1.x - v0.x;
                const float d01 = v1.y - v0.y;
                const float d10 = v2.x - v0.x;
                const float d11 = v2.y - v0.y;
                const float denom = d00 * d11 - d01 * d10;
                if (std::fabs(denom) <= 1.0e-9f) {
                    continue;
                }
                const float inv_denom = 1.0f / denom;
                const float px = x - v0.x;
                const float py = y - v0.y;
                const float u = (px * d11 - py * d10) * inv_denom;
                const float v = (d00 * py - d01 * px) * inv_denom;
                const float w = 1.0f - u - v;
                if (u >= kEps && v >= kEps && w >= kEps) {
                    out_z = w * v0.z + u * v1.z + v * v2.z;
                    if (out_material != nullptr) {
                        *out_material = face.material_no;
                    }
                    return true;
                }
            }
        }
        return false;
    }
};

[[nodiscard]] inline ModuleLoadError read_manifold_map(io::TokenReader& reader,
                                                       WorldMapManifold& out,
                                                       std::string_view src = {}) {
    out = {};
    std::int32_t num_verts = 0;
    ModuleLoadError err{};
    if (!read_count_token(reader, num_verts, kMaxModuleRecords, src, "map vertices", err)) {
        return err;
    }
    // A vertex is at least 3 one-character tokens plus separators (6 bytes); checking this
    // before `reserve` stops a short file from reserving storage for a huge declared count.
    if (reader.plausible_count(static_cast<std::size_t>(num_verts), 6U) <
        static_cast<std::size_t>(num_verts)) {
        return make_load_error(src, "truncated map vertex table");
    }
    out.vertices.reserve(static_cast<std::size_t>(num_verts));
    for (std::int32_t i = 0; i < num_verts; ++i) {
        WorldMapVertex v{};
        if (!reader.read_f32(v.x) || !reader.read_f32(v.y) || !reader.read_f32(v.z)) {
            return make_load_error(src, "truncated map vertex", static_cast<std::size_t>(i));
        }
        out.vertices.push_back(v);
    }

    std::int32_t num_faces = 0;
    if (!read_count_token(reader, num_faces, kMaxModuleRecords, src, "map faces", err)) {
        return err;
    }
    // A face is at least 6 tokens (material, flags, count, 3 indices): 12 bytes.
    if (reader.plausible_count(static_cast<std::size_t>(num_faces), 12U) <
        static_cast<std::size_t>(num_faces)) {
        return make_load_error(src, "truncated map face table");
    }
    out.faces.reserve(static_cast<std::size_t>(num_faces));
    for (std::int32_t f = 0; f < num_faces; ++f) {
        std::uint32_t packed_material = 0;
        std::uint32_t flags = 0;
        std::uint32_t n_verts = 0;
        if (!read_u32_token(reader, packed_material) || !read_u32_token(reader, flags) ||
            !read_u32_token(reader, n_verts)) {
            return make_load_error(src, "truncated map face header", static_cast<std::size_t>(f));
        }
        if (n_verts < 3U || n_verts > 64U) {
            return make_load_error(src, "invalid map face vertex count", static_cast<std::size_t>(f));
        }
        WorldMapFace face{};
        if (packed_material >= 0x100U) {
            face.extra = static_cast<std::uint16_t>(packed_material >> 8U);
            face.material_no = static_cast<std::uint16_t>(packed_material & 0xFFU);
        } else {
            face.material_no = static_cast<std::uint16_t>(packed_material);
        }
        face.flags = flags;
        face.vertex_indices.reserve(n_verts);
        for (std::uint32_t k = 0; k < n_verts; ++k) {
            std::uint32_t vidx = 0;
            if (!read_u32_token(reader, vidx) || vidx >= out.vertices.size()) {
                return make_load_error(src, "map face vertex index out of range",
                                       static_cast<std::size_t>(f));
            }
            face.vertex_indices.push_back(vidx);
        }
        out.faces.push_back(std::move(face));
    }

    out.compute_bounds();
    return {};
}

[[nodiscard]] inline ModuleLoadError load_manifold_map(
    platform::Vfs& vfs,
    std::string_view path,
    WorldMapManifold& out) {
    out = {};
    std::string text;
    ModuleLoadError err{};
    if (!read_vfs_text_checked(vfs, path, text, err)) {
        return err;
    }
    io::TokenReader reader(text);
    return read_manifold_map(reader, out, path);
}

// -----------------------------------------------------------------------------
// Scene object binary files (`SceneObj/*.sco`: mbSite::readSceneObjFile 0x140210d60)
// -----------------------------------------------------------------------------

inline constexpr std::uint32_t kSceneObjMagic = 0xFFFFFD33U;
inline constexpr std::uint32_t kSceneGroundPaintMagic = 0xFF4AD1A6U;

struct LoadedSceneObject {
    std::uint32_t kind_type = 0;  // 0=scene_prop, 2=item, 4=flora (0x140152d60)
    std::uint32_t sub_kind = 0;
    std::uint32_t file_var_id = 0;
    core::WseTransform transform = core::WseTransform::identity();
    std::string raw_id{};
    std::string normalized_id{};  // prefixed with "spr_" when kind_type == 0 (0x140152e6f)
    std::uint32_t variation_id = 0;
    std::uint32_t variation_id_2 = 0;
    core::WseVec4 scale{1.0f, 1.0f, 1.0f, 1.0f};
};

struct SceneObjectFile {
    std::int32_t version = 0;
    std::vector<LoadedSceneObject> objects{};
    bool has_ai_mesh = false;
    std::vector<std::uint8_t> ai_mesh_bytes{};
    std::int32_t ai_vertex_count = 0;
    std::int32_t ai_edge_count = 0;
    bool has_ground_paint = false;
    std::string ground_paint{};
};

namespace scene_detail {

[[nodiscard]] inline bool read_transform_binary(io::BinaryReader& r,
                                                core::WseTransform& tf) noexcept {
    // Mirrors rglTransform::readStream (0x1402b5d90): sequential s, f, u, o Vec3.
    tf = core::WseTransform::identity();
    return r.read_f32(tf.s.x) && r.read_f32(tf.s.y) && r.read_f32(tf.s.z) &&
           r.read_f32(tf.f.x) && r.read_f32(tf.f.y) && r.read_f32(tf.f.z) &&
           r.read_f32(tf.u.x) && r.read_f32(tf.u.y) && r.read_f32(tf.u.z) &&
           r.read_f32(tf.o.x) && r.read_f32(tf.o.y) && r.read_f32(tf.o.z);
}

[[nodiscard]] inline bool starts_with(std::string_view text,
                                      std::string_view prefix) noexcept {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

inline void extract_ai_mesh_header(std::span<const std::uint8_t> bytes,
                                   SceneObjectFile& out) noexcept {
    // Mirrors the head of rglAiMesh::readStream (0x140252780): int32 num_vertices,
    // num_vertices * Vec3 (12 B), int32 num_edges. The edge and face records that follow are
    // kept only as raw bytes (`ai_mesh_bytes`).
    io::BinaryReader ai_r(std::as_bytes(bytes));
    std::int32_t nv = 0;
    if (!ai_r.read_i32(nv) || nv < 0) {
        return;
    }
    out.ai_vertex_count = nv;
    const std::size_t vbytes = static_cast<std::size_t>(nv) * 12U;
    if (vbytes > ai_r.remaining() || !ai_r.skip(vbytes)) {
        return;
    }
    std::int32_t ne = 0;
    if (ai_r.read_i32(ne) && ne >= 0) {
        out.ai_edge_count = ne;
    }
}

[[nodiscard]] inline bool parse_f32_sv(std::string_view s, float& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size() && std::isfinite(out);
}

[[nodiscard]] inline bool parse_i32_sv(std::string_view s, std::int32_t& out) noexcept {
    if (s.empty()) {
        return false;
    }
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc{} && ptr == s.data() + s.size();
}

}  // namespace scene_detail

[[nodiscard]] inline bool parse_scene_obj_bytes(std::span<const std::uint8_t> bytes,
                                                SceneObjectFile& out,
                                                std::string* error_out = nullptr) {
    out = {};
    io::BinaryReader r(std::as_bytes(bytes));
    std::uint32_t first_word = 0;
    if (!r.read_u32(first_word)) {
        if (error_out) *error_out = "empty .sco stream";
        return false;
    }

    std::uint32_t num_objects = 0;
    if (first_word == kSceneObjMagic) {
        if (!r.read_i32(out.version) || !r.read_u32(num_objects)) {
            if (error_out) *error_out = "truncated .sco versioned header";
            return false;
        }
        if (out.version < 0 || out.version > 16) {
            if (error_out) *error_out = "unsupported .sco version";
            return false;
        }
    } else {
        out.version = 0;
        num_objects = first_word;
    }

    // Minimum record size is 12 (kind words) + 48 (transform) + 4 (empty string length) +
    // 4 (variation id) = 68 bytes.
    if (num_objects > r.remaining() / 68U) {
        if (error_out) *error_out = ".sco object count exceeds stream size";
        return false;
    }
    out.objects.reserve(num_objects);

    for (std::uint32_t i = 0; i < num_objects; ++i) {
        LoadedSceneObject obj{};
        if (!r.read_u32(obj.kind_type) || !r.read_u32(obj.sub_kind) ||
            !r.read_u32(obj.file_var_id) ||
            !scene_detail::read_transform_binary(r, obj.transform) ||
            !r.read_string(obj.raw_id) || !r.read_u32(obj.variation_id)) {
            if (error_out) *error_out = "truncated .sco mission object record";
            return false;
        }
        if (obj.kind_type == 0U && !scene_detail::starts_with(obj.raw_id, "spr_")) {
            obj.normalized_id = "spr_" + obj.raw_id;
        } else {
            obj.normalized_id = obj.raw_id;
        }
        if (out.version >= 2 && !r.read_u32(obj.variation_id_2)) {
            if (error_out) *error_out = "truncated .sco variation_id_2";
            return false;
        }
        if (out.version >= 3) {
            if (!r.read_f32(obj.scale.x) || !r.read_f32(obj.scale.y) ||
                !r.read_f32(obj.scale.z)) {
                if (error_out) *error_out = "truncated .sco object scale";
                return false;
            }
        }
        out.objects.push_back(std::move(obj));
    }

    if (out.version >= 4) {
        std::uint32_t ai_len = 0;
        if (!r.read_u32(ai_len) || ai_len > r.remaining()) {
            if (error_out) *error_out = "truncated .sco AI mesh blob";
            return false;
        }
        out.has_ai_mesh = true;
        out.ai_mesh_bytes.resize(ai_len);
        if (ai_len > 0U &&
            !r.read_bytes(std::as_writable_bytes(std::span<std::uint8_t>(out.ai_mesh_bytes)))) {
            if (error_out) *error_out = "failed reading .sco AI mesh bytes";
            return false;
        }
        scene_detail::extract_ai_mesh_header(out.ai_mesh_bytes, out);
    }

    if (!r.at_end()) {
        std::uint32_t paint_magic = 0;
        if (!r.read_u32(paint_magic) || paint_magic != kSceneGroundPaintMagic) {
            if (error_out) *error_out = "invalid .sco ground paint magic";
            return false;
        }
        if (!r.read_string(out.ground_paint, (16U << 20U))) {
            if (error_out) *error_out = "truncated .sco ground paint payload";
            return false;
        }
        out.has_ground_paint = true;
    }

    return true;
}

[[nodiscard]] inline ModuleLoadError load_scene_obj_file(platform::Vfs& vfs,
                                                         std::string_view path,
                                                         SceneObjectFile& out) {
    out = {};
    std::vector<std::uint8_t> bytes;
    if (!vfs.read_file(path, bytes)) {
        return make_load_error(path, "unable to open scene object file");
    }
    std::string parse_err;
    if (!parse_scene_obj_bytes(bytes, out, &parse_err)) {
        return make_load_error(path, parse_err);
    }
    return {};
}

// `.sco` record kinds (`mbMissionObject` meta type). `try_for_prop_instances` filters on
// `meta type + 1`, matching the module system's `somt_object` (1), `somt_entry` (2),
// `somt_item` (3), `somt_baggage` (4), `somt_flora` (5), and `somt_passage` (6).
inline constexpr std::uint32_t kSceneObjKindProp = 0U;
inline constexpr std::uint32_t kSceneObjKindEntryPoint = 1U;
inline constexpr std::uint32_t kSceneObjKindItem = 2U;
inline constexpr std::uint32_t kSceneObjKindPassage = 5U;

// Replaces the current scene's mission entities with the `.sco` placements and marks them
// non-prunable (`mbSite::readSceneObjFile` 0x140210e75). Every record keeps its kind in
// `meta_mesh_no`; only scene props, passages, and items carry a `scene_prop_no` (prop or item
// kind), so entry points and flora never alias the scene-prop kind of the same number. Entry
// points also fill `world.entry_points`. Returns the number of records dropped because the
// mission-object table was full (`kMaxSpawnedMissionObjects`).
inline std::size_t instantiate_scene_objects(
    const SceneObjectFile& scene_obj,
    WseWorldState& world,
    const std::function<std::int32_t(std::uint32_t kind_type, std::string_view id)>& resolver = {}) {
    // A new scene: clear agents and every reference to the previous scene's objects (attached
    // missiles, edit-mode selection, spawn effectors) before the ids are reused.
    world.clear_mission_entities();
    world.entry_points.fill(WseEntryPoint{});
    std::size_t dropped = 0;
    for (const LoadedSceneObject& src : scene_obj.objects) {
        WseMissionObject dst{};
        dst.active = true;
        dst.meta_mesh_no = static_cast<std::int32_t>(src.kind_type);
        dst.is_item = (src.kind_type == kSceneObjKindItem);
        const bool has_kind = src.kind_type == kSceneObjKindProp ||
                              src.kind_type == kSceneObjKindPassage || dst.is_item;
        if (has_kind) {
            std::int32_t resolved_kind = static_cast<std::int32_t>(src.sub_kind);
            if (resolver) {
                const std::int32_t r_id = resolver(src.kind_type, src.normalized_id);
                if (r_id >= 0) {
                    resolved_kind = r_id;
                }
            }
            dst.scene_prop_no = resolved_kind;
        }
        dst.variation_id_no = static_cast<std::int32_t>(src.variation_id);
        dst.variation_id_2_no = static_cast<std::int32_t>(src.variation_id_2);
        dst.position = src.transform;
        dst.target_position = src.transform;
        dst.starting_position = src.transform;
        dst.scale = src.scale;
        dst.m5.enable_pruning = false;
        if (src.kind_type == kSceneObjKindEntryPoint &&
            src.sub_kind < static_cast<std::uint32_t>(kMaxEntryPoints)) {
            WseEntryPoint& ep = world.entry_points[src.sub_kind];
            ep.valid = true;
            ep.position = src.transform;
        }
        if (world.allocate_mission_object(std::move(dst)) < 0) {
            ++dropped;
        }
    }
    return dropped;
}

// -----------------------------------------------------------------------------
// Skeleton Physics Bodies (`Data/skeleton_bodies.xml`: 0x140211d20 / 0x140211600)
// -----------------------------------------------------------------------------

struct LoadedSkeletonBody {
    std::string type = "capsule";
    float radius = 0.05f;
    core::WseVec4 pos{};
    core::WseVec4 pos2{};
    bool for_ragdoll_only = false;
    bool for_collision_only = false;
};

struct LoadedSkeletonBoneDesc {
    std::string name{};
    float mass = 2.0f;
    float cone_min = -0.01f;
    float cone_max = 1.0f;
    float twist_limit_min = 1.0f;
    float twist_limit_max = -1.0f;
    core::WseVec4 socket_dir{0.0f, 1.0f, 0.0f, 0.0f};
    core::WseVec4 plane_dir{1.0f, 0.0f, 0.0f, 0.0f};
    float plane_min = 0.0f;
    float plane_max = 0.0f;
    float rotate_side_rad = 0.0f;
    float rotate_forward_rad = 0.0f;
    float rotate_up_rad = 0.0f;
    std::vector<LoadedSkeletonBody> bodies{};
};

struct LoadedSkeletonDesc {
    std::string name{};
    std::vector<LoadedSkeletonBoneDesc> bones{};
};

namespace xml_detail {

inline void normalize_dir_or_default(core::WseVec4& v,
                                     float def_x,
                                     float def_y,
                                     float def_z) noexcept {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 0.01f) {
        v = {def_x, def_y, def_z, 0.0f};
        return;
    }
    if (len > 1.0e-10f) {
        const float inv = 1.0f / len;
        v.x *= inv;
        v.y *= inv;
        v.z *= inv;
    }
}

}  // namespace xml_detail

[[nodiscard]] inline bool parse_skeleton_bodies_xml(
    std::string_view xml,
    std::vector<LoadedSkeletonDesc>& out,
    std::string* error_out = nullptr) {
    out.clear();
    io::XmlElementNode root{};
    if (!io::parse_xml_document(xml, root, error_out)) {
        return false;
    }
    const io::XmlElementNode* skeletons_el =
        (root.tag == "Skeletons") ? &root : io::find_xml_child(root, "Skeletons");
    if (skeletons_el == nullptr) {
        if (error_out) *error_out = "missing <Skeletons> element";
        return false;
    }

    constexpr float kDegToRad = 0.017453292519943295f;  // __real@3c8efa35 at 0x140211783
    for (const io::XmlElementNode& skel_el : skeletons_el->children) {
        if (skel_el.tag != "Skeleton") {
            continue;
        }
        LoadedSkeletonDesc skel{};
        io::read_xml_attr_str(skel_el, "name", skel.name);
        const io::XmlElementNode* bones_el = io::find_xml_child(skel_el, "Bones");
        if (bones_el != nullptr) {
            for (const io::XmlElementNode& bone_el : bones_el->children) {
                if (bone_el.tag != "Bone") {
                    continue;
                }
                LoadedSkeletonBoneDesc bone{};
                io::read_xml_attr_str(bone_el, "name", bone.name);
                io::read_xml_attr_f32(bone_el, "bone_mass", bone.mass);
                io::read_xml_attr_f32(bone_el, "bone_cone_min", bone.cone_min);
                io::read_xml_attr_f32(bone_el, "bone_cone_max", bone.cone_max);
                io::read_xml_attr_f32(bone_el, "bone_twist_limit_min", bone.twist_limit_min);
                io::read_xml_attr_f32(bone_el, "bone_twist_limit_max", bone.twist_limit_max);
                // Initialize optional vector components to 0 before attribute read, then normalize.
                core::WseVec4 sock{0.0f, 0.0f, 0.0f, 0.0f};
                io::read_xml_attr_f32(bone_el, "socket_dir_x", sock.x);
                io::read_xml_attr_f32(bone_el, "socket_dir_y", sock.y);
                io::read_xml_attr_f32(bone_el, "socket_dir_z", sock.z);
                xml_detail::normalize_dir_or_default(sock, 0.0f, 1.0f, 0.0f);
                bone.socket_dir = sock;

                core::WseVec4 plane{0.0f, 0.0f, 0.0f, 0.0f};
                io::read_xml_attr_f32(bone_el, "plane_dir_x", plane.x);
                io::read_xml_attr_f32(bone_el, "plane_dir_y", plane.y);
                io::read_xml_attr_f32(bone_el, "plane_dir_z", plane.z);
                xml_detail::normalize_dir_or_default(plane, 1.0f, 0.0f, 0.0f);
                bone.plane_dir = plane;

                io::read_xml_attr_f32(bone_el, "rotate_side", bone.rotate_side_rad, kDegToRad);
                io::read_xml_attr_f32(bone_el, "rotate_forward", bone.rotate_forward_rad, kDegToRad);
                io::read_xml_attr_f32(bone_el, "rotate_up", bone.rotate_up_rad, kDegToRad);
                io::read_xml_attr_f32(bone_el, "bone_plane_min", bone.plane_min);
                io::read_xml_attr_f32(bone_el, "bone_plane_max", bone.plane_max);

                // Retail 0x140211a78: when twist_limit_max == -1.0f, mirror +/- twist_limit_min.
                if (bone.twist_limit_max == -1.0f) {
                    bone.twist_limit_max = bone.twist_limit_min;
                    bone.twist_limit_min = -bone.twist_limit_min;
                }

                const io::XmlElementNode* bodies_el = io::find_xml_child(bone_el, "Bodies");
                if (bodies_el != nullptr) {
                    for (const io::XmlElementNode& body_el : bodies_el->children) {
                        if (body_el.tag != "body") {
                            continue;
                        }
                        LoadedSkeletonBody body{};
                        io::read_xml_attr_str(body_el, "type", body.type);
                        io::read_xml_attr_f32(body_el, "radius", body.radius);
                        io::read_xml_attr_f32(body_el, "pos_x", body.pos.x);
                        io::read_xml_attr_f32(body_el, "pos_y", body.pos.y);
                        io::read_xml_attr_f32(body_el, "pos_z", body.pos.z);
                        io::read_xml_attr_f32(body_el, "pos2_x", body.pos2.x);
                        io::read_xml_attr_f32(body_el, "pos2_y", body.pos2.y);
                        io::read_xml_attr_f32(body_el, "pos2_z", body.pos2.z);
                        io::read_xml_attr_flag_bit(body_el, "for_ragdoll_only", body.for_ragdoll_only);
                        io::read_xml_attr_flag_bit(body_el, "for_collision_only", body.for_collision_only);
                        bone.bodies.push_back(std::move(body));
                    }
                }
                skel.bones.push_back(std::move(bone));
            }
        }
        out.push_back(std::move(skel));
    }
    return true;
}

[[nodiscard]] inline ModuleLoadError load_skeleton_bodies_xml(
    platform::Vfs& vfs,
    std::string_view path,
    std::vector<LoadedSkeletonDesc>& out) {
    out.clear();
    std::string xml;
    ModuleLoadError err{};
    if (!read_vfs_text_checked(vfs, path, xml, err)) {
        return err;
    }
    std::string parse_err;
    if (!parse_skeleton_bodies_xml(xml, out, &parse_err)) {
        return make_load_error(path, parse_err);
    }
    return {};
}

}  // namespace wse::game
