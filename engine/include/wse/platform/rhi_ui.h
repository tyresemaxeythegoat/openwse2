// Clean-room 2D UI RHI renderer & resource cache over wse::platform::RhiDevice
// (`docs/PLATFORM_PLAN.md`, P9 Part 2).
//
// Port infrastructure over `wse::platform::RhiDevice`; contains no v1.1.5.5 PE/PDB
// citations. Transforms Warband Y-up `[0, 1] x [0, 0.75]` UI coordinates into
// Vulkan clip space (`vulkan_flip_y = true`) and flips bottom-up Warband BRF/font
// UV V coordinates into top-down `decode_dds_mip_rgba8` / `SoftwareRhiDevice` rows
// (`v_rhi = 1.0f - v`).
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

#include "wse/asset/brf.h"
#include "wse/asset/dds.h"
#include "wse/asset/font.h"
#include "wse/game/module_loader_common.h"
#include "wse/game/ui_runtime.h"
#include "wse/platform/rhi.h"
#include "wse/platform/rhi_material.h"
#include "wse/platform/vfs.h"

namespace wse::platform {

struct UiRenderConfig {
    std::uint32_t viewport_width = 800;
    std::uint32_t viewport_height = 600;
    float virtual_width = game::kUiScreenWidth;
    float virtual_height = game::kUiScreenHeight;
    std::uint8_t clear_r = 12;
    std::uint8_t clear_g = 12;
    std::uint8_t clear_b = 14;
    std::uint8_t clear_a = 255;
    float text_scale = 1.0f;
};

struct DecodedUiTexture {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba8{};
};

struct LoadedUiAssets {
    asset::FontData font{};
    game::LanguageTable lang{};
    game::GameVariablesTable vars{};
    std::unordered_map<std::string, asset::BrfMesh> meshes{};
    std::unordered_map<std::string, asset::BrfMaterial> materials{};
    std::unordered_map<std::string, DecodedUiTexture> textures{};
    std::string font_texture_key = "font";

    [[nodiscard]] std::size_t texture_bytes() const noexcept {
        std::size_t total = 0U;
        for (const auto& kv : textures) {
            total += kv.second.rgba8.size();
        }
        return total;
    }
};

[[nodiscard]] inline bool load_dds_texture_into_map(
    Vfs& vfs,
    std::string_view vfs_path,
    std::string_view key,
    std::unordered_map<std::string, DecodedUiTexture>& out_textures,
    std::string* error_out = nullptr,
    std::uint32_t max_texture_size = 0U) {
    const std::string lkey = ascii_lower(key);
    if (out_textures.find(lkey) != out_textures.end()) {
        return true;
    }
    std::vector<std::uint8_t> raw;
    if (!vfs.read_file(vfs_path, raw)) {
        if (error_out) *error_out = "failed to read DDS texture: " + std::string(vfs_path);
        return false;
    }
    asset::DdsImageView view{};
    if (!asset::parse_dds(std::as_bytes(std::span<const std::uint8_t>(raw)), view, error_out)) {
        return false;
    }
    DecodedUiTexture tex{};
    if (!asset::decode_dds_rgba8_budgeted(view, 0U, max_texture_size, tex.rgba8, tex.width,
                                          tex.height, error_out)) {
        return false;
    }
    out_textures[lkey] = std::move(tex);
    return true;
}

namespace rhi_ui_detail {

inline void mat4_identity(float* m) noexcept {
    std::memset(m, 0, 16U * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

inline void mat4_mul(const float* a, const float* b, float* out) noexcept {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            out[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] +
                                 a[1 * 4 + row] * b[col * 4 + 1] +
                                 a[2 * 4 + row] * b[col * 4 + 2] +
                                 a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

inline void make_stretched_model_matrix(float dst_x,
                                        float dst_y,
                                        float dst_w,
                                        float dst_h,
                                        float bx0,
                                        float bx1,
                                        float by0,
                                        float by1,
                                        float* out_m) noexcept {
    mat4_identity(out_m);
    const float span_x = bx1 - bx0;
    const float span_y = by1 - by0;
    const float sx = (span_x > 1.0e-5f) ? (dst_w / span_x) : dst_w;
    const float sy = (span_y > 1.0e-5f) ? (dst_h / span_y) : dst_h;
    out_m[0] = sx;
    out_m[5] = sy;
    out_m[12] = dst_x - bx0 * sx;
    out_m[13] = dst_y - by0 * sy;
}

inline void append_quad(std::vector<MeshVertex>& verts,
                        std::vector<std::uint16_t>& indices,
                        float x0,
                        float y0,
                        float x1,
                        float y1,
                        float u0,
                        float v0,
                        float u1,
                        float v1,
                        std::uint32_t color_rgba) {
    if (verts.size() + 4U > 65535U) {
        return;
    }
    const auto base = static_cast<std::uint16_t>(verts.size());
    verts.push_back(MeshVertex{x0, y0, 0.0f, 0.0f, 0.0f, 1.0f, u0, v0, color_rgba});
    verts.push_back(MeshVertex{x1, y0, 0.0f, 0.0f, 0.0f, 1.0f, u1, v0, color_rgba});
    verts.push_back(MeshVertex{x1, y1, 0.0f, 0.0f, 0.0f, 1.0f, u1, v1, color_rgba});
    verts.push_back(MeshVertex{x0, y1, 0.0f, 0.0f, 0.0f, 1.0f, u0, v1, color_rgba});

    indices.push_back(base);
    indices.push_back(static_cast<std::uint16_t>(base + 1U));
    indices.push_back(static_cast<std::uint16_t>(base + 2U));
    indices.push_back(base);
    indices.push_back(static_cast<std::uint16_t>(base + 2U));
    indices.push_back(static_cast<std::uint16_t>(base + 3U));
}

[[nodiscard]] inline bool is_ui_brf_resource(std::string_view name) {
    const std::string lower = ascii_lower(name);
    return lower == "core_ui_meshes" || lower == "core_pictures" ||
           lower == "core_materials" || lower.rfind("user_interface", 0) == 0;
}

// Indexes one `.brf` into `out`. Materials are always kept (they are small and UI meshes
// may name materials from any resource). Meshes are kept only when `mesh_filter` accepts
// them, so 3D content is never copied into (or later uploaded by) the UI asset set.
template <typename MeshFilter>
inline void index_brf_file(const asset::BrfFile& file,
                           LoadedUiAssets& out,
                           std::vector<std::string>& ui_materials_to_decode,
                           MeshFilter&& mesh_filter) {
    for (const asset::BrfMaterial& mat : file.materials) {
        out.materials[ascii_lower(mat.name)] = mat;
    }
    for (const asset::BrfMesh& mesh : file.meshes) {
        std::string key = ascii_lower(mesh.name);
        if (!mesh_filter(key)) {
            continue;
        }
        if (!mesh.material_name.empty()) {
            ui_materials_to_decode.push_back(ascii_lower(mesh.material_name));
        }
        out.meshes[std::move(key)] = mesh;
    }
}

inline void try_load_named_texture(Vfs& vfs,
                                   std::string_view warband_root,
                                   std::string_view module_rel_dir,
                                   std::string_view language,
                                   std::string_view tex_name,
                                   LoadedUiAssets& out,
                                   std::uint32_t max_texture_size = 0U) {
    if (tex_name.empty() || tex_name == "none") {
        return;
    }
    const std::string lkey = ascii_lower(tex_name);
    if (out.textures.find(lkey) != out.textures.end()) {
        return;
    }
    const std::string resolved = asset::resolve_texture_path(
        vfs, tex_name, warband_root, module_rel_dir, language, false, -1);
    if (resolved.empty()) {
        return;
    }
    const std::string lower_path = ascii_lower(resolved);
    if (lower_path.size() >= 4U && lower_path.compare(lower_path.size() - 4U, 4U, ".dds") == 0) {
        (void)load_dds_texture_into_map(vfs, resolved, lkey, out.textures, nullptr,
                                        max_texture_size);
    }
}

[[nodiscard]] inline float clamp_ui_text_scale(float s) noexcept {
    return std::isfinite(s) ? std::clamp(s, 0.75f, 2.0f) : 1.0f;
}

[[nodiscard]] inline bool resolve_widget_text_rect(const game::UiWidget& w,
                                                   float abs_x,
                                                   float abs_y,
                                                   float text_scale,
                                                   float& out_min_x,
                                                   float& out_max_x,
                                                   float& out_top_y) noexcept {
    if (w.text.empty() || w.text_scale.x <= 0.0f || w.text_scale.y <= 0.0f) {
        return false;
    }
    const float min_x = abs_x + w.text_pos.x;
    if (w.size.x <= 0.0f || w.size.y <= 0.0f) {
        // Box-less text (overlay text without `overlay_set_area_size`): anchored at the
        // widget position, first baseline on it, aligned around it, broken only at '\n'.
        out_min_x = min_x;
        out_max_x = min_x;
        out_top_y = abs_y + w.text_pos.y;
        return true;
    }
    const bool single_line = (w.text_flags & asset::kTextFlagSingleLine) != 0U ||
                             (w.size.y <= w.text_scale.y * 2.1f);
    const float eff_scale_y = w.text_scale.y * text_scale;
    const float max_x = min_x + w.size.x;
    out_min_x = min_x;
    out_max_x = max_x;
    if (single_line) {
        out_top_y = abs_y + w.text_pos.y + w.size.y * 0.5f - eff_scale_y * 0.28f;
        // Single-line widgets (e.g. menu buttons) keep retail box bounds, so when text is
        // scaled above 1.0 expand the wrap span around the horizontal anchor rather than
        // wrapping onto extra lines over neighbouring widgets.
        if (text_scale > 1.0f && (w.text_flags & asset::kTextFlagBoxWrap) == 0U) {
            const bool vcenter_or_left = (w.text_flags & asset::kTextFlagVCenterOrLeft) != 0U;
            const bool center_align =
                ((w.text_flags & asset::kTextFlagCenterAlign) != 0U) && !vcenter_or_left;
            const bool right_align =
                ((w.text_flags & asset::kTextFlagRightAlign) != 0U) && !center_align &&
                !vcenter_or_left;
            const float span = w.size.x * text_scale;
            if (center_align) {
                const float mid = (min_x + max_x) * 0.5f;
                out_min_x = mid - span * 0.5f;
                out_max_x = mid + span * 0.5f;
            } else if (right_align) {
                out_min_x = max_x - span;
                out_max_x = max_x;
            } else {
                out_min_x = min_x;
                out_max_x = min_x + span;
            }
        }
    } else {
        out_top_y = abs_y + w.text_pos.y + w.size.y - eff_scale_y * 0.75f;
    }
    return true;
}

}  // namespace rhi_ui_detail

// Loads core & module UI assets (`font_data.xml`, `Textures/font.dds`, language CSVs,
// `game_variables.txt`, UI `.brf` meshes/materials, and UI `.dds` textures) via `Vfs`.
// Meshes come from the core UI resources (module resources may override them by name);
// other module meshes are kept only when listed in `extra_mesh_names`, so a large mod's
// 3D content is neither copied, decoded, nor uploaded as UI assets.
[[nodiscard]] inline bool load_native_ui_assets(Vfs& vfs,
                                                std::string_view warband_root,
                                                std::string_view module_rel_dir,
                                                std::string_view language,
                                                LoadedUiAssets& out,
                                                std::string* error_out = nullptr,
                                                std::span<const std::string> extra_mesh_names = {},
                                                std::uint32_t max_texture_size = 0U) {
    out = LoadedUiAssets{};

    // 1. Font XML (`Data/font_data.xml`, module override first).
    std::string font_xml_path =
        module_rel_dir.empty() ? std::string{} : game::vfs_join_path(module_rel_dir, "Data/font_data.xml");
    if (font_xml_path.empty() || !vfs.is_file(font_xml_path)) {
        font_xml_path = warband_root.empty()
                            ? std::string("Data/font_data.xml")
                            : game::vfs_join_path(warband_root, "Data/font_data.xml");
    }
    if (!asset::load_font_data(vfs, font_xml_path, out.font, error_out)) {
        return false;
    }

    // 2. Localization & layout properties.
    (void)out.lang.load_from_vfs(vfs, language, module_rel_dir);
    (void)out.vars.load_from_vfs(vfs, module_rel_dir);

    // 3. Parse core + module BRF UI resources.
    game::ModuleIniData ini{};
    if (!module_rel_dir.empty()) {
        std::vector<std::uint8_t> ini_raw;
        const std::string ini_path = game::vfs_join_path(module_rel_dir, "module.ini");
        if (vfs.read_file(ini_path, ini_raw)) {
            game::parse_module_ini(
                std::string_view(reinterpret_cast<const char*>(ini_raw.data()), ini_raw.size()),
                ini);
        }
    }

    std::vector<std::string> ui_materials;
    asset::BrfCatalog catalog{};
    if (!asset::load_module_brf_catalog(vfs, warband_root, module_rel_dir, ini, catalog, error_out)) {
        return false;
    }
    // Pass 1: core UI resources (`core_ui_meshes`, `core_pictures`, `user_interface*`, ...),
    // plus the CommonRes `user_interface_b/c` packs when `module.ini` does not load them.
    // Other core resources contribute materials and only the wanted mesh names.
    const auto keep_all = [](const std::string&) { return true; };
    // Meshes `UiSession` names itself (initial / menu / notes windows) plus the caller's.
    std::unordered_map<std::string, bool> wanted;
    for (const std::string_view name : game::kUiSessionMeshNames) {
        wanted.emplace(std::string(name), true);
    }
    for (const std::string& name : extra_mesh_names) {
        wanted.emplace(ascii_lower(name), true);
    }
    const auto keep_wanted = [&wanted](const std::string& key) {
        return wanted.find(key) != wanted.end();
    };
    bool have_ui_b = false;
    bool have_ui_c = false;
    for (const asset::BrfCatalogEntry& entry : catalog.entries) {
        if (!entry.loaded || entry.from_module) {
            continue;
        }
        const std::string lname = ascii_lower(entry.resource_name);
        have_ui_b = have_ui_b || lname == "user_interface_b";
        have_ui_c = have_ui_c || lname == "user_interface_c";
        if (rhi_ui_detail::is_ui_brf_resource(entry.resource_name)) {
            rhi_ui_detail::index_brf_file(entry.file, out, ui_materials, keep_all);
        } else {
            rhi_ui_detail::index_brf_file(entry.file, out, ui_materials, keep_wanted);
        }
    }
    const std::array<std::pair<std::string_view, bool>, 2> kFallbackUiPacks{{
        {"user_interface_b", have_ui_b},
        {"user_interface_c", have_ui_c},
    }};
    for (const auto& [pack, already_loaded] : kFallbackUiPacks) {
        if (already_loaded) {
            continue;
        }
        const std::string path =
            game::vfs_join_path(game::vfs_join_path(warband_root, "CommonRes"), std::string(pack) + ".brf");
        if (vfs.is_file(path)) {
            asset::BrfFile bf{};
            if (asset::load_brf(vfs, path, bf, nullptr)) {
                rhi_ui_detail::index_brf_file(bf, out, ui_materials, keep_all);
            }
        }
    }
    // Pass 2: module resources, applied last so they override core meshes of the same name.
    // Only meshes the UI can name are kept: core UI mesh overrides, the session's own mesh
    // names, and `extra_mesh_names` (e.g. the resource names in the module's `meshes.txt`,
    // which presentation mesh overlays reference).
    for (const auto& kv : out.meshes) {
        wanted.emplace(kv.first, true);
    }
    for (const asset::BrfCatalogEntry& entry : catalog.entries) {
        if (!entry.loaded || !entry.from_module) {
            continue;
        }
        rhi_ui_detail::index_brf_file(entry.file, out, ui_materials, keep_wanted);
    }

    // 4. Decode font atlas + textures referenced by UI meshes (`main_menu_nord`,
    //    `ui_quick_battle`, `warrider_logo`, `interface`, etc.). The font atlas is never
    //    downscaled because `font_data.xml` glyph bounds are authored at full atlas pixels.
    rhi_ui_detail::try_load_named_texture(
        vfs, warband_root, module_rel_dir, language, out.font_texture_key, out, 0U);
    const std::string font_lkey = ascii_lower(out.font_texture_key);
    for (const std::string& mat_key : ui_materials) {
        const auto mit = out.materials.find(mat_key);
        const std::string_view tex_name =
            (mit != out.materials.end() && !mit->second.texture_names[0].empty())
                ? std::string_view(mit->second.texture_names[0])
                : std::string_view(mat_key);
        const std::uint32_t cap =
            (!font_lkey.empty() && ascii_lower(tex_name) == font_lkey) ? 0U : max_texture_size;
        rhi_ui_detail::try_load_named_texture(
            vfs, warband_root, module_rel_dir, language, tex_name, out, cap);
    }
    return true;
}

// UI-space extent of every loaded UI mesh (lower-case keys), for `game::UiSession::mesh_bounds`
// so mesh overlays are placed at their native size times `overlay_set_size`.
[[nodiscard]] inline std::unordered_map<std::string, game::UiMeshBounds> collect_ui_mesh_bounds(
    const LoadedUiAssets& assets) {
    std::unordered_map<std::string, game::UiMeshBounds> out;
    out.reserve(assets.meshes.size());
    for (const auto& [key, mesh] : assets.meshes) {
        if (mesh.positions.empty()) {
            continue;
        }
        game::UiMeshBounds b{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                             std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
        for (const asset::Vec3f& p : mesh.positions) {
            b.min_x = std::min(b.min_x, p.x);
            b.min_y = std::min(b.min_y, p.y);
            b.max_x = std::max(b.max_x, p.x);
            b.max_y = std::max(b.max_y, p.y);
        }
        out.emplace(key, b);
    }
    return out;
}

class UiRhiRenderer {
public:
    explicit UiRhiRenderer(RhiDevice& rhi) noexcept : rhi_(rhi) {}

    UiRhiRenderer(const UiRhiRenderer&) = delete;
    UiRhiRenderer& operator=(const UiRhiRenderer&) = delete;

    ~UiRhiRenderer() { shutdown(); }

    [[nodiscard]] bool init(const LoadedUiAssets* assets = nullptr,
                            std::string* error_out = nullptr) {
        shutdown();

        static constexpr std::array<std::uint8_t, 4> kWhitePixel{255, 255, 255, 255};
        white_tex_ = rhi_.create_texture_rgba8(1, 1, kWhitePixel);
        if (white_tex_ == 0) {
            if (error_out) *error_out = "failed to create 1x1 white UI texture";
            return false;
        }

        PipelineDesc alpha_desc{};
        alpha_desc.depth_test = false;
        alpha_desc.depth_write = false;
        alpha_desc.cull_mode = CullMode::none;
        alpha_desc.front_face = FrontFace::counter_clockwise;
        alpha_desc.blend_mode = BlendMode::alpha;
        alpha_desc.srgb_texture = false;
        alpha_desc.srgb_framebuffer = false;
        alpha_desc.enable_lighting = false;
        alpha_pip_ = rhi_.create_pipeline(alpha_desc);

        PipelineDesc add_desc = alpha_desc;
        add_desc.blend_mode = BlendMode::additive_alpha;
        add_pip_ = rhi_.create_pipeline(add_desc);

        if (alpha_pip_ == 0 || add_pip_ == 0) {
            shutdown();
            if (error_out) *error_out = "failed to create UI RHI pipelines";
            return false;
        }

        if (assets != nullptr) {
            upload_assets(*assets);
        }
        return true;
    }

    void shutdown() noexcept {
        if (dyn_vb_ != 0) {
            rhi_.destroy_buffer(dyn_vb_);
            dyn_vb_ = 0;
        }
        if (dyn_ib_ != 0) {
            rhi_.destroy_buffer(dyn_ib_);
            dyn_ib_ = 0;
        }
        for (const auto& kv : meshes_) {
            if (kv.second.vb != 0) rhi_.destroy_buffer(kv.second.vb);
            if (kv.second.ib != 0) rhi_.destroy_buffer(kv.second.ib);
        }
        meshes_.clear();
        for (const auto& kv : textures_) {
            if (kv.second != 0) rhi_.destroy_texture(kv.second);
        }
        textures_.clear();
        material_tex_.clear();
        if (white_tex_ != 0) {
            rhi_.destroy_texture(white_tex_);
            white_tex_ = 0;
        }
        if (alpha_pip_ != 0) {
            rhi_.destroy_pipeline(alpha_pip_);
            alpha_pip_ = 0;
        }
        if (add_pip_ != 0) {
            rhi_.destroy_pipeline(add_pip_);
            add_pip_ = 0;
        }
        font_tex_ = 0;
    }

    bool upload_texture(std::string_view key,
                        std::uint32_t width,
                        std::uint32_t height,
                        std::span<const std::uint8_t> rgba8) {
        if (key.empty() || width == 0 || height == 0 ||
            rgba8.size() < static_cast<std::size_t>(width) * height * 4U) {
            return false;
        }
        const std::string lkey = ascii_lower(key);
        // Create the replacement first: if that fails the old texture (and `font_tex_`)
        // stays valid instead of pointing at a destroyed handle.
        const TextureHandle h = rhi_.create_texture_rgba8(width, height, rgba8);
        if (h == 0) {
            return false;
        }
        const auto old = textures_.find(lkey);
        if (old != textures_.end() && old->second != 0) {
            if (font_tex_ == old->second) {
                font_tex_ = 0;
            }
            rhi_.destroy_texture(old->second);
        }
        textures_[lkey] = h;
        if (lkey == "font") {
            font_tex_ = h;
        }
        return true;
    }

    bool upload_brf_mesh(const asset::BrfMesh& mesh,
                         std::string_view diffuse_tex_key = {},
                         BlendMode blend = BlendMode::alpha) {
        if (mesh.name.empty() || mesh.faces.empty() || mesh.face_corners.empty() ||
            mesh.face_corners.size() > 65535U) {
            return false;
        }
        std::vector<MeshVertex> verts;
        verts.reserve(mesh.face_corners.size());
        float bx0 = std::numeric_limits<float>::max();
        float bx1 = std::numeric_limits<float>::lowest();
        float by0 = std::numeric_limits<float>::max();
        float by1 = std::numeric_limits<float>::lowest();

        for (const asset::BrfFaceCorner& fc : mesh.face_corners) {
            if (fc.vertex_index < 0 ||
                static_cast<std::size_t>(fc.vertex_index) >= mesh.positions.size()) {
                return false;
            }
            const asset::Vec3f& p = mesh.positions[static_cast<std::size_t>(fc.vertex_index)];
            bx0 = std::min(bx0, p.x);
            bx1 = std::max(bx1, p.x);
            by0 = std::min(by0, p.y);
            by1 = std::max(by1, p.y);
            const std::uint32_t col_argb = (fc.color == 0U) ? 0xFFFFFFFFU : fc.color;
            verts.push_back(MeshVertex{
                p.x,
                p.y,
                p.z,
                fc.normal.x,
                fc.normal.y,
                fc.normal.z,
                fc.uv.x,
                1.0f - fc.uv.y,
                argb_to_rgba8(col_argb),
            });
        }

        std::vector<std::uint16_t> indices;
        indices.reserve(mesh.faces.size() * 3U);
        for (const asset::BrfMeshFace& face : mesh.faces) {
            for (const std::uint16_t ci : face.corners) {
                if (ci >= verts.size()) {
                    return false;
                }
                indices.push_back(ci);
            }
        }

        const BufferHandle vb = rhi_.create_buffer(
            BufferUsage::vertex,
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(verts.data()),
                                          verts.size() * sizeof(MeshVertex)));
        const BufferHandle ib = rhi_.create_buffer(
            BufferUsage::index,
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(indices.data()),
                                          indices.size() * sizeof(std::uint16_t)));
        if (vb == 0 || ib == 0) {
            if (vb != 0) rhi_.destroy_buffer(vb);
            if (ib != 0) rhi_.destroy_buffer(ib);
            return false;
        }

        const std::string lkey = ascii_lower(mesh.name);
        const auto prev = meshes_.find(lkey);
        if (prev != meshes_.end()) {
            if (prev->second.vb != 0) rhi_.destroy_buffer(prev->second.vb);
            if (prev->second.ib != 0) rhi_.destroy_buffer(prev->second.ib);
        }

        GpuUiMesh rec{};
        rec.vb = vb;
        rec.ib = ib;
        rec.index_count = static_cast<std::uint32_t>(indices.size());
        rec.bx0 = bx0;
        rec.bx1 = bx1;
        rec.by0 = by0;
        rec.by1 = by1;
        rec.default_tex_key = ascii_lower(diffuse_tex_key);
        rec.blend = blend;
        meshes_[lkey] = std::move(rec);
        return true;
    }

    void upload_assets(const LoadedUiAssets& assets) {
        for (const auto& kv : assets.textures) {
            (void)upload_texture(kv.first, kv.second.width, kv.second.height, kv.second.rgba8);
        }
        if (!assets.font_texture_key.empty()) {
            const auto fit = textures_.find(ascii_lower(assets.font_texture_key));
            if (fit != textures_.end()) {
                font_tex_ = fit->second;
            }
        }
        for (const auto& [mat_key, mat] : assets.materials) {
            material_tex_[mat_key] = mat.texture_names[0].empty()
                                         ? mat_key
                                         : ascii_lower(mat.texture_names[0]);
        }
        for (const auto& kv : assets.meshes) {
            const asset::BrfMesh& mesh = kv.second;
            std::string tex_key;
            BlendMode blend = BlendMode::alpha;
            if (!mesh.material_name.empty()) {
                const auto mit = assets.materials.find(ascii_lower(mesh.material_name));
                if (mit != assets.materials.end()) {
                    if (!mit->second.texture_names[0].empty()) {
                        tex_key = ascii_lower(mit->second.texture_names[0]);
                    } else {
                        tex_key = ascii_lower(mit->second.name);
                    }
                    const BlendMode mat_blend =
                        to_rhi_blend_mode(asset::decode_material_render_state(mit->second).blend_mode);
                    blend = (mat_blend == BlendMode::additive_alpha ||
                             mat_blend == BlendMode::additive)
                                ? mat_blend
                                : BlendMode::alpha;
                } else {
                    tex_key = ascii_lower(mesh.material_name);
                }
            }
            (void)upload_brf_mesh(mesh, tex_key, blend);
        }
    }

    [[nodiscard]] bool has_mesh(std::string_view name) const {
        return meshes_.find(ascii_lower(name)) != meshes_.end();
    }

    [[nodiscard]] bool has_texture(std::string_view name) const {
        return textures_.find(ascii_lower(name)) != textures_.end();
    }

    [[nodiscard]] bool render_tree(const game::UiWidget& root,
                                   const asset::FontData& font,
                                   const UiRenderConfig& cfg,
                                   std::vector<std::uint8_t>& out_rgba8) {
        if (alpha_pip_ == 0 || white_tex_ == 0 || cfg.viewport_width == 0 ||
            cfg.viewport_height == 0 || cfg.virtual_width <= 0.0f || cfg.virtual_height <= 0.0f) {
            return false;
        }

        float proj[16]{};
        make_d3d9_ortho_off_center_rh(
            0.0f, cfg.virtual_width, 0.0f, cfg.virtual_height, -10.0f, 10.0f, proj, true);
        const float text_scale = rhi_ui_detail::clamp_ui_text_scale(cfg.text_scale);

        // Emit dynamic quads & static BRF draws cleanly while keeping painter's order.
        struct ClipRect {
            bool has_clip = false;
            float x0 = 0.0f;
            float y0 = 0.0f;
            float x1 = 0.0f;
            float y1 = 0.0f;
        };
        const auto intersect_clip = [](const ClipRect& parent,
                                       float x0,
                                       float y0,
                                       float x1,
                                       float y1) noexcept -> ClipRect {
            if (!parent.has_clip) {
                return ClipRect{true, x0, y0, x1, y1};
            }
            return ClipRect{true, std::max(parent.x0, x0), std::max(parent.y0, y0),
                            std::min(parent.x1, x1), std::min(parent.y1, y1)};
        };

        struct PendingBatch {
            bool is_dynamic = false;
            std::size_t first_idx = 0;
            std::size_t idx_count = 0;
            TextureHandle tex = 0;
            PipelineHandle pip = 0;
            bool has_clip = false;
            float clip_x0 = 0.0f;
            float clip_y0 = 0.0f;
            float clip_x1 = 0.0f;
            float clip_y1 = 0.0f;
            MeshDrawCall static_draw{};
        };

        std::vector<MeshVertex> dyn_verts;
        std::vector<std::uint16_t> dyn_indices;
        std::vector<PendingBatch> batches;

        const auto push_dyn_quads = [&](std::span<const MeshVertex> qv,
                                        std::span<const std::uint16_t> qi,
                                        TextureHandle tex,
                                        PipelineHandle pip,
                                        const ClipRect& clip) {
            if (qi.empty() || dyn_verts.size() + qv.size() > 65535U) {
                return;
            }
            const auto vbase = static_cast<std::uint16_t>(dyn_verts.size());
            const std::size_t first_i = dyn_indices.size();
            dyn_verts.insert(dyn_verts.end(), qv.begin(), qv.end());
            for (const std::uint16_t idx : qi) {
                dyn_indices.push_back(static_cast<std::uint16_t>(vbase + idx));
            }
            const bool same_clip =
                !batches.empty() && batches.back().has_clip == clip.has_clip &&
                (!clip.has_clip ||
                 (batches.back().clip_x0 == clip.x0 && batches.back().clip_y0 == clip.y0 &&
                  batches.back().clip_x1 == clip.x1 && batches.back().clip_y1 == clip.y1));
            if (!batches.empty() && batches.back().is_dynamic && batches.back().tex == tex &&
                batches.back().pip == pip && same_clip) {
                batches.back().idx_count += qi.size();
            } else {
                PendingBatch pb{};
                pb.is_dynamic = true;
                pb.first_idx = first_i;
                pb.idx_count = qi.size();
                pb.tex = tex;
                pb.pip = pip;
                pb.has_clip = clip.has_clip;
                pb.clip_x0 = clip.x0;
                pb.clip_y0 = clip.y0;
                pb.clip_x1 = clip.x1;
                pb.clip_y1 = clip.y1;
                batches.push_back(pb);
            }
        };

        const auto emit_solid_panel = [&](float x,
                                          float y,
                                          float w,
                                          float h,
                                          std::uint32_t argb,
                                          const ClipRect& clip) {
            if (w <= 0.0f || h <= 0.0f || ((argb >> 24U) == 0U)) {
                return;
            }
            std::vector<MeshVertex> qv;
            std::vector<std::uint16_t> qi;
            rhi_ui_detail::append_quad(
                qv, qi, x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, argb_to_rgba8(argb));
            push_dyn_quads(qv, qi, white_tex_, alpha_pip_, clip);
        };

        const auto resolve_tex = [&](std::string_view key, TextureHandle fallback) noexcept {
            if (!key.empty()) {
                const auto it = textures_.find(ascii_lower(key));
                if (it != textures_.end() && it->second != 0) {
                    return it->second;
                }
            }
            return fallback;
        };

        const auto visit = [&](auto& self,
                               const game::UiWidget& node,
                               float px,
                               float py,
                               const ClipRect& clip) -> void {
            if (!node.visible) {
                return;
            }
            const float ax = px + node.pos.x;
            const float ay = py + node.pos.y;

            if (node.kind == game::UiWidgetKind::Image ||
                node.kind == game::UiWidgetKind::SimpleButton ||
                node.kind == game::UiWidgetKind::GameButton ||
                node.kind == game::UiWidgetKind::SolidPanel) {
                const std::string& desired_mesh =
                    (node.highlighted && !node.highlight_mesh_name.empty())
                        ? node.highlight_mesh_name
                        : node.mesh_name;
                const auto mit = desired_mesh.empty()
                                     ? meshes_.end()
                                     : meshes_.find(ascii_lower(desired_mesh));
                if (mit != meshes_.end()) {
                    const GpuUiMesh& gm = mit->second;
                    float model[16]{};
                    if (node.has_mesh_rect) {
                        rhi_ui_detail::make_stretched_model_matrix(
                            ax + node.mesh_pos.x, ay + node.mesh_pos.y, node.mesh_size.x,
                            node.mesh_size.y, gm.bx0, gm.bx1, gm.by0, gm.by1, model);
                    } else {
                        rhi_ui_detail::make_stretched_model_matrix(
                            ax, ay, node.size.x, node.size.y, gm.bx0, gm.bx1, gm.by0, gm.by1, model);
                    }
                    PendingBatch pb{};
                    pb.is_dynamic = false;
                    pb.static_draw.pipeline =
                        (gm.blend == BlendMode::additive_alpha || gm.blend == BlendMode::additive)
                            ? add_pip_
                            : alpha_pip_;
                    pb.static_draw.vertex_buffer = gm.vb;
                    pb.static_draw.index_buffer = gm.ib;
                    pb.static_draw.index_count = gm.index_count;
                    pb.static_draw.texture = resolve_tex(gm.default_tex_key, white_tex_);
                    if (!node.material_name.empty()) {
                        // `material_name` names a material; draw with its diffuse texture.
                        const std::string lmat = ascii_lower(node.material_name);
                        const auto mt = material_tex_.find(lmat);
                        pb.static_draw.texture = resolve_tex(
                            mt != material_tex_.end() ? std::string_view(mt->second)
                                                      : std::string_view(lmat),
                            pb.static_draw.texture);
                    }
                    if (node.tint_mesh) {
                        pb.static_draw.tint_rgba = argb_to_rgba8(node.color_argb);
                    }
                    pb.static_draw.has_clip = clip.has_clip;
                    pb.static_draw.clip_x0 = clip.x0;
                    pb.static_draw.clip_y0 = clip.y0;
                    pb.static_draw.clip_x1 = clip.x1;
                    pb.static_draw.clip_y1 = clip.y1;
                    std::memcpy(pb.static_draw.model, model, sizeof(model));
                    rhi_ui_detail::mat4_mul(proj, model, pb.static_draw.mvp);
                    batches.push_back(pb);
                } else if (node.kind == game::UiWidgetKind::SolidPanel) {
                    emit_solid_panel(ax, ay, node.size.x, node.size.y, node.color_argb, clip);
                }
            }

            float tmin_x = 0.0f;
            float tmax_x = 0.0f;
            float ttop_y = 0.0f;
            if (rhi_ui_detail::resolve_widget_text_rect(
                    node, ax, ay, text_scale, tmin_x, tmax_x, ttop_y)) {
                const std::uint32_t base_col =
                    (node.highlighted &&
                     (node.kind == game::UiWidgetKind::SimpleButton ||
                      node.kind == game::UiWidgetKind::GameButton))
                        ? node.highlight_color_argb
                        : node.text_color_argb;
                const asset::TextLayoutResult layout = font.layout_text(
                    node.text, tmin_x, tmax_x, ttop_y, node.text_scale.x * text_scale,
                    node.text_scale.y * text_scale, node.text_flags, base_col);
                if (!layout.quads.empty() && font_tex_ != 0) {
                    std::vector<MeshVertex> qv;
                    std::vector<std::uint16_t> qi;
                    qv.reserve(layout.quads.size() * 4U);
                    qi.reserve(layout.quads.size() * 6U);
                    for (const asset::FontQuad& q : layout.quads) {
                        rhi_ui_detail::append_quad(
                            qv, qi, q.x0, q.y0, q.x1, q.y1, q.u0, 1.0f - q.v0, q.u1, 1.0f - q.v1,
                            argb_to_rgba8(q.color_argb));
                    }
                    push_dyn_quads(qv, qi, font_tex_, alpha_pip_, clip);
                }
            }

            const float cx = ax + node.child_offset.x;
            const float cy = ay + node.child_offset.y;
            const ClipRect child_clip =
                node.clip_children
                    ? intersect_clip(clip, ax, ay, ax + node.size.x, ay + node.size.y)
                    : clip;
            if (child_clip.has_clip &&
                (!(child_clip.x1 > child_clip.x0) || !(child_clip.y1 > child_clip.y0))) {
                return;
            }
            for (const game::UiWidget& child : node.children) {
                if (child_clip.has_clip && child.children.empty() && !child.has_mesh_rect &&
                    child.text_pos.x == 0.0f && child.text_pos.y == 0.0f &&
                    child.size.x > 0.0f && child.size.y > 0.0f) {
                    const float c0x = cx + child.pos.x;
                    const float c0y = cy + child.pos.y;
                    if (c0x > child_clip.x1 || c0x + child.size.x < child_clip.x0 ||
                        c0y > child_clip.y1 || c0y + child.size.y < child_clip.y0) {
                        continue;
                    }
                }
                self(self, child, cx, cy, child_clip);
            }
        };

        visit(visit, root, 0.0f, 0.0f, ClipRect{});

        std::vector<MeshDrawCall> draw_calls;
        draw_calls.reserve(batches.size());

        // All dynamic batches share one vertex and one index buffer, kept across frames and
        // refilled with `update_buffer`; each batch draws its own range via `first_index`.
        if (!dyn_verts.empty() && !dyn_indices.empty()) {
            const auto vb_bytes = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(dyn_verts.data()),
                dyn_verts.size() * sizeof(MeshVertex));
            const auto ib_bytes = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(dyn_indices.data()),
                dyn_indices.size() * sizeof(std::uint16_t));
            if (!refill_buffer(dyn_vb_, BufferUsage::vertex, vb_bytes) ||
                !refill_buffer(dyn_ib_, BufferUsage::index, ib_bytes)) {
                return false;
            }
        }

        for (const PendingBatch& pb : batches) {
            if (!pb.is_dynamic) {
                draw_calls.push_back(pb.static_draw);
                continue;
            }
            MeshDrawCall dc{};
            dc.pipeline = pb.pip;
            dc.vertex_buffer = dyn_vb_;
            dc.index_buffer = dyn_ib_;
            dc.texture = pb.tex;
            dc.first_index = static_cast<std::uint32_t>(pb.first_idx);
            dc.index_count = static_cast<std::uint32_t>(pb.idx_count);
            dc.has_clip = pb.has_clip;
            dc.clip_x0 = pb.clip_x0;
            dc.clip_y0 = pb.clip_y0;
            dc.clip_x1 = pb.clip_x1;
            dc.clip_y1 = pb.clip_y1;
            std::memcpy(dc.mvp, proj, sizeof(proj));
            rhi_ui_detail::mat4_identity(dc.model);
            draw_calls.push_back(dc);
        }

        RenderPassDesc pass{};
        pass.width = cfg.viewport_width;
        pass.height = cfg.viewport_height;
        pass.clear_r = cfg.clear_r;
        pass.clear_g = cfg.clear_g;
        pass.clear_b = cfg.clear_b;
        pass.clear_a = cfg.clear_a;
        pass.clear_depth = 1.0f;

        return rhi_.render_pass_and_readback(pass, draw_calls, out_rgba8);
    }

    // The shared dynamic buffers (0 until the first frame with text or panels).
    [[nodiscard]] BufferHandle dynamic_vertex_buffer() const noexcept { return dyn_vb_; }
    [[nodiscard]] BufferHandle dynamic_index_buffer() const noexcept { return dyn_ib_; }

private:
    // Creates `handle` on first use, otherwise replaces its contents in place.
    bool refill_buffer(BufferHandle& handle, BufferUsage usage, std::span<const std::uint8_t> bytes) {
        if (handle != 0) {
            return rhi_.update_buffer(handle, bytes);
        }
        handle = rhi_.create_buffer(usage, bytes);
        return handle != 0;
    }

    struct GpuUiMesh {
        BufferHandle vb = 0;
        BufferHandle ib = 0;
        std::uint32_t index_count = 0;
        float bx0 = 0.0f;
        float bx1 = 1.0f;
        float by0 = 0.0f;
        float by1 = 1.0f;
        std::string default_tex_key{};
        BlendMode blend = BlendMode::alpha;
    };

    RhiDevice& rhi_;
    TextureHandle white_tex_ = 0;
    TextureHandle font_tex_ = 0;
    BufferHandle dyn_vb_ = 0;
    BufferHandle dyn_ib_ = 0;
    PipelineHandle alpha_pip_ = 0;
    PipelineHandle add_pip_ = 0;
    std::unordered_map<std::string, TextureHandle> textures_{};
    std::unordered_map<std::string, GpuUiMesh> meshes_{};
    // Lower-case material name -> lower-case texture key, for `UiWidget::material_name`.
    std::unordered_map<std::string, std::string> material_tex_{};
};

}  // namespace wse::platform
