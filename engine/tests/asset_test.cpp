// Host unit tests for clean-room v1.1.5.5 .brf and .dds asset decoders
// (engine/include/wse/asset/brf.h and engine/include/wse/asset/dds.h).
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wse/asset/brf.h"
#include "wse/asset/dds.h"
#include "wse/game/module_loader_common.h"
#include "wse/io/binary_stream.h"
#include "wse/platform/vfs.h"

namespace {

int failures = 0;

void expect(bool cond, const char* msg) {
    if (!cond) {
        ++failures;
        std::printf("FAIL  %s\n", msg);
    }
}

std::vector<std::uint8_t> to_u8(std::span<const std::byte> bytes) {
    std::vector<std::uint8_t> out(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        out[i] = std::to_integer<std::uint8_t>(bytes[i]);
    }
    return out;
}

void push_u16_le(wse::io::BinaryWriter& w, std::uint16_t v) {
    w.write_byte(static_cast<std::uint8_t>(v & 0xFFU));
    w.write_byte(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
}

void push_u32_le(wse::io::BinaryWriter& w, std::uint32_t v) {
    w.write_u32(v);
}

std::uint16_t pack_rgb565(int r8, int g8, int b8) {
    const std::uint16_t r = static_cast<std::uint16_t>((r8 >> 3) & 0x1F);
    const std::uint16_t g = static_cast<std::uint16_t>((g8 >> 2) & 0x3F);
    const std::uint16_t b = static_cast<std::uint16_t>((b8 >> 3) & 0x1F);
    return static_cast<std::uint16_t>((r << 11) | (g << 5) | b);
}

void write_dds_header(wse::io::BinaryWriter& w,
                      std::uint32_t width,
                      std::uint32_t height,
                      std::uint32_t flags,
                      std::uint32_t mip_count,
                      std::uint32_t pf_flags,
                      std::uint32_t fourcc,
                      std::uint32_t bpp,
                      std::uint32_t r_mask,
                      std::uint32_t g_mask,
                      std::uint32_t b_mask,
                      std::uint32_t a_mask,
                      std::uint32_t caps = 0x1000U,
                      std::uint32_t caps2 = 0U,
                      std::uint32_t depth = 1U) {
    w.write_u32(wse::asset::dds_detail::fourcc('D', 'D', 'S', ' '));
    w.write_u32(124U);
    w.write_u32(flags);
    w.write_u32(height);
    w.write_u32(width);
    w.write_u32(0U);     // pitch
    w.write_u32(depth);  // depth
    w.write_u32(mip_count);
    for (int i = 0; i < 11; ++i) w.write_u32(0U);
    w.write_u32(32U);
    w.write_u32(pf_flags);
    w.write_u32(fourcc);
    w.write_u32(bpp);
    w.write_u32(r_mask);
    w.write_u32(g_mask);
    w.write_u32(b_mask);
    w.write_u32(a_mask);
    w.write_u32(caps);
    w.write_u32(caps2);
    w.write_u32(0U);
    w.write_u32(0U);
    w.write_u32(0U);
}

void write_vec3(wse::io::BinaryWriter& w, float x, float y, float z) {
    w.write_f32(x);
    w.write_f32(y);
    w.write_f32(z);
}

void test_brf_unversioned_all_eight_types() {
    wse::io::BinaryWriter w;

    // 1. texture
    w.write_string("texture");
    w.write_i32(1);
    w.write_string("sword_tex.dds");
    w.write_u32(0x10000007U);  // bit 0x10000000 masked off by rglTexture decode

    // 2. shader
    w.write_string("shader");
    w.write_i32(1);
    w.write_string("standart_shader");
    w.write_u32(0x80002001U);  // clears 0x2000 because 0x80002000 set
    w.write_u32(0x4U);
    w.write_string("tech_main");
    w.write_i32(2);
    w.write_string("fallback_1");
    w.write_string("fallback_2");
    w.write_i32(1);
    w.write_i32(3);
    w.write_u32(0x18U);  // (color_op & 0x1F) == 0x18 sets bit 0x40000 on shader.flags
    w.write_u32(0x1U);
    w.write_u32(0x2U);

    // 3. material
    w.write_string("material");
    w.write_i32(1);
    w.write_string("sword_mat");
    w.write_u32(0x12U);
    w.write_string("standart_shader");
    w.write_string("sword_tex.dds");
    w.write_string("none");
    w.write_string("none");
    w.write_string("none");
    w.write_string("none");
    w.write_f32(12.5f);
    write_vec3(w, 0.25f, 0.5f, 0.75f);

    // 4. mesh (unversioned -> no tangents, dual UVs)
    w.write_string("mesh");
    w.write_i32(1);
    w.write_string("sword_a");
    w.write_u32(0x30001U);  // even with rfver==0 bits, tangents/single_uv stay false!
    w.write_string("sword_mat");
    w.write_u32(3U);
    write_vec3(w, 0.0f, 0.0f, 0.0f);
    write_vec3(w, 1.0f, 0.0f, 0.0f);
    write_vec3(w, 0.0f, 1.0f, 0.0f);
    w.write_u32(1U);  // 1 vertex group
    w.write_i32(5);
    w.write_u32(2U);
    w.write_i32(0);
    w.write_f32(0.75f);
    w.write_i32(1);
    w.write_f32(0.25f);
    w.write_i32(1);  // 1 vertex key
    w.write_i32(10);
    w.write_u32(1U);
    write_vec3(w, 0.0f, 0.0f, 1.0f);
    w.write_u32(1U);
    write_vec3(w, 0.0f, 1.0f, 0.0f);
    w.write_u32(3U);  // 3 corners
    for (int i = 0; i < 3; ++i) {
        w.write_i32(i);
        w.write_u32(0xFFFFFFFFU);
        write_vec3(w, 0.0f, 0.0f, 1.0f);
        w.write_f32(0.1f * static_cast<float>(i));
        w.write_f32(0.2f * static_cast<float>(i));
        w.write_f32(0.3f * static_cast<float>(i));
        w.write_f32(0.4f * static_cast<float>(i));
    }
    w.write_u32(1U);
    w.write_i32(0);
    w.write_i32(1);
    w.write_i32(2);

    // 5. body (composite containing sphere, capsule, face, manifold)
    w.write_string("body");
    w.write_i32(1);
    w.write_string("bo_sword");
    w.write_string("composite");
    w.write_i32(4);
    w.write_string("sphere");
    w.write_f32(2.5f);
    write_vec3(w, 1.0f, 2.0f, 3.0f);
    w.write_u32(0x1U);
    w.write_string("capsule");
    w.write_f32(0.5f);
    write_vec3(w, 0.0f, 0.0f, 0.0f);
    write_vec3(w, 0.0f, 0.0f, 1.5f);
    w.write_u32(0x2U);
    w.write_string("face");
    w.write_i32(3);
    write_vec3(w, 0.0f, 0.0f, 0.0f);
    write_vec3(w, 1.0f, 0.0f, 0.0f);
    write_vec3(w, 0.0f, 1.0f, 0.0f);
    w.write_u32(0x4U);
    w.write_string("manifold");
    w.write_i32(3);
    write_vec3(w, 0.0f, 0.0f, 0.0f);
    write_vec3(w, 2.0f, 0.0f, 0.0f);
    write_vec3(w, 0.0f, 2.0f, 0.0f);
    w.write_i32(1);
    w.write_i32(-1);
    w.write_u32(0x8U);
    w.write_i32(3);
    w.write_i32(0);
    w.write_i32(1);
    w.write_i32(2);

    // 6. skeleton
    w.write_string("skeleton");
    w.write_i32(1);
    w.write_string("skel_human");
    w.write_i32(2);
    w.write_i32(-1);
    w.write_string("abdomen");
    w.write_u32(0U);
    write_vec3(w, 1.0f, 0.0f, 0.0f);
    write_vec3(w, 0.0f, 1.0f, 0.0f);
    write_vec3(w, 0.0f, 0.0f, 1.0f);
    write_vec3(w, 0.0f, 0.0f, 0.9f);
    w.write_i32(0);
    w.write_string("thorax");
    w.write_u32(1U);
    write_vec3(w, 1.0f, 0.0f, 0.0f);
    write_vec3(w, 0.0f, 1.0f, 0.0f);
    write_vec3(w, 0.0f, 0.0f, 1.0f);
    write_vec3(w, 0.0f, 0.0f, 0.25f);

    // 7. skeleton_anim (stream quaternion order: qy, qz, qw, qx)
    w.write_string("skeleton_anim");
    w.write_i32(1);
    w.write_string("anim_swing");
    w.write_u32(1U);
    w.write_i32(1);
    w.write_i32(15);
    w.write_f32(0.2f);  // qy
    w.write_f32(0.3f);  // qz
    w.write_f32(0.9f);  // qw
    w.write_f32(0.1f);  // qx
    w.write_i32(1);
    w.write_i32(15);
    write_vec3(w, 0.1f, 0.2f, 0.3f);

    // 8. physics_material
    w.write_string("physics_material");
    w.write_i32(1);
    w.write_string("phys_wood");
    w.write_u32(0x3U);
    w.write_f32(0.625f);

    w.write_string("end");

    wse::asset::BrfFile file;
    std::string err;
    expect(wse::asset::parse_brf(w.bytes(), file, &err), "parse unversioned brf");
    expect(file.rfver == 0, "unversioned rfver == 0");
    expect(file.textures.size() == 1 && file.textures[0].flags == 0x7U, "texture masks bit 0x10000000");
    expect(file.shaders.size() == 1 && file.shaders[0].flags == 0x80040001U && file.shaders[0].alternatives.size() == 2,
           "shader flag rules match 0x140301940");
    expect(file.materials.size() == 1 && file.materials[0].specular_coeff == 12.5f &&
               file.materials[0].texture_names[0] == "sword_tex.dds",
           "material parsed");
    expect(file.meshes.size() == 1 && !file.meshes[0].has_tangents && !file.meshes[0].single_uv &&
               file.meshes[0].vertex_groups.size() == 1 && file.meshes[0].vertex_groups[0].bone_no == 5 &&
               file.meshes[0].face_corners[2].uv2.y == 0.8f,
           "unversioned mesh parsed");
    expect(file.bodies.size() == 1 && file.bodies[0].root.kind == wse::asset::BrfBodyPartKind::Composite &&
               file.bodies[0].root.children.size() == 4 &&
               file.bodies[0].root.children[3].manifold_faces[0].vertex_indices.size() == 3,
           "composite body parsed");
    expect(file.skeletons.size() == 1 && file.skeletons[0].bones.size() == 2 &&
               file.skeletons[0].bones[1].parent_index == 0 && file.skeletons[0].bones[1].name == "thorax",
           "skeleton parsed");
    expect(file.animations.size() == 1 && file.animations[0].bone_rotations.size() == 1 &&
               file.animations[0].bone_rotations[0][0].rotation.x == 0.1f &&
               file.animations[0].bone_rotations[0][0].rotation.y == 0.2f &&
               file.animations[0].bone_rotations[0][0].rotation.z == 0.3f &&
               file.animations[0].bone_rotations[0][0].rotation.w == 0.9f,
           "skeleton_anim permutes qy/qz/qw/qx into Quatf{x,y,z,w}");
    expect(file.physics_materials.size() == 1 && file.physics_materials[0].sound_occlusion == 0.625f,
           "physics_material reads m_soundOcclusion");
}

void test_brf_rfver1_metamesh_and_catalog() {
    // rfver 1 mesh with tangents + single_uv
    wse::io::BinaryWriter w;
    w.write_string("rfver ");
    w.write_i32(1);
    w.write_string("mesh");
    w.write_i32(7);

    const std::array<const char*, 7> names = {
        "castle", "castle.1", "castle.lod1", "castle.LOD2", "castle.lod3.1", "castle.lod4", "prop_x"
    };
    for (std::size_t n = 0; n < names.size(); ++n) {
        w.write_string(names[n]);
        w.write_u32(n == 0 ? 0x30000U : 0U);  // tangents + single_uv on mesh 0 only
        w.write_string("stone");
        w.write_u32(1U);
        write_vec3(w, 1.0f, 2.0f, 3.0f);
        w.write_u32(0U);  // vertex groups
        w.write_i32(0);   // vertex keys
        w.write_u32(3U);
        for (int c = 0; c < 3; ++c) {
            w.write_i32(0);
            w.write_u32(0U);
            write_vec3(w, 0.0f, 0.0f, 1.0f);
            if (n == 0) {
                write_vec3(w, 1.0f, 0.0f, 0.0f);
                w.write_bool(true);
            }
            w.write_f32(0.5f);
            w.write_f32(0.25f);
            if (n != 0) {
                w.write_f32(0.9f);
                w.write_f32(0.8f);
            }
        }
        w.write_u32(1U);
        w.write_i32(0);
        w.write_i32(1);
        w.write_i32(2);
    }
    w.write_string("end");

    wse::asset::BrfFile rf1;
    std::string err;
    expect(wse::asset::parse_brf(w.bytes(), rf1, &err), "parse rfver 1 brf");
    expect(rf1.rfver == 1 && rf1.meshes.size() == 7, "rfver 1 count");
    expect(rf1.meshes[0].has_tangents && rf1.meshes[0].single_uv &&
               rf1.meshes[0].face_corners[0].binormal_sign &&
               rf1.meshes[0].face_corners[0].uv2.x == 0.5f,
           "rfver 1 tangents and single_uv decode");

    const auto groups = wse::asset::group_meta_meshes(rf1.meshes);
    expect(groups.size() == 2 && groups[0].base_name == "castle" && groups[1].base_name == "prop_x",
           "MetaMesh grouping count and names");
    expect(groups[0].lod_mesh_indices[0].size() == 2 &&
               groups[0].lod_mesh_indices[1].size() == 1 &&
               groups[0].lod_mesh_indices[2].size() == 1 &&
               groups[0].lod_mesh_indices[3].size() == 1 &&
               groups[0].lod_mesh_indices[4].size() == 1,
           "MetaMesh LOD buckets [0..4]");

    // Verified createMetaMesh (0x1402f29a0) edge cases: dotted non-LOD names land in LOD 0,
    // ".lod3_alt" still selects LOD 3, and prefixes shorter than 2 chars before '.' are not split.
    {
        std::vector<wse::asset::BrfMesh> edge_meshes(4);
        edge_meshes[0].name = "tree";
        edge_meshes[1].name = "tree.1";
        edge_meshes[2].name = "tree.lod3_alt";
        edge_meshes[3].name = "x.lod1";
        const auto edge_groups = wse::asset::group_meta_meshes(edge_meshes);
        expect(edge_groups.size() == 2 &&
                   edge_groups[0].base_name == "tree" &&
                   edge_groups[0].lod_mesh_indices[0].size() == 2 &&
                   edge_groups[0].lod_mesh_indices[3].size() == 1 &&
                   edge_groups[1].base_name == "x.lod1" &&
                   edge_groups[1].lod_mesh_indices[0].size() == 1,
               "MetaMesh createMetaMesh prefix >= 2 and dotted/prefix LOD rules");
    }

    // Rejections
    {
        wse::io::BinaryWriter bad;
        bad.write_string("bogus");
        bad.write_i32(0);
        bad.write_string("end");
        wse::asset::BrfFile out;
        expect(!wse::asset::parse_brf(bad.bytes(), out, &err), "rejects unknown resource type");
    }
    {
        wse::io::BinaryWriter bad;
        bad.write_string("shader");
        bad.write_i32(1);
        bad.write_string("s");
        bad.write_u32(0);
        bad.write_u32(0);
        bad.write_string("t");
        bad.write_i32(4);  // exceeds 3 alternatives
        wse::asset::BrfFile out;
        expect(!wse::asset::parse_brf(bad.bytes(), out, &err), "rejects > 3 shader alternatives");
    }
    {
        wse::io::BinaryWriter bad;
        bad.write_string("mesh");
        bad.write_i32(1);
        bad.write_string("m");
        bad.write_u32(0);
        bad.write_string("mat");
        bad.write_u32(1);
        write_vec3(bad, 0, 0, 0);
        bad.write_u32(0);
        bad.write_i32(0);
        bad.write_u32(1);
        bad.write_i32(99);  // invalid vertex index
        wse::asset::BrfFile out;
        expect(!wse::asset::parse_brf(bad.bytes(), out, &err), "rejects invalid vertex index");
    }
    {
        wse::io::BinaryWriter bad;
        bad.write_string("mesh");
        bad.write_i32(1);
        bad.write_string("m");
        bad.write_u32(0);
        bad.write_string("mat");
        bad.write_u32(1);
        write_vec3(bad, 0, 0, 0);
        bad.write_u32(1);
        bad.write_i32(64);  // bone_no >= 64
        wse::asset::BrfFile out;
        expect(!wse::asset::parse_brf(bad.bytes(), out, &err), "rejects >= 64 bone index");
    }
    {
        const auto truncated = std::span<const std::byte>(w.bytes()).subspan(0, 12);
        wse::asset::BrfFile out;
        expect(!wse::asset::parse_brf(truncated, out, &err), "rejects truncated brf");
    }

    // Catalog loading via MemoryVfs + module.ini
    wse::platform::MemoryVfs vfs;
    const auto rf1_u8 = to_u8(w.bytes());
    wse::io::BinaryWriter empty_brf;
    empty_brf.write_string("end");
    const auto empty_u8 = to_u8(empty_brf.bytes());

    vfs.write_file("warband/CommonRes/core_shaders.brf", empty_u8);
    vfs.write_file("warband/CommonRes/core_materials.brf", empty_u8);
    vfs.write_file("warband/CommonRes/shared_props.brf", rf1_u8);
    vfs.write_file("warband/Modules/Mod/Resource/mod_weapons.brf", rf1_u8);

    wse::game::ModuleIniData ini{};
    wse::game::parse_module_ini(
        "load_resource = shared_props\nload_mod_resource = mod_weapons\nload_resource_nofast = extra_missing\n",
        ini);
    expect(ini.load_resources.size() == 2 && ini.load_mod_resources.size() == 1,
           "module ini captures load_resource_nofast");

    wse::asset::BrfCatalog cat;
    expect(wse::asset::load_module_brf_catalog(vfs, "warband", "warband/Modules/Mod", ini, cat, &err),
           "load_module_brf_catalog succeeds");
    // 6 core + 1 optional wse2_shaders + 2 load_resources + 1 load_mod_resource = 10 entries
    expect(cat.entries.size() == 10 && cat.entries[0].loaded && !cat.entries[6].loaded &&
               cat.entries[7].loaded && cat.entries[9].loaded && cat.entries[9].from_module,
           "catalog loads core + common + mod brf and warns on missing non-optional");
    expect(cat.warnings.size() == 5, "missing non-optional core + nofast logged as warnings (not optional)");
}

bool contains(const std::string& haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

void write_dx10_header(wse::io::BinaryWriter& w,
                       std::uint32_t dxgi_format,
                       std::uint32_t res_dim,
                       std::uint32_t misc,
                       std::uint32_t array_size) {
    w.write_u32(dxgi_format);
    w.write_u32(res_dim);
    w.write_u32(misc);
    w.write_u32(array_size);
    w.write_u32(0U);
}

void test_binary_stream_strings() {
    wse::io::BinaryWriter w;
    w.write_string("");
    w.write_string("abc");
    w.write_string(std::string_view("a\0b", 3));
    wse::io::BinaryReader r(w.bytes());
    std::string a, b, c;
    expect(r.read_string(a) && a.empty() && r.read_string(b) && b == "abc" &&
               r.read_string(c) && c.size() == 3 && c[1] == '\0' && r.at_end(),
           "BinaryWriter/Reader string round-trip (empty, ascii, embedded NUL)");

    wse::io::BinaryWriter big;
    big.write_string("hello");
    wse::io::BinaryReader rb(big.bytes());
    std::string out;
    expect(!rb.read_string(out, 4) && rb.cursor() == 4U, "read_string rejects length above max_len");

    wse::io::BinaryWriter neg;
    neg.write_i32(-1);
    wse::io::BinaryReader rn(neg.bytes());
    expect(!rn.read_string(out), "read_string rejects negative length");

    wse::io::BinaryWriter overrun;
    overrun.write_i32(10);
    overrun.write_byte('x');
    wse::io::BinaryReader ro(overrun.bytes());
    expect(!ro.read_string(out), "read_string rejects length past end of stream");

    std::array<std::byte, 2> buf{};
    wse::io::BinaryReader rs(big.bytes());
    expect(rs.skip(4U) && rs.read_bytes(buf) && buf[0] == std::byte{'h'} && rs.cursor() == 6U &&
               !rs.skip(100U) && rs.cursor() == 6U,
           "skip / read_bytes bounds");
}

void test_brf_hardening() {
    std::string err;
    {
        // Declared count far larger than the in-memory reservation cap: the header is
        // consistent with the stream size (4 B per record) but each BrfMesh is ~200 B.
        constexpr std::size_t kCount = 200000;
        wse::io::BinaryWriter w;
        w.write_string("mesh");
        w.write_i32(static_cast<std::int32_t>(kCount));
        // 0xFF padding makes the first record's name length -1, so parsing stops at
        // record 0 and the capacity reflects only the up-front reservation.
        const std::vector<std::byte> pad(kCount * 4U, std::byte{0xFF});
        w.write_bytes(pad);
        wse::asset::BrfFile out;
        expect(!wse::asset::parse_brf(w.bytes(), out, &err), "huge mesh count still fails to parse");
        expect(out.meshes.capacity() <= wse::asset::detail::kMaxReserveElements,
               "section reservation is capped regardless of declared count");
    }
    {
        // Specific count diagnostic is not overwritten by a generic message.
        wse::io::BinaryWriter w;
        w.write_string("mesh");
        w.write_i32(1);
        w.write_string("m");
        w.write_u32(0);
        w.write_string("mat");
        w.write_u32(1000000U);
        wse::asset::BrfFile out;
        err.clear();
        expect(!wse::asset::parse_brf(w.bytes(), out, &err) && contains(err, "mesh positions in m") &&
                   contains(err, "exceeds remaining stream bytes"),
               "count overflow keeps its specific error message");
    }
    {
        wse::io::BinaryWriter w;
        w.write_string("texture");
        w.write_i32(-2);
        wse::asset::BrfFile out;
        err.clear();
        expect(!wse::asset::parse_brf(w.bytes(), out, &err) && contains(err, "negative count"),
               "negative section count reported as negative");
    }
    {
        wse::io::BinaryWriter w;
        w.write_string("skeleton");
        w.write_i32(1);
        w.write_string("skel");
        wse::asset::BrfFile out;
        err.clear();
        expect(!wse::asset::parse_brf(w.bytes(), out, &err) && contains(err, "truncated skeleton bones in skel count"),
               "truncated count reported as truncated");
    }
    {
        // Vertex index 65536 is valid against the position table but cannot fit
        // rglMeshFaceCorner::m_vertexNo (unsigned short).
        constexpr std::uint32_t kPositions = 65537U;
        wse::io::BinaryWriter w;
        w.write_string("mesh");
        w.write_i32(1);
        w.write_string("big");
        w.write_u32(0);
        w.write_string("mat");
        w.write_u32(kPositions);
        for (std::uint32_t i = 0; i < kPositions; ++i) write_vec3(w, 0, 0, 0);
        w.write_u32(0);  // vertex groups
        w.write_i32(0);  // vertex keys
        w.write_u32(1);  // face corners
        w.write_i32(65536);
        w.write_u32(0);
        write_vec3(w, 0, 0, 1);
        w.write_f32(0);
        w.write_f32(0);
        w.write_f32(0);
        w.write_f32(0);
        w.write_u32(0);  // faces
        w.write_string("end");
        wse::asset::BrfFile out;
        err.clear();
        expect(!wse::asset::parse_brf(w.bytes(), out, &err) && contains(err, "Invalid vertex index 65536"),
               "rejects vertex index above unsigned short range");
    }
    {
        // Composite nesting beyond the depth limit is rejected rather than recursing.
        wse::io::BinaryWriter w;
        w.write_string("body");
        w.write_i32(1);
        w.write_string("deep");
        for (int i = 0; i < 10; ++i) {
            w.write_string("composite");
            w.write_i32(1);
        }
        w.write_string("sphere");
        w.write_f32(1);
        write_vec3(w, 0, 0, 0);
        w.write_u32(0);
        w.write_string("end");
        wse::asset::BrfFile out;
        err.clear();
        expect(!wse::asset::parse_brf(w.bytes(), out, &err) && contains(err, "depth limit"),
               "rejects composite body nesting beyond depth limit");
    }
}

void test_dds_hardening() {
    std::string err;
    const std::uint32_t argb_masks[4] = {0x00FF0000U, 0x0000FF00U, 0x000000FFU, 0xFF000000U};
    {
        // Legacy multi-slice volume texture (DDSCAPS2_VOLUME + DDSD_DEPTH, depth = 4).
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U | 0x800000U, 1, 0x41U, 0U, 32U, argb_masks[0], argb_masks[1],
                         argb_masks[2], argb_masks[3], 0x1000U, 0x200000U, 4U);
        const std::vector<std::byte> px(4U * 4U * 4U * 4U * 4U);
        w.write_bytes(px);
        wse::asset::DdsImageView img;
        err.clear();
        expect(!wse::asset::parse_dds(w.bytes(), img, &err) && contains(err, "volume"),
               "rejects legacy multi-slice volume DDS");
    }
    {
        // DDSCAPS2_VOLUME without DDSD_DEPTH is still a single slice when dwDepth is 0 or 1.
        for (const std::uint32_t depth : {0U, 1U}) {
            wse::io::BinaryWriter w;
            write_dds_header(w, 4, 4, 0x1007U, 1, 0x41U, 0U, 32U, argb_masks[0], argb_masks[1],
                             argb_masks[2], argb_masks[3], 0x1000U, 0x200000U, depth);
            const std::vector<std::byte> px(4U * 4U * 4U);
            w.write_bytes(px);
            wse::asset::DdsImageView img;
            err.clear();
            expect(wse::asset::parse_dds(w.bytes(), img, &err) && img.slices.size() == 1U,
                   "accepts single-slice DDSCAPS2_VOLUME without DDSD_DEPTH");
        }
    }
    {
        // DDSCAPS2_VOLUME without DDSD_DEPTH but dwDepth = 4 is a multi-slice volume.
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x41U, 0U, 32U, argb_masks[0], argb_masks[1],
                         argb_masks[2], argb_masks[3], 0x1000U, 0x200000U, 4U);
        const std::vector<std::byte> px(4U * 4U * 4U * 4U);
        w.write_bytes(px);
        wse::asset::DdsImageView img;
        err.clear();
        expect(!wse::asset::parse_dds(w.bytes(), img, &err) && contains(err, "volume"),
               "rejects DDSCAPS2_VOLUME with dwDepth > 1 and no DDSD_DEPTH");
    }
    {
        // Plain 2D texture (no DDSD_DEPTH, no DDSCAPS2_VOLUME) with junk dwDepth: depth ignored.
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x41U, 0U, 32U, argb_masks[0], argb_masks[1],
                         argb_masks[2], argb_masks[3], 0x1000U, 0U, 7U);
        const std::vector<std::byte> px(4U * 4U * 4U);
        w.write_bytes(px);
        wse::asset::DdsImageView img;
        err.clear();
        expect(wse::asset::parse_dds(w.bytes(), img, &err) && img.slices.size() == 1U &&
                   img.width == 4U && img.height == 4U,
               "ignores dwDepth on a 2D DDS without DDSD_DEPTH");
    }
    {
        // Single-slice DDSCAPS2_VOLUME + DDSD_DEPTH + depth == 1 (retail Warband cnt/font.dds).
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U | 0x800000U, 1, 0x41U, 0U, 32U, argb_masks[0], argb_masks[1],
                         argb_masks[2], argb_masks[3], 0x1000U, 0x200000U, 1U);
        const std::vector<std::byte> px(4U * 4U * 4U);
        w.write_bytes(px);
        wse::asset::DdsImageView img;
        err.clear();
        expect(wse::asset::parse_dds(w.bytes(), img, &err) && img.slices.size() == 1U,
               "accepts single-slice DDSCAPS2_VOLUME with DDSD_DEPTH and depth == 1");
    }
    {
        // DX10 TEXTURE3D with dwDepth = 4.
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U | 0x800000U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', '1', '0'),
                         0, 0, 0, 0, 0, 0x1000U, 0x200000U, 4U);
        write_dx10_header(w, 28U, 4U, 0U, 1U);
        const std::vector<std::byte> px(4U * 4U * 4U * 4U);
        w.write_bytes(px);
        wse::asset::DdsImageView img;
        err.clear();
        expect(!wse::asset::parse_dds(w.bytes(), img, &err) && contains(err, "volume"),
               "rejects multi-slice DX10 TEXTURE3D");
    }
    {
        // DX10 TEXTURE3D with a single slice (legacy header depth 1, no DDSD_DEPTH) is 2D.
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', '1', '0'),
                         0, 0, 0, 0, 0);
        write_dx10_header(w, 28U, 4U, 0U, 1U);
        const std::vector<std::byte> px(4U * 4U * 4U);
        w.write_bytes(px);
        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> rgba;
        err.clear();
        expect(wse::asset::parse_dds(w.bytes(), img, &err) && img.slices.size() == 1U &&
                   wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba) && rgba.size() == 64U,
               "accepts single-slice DX10 TEXTURE3D");
    }
    {
        // DX10 cube array of 2 cubes with the legacy face bits also set: 12 faces.
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', '1', '0'),
                         0, 0, 0, 0, 0, 0x1008U, 0x200U | 0xFC00U);
        write_dx10_header(w, 71U, 3U, 0x4U, 2U);
        const std::vector<std::byte> blocks(12U * 8U);
        w.write_bytes(blocks);
        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> rgba;
        expect(wse::asset::parse_dds(w.bytes(), img, &err) && img.is_cubemap && img.face_count == 12U &&
                   img.slices.size() == 12U && wse::asset::decode_dds_mip_rgba8(img, 0, 11, rgba),
               "DX10 cube array counts six faces per array element");
        expect(!wse::asset::parse_dds(std::span<const std::byte>(w.bytes()).subspan(0, w.size() - 8U), img, &err),
               "DX10 cube array with a missing face is truncated");
    }
    {
        // Legacy partial cube map keeps the face-bit count.
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', 'T', '1'),
                         0, 0, 0, 0, 0, 0x1008U, 0x200U | 0x400U | 0x800U);
        const std::vector<std::byte> blocks(2U * 8U);
        w.write_bytes(blocks);
        wse::asset::DdsImageView img;
        expect(wse::asset::parse_dds(w.bytes(), img, &err) && img.face_count == 2U,
               "legacy partial cube map counts face bits");
    }
    {
        // sRGB flag for DX10 *_SRGB formats only.
        for (const std::uint32_t dxgi : {71U, 72U}) {
            wse::io::BinaryWriter w;
            write_dds_header(w, 4, 4, 0x1007U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', '1', '0'),
                             0, 0, 0, 0, 0);
            write_dx10_header(w, dxgi, 3U, 0U, 1U);
            const std::vector<std::byte> block(8U);
            w.write_bytes(block);
            wse::asset::DdsImageView img;
            expect(wse::asset::parse_dds(w.bytes(), img, &err) && img.format == wse::asset::DdsFormat::BC1_UNORM &&
                       img.is_srgb == (dxgi == 72U),
                   "DX10 sRGB flag set only for BC1_UNORM_SRGB");
        }
    }
    {
        // Truncated mip chain: header claims 3 mips, payload holds 2.
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U | 0x20000U, 3, 0x41U, 0U, 32U, argb_masks[0], argb_masks[1],
                         argb_masks[2], argb_masks[3]);
        const std::vector<std::byte> px((16U + 4U) * 4U);
        w.write_bytes(px);
        wse::asset::DdsImageView img;
        err.clear();
        expect(!wse::asset::parse_dds(w.bytes(), img, &err) && contains(err, "truncated DDS mip payload"),
               "rejects truncated mip chain");
    }
    {
        // Hand-modified views cannot make the decoder read out of bounds.
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x41U, 0U, 32U, argb_masks[0], argb_masks[1],
                         argb_masks[2], argb_masks[3]);
        const std::vector<std::byte> px(64U);
        w.write_bytes(px);
        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> rgba;
        expect(wse::asset::parse_dds(w.bytes(), img, &err), "parse 4x4 BGRA for tamper tests");
        wse::asset::DdsImageView short_slice = img;
        short_slice.slices[0].data = short_slice.slices[0].data.subspan(0, 32U);
        expect(!wse::asset::decode_dds_mip_rgba8(short_slice, 0, 0, rgba, &err),
               "decode rejects slice payload smaller than its dimensions");
        wse::asset::DdsImageView no_slices = img;
        no_slices.slices.clear();
        expect(!wse::asset::decode_dds_mip_rgba8(no_slices, 0, 0, rgba, &err),
               "decode rejects slice table that does not match face/mip count");
        wse::asset::DdsImageView grown = img;
        grown.slices[0].width = 8U;
        expect(!wse::asset::decode_dds_mip_rgba8(grown, 0, 0, rgba, &err),
               "decode rejects slice dimensions larger than its payload");
    }
}

void test_dds_formats_and_resolver() {
    // 1. DXT1 4-color opaque (c0 > c1) and 1-bit transparent (c0 <= c1)
    {
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', 'T', '1'),
                         0, 0, 0, 0, 0);
        const std::uint16_t c0 = pack_rgb565(255, 255, 255);  // 0xFFFF
        const std::uint16_t c1 = pack_rgb565(0, 0, 0);        // 0x0000 -> c0 > c1
        push_u16_le(w, c0);
        push_u16_le(w, c1);
        push_u32_le(w, 0xE4U);  // px 0:0, px 1:1, px 2:2, px 3:3

        wse::asset::DdsImageView img;
        std::string err;
        expect(wse::asset::parse_dds(w.bytes(), img, &err), "parse DXT1 opaque");
        std::vector<std::uint8_t> rgba;
        expect(wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba, &err), "decode DXT1 opaque");
        expect(rgba.size() == 64 && rgba[0] == 255 && rgba[3] == 255 &&
                   rgba[4] == 0 && rgba[7] == 255 && rgba[8] == 170 && rgba[12] == 85,
               "DXT1 4-color palette matches spec");
    }
    {
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', 'T', '1'),
                         0, 0, 0, 0, 0);
        push_u16_le(w, 0x0000U);  // c0 <= c1 -> index 3 is transparent
        push_u16_le(w, 0xFFFFU);
        push_u32_le(w, 0xE4U);
        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> rgba;
        expect(wse::asset::parse_dds(w.bytes(), img) && wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba),
               "decode DXT1 1-bit alpha");
        expect(rgba[8] == 127 && rgba[11] == 255 && rgba[15] == 0, "DXT1 1-bit alpha transparent pixel");
    }

    // 2. DXT3 explicit 4-bit alpha (also c0 <= c1 still uses 4-color mode)
    {
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', 'T', '3'),
                         0, 0, 0, 0, 0);
        push_u16_le(w, 0xF500U);  // row 0: px0=0, px1=0, px2=5 (->0x55), px3=15 (->0xFF)
        push_u16_le(w, 0U);
        push_u16_le(w, 0U);
        push_u16_le(w, 0U);
        push_u16_le(w, 0x0000U);
        push_u16_le(w, 0xFFFFU);
        push_u32_le(w, 0xE4U);
        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> rgba;
        expect(wse::asset::parse_dds(w.bytes(), img) && wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba),
               "decode DXT3");
        expect(rgba[3] == 0 && rgba[11] == 0x55 && rgba[15] == 255 && rgba[12] == 170,
               "DXT3 alpha expansion and 4-color interpolation");
    }

    // 3. DXT5 both alpha modes + BC4
    {
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', 'T', '5'),
                         0, 0, 0, 0, 0);
        // 8-alpha mode (a0=200 > a1=60): px0 idx0->200, px1 idx1->60, px2 idx2->180
        w.write_byte(200);
        w.write_byte(60);
        const std::uint64_t idx = (0ULL << 0) | (1ULL << 3) | (2ULL << 6);
        for (int b = 0; b < 6; ++b) w.write_byte(static_cast<std::uint8_t>((idx >> (b * 8)) & 0xFFULL));
        push_u16_le(w, 0xFFFFU);
        push_u16_le(w, 0xFFFFU);
        push_u32_le(w, 0U);
        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> rgba;
        expect(wse::asset::parse_dds(w.bytes(), img) && wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba),
               "decode DXT5 8-alpha");
        expect(rgba[3] == 200 && rgba[7] == 60 && rgba[11] == 180, "DXT5 8-alpha interpolation");
    }
    {
        wse::io::BinaryWriter w;
        write_dds_header(w, 4, 4, 0x1007U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', 'T', '5'),
                         0, 0, 0, 0, 0);
        // 6-alpha mode (a0=50 <= a1=200): idx6=0, idx7=255, idx2=(4*50+200)/5=80
        w.write_byte(50);
        w.write_byte(200);
        const std::uint64_t idx = (6ULL << 0) | (7ULL << 3) | (2ULL << 6);
        for (int b = 0; b < 6; ++b) w.write_byte(static_cast<std::uint8_t>((idx >> (b * 8)) & 0xFFULL));
        push_u16_le(w, 0xFFFFU);
        push_u16_le(w, 0xFFFFU);
        push_u32_le(w, 0U);
        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> rgba;
        expect(wse::asset::parse_dds(w.bytes(), img) && wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba),
               "decode DXT5 6-alpha");
        expect(rgba[3] == 0 && rgba[7] == 255 && rgba[11] == 80, "DXT5 6-alpha interpolation");
    }

    // 4. Uncompressed B8G8R8A8 + B8G8R8 + L8 + mip chain slicing + DX10
    {
        wse::io::BinaryWriter w;
        write_dds_header(w, 2, 2, 0x21007U, 2, 0x41U, 0U, 32U,
                         0x00FF0000U, 0x0000FF00U, 0x000000FFU, 0xFF000000U);
        // mip 0 (4 pixels BGRA): [B, G, R, A]
        for (int i = 0; i < 4; ++i) {
            w.write_byte(10);
            w.write_byte(20);
            w.write_byte(30);
            w.write_byte(40);
        }
        // mip 1 (1x1): [B=1, G=2, R=3, A=255]
        w.write_byte(1);
        w.write_byte(2);
        w.write_byte(3);
        w.write_byte(255);

        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> rgba0, rgba1;
        expect(wse::asset::parse_dds(w.bytes(), img) && img.mip_count == 2 && img.slices.size() == 2,
               "parse uncompressed BGRA with 2 mips");
        expect(wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba0) &&
                   wse::asset::decode_dds_mip_rgba8(img, 1, 0, rgba1),
               "decode mip 0 and mip 1");
        expect(rgba0[0] == 30 && rgba0[1] == 20 && rgba0[2] == 10 && rgba0[3] == 40 &&
                   rgba1[0] == 3 && rgba1[1] == 2 && rgba1[2] == 1 && rgba1[3] == 255,
               "BGRA channel swizzle");
    }
    {
        // DX10 BC1 2x2
        wse::io::BinaryWriter w;
        write_dds_header(w, 2, 2, 0x1007U, 1, 0x4U, wse::asset::dds_detail::fourcc('D', 'X', '1', '0'),
                         0, 0, 0, 0, 0);
        w.write_u32(71U);  // DXGI_FORMAT_BC1_UNORM
        w.write_u32(3U);   // TEXTURE2D
        w.write_u32(0U);
        w.write_u32(1U);
        w.write_u32(0U);
        push_u16_le(w, 0xFFFFU);
        push_u16_le(w, 0U);
        push_u32_le(w, 0U);
        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> rgba;
        expect(wse::asset::parse_dds(w.bytes(), img) && img.has_dx10_header &&
                   img.format == wse::asset::DdsFormat::BC1_UNORM &&
                   wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba) && rgba.size() == 16,
               "parse and decode DX10 BC1_UNORM");
    }
    {
        // 16-bit luminance (L16_UNORM, used by Warband skybox_*_exp.dds)
        wse::io::BinaryWriter w;
        write_dds_header(w, 2, 1, 0x1007U, 1, 0x20000U, 0U, 16U, 0xFFFFU, 0U, 0U, 0U);
        push_u16_le(w, 0x1234U);
        push_u16_le(w, 0xABCDU);
        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> rgba;
        expect(wse::asset::parse_dds(w.bytes(), img) &&
                   img.format == wse::asset::DdsFormat::L16_UNORM &&
                   !img.has_alpha &&
                   wse::asset::decode_dds_mip_rgba8(img, 0, 0, rgba) && rgba.size() == 8 &&
                   rgba[0] == 0x12 && rgba[1] == 0x12 && rgba[2] == 0x12 && rgba[3] == 255 &&
                   rgba[4] == 0xAB && rgba[7] == 255,
               "parse and decode L16_UNORM");
    }

    // 5. DDS error checks
    {
        std::array<std::byte, 4> bad{std::byte{'N'}, std::byte{'O'}, std::byte{'P'}, std::byte{'E'}};
        wse::asset::DdsImageView img;
        std::string err;
        expect(!wse::asset::parse_dds(bad, img, &err), "reject bad DDS magic");
    }

    // 6. Texture path resolution priority (tryGetTexture 0x1402f2f10 + initDeviceObjects 0x140317710)
    {
        expect(wse::asset::texture_frame_count(0U) == 1U &&
                   wse::asset::texture_frame_count(0x01000000U) == 4U &&
                   wse::asset::texture_frame_count(0x07000000U) == 28U &&
                   wse::asset::texture_frame_count(0x0F000000U) == 60U,
               "texture_frame_count matches ((flags >> 24) & 0xF) * 4");

        wse::platform::MemoryVfs vfs;
        const std::vector<std::uint8_t> dummy{1};
        vfs.write_file("warband/Textures/ui_icon.dds", dummy);
        vfs.write_file("warband/Textures/languages/fr/ui_icon.dds", dummy);
        vfs.write_file("warband/TexturesHD/ui_icon.dds", dummy);
        vfs.write_file("warband/Modules/Mod/Textures/ui_icon.dds", dummy);
        vfs.write_file("warband/Modules/Mod/Textures/languages/fr/ui_icon.dds", dummy);
        vfs.write_file("warband/Textures/languages/fr/lang_only.dds", dummy);
        vfs.write_file("warband/Textures/lang_only.dds", dummy);
        vfs.write_file("warband/Textures/base_only.tga", dummy);
        vfs.write_file("warband/TexturesHD/hd_only.dds", dummy);
        vfs.write_file("warband/Textures/rock.1.dds", dummy);

        vfs.write_file("warband/Textures/waterbump_0.dds", dummy);
        vfs.write_file("warband/TexturesHD/waterbump_0.dds", dummy);
        vfs.write_file("warband/Textures/languages/fr/waterbump_0.dds", dummy);
        vfs.write_file("warband/Modules/Mod/Textures/waterbump_0.dds", dummy);
        vfs.write_file("warband/Modules/Mod/Textures/languages/fr/waterbump_0.dds", dummy);
        vfs.write_file("warband/Textures/waterbump_5.dds", dummy);
        vfs.write_file("warband/Textures/languages/fr/anim_lang_0.dds", dummy);
        vfs.write_file("warband/TexturesHD/anim_lang_0.dds", dummy);

        expect(wse::asset::resolve_texture_path(vfs, "Textures\\ui_icon", "warband", "warband/Modules/Mod", "fr", true) ==
                   "warband/Modules/Mod/Textures/languages/fr/ui_icon.dds",
               "module localized texture takes highest priority");
        expect(wse::asset::resolve_texture_path(vfs, "ui_icon", "warband", "warband/Modules/Mod", "", true) ==
                   "warband/Modules/Mod/Textures/ui_icon.dds",
               "module texture overrides base TexturesHD, base language, and Textures");
        expect(wse::asset::resolve_texture_path(vfs, "ui_icon", "warband", "", "fr", true) ==
                   "warband/TexturesHD/ui_icon.dds",
               "TexturesHD overrides base Textures/languages when enabled");
        expect(wse::asset::resolve_texture_path(vfs, "lang_only", "warband", "", "fr", true) ==
                   "warband/Textures/languages/fr/lang_only.dds",
               "base Textures/languages used when absent from module and TexturesHD");
        expect(wse::asset::resolve_texture_path(vfs, "base_only", "warband", "", "", true) ==
                   "warband/Textures/base_only.tga",
               "probes .dds -> exact -> .jpg -> .tga");
        expect(wse::asset::resolve_texture_path(vfs, "rock.1", "warband", "", "", true) ==
                   "warband/Textures/rock.1.dds",
               "dotted texture stem probes .dds via tryGetTexture");
        expect(wse::asset::resolve_texture_path(vfs, "hd_only.dds", "warband", "", "", false).empty() &&
                   wse::asset::resolve_texture_path(vfs, "hd_only.dds", "warband", "", "", true) ==
                       "warband/TexturesHD/hd_only.dds",
               "TexturesHD searched only when enabled");

        expect(wse::asset::resolve_texture_path(vfs, "waterbump.dds", "warband", "warband/Modules/Mod", "fr", true, 0) ==
                   "warband/Modules/Mod/Textures/languages/fr/waterbump_0.dds",
               "indexed frame prefers module language");
        expect(wse::asset::resolve_texture_path(vfs, "waterbump", "warband", "warband/Modules/Mod", "", true, 0) ==
                   "warband/Modules/Mod/Textures/waterbump_0.dds",
               "indexed frame checks module Textures");
        expect(wse::asset::resolve_texture_path(vfs, "anim_lang.dds", "warband", "", "fr", true, 0) ==
                   "warband/Textures/languages/fr/anim_lang_0.dds",
               "indexed frame skips TexturesHD and uses base language");
        expect(wse::asset::resolve_texture_path(vfs, "waterbump.dds", "warband", "", "", true, 0) ==
                   "warband/Textures/waterbump_0.dds",
               "indexed frame skips TexturesHD and falls back to base Textures");
        expect(wse::asset::resolve_texture_path(vfs, "waterbump", "warband", "", "", false, 5) ==
                   "warband/Textures/waterbump_5.dds",
               "indexed frame formats <stem>_<frame>.dds");

        // An empty warband_root is the VFS root (vfs_join_path("", x) == x).
        wse::platform::MemoryVfs root_vfs;
        root_vfs.write_file("Textures/rootless.dds", dummy);
        root_vfs.write_file("TexturesHD/rootless_hd.dds", dummy);
        root_vfs.write_file("Textures/languages/fr/rootless_lang.dds", dummy);
        root_vfs.write_file("Textures/rootless_anim_2.dds", dummy);
        expect(wse::asset::resolve_texture_path(root_vfs, "rootless", "") == "Textures/rootless.dds" &&
                   wse::asset::resolve_texture_path(root_vfs, "rootless_hd", "", "", "", true) ==
                       "TexturesHD/rootless_hd.dds" &&
                   wse::asset::resolve_texture_path(root_vfs, "rootless_lang", "", "", "fr") ==
                       "Textures/languages/fr/rootless_lang.dds" &&
                   wse::asset::resolve_texture_path(root_vfs, "rootless_anim", "", "", "", false, 2) ==
                       "Textures/rootless_anim_2.dds",
               "empty warband_root probes base directories at the VFS root");
    }
}

// Counts is_file/is_dir calls on top of a MemoryVfs.
class CountingVfs final : public wse::platform::Vfs {
public:
    wse::platform::MemoryVfs inner;
    std::size_t is_file_calls = 0;
    std::size_t is_dir_calls = 0;

    bool read_file(std::string_view path, std::vector<std::uint8_t>& out) override {
        return inner.read_file(path, out);
    }
    bool write_file(std::string_view path, std::span<const std::uint8_t> data) override {
        return inner.write_file(path, data);
    }
    bool remove_file(std::string_view path) override { return inner.remove_file(path); }
    bool list_dir(std::string_view path, std::vector<wse::platform::VfsEntry>& out) override {
        return inner.list_dir(path, out);
    }
    bool is_file(std::string_view path) override {
        ++is_file_calls;
        return inner.is_file(path);
    }
    bool is_dir(std::string_view path) override {
        ++is_dir_calls;
        return inner.is_dir(path);
    }
};

std::vector<std::byte> dds_with_payload(std::uint32_t width, std::uint32_t height, std::uint32_t fourcc,
                                        const std::vector<std::byte>& payload) {
    wse::io::BinaryWriter w;
    write_dds_header(w, width, height, 0x1007U, 1, 0x4U, fourcc, 0, 0, 0, 0, 0);
    w.write_bytes(payload);
    return std::vector<std::byte>(w.bytes().begin(), w.bytes().end());
}

void test_dds_decode_blocks_and_resolver_probes() {
    using wse::asset::dds_detail::fourcc;
    // Deterministic block data: any 16 bytes are a valid BC2/BC3 block.
    std::vector<std::byte> blocks(4U * 16U);
    std::uint32_t seed = 0x12345678U;
    for (std::byte& b : blocks) {
        seed = seed * 1664525U + 1013904223U;
        b = static_cast<std::byte>(seed >> 24);
    }
    for (const std::uint32_t cc : {fourcc('D', 'X', 'T', '3'), fourcc('D', 'X', 'T', '5')}) {
        // 8x8 and 6x5 share the same 2x2 block grid; 6x5 must be the top-left crop of 8x8.
        const auto full_bytes = dds_with_payload(8, 8, cc, blocks);
        const auto crop_bytes = dds_with_payload(6, 5, cc, blocks);
        wse::asset::DdsImageView full_img, crop_img;
        std::vector<std::uint8_t> full, crop;
        const bool ok = wse::asset::parse_dds(full_bytes, full_img) &&
                        wse::asset::parse_dds(crop_bytes, crop_img) &&
                        wse::asset::decode_dds_mip_rgba8(full_img, 0, 0, full) &&
                        wse::asset::decode_dds_mip_rgba8(crop_img, 0, 0, crop) &&
                        full.size() == 8U * 8U * 4U && crop.size() == 6U * 5U * 4U;
        bool same = ok;
        for (std::size_t y = 0; same && y < 5U; ++y) {
            for (std::size_t x = 0; x < 6U * 4U; ++x) {
                if (crop[y * 6U * 4U + x] != full[y * 8U * 4U + x]) {
                    same = false;
                    break;
                }
            }
        }
        expect(same, "edge blocks of a non-multiple-of-4 image are clipped, not shifted");

        // A reused buffer of the same size is fully overwritten.
        std::vector<std::uint8_t> dirty(full.size(), 0xA5U);
        expect(wse::asset::decode_dds_mip_rgba8(full_img, 0, 0, dirty) && dirty == full,
               "decode into a reused dirty buffer matches a fresh decode");
        std::vector<std::uint8_t> larger(4096U, 0xA5U);
        expect(wse::asset::decode_dds_mip_rgba8(crop_img, 0, 0, larger) && larger == crop,
               "decode into a larger reused buffer shrinks it to the image");
    }
    {
        // Uncompressed path into a dirty buffer.
        wse::io::BinaryWriter w;
        write_dds_header(w, 3, 1, 0x1007U, 1, 0x41U, 0U, 32U, 0x00FF0000U, 0x0000FF00U, 0x000000FFU,
                         0xFF000000U);
        for (int i = 0; i < 12; ++i) w.write_byte(static_cast<std::uint8_t>(i * 7));
        wse::asset::DdsImageView img;
        std::vector<std::uint8_t> fresh, dirty(12U, 0xA5U);
        expect(wse::asset::parse_dds(w.bytes(), img) && wse::asset::decode_dds_mip_rgba8(img, 0, 0, fresh) &&
                   wse::asset::decode_dds_mip_rgba8(img, 0, 0, dirty) && dirty == fresh,
               "uncompressed decode into a reused dirty buffer matches a fresh decode");
    }
    {
        // DX10 TEXTURE3D must have array_size == 1 and no TEXTURECUBE flag.
        const std::pair<std::uint32_t, std::uint32_t> bad[] = {{0U, 2U}, {0x4U, 1U}};
        for (const auto& [misc, array_size] : bad) {
            wse::io::BinaryWriter w;
            write_dds_header(w, 4, 4, 0x1007U, 1, 0x4U, fourcc('D', 'X', '1', '0'), 0, 0, 0, 0, 0);
            write_dx10_header(w, 28U, 4U, misc, array_size);
            const std::vector<std::byte> px(4U * 4U * 4U * 6U * 2U);
            w.write_bytes(px);
            wse::asset::DdsImageView img;
            std::string err;
            expect(!wse::asset::parse_dds(w.bytes(), img, &err) && contains(err, "volume"),
                   "rejects DX10 TEXTURE3D with array_size > 1 or TEXTURECUBE");
        }
    }
    {
        // A missing directory costs one is_dir call, not one is_file per candidate.
        CountingVfs vfs;
        const std::vector<std::uint8_t> dummy{0};
        vfs.write_file("warband/Textures/stone.dds", dummy);
        vfs.write_file("warband/Textures/languages/fr/sign.dds", dummy);
        vfs.is_file_calls = 0;
        vfs.is_dir_calls = 0;
        // Missing: module Textures/languages/de, module Textures, TexturesHD, base languages/de.
        expect(wse::asset::resolve_texture_path(vfs, "stone", "warband", "warband/Modules/M", "de", true) ==
                   "warband/Textures/stone.dds",
               "resolver finds the base texture past four missing directories");
        expect(vfs.is_file_calls == 1U && vfs.is_dir_calls == 5U,
               "missing directories are skipped without per-candidate is_file probes");

        // A trailing separator on the language gives the same path as vfs_join_path.
        expect(wse::asset::resolve_texture_path(vfs, "sign", "warband", "", "fr/") ==
                   "warband/Textures/languages/fr/sign.dds",
               "language with a trailing separator joins without a double slash");
    }
}

}  // namespace

namespace wse::selftest {

int run_asset_suite(const char* fixture_path) {
    (void)fixture_path;
    failures = 0;
    test_brf_unversioned_all_eight_types();
    test_brf_rfver1_metamesh_and_catalog();
    test_dds_formats_and_resolver();
    test_binary_stream_strings();
    test_brf_hardening();
    test_dds_hardening();
    test_dds_decode_blocks_and_resolver_probes();

    if (failures == 0) {
        std::printf("asset tests: PASS\n");
        return 0;
    }
    std::printf("asset tests: FAIL (%d)\n", failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_asset_suite(nullptr);
}
#endif
