// Host & selftest suite for Phase 2A scene/terrain/skeleton loaders and
// Slice P9 Part 1 skeletal evaluation, skinning, and multi-object RHI rendering.
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/asset/skeleton.h"
#include "wse/core/math.h"
#include "wse/game/scene_loaders.h"
#include "wse/game/world_state.h"
#include "wse/io/binary_stream.h"
#include "wse/io/text_stream.h"
#include "wse/platform/rhi.h"
#include "wse/platform/rhi_scene.h"
#include "wse/platform/vfs.h"

namespace wse::selftest {

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL  %s\n", what);
    }
}

bool close(float a, float b, float eps = 1.0e-3f) {
    return std::fabs(a - b) <= eps;
}

std::filesystem::path find_warband_root_if_present() {
    if (const char* env = std::getenv("WSE_WARBAND_ROOT"); env != nullptr && env[0] != '\0') {
        std::filesystem::path p(env);
        if (std::filesystem::exists(p / "Modules" / "Native" / "map.txt")) {
            return p;
        }
    }
    const std::filesystem::path gog(R"(C:\GOG Games\Mount and Blade - Warband)");
    if (std::filesystem::exists(gog / "Modules" / "Native" / "map.txt")) {
        return gog;
    }
    return {};
}

// Builds a double-sided CCW unit quad in Z-up around (0,0,z) or small triangle prism
// to keep synthetic BrfMesh fixtures compact and clean.
wse::asset::BrfMesh make_rigged_bar_mesh() {
    wse::asset::BrfMesh mesh;
    mesh.name = "rigged_bar";
    mesh.single_uv = true;
    // Two quads: lower [z=0..1] bound 100% to bone 0; upper [z=1..2] split or bound to bone 1.
    // Vertices 0..3 at bottom square (z=0), 4..7 at top square (z=2), front face (y = -0.25).
    mesh.positions = {
        {-0.25f, -0.25f, 0.0f}, {0.25f, -0.25f, 0.0f},
        {0.25f, -0.25f, 1.0f},  {-0.25f, -0.25f, 1.0f},
        {0.25f, -0.25f, 2.0f},  {-0.25f, -0.25f, 2.0f},
    };
    for (std::int32_t i = 0; i < 6; ++i) {
        wse::asset::BrfFaceCorner c{};
        c.vertex_index = i;
        c.color = 0xFFE0A060U;
        c.normal = {0.0f, -1.0f, 0.0f};
        c.uv = {static_cast<float>(i % 2), static_cast<float>(i / 2) * 0.5f};
        mesh.face_corners.push_back(c);
    }
    mesh.faces = {
        {{0, 1, 2}}, {{0, 2, 3}},
        {{3, 2, 4}}, {{3, 4, 5}},
    };
    wse::asset::BrfVertexGroup g0{};
    g0.bone_no = 0U;
    g0.pairs = {{0, 1.0f}, {1, 1.0f}, {2, 0.5f}, {3, 0.5f}};
    wse::asset::BrfVertexGroup g1{};
    g1.bone_no = 1U;
    g1.pairs = {{2, 0.5f}, {3, 0.5f}, {4, 1.0f}, {5, 1.0f}};
    mesh.vertex_groups = {std::move(g0), std::move(g1)};
    return mesh;
}

wse::asset::BrfMesh make_unit_pyramid_prop() {
    wse::asset::BrfMesh mesh;
    mesh.name = "prop_pyr";
    mesh.single_uv = true;
    mesh.positions = {
        {-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.0f, 0.5f, 0.0f}, {0.0f, 0.0f, 1.0f}
    };
    auto add_tri = [&](std::int32_t a, std::int32_t b, std::int32_t c, std::uint32_t col) {
        const auto& p0 = mesh.positions[static_cast<std::size_t>(a)];
        const auto& p1 = mesh.positions[static_cast<std::size_t>(b)];
        const auto& p2 = mesh.positions[static_cast<std::size_t>(c)];
        const float ex = p1.x - p0.x, ey = p1.y - p0.y, ez = p1.z - p0.z;
        const float fx = p2.x - p0.x, fy = p2.y - p0.y, fz = p2.z - p0.z;
        float nx = ey * fz - ez * fy;
        float ny = ez * fx - ex * fz;
        float nz = ex * fy - ey * fx;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 1.0e-6f) {
            nx /= len; ny /= len; nz /= len;
        }
        const std::uint16_t base = static_cast<std::uint16_t>(mesh.face_corners.size());
        for (const std::int32_t vi : {a, b, c}) {
            wse::asset::BrfFaceCorner fc{};
            fc.vertex_index = vi;
            fc.color = col;
            fc.normal = {nx, ny, nz};
            mesh.face_corners.push_back(fc);
        }
        mesh.faces.push_back({{base, static_cast<std::uint16_t>(base + 1U),
                              static_cast<std::uint16_t>(base + 2U)}});
    };
    add_tri(0, 1, 3, 0xFFCC4444U);
    add_tri(1, 2, 3, 0xFF44CC44U);
    add_tri(2, 0, 3, 0xFF4488EEU);
    add_tri(2, 1, 0, 0xFF888888U);
    return mesh;
}

void test_map_manifold_and_height_query() {
    // Sloped square z = x + 2*y over [0, 2] x [0, 2], with packed material (extra=3, mat=4 -> 0x0304 = 772)
    const char* map_text =
        "4\n"
        "0.0 0.0 0.0\n"
        "2.0 0.0 2.0\n"
        "2.0 2.0 6.0\n"
        "0.0 2.0 4.0\n"
        "1\n"
        "772 0 4 0 1 2 3\n";
    wse::game::WorldMapManifold map;
    wse::io::TokenReader reader(map_text);
    const wse::game::ModuleLoadError err = wse::game::read_manifold_map(reader, map, "map.txt");
    expect(err.ok && map.vertices.size() == 4U && map.faces.size() == 1U,
           "map.txt manifold parses vertices and quad face");
    expect(map.faces[0].material_no == 4U && map.faces[0].extra == 3U,
           "mbGame::readMap unpacks packed material bits (>8 extra, low 8 material_no)");
    expect(close(map.min_bounds.z, 0.0f) && close(map.max_bounds.z, 6.0f),
           "rglManifold::computeBounds computes tight min/max AABB");

    float z = 0.0f;
    std::uint16_t mat = 0;
    expect(map.sample_surface_at(1.0f, 1.5f, z, &mat) && close(z, 4.0f) && mat == 4U,
           "barycentric manifold sample_surface_at interpolates sloped elevation and material");
    expect(!map.sample_surface_at(-0.5f, 1.0f, z, &mat),
           "sample_surface_at returns false outside manifold");

    // A declared count far larger than the file is rejected before reserving storage.
    wse::io::TokenReader huge_reader("1000000\n0 0 0\n");
    wse::game::WorldMapManifold huge_map;
    expect(!wse::game::read_manifold_map(huge_reader, huge_map, "huge_map.txt").ok &&
               huge_map.vertices.capacity() == 0U,
           "map.txt rejects a vertex count the file cannot hold");

    // Out-of-range face vertex index fails loudly.
    wse::io::TokenReader bad_reader("2\n0 0 0\n1 0 0\n1\n0 0 3 0 1 99\n");
    wse::game::WorldMapManifold bad_map;
    expect(!wse::game::read_manifold_map(bad_reader, bad_map, "bad_map.txt").ok,
           "map.txt rejects out-of-range face vertex index");
}

void test_sco_scene_object_versions_and_ai_ground() {
    auto write_tf = [](wse::io::BinaryWriter& w, const wse::core::WseTransform& tf) {
        for (const auto& v : {tf.s, tf.f, tf.u, tf.o}) {
            w.write_f32(v.x);
            w.write_f32(v.y);
            w.write_f32(v.z);
        }
    };

    // v4 .sco with 3 objects (prop without spr_ prefix, item, prop with spr_ prefix),
    // 12-byte AI mesh header (1 vertex, 0 edges), and 0xff4ad1a6 ground paint payload.
    wse::io::BinaryWriter w;
    w.write_u32(wse::game::kSceneObjMagic);
    w.write_i32(4);  // version
    w.write_u32(3U); // num_objects

    wse::core::WseTransform tf0 = wse::core::WseTransform::identity();
    tf0.o = {10.0f, 20.0f, 1.5f, 1.0f};
    w.write_u32(0U);  // kind 0 -> scene prop
    w.write_u32(7U);  // sub_kind
    w.write_u32(0U);
    write_tf(w, tf0);
    w.write_string("village_house");
    w.write_u32(2U);
    w.write_u32(3U);
    w.write_f32(1.25f);
    w.write_f32(1.25f);
    w.write_f32(2.0f);

    w.write_u32(2U);  // kind 2 -> item
    w.write_u32(12U);
    w.write_u32(0U);
    write_tf(w, wse::core::WseTransform::identity());
    w.write_string("itm_sword");
    w.write_u32(0U);
    w.write_u32(0U);
    w.write_f32(1.0f);
    w.write_f32(1.0f);
    w.write_f32(1.0f);

    w.write_u32(0U);
    w.write_u32(9U);
    w.write_u32(0U);
    write_tf(w, wse::core::WseTransform::identity());
    w.write_string("spr_castle_gate");
    w.write_u32(4U);
    w.write_u32(5U);
    w.write_f32(1.0f);
    w.write_f32(1.0f);
    w.write_f32(1.0f);

    // AI mesh: 1 vertex (3 floats) + 0 edges -> 20 bytes
    wse::io::BinaryWriter ai_w;
    ai_w.write_i32(1);
    ai_w.write_f32(1.0f);
    ai_w.write_f32(2.0f);
    ai_w.write_f32(3.0f);
    ai_w.write_i32(0);
    w.write_u32(static_cast<std::uint32_t>(ai_w.bytes().size()));
    w.write_bytes(ai_w.bytes());

    w.write_u32(wse::game::kSceneGroundPaintMagic);
    w.write_string("GROUND_PAINT_DATA");

    std::vector<std::uint8_t> raw(w.bytes().size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        raw[i] = static_cast<std::uint8_t>(w.bytes()[i]);
    }
    wse::game::SceneObjectFile sco;
    std::string err;
    expect(wse::game::parse_scene_obj_bytes(raw, sco, &err) && sco.version == 4 &&
               sco.objects.size() == 3U,
           "v4 .sco parses 3 objects");
    expect(sco.objects[0].normalized_id == "spr_village_house" &&
               sco.objects[1].normalized_id == "itm_sword" &&
               sco.objects[2].normalized_id == "spr_castle_gate" &&
               close(sco.objects[0].scale.z, 2.0f) && close(sco.objects[0].transform.o.y, 20.0f),
           ".sco normalizes spr_ prefix for kind 0 and preserves scale/transform");
    expect(sco.has_ai_mesh && sco.ai_vertex_count == 1 && sco.has_ground_paint &&
               sco.ground_paint == "GROUND_PAINT_DATA",
           ".sco decodes v4 AI mesh header and 0xff4ad1a6 ground paint payload");

    // Instantiate into world and verify placed props survive retail 416 clear_multiplayer_scene.
    wse::game::WseWorldState world;
    wse::game::instantiate_scene_objects(sco, world);
    expect(world.mission_objects.size() == 3U && !world.mission_objects[0].m5.enable_pruning &&
               world.mission_objects[1].is_item && close(world.mission_objects[0].scale.z, 2.0f),
           "instantiate_scene_objects sets non-prunable placements into world state");
    world.clear_multiplayer_scene();
    expect(world.mission_objects.size() == 3U && world.mission_objects[0].active,
           ".sco placed props survive retail 416 clear_multiplayer_scene");

    // Regression: instantiating a new scene drops every reference to the previous scene's
    // objects, keeps each record's kind, and only props/passages/items carry a kind number.
    {
        wse::game::WseWorldState w2;
        wse::game::WseMissionObject old_obj{};
        (void)w2.allocate_mission_object(old_obj);
        w2.edit_mode.selected_prop_instances.push_back(0);
        w2.m5.attached_missiles.push_back(wse::game::WseAttachedMissile{0, 0});
        w2.fx.spawn_effector_mission_object_nos.fill(0);

        wse::game::SceneObjectFile kinds;
        kinds.objects.resize(4);
        kinds.objects[0].kind_type = 0U;  // scene prop kind 3
        kinds.objects[0].sub_kind = 3U;
        kinds.objects[1].kind_type = 1U;  // entry point 3
        kinds.objects[1].sub_kind = 3U;
        kinds.objects[1].transform.o = {4.0f, 5.0f, 6.0f, 1.0f};
        kinds.objects[2].kind_type = 4U;  // flora kind 3
        kinds.objects[2].sub_kind = 3U;
        kinds.objects[3].kind_type = 5U;  // passage prop kind 3
        kinds.objects[3].sub_kind = 3U;
        const std::size_t dropped = wse::game::instantiate_scene_objects(kinds, w2);
        expect(dropped == 0U && w2.edit_mode.selected_prop_instances.empty() &&
                   w2.m5.attached_missiles.empty() &&
                   w2.fx.spawn_effector_mission_object_nos[0] == -1,
               "instantiate_scene_objects clears references to the previous scene's objects");
        expect(w2.mission_objects.size() == 4U && w2.mission_objects[0].scene_prop_no == 3 &&
                   w2.mission_objects[1].scene_prop_no == -1 &&
                   w2.mission_objects[2].scene_prop_no == -1 &&
                   w2.mission_objects[3].scene_prop_no == 3 &&
                   w2.mission_objects[1].meta_mesh_no == 1 &&
                   w2.mission_objects[2].meta_mesh_no == 4,
               "entry points and flora keep their meta type and never alias scene-prop kinds");
        expect(w2.entry_points[3].valid && close(w2.entry_points[3].position.o.y, 5.0f) &&
                   !w2.entry_points[0].valid,
               ".sco entry points fill world.entry_points");
    }

    // Corrupt trailing ground paint magic is rejected cleanly.
    raw[w.bytes().size() - 4U - 17U - 1U] ^= 0xFFU;
    wse::game::SceneObjectFile bad_sco;
    expect(!wse::game::parse_scene_obj_bytes(raw, bad_sco, &err),
           ".sco rejects corrupted trailing ground paint magic");
}

void test_skeleton_bodies_xml_parser() {
    const char* xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n"
        "<Data>\n"
        "<!-- ignored multi-line\n"
        "     <Skeleton name=\"fake_commented\"/> -->\n"
        "<Skeletons>\n"
        "  <Skeleton name=\"skel_test\">\n"
        "    <Bones>\n"
        "      <Bone name=\"root\" bone_mass=\"12.5\" bone_twist_limit_min=\"45\" "
        "socket_dir_x=\"3\" socket_dir_y=\"4\" rotate_side=\"90\">\n"
        "        <Bodies>\n"
        "          <body type=\"capsule\" radius=\"0.12\" pos_x=\"0.1\" pos2_y=\"0.4\" for_ragdoll_only=\"1\"/>\n"
        "        </Bodies>\n"
        "      </Bone>\n"
        "      <Bone name=\"leaf\" for_empty=\"1\"/>\n"
        "    </Bones>\n"
        "  </Skeleton>\n"
        "</Skeletons>\n"
        "</Data>\n";

    std::vector<wse::game::LoadedSkeletonDesc> skels;
    std::string err;
    expect(wse::game::parse_skeleton_bodies_xml(xml, skels, &err) && skels.size() == 1U &&
               skels[0].name == "skel_test" && skels[0].bones.size() == 2U,
           "skeleton_bodies XML skips comments and parses Skeleton/Bones");
    const auto& root = skels[0].bones[0];
    expect(close(root.mass, 12.5f) && close(root.twist_limit_min, -45.0f) &&
               close(root.twist_limit_max, 45.0f) && close(root.socket_dir.x, 0.6f) &&
               close(root.socket_dir.y, 0.8f) && close(root.rotate_side_rad, 1.570796f) &&
               root.bodies.size() == 1U && root.bodies[0].for_ragdoll_only &&
               !root.bodies[0].for_collision_only && close(root.bodies[0].radius, 0.12f),
           "mbSkeletonBone::readXMLElement applies twist symmetry, normalized dirs, and rad conversion");

    std::vector<wse::game::LoadedSkeletonDesc> bad_skels;
    expect(!wse::game::parse_skeleton_bodies_xml("<Data><Skeletons></Wrong></Data>", bad_skels, &err),
           "skeleton_bodies XML rejects mismatched tags");
}

void test_skeleton_pose_and_skinning() {
    // 2-bone rest hierarchy: bone 0 at (0,0,1) rotated +90 deg around Z;
    // bone 1 at local (1,0,0) -> world rest = (0, 1, 1).
    wse::asset::BrfSkeleton skel;
    skel.name = "test_skel";
    wse::asset::BrfSkeletonBone b0{};
    b0.parent_index = -1;
    b0.name = "root";
    b0.rot_s = {0.0f, 1.0f, 0.0f};
    b0.rot_f = {-1.0f, 0.0f, 0.0f};
    b0.rot_u = {0.0f, 0.0f, 1.0f};
    b0.origin = {0.0f, 0.0f, 1.0f};
    wse::asset::BrfSkeletonBone b1{};
    b1.parent_index = 0;
    b1.name = "tip";
    b1.rot_s = {1.0f, 0.0f, 0.0f};
    b1.rot_f = {0.0f, 1.0f, 0.0f};
    b1.rot_u = {0.0f, 0.0f, 1.0f};
    b1.origin = {1.0f, 0.0f, 0.0f};
    skel.bones = {b0, b1};

    const wse::asset::SkeletonBindPose bind = wse::asset::build_skeleton_bind_pose(skel);
    expect(bind.world_rest.size() == 2U && close(bind.world_rest[1].o.x, 0.0f) &&
               close(bind.world_rest[1].o.y, 1.0f) && close(bind.world_rest[1].o.z, 1.0f),
           "SkeletonBindPose accumulates hierarchical rest-world transform");

    // Shortest-arc hemisphere interpolation: negating q1 keeps midpoint identical.
    const wse::asset::Quatf q0{0.0f, 0.0f, 0.0f, 1.0f};
    const wse::asset::Quatf q1_neg{0.0f, 0.0f, -0.70710678f, -0.70710678f};
    const wse::asset::Quatf qm = wse::asset::interpolate_quat(q0, q1_neg, 0.5f);
    expect(qm.w > 0.8f && qm.z > 0.3f,
           "interpolate_quat flips negative-dot hemisphere like 0x1402d6bb0");

    // Animation: bone 0 stays aligned to rest (+90 deg Z: z=sin45, w=cos45) and
    // translates +0.5 Z at end; bone 1 rotates +90 deg Z relative to bone 0!
    wse::asset::BrfSkeletonAnimation anim;
    anim.name = "bend";
    anim.bone_rotations.resize(2);
    const wse::asset::Quatf q_z90{0.0f, 0.0f, 0.70710678f, 0.70710678f};
    anim.bone_rotations[0] = {{0, q_z90}, {10, q_z90}};
    anim.bone_rotations[1] = {{0, {0.0f, 0.0f, 0.0f, 1.0f}}, {10, q_z90}};
    anim.root_positions = {{0, {0.0f, 0.0f, 0.0f}}, {10, {0.0f, 0.0f, 0.5f}}};

    std::vector<wse::core::WseTransform> pose_mid, skin_mid;
    wse::asset::evaluate_skeleton_pose(bind, &anim, 0, 10, 1.0f, pose_mid);
    // Verify analytical root motion (`0x14030a96b` / `0x140309ad2`): bone 0 origin gains
    // root translation key `(0, 0, 0.5)`, and bone 1 world origin follows rotated bone 0 + offset.
    expect(close(pose_mid[0].o.x, 0.0f) && close(pose_mid[0].o.y, 0.0f) &&
               close(pose_mid[0].o.z, 1.5f) && close(pose_mid[1].o.x, 0.0f) &&
               close(pose_mid[1].o.y, 1.0f) && close(pose_mid[1].o.z, 1.5f),
           "evaluate_skeleton_pose composes root translation key directly onto root bone[0].o");
    wse::asset::compute_skinning_transforms(bind, pose_mid, skin_mid);

    // Build a rigged mesh with vertex v4 at rest tip origin (0, 2, 1) which is
    // local (1, 0, 0) in bone 1! Under bone 1 rotated +90 deg around Z (from world
    // rest +90 -> world anim +180), local (1,0,0) moves to world (0, 1, 1.5) + (-1,0,0) = (-1, 1, 1.5)!
    wse::asset::BrfMesh mesh = make_rigged_bar_mesh();
    mesh.positions[4] = {0.0f, 2.0f, 1.0f};
    wse::asset::BrfMesh skinned;
    wse::asset::skin_brf_mesh(mesh, skin_mid, skinned);
    expect(close(skinned.positions[4].x, -1.0f, 0.01f) &&
               close(skinned.positions[4].y, 1.0f, 0.01f) &&
               close(skinned.positions[4].z, 1.5f, 0.01f),
           "skin_brf_mesh transforms rigid tip vertex cleanly across rotated parent+child chain");
    // Normal (0, -1, 0) skinned purely by bone 1 rotates +90 deg Z from rest (+1, 0, 0).
    expect(close(skinned.face_corners[4].normal.x, 1.0f, 0.01f) &&
               close(skinned.face_corners[4].normal.y, 0.0f, 0.01f),
           "skin_brf_mesh rotates corner normals with skinning basis");

    // Regression: a vertex with 5 influences keeps the 4 largest regardless of group order,
    // and weights that do not sum to 1 are renormalized instead of shrinking the vertex.
    {
        wse::asset::BrfMesh m;
        m.positions = {{1.0f, 2.0f, 3.0f}};
        for (std::uint8_t b = 0; b < 5U; ++b) {
            wse::asset::BrfVertexGroup g{};
            g.bone_no = b;
            g.pairs = {{0, (b == 0U) ? 0.05f : 0.2f}};  // smallest influence listed first
            m.vertex_groups.push_back(std::move(g));
        }
        const auto sw = wse::asset::build_mesh_skin_data(m, 1U);
        bool keeps_bone0 = false;
        for (std::size_t k = 0; k < wse::asset::kMaxVertexSkinInfluences; ++k) {
            keeps_bone0 = keeps_bone0 || (sw[0].bones[k] == 0U && sw[0].weights[k] > 0.0f);
        }
        expect(!keeps_bone0, "build_mesh_skin_data keeps the 4 largest influences");

        const std::vector<wse::core::WseTransform> ident(5U, wse::core::WseTransform::identity());
        wse::asset::BrfMesh out;
        wse::asset::skin_brf_mesh(m, ident, out);
        expect(close(out.positions[0].x, 1.0f) && close(out.positions[0].y, 2.0f) &&
                   close(out.positions[0].z, 3.0f),
               "skin_brf_mesh renormalizes kept weights (identity pose leaves vertex in place)");
    }
}

void test_rhi_scene_terrain_props_and_skinned_render() {
    // Terrain tile over [-4, 4] x [-4, 4] at z = -0.2.
    wse::game::WorldMapManifold terrain;
    terrain.vertices = {{-4, -4, -0.2f}, {4, -4, -0.2f}, {4, 4, -0.2f}, {-4, 4, -0.2f}};
    terrain.faces = {{4U, 0U, 0U, {0, 1, 2, 3}}};
    terrain.compute_bounds();

    const std::vector<wse::platform::RhiMeshChunk> tchunks =
        wse::platform::build_terrain_chunks(terrain);
    expect(tchunks.size() == 1U && tchunks[0].indices.size() == 6U,
           "build_terrain_chunks triangulates manifold quads with normals and material colors");

    // 2-bone rest skeleton along Z at root=(0,0,0), top=(0,0,1).
    wse::asset::BrfSkeleton skel;
    skel.bones = {
        {-1, "root", 0U, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}},
        {0, "top", 0U, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 1.0f}},
    };
    const auto bind = wse::asset::build_skeleton_bind_pose(skel);

    wse::asset::BrfSkeletonAnimation anim;
    anim.bone_rotations.resize(2);
    anim.bone_rotations[0] = {{0, {0, 0, 0, 1}}, {10, {0, 0, 0, 1}}};
    anim.bone_rotations[1] = {{0, {0, 0, 0, 1}}, {10, {0.0f, 0.70710678f, 0.0f, 0.70710678f}}};  // bend 90 deg X/Z (around Y)

    const wse::asset::BrfMesh prop_mesh = make_unit_pyramid_prop();
    const wse::asset::BrfMesh char_mesh = make_rigged_bar_mesh();

    wse::platform::SoftwareRhiDevice rhi;
    wse::platform::BrfSceneRenderer renderer;
    std::string err;
    expect(renderer.init(rhi, wse::platform::CullMode::back,
                         wse::platform::FrontFace::counter_clockwise, &err),
           "BrfSceneRenderer initializes on SoftwareRhiDevice");

    const auto terrain_id = renderer.upload_chunk(tchunks[0], 0U, &err);
    const auto prop_id = renderer.upload_brf_mesh(prop_mesh, nullptr, &err);
    const auto char_id = renderer.upload_brf_mesh(char_mesh, nullptr, &err);
    expect(terrain_id != 0U && prop_id != 0U && char_id != 0U,
           "BrfSceneRenderer uploads terrain, static prop, and character meshes");

    float vp[16]{};
    wse::platform::make_zup_look_at_vp(1.0f, {0.0f, -5.0f, 2.2f, 1.0f},
                                       {0.0f, 0.0f, 0.6f, 1.0f}, 60.0f, 0.1f, 50.0f, vp);

    wse::core::WseTransform prop_tf = wse::core::WseTransform::identity();
    prop_tf.o = {-1.2f, 0.0f, 0.0f, 1.0f};
    std::vector<wse::platform::SceneDrawInstance> draws = {
        {terrain_id, wse::core::WseTransform::identity(), {1, 1, 1, 1}},
        {prop_id, prop_tf, {1, 1, 1, 1}},
        {char_id, wse::core::WseTransform::identity(), {1, 1, 1, 1}},
    };

    std::vector<wse::core::WseTransform> pose, skin;
    wse::asset::evaluate_skeleton_pose(bind, &anim, 0, 10, 0.0f, pose);
    wse::asset::compute_skinning_transforms(bind, pose, skin);
    const auto vb_before = renderer.slot_vertex_buffer(char_id);
    const auto ib_before = renderer.slot_index_buffer(char_id);
    expect(vb_before != 0U && ib_before != 0U, "initial skinned slot holds valid GPU VB/IB");
    expect(renderer.update_skinned_mesh(char_id, char_mesh, skin, &err),
           "update_skinned_mesh uploads frame 0 vertex buffer");
    expect(renderer.slot_index_buffer(char_id) == ib_before &&
               renderer.slot_vertex_buffer(char_id) == vb_before && vb_before != 0U,
           "update_skinned_mesh refills the VB in place while reusing cached IB");

    std::vector<std::uint8_t> frame0, frame1;
    expect(renderer.render(96U, 96U, vp, draws, frame0, &err) &&
               frame0.size() == 96U * 96U * 4U,
           "BrfSceneRenderer renders scene frame 0");

    wse::asset::evaluate_skeleton_pose(bind, &anim, 0, 10, 1.0f, pose);
    wse::asset::compute_skinning_transforms(bind, pose, skin);
    expect(renderer.update_skinned_mesh(char_id, char_mesh, skin, &err) &&
               renderer.slot_index_buffer(char_id) == ib_before &&
               renderer.render(96U, 96U, vp, draws, frame1, &err),
           "BrfSceneRenderer updates and renders bent pose frame 1 without IB rebuild");

    // Equivalence check: cached VB update matches uploading `skin_brf_mesh` directly.
    wse::asset::BrfMesh direct_deformed;
    wse::asset::skin_brf_mesh(char_mesh, skin, direct_deformed);
    const auto direct_char_id = renderer.upload_brf_mesh(direct_deformed, nullptr, &err);
    std::vector<wse::platform::SceneDrawInstance> direct_draws = draws;
    direct_draws[2].mesh_id = direct_char_id;
    std::vector<std::uint8_t> frame1_direct;
    expect(direct_char_id != 0U && renderer.render(96U, 96U, vp, direct_draws, frame1_direct, &err) &&
               frame1_direct == frame1,
           "cached update_skinned_mesh matches direct skin_brf_mesh upload pixel-for-pixel");

    const std::uint64_t h0 = wse::platform::hash_rgba8_frame(frame0);
    const std::uint64_t h1 = wse::platform::hash_rgba8_frame(frame1);
    expect(h0 != h1 && frame0[3] == 255U,
           "skeletal pose change alters rendered multi-object RHI framebuffer");
}

void test_retail_warband_scene_files_when_present() {
    const std::filesystem::path root = find_warband_root_if_present();
    if (root.empty()) {
        return;  // Optional retail validation; skipped cleanly when game install absent.
    }
    wse::platform::HostVfs vfs(root);

    // 1. Real Data/skeleton_bodies.xml
    std::vector<wse::game::LoadedSkeletonDesc> skel_bodies;
    expect(wse::game::load_skeleton_bodies_xml(vfs, "Data/skeleton_bodies.xml", skel_bodies).ok &&
               skel_bodies.size() >= 2U && skel_bodies[0].name == "skel_human" &&
               skel_bodies[0].bones.size() == 20U && skel_bodies[1].bones.size() == 28U,
           "retail Data/skeleton_bodies.xml loads skel_human(20) and skel_horse(28)");

    // 2. Real Modules/Native/map.txt -> terrain chunks
    wse::game::WorldMapManifold native_map;
    expect(wse::game::load_manifold_map(vfs, "Modules/Native/map.txt", native_map).ok &&
               native_map.vertices.size() == 20791U && native_map.faces.size() == 41387U,
           "retail Modules/Native/map.txt loads 20,791 vertices and 41,387 faces");
    float z_center = 0.0f;
    std::uint16_t mat_center = 0;
    expect(native_map.sample_surface_at(0.0f, 0.0f, z_center, &mat_center),
           "retail Native map.txt samples center height");
    const auto chunks = wse::platform::build_terrain_chunks(native_map);
    expect(!chunks.empty() && chunks.size() >= 2U,
           "retail Native map.txt splits cleanly into 16-bit RHI terrain chunks");

    // 3. Real Modules/Native/SceneObj/*.sco
    wse::game::SceneObjectFile town_sco;
    expect(wse::game::load_scene_obj_file(vfs, "Modules/Native/SceneObj/scn_town_1_center.sco",
                                          town_sco)
                   .ok &&
               !town_sco.objects.empty() && town_sco.has_ai_mesh,
           "retail Native scn_town_1_center.sco decodes objects and AI mesh");

    // 4. Real skel_human + ani_stand.brf + body_meshes.brf rigged character RHI render
    std::vector<std::uint8_t> skel_bytes, anim_bytes, body_bytes;
    expect(vfs.read_file("CommonRes/skeletons.brf", skel_bytes) &&
               vfs.read_file("CommonRes/ani_stand.brf", anim_bytes) &&
               vfs.read_file("CommonRes/body_meshes.brf", body_bytes),
           "retail skeletons.brf, ani_stand.brf, and body_meshes.brf load");
    wse::asset::BrfFile f_skel, f_anim, f_body;
    expect(wse::asset::parse_brf(std::as_bytes(std::span<const std::uint8_t>(skel_bytes)), f_skel) &&
               wse::asset::parse_brf(std::as_bytes(std::span<const std::uint8_t>(anim_bytes)), f_anim) &&
               wse::asset::parse_brf(std::as_bytes(std::span<const std::uint8_t>(body_bytes)), f_body) &&
               !f_skel.skeletons.empty() && !f_anim.animations.empty(),
           "retail skeleton + stand anim + body meshes decode cleanly");

    const wse::asset::BrfSkeleton* human_skel = nullptr;
    for (const auto& s : f_skel.skeletons) {
        if (s.name == "skel_human") {
            human_skel = &s;
            break;
        }
    }
    const wse::asset::BrfMesh* rigged_body = nullptr;
    for (const auto& m : f_body.meshes) {
        if (!m.vertex_groups.empty() && !m.faces.empty() && m.face_corners.size() < 65000U) {
            rigged_body = &m;
            break;
        }
    }
    expect(human_skel != nullptr && rigged_body != nullptr,
           "located retail skel_human and rigged body mesh");
    if (human_skel != nullptr && rigged_body != nullptr) {
        const auto bind = wse::asset::build_skeleton_bind_pose(*human_skel);
        std::vector<wse::core::WseTransform> pose, skin;
        wse::asset::evaluate_skeleton_pose(bind, &f_anim.animations[0], 0, 100, 0.25f, pose);
        wse::asset::compute_skinning_transforms(bind, pose, skin);
        wse::asset::BrfMesh posed_body;
        wse::asset::skin_brf_mesh(*rigged_body, skin, posed_body);

        wse::platform::SoftwareRhiDevice rhi;
        wse::platform::BrfSceneRenderer renderer;
        std::string err;
        expect(renderer.init(rhi, wse::platform::CullMode::back,
                             wse::platform::FrontFace::counter_clockwise, &err),
               "retail BrfSceneRenderer init");
        const auto body_id = renderer.upload_brf_mesh(posed_body, nullptr, &err);
        expect(body_id != 0U, "retail skinned human mesh uploads to RHI");

        float vp[16]{};
        wse::platform::make_zup_look_at_vp(1.0f, {0.0f, -2.5f, 1.0f, 1.0f},
                                           {0.0f, 0.0f, 0.9f, 1.0f}, 60.0f, 0.1f, 50.0f, vp);
        const std::array<wse::platform::SceneDrawInstance, 1> draws{
            wse::platform::SceneDrawInstance{body_id, wse::core::WseTransform::identity(), {1, 1, 1, 1}}
        };
        std::vector<std::uint8_t> rgba;
        expect(renderer.render(64U, 64U, vp, draws, rgba, &err) && rgba.size() == 64U * 64U * 4U,
               "retail posed human mesh renders in SoftwareRhiDevice");
        bool has_shaded_pixel = false;
        for (std::size_t i = 0; i < rgba.size(); i += 4U) {
            if (rgba[i] != 18U || rgba[i + 1U] != 22U || rgba[i + 2U] != 30U) {
                has_shaded_pixel = true;
                break;
            }
        }
        expect(has_shaded_pixel, "retail posed human mesh produces non-background shaded pixels");
    }
}

}  // namespace

int run_scene_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_map_manifold_and_height_query();
    test_sco_scene_object_versions_and_ai_ground();
    test_skeleton_bodies_xml_parser();
    test_skeleton_pose_and_skinning();
    test_rhi_scene_terrain_props_and_skinned_render();
    test_retail_warband_scene_files_when_present();

    if (failures == 0) {
        std::printf("scene tests: PASS\n");
        return 0;
    }
    std::printf("scene tests: FAIL (%d)\n", failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_scene_suite(nullptr);
}
#endif
