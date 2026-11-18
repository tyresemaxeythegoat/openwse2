// Clean-room v1.1.5.5 .brf binary resource container decoder, MetaMesh LOD
// grouper, and module.ini resource catalog loader.
//
// Reconstructed from v1.1.5.5 PE + PDB evidence:
// - rglResourceManager (0xae1e, 1880 B), g_resourceManager (0x140624a70)
// - rglResourceManager::readStream (0x1402f04a0), registerResource (0x1402efaf0)
// - rglResourceManager::loadCoreResources (0x1402ef480),
//   loadOptionalShaderResource (0x1402ef550), loadResourceFile (0x1402ef6c0)
// - rglResourceManager::createMetaMesh (0x1402f3030)
// - mbCoreGame::readModuleCommands (0x14008cca0)
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wse/game/module_loader_common.h"
#include "wse/io/binary_stream.h"
#include "wse/platform/vfs.h"

namespace wse::asset {

struct Vec2f {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quatf {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

// rglMeshVertexGroupPair (0x1df81, 8 B).
struct BrfVertexWeight {
    std::int32_t vertex_index = 0;
    float weight = 0.0f;
};

// rglMeshVertexGroup (0x1e386, 32 B).
struct BrfVertexGroup {
    std::uint8_t bone_no = 0;
    std::vector<BrfVertexWeight> pairs{};
};

// rglMeshVertexKey (0x19d6a, 48 B), read via rglMeshVertexKey::readStream (0x1402b7f50)
// and rglIpo<rglMeshVertexKey>::readStream (0x1402c4070).
struct BrfVertexKeyFrame {
    std::int32_t time_frame = 0;
    std::vector<Vec3f> positions{};
    std::vector<Vec3f> normals{};
};

// rglMeshFaceCorner (0x75b6, 68 B), read via rglMeshFaceCorner::readStream (0x1402b7c90).
struct BrfFaceCorner {
    std::int32_t vertex_index = 0;
    std::uint32_t color = 0;
    Vec3f normal{};
    Vec3f tangent{};
    bool binormal_sign = false;
    Vec2f uv{};
    Vec2f uv2{};
};

// rglMeshFace (0x75bb, 6 B).
struct BrfMeshFace {
    std::array<std::uint16_t, 3> corners{};
};

// rglMesh (0x1c4a5, 872 B), read via rglMesh::readStream (0x1402b8720).
struct BrfMesh {
    std::string name{};
    std::uint32_t flags = 0;
    std::string material_name{};
    bool has_tangents = false;
    bool single_uv = false;
    std::vector<Vec3f> positions{};  // rglMeshVertex (0x18f59, 24 B)
    std::vector<BrfVertexGroup> vertex_groups{};
    std::vector<BrfVertexKeyFrame> vertex_keys{};
    std::vector<BrfFaceCorner> face_corners{};
    std::vector<BrfMeshFace> faces{};
};

// rglTexture (0x1aadd, 288 B), decoded inline in rglResourceManager::readStream (0x1402f04a0).
struct BrfTexture {
    std::string name{};
    std::uint32_t flags = 0;  // raw_flags & 0xEFFFFFFFU
};

struct BrfShaderTechnique {
    std::int32_t map_op = 0;
    std::uint32_t color_op = 0;
    std::uint32_t alpha_op = 0;
    std::uint32_t flags = 0;
};

// rglShader (0x1fa6f, 504 B), read via rglShader::readStream (0x140302750).
struct BrfShader {
    std::string name{};
    std::uint32_t flags = 0;
    std::uint32_t requires_flags = 0;
    std::string technique_name{};
    std::vector<std::string> alternatives{};
    std::vector<BrfShaderTechnique> techniques{};
};

// rglMaterial (0x1dc61, 632 B), read via rglMaterial::readStream (0x1402b5be0).
struct BrfMaterial {
    std::string name{};
    std::uint32_t flags = 0;
    std::string shader_name{};
    std::array<std::string, 5> texture_names{};
    float specular_coeff = 0.0f;
    Vec3f specular_rgb{};
};

enum class BrfBodyPartKind : std::uint8_t {
    Sphere = 0,    // "sphere" (0x14055edfc), rglSphere (0x3516, 128 B)
    Capsule = 1,   // "capsule" (0x14055ee08), rglCapsule (0x1f07c, 160 B)
    Face = 2,      // "face" (0x14055ee10), rglQuad (0x1d157, 304 B)
    Manifold = 3,  // "manifold" (0x14055ee28), rglManifoldBody (0x1c79f, 160 B)
    Composite = 4  // "composite" (0x14055ee18), rglCompositeBody (0x1d3c4, 104 B)
};

// rglManifoldBodyFace (0x17e42, 40 B).
struct BrfManifoldFace {
    std::int32_t orientation = 0;
    std::uint32_t flags = 0;
    std::vector<std::int32_t> vertex_indices{};
};

// rglBodyPart (0x1c312, 88 B) polymorphic hierarchy.
struct BrfBodyPart {
    BrfBodyPartKind kind = BrfBodyPartKind::Sphere;
    std::uint32_t flags = 0;
    float radius = 0.0f;
    Vec3f a{};
    Vec3f b{};
    std::vector<Vec3f> vertices{};
    std::vector<BrfManifoldFace> manifold_faces{};
    std::vector<BrfBodyPart> children{};
};

// rglBody (0x19f4b, 104 B), read via rglBody::readStream (0x140258320).
struct BrfBody {
    std::string name{};
    BrfBodyPart root{};
};

// rglSkeletonModelBone (0x2081e, 360 B), transform fields rot.s/f/u + origin (12 floats).
struct BrfSkeletonBone {
    std::int32_t parent_index = -1;
    std::string name{};
    std::uint32_t flags = 0;
    Vec3f rot_s{};
    Vec3f rot_f{};
    Vec3f rot_u{};
    Vec3f origin{};
};

// rglSkeletonModel (0x1ef4b, 152 B), read via rglSkeletonModel::readStream (0x14030dff0).
struct BrfSkeleton {
    std::string name{};
    std::vector<BrfSkeletonBone> bones{};
};

struct BrfQuatKey {
    std::int32_t time_frame = 0;
    Quatf rotation{};
};

struct BrfPosKey {
    std::int32_t time_frame = 0;
    Vec3f position{};
};

// rglSkeletonAnimation (0x19f91, 144 B) + rglSkeletonAnimationBone (0x17ac9, 24 B),
// read via rglSkeletonAnimation::readStream (0x14030cef0),
// rglSkeletonAnimationBone::readStream (0x14030ce20),
// rglIpo<rglQuaternion>::readStream (0x14030d0e0), rglIpo<rglVector3>::readStream (0x14030d1e0).
struct BrfSkeletonAnimation {
    std::string name{};
    std::vector<std::vector<BrfQuatKey>> bone_rotations{};
    std::vector<BrfPosKey> root_positions{};
};

// rglPhysicsMaterial (0x184e3, 104 B, vftable 0x1405647f8).
// Note: +0x60 is m_soundOcclusion (default 0.5f), not friction.
struct BrfPhysicsMaterial {
    std::string name{};
    std::uint32_t flags = 0;
    float sound_occlusion = 0.5f;
};

struct BrfFile {
    std::int32_t rfver = 0;
    std::vector<BrfMesh> meshes{};
    std::vector<BrfTexture> textures{};
    std::vector<BrfShader> shaders{};
    std::vector<BrfMaterial> materials{};
    std::vector<BrfBody> bodies{};
    std::vector<BrfSkeleton> skeletons{};
    std::vector<BrfSkeletonAnimation> animations{};
    std::vector<BrfPhysicsMaterial> physics_materials{};
};

// rglMetaMesh (0x18285, 344 B) + rglMetaMeshLod (0xb348, 24 B) grouping of mesh names.
struct BrfMetaMeshGroup {
    std::string base_name{};
    std::array<std::vector<std::size_t>, 5> lod_mesh_indices{};
};

struct BrfCatalogEntry {
    std::string resource_name{};
    std::string vfs_path{};
    bool from_module = false;
    bool optional = false;
    bool loaded = false;
    BrfFile file{};
};

struct BrfCatalog {
    std::vector<BrfCatalogEntry> entries{};
    std::vector<std::string> warnings{};
};

namespace detail {

inline bool set_error(std::string* error_out, std::string msg) {
    if (error_out != nullptr) *error_out = std::move(msg);
    return false;
}

inline bool read_vec2(wse::io::BinaryReader& r, Vec2f& out) noexcept {
    return r.read_f32(out.x) && r.read_f32(out.y);
}

inline bool read_vec3(wse::io::BinaryReader& r, Vec3f& out) noexcept {
    return r.read_f32(out.x) && r.read_f32(out.y) && r.read_f32(out.z);
}

// Upper bound on elements reserved before they are read. Counts are only
// validated against the remaining stream bytes (at the record's minimum wire
// size), and in-memory records are much larger than that minimum (BrfMesh is
// ~200 B against 4 B on the wire), so reserving the full declared count let a
// corrupt header allocate ~50x the file size. Vectors grow normally past this.
inline constexpr std::size_t kMaxReserveElements = 4096U;

inline std::string count_label(std::string_view what, std::string_view owner) {
    std::string label(what);
    if (!owner.empty()) {
        label += " in ";
        label += owner;
    }
    return label;
}

template <typename T>
bool check_count_and_reserve(std::vector<T>& vec,
                             std::uint64_t count,
                             const wse::io::BinaryReader& r,
                             std::size_t min_bytes_per_elem,
                             std::string* error_out,
                             std::string_view what,
                             std::string_view owner = {}) {
    if (min_bytes_per_elem > 0 && count > r.remaining() / min_bytes_per_elem) {
        return set_error(error_out, count_label(what, owner) + ": count " + std::to_string(count) +
                                        " exceeds remaining stream bytes");
    }
    vec.reserve(static_cast<std::size_t>((std::min)(count, static_cast<std::uint64_t>(kMaxReserveElements))));
    return true;
}

// Reads an unsigned 32-bit element count and validates it (see check_count_and_reserve).
template <typename T>
bool read_count_u32(wse::io::BinaryReader& r,
                    std::vector<T>& vec,
                    std::size_t min_bytes_per_elem,
                    std::uint32_t& count_out,
                    std::string* error_out,
                    std::string_view what,
                    std::string_view owner = {}) {
    if (!r.read_u32(count_out)) {
        return set_error(error_out, "truncated " + count_label(what, owner) + " count");
    }
    return check_count_and_reserve(vec, count_out, r, min_bytes_per_elem, error_out, what, owner);
}

// Reads a signed 32-bit element count, rejecting negatives.
template <typename T>
bool read_count_i32(wse::io::BinaryReader& r,
                    std::vector<T>& vec,
                    std::size_t min_bytes_per_elem,
                    std::int32_t& count_out,
                    std::string* error_out,
                    std::string_view what,
                    std::string_view owner = {}) {
    if (!r.read_i32(count_out)) {
        return set_error(error_out, "truncated " + count_label(what, owner) + " count");
    }
    if (count_out < 0) {
        return set_error(error_out, count_label(what, owner) + ": negative count " +
                                        std::to_string(count_out));
    }
    return check_count_and_reserve(vec, static_cast<std::uint64_t>(count_out), r,
                                   min_bytes_per_elem, error_out, what, owner);
}

inline bool read_mesh(wse::io::BinaryReader& r,
                      std::int32_t rfver,
                      BrfMesh& out,
                      std::string* error_out) {
    if (!r.read_string(out.name) || !r.read_u32(out.flags) || !r.read_string(out.material_name)) {
        return set_error(error_out, "truncated mesh header");
    }

    std::uint32_t num_positions = 0;
    if (!read_count_u32(r, out.positions, 12, num_positions, error_out, "mesh positions", out.name)) {
        return false;
    }
    for (std::uint32_t i = 0; i < num_positions; ++i) {
        Vec3f p{};
        if (!read_vec3(r, p)) return set_error(error_out, "truncated mesh position in " + out.name);
        out.positions.push_back(p);
    }

    std::uint32_t num_groups = 0;
    if (!read_count_u32(r, out.vertex_groups, 8, num_groups, error_out, "mesh vertex groups", out.name)) {
        return false;
    }
    for (std::uint32_t g = 0; g < num_groups; ++g) {
        std::int32_t bone_no = 0;
        std::uint32_t num_pairs = 0;
        if (!r.read_i32(bone_no) || !r.read_u32(num_pairs)) {
            return set_error(error_out, "truncated vertex group header in " + out.name);
        }
        // rglMesh::readStream (0x1402b8720) stores byte m_boneNo and asserts bone_no + 1 <= 64
        // ("Too many bones in mesh %s" at 0x140566660).
        if (bone_no < 0 || bone_no >= 64) {
            return set_error(error_out, "bone index out of range [0, 64) in mesh " + out.name);
        }
        BrfVertexGroup grp{};
        grp.bone_no = static_cast<std::uint8_t>(bone_no);
        if (!check_count_and_reserve(grp.pairs, num_pairs, r, 8, error_out, "vertex group pairs", out.name)) {
            return false;
        }
        for (std::uint32_t p = 0; p < num_pairs; ++p) {
            BrfVertexWeight w{};
            if (!r.read_i32(w.vertex_index) || !r.read_f32(w.weight)) {
                return set_error(error_out, "truncated vertex group pair in " + out.name);
            }
            grp.pairs.push_back(w);
        }
        out.vertex_groups.push_back(std::move(grp));
    }

    // rglIpo<rglMeshVertexKey>::readStream (0x1402c4070) -> rglMeshVertexKey::readStream (0x1402b7f50)
    std::int32_t num_keys = 0;
    if (!read_count_i32(r, out.vertex_keys, 12, num_keys, error_out, "vertex keys", out.name)) {
        return false;
    }
    for (std::int32_t k = 0; k < num_keys; ++k) {
        BrfVertexKeyFrame kf{};
        std::uint32_t npos = 0;
        if (!r.read_i32(kf.time_frame)) {
            return set_error(error_out, "truncated vertex keyframe time in " + out.name);
        }
        if (!read_count_u32(r, kf.positions, 12, npos, error_out, "keyframe positions", out.name)) {
            return false;
        }
        for (std::uint32_t i = 0; i < npos; ++i) {
            Vec3f p{};
            if (!read_vec3(r, p)) return set_error(error_out, "truncated keyframe position in " + out.name);
            kf.positions.push_back(p);
        }
        std::uint32_t nnorm = 0;
        if (!read_count_u32(r, kf.normals, 12, nnorm, error_out, "keyframe normals", out.name)) {
            return false;
        }
        for (std::uint32_t i = 0; i < nnorm; ++i) {
            Vec3f n{};
            if (!read_vec3(r, n)) return set_error(error_out, "truncated keyframe normal in " + out.name);
            kf.normals.push_back(n);
        }
        out.vertex_keys.push_back(std::move(kf));
    }

    out.has_tangents = (rfver == 1) && ((out.flags & 0x10000U) != 0U);
    out.single_uv = (rfver == 1) && ((out.flags & 0x20000U) != 0U);
    const std::size_t corner_min_bytes = 4U + 4U + 12U + (out.has_tangents ? 13U : 0U) + 8U + (out.single_uv ? 0U : 8U);

    std::uint32_t num_corners = 0;
    if (!read_count_u32(r, out.face_corners, corner_min_bytes, num_corners, error_out, "face corners", out.name)) {
        return false;
    }
    for (std::uint32_t c = 0; c < num_corners; ++c) {
        BrfFaceCorner corner{};
        if (!r.read_i32(corner.vertex_index) || !r.read_u32(corner.color) || !read_vec3(r, corner.normal)) {
            return set_error(error_out, "truncated face corner in " + out.name);
        }
        // "Invalid vertex index %d in mesh %s" (0x140566638) checked in rglMesh::readStream (0x1402b8720).
        // Safety deviation: rglMeshFaceCorner::m_vertexNo (+0x40, PDB 0x75b6) is an unsigned short,
        // so an index above 65535 cannot be represented and is rejected instead of truncated.
        if (corner.vertex_index < 0 || static_cast<std::size_t>(corner.vertex_index) >= out.positions.size() ||
            corner.vertex_index > 65535) {
            return set_error(error_out, "Invalid vertex index " + std::to_string(corner.vertex_index) +
                                            " in mesh " + out.name);
        }
        if (out.has_tangents) {
            if (!read_vec3(r, corner.tangent) || !r.read_bool(corner.binormal_sign)) {
                return set_error(error_out, "truncated face corner tangent in " + out.name);
            }
        }
        if (!read_vec2(r, corner.uv)) {
            return set_error(error_out, "truncated face corner uv in " + out.name);
        }
        if (!out.single_uv) {
            if (!read_vec2(r, corner.uv2)) {
                return set_error(error_out, "truncated face corner uv2 in " + out.name);
            }
        } else {
            corner.uv2 = corner.uv;
        }
        out.face_corners.push_back(corner);
    }

    std::uint32_t num_faces = 0;
    if (!read_count_u32(r, out.faces, 12, num_faces, error_out, "mesh faces", out.name)) {
        return false;
    }
    for (std::uint32_t f = 0; f < num_faces; ++f) {
        BrfMeshFace face{};
        for (int i = 0; i < 3; ++i) {
            std::int32_t idx = 0;
            if (!r.read_i32(idx)) return set_error(error_out, "truncated face index in " + out.name);
            if (idx < 0 || static_cast<std::size_t>(idx) >= out.face_corners.size() || idx > 65535) {
                return set_error(error_out, "invalid face corner index in mesh " + out.name);
            }
            face.corners[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(idx);
        }
        out.faces.push_back(face);
    }
    return true;
}

inline bool read_texture(wse::io::BinaryReader& r, BrfTexture& out, std::string* error_out) {
    std::uint32_t raw_flags = 0;
    if (!r.read_string(out.name) || !r.read_u32(raw_flags)) {
        return set_error(error_out, "truncated texture record");
    }
    out.flags = raw_flags & 0xEFFFFFFFU;
    return true;
}

inline bool read_shader(wse::io::BinaryReader& r, BrfShader& out, std::string* error_out) {
    std::int32_t num_alternatives = 0;
    if (!r.read_string(out.name) || !r.read_u32(out.flags) || !r.read_u32(out.requires_flags) ||
        !r.read_string(out.technique_name) || !r.read_i32(num_alternatives)) {
        return set_error(error_out, "truncated shader header");
    }
    // rglShader::readStream (0x140302750): "Number of alternatives exceeds %d for shader %s" (0x14056a1f8).
    if (num_alternatives < 0 || num_alternatives > 3) {
        return set_error(error_out, "Number of alternatives exceeds 3 for shader " + out.name);
    }
    out.alternatives.reserve(static_cast<std::size_t>(num_alternatives));
    for (std::int32_t i = 0; i < num_alternatives; ++i) {
        std::string alt;
        if (!r.read_string(alt)) return set_error(error_out, "truncated shader alternative in " + out.name);
        out.alternatives.push_back(std::move(alt));
    }
    std::int32_t num_techniques = 0;
    if (!read_count_i32(r, out.techniques, 16, num_techniques, error_out, "shader techniques", out.name)) {
        return false;
    }
    for (std::int32_t t = 0; t < num_techniques; ++t) {
        BrfShaderTechnique tech{};
        if (!r.read_i32(tech.map_op) || !r.read_u32(tech.color_op) ||
            !r.read_u32(tech.alpha_op) || !r.read_u32(tech.flags)) {
            return set_error(error_out, "truncated shader technique in " + out.name);
        }
        if (t == 0) {
            if ((tech.color_op & 0x1FU) == 0x18U) {
                out.flags |= 0x40000U;
            } else {
                out.flags &= ~0x40000U;
            }
        }
        out.techniques.push_back(tech);
    }
    if ((out.flags & 0x80002000U) == 0x80002000U) {
        out.flags &= ~0x2000U;
    }
    return true;
}

inline bool read_material(wse::io::BinaryReader& r, BrfMaterial& out, std::string* error_out) {
    if (!r.read_string(out.name) || !r.read_u32(out.flags) || !r.read_string(out.shader_name)) {
        return set_error(error_out, "truncated material header");
    }
    for (auto& tex : out.texture_names) {
        if (!r.read_string(tex)) return set_error(error_out, "truncated material texture slot in " + out.name);
    }
    if (!r.read_f32(out.specular_coeff) || !read_vec3(r, out.specular_rgb)) {
        return set_error(error_out, "truncated material specular data in " + out.name);
    }
    return true;
}

inline bool read_body_part(wse::io::BinaryReader& r,
                           BrfBodyPart& out,
                           int depth,
                           std::string* error_out) {
    if (depth > 8) {
        return set_error(error_out, "composite body nesting exceeds depth limit");
    }
    std::string part_type;
    if (!r.read_string(part_type)) return set_error(error_out, "truncated body part type");

    if (part_type == "sphere") {
        out.kind = BrfBodyPartKind::Sphere;
        if (!r.read_f32(out.radius) || !read_vec3(r, out.a) || !r.read_u32(out.flags)) {
            return set_error(error_out, "truncated sphere body part");
        }
        return true;
    }
    if (part_type == "capsule") {
        out.kind = BrfBodyPartKind::Capsule;
        if (!r.read_f32(out.radius) || !read_vec3(r, out.a) || !read_vec3(r, out.b) || !r.read_u32(out.flags)) {
            return set_error(error_out, "truncated capsule body part");
        }
        return true;
    }
    if (part_type == "face") {
        // rglQuad::readStream (0x1402d52e0): int32 num_verts * Vec3f, then uint32 flags.
        out.kind = BrfBodyPartKind::Face;
        std::int32_t num_verts = 0;
        if (!read_count_i32(r, out.vertices, 12, num_verts, error_out, "face body vertices")) {
            return false;
        }
        for (std::int32_t i = 0; i < num_verts; ++i) {
            Vec3f v{};
            if (!read_vec3(r, v)) return set_error(error_out, "truncated face body vertex");
            out.vertices.push_back(v);
        }
        if (!r.read_u32(out.flags)) return set_error(error_out, "truncated face body flags");
        return true;
    }
    if (part_type == "manifold") {
        // rglManifoldBody::readStream (0x1402b4980)
        out.kind = BrfBodyPartKind::Manifold;
        std::int32_t num_positions = 0;
        if (!read_count_i32(r, out.vertices, 12, num_positions, error_out, "manifold positions")) {
            return false;
        }
        for (std::int32_t i = 0; i < num_positions; ++i) {
            Vec3f v{};
            if (!read_vec3(r, v)) return set_error(error_out, "truncated manifold position");
            out.vertices.push_back(v);
        }
        std::int32_t num_faces = 0;
        if (!read_count_i32(r, out.manifold_faces, 12, num_faces, error_out, "manifold faces")) {
            return false;
        }
        for (std::int32_t f = 0; f < num_faces; ++f) {
            BrfManifoldFace face{};
            std::int32_t num_verts = 0;
            if (!r.read_i32(face.orientation) || !r.read_u32(face.flags)) {
                return set_error(error_out, "truncated manifold face header");
            }
            if (!read_count_i32(r, face.vertex_indices, 4, num_verts, error_out, "manifold face vertices")) {
                return false;
            }
            for (std::int32_t v = 0; v < num_verts; ++v) {
                std::int32_t vi = 0;
                if (!r.read_i32(vi)) return set_error(error_out, "truncated manifold face vertex index");
                face.vertex_indices.push_back(vi);
            }
            out.manifold_faces.push_back(std::move(face));
        }
        return true;
    }
    if (part_type == "composite") {
        // rglCompositeBody::readStream (0x140263db0)
        out.kind = BrfBodyPartKind::Composite;
        std::int32_t num_parts = 0;
        if (!read_count_i32(r, out.children, 4, num_parts, error_out, "composite children")) {
            return false;
        }
        for (std::int32_t p = 0; p < num_parts; ++p) {
            BrfBodyPart child{};
            if (!read_body_part(r, child, depth + 1, error_out)) return false;
            out.children.push_back(std::move(child));
        }
        return true;
    }
    return set_error(error_out, "unknown body part type: " + part_type);
}

inline bool read_body(wse::io::BinaryReader& r, BrfBody& out, std::string* error_out) {
    if (!r.read_string(out.name)) return set_error(error_out, "truncated body name");
    return read_body_part(r, out.root, 0, error_out);
}

inline bool read_skeleton(wse::io::BinaryReader& r, BrfSkeleton& out, std::string* error_out) {
    std::int32_t num_bones = 0;
    if (!r.read_string(out.name)) {
        return set_error(error_out, "truncated skeleton name");
    }
    if (!read_count_i32(r, out.bones, 60, num_bones, error_out, "skeleton bones", out.name)) {
        return false;
    }
    for (std::int32_t i = 0; i < num_bones; ++i) {
        BrfSkeletonBone bone{};
        if (!r.read_i32(bone.parent_index) || !r.read_string(bone.name) || !r.read_u32(bone.flags) ||
            !read_vec3(r, bone.rot_s) || !read_vec3(r, bone.rot_f) ||
            !read_vec3(r, bone.rot_u) || !read_vec3(r, bone.origin)) {
            return set_error(error_out, "truncated skeleton bone in " + out.name);
        }
        out.bones.push_back(std::move(bone));
    }
    return true;
}

inline bool read_skeleton_anim(wse::io::BinaryReader& r, BrfSkeletonAnimation& out, std::string* error_out) {
    std::uint32_t num_bones = 0;
    if (!r.read_string(out.name)) {
        return set_error(error_out, "truncated skeleton animation name");
    }
    if (!read_count_u32(r, out.bone_rotations, 4, num_bones, error_out, "animation bones", out.name)) {
        return false;
    }
    for (std::uint32_t b = 0; b < num_bones; ++b) {
        std::int32_t num_keys = 0;
        std::vector<BrfQuatKey> keys;
        if (!read_count_i32(r, keys, 20, num_keys, error_out, "rotation keys", out.name)) {
            return false;
        }
        // rglIpo<rglQuaternion>::readStream (0x14030d0e0) reads stream order:
        // qx (+0x4), qy (+0x8), qz (+0xc), qw (+0x0 in rglQuaternion{w,x,y,z}; 0x1402b6840).
        for (std::int32_t k = 0; k < num_keys; ++k) {
            BrfQuatKey key{};
            if (!r.read_i32(key.time_frame) ||
                !r.read_f32(key.rotation.x) || !r.read_f32(key.rotation.y) ||
                !r.read_f32(key.rotation.z) || !r.read_f32(key.rotation.w)) {
                return set_error(error_out, "truncated quaternion key in " + out.name);
            }
            keys.push_back(key);
        }
        out.bone_rotations.push_back(std::move(keys));
    }

    std::int32_t num_pos_keys = 0;
    if (!read_count_i32(r, out.root_positions, 16, num_pos_keys, error_out, "position keys", out.name)) {
        return false;
    }
    for (std::int32_t k = 0; k < num_pos_keys; ++k) {
        BrfPosKey pk{};
        if (!r.read_i32(pk.time_frame) || !read_vec3(r, pk.position)) {
            return set_error(error_out, "truncated root position key in " + out.name);
        }
        out.root_positions.push_back(pk);
    }
    return true;
}

inline bool read_physics_material(wse::io::BinaryReader& r, BrfPhysicsMaterial& out, std::string* error_out) {
    if (!r.read_string(out.name) || !r.read_u32(out.flags) || !r.read_f32(out.sound_occlusion)) {
        return set_error(error_out, "truncated physics_material record");
    }
    return true;
}

template <typename Elem, typename ReaderFn>
bool read_section(wse::io::BinaryReader& r,
                  std::vector<Elem>& target,
                  ReaderFn&& elem_reader,
                  std::string* error_out,
                  const char* kind_name) {
    std::int32_t count = 0;
    if (!read_count_i32(r, target, 4, count, error_out, std::string(kind_name) + " section")) {
        return false;
    }
    for (std::int32_t i = 0; i < count; ++i) {
        Elem elem{};
        if (!elem_reader(elem)) return false;
        target.push_back(std::move(elem));
    }
    return true;
}

inline int parse_lod_suffix(std::string_view suffix) noexcept {
    // Matches rglResourceManager::createMetaMesh (0x1402f3030) suffix rule:
    // 0x1402f327d: suffix[0] == '.'; 0x1402f328a: suffix.size() <= 4 stays LOD 0;
    // 0x1402f328e..0x1402f32d9: suffix[1..3] == "lod" (ASCII CI) and suffix[4] in '1'..'4'
    // (without requiring end-of-string after suffix[4]) gives LOD 1..4.
    if (suffix.size() >= 5 && suffix[0] == '.') {
        using wse::platform::ascii_lower;
        if (ascii_lower(suffix[1]) == 'l' &&
            ascii_lower(suffix[2]) == 'o' &&
            ascii_lower(suffix[3]) == 'd' &&
            suffix[4] >= '1' && suffix[4] <= '4') {
            return suffix[4] - '0';
        }
    }
    return 0;
}

}  // namespace detail

// Parses a .brf resource stream following rglResourceManager::readStream (0x1402f04a0).
inline bool parse_brf(std::span<const std::byte> data,
                      BrfFile& out,
                      std::string* error_out = nullptr) {
    out = BrfFile{};
    wse::io::BinaryReader r(data);
    std::string token;
    if (!r.read_string(token)) {
        return detail::set_error(error_out, "empty or truncated brf stream");
    }

    // Optional version header ("rfver ", 0x1405691ec).
    if (token == "rfver ") {
        if (!r.read_i32(out.rfver)) {
            return detail::set_error(error_out, "truncated rfver value");
        }
        if (!r.read_string(token)) {
            return detail::set_error(error_out, "truncated section header after rfver");
        }
    }

    while (token != "end") {  // "end" (0x1405691f4)
        bool ok = false;
        if (token == "mesh") {  // "mesh" (0x1405691f8)
            ok = detail::read_section(r, out.meshes,
                [&](BrfMesh& m) { return detail::read_mesh(r, out.rfver, m, error_out); },
                error_out, "mesh");
        } else if (token == "texture") {  // "texture" (0x140569200)
            ok = detail::read_section(r, out.textures,
                [&](BrfTexture& t) { return detail::read_texture(r, t, error_out); },
                error_out, "texture");
        } else if (token == "shader") {  // "shader" (0x140569208)
            ok = detail::read_section(r, out.shaders,
                [&](BrfShader& s) { return detail::read_shader(r, s, error_out); },
                error_out, "shader");
        } else if (token == "material") {  // "material" (0x140569210)
            ok = detail::read_section(r, out.materials,
                [&](BrfMaterial& m) { return detail::read_material(r, m, error_out); },
                error_out, "material");
        } else if (token == "body") {  // "body" (0x14055d9b4)
            ok = detail::read_section(r, out.bodies,
                [&](BrfBody& b) { return detail::read_body(r, b, error_out); },
                error_out, "body");
        } else if (token == "skeleton") {  // "skeleton" (0x140569220)
            ok = detail::read_section(r, out.skeletons,
                [&](BrfSkeleton& s) { return detail::read_skeleton(r, s, error_out); },
                error_out, "skeleton");
        } else if (token == "skeleton_anim") {  // "skeleton_anim" (0x140569230)
            ok = detail::read_section(r, out.animations,
                [&](BrfSkeletonAnimation& a) { return detail::read_skeleton_anim(r, a, error_out); },
                error_out, "skeleton_anim");
        } else if (token == "physics_material") {  // "physics_material" (0x140569240)
            ok = detail::read_section(r, out.physics_materials,
                [&](BrfPhysicsMaterial& p) { return detail::read_physics_material(r, p, error_out); },
                error_out, "physics_material");
        } else {
            // "Unrecognized resource type %s" (0x140569258)
            return detail::set_error(error_out, "Unrecognized resource type " + token);
        }
        if (!ok) return false;
        if (!r.read_string(token)) {
            return detail::set_error(error_out, "missing 'end' section marker");
        }
    }
    return true;
}

inline bool load_brf(wse::platform::Vfs& vfs,
                     std::string_view vfs_path,
                     BrfFile& out,
                     std::string* error_out = nullptr) {
    std::vector<std::uint8_t> raw;
    if (!vfs.read_file(vfs_path, raw)) {
        return detail::set_error(error_out, "unable to read brf file: " + std::string(vfs_path));
    }
    const auto bytes = std::as_bytes(std::span<const std::uint8_t>(raw));
    return parse_brf(bytes, out, error_out);
}

// Groups mesh records into MetaMesh LOD buckets [0..4] matching
// rglResourceManager::createMetaMesh (0x1402f3030..0x1402f3360):
// - 0x1402f31b3: prefix length must be >= 2 (shorter prefixes before '.' are not split).
// - 0x1402f3272..0x1402f327d: exact prefix match -> LOD 0; longer names must have '.' at
//   prefix length (first '.').
// - 0x1402f328a..0x1402f32d9: dotted suffixes with size <= 4 (such as "tree.1") or not
//   beginning with case-insensitive ".lod[1-4]" (so ".lod1x" is LOD 1) land in LOD 0.
inline std::vector<BrfMetaMeshGroup> group_meta_meshes(std::span<const BrfMesh> meshes) {
    std::vector<BrfMetaMeshGroup> groups;
    std::unordered_map<std::string, std::size_t> index_by_base;
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        const std::string& full = meshes[i].name;
        const std::size_t dot = full.find('.');
        const bool split = (dot != std::string::npos && dot >= 2U);
        const std::string_view base_sv = split
            ? std::string_view(full).substr(0, dot)
            : std::string_view(full);
        const std::string_view suffix_sv = split
            ? std::string_view(full).substr(dot)
            : std::string_view();
        const int lod = detail::parse_lod_suffix(suffix_sv);

        const std::string key(base_sv);
        auto [it, inserted] = index_by_base.emplace(key, groups.size());
        if (inserted) {
            BrfMetaMeshGroup grp{};
            grp.base_name = key;
            groups.push_back(std::move(grp));
        }
        groups[it->second].lod_mesh_indices[static_cast<std::size_t>(lod)].push_back(i);
    }
    return groups;
}

// Resolves and parses core + module .brf resources for a Warband installation:
// - loadCoreResources (0x1402ef480): "core_shaders,core_textures,core_materials,core_ui_meshes,core_pictures,core_physics_materials" (0x14055f8f0)
// - loadOptionalShaderResource (0x1402ef550): "wse2_shaders" (0x140569028)
// - mbCoreGame::readModuleCommands (0x14008cca0): load_resource (0x14054d3d0),
//   load_resource_nofast (0x14054d3e0), load_mod_resource (0x14054d410),
//   load_module_resource (0x14054d428).
inline bool load_module_brf_catalog(wse::platform::Vfs& vfs,
                                    std::string_view warband_root,
                                    std::string_view module_root,
                                    const wse::game::ModuleIniData& ini,
                                    BrfCatalog& out,
                                    std::string* error_out = nullptr) {
    out = BrfCatalog{};

    const auto append_entry = [&](std::string_view res_name, bool from_module, bool optional) -> bool {
        std::string name(res_name);
        std::string file_name = name;
        const std::string lower = wse::platform::ascii_lower(file_name);
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".brf") != 0) {
            file_name += ".brf";
        }
        const std::string dir = from_module
            ? wse::game::vfs_join_path(module_root, "Resource")
            : wse::game::vfs_join_path(warband_root, "CommonRes");
        const std::string path = wse::game::vfs_join_path(dir, file_name);

        BrfCatalogEntry entry{};
        entry.resource_name = std::move(name);
        entry.vfs_path = path;
        entry.from_module = from_module;
        entry.optional = optional;

        if (!vfs.is_file(path)) {
            if (!optional) {
                out.warnings.push_back("missing resource brf: " + path);
            }
            out.entries.push_back(std::move(entry));
            return true;
        }
        std::string parse_err;
        if (!load_brf(vfs, path, entry.file, &parse_err)) {
            return detail::set_error(error_out, path + ": " + parse_err);
        }
        entry.loaded = true;
        out.entries.push_back(std::move(entry));
        return true;
    };

    static constexpr std::array<std::string_view, 6> kCoreBrfs = {
        "core_shaders",
        "core_textures",
        "core_materials",
        "core_ui_meshes",
        "core_pictures",
        "core_physics_materials",
    };
    for (std::string_view core : kCoreBrfs) {
        if (!append_entry(core, false, false)) return false;
    }
    if (!append_entry("wse2_shaders", false, true)) return false;

    for (const std::string& res : ini.load_resources) {
        if (!append_entry(res, false, false)) return false;
    }
    for (const std::string& res : ini.load_mod_resources) {
        if (!append_entry(res, true, false)) return false;
    }
    return true;
}

}  // namespace wse::asset
