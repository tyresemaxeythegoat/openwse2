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
                      std::uint32_t caps2 = 0U) {
    w.write_u32(wse::asset::dds_detail::fourcc('D', 'D', 'S', ' '));
    w.write_u32(124U);
    w.write_u32(flags);
    w.write_u32(height);
    w.write_u32(width);
    w.write_u32(0U);  // pitch
    w.write_u32(1U);  // depth
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

    // 6. Texture path resolution priority
    {
        wse::platform::MemoryVfs vfs;
        const std::vector<std::uint8_t> dummy{1};
        vfs.write_file("warband/Textures/ui_icon.dds", dummy);
        vfs.write_file("warband/TexturesHD/ui_icon.dds", dummy);
        vfs.write_file("warband/Modules/Mod/Textures/ui_icon.dds", dummy);
        vfs.write_file("warband/Modules/Mod/Textures/languages/fr/ui_icon.dds", dummy);
        vfs.write_file("warband/Textures/base_only.tga", dummy);
        vfs.write_file("warband/TexturesHD/hd_only.dds", dummy);

        expect(wse::asset::resolve_texture_path(vfs, "Textures\\ui_icon", "warband", "warband/Modules/Mod", "fr", true) ==
                   "warband/Modules/Mod/Textures/languages/fr/ui_icon.dds",
               "module localized texture takes highest priority");
        expect(wse::asset::resolve_texture_path(vfs, "ui_icon", "warband", "warband/Modules/Mod", "", true) ==
                   "warband/Modules/Mod/Textures/ui_icon.dds",
               "module texture overrides base TexturesHD and Textures");
        expect(wse::asset::resolve_texture_path(vfs, "base_only", "warband", "", "", true) ==
                   "warband/Textures/base_only.tga",
               "probes .dds -> .jpg -> .tga");
        expect(wse::asset::resolve_texture_path(vfs, "hd_only.dds", "warband", "", "", false).empty() &&
                   wse::asset::resolve_texture_path(vfs, "hd_only.dds", "warband", "", "", true) ==
                       "warband/TexturesHD/hd_only.dds",
               "TexturesHD searched only when enabled");
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
